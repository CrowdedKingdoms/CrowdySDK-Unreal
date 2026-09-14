#include "CrowdyCppReplication.h"

#include "CrowdyCppBridge.h"
#include "CrowdyCppVideo.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformTLS.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/ScopeLock.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

THIRD_PARTY_INCLUDES_START
#include "crowdy/core/clock.hpp"
#include "crowdy/core/crypto.hpp"
#include "crowdy/core/logger.hpp"
#include "crowdy/core/uuid.hpp"
#include "crowdy/media/video_frames.hpp"
#include "crowdy/replication/connection.hpp"
#include "crowdy/replication/session_provider.hpp"
#include "crowdy/wire/codec.hpp"
#include "crowdy/wire/protocol.hpp"
THIRD_PARTY_INCLUDES_END

#include <atomic>
#include <exception>
#include <memory>
#include <string>

namespace
{
	using crowdy::wire::MessageType;

	ECrowdyCppConnState ToUnrealState(const crowdy::replication::ConnState State)
	{
		switch (State)
		{
		case crowdy::replication::ConnState::Idle:         return ECrowdyCppConnState::Idle;
		case crowdy::replication::ConnState::Connecting:   return ECrowdyCppConnState::Connecting;
		case crowdy::replication::ConnState::Connected:    return ECrowdyCppConnState::Connected;
		case crowdy::replication::ConnState::Reconnecting: return ECrowdyCppConnState::Reconnecting;
		case crowdy::replication::ConnState::Failed:       return ECrowdyCppConnState::Failed;
		case crowdy::replication::ConnState::Closed:       return ECrowdyCppConnState::Closed;
		}
		return ECrowdyCppConnState::Idle;
	}

	/** True while the connection has a socket to work with, which is the only time pumping or sending achieves anything. */
	bool IsLiveState(const crowdy::replication::ConnState State)
	{
		return State == crowdy::replication::ConnState::Connecting
			|| State == crowdy::replication::ConnState::Connected
			|| State == crowdy::replication::ConnState::Reconnecting;
	}

	FString DescribeErrc(const crowdy::Errc Code)
	{
		return FString(ANSI_TO_TCHAR(crowdy::errcName(Code)));
	}

	TArrayView<const uint8> ToView(const crowdy::Bytes Span)
	{
		if (Span.empty())
		{
			return TArrayView<const uint8>();
		}
		return TArrayView<const uint8>(Span.data(), static_cast<int32>(Span.size()));
	}

	/**
	 * Callers put their own text in these fields and the failure paths log them, so a caller that pastes a raw
	 * server response into one would otherwise write however much of it it liked, credentials included, into the log.
	 */
	FString TruncateForLog(const FString& Message)
	{
		constexpr int32 MaxLoggedCharacters = 200;
		if (Message.IsEmpty())
		{
			return TEXT("no reason given");
		}
		return Message.Len() <= MaxLoggedCharacters ? Message : Message.Left(MaxLoggedCharacters) + TEXT(" ...");
	}

	/**
	 * The wire token is used as raw key octets exactly as written, so a token of any other length cannot sign and
	 * the connection would refuse to open with it.
	 */
	bool IsUsableToken(const FCrowdyCppReplicationToken& Token)
	{
		return Token.bOk && Token.Token.Len() == static_cast<int32>(crowdy::wire::kTokenOctets);
	}

	crowdy::replication::TokenInfo ToLibraryToken(const FCrowdyCppReplicationToken& Token)
	{
		crowdy::replication::TokenInfo Out;
		Out.token = std::string(TCHAR_TO_UTF8(*Token.Token));
		Out.gameTokenId = Token.GameTokenId;
		Out.expiresAtEpochMs = Token.ExpiresAtEpochMs;
		Out.authorizedOnCurrentServer = Token.bAuthorizedOnCurrentServer;
		return Out;
	}

	/**
	 * Bridges the library's session plane to the two callbacks the owner supplied. Both are called on the network
	 * thread and both are allowed to block there. An exception escaping a callback would cross back into the library,
	 * so each is contained and reported as a refusal instead.
	 */
	class FCrowdyCppSessionProvider final : public crowdy::replication::ISessionProvider
	{
	public:
		FCrowdyCppSessionProvider(FCrowdyCppAssignServer InAssign, FCrowdyCppRefreshToken InRefresh,
			const std::atomic<bool>& InAborting, const int64 InMinReassignIntervalMs)
			: Assign(MoveTemp(InAssign))
			, Refresh(MoveTemp(InRefresh))
			, Aborting(InAborting)
			, MinReassignIntervalMs(InMinReassignIntervalMs)
		{
		}

		crowdy::Result<crowdy::replication::Assignment> assignServer() override
		{
			if (IsAborting())
			{
				return crowdy::Errc::Closed;
			}

			WaitOutReassignFloor();

			if (IsAborting())
			{
				return crowdy::Errc::Closed;
			}

			if (!Assign)
			{
				UE_LOG(LogCrowdyCpp, Error, TEXT("Replication: no server-assignment callback was supplied."));
				return crowdy::Errc::Rejected;
			}

			FCrowdyCppSessionAssignment Answer;
			try
			{
				Answer = Assign(MakeAbortPredicate());
			}
			catch (const std::exception& Ex)
			{
				UE_LOG(LogCrowdyCpp, Error, TEXT("Replication: server assignment threw: %hs"), Ex.what());
				return crowdy::Errc::Rejected;
			}
			catch (...)
			{
				UE_LOG(LogCrowdyCpp, Error, TEXT("Replication: server assignment threw a non-standard exception."));
				return crowdy::Errc::Rejected;
			}

			LastAssignSeconds = FPlatformTime::Seconds();

			if (!Answer.bOk)
			{
				UE_LOG(LogCrowdyCpp, Warning, TEXT("Replication: server assignment failed: %s"),
					*TruncateForLog(Answer.ErrorMessage));
				return crowdy::Errc::Rejected;
			}

			crowdy::replication::Assignment Out;
			Out.ip4 = std::string(TCHAR_TO_UTF8(*Answer.Ip4));
			Out.ip6 = std::string(TCHAR_TO_UTF8(*Answer.Ip6));
			Out.clientPort = Answer.ClientPort;
			return Out;
		}

		crowdy::Result<crowdy::replication::TokenInfo> refreshToken() override
		{
			return RefreshWithCurrentServer(nullptr);
		}

		/**
		 * The server-aware rotation. The library calls this one and names the server the connection is on, which is
		 * what lets the answer report the replacement token as already authorized there.
		 */
		crowdy::Result<crowdy::replication::TokenInfo> refreshToken(
			const crowdy::replication::Assignment* Current) override
		{
			if (!Current)
			{
				return RefreshWithCurrentServer(nullptr);
			}

			FCrowdyCppCurrentServer Server;
			Server.Ip4 = UTF8_TO_TCHAR(Current->ip4.c_str());
			Server.ClientPort = Current->clientPort;
			return RefreshWithCurrentServer(&Server);
		}

