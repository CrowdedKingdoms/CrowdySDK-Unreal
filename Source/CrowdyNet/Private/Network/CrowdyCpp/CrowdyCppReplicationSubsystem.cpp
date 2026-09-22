#include "Network/CrowdyCpp/CrowdyCppReplicationSubsystem.h"

#include "Async/Async.h"
#include "CrowdyCppClient.h"
#include "CrowdyCppReplication.h"
#include "CrowdyNetLog.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
#include "Internal/FCrowdyServiceRegistry.h"
#include "Network/CrowdyCpp/CrowdyCppClientSubsystem.h"
#include "Network/CrowdyCpp/CrowdyCppInboundFrame.h"
#include "Network/CrowdyCpp/CrowdyTokenAuthorization.h"
#include "Network/UDP/CrowdyCppSendAdapter.h"
#include "Network/UDP/CrowdyUDPSubsystem.h"
#include "Network/UDP/FCrowdyUDPEndpoint.h"
#include "Serialization/FCrowdyMessageParser.h"
#include "Subsystem/CrowdyGameSession.h"

#include <atomic>

namespace
{
	/**
	 * How many inbound messages one frame delivers. Delivery runs on the game thread, and the connection's network
	 * thread refills its queue while the drain is running, so an unbounded drain would not return while traffic
	 * lasted. Whatever is left over waits for the next frame.
	 *
	 * A console variable rather than a constant because it IS the client's own receive ceiling, and the population a
	 * game supports is read straight off it: at ten updates a second per player, a client can take at most this many
	 * multiplied by its frame rate, however those updates were sent and whatever the network could carry. The former
	 * value of 256 is about 1,500 players at sixty frames a second, which was ample when the design assumed two
	 * hundred relevant entities and is below the two thousand it now targets.
	 *
	 * Raising it costs nothing while there is no traffic to drain: it is a ceiling, not a reservation.
	 *
	 * It is sized to sit just above what the time budget below can afford, so that time stays the constraint that
	 * binds and this stays a cap on a flood of unusually cheap messages. That means the two move together: the
	 * value is the largest drain the window below actually completes, plus headroom. It was 1,600 against a
	 * per-message cost of roughly 2.6 us, and 1,024 while a message cost about 4 us. Re-measure before moving it
	 * again; a count raised past what the window can spend is not a higher ceiling, it just stops being the thing
	 * that reports the truncation.
	 *
	 * Derive it from an OBSERVED drain, not from an average cost. DescribeInboundDrainCost reports microseconds
	 * per message pooled across every drain, and a full drain amortises the fixed per-drain work better than a
	 * short one, so that average overstates the marginal cost: measured 2.02 us pooled against about 1.74 us
	 * inside a drain that ran the whole window. The reading a value comes from is the harness's own
	 * "largest single drain" once the message allowance is lifted clear, which at this cost settles near 2,300
	 * and peaks near 2,800 while a backlog clears. Hence 3,072.
	 */
	TAutoConsoleVariable<int32> CVarReceiveMaxMessagesPerPoll(
		TEXT("crowdy.net.receive.maxmessages"), 3072,
		TEXT("How many inbound replication messages one frame may deliver. This is the client's receive ceiling: at "
			"10 Hz per player it supports roughly this many multiplied by the frame rate, divided by ten, players. "
			"Whatever is left over waits for the next frame, and the connection's ring drops it if it waits too long."),
		ECVF_Default);

	/**
	 * Delivered in batches this size so the budget below can be a time budget as well as a count. The count alone is
	 * not enough: what one message costs depends on its payload, and a frame's worth of large ones is a visible hitch
	 * where the same number of small ones is nothing.
	 */
	constexpr int32 MessagesPerDrainSlice = 32;

	/**
	 * How long one frame may spend delivering. Whatever is left over waits for the next frame.
	 *
	 * This is the budget that protects the frame, and it is deliberately raised by less than the message count above,
	 * so that it stays the binding constraint under pathological load. The two failures need opposite answers: a
	 * message budget that binds is a count to raise, while a time budget that binds means delivery costs more per
	 * message than the frame can afford, and raising the count would only make that worse. Which one ended a drain is
	 * now counted separately for exactly that reason.
	 */
	TAutoConsoleVariable<float> CVarReceiveMaxDrainMilliseconds(
		TEXT("crowdy.net.receive.maxdrainms"), 4.0f,
		TEXT("How many milliseconds of one frame may be spent delivering inbound replication messages. Raise the "
			"message count first; if drains keep ending on this budget instead, delivery is costing more per message "
			"than the frame can afford and the count is not the lever."),
		ECVF_Default);

	/** Read when a connection opens; flipping it takes effect on the next connect, not on the live socket. */
	TAutoConsoleVariable<int32> CVarSendBundle(
		TEXT("crowdy.net.send.bundle"), 1,
		TEXT("Pack the outbound replication messages of one network pass into MESSAGE_BUNDLE datagrams. Needs a "
			"replication server of v0.27.0 or later; against an older one every bundled message is dropped together, "
			"and 0 sends one datagram per message."),
		ECVF_Default);

	/** Read when a connection opens, like crowdy.net.send.bundle. */
	TAutoConsoleVariable<int32> CVarAdvertiseCapabilities(
		TEXT("crowdy.net.recv.signedbundles"), 1,
		TEXT("Tell the replication server this client reads MESSAGE_BUNDLE_SIGNED, so downlink bundles arrive with one "
			"signature per datagram instead of one per member. A server older than v0.30.0 ignores it. 0 stops "
			"advertising and keeps the per-member form; a diagnostic switch, not a setting."),
		ECVF_Default);

	/** Inbound pressure is reported at most this often, since it arrives at packet rate when it arrives at all. */
	constexpr double InboundPressureReportIntervalSeconds = 5.0;

	/**
	 * How long a blocking session callback waits on an answer from the game thread before giving up. Generous,
	 * because it is waiting on a real server round trip, and bounded because the thread it blocks is the one that
	 * carries every send.
	 */
	constexpr double AssignmentTimeoutSeconds = 30.0;

	/**
	 * How long one read of the session may take. Deliberately short: it is a queued task reading three fields, and
	 * the thread waiting on it is the one that reads the socket and puts queued sends on the wire, so anything
	 * longer stops replication outright rather than merely delaying an answer.
	 */
	constexpr double SessionReadTimeoutSeconds = 2.0;

	/** How often a blocking wait wakes to re-check the abort predicate and its deadline. */
	constexpr float BlockingWaitSliceSeconds = 0.01f;

	/** Shortest gap between two requests for a token rotation, since each one is a real call against the auth API. */
	constexpr double RotationRequestFloorSeconds = 30.0;

	/** Shared between a blocking session callback and the game-thread work it queued, so either may finish first. */
	struct FAssignmentHandoff
	{
		std::atomic<bool> bDone{false};
		FCrowdyCppSessionAssignment Answer;
	};

	struct FTokenHandoff
	{
		std::atomic<bool> bDone{false};
		FString Token;
		int64 GameTokenId = 0;
		FString ExpiresAtIso8601;

