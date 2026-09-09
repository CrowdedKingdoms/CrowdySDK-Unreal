#include "Replication/Subsystems/CrowdyVideoFrameReceiver.h"

#include "Core/CrowdySDKBridgeSubsystem.h"
#include "CrowdyReplicationLog.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Internal/FCrowdyServiceRegistry.h"
#include "Messages/Communication/FClientVideoNotification.h"
#include "Subsystem/CrowdyGameSession.h"

namespace
{
	/** The local clock the reassembly timeout is measured against, in milliseconds. */
	int64 CrowdyVideoReceiverNowMs()
	{
		return static_cast<int64>(FPlatformTime::Seconds() * 1000.0);
	}

	TConstArrayView<uint8> CrowdyVideoReceiverOctets(const FCrowdyActorId& ActorId)
	{
		return TConstArrayView<uint8>(ActorId.Octets, FCrowdyActorId::NumOctets);
	}
}

// Built here rather than in Initialize so the reassembly rules can be exercised on a bare instance, with
// no world and no socket behind it.
UCrowdyVideoFrameReceiver::UCrowdyVideoFrameReceiver()
	: Assembler(MakeUnique<FCrowdyCppVideoAssembler>())
{
}

bool UCrowdyVideoFrameReceiver::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}

	const UWorld* World = Outer ? Outer->GetWorld() : nullptr;
	if (!World)
	{
		return false;
	}

	return World->WorldType == EWorldType::PIE || World->WorldType == EWorldType::Game;
}

void UCrowdyVideoFrameReceiver::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	const UWorld* World = GetWorld();
	if (!IsValid(World))
	{
		return;
	}

	// Null during engine init, when the first Game world exists before any game instance owns it.
	UGameInstance* GameInstance = World->GetGameInstance();
	if (!GameInstance)
	{
		UE_CLOG(CrowdyReplicationTrace::Entity(), LogCrowdyReplication, Verbose,
			TEXT("[Crowdy Video Receiver]: No GameInstance yet; skipping init."));
		return;
	}

	// Ordered explicitly: the departure delegate this binds to belongs to the tracker, and a subsystem
	// collection does not otherwise promise which of two world subsystems is built first.
	if (UCrowdyActorTracker* Tracker = Collection.InitializeDependency<UCrowdyActorTracker>())
	{
		// The announced departure rather than the reported one: the tracker suppresses its report for an
		// actor it was not holding, and a sender it never held can still have left assembler state behind.
		Tracker->OnRemoteEntityLeftAnnounced.AddDynamic(this, &UCrowdyVideoFrameReceiver::HandleActorLeft);
		Tracker->OnRemoteEntityTimedOut.AddDynamic(this, &UCrowdyVideoFrameReceiver::HandleActorTimedOut);
	}

	if (UCrowdyGameSession* GameSession = GameInstance->GetSubsystem<UCrowdyGameSession>())
	{
		// Read once as well as subscribed to, because the id is normally settled long before a world exists.
		HandleOwnerUUIDUpdated(GameSession->GetUUID());
		GameSession->OnOwnerUUIDUpdated.AddDynamic(this, &UCrowdyVideoFrameReceiver::HandleOwnerUUIDUpdated);
	}

	const UCrowdySDKBridgeSubsystem* CrowdyBridge = GameInstance->GetSubsystem<UCrowdySDKBridgeSubsystem>();
	if (!IsValid(CrowdyBridge) || !CrowdyBridge->ServiceRegistry)
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[Crowdy Video Receiver]: No service registry; video fragments will not be reassembled."));
		return;
	}

	// By opcode: a video fragment carries a media header rather than a payload type tag, so there is no
	// key to route it by.
	VideoSubscription = CrowdyBridge->ServiceRegistry->SubscribeToOpcode(
		ECrowdyMessageType::CLIENT_VIDEO_NOTIFICATION,
		{ ECrowdySubscriptionRole::Observe, /*bRequiresExclusiveHandling*/ false, TEXT("CrowdyVideoFrameReceiver") },
		[this](const FCrowdyDelivery& Delivery) { HandleVideoDelivery(Delivery); });

	UE_CLOG(CrowdyReplicationTrace::Entity(), LogCrowdyReplication, Log,
		TEXT("[Crowdy Video Receiver]: Initialized."));
}

void UCrowdyVideoFrameReceiver::Deinitialize()
{
	if (UCrowdyActorTracker* Tracker = GetWorld() ? GetWorld()->GetSubsystem<UCrowdyActorTracker>() : nullptr)
	{
		Tracker->OnRemoteEntityLeftAnnounced.RemoveAll(this);
		Tracker->OnRemoteEntityTimedOut.RemoveAll(this);
	}

	// The session outlives this world, so a subscription left on it would fire into a destroyed subsystem.
	const UWorld* World = GetWorld();
	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	if (UCrowdyGameSession* GameSession = GameInstance ? GameInstance->GetSubsystem<UCrowdyGameSession>() : nullptr)
	{
		GameSession->OnOwnerUUIDUpdated.RemoveAll(this);
	}

	VideoSubscription.Release();
	SenderIdsByGuid.Empty();

	Super::Deinitialize();
}

bool UCrowdyVideoFrameReceiver::HasListeners() const
{
	return OnVideoFrameReady.IsBound() || OnVideoFrameAssembled.IsBound();
}