	private:

		crowdy::Result<crowdy::replication::TokenInfo> RefreshWithCurrentServer(const FCrowdyCppCurrentServer* Current)
		{
			if (IsAborting())
			{
				return crowdy::Errc::Closed;
			}

			if (!Refresh)
			{
				UE_LOG(LogCrowdyCpp, Error, TEXT("Replication: no token-refresh callback was supplied."));
				return crowdy::Errc::Rejected;
			}

			FCrowdyCppReplicationToken Answer;
			try
			{
				Answer = Refresh(MakeAbortPredicate(), Current);
			}
			catch (const std::exception& Ex)
			{
				UE_LOG(LogCrowdyCpp, Error, TEXT("Replication: token refresh threw: %hs"), Ex.what());
				return crowdy::Errc::Rejected;
			}
			catch (...)
			{
				UE_LOG(LogCrowdyCpp, Error, TEXT("Replication: token refresh threw a non-standard exception."));
				return crowdy::Errc::Rejected;
			}

			if (!IsUsableToken(Answer))
			{
				UE_LOG(LogCrowdyCpp, Warning, TEXT("Replication: token refresh yielded no usable token: %s"),
					*TruncateForLog(Answer.ErrorMessage));
				return crowdy::Errc::Rejected;
			}

			return ToLibraryToken(Answer);
		}

		bool IsAborting() const { return Aborting.load(std::memory_order_acquire); }

		FCrowdyCppShouldAbort MakeAbortPredicate() const
		{
			const std::atomic<bool>* Flag = &Aborting;
			return [Flag]() { return Flag->load(std::memory_order_acquire); };
		}

		/**
		 * A re-assignment can be provoked from the wire, and each one is a blocking round trip against the Game API,
		 * so a floor between them is what keeps a trickle of forged datagrams from becoming a request storm. Waiting
		 * here rather than refusing is deliberate: refusing would put the connection into a failed state that
		 * nothing retries, which is a better outcome for the attacker than a slow reconnect.
		 */
		void WaitOutReassignFloor()
		{
			if (MinReassignIntervalMs <= 0 || LastAssignSeconds <= 0.0)
			{
				return;
			}

			const double ReadyAt = LastAssignSeconds + static_cast<double>(MinReassignIntervalMs) / 1000.0;
			while (FPlatformTime::Seconds() < ReadyAt && !IsAborting())
			{
				FPlatformProcess::Sleep(0.02f);
			}
		}

		FCrowdyCppAssignServer Assign;
		FCrowdyCppRefreshToken Refresh;
		const std::atomic<bool>& Aborting;
		int64 MinReassignIntervalMs = 0;

		// Network thread only.
		double LastAssignSeconds = 0.0;
	};

	/** One send waiting for the network thread. The payload is copied because the caller's view does not outlive the call. */
	struct FCrowdyCppQueuedSend
	{
		bool bIsChannel = false;
		uint8 Opcode = 0;
		int64 ChannelId = 0;
		int64 ChunkX = 0;
		int64 ChunkY = 0;
		int64 ChunkZ = 0;
		crowdy::core::ActorUuid Uuid{};
		TArray<uint8> Payload;
		uint8 Distance = 0;
		uint8 Decay = 0;
	};

	/** What one sequence number was last used for, so an error frame naming it can say what it is about. */
	struct FCrowdySendRecord
	{
		int64 SentAtMs = 0;
		crowdy::core::ActorUuid Uuid{};
		uint8 Opcode = 0;
		bool bIsChannel = false;
		bool bUsed = false;
	};

	/**
	 * One slot per sequence value. The network thread writes as it puts sends on the wire and the polling thread
	 * reads when an error arrives, so the two are separated by a lock rather than by a thread confinement claim.
	 * A slot is overwritten every time its sequence comes round again, which is the whole reason an attribution
	 * carries an age and is refused once it is stale.
	 */
	class FCrowdySendLedger
	{
	public:
		void Record(uint8 Sequence, const FCrowdyCppQueuedSend& Send, int64 NowMs)
		{
			FScopeLock Lock(&Mutex);
			FCrowdySendRecord& Slot = Slots[Sequence];
			Slot.SentAtMs = NowMs;
			Slot.Uuid = Send.Uuid;
			Slot.Opcode = Send.Opcode;
			Slot.bIsChannel = Send.bIsChannel;
			Slot.bUsed = true;
		}

		/** The send this sequence was last used for, or an unused record when there is none worth believing. */
		FCrowdySendRecord Lookup(uint8 Sequence, int64 NowMs, int64 MaxAgeMs) const
		{
			FScopeLock Lock(&Mutex);
			const FCrowdySendRecord& Slot = Slots[Sequence];
			// The flag is not redundant against the age check, though it looks it: an untouched slot has a send
			// time of zero, which the age check only rejects because the clock is normally well past the window
			// by the time anything sends. Inside the first few seconds of the process it is not, and then every
			// sequence nothing has used would answer with a zeroed record.
			if (!Slot.bUsed || NowMs - Slot.SentAtMs > MaxAgeMs || NowMs < Slot.SentAtMs)
			{
				return FCrowdySendRecord();
			}
			return Slot;
		}

		void Reset()
		{
			FScopeLock Lock(&Mutex);
			for (FCrowdySendRecord& Slot : Slots)
			{
				Slot = FCrowdySendRecord();
			}
		}

	private:
		mutable FCriticalSection Mutex;
		FCrowdySendRecord Slots[256];
	};
}

struct FCrowdyCppReplication::FImpl
{
	// Declared before the connection so they outlive it: the connection holds forwarding lambdas that read them,
	// and members are released in reverse order of declaration.
	mutable FCriticalSection HandlerMutex;
	FCrowdyCppReplicationHandlers Handlers;

	// The set the forwarding lambdas actually read, refreshed once per Poll. Polling-thread only, which is what
	// lets the lambdas read it without a lock on the receive path.
	FCrowdyCppReplicationHandlers ActiveHandlers;

	mutable FCriticalSection SendQueueMutex;
	TArray<FCrowdyCppQueuedSend> SendQueue;

	// The array DrainSendQueue swaps in for SendQueue each cycle, so neither has to grow from nothing
	// again. Touched only by the pump thread, and only outside the lock apart from the swap itself.
	TArray<FCrowdyCppQueuedSend> DrainBatch;
	int32 MaxQueuedSends = 2048;
	std::atomic<int64> SendsDropped{0};

	FCrowdySendLedger SendLedger;

	FCriticalSection TeardownMutex;