		/** Where the Game API installed Token, or empty when it named nowhere. */
		FString AuthorizedServerIp4;
		int32 AuthorizedServerClientPort = 0;
	};

	/** Milliseconds since the epoch, or zero when the expiry is missing or unreadable. */
	int64 ParseExpiryEpochMs(const FString& Iso8601)
	{
		FDateTime Parsed;
		if (Iso8601.IsEmpty() || !FDateTime::ParseIso8601(*Iso8601, Parsed))
		{
			return 0;
		}

		return Parsed.ToUnixTimestamp() * 1000;
	}

	/**
	 * Wait for work queued on the game thread, from a thread that is not the game thread.
	 *
	 * Returns false when the caller was asked to abort or the deadline passed. The queued work may still be pending
	 * in that case, so everything it writes to has to outlive this call: callers own that state through a shared
	 * pointer the queued task captures by value.
	 */
	bool WaitForGameThreadWork(const std::atomic<bool>& Done, const FCrowdyCppShouldAbort& ShouldAbort,
		const double TimeoutSeconds)
	{
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		while (!Done.load(std::memory_order_acquire))
		{
			if (ShouldAbort && ShouldAbort())
			{
				return false;
			}

			if (FPlatformTime::Seconds() >= Deadline)
			{
				return false;
			}

			FPlatformProcess::Sleep(BlockingWaitSliceSeconds);
		}

		return true;
	}

	/** The session's current app-scoped token material. Game thread only, since it reads engine objects. */
	void ReadTokenFromSession(const UGameInstance& Instance, FTokenHandoff& Out)
	{
		if (const UCrowdyGameSession* Session = Instance.GetSubsystem<UCrowdyGameSession>())
		{
			Out.Token = Session->GetGameToken();
			Out.GameTokenId = Session->GetGameTokenID();
			Out.ExpiresAtIso8601 = Session->GetAppTokenExpiresAt();
			Out.AuthorizedServerIp4 = Session->GetAppTokenAuthorizedServerIp4();
			Out.AuthorizedServerClientPort = Session->GetAppTokenAuthorizedServerClientPort();
		}
	}

	/**
	 * Assign a replication server, blocking on the connection's own network thread.
	 *
	 * The query itself runs on the game thread, because the API client and its completion pump belong there, and
	 * this waits for it. That is the shape the callback contract asks for, and it is only safe because the wait
	 * honours the abort predicate: teardown raises it before joining this thread, so a call in flight when the game
	 * shuts down gives up instead of holding the join open for a full round trip.
	 */
	FCrowdyCppAssignServer MakeAssignServerCallback(const TWeakObjectPtr<UGameInstance>& WeakInstance,
		const FCrowdyCppClientConfig& ClientConfig, TFunction<void()> ReportAppFull)
	{
		return [WeakInstance, ClientConfig, ReportAppFull](const FCrowdyCppShouldAbort& ShouldAbort)
		{
			const TSharedRef<FAssignmentHandoff> Handoff = MakeShared<FAssignmentHandoff>();

			AsyncTask(ENamedThreads::GameThread, [WeakInstance, ClientConfig, ReportAppFull, Handoff]()
			{
				const auto Finish = [Handoff](FCrowdyCppSessionAssignment&& Answer)
				{
					Handoff->Answer = MoveTemp(Answer);
					Handoff->bDone.store(true, std::memory_order_release);
				};

				UGameInstance* Instance = WeakInstance.Get();
				UCrowdyCppClientSubsystem* ClientHost = Instance
					? Instance->GetSubsystem<UCrowdyCppClientSubsystem>()
					: nullptr;
				if (!ClientHost)
				{
					FCrowdyCppSessionAssignment Answer;
					Answer.ErrorMessage = TEXT("there is no API client host on this game instance");
					Finish(MoveTemp(Answer));
					return;
				}

				// Taken from the live session on every attempt rather than trusting whatever the shared client still
				// holds, so an assignment issued after a sign-out fails instead of answering as the account that left.
				if (const UCrowdyGameSession* Session = Instance->GetSubsystem<UCrowdyGameSession>())
				{
					ClientHost->SetGameToken(Session->GetGameToken());
				}

				FCrowdyCppClient* Client = ClientHost->GetClient(ClientConfig);
				if (!Client)
				{
					FCrowdyCppSessionAssignment Answer;
					Answer.ErrorMessage = TEXT("the API client could not be constructed");
					Finish(MoveTemp(Answer));
					return;
				}

				Client->RunOp(ECrowdyCppApiDomain::ServerStatus, TEXT("ServerWithLeastClients"),
					MakeShared<FJsonObject>(), [Finish, ReportAppFull, WeakInstance](FCrowdyCppJsonResult Result)
					{
						FCrowdyCppSessionAssignment Answer;

						if (!Result.bTransportOk)
						{
							// A full app comes back as an error and is not one: the server was reached and simply
							// has no room, so it is worth telling the player rather than retrying into it.
							if (Result.ErrorMessage.Contains(TEXT("Insufficient")) && ReportAppFull)
							{
								ReportAppFull();
							}

							Answer.ErrorMessage = Result.ErrorMessage;
							Finish(MoveTemp(Answer));
							return;
						}

						FCrowdyUDPEndpoint Endpoint;
						FString Error;
						if (!CrowdyUDPEndpoint::ParseServerAssignment(Result.Data, Endpoint, Error))
						{
							Answer.ErrorMessage = Error;
							Finish(MoveTemp(Answer));
							return;
						}

						Answer.bOk = true;
						Answer.Ip4 = Endpoint.IPv4Address;
						Answer.Ip6 = Endpoint.IPv6Address;
						Answer.ClientPort = Endpoint.Port;

						// Recorded so the auth plane can name this server when it rotates the app token, and so a
						// rotation's answer can be compared against it. Written on every assignment rather than
						// only the first, since a re-assignment moves the client and a stale address here would
						// have a later refresh authorize the new token on a server nobody is on.
						//
						// The IPv4 address even when the connection dials IPv6: the Game API names a node by its
						// ip4, so the other family's address would name nothing it can match.
						if (UGameInstance* Live = WeakInstance.Get())
						{
							if (UCrowdyGameSession* Session = Live->GetSubsystem<UCrowdyGameSession>())
							{
								Session->SetReplicationServer(Endpoint.IPv4Address, Endpoint.Port);
							}
						}

						Finish(MoveTemp(Answer));
					});
			});

			if (!WaitForGameThreadWork(Handoff->bDone, ShouldAbort, AssignmentTimeoutSeconds))
			{
				FCrowdyCppSessionAssignment GaveUp;
				GaveUp.ErrorMessage = TEXT("the server assignment was abandoned before it answered");
				return GaveUp;
			}

			return Handoff->Answer;
		};
	}

	/** What a rotation callback remembers between calls. Read and written only on the connection's network thread. */
	struct FRotationState
	{
		FString LastOffered;
		double LastRequestSeconds = 0.0;
	};

