#include "Network/CrowdyCpp/CrowdyCppLoopback.h"

#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "CrowdyNetLog.h"
#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"
#include "Network/UDP/CrowdyCppSendAdapter.h"
#include "Serialization/CrowdyFrame.h"

namespace
{
	/**
	 * The opcode a locally delivered update has to arrive under.
	 *
	 * A client sends an actor update as a request and receives one as a notification, so handing the
	 * request straight back would reach a decoder that refuses it. This is the one substitution the
	 * server would have made, and it is the only one made here: anything without an inbound form is
	 * left to the wire.
	 */
	bool TryInboundOpcode(const uint8 Outbound, uint8& OutInbound)
	{
		if (Outbound != static_cast<uint8>(ECrowdyMessageType::ACTOR_UPDATE_REQUEST))
		{
			return false;
		}

		OutInbound = static_cast<uint8>(ECrowdyMessageType::ACTOR_UPDATE_NOTIFICATION);
		return true;
	}

	int64 UtcMillisNow()
	{
		return static_cast<int64>(FDateTime::UtcNow().ToUnixTimestampDecimal() * 1000.0);
	}
}

void FCrowdyCppLoopback::Arm(const TConstArrayView<FCrowdyActorId> InActorIds, const FCrowdyLoopbackSettings& InSettings)
{
	if (InActorIds.IsEmpty())
	{
		UE_LOG(LogCrowdyNet, Warning,
			TEXT("Local delivery was armed with no actor ids, so nothing will be intercepted. It matches by id "
				"rather than taking every send, so that a session running it still round-trips its own player "
				"through the real relay."));
		Disarm();
		return;
	}

	{
		FScopeLock Lock(&Mutex);

		InterceptedIds.Reset();
		InterceptedIds.Reserve(InActorIds.Num());
		for (const FCrowdyActorId& ActorId : InActorIds)
		{
			InterceptedIds.Add(ActorId);
		}

		Settings = InSettings;
		Settings.LatencySeconds = FMath::Max(Settings.LatencySeconds, 0.0f);
		Settings.JitterSeconds = FMath::Clamp(Settings.JitterSeconds, 0.0f, Settings.LatencySeconds);
		Settings.LossFraction = FMath::Clamp(Settings.LossFraction, 0.0f, 1.0f);
		Settings.MaxQueuedFrames = FMath::Max(Settings.MaxQueuedFrames, 1);

		Queue.Reset();
		Stats = FCrowdyLoopbackStats();
		NoiseStream.Initialize(1);
		NextSequence = 0;
	}

	bArmed.store(true, std::memory_order_relaxed);

	UE_LOG(LogCrowdyNet, Log,
		TEXT("Local delivery armed for %d actor ids: their updates will be held for %.0f ms give or take %.0f, "
			"%.1f%% of them thrown away, and handed back into this process instead of being sent. Nothing else "
			"is intercepted."),
		InActorIds.Num(), Settings.LatencySeconds * 1000.0f, Settings.JitterSeconds * 1000.0f,
		Settings.LossFraction * 100.0f);
}

void FCrowdyCppLoopback::Disarm()
{
	bArmed.store(false, std::memory_order_relaxed);

	FScopeLock Lock(&Mutex);
	InterceptedIds.Reset();
	Queue.Reset();
}

bool FCrowdyCppLoopback::TryAcceptOutbound(const FCrowdyCppOutboundFrame& Frame)
{
	// A channel message is a different frame layout with a different delivery path, and nothing this
	// exists for travels on one.
	if (Frame.bIsChannel)
	{
		return false;
	}

	uint8 InboundOpcode = 0;
	if (!TryInboundOpcode(Frame.Opcode, InboundOpcode))
	{
		return false;
	}

	FCrowdyActorId ActorId;
	if (!FCrowdyActorId::TryFromOctets(Frame.Uuid, ActorId))
	{
		return false;
	}

	FScopeLock Lock(&Mutex);

	if (!InterceptedIds.Contains(ActorId))
	{
		return false;
	}

	++Stats.Accepted;

	// Drawn before anything is copied, so a lost update costs no allocation.
	if (Settings.LossFraction > 0.0f && NoiseStream.FRand() < Settings.LossFraction)
	{
		++Stats.DroppedToLoss;
		return true;
	}

	// The newest is refused rather than the oldest evicted. The oldest is the one closest to being due,
	// so dropping it would push the delay of everything that did arrive up by a whole queue, and the
	// delay is the property this is here to hold steady.
	if (Queue.Num() >= Settings.MaxQueuedFrames)
	{
		++Stats.DroppedToQueueFull;
		return true;
	}

	FQueuedFrame Queued;
	Queued.Opcode = InboundOpcode;
	Queued.AppId = Frame.AppId;
	Queued.ChunkX = Frame.ChunkX;
	Queued.ChunkY = Frame.ChunkY;
	Queued.ChunkZ = Frame.ChunkZ;
	Queued.Uuid = ActorId;
	Queued.Payload.Append(Frame.Payload.GetData(), Frame.Payload.Num());
	Queued.Sequence = NextSequence++;

	// Stamped when the update was SENT rather than when it will be delivered, which is what a server
	// stamps and what interpolation reads. Stamping the delivery time instead would fold the jitter into
	// the timestamps as well as into the arrival times, so the receiver would see it twice and the
	// arrival-interval estimate would widen for a reason that does not exist.
	Queued.TimestampMillis = UtcMillisNow();

	const float Jitter = Settings.JitterSeconds > 0.0f
		? NoiseStream.FRandRange(-Settings.JitterSeconds, Settings.JitterSeconds) : 0.0f;
	Queued.DueSeconds = NowSeconds() + Settings.LatencySeconds + Jitter;

	Queue.HeapPush(MoveTemp(Queued), FDueFirst());

	Stats.QueueDepth = Queue.Num();
	Stats.LargestQueueDepth = FMath::Max(Stats.LargestQueueDepth, Stats.QueueDepth);

	return true;
}