	std::atomic<bool> bConnectRequested{false};
	std::atomic<bool> bStopping{false};

	// Read by a blocking session callback so teardown does not have to wait out a network round trip.
	std::atomic<bool> bAborting{false};

	// Set by Disconnect so no callback can be delivered afterwards, which is what makes the teardown guarantee
	// true rather than merely likely: the connection's own closing state change is still sitting in the queue.
	std::atomic<bool> bClosed{false};

	std::atomic<uint32> PumpThreadId{0};
	std::atomic<uint32> PollThreadId{0};

	// Polling-thread only. The queue between the network thread and Poll drops a state change when it is full, so
	// the real state is reconciled against this on every Poll rather than trusting the queue to carry every one.
	ECrowdyCppConnState LastReportedState = ECrowdyCppConnState::Idle;

	std::shared_ptr<FCrowdyCppSessionProvider> Provider;
	TUniquePtr<crowdy::replication::Connection> Connection;

	FRunnableThread* Thread = nullptr;
	FRunnable* Pump = nullptr;

	FCrowdyCppReplicationHandlers SnapshotHandlers() const
	{
		FScopeLock Lock(&HandlerMutex);
		return Handlers;
	}

	/** Hands one queued message to the library. Network thread only, so it cannot race the reconnect path's socket. */
	void PerformSend(const FCrowdyCppQueuedSend& Send)
	{
		if (!Connection)
		{
			return;
		}

		const crowdy::wire::ChunkCoord Chunk{Send.ChunkX, Send.ChunkY, Send.ChunkZ};
		const crowdy::Bytes Payload(Send.Payload.GetData(), static_cast<std::size_t>(Send.Payload.Num()));
		const auto Decay = static_cast<crowdy::wire::DecayRate>(Send.Decay);

		crowdy::replication::SpatialSend Spatial;
		Spatial.chunk = Chunk;
		Spatial.uuid = Send.Uuid;
		Spatial.payload = Payload;
		Spatial.distance = Send.Distance;
		Spatial.decay = Decay;

		crowdy::Result<std::uint8_t> Sent = crowdy::Errc::InvalidArgument;

		try
		{
			if (Send.bIsChannel)
			{
				Sent = Connection->sendChannelMessage(Send.ChannelId, Send.Uuid, Payload);
			}
			else
			{
				switch (static_cast<MessageType>(Send.Opcode))
				{
				case MessageType::ActorUpdateRequest:
					Sent = Connection->sendActorUpdate(Spatial);
					break;

				case MessageType::ClientAudioPacket:
					Sent = Connection->sendAudio(Spatial);
					break;

				case MessageType::ClientVideoPacket:
					// One fragment. A whole frame was split into these before it was queued, so the split is
					// not repeated here and the library's own frame-level send is not used.
					Sent = Connection->sendVideo(Spatial);
					break;

				case MessageType::ClientTextPacket:
					Sent = Connection->sendText(Spatial);
					break;

				case MessageType::GenericSpatial1:
					Sent = Connection->sendGenericSpatial(Spatial);
					break;

				case MessageType::ClientActorHeartbeat:
					Sent = Connection->sendHeartbeat(Chunk, Send.Uuid);
					break;

				case MessageType::SingleActorMessage:
					Sent = Connection->sendSingleActorMessage(Chunk, Send.Uuid, Payload);
					break;

				case MessageType::VoxelUpdateRequest:
				{
					// The typed send composes the voxel payload from its parts, so the parts are recovered here with
					// the library's own reader. The caller's payload was checked for self-consistency when it was
					// queued, so this split is exact rather than lossy.
					const crowdy::Result<crowdy::wire::VoxelPayloadView> Voxel =
						crowdy::wire::parseVoxelPayload(Payload);
					if (!Voxel.ok())
					{
						Sent = Voxel.error();
						break;
					}
					Sent = Connection->sendVoxelUpdate(Chunk, Send.Uuid, Voxel->x, Voxel->y, Voxel->z,
						Voxel->voxelType, Voxel->state, Send.Distance, Decay);
					break;
				}

				case MessageType::ClientEventNotification:
				{
					// Same reasoning as the voxel case: the event type is a field of the payload the typed send
					// writes itself, so it is read back out with the library's reader and the remainder rides as
					// the event state.
					const crowdy::Result<crowdy::wire::EventPayloadView> Event =
						crowdy::wire::parseEventPayload(Payload);
					if (!Event.ok())
					{
						Sent = Event.error();
						break;
					}
					Sent = Connection->sendClientEvent(Chunk, Send.Uuid, Event->eventType, Event->state,
						Send.Distance, Decay);
					break;
				}

				default:
					break;
				}
			}
		}
		catch (const std::exception& Ex)
		{
			UE_LOG(LogCrowdyCpp, Error, TEXT("Replication: a send threw: %hs"), Ex.what());
			SendsDropped.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		catch (...)
		{
			UE_LOG(LogCrowdyCpp, Error, TEXT("Replication: a send threw a non-standard exception."));
			SendsDropped.fetch_add(1, std::memory_order_relaxed);
			return;
		}

		// A full kernel send buffer is backpressure, not a fault. The message never left and the socket is healthy,
		// so this is neither a drop nor worth a warning: the library already counts it in sendsDeferred, and warning
		// per message would turn one busy frame into a log flood. It is deliberately not retried, because a state
		// update is only worth sending while it is current and the next one supersedes it. Saturation is visible in
		// FCrowdyCppReplicationStats::SendsDeferred, and the answer to it is a larger SocketSendBufferBytes. Under
		// bundling an accepted message has joined the pending bundle, and flushing that is the library's.
		if (!Sent.ok() && Sent.error() == crowdy::Errc::WouldBlock)
		{
			return;
		}

		if (!Sent.ok())
		{
			SendsDropped.fetch_add(1, std::memory_order_relaxed);
			UE_CLOG(Sent.error() != crowdy::Errc::NotConnected, LogCrowdyCpp, Warning,
				TEXT("Replication: a queued send was rejected (%s)."), *DescribeErrc(Sent.error()));
			return;
		}

		// The sequence the library minted for this datagram exists nowhere else: the caller was told only that the
		// send was queued. Noting it here is what lets a later error frame naming that sequence say which send it
		// is about, and this is the only point in the process where both halves are in hand.
		SendLedger.Record(Sent.value(), Send, crowdy::core::systemClock().monotonicMillis());
	}

	void DrainSendQueue()
	{
		// Swapped rather than stolen: moving SendQueue out left it with no allocation at all, so the next
		// cycle's first send had to grow it again from nothing. DrainBatch comes back emptied but still
		// allocated, so after the first cycle neither array allocates.
		{
			FScopeLock Lock(&SendQueueMutex);
			Swap(SendQueue, DrainBatch);
		}

		for (const FCrowdyCppQueuedSend& Send : DrainBatch)
		{
			PerformSend(Send);
		}

		// A drain is this thread's frame boundary: everything it just handed to the library leaves now rather
		// than at the end of the bundle window. A no-op with nothing pending or bundling off. A full kernel
		// buffer keeps the bundle pending, and the library's pump then returns without waiting so it can retry;
		// one window of sleep keeps that retry at the normal pass cadence instead of a spin until the buffer
		// drains.
		if (Connection && Connection->flushSends().code == crowdy::Errc::WouldBlock)
		{
			FPlatformProcess::Sleep(0.001f);
		}

		// Outside the lock, and only the pump thread reaches here. Reset keeps the array's own allocation
		// for the next swap; the per-send payload arrays are released with their elements.
		DrainBatch.Reset();
	}

	bool EnqueueSend(FCrowdyCppQueuedSend&& Queued, FString& OutError)
	{
		if (!Connection || !IsLiveState(Connection->state()))
		{
			OutError = TEXT("the connection is not open");
			return false;
		}

		FScopeLock Lock(&SendQueueMutex);

		// Closing takes the same lock before it discards the queue, so a send that gets here is either queued ahead
		// of the teardown or refused by it, rather than landing in a queue nobody will drain.
		if (bClosed.load(std::memory_order_acquire))
		{
			OutError = TEXT("the connection is closed");
			return false;
		}

		if (SendQueue.Num() >= MaxQueuedSends)
		{
			SendsDropped.fetch_add(1, std::memory_order_relaxed);
			OutError = TEXT("the outbound queue is full");
			return false;
		}

		SendQueue.Add(MoveTemp(Queued));
		return true;
	}

	/**
	 * Queue several sends as one unit: either every one is accepted or none is.
	 *
	 * The queue is measured against the whole batch under a single hold of the lock, which is what makes
	 * the guarantee real. A caller that queued the same messages one at a time would get a prefix of them
	 * accepted and the rest refused whenever the queue filled part way through.
	 */
	bool EnqueueSends(TArray<FCrowdyCppQueuedSend>&& Batch, FString& OutError)
	{
		if (Batch.IsEmpty())
		{
			OutError = TEXT("there is nothing to send");
			return false;
		}

		if (!Connection || !IsLiveState(Connection->state()))
		{
			OutError = TEXT("the connection is not open");
			return false;
		}

		FScopeLock Lock(&SendQueueMutex);

		if (bClosed.load(std::memory_order_acquire))
		{
			OutError = TEXT("the connection is closed");
			return false;
		}

		if (SendQueue.Num() + Batch.Num() > MaxQueuedSends)
		{
			SendsDropped.fetch_add(Batch.Num(), std::memory_order_relaxed);
			OutError = FString::Printf(TEXT("the outbound queue has room for %d more sends, not the %d this one needs"),
				FMath::Max(MaxQueuedSends - SendQueue.Num(), 0), Batch.Num());
			return false;
		}

		SendQueue.Append(MoveTemp(Batch));
		return true;
	}

	void DiscardQueuedSends()
	{
		FScopeLock Lock(&SendQueueMutex);
		if (SendQueue.Num() > 0)
		{
			SendsDropped.fetch_add(SendQueue.Num(), std::memory_order_relaxed);
			SendQueue.Reset();
		}
	}

	/**
	 * Drives the socket and the connection's lifecycle housekeeping, and puts every queued send on the wire. It is
	 * the only thread that touches the library, which is what keeps a send from racing the reconnect path's close
	 * and reopen of the socket.
	 */
	class FPump final : public FRunnable
	{
	public:
		explicit FPump(FImpl& InOwner) : Owner(InOwner) {}

		virtual uint32 Run() override
		{
			// Short enough that a stop is observed promptly, long enough that a connection with no socket is not
			// spinning for the rest of the session.
			constexpr float IdleSleepSeconds = 0.1f;
			constexpr int32 PumpWaitMs = 20;

			Owner.PumpThreadId.store(FPlatformTLS::GetCurrentThreadId(), std::memory_order_release);

			while (!Owner.bStopping.load(std::memory_order_acquire))
			{
				try
				{
					if (Owner.bConnectRequested.exchange(false, std::memory_order_acq_rel))
					{
						const crowdy::Status Started = Owner.Connection->connect();
						if (!Started.ok())
						{
							UE_LOG(LogCrowdyCpp, Warning, TEXT("Replication: connect failed (%s)."),
								*DescribeErrc(Started.code));
						}
					}

					if (IsLiveState(Owner.Connection->state()))
					{
						Owner.DrainSendQueue();
						Owner.Connection->pump(PumpWaitMs);
						continue;
					}

					// Nothing queued can go anywhere without a socket, and holding it would mean delivering it
					// against a session the server no longer has.
					Owner.DiscardQueuedSends();
				}
				catch (const std::exception& Ex)
				{
					UE_LOG(LogCrowdyCpp, Error, TEXT("Replication: network thread threw: %hs"), Ex.what());
				}
				catch (...)
				{
					UE_LOG(LogCrowdyCpp, Error, TEXT("Replication: network thread threw a non-standard exception."));
				}

				FPlatformProcess::Sleep(IdleSleepSeconds);
			}

			return 0;
		}

		virtual void Stop() override
		{
			Owner.bStopping.store(true, std::memory_order_release);
		}

	private:
		FImpl& Owner;
	};
};

FCrowdyCppReplication::FCrowdyCppReplication()
	: Impl(MakeUnique<FImpl>())
{
}

FCrowdyCppReplication::~FCrowdyCppReplication()
{
	Disconnect();
}

TSharedPtr<FCrowdyCppReplication> FCrowdyCppReplication::Make(const FCrowdyCppReplicationConfig& Config,
	FCrowdyCppAssignServer AssignServer, FCrowdyCppRefreshToken RefreshToken)
{
	if (!IsUsableToken(Config.Token))
	{
		UE_LOG(LogCrowdyCpp, Error,
			TEXT("Replication: cannot open a connection without a %d-character app-scoped token."),
			static_cast<int32>(crowdy::wire::kTokenOctets));
		return nullptr;
	}

	TSharedPtr<FCrowdyCppReplication> Wrapper = MakeShareable(new FCrowdyCppReplication());
	FImpl& Owned = *Wrapper->Impl;
	Owned.MaxQueuedSends = FMath::Clamp(Config.MaxQueuedSends, 1, 65536);

	try
	{
		Owned.Provider = std::make_shared<FCrowdyCppSessionProvider>(MoveTemp(AssignServer), MoveTemp(RefreshToken),
			Owned.bAborting, Config.MinReassignIntervalMs);

		crowdy::replication::Config Library;
		Library.appId = Config.AppId;
		Library.token = ToLibraryToken(Config.Token);
		// The facade owns the network thread, so the library must not spawn one of its own.
		Library.manualPump = true;
		Library.preferIpv6 = Config.bPreferIpv6;
		Library.sessionReadyWaitMs = Config.SessionReadyWaitMs;
		Library.refreshLeadMs = Config.RefreshLeadMs;
		// Check the server signature on every frame that carries one. This does not make every delivered frame
		// authentic: the protocol lets a spatial frame declare that it carries no signature, and a channel
		// notification has nowhere to put one, so both reach the handlers unverified. Consumers are told as much on
		// the message structs.
		Library.verifyNotifications = true;
		Library.watchdogSilenceMs = Config.WatchdogSilenceMs;
		// Each slot holds a whole datagram, so an unclamped value here is a quiet way to allocate hundreds of
		// megabytes.
		Library.ringCapacity = static_cast<std::size_t>(FMath::Clamp(Config.RingCapacity, 16, 65536));
		Library.socketRecvBufferBytes = Config.SocketRecvBufferBytes;
		Library.socketSendBufferBytes = Config.SocketSendBufferBytes;
		Library.bundleSends = Config.bBundleSends;
		Library.bundleWindowMs = FMath::Max(Config.BundleWindowMs, 0);

		Owned.Connection = MakeUnique<crowdy::replication::Connection>(
			std::move(Library), Owned.Provider, crowdy::core::defaultCrypto());
	}
	catch (const std::exception& Ex)
	{
		UE_LOG(LogCrowdyCpp, Error, TEXT("Replication: connection construction threw: %hs"), Ex.what());
		return nullptr;
	}
	catch (...)
	{
		UE_LOG(LogCrowdyCpp, Error, TEXT("Replication: connection construction threw a non-standard exception."));
		return nullptr;
	}

	FImpl* Raw = Wrapper->Impl.Get();

	crowdy::replication::Handlers Forward;
	Forward.any = [Raw](const crowdy::replication::SpatialNotification& Notification)
	{
		const uint8 Opcode = static_cast<uint8>(Notification.type);
		// The decoder accepts every opcode that shares the long-spatial layout, including the request opcodes only a
		// client sends. Delivering one of those would walk a consumer into handling its own outbound traffic.
		if (!IsDeliverableSpatialOpcode(Opcode) || !Raw->ActiveHandlers.OnSpatial)
		{
			return;
		}

		FCrowdyCppSpatialMessage Message;
		Message.Opcode = Opcode;
		Message.AppId = Notification.appId;
		Message.ChunkX = Notification.chunk.x;
		Message.ChunkY = Notification.chunk.y;
		Message.ChunkZ = Notification.chunk.z;
		Message.Uuid = TArrayView<const uint8>(
			reinterpret_cast<const uint8*>(Notification.uuid), static_cast<int32>(crowdy::wire::kUuidSize));
		Message.Payload = ToView(Notification.payload);
		Message.EpochMillis = Notification.epochMillis;
		Message.Sequence = Notification.sequence;

		try
		{
			Raw->ActiveHandlers.OnSpatial(Message);
		}
		catch (const std::exception& Ex)
		{
			UE_LOG(LogCrowdyCpp, Error, TEXT("Replication: a spatial handler threw: %hs"), Ex.what());
		}
		catch (...)
		{
			UE_LOG(LogCrowdyCpp, Error, TEXT("Replication: a spatial handler threw a non-standard exception."));
		}
	};

	Forward.channelMessage = [Raw](const crowdy::replication::ChannelNotification& Notification)
	{
		if (!Raw->ActiveHandlers.OnChannel)
		{
			return;
		}

		FCrowdyCppChannelMessage Message;
		Message.ChannelId = Notification.channelId;
		Message.SenderUuid = TArrayView<const uint8>(
			reinterpret_cast<const uint8*>(Notification.senderUuid), static_cast<int32>(crowdy::wire::kUuidSize));
		Message.Payload = ToView(Notification.payload);
		Message.EpochMillis = Notification.epochMillis;
		Message.Sequence = Notification.sequence;

		try
		{
			Raw->ActiveHandlers.OnChannel(Message);
		}
		catch (const std::exception& Ex)
		{
			UE_LOG(LogCrowdyCpp, Error, TEXT("Replication: a channel handler threw: %hs"), Ex.what());
		}
		catch (...)
		{
			UE_LOG(LogCrowdyCpp, Error, TEXT("Replication: a channel handler threw a non-standard exception."));
		}
	};

	Forward.genericError = [Raw](const crowdy::replication::GenericError& Error)
	{
		if (!Raw->ActiveHandlers.OnError)
		{
			return;
		}

		const int64 NowMs = crowdy::core::systemClock().monotonicMillis();
		const FCrowdySendRecord Attributed = Raw->SendLedger.Lookup(
			Error.sequence, NowMs, FCrowdyCppReplication::MaxSendAttributionAgeMs);

		FCrowdyCppSendError Reported;
		Reported.Sequence = Error.sequence;
		Reported.ErrorCode = static_cast<uint8>(Error.code);
		Reported.bAttributed = Attributed.bUsed;
		if (Attributed.bUsed)
		{
			Reported.SendOpcode = Attributed.Opcode;
			Reported.bSendWasChannel = Attributed.bIsChannel;
			// Borrows the record, which is a copy taken under the ledger's lock and lives for this whole call.
			Reported.SendUuid = TArrayView<const uint8>(
				reinterpret_cast<const uint8*>(Attributed.Uuid.data()), Attributed.Uuid.size());
			Reported.SendAgeMs = NowMs - Attributed.SentAtMs;
		}

		try
		{
			Raw->ActiveHandlers.OnError(Reported);
		}
		catch (const std::exception& Ex)
		{
			UE_LOG(LogCrowdyCpp, Error, TEXT("Replication: an error handler threw: %hs"), Ex.what());
		}
		catch (...)
		{
			UE_LOG(LogCrowdyCpp, Error, TEXT("Replication: an error handler threw a non-standard exception."));
		}
	};

	Forward.status = [Raw](const crowdy::replication::ConnState State)
	{
		const ECrowdyCppConnState Reported = ToUnrealState(State);
		Raw->LastReportedState = Reported;
		if (!Raw->ActiveHandlers.OnStateChanged)
		{
			return;
		}

		try
		{
			Raw->ActiveHandlers.OnStateChanged(Reported);
		}
		catch (const std::exception& Ex)
		{
			UE_LOG(LogCrowdyCpp, Error, TEXT("Replication: a state handler threw: %hs"), Ex.what());
		}
		catch (...)
		{
			UE_LOG(LogCrowdyCpp, Error, TEXT("Replication: a state handler threw a non-standard exception."));
		}
	};

	Owned.Connection->setHandlers(std::move(Forward));

	Owned.Pump = new FImpl::FPump(Owned);
	Owned.Thread = FRunnableThread::Create(Owned.Pump, TEXT("CrowdyReplicationPump"), 0, TPri_AboveNormal);
	if (!Owned.Thread)
	{
		UE_LOG(LogCrowdyCpp, Error, TEXT("Replication: could not start the network thread."));
		delete Owned.Pump;
		Owned.Pump = nullptr;
		return nullptr;
	}

	return Wrapper;
}

void FCrowdyCppReplication::SetHandlers(FCrowdyCppReplicationHandlers Handlers)
{
	FScopeLock Lock(&Impl->HandlerMutex);
	Impl->Handlers = MoveTemp(Handlers);
}

void FCrowdyCppReplication::SetToken(const FCrowdyCppReplicationToken& Token)
{
	if (Impl->bClosed.load(std::memory_order_acquire) || !Impl->Connection)
	{
		return;
	}

	if (!IsUsableToken(Token))
	{
		UE_LOG(LogCrowdyCpp, Warning, TEXT("Replication: refused a token that cannot sign; keeping the current one."));
		return;
	}

	try
	{
		Impl->Connection->setToken(ToLibraryToken(Token));
	}
	catch (...)
	{
		UE_LOG(LogCrowdyCpp, Error, TEXT("Replication: installing a token threw."));
	}
}

void FCrowdyCppReplication::ConnectAsync()
{
	if (Impl->bClosed.load(std::memory_order_acquire))
	{
		UE_LOG(LogCrowdyCpp, Warning, TEXT("Replication: ConnectAsync on a closed connection does nothing."));
		return;
	}

	Impl->bConnectRequested.store(true, std::memory_order_release);
}

void FCrowdyCppReplication::Disconnect()
{
	// A session callback runs on the network thread, so a callback that calls this would have the thread wait for
	// itself. Refusing loudly is the only outcome that is not a permanent hang.
	if (Impl->PumpThreadId.load(std::memory_order_acquire) == FPlatformTLS::GetCurrentThreadId())
	{
		UE_LOG(LogCrowdyCpp, Error,
			TEXT("Replication: Disconnect was called from the network thread and was ignored; a session callback "
				"must not tear down the connection it is running for."));
		return;
	}

	// Held for the whole teardown rather than around a flag, so a second caller waits for the first to finish
	// instead of returning while the thread and the socket are still going away.
	FScopeLock Teardown(&Impl->TeardownMutex);

	if (Impl->bClosed.exchange(true, std::memory_order_acq_rel))
	{
		return;
	}

	// Set before the thread is asked to stop, so a blocking session callback can see it and give up.
	Impl->bAborting.store(true, std::memory_order_release);
	Impl->bStopping.store(true, std::memory_order_release);

	if (Impl->Thread)
	{
		Impl->Thread->Kill(true);
		delete Impl->Thread;
		Impl->Thread = nullptr;
	}

	if (Impl->Pump)
	{
		delete Impl->Pump;
		Impl->Pump = nullptr;
	}

	Impl->DiscardQueuedSends();

	// The sequence counter belongs to the connection, so records kept against it stop meaning anything once this
	// one is gone. Cleared here rather than left to age out, so a later connection cannot inherit an attribution.
	Impl->SendLedger.Reset();

	// Only safe once the network thread is gone: it closes the socket, which that thread reads on every pass.
	if (Impl->Connection)
	{
		try
		{
			Impl->Connection->disconnect();
		}
		catch (...)
		{
			UE_LOG(LogCrowdyCpp, Error, TEXT("Replication: disconnect threw."));
		}
	}

	{
		FScopeLock Lock(&Impl->HandlerMutex);
		Impl->Handlers = FCrowdyCppReplicationHandlers();
	}
	Impl->ActiveHandlers = FCrowdyCppReplicationHandlers();
}

ECrowdyCppConnState FCrowdyCppReplication::GetState() const
{
	if (!Impl->Connection)
	{
		return ECrowdyCppConnState::Closed;
	}
	return ToUnrealState(Impl->Connection->state());
}

int32 FCrowdyCppReplication::Poll(const int32 MaxEvents)
{
	if (MaxEvents <= 0)
	{
		return 0;
	}

	// A handler is allowed to release the last reference to this object. Pinning it here keeps the rest of the
	// drain, and the library frame it is running inside, from continuing on a destroyed connection.
	const TSharedRef<FCrowdyCppReplication> Pin = AsShared();

	if (Impl->bClosed.load(std::memory_order_acquire) || !Impl->Connection)
	{
		return 0;
	}

#if !UE_BUILD_SHIPPING
	{
		const uint32 ThisThread = FPlatformTLS::GetCurrentThreadId();
		uint32 Expected = 0;
		if (!Impl->PollThreadId.compare_exchange_strong(Expected, ThisThread))
		{
			ensureMsgf(Expected == ThisThread,
				TEXT("Replication: Poll is being called from two threads, which corrupts the queue it drains."));
		}
	}
#endif

	Impl->ActiveHandlers = Impl->SnapshotHandlers();

	int32 Delivered = 0;
	try
	{
		Delivered = static_cast<int32>(Impl->Connection->poll(static_cast<std::size_t>(MaxEvents)));
	}
	catch (const std::exception& Ex)
	{
		UE_LOG(LogCrowdyCpp, Error, TEXT("Replication: delivering messages threw: %hs"), Ex.what());
	}
	catch (...)
	{
		UE_LOG(LogCrowdyCpp, Error, TEXT("Replication: delivering messages threw a non-standard exception."));
	}

	// The queue drops a state change when it is full, so a caller driven by the state handler could otherwise miss
	// the connection dying. Reconcile against the real state rather than trusting the queue to have carried it.
	const ECrowdyCppConnState Actual = ToUnrealState(Impl->Connection->state());
	if (Actual != Impl->LastReportedState)
	{
		Impl->LastReportedState = Actual;
		if (Impl->ActiveHandlers.OnStateChanged)
		{
			try
			{
				Impl->ActiveHandlers.OnStateChanged(Actual);
			}
			catch (const std::exception& Ex)
			{
				UE_LOG(LogCrowdyCpp, Error, TEXT("Replication: a state handler threw: %hs"), Ex.what());
			}
			catch (...)
			{
				UE_LOG(LogCrowdyCpp, Error, TEXT("Replication: a state handler threw a non-standard exception."));
			}
		}
	}

	Impl->ActiveHandlers = FCrowdyCppReplicationHandlers();
	return Delivered;
}

bool FCrowdyCppReplication::IsSendableSpatialOpcode(const uint8 Opcode)
{
	switch (static_cast<MessageType>(Opcode))
	{
	case MessageType::ActorUpdateRequest:
	case MessageType::VoxelUpdateRequest:
	case MessageType::ClientAudioPacket:
	case MessageType::ClientVideoPacket:
	case MessageType::ClientTextPacket:
	case MessageType::ClientEventNotification:
	case MessageType::GenericSpatial1:
	case MessageType::SingleActorMessage:
	case MessageType::ClientActorHeartbeat:
		return true;
	default:
		return false;
	}
}

bool FCrowdyCppReplication::IsDeliverableSpatialOpcode(const uint8 Opcode)
{
	switch (static_cast<MessageType>(Opcode))
	{
	case MessageType::ActorUpdateNotification:
	case MessageType::VoxelUpdateNotification:
	case MessageType::ClientAudioNotification:
	case MessageType::ClientVideoNotification:
	case MessageType::ClientTextNotification:
	// A client event is replicated back out under the same opcode it was sent with, so this one is on both lists.
	case MessageType::ClientEventNotification:
	case MessageType::ServerEventNotification:
	case MessageType::GenericSpatial1:
	case MessageType::SingleActorMessage:
	case MessageType::ActorLeftNotification:
		return true;
	default:
		return false;
	}
}

FString FCrowdyCppReplication::DescribeErrorCode(const uint8 ErrorCode)
{
	switch (static_cast<crowdy::wire::ErrorCode>(ErrorCode))
	{
	case crowdy::wire::ErrorCode::NoError:              return TEXT("NoError");
	case crowdy::wire::ErrorCode::UnknownError:         return TEXT("UnknownError");
	case crowdy::wire::ErrorCode::InvalidToken:         return TEXT("InvalidToken");
	case crowdy::wire::ErrorCode::AppNotFound:          return TEXT("AppNotFound");
	case crowdy::wire::ErrorCode::Unauthorized:         return TEXT("Unauthorized");
	case crowdy::wire::ErrorCode::GameTokenWrongSize:   return TEXT("GameTokenWrongSize");
	case crowdy::wire::ErrorCode::InvalidRequest:       return TEXT("InvalidRequest");
	case crowdy::wire::ErrorCode::InvalidAppId:         return TEXT("InvalidAppId");
	case crowdy::wire::ErrorCode::UserNotAuthenticated: return TEXT("UserNotAuthenticated");
	case crowdy::wire::ErrorCode::TokenExpired:         return TEXT("TokenExpired");
	default:                                            return TEXT("Unrecognised");
	}
}

bool FCrowdyCppReplication::IsUnauthorizedErrorCode(const uint8 ErrorCode)
{
	return static_cast<crowdy::wire::ErrorCode>(ErrorCode) == crowdy::wire::ErrorCode::Unauthorized;
}

bool FCrowdyCppReplication::SendSpatial(const uint8 Opcode, const int64 ChunkX, const int64 ChunkY, const int64 ChunkZ,
	const TArrayView<const uint8> Uuid, const TArrayView<const uint8> Payload, const uint8 Distance, const uint8 Decay,
	FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Crowdy_SendSpatial);