	/**
	 * Rotate the app-scoped token, on the connection's own network thread.
	 *
	 * Rotation belongs to the game's authentication plane, which this module cannot see and which answers
	 * asynchronously. This does NOT wait for that answer, and that is the important part: the calling thread is the
	 * only one that reads the socket and puts queued sends on the wire, so holding it for the length of a rotation
	 * would stop replication outright rather than merely delay it.
	 *
	 * So it asks for a rotation, answers with what the session holds at that instant, and returns. That is fresh
	 * material only when a rotation has already landed; otherwise it reports that one is on the way. Material that
	 * lands afterwards reaches the connection on its own, because the game installs it directly when it arrives.
	 * Requests are floored, since each one is a real call against the auth API and this can be asked repeatedly.
	 */
	FCrowdyCppRefreshToken MakeRefreshTokenCallback(const TWeakObjectPtr<UGameInstance>& WeakInstance,
		TFunction<void()> RequestRefresh, const FString& OpenedWith)
	{
		const TSharedRef<FRotationState> State = MakeShared<FRotationState>();
		State->LastOffered = OpenedWith;

		return [WeakInstance, RequestRefresh, State](const FCrowdyCppShouldAbort& ShouldAbort,
			const FCrowdyCppCurrentServer* Current)
		{
			FCrowdyCppReplicationToken Answer;

			const double Now = FPlatformTime::Seconds();
			const bool bAskForOne = State->LastRequestSeconds <= 0.0
				|| Now - State->LastRequestSeconds >= RotationRequestFloorSeconds;

			const TSharedRef<FTokenHandoff> Read = MakeShared<FTokenHandoff>();
			AsyncTask(ENamedThreads::GameThread, [WeakInstance, RequestRefresh, bAskForOne, Read]()
			{
				if (const UGameInstance* Instance = WeakInstance.Get())
				{
					ReadTokenFromSession(*Instance, *Read);

					if (bAskForOne && RequestRefresh)
					{
						RequestRefresh();
					}
				}

				Read->bDone.store(true, std::memory_order_release);
			});

			if (bAskForOne)
			{
				State->LastRequestSeconds = Now;
			}

			if (!WaitForGameThreadWork(Read->bDone, ShouldAbort, SessionReadTimeoutSeconds))
			{
				Answer.ErrorMessage = TEXT("the session could not be read for a token rotation");
				return Answer;
			}

			// Offering back what the connection is already signing with would be reported as a successful rotation
			// and would install nothing, so an unchanged token is an answer of "not yet" rather than an answer.
			if (Read->Token.IsEmpty() || Read->Token == State->LastOffered)
			{
				Answer.ErrorMessage = TEXT("a token rotation has been asked for and has not landed yet");
				return Answer;
			}

			State->LastOffered = Read->Token;

			Answer.bOk = true;
			Answer.Token = Read->Token;
			Answer.GameTokenId = Read->GameTokenId;
			Answer.ExpiresAtEpochMs = ParseExpiryEpochMs(Read->ExpiresAtIso8601);

			Answer.bAuthorizedOnCurrentServer = CrowdyTokenAuthorization::KeepsCurrentServer(
				Read->AuthorizedServerIp4, Read->AuthorizedServerClientPort, Current);

			UE_CLOG(!Answer.bAuthorizedOnCurrentServer && Current != nullptr, LogCrowdyNet, Verbose,
				TEXT("A rotated app token is not authorized on the current replication server, so the connection "
					"will re-assign."));

			return Answer;
		};
	}

	/** How often the connection's cumulative counters are turned into a rate and published. */
	constexpr double NetworkStatsReportIntervalSeconds = 1.0;

	/**
	 * A cumulative counter only grows, except across a reconnect, when it restarts at zero. Read as a negative
	 * rate that would be, so it is reported as the whole current reading instead.
	 */
	int64 DeltaSinceLastReading(const int64 CurrentValue, const int64 PreviousValue)
	{
		return CurrentValue >= PreviousValue ? CurrentValue - PreviousValue : CurrentValue;
	}

	/**
	 * Bounded above so a connection that has carried more than 2^31 octets or messages since it opened cannot
	 * wrap a 32-bit stat field negative on the cast down. Deliberately not bounded below: a delta is expected to
	 * be non-negative by construction, and clamping a negative one to zero here would hide a defect in that
	 * construction instead of surfacing it.
	 */
	int32 ClampToStatField(const int64 Value)
	{
		return static_cast<int32>(FMath::Min(Value, static_cast<int64>(TNumericLimits<int32>::Max())));
	}

	const TCHAR* DescribeConnectionState(const ECrowdyCppConnState State)
	{
		switch (State)
		{
		case ECrowdyCppConnState::Idle: return TEXT("idle");
		case ECrowdyCppConnState::Connecting: return TEXT("connecting");
		case ECrowdyCppConnState::Connected: return TEXT("connected");
		case ECrowdyCppConnState::Reconnecting: return TEXT("reconnecting");
		case ECrowdyCppConnState::Failed: return TEXT("failed");
		case ECrowdyCppConnState::Closed: return TEXT("closed");
		}
		return TEXT("unknown");
	}
}

void UCrowdyCppReplicationSubsystem::Deinitialize()
{
	// Before the receive target goes, since its own ticker delivers into the parser this is about to
	// detach.
	DisarmLocalDelivery();

	SetConnection(nullptr);
	SetReceiveTarget(nullptr, nullptr, nullptr);

	// Only reachable if a drain was interrupted, which teardown cannot be, but an undisposed connection here would
	// leave a network thread running past the game instance.
	for (const TSharedPtr<FCrowdyCppReplication>& Held : DeferredDisposal)
	{
		if (Held.IsValid())
		{
			Held->Disconnect();
		}
	}
	DeferredDisposal.Reset();

	Super::Deinitialize();
}

UCrowdyCppReplicationSubsystem* UCrowdyCppReplicationSubsystem::Get(const UObject* WorldContextObject)
{
	const UWorld* World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	return GameInstance ? GameInstance->GetSubsystem<UCrowdyCppReplicationSubsystem>() : nullptr;
}

void UCrowdyCppReplicationSubsystem::SetConnection(TSharedPtr<FCrowdyCppReplication> InConnection)
{
	// Re-installing what is already installed is a no-op rather than a replacement. Disposal is terminal, so the
	// alternative is a closed connection still sitting in place, reporting that it is routing while refusing
	// everything.
	{
		FScopeLock Lock(&ConnectionMutex);
		if (Connection == InConnection)
		{
			return;
		}
	}

	// The handlers go on before the connection is published, so an installed connection is never briefly pollable
	// with nothing to deliver to.
	InstallReceiveHandlers(InConnection);

	TSharedPtr<FCrowdyCppReplication> Retiring;
	bool bHaveConnection = false;

	{
		FScopeLock Lock(&ConnectionMutex);
		Retiring = MoveTemp(Connection);
		Connection = MoveTemp(InConnection);
		bHaveConnection = Connection.IsValid();
		bWarnedAboutMissingConnection = false;
	}

	// An unpolled connection delivers nothing and lets its queue fill, so the poll is tied to installation rather
	// than left to the caller.
	if (bHaveConnection && !PollTickerHandle.IsValid())
	{
		PollTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateUObject(this, &UCrowdyCppReplicationSubsystem::TickPollConnection));
	}
	else if (!bHaveConnection && PollTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(PollTickerHandle);
		PollTickerHandle.Reset();
	}

	// The rates are published by the poll, so without this the throughput rows would sit at the last connected
	// second's numbers for as long as the overlay is open, which reads as a live connection.
	if (!bHaveConnection)
	{
		LastStatsReading = FCounterSnapshot();
		LastStatsReportSeconds = 0.0;
		PendingClientNotifiesSent.store(0, std::memory_order_relaxed);

		if (UCrowdyUDPSubsystem* Stats = ReceiveStats.Get())
		{
			Stats->ReportTransportSample(FCrowdyTransportSample());
		}
	}

	if (!Retiring.IsValid())
	{
		return;
	}

	// Held over when this call came from inside delivery. Disposing there would destroy the handler that is running,
	// discard the rest of the batch, and block the game thread joining the network thread from inside the frame.
	if (bDraining)
	{
		DeferredDisposal.Add(MoveTemp(Retiring));
		return;
	}

	// Outside the lock on purpose. Disconnect blocks until the network thread has stopped, and a session callback on
	// that thread is allowed to ask this subsystem for the connection, which would deadlock against a held lock.
	Retiring->Disconnect();
}