int32 FCrowdyCppLoopback::DrainDue(const int32 MaxFrames, const double MaxSeconds,
	TFunctionRef<void(const FCrowdyFrame&)> Deliver)
{
	// Both budgets read the same clock, so a test that freezes it gets an unlimited time budget and can
	// assert the message budget on its own. In a real session there is no override and this is the
	// platform clock on both sides.
	const double Started = NowSeconds();
	int32 Delivered = 0;
	bool bTimeBudgetExpired = false;

	while (Delivered < MaxFrames)
	{
		FQueuedFrame Popped;

		{
			FScopeLock Lock(&Mutex);
			if (Queue.IsEmpty() || Queue.HeapTop().DueSeconds > NowSeconds())
			{
				// Recorded on the way out of the ordinary exit as well as the truncated ones. Updating
				// it only where a drain was cut short would report zero for every run that was keeping
				// up, which is exactly the run whose delivered-per-frame figure is worth having.
				Stats.QueueDepth = Queue.Num();
				Stats.LargestDrain = FMath::Max(Stats.LargestDrain, Delivered);
				return Delivered;
			}

			Queue.HeapPop(Popped, FDueFirst(), EAllowShrinking::No);
			++Stats.Delivered;
			Stats.QueueDepth = Queue.Num();
		}

		// Built outside the lock, and the views point at the popped frame, which lives until this
		// iteration ends. Delivery reaches reception layers and anything they touch, so holding the lock
		// across it would put every worker thread's send behind whatever the game does with an update.
		FCrowdyFrame Frame;
		Frame.Opcode = Popped.Opcode;
		Frame.Body = Popped.Payload;
		Frame.Envelope.AppId = Popped.AppId;
		Frame.Envelope.ChunkX = Popped.ChunkX;
		Frame.Envelope.ChunkY = Popped.ChunkY;
		Frame.Envelope.ChunkZ = Popped.ChunkZ;
		Frame.Envelope.Uuid = Popped.Uuid.AsOctets();
		Frame.Envelope.Timestamp = Popped.TimestampMillis;
		Frame.Envelope.Sequence = Popped.Sequence;
		Frame.bHasEnvelope = true;

		Deliver(Frame);
		++Delivered;

		if (NowSeconds() - Started >= MaxSeconds)
		{
			// Only when the message budget was NOT also reached on this iteration. The two counters
			// want opposite fixes, and at this load a time-bound drain is usually message-bound too, so
			// a tie broken toward the message budget would keep pointing at the lever that cannot help.
			bTimeBudgetExpired = Delivered < MaxFrames;
			break;
		}
	}

	FScopeLock Lock(&Mutex);
	Stats.LargestDrain = FMath::Max(Stats.LargestDrain, Delivered);

	// Classified only when something was still due, so a drain that emptied the queue on its last
	// allowed message is not reported as having run out of room.
	if (Queue.IsEmpty() || Queue.HeapTop().DueSeconds > NowSeconds())
	{
		return Delivered;
	}

	if (bTimeBudgetExpired)
	{
		++Stats.DrainsCutShortByTimeBudget;
	}
	else if (Delivered >= MaxFrames)
	{
		++Stats.DrainsCutShortByMessageBudget;
	}

	return Delivered;
}

FCrowdyLoopbackStats FCrowdyCppLoopback::GetStats() const
{
	FScopeLock Lock(&Mutex);
	return Stats;
}

double FCrowdyCppLoopback::NowSeconds() const
{
	return bUseTimeOverrideForTest ? TestTimeSeconds : FPlatformTime::Seconds();
}

void FCrowdyCppLoopback::SetTimeForTests(const double InSeconds)
{
	bUseTimeOverrideForTest = true;
	TestTimeSeconds = InSeconds;
}

void FCrowdyCppLoopback::ClearTimeForTests()
{
	bUseTimeOverrideForTest = false;
	TestTimeSeconds = 0.0;
}

void FCrowdyCppLoopback::ResetCounters()
{
	FScopeLock Lock(&Mutex);

	const int32 Depth = Queue.Num();
	Stats = FCrowdyLoopbackStats();
	Stats.QueueDepth = Depth;
}