	if (Impl->bClosed.load(std::memory_order_acquire) || !Impl->Connection)
	{
		OutError = TEXT("the connection is closed");
		return false;
	}

	if (!IsSendableSpatialOpcode(Opcode))
	{
		OutError = FString::Printf(TEXT("opcode %u is not one this connection can send"), Opcode);
		return false;
	}

	if (Uuid.Num() != static_cast<int32>(crowdy::wire::kUuidSize))
	{
		OutError = FString::Printf(TEXT("the actor id is %d octets, not %d"),
			Uuid.Num(), static_cast<int32>(crowdy::wire::kUuidSize));
		return false;
	}

	const int32 PayloadSize = FMath::Max(Payload.Num(), 0);
	if (PayloadSize > static_cast<int32>(crowdy::wire::kMaxLongSpatialPayload))
	{
		OutError = FString::Printf(TEXT("the payload is %d octets, over the %d a single datagram carries"),
			PayloadSize, static_cast<int32>(crowdy::wire::kMaxLongSpatialPayload));
		return false;
	}

	const crowdy::Bytes PayloadBytes(Payload.GetData(), static_cast<std::size_t>(PayloadSize));

	switch (static_cast<MessageType>(Opcode))
	{
	case MessageType::ClientActorHeartbeat:
		if (PayloadSize != 0)
		{
			OutError = TEXT("a heartbeat carries no payload");
			return false;
		}
		break;

	case MessageType::VoxelUpdateRequest:
	{
		// The length field and the state bytes are written separately by the caller, and the send composes the
		// payload from the two, so a payload whose declared length disagrees with what follows it would be either
		// truncated or refused later with nothing to point at. Catch it here, where the caller can be told.
		const crowdy::Result<crowdy::wire::VoxelPayloadView> Voxel = crowdy::wire::parseVoxelPayload(PayloadBytes);
		if (!Voxel.ok())
		{
			OutError = TEXT("the voxel payload declares more state than it carries");
			return false;
		}
		if (crowdy::wire::voxel::kFixedSize + Voxel->state.size() != static_cast<std::size_t>(PayloadSize))
		{
			OutError = TEXT("the voxel payload carries more state than it declares");
			return false;
		}
		break;
	}

	case MessageType::ClientEventNotification:
		if (PayloadSize < static_cast<int32>(crowdy::wire::kEventTypeSize))
		{
			OutError = TEXT("an event payload starts with a two-octet event type");
			return false;
		}
		break;

	default:
		break;
	}