bool UCrowdyCppReplicationSubsystem::OpenConnection(const FCrowdyCppConnectionRequest& Request)
{
	UGameInstance* Instance = GetGameInstance();
	if (!Instance)
	{
		UE_LOG(LogCrowdyNet, Warning, TEXT("Cannot open a replication connection without a game instance."));
		return false;
	}

	// The flag describes this attempt, not any earlier one. Left set from a previous attempt it would report the next
	// failure of any kind as the app being full, which stops the reconnect ladder and leaves the player with a wrong
	// reason and no way back.
	bLastAssignmentReportedAppFull = false;

	// Reconnecting in place is not an optimization. Replacing a connection discards its queued sends and its session,
	// and the credentials are what a connection is built around, so anything else is a reconnect.
	const TSharedPtr<FCrowdyCppReplication> Installed = GetConnection();
	if (Installed.IsValid() && Installed->GetState() != ECrowdyCppConnState::Closed
		&& OpenedWithToken == Request.Token && OpenedWithAppId == Request.AppId)
	{
		Installed->ConnectAsync();
		return true;
	}

	FCrowdyCppReplicationConfig Config;
	Config.AppId = Request.AppId;
	Config.Token.bOk = true;
	Config.Token.Token = Request.Token;
	Config.Token.GameTokenId = Request.GameTokenId;
	Config.Token.ExpiresAtEpochMs = ParseExpiryEpochMs(Request.TokenExpiresAtIso8601);
	Config.bPreferIpv6 = Request.bPreferIpv6;

	// The game rotates its own app token on its own schedule and installs the result here, so the connection is told
	// not to chase the expiry itself. Two things watching the same clock would have the connection asking for a
	// rotation the game has not decided to make yet, repeatedly, from the thread that carries the traffic.
	Config.RefreshLeadMs = 0;

	// The connection watches its own silence. It answers by re-assigning a server and carrying on, so the game only
	// hears about it if the re-assignment itself fails.
	Config.WatchdogSilenceMs = Request.SilenceTimeoutSeconds > 0.f
		? static_cast<int64>(Request.SilenceTimeoutSeconds * 1000.f)
		: 0;

	Config.bBundleSends = CVarSendBundle.GetValueOnGameThread() != 0;
	Config.bAdvertiseCapabilities = CVarAdvertiseCapabilities.GetValueOnGameThread() != 0;

	// The game is told the app is full, and so is this subsystem, because the two act on it differently: the game
	// surfaces it to the player, and this has to stop treating the failure that follows as something to recover from.
	const TWeakObjectPtr<UCrowdyCppReplicationSubsystem> WeakSelf(this);
	TFunction<void()> TellTheGame = Request.ReportAppFull;
	TFunction<void()> OnAppFull = [WeakSelf, TellTheGame]()
	{
		if (UCrowdyCppReplicationSubsystem* Self = WeakSelf.Get())
		{
			Self->bLastAssignmentReportedAppFull = true;
		}

		if (TellTheGame)
		{
			TellTheGame();
		}
	};

	const TWeakObjectPtr<UGameInstance> WeakInstance(Instance);

	// Assignment is fetched through the API client, so it has to be asked of the datacenter the app is served from.
	// Before one has been resolved the shared origin stands in: it is answered everywhere, and a WRONG_DATACENTER
	// from it moves the client to the right place rather than failing the connection.
	FCrowdyCppClientConfig ClientConfig;
	ClientConfig.DiscoveryUrl = Request.DiscoveryUrl;
	ClientConfig.ApiUrl = Request.GameApiUrl.IsEmpty() ? Request.DiscoveryUrl : Request.GameApiUrl;

	TSharedPtr<FCrowdyCppReplication> Built = FCrowdyCppReplication::Make(Config,
		MakeAssignServerCallback(WeakInstance, ClientConfig, MoveTemp(OnAppFull)),
		MakeRefreshTokenCallback(WeakInstance, Request.RequestTokenRefresh, Request.Token));

	if (!Built.IsValid())
	{
		UE_LOG(LogCrowdyNet, Warning,
			TEXT("A replication connection could not be built, so nothing can be sent or received."));
		return false;
	}

	OpenedWithToken = Request.Token;
	OpenedWithAppId = Request.AppId;

	// Installed before it is opened, so the first state change has handlers to reach.
	SetConnection(Built);
	Built->ConnectAsync();
	return true;
}

void UCrowdyCppReplicationSubsystem::CloseConnection()
{
	// The credentials go too. Leaving them behind would let a later request for the same token be mistaken for a
	// reconnect of a connection that no longer exists.
	OpenedWithToken.Reset();
	OpenedWithAppId = 0;
	bLastAssignmentReportedAppFull = false;

	SetConnection(nullptr);
}

bool UCrowdyCppReplicationSubsystem::InstallToken(const FString& Token, const int64 GameTokenId,
	const FString& ExpiresAtIso8601)
{
	const TSharedPtr<FCrowdyCppReplication> Installed = GetConnection();
	if (!Installed.IsValid())
	{
		return false;
	}

	FCrowdyCppReplicationToken Material;
	Material.bOk = true;
	Material.Token = Token;
	Material.GameTokenId = GameTokenId;
	Material.ExpiresAtEpochMs = ParseExpiryEpochMs(ExpiresAtIso8601);

	Installed->SetToken(Material);

	// Kept in step with what the connection is signing with, so a later request carrying this same token is
	// recognised as a reconnect rather than rebuilding a connection that is already using it.
	OpenedWithToken = Token;
	return true;
}