int32 UCrowdyVideoFrameReceiver::GetPendingSenderCount() const
{
	return Assembler.IsValid() ? Assembler->PendingSenders() : 0;
}

int64 UCrowdyVideoFrameReceiver::GetDroppedFragmentCount() const
{
	return Assembler.IsValid() ? Assembler->GetDroppedFragments() : 0;
}

int64 UCrowdyVideoFrameReceiver::GetAbandonedFrameCount() const
{
	return Assembler.IsValid() ? Assembler->GetAbandonedFrames() : 0;
}

bool UCrowdyVideoFrameReceiver::IngestFragment(const FCrowdyActorId& SenderId, const FGuid& SenderGuid,
	const TConstArrayView<uint8> Fragment, const int64 NowMs, FCrowdyCppAssembledVideoFrame& OutFrame)
{
	if (!Assembler.IsValid())
	{
		return false;
	}

	// Swept on the way in rather than on a timer, because a frame only goes stale while its sender is
	// still sending and the sweep costs one pass over the senders that have something outstanding.
	if (NowMs - LastPruneMs >= FCrowdyCppVideoAssembler::DefaultFrameTimeoutMs)
	{
		LastPruneMs = NowMs;
		Assembler->Prune(NowMs);
	}

	// Recorded before the ingest, and by the octets: the assembler keeps this sender's frame counter from
	// here on, whether or not this fragment completed anything, and only Forget releases it.
	SenderIdsByGuid.FindOrAdd(SenderGuid).AddUnique(SenderId);
	return Assembler->Ingest(CrowdyVideoReceiverOctets(SenderId), Fragment, NowMs, OutFrame);
}

void UCrowdyVideoFrameReceiver::ForgetSender(const FGuid& SenderGuid)
{
	TArray<FCrowdyActorId, TInlineAllocator<1>> SenderIds;
	if (!SenderIdsByGuid.RemoveAndCopyValue(SenderGuid, SenderIds))
	{
		return;
	}

	if (!Assembler.IsValid())
	{
		return;
	}

	for (const FCrowdyActorId& SenderId : SenderIds)
	{
		Assembler->Forget(CrowdyVideoReceiverOctets(SenderId));
	}
}

void UCrowdyVideoFrameReceiver::HandleVideoDelivery(const FCrowdyDelivery& Delivery)
{
	// Nothing would receive the frame, and reassembling one costs a copy of every fragment, so the
	// fragment is dropped where it arrives instead.
	if (!HasListeners())
	{
		return;
	}

	// Only the video opcode routes here, and only this decoder produces a message under it.
	const FClientVideoNotification& Fragment = Delivery.GetAs<FClientVideoNotification>();

	// The server fans a video packet out over the chunk it was sent to, which includes the sender, so a
	// client sending video sees its own frames come back. Reassembling them would cost the same as a real
	// sender's and hand back a picture the caller already had.
	if (LocalSenderId.IsSet() && Fragment.UUID == LocalSenderId)
	{
		return;
	}

	FCrowdyCppAssembledVideoFrame Assembled;
	if (!IngestFragment(Fragment.UUID, Fragment.GUID, Fragment.FragmentView, CrowdyVideoReceiverNowMs(), Assembled))
	{
		return;
	}

	FCrowdyVideoFrame Frame;
	Frame.SenderUUID = Fragment.GUID;
	// Carried as well as the key, because the key is derived and two senders can share one.
	Frame.SenderId = Fragment.UUID.ToString();
	Frame.Codec = CrowdyVideoCodecFromByte(Assembled.Codec);
	Frame.FrameId = Assembled.FrameId;
	Frame.Bytes = MoveTemp(Assembled.Bytes);
	Frame.ChunkX = Fragment.ChunkX;
	Frame.ChunkY = Fragment.ChunkY;
	Frame.ChunkZ = Fragment.ChunkZ;
	Frame.ServerTimestamp = Fragment.Timestamp;

	// The size and the sender, never the octets: a frame is an image of whoever is on the other camera.
	UE_CLOG(CrowdyReplicationTrace::Entity(), LogCrowdyReplication, Verbose,
		TEXT("[Crowdy Video Receiver]: Frame %d from %s assembled, %d octets."),
		Frame.FrameId, *Frame.SenderUUID.ToString(), Frame.Bytes.Num());

	// Straight from here: every subscriber handler runs on the game thread, so a dynamic delegate can be
	// fired without a hop. See FCrowdyDelivery for the contract.
	OnVideoFrameReady.Broadcast(Frame);
	OnVideoFrameAssembled.Broadcast(Frame);
}

void UCrowdyVideoFrameReceiver::HandleActorLeft(const FCrowdyActorLeft& ActorLeft, int32 ActorCount)
{
	// A sender that has gone will never finish what it started, and its frame counter means nothing to
	// whoever rejoins under the same id later.
	ForgetSender(ActorLeft.UUID);
}

void UCrowdyVideoFrameReceiver::HandleActorTimedOut(FGuid UUID, int32 ActorCount)
{
	ForgetSender(UUID);
}

void UCrowdyVideoFrameReceiver::HandleOwnerUUIDUpdated(FString NewUUID)
{
	// Text that is not 32 octets addresses no actor, and leaves the filter off rather than matching nothing
	// in particular.
	if (!FCrowdyActorId::TryFromString(NewUUID, LocalSenderId))
	{
		LocalSenderId.Reset();
	}
}