	FCrowdyCppQueuedSend Queued;
	Queued.Opcode = Opcode;
	Queued.ChunkX = ChunkX;
	Queued.ChunkY = ChunkY;
	Queued.ChunkZ = ChunkZ;
	FMemory::Memcpy(Queued.Uuid.data(), Uuid.GetData(), crowdy::wire::kUuidSize);
	Queued.Payload.Append(Payload.GetData(), PayloadSize);
	Queued.Distance = Distance;
	Queued.Decay = Decay;

	return Impl->EnqueueSend(MoveTemp(Queued), OutError);
}

bool FCrowdyCppReplication::SendChannelMessage(const int64 ChannelId, const TArrayView<const uint8> Uuid,
	const TArrayView<const uint8> Payload, FString& OutError)
{
	if (Impl->bClosed.load(std::memory_order_acquire) || !Impl->Connection)
	{
		OutError = TEXT("the connection is closed");
		return false;
	}

	if (Uuid.Num() != static_cast<int32>(crowdy::wire::kUuidSize))
	{
		OutError = FString::Printf(TEXT("the actor id is %d octets, not %d"),
			Uuid.Num(), static_cast<int32>(crowdy::wire::kUuidSize));
		return false;
	}

	const int32 PayloadSize = FMath::Max(Payload.Num(), 0);
	if (PayloadSize > static_cast<int32>(crowdy::wire::channel::kMaxPayload))
	{
		OutError = FString::Printf(TEXT("the payload is %d octets, over the %d a channel message carries"),
			PayloadSize, static_cast<int32>(crowdy::wire::channel::kMaxPayload));
		return false;
	}

	FCrowdyCppQueuedSend Queued;
	Queued.bIsChannel = true;
	Queued.ChannelId = ChannelId;
	FMemory::Memcpy(Queued.Uuid.data(), Uuid.GetData(), crowdy::wire::kUuidSize);
	Queued.Payload.Append(Payload.GetData(), PayloadSize);

	return Impl->EnqueueSend(MoveTemp(Queued), OutError);
}