void UCrowdyCppReplicationSubsystem::ApplyConnectionState(const ECrowdyCppConnState State)
{
	UCrowdyUDPSubsystem* Udp = ReceiveStats.Get();
	if (!Udp)
	{
		return;
	}

	switch (State)
	{
	case ECrowdyCppConnState::Connecting:
		Udp->SetConnectionState(EUDPConnectionState::Connecting);
		break;

	case ECrowdyCppConnState::Connected:
		// The connection is usable, so everything waiting on it can proceed, the reliable-RPC channel joins included.
		bLastAssignmentReportedAppFull = false;
		Udp->MarkRoutedConnectionUp();
		break;

	case ECrowdyCppConnState::Reconnecting:
		// Reported, not acted on: the connection re-assigns a server on its own and only a failed re-assignment is
		// the game's problem.
		Udp->SetConnectionState(EUDPConnectionState::Reconnecting);
		break;

	case ECrowdyCppConnState::Failed:
		// A full app is an answer rather than a failure, and the player has already been told. Recovering from it
		// would mean asking a server that has no room whether it has room, every few seconds, for as long as the
		// game is running, so the state says so and stops.
		if (bLastAssignmentReportedAppFull)
		{
			Udp->SetConnectionState(EUDPConnectionState::GateKeep);
			break;
		}

		// Anything else is worth recovering from, and nothing retries a failed connection on its own, so this is
		// where the game has to hear about it. This is the only producer of the timeout signal, and it is what
		// drives the reconnect ladder.
		Udp->SetConnectionState(EUDPConnectionState::Disconnected);
		Udp->OnUDPTimeout.Broadcast();
		break;

	case ECrowdyCppConnState::Closed:
		Udp->SetConnectionState(EUDPConnectionState::Disconnected);
		break;

	case ECrowdyCppConnState::Idle:
	default:
		break;
	}
}

void UCrowdyCppReplicationSubsystem::SetReceiveTarget(FCrowdyMessageParser* InParser,
	FCrowdyServiceRegistry* InRegistry, UCrowdyUDPSubsystem* InUdpSubsystem)
{
	ReceiveParser = InParser;
	ReceiveRegistry = InRegistry;
	ReceiveStats = InUdpSubsystem;
	bWarnedAboutMissingReceiveTarget = false;
}

void UCrowdyCppReplicationSubsystem::InstallReceiveHandlers(const TSharedPtr<FCrowdyCppReplication>& Target)
{
	if (!Target.IsValid())
	{
		return;
	}

	// Weakly captured: the handlers only run from a poll this subsystem drives, and a retiring connection is
	// disconnected before it is released, so a handler cannot outlive the subsystem as things stand. The weak
	// pointer is what keeps that true for anything that later polls a connection from somewhere else.
	const TWeakObjectPtr<UCrowdyCppReplicationSubsystem> WeakThis(this);

	FCrowdyCppReplicationHandlers Handlers;

	Handlers.OnSpatial = [WeakThis](const FCrowdyCppSpatialMessage& Message)
	{
		UCrowdyCppReplicationSubsystem* Self = WeakThis.Get();
		if (!Self)
		{
			return;
		}

		Self->DeliverFrame(CrowdyCppInboundFrame::FromSpatial(Message));
	};

	Handlers.OnChannel = [WeakThis](const FCrowdyCppChannelMessage& Message)
	{
		UCrowdyCppReplicationSubsystem* Self = WeakThis.Get();
		if (!Self)
		{
			return;
		}

		Self->DeliverFrame(CrowdyCppInboundFrame::FromChannel(Message));
	};

	// The error frame is delivered rather than acted on here, because the parser is where the expired-token recovery
	// hangs off it. What is reported here instead is what the frame is about: the parser sees two octets, while the
	// transport is the only thing that knows which send used that sequence.
	Handlers.OnError = [WeakThis](const FCrowdyCppSendError& Error)
	{
		UCrowdyCppReplicationSubsystem* Self = WeakThis.Get();
		if (!Self)
		{
			return;
		}

		Self->ReportSendError(Error);

		// Held here rather than inside the builder, because the frame borrows these two octets.
		const uint8 Body[] = { Error.Sequence, Error.ErrorCode };
		Self->DeliverFrame(CrowdyCppInboundFrame::FromError(Body));
	};

	// A failure is the one state worth saying unconditionally, because nothing retries it on its own.
	Handlers.OnStateChanged = [WeakThis](const ECrowdyCppConnState State)
	{
		UE_CLOG(State == ECrowdyCppConnState::Failed, LogCrowdyNet, Warning,
			TEXT("The replication connection failed and will not retry on its own."));

		UE_CLOG(State != ECrowdyCppConnState::Failed && CrowdyNetTrace::Net(), LogCrowdyNet, Log,
			TEXT("The replication connection is now %s."), DescribeConnectionState(State));

		if (UCrowdyCppReplicationSubsystem* Self = WeakThis.Get())
		{
			Self->ApplyConnectionState(State);
		}
	};

	Target->SetHandlers(MoveTemp(Handlers));
}

bool UCrowdyCppReplicationSubsystem::TickPollConnection(float DeltaTime)
{
	// Pinned across the drain, because a reception layer reached from a delivered message is free to replace the
	// connection. The pin keeps this one alive to the end of its own drain; holding its disposal over until then is
	// what makes that safe, and that is SetConnection's job.
	const TSharedPtr<FCrowdyCppReplication> Pinned = GetConnection();
	if (!Pinned.IsValid())
	{
		return true;
	}

	// Whatever this frame's budget has left, not a fresh copy of it. Local delivery drains on its own ticker in the
	// same frame, and both would otherwise spend the full allowance, letting one frame deliver twice what the client
	// being measured can. Checked before bDraining is raised, so an exhausted budget cannot leave the flag set.
	BeginFrameDrainBudget();
	const int32 MaxMessages = RemainingDrainMessages();
	const double MaxDrainSeconds = RemainingDrainSeconds();
	if (MaxMessages <= 0 || MaxDrainSeconds <= 0.0)
	{
		return true;
	}

	bDraining = true;

	const double Started = FPlatformTime::Seconds();
	int32 Delivered = 0;
	bool bTimeBudgetExpired = false;

	// A slice that came back full is the only evidence available here that the connection still had messages to
	// give. Asking it again would be a second poll with its own side effects, so the last slice's fullness is what
	// both truncation counters below are conditioned on.
	bool bLastSliceWasFull = false;

	{
		// The whole drain, not the delivery inside it. What is left after subtracting Crowdy_DeliverFrame is the
		// connection's own dequeue, which is real drain cost and had no scope of its own.
		TRACE_CPUPROFILER_EVENT_SCOPE(Crowdy_ReceiveDrain);

		while (Delivered < MaxMessages)
		{
			const int32 Requested = FMath::Min(MessagesPerDrainSlice, MaxMessages - Delivered);
			const int32 InSlice = Pinned->Poll(Requested);
			if (InSlice <= 0)
			{
				bLastSliceWasFull = false;
				break;
			}

			bLastSliceWasFull = InSlice == Requested;
			Delivered += InSlice;

			if (FPlatformTime::Seconds() - Started >= MaxDrainSeconds)
			{
				// Only when the message allowance was not also spent on this iteration. The two counters want opposite
				// fixes, and under a crowd a time-bound drain is usually message-bound too, so a tie broken toward the
				// message budget would keep pointing at the lever that cannot help.
				bTimeBudgetExpired = bLastSliceWasFull && Delivered < MaxMessages;
				break;
			}
		}
	}

	ConsumeDrainBudget(Delivered, FPlatformTime::Seconds() - Started);

	// Which budget ended the drain, not merely that one did. Until now only the time budget was counted, so a drain
	// that spent its entire message allowance left the rest for the next frame with nothing saying so, which is the
	// case a crowd actually produces. The two need opposite answers, hence two counters.
	if (bTimeBudgetExpired)
	{
		++TimeBudgetTruncatedDrains;
	}
	else if (Delivered >= MaxMessages && bLastSliceWasFull)
	{
		++MessageBudgetTruncatedDrains;
	}

	bDraining = false;

	// Disposed here rather than where it was asked for, so nothing is torn down under its own drain.
	if (DeferredDisposal.Num() > 0)
	{
		TArray<TSharedPtr<FCrowdyCppReplication>> Retiring = MoveTemp(DeferredDisposal);
		DeferredDisposal.Reset();
		for (const TSharedPtr<FCrowdyCppReplication>& Held : Retiring)
		{
			if (Held.IsValid())
			{
				Held->Disconnect();
			}
		}
	}

	ReportInboundPressure(*Pinned);
	ReportNetworkStats(*Pinned);
	return true;
}