bool FCrowdyCppReplication::SendVideoFrame(const int64 ChunkX, const int64 ChunkY, const int64 ChunkZ,
	const TArrayView<const uint8> Uuid, const TArrayView<const uint8> Frame, const int32 FrameId,
	const uint8 Codec, const uint8 Distance, const uint8 Decay, int32& OutFragmentsQueued, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Crowdy_SendVideoFrame);

	OutFragmentsQueued = 0;

	if (Impl->bClosed.load(std::memory_order_acquire) || !Impl->Connection)
	{
		OutError = TEXT("the connection is closed");
		return false;
	}

	if (Uuid.Num() != static_cast<int32>(crowdy::wire::kUuidSize))
	{
		OutError = FString::Printf(TEXT("the actor id is %d octets, not %d"),
			Uuid.Num(), static_cast<int32>(crowdy::wire::kUuidSize));
		return false;
	}

	if (Codec != static_cast<uint8>(crowdy::media::VideoCodec::Jpeg)
		&& Codec != static_cast<uint8>(crowdy::media::VideoCodec::WebP))
	{
		OutError = FString::Printf(TEXT("codec %u is not one the protocol assigns"), Codec);
		return false;
	}

	// Refused rather than wrapped, because the counter is the receiver's only way to tell one frame from
	// the next and a silently wrapped id would collide with a frame still being assembled.
	if (FrameId < 0 || FrameId > static_cast<int32>(MAX_uint16))
	{
		OutError = FString::Printf(TEXT("the frame id is %d, outside the 0 to %d the header carries"),
			FrameId, static_cast<int32>(MAX_uint16));
		return false;
	}

	TArray<TArray<uint8>> Fragments;
	if (!FCrowdyCppVideoAssembler::FragmentFrame(Frame, FrameId, Codec, Fragments))
	{
		OutError = FString::Printf(
			TEXT("a %d octet frame is either empty or needs more than the %d fragments a frame may cross as"),
			FMath::Max(Frame.Num(), 0), FCrowdyCppVideoAssembler::MaxFragments);
		return false;
	}

	TArray<FCrowdyCppQueuedSend> Batch;
	Batch.Reserve(Fragments.Num());
	for (TArray<uint8>& Fragment : Fragments)
	{
		FCrowdyCppQueuedSend Queued;
		Queued.Opcode = static_cast<uint8>(MessageType::ClientVideoPacket);
		Queued.ChunkX = ChunkX;
		Queued.ChunkY = ChunkY;
		Queued.ChunkZ = ChunkZ;
		FMemory::Memcpy(Queued.Uuid.data(), Uuid.GetData(), crowdy::wire::kUuidSize);
		Queued.Payload = MoveTemp(Fragment);
		Queued.Distance = Distance;
		Queued.Decay = Decay;
		Batch.Add(MoveTemp(Queued));
	}

	const int32 FragmentCount = Batch.Num();
	if (!Impl->EnqueueSends(MoveTemp(Batch), OutError))
	{
		return false;
	}

	OutFragmentsQueued = FragmentCount;
	return true;
}