void UCrowdyCppReplicationSubsystem::ReportInboundPressure(const FCrowdyCppReplication& Polled)
{
	// Two readings of the same problem, said together because one leads to the other: drains the frame budget ended
	// while the connection still had messages, and messages the connection then had to throw away because nothing
	// came back for them in time. Nothing else surfaces either, and silent inbound loss under load looks like a game
	// bug rather than a budget.
	const int64 Dropped = Polled.GetStats().RingDropped;

	// A reconnect restarts the connection's counters, so a lower reading is a new connection rather than negative
	// loss.
	if (Dropped < LastReportedRingDropped)
	{
		LastReportedRingDropped = 0;
	}

	const int64 NewDrops = Dropped - LastReportedRingDropped;
	if (NewDrops <= 0 && MessageBudgetTruncatedDrains == 0 && TimeBudgetTruncatedDrains == 0)
	{
		return;
	}

	const double Now = FPlatformTime::Seconds();
	if (LastInboundPressureReportSeconds > 0.0
		&& Now - LastInboundPressureReportSeconds < InboundPressureReportIntervalSeconds)
	{
		return;
	}

	// Named by budget, because the two have opposite answers and a single "the frame budget" reading sent readers to
	// the wrong lever. Spending the whole message allowance means raising crowdy.net.receive.maxmessages; running out
	// of time means delivery costs more per message than the frame can afford, and raising the count makes it worse.
	UE_LOG(LogCrowdyNet, Warning,
		TEXT("Inbound replication is behind: %lld drains spent their whole message allowance (crowdy.net.receive."
			"maxmessages, currently %d), %lld ran out of time (crowdy.net.receive.maxdrainms, currently %.1f), and "
			"%lld messages were dropped that could not be delivered fast enough (%lld in total)."),
		MessageBudgetTruncatedDrains, CVarReceiveMaxMessagesPerPoll.GetValueOnGameThread(),
		TimeBudgetTruncatedDrains, CVarReceiveMaxDrainMilliseconds.GetValueOnGameThread(),
		NewDrops, Dropped);

	// Said alongside, because the line above names two dials without saying which one has room. Which budget ended a
	// drain says which dial to reach for; only this says how far it can move.
	UE_LOG(LogCrowdyNet, Warning, TEXT("Inbound drain cost: %s"), *DescribeInboundDrainCost());

	LastReportedRingDropped = Dropped;
	MessageBudgetTruncatedDrains = 0;
	TimeBudgetTruncatedDrains = 0;
	ResetInboundDrainCost();
	LastInboundPressureReportSeconds = Now;
}

void UCrowdyCppReplicationSubsystem::ReportSendError(const FCrowdyCppSendError& Error)
{
	const double Now = FPlatformTime::Seconds();
	if (SendErrorWindowStartedSeconds <= 0.0 || Now - SendErrorWindowStartedSeconds >= 1.0)
	{
		if (SendErrorsSuppressedThisWindow > 0)
		{
			UE_LOG(LogCrowdyNet, Warning, TEXT("%d further server error frames were not reported."),
				SendErrorsSuppressedThisWindow);
		}
		SendErrorWindowStartedSeconds = Now;
		SendErrorsReportedThisWindow = 0;
		SendErrorsSuppressedThisWindow = 0;
	}

	constexpr int32 MaxSendErrorsPerSecond = 4;
	if (SendErrorsReportedThisWindow >= MaxSendErrorsPerSecond)
	{
		++SendErrorsSuppressedThisWindow;
		return;
	}
	++SendErrorsReportedThisWindow;

	const FString CodeName = FCrowdyCppReplication::DescribeErrorCode(Error.ErrorCode);
	if (!Error.bAttributed)
	{
		// No send on record for this sequence. Either the counter lapped, or the connection is new, or nothing we
		// sent provoked it, which is worth saying rather than leaving the reader to assume the first cause.
		UE_LOG(LogCrowdyNet, Warning,
			TEXT("The server rejected something: code %d (%s), sequence %d. No recent send is on record for that "
				"sequence, so it cannot be traced to a call."),
			Error.ErrorCode, *CodeName, Error.Sequence);
		return;
	}

	const FString OpcodeName = Error.bSendWasChannel
		? FString(TEXT("a channel message"))
		: FString::Printf(TEXT("opcode %d (%s)"), Error.SendOpcode,
			*StaticEnum<ECrowdyMessageType>()->GetNameStringByValue(static_cast<int64>(Error.SendOpcode)));

	// The id is 32 ASCII octets rather than text, so it is built from the bytes rather than cast.
	FString ActorId;
	for (const uint8 Octet : Error.SendUuid)
	{
		ActorId.AppendChar(static_cast<TCHAR>(Octet));
	}

	// A refused capability is the one rejection with nothing else to see: the send was accepted locally, the
	// datagram went out, and the only sign anything is wrong is this frame. Saying which capability the
	// opcode needs is what turns it into something to act on rather than a code to look up.
	FString Remedy;
	if (FCrowdyCppReplication::IsUnauthorizedErrorCode(Error.ErrorCode) && !Error.bSendWasChannel
		&& Error.SendOpcode == static_cast<uint8>(ECrowdyMessageType::CLIENT_VIDEO_PACKET))
	{
		Remedy = TEXT(" Video is a per-app capability the server grants, so this app does not carry it and no"
			" video frame will reach anyone until it does.");
	}

	UE_LOG(LogCrowdyNet, Warning,
		TEXT("The server rejected a send: code %d (%s), sequence %d, sent %lldms ago as %s for actor %s.%s"),
		Error.ErrorCode, *CodeName, Error.Sequence, Error.SendAgeMs, *OpcodeName, *ActorId, *Remedy);
}

void UCrowdyCppReplicationSubsystem::ReportNetworkStats(const FCrowdyCppReplication& Polled)
{
	const double Now = FPlatformTime::Seconds();
	if (LastStatsReportSeconds > 0.0 && Now - LastStatsReportSeconds < NetworkStatsReportIntervalSeconds)
	{
		return;
	}
	LastStatsReportSeconds = Now;

	const FCrowdyCppReplicationStats RawStats = Polled.GetStats();

	FCounterSnapshot Current;
	Current.BytesSent = RawStats.BytesSent;
	Current.BytesReceived = RawStats.BytesReceived;
	Current.DatagramsSent = RawStats.DatagramsSent;
	Current.DatagramsReceived = RawStats.DatagramsReceived;
	Current.MessagesSent = RawStats.MessagesSent;

	FCrowdyTransportSample Sample;
	Sample.BytesSent = ClampToStatField(DeltaSinceLastReading(Current.BytesSent, LastStatsReading.BytesSent));
	Sample.BytesReceived =
		ClampToStatField(DeltaSinceLastReading(Current.BytesReceived, LastStatsReading.BytesReceived));
	Sample.DatagramsSent =
		ClampToStatField(DeltaSinceLastReading(Current.DatagramsSent, LastStatsReading.DatagramsSent));
	Sample.DatagramsReceived =
		ClampToStatField(DeltaSinceLastReading(Current.DatagramsReceived, LastStatsReading.DatagramsReceived));
	Sample.MessagesSent =
		ClampToStatField(DeltaSinceLastReading(Current.MessagesSent, LastStatsReading.MessagesSent));

	// Stored even when there is nowhere to report the sample to, so a stats target attached later starts from an
	// accurate baseline instead of one that is a whole connection's lifetime stale.
	LastStatsReading = Current;

	// Taken whether or not there is anywhere to publish it, so the count cannot build up unboundedly while no stats
	// target is attached and then arrive as one spike.
	const int64 NotifiesSent = PendingClientNotifiesSent.exchange(0, std::memory_order_relaxed);

	if (UCrowdyUDPSubsystem* Stats = ReceiveStats.Get())
	{
		Stats->ReportTransportSample(Sample);
		Stats->AddTotalClientNotifiesSent(NotifiesSent);
	}
}

void UCrowdyCppReplicationSubsystem::DeliverFrame(const FCrowdyFrame& Frame)
{
	// The whole per-message delivery cost, which is the term the receive drain's TIME budget is spent on
	// and therefore one of the two things that bound the population ceiling. Traced because the budget
	// split alone cannot say what the time went to: see the drain-budget counters above, which report
	// THAT time ran out and never what spent it.
	TRACE_CPUPROFILER_EVENT_SCOPE(Crowdy_DeliverFrame);

	if (!ReceiveParser || !ReceiveRegistry)
	{
		const bool bShouldWarn = !bWarnedAboutMissingReceiveTarget;
		bWarnedAboutMissingReceiveTarget = true;

		// Traffic that arrives and reaches nothing looks exactly like traffic that never arrived, which is the kind
		// of thing that gets debugged from the wrong end.
		UE_CLOG(bShouldWarn, LogCrowdyNet, Warning,
			TEXT("A replication connection is delivering messages but no parser is attached, so they are being "
				"dropped."));
		return;
	}

	UCrowdyUDPSubsystem* Stats = ReceiveStats.Get();

	// Teardown has to stop dispatching into objects that are going away, and a developer who turned message
	// processing off meant it.
	if (Stats && (Stats->IsShuttingDown() || Stats->IsDiscardingReceivedMessages()))
	{
		return;
	}

	// The envelope is already off the message here, so this reaches the decoder directly. A bundle never arrives,
	// since the connection unpacks one before queueing its members, and an unknown opcode still goes to the
	// registry as the default message.
	const TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe> Message = ReceiveParser->DecodeFrame(Frame);

	if (Stats)
	{
		Stats->IncrementReceivedMessageCount();
	}

	ReceiveRegistry->DispatchMessage(Message);
}

void UCrowdyCppReplicationSubsystem::BeginFrameDrainBudget()
{
	if (DrainBudgetFrame == GFrameCounter)
	{
		return;
	}

	DrainBudgetFrame = GFrameCounter;
	DrainMessagesUsedThisFrame = 0;
	DrainSecondsUsedThisFrame = 0.0;
}

int32 UCrowdyCppReplicationSubsystem::RemainingDrainMessages() const
{
	const int32 Budget = FMath::Max(CVarReceiveMaxMessagesPerPoll.GetValueOnGameThread(), 1);
	return FMath::Max(Budget - DrainMessagesUsedThisFrame, 0);
}

double UCrowdyCppReplicationSubsystem::RemainingDrainSeconds() const
{
	const double Budget = FMath::Max(CVarReceiveMaxDrainMilliseconds.GetValueOnGameThread(), 0.1f) / 1000.0;
	return FMath::Max(Budget - DrainSecondsUsedThisFrame, 0.0);
}

void UCrowdyCppReplicationSubsystem::ConsumeDrainBudget(const int32 Messages, const double Seconds)
{
	DrainMessagesUsedThisFrame += FMath::Max(Messages, 0);
	DrainSecondsUsedThisFrame += FMath::Max(Seconds, 0.0);

	// Recorded here rather than in either drain, because this is the one place both of them report through and the
	// cost that decides the message allowance is the cost of the whole drain.
	InboundDrainCost.Record(Messages, Seconds);
}

FCrowdyDrainCost UCrowdyCppReplicationSubsystem::GetInboundDrainCost() const
{
	const double Window = FMath::Max(CVarReceiveMaxDrainMilliseconds.GetValueOnGameThread(), 0.1f) / 1000.0;
	return InboundDrainCost.Read(Window);
}

FString UCrowdyCppReplicationSubsystem::DescribeInboundDrainCost() const
{
	const FCrowdyDrainCost Cost = GetInboundDrainCost();
	const int32 Configured = FMath::Max(CVarReceiveMaxMessagesPerPoll.GetValueOnGameThread(), 1);
	const float BudgetMilliseconds = FMath::Max(CVarReceiveMaxDrainMilliseconds.GetValueOnGameThread(), 0.1f);

	if (Cost.Messages <= 0)
	{
		return FString::Printf(
			TEXT("no inbound messages have been delivered since the last reading, so there is no per-message cost to "
				"derive crowdy.net.receive.maxmessages (currently %d) from."),
			Configured);
	}

	// Said as the derivation rather than as a measurement, because the count is meant to sit just above what the
	// window affords and a reader who has only the cost still has to do that arithmetic.
	return FString::Printf(
		TEXT("delivering one inbound message cost %.2f us over the last %lld, in %lld drains of %.0f (%.1f ms of "
			"drain). The %.1f ms window (crowdy.net.receive.maxdrainms) affords about %d at that cost, against a "
			"message allowance (crowdy.net.receive.maxmessages) of %d."),
		Cost.MicrosecondsPerMessage, Cost.Messages, Cost.Drains, Cost.MessagesPerDrain, Cost.Seconds * 1000.0,
		BudgetMilliseconds, Cost.MessagesAffordedByWindow, Configured);
}