FCrowdyCppReplicationStats FCrowdyCppReplication::GetStats() const
{
	FCrowdyCppReplicationStats Out;
	if (!Impl->Connection)
	{
		return Out;
	}

	Out.SendsDropped = Impl->SendsDropped.load(std::memory_order_relaxed);

	try
	{
		const crowdy::replication::Connection::Stats Stats = Impl->Connection->stats();
		Out.DatagramsSent = static_cast<int64>(Stats.datagramsSent);
		Out.DatagramsReceived = static_cast<int64>(Stats.datagramsReceived);
		Out.MessagesSent = static_cast<int64>(Stats.messagesSent);
		Out.MessagesReceived = static_cast<int64>(Stats.messagesReceived);
		Out.BytesSent = static_cast<int64>(Stats.bytesSent);
		Out.BytesReceived = static_cast<int64>(Stats.bytesReceived);
		Out.HmacFailures = static_cast<int64>(Stats.hmacFailures);
		Out.Malformed = static_cast<int64>(Stats.malformed);
		Out.RingDropped = static_cast<int64>(Stats.ringDropped);
		Out.SendsDeferred = static_cast<int64>(Stats.sendsDeferred);
		Out.SendsFailed = static_cast<int64>(Stats.sendsFailed);
		Out.BundlesSent = static_cast<int64>(Stats.bundlesSent);
		Out.MessagesDropped = static_cast<int64>(Stats.messagesDropped);
		Out.Reconnects = static_cast<int64>(Stats.reconnects);
		Out.LastServerEpochMs = Stats.lastServerEpochMs;
	}
	catch (...)
	{
		UE_LOG(LogCrowdyCpp, Error, TEXT("Replication: reading counters threw."));
	}

	return Out;
}

FCrowdyCppSessionAssignment FCrowdyCppReplication::GetAssignment() const
{
	FCrowdyCppSessionAssignment Out;
	if (!Impl->Connection || Impl->bClosed.load(std::memory_order_acquire))
	{
		return Out;
	}

	try
	{
		// The library keeps the last endpoint it was given even after the socket closes or a re-assignment fails,
		// so the state is what decides whether it is still in use.
		if (!IsLiveState(Impl->Connection->state()))
		{
			return Out;
		}

		const crowdy::replication::Assignment Assigned = Impl->Connection->assignmentSnapshot();
		Out.Ip4 = FString(UTF8_TO_TCHAR(Assigned.ip4.c_str()));
		Out.Ip6 = FString(UTF8_TO_TCHAR(Assigned.ip6.c_str()));
		Out.ClientPort = Assigned.clientPort;
		Out.bOk = Assigned.clientPort > 0 && (!Out.Ip4.IsEmpty() || !Out.Ip6.IsEmpty());
	}
	catch (...)
	{
		UE_LOG(LogCrowdyCpp, Error, TEXT("Replication: reading the assignment threw."));
	}

	return Out;
}