void UCrowdyCppReplicationSubsystem::ArmLocalDelivery(const TConstArrayView<FCrowdyActorId> ActorIds,
	const FCrowdyLoopbackSettings& Settings)
{
#if UE_BUILD_SHIPPING
	// Said rather than ignored, so a build that somehow reaches this reports why nothing happened
	// instead of running on looking armed.
	UE_LOG(LogCrowdyNet, Warning,
		TEXT("Local delivery was asked to hold %d actor ids for %.0f ms, and it is development tooling that "
			"does not run in a Shipping build."),
		ActorIds.Num(), Settings.LatencySeconds * 1000.0f);
#else
	Loopback.Arm(ActorIds, Settings);

	// Arm refuses an empty id set, so the ticker follows what actually armed rather than what was asked
	// for.
	if (!Loopback.IsArmed() || LoopbackTickerHandle.IsValid())
	{
		return;
	}

	LoopbackTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UCrowdyCppReplicationSubsystem::TickDrainLocalDelivery));
#endif
}

void UCrowdyCppReplicationSubsystem::DisarmLocalDelivery()
{
	if (LoopbackTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(LoopbackTickerHandle);
		LoopbackTickerHandle.Reset();
	}

	Loopback.Disarm();
}

bool UCrowdyCppReplicationSubsystem::TickDrainLocalDelivery(float DeltaTime)
{
	if (!Loopback.IsArmed())
	{
		return true;
	}

	// Whatever the connection drain has left of this frame's budget, not a second copy of it. Sharing one
	// budget is the point rather than a convenience: a locally delivered crowd has to be one this client
	// could genuinely have taken off the network, and two full budgets in one frame would let it deliver
	// twice that and inflate the very number this mode exists to produce.
	BeginFrameDrainBudget();
	const int32 MaxMessages = RemainingDrainMessages();
	const double MaxSeconds = RemainingDrainSeconds();
	if (MaxMessages <= 0 || MaxSeconds <= 0.0)
	{
		return true;
	}

	// Set for the same reason the connection poll sets it: a delivered update reaches reception layers,
	// and one of those is free to replace the connection, which must not be disposed from inside a
	// delivery that is still running.
	bDraining = true;

	const double Started = FPlatformTime::Seconds();
	int32 Delivered = 0;

	{
		// Kept apart from the connection's drain rather than sharing its name. What surrounds delivery here is a
		// locked heap pop, and what surrounds it there is the library's ring, so one scope over both would average
		// two different costs into a number that describes neither.
		TRACE_CPUPROFILER_EVENT_SCOPE(Crowdy_LocalDeliveryDrain);

		Delivered = Loopback.DrainDue(MaxMessages, MaxSeconds,
			[this](const FCrowdyFrame& Frame)
			{
				DeliverFrame(Frame);
			});
	}

	ConsumeDrainBudget(Delivered, FPlatformTime::Seconds() - Started);

	bDraining = false;

	if (DeferredDisposal.Num() > 0)
	{
		TArray<TSharedPtr<FCrowdyCppReplication>> Retiring = MoveTemp(DeferredDisposal);
		DeferredDisposal.Reset();
		for (const TSharedPtr<FCrowdyCppReplication>& Held : Retiring)
		{
			if (Held.IsValid())
			{
				Held->Disconnect();
			}
		}
	}

	return true;
}

TSharedPtr<FCrowdyCppReplication> UCrowdyCppReplicationSubsystem::GetConnection() const
{
	FScopeLock Lock(&ConnectionMutex);
	return Connection;
}

bool UCrowdyCppReplicationSubsystem::IsRouting() const
{
	return GetConnection().IsValid();
}

bool UCrowdyCppReplicationSubsystem::IsPolling() const
{
	return PollTickerHandle.IsValid();
}

ECrowdyCppSendOutcome UCrowdyCppReplicationSubsystem::TrySendMessage(const ICrowdyMessage& Message)
{
	// Ahead of the connection check on purpose, so that a locally delivered crowd does not need a
	// connection to exist. When local delivery is not armed this is one relaxed atomic read and the rest
	// of this function is what it has always been, message for message.
	if (Loopback.IsArmed())
	{
		TArray<uint8> LocalStorage;
		FCrowdyCppOutboundFrame LocalFrame;
		FString LocalError;

		// Split here as well rather than once above, because moving the shared split ahead of the
		// connection check would change what a send with no connection installed reports: a malformed
		// message would start coming back Refused where it reports NotRouted today.
		if (CrowdyCppSend::SplitMessage(Message, LocalStorage, LocalFrame, LocalError)
			&& Loopback.TryAcceptOutbound(LocalFrame))
		{
			return ECrowdyCppSendOutcome::Sent;
		}
	}

	// Pinned for the whole send, so a concurrent SetConnection cannot release the connection underneath it.
	const TSharedPtr<FCrowdyCppReplication> Pinned = GetConnection();
	if (!Pinned.IsValid())
	{
		bool bShouldWarn = false;
		{
			FScopeLock Lock(&ConnectionMutex);
			bShouldWarn = !bWarnedAboutMissingConnection;
			bWarnedAboutMissingConnection = true;
		}

		// Said once per connection cycle, and said at all: this is the only transport, so a message offered before a
		// connection exists is dropped, and a drop that says nothing looks exactly like a message that was sent.
		UE_CLOG(bShouldWarn, LogCrowdyNet, Warning,
			TEXT("No replication connection is installed, so outbound messages are being dropped until one is."));

		return ECrowdyCppSendOutcome::NotRouted;
	}

	// Held for the whole send, because the frame's actor id and payload are views into it rather than copies.
	TArray<uint8> Serialized;
	FCrowdyCppOutboundFrame Frame;
	FString Error;
	if (!CrowdyCppSend::SplitMessage(Message, Serialized, Frame, Error))
	{
		UE_LOG(LogCrowdyNet, Warning, TEXT("Dropped a %s: %s."), *Message.GetTypeName().ToString(), *Error);
		return ECrowdyCppSendOutcome::Refused;
	}

	const bool bAccepted = Frame.bIsChannel
		? Pinned->SendChannelMessage(Frame.ChannelId, Frame.Uuid, Frame.Payload, Error)
		: Pinned->SendSpatial(Frame.Opcode, Frame.ChunkX, Frame.ChunkY, Frame.ChunkZ, Frame.Uuid, Frame.Payload,
			Frame.Distance, Frame.Decay, Error);

	if (!bAccepted)
	{
		UE_LOG(LogCrowdyNet, Warning, TEXT("The replication connection refused a %s: %s."),
			*Message.GetTypeName().ToString(), *Error);
		return ECrowdyCppSendOutcome::Refused;
	}

	// Counted once the connection has accepted it, so a refused message is not reported as sent. Sends run on
	// whichever thread built the message, so this accumulates into an atomic here and the game thread publishes it
	// from the stats report. Resolving a weak object pointer from a background thread would race a teardown that
	// clears it.
	if (static_cast<ECrowdyMessageType>(Frame.Opcode) == ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION)
	{
		PendingClientNotifiesSent.fetch_add(1, std::memory_order_relaxed);
	}

	UE_CLOG(CrowdyNetTrace::Net(), LogCrowdyNet, Log, TEXT("Routed a %s of %d payload octets."),
		*Message.GetTypeName().ToString(), Frame.Payload.Num());

	return ECrowdyCppSendOutcome::Sent;
}
