#pragma once

#include <atomic>
#include "Containers/Ticker.h"
#include "CoreMinimal.h"
#include "Network/CrowdyCpp/CrowdyCppLoopback.h"
#include "Network/CrowdyCpp/CrowdyDrainCostMeter.h"
#include "Serialization/CrowdyFrame.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "CrowdyCppReplicationSubsystem.generated.h"

class FCrowdyCppReplication;
class FCrowdyMessageParser;
enum class ECrowdyCppConnState : uint8;
class FCrowdyServiceRegistry;
class ICrowdyMessage;
class UCrowdyUDPSubsystem;

/** What happened to a message offered to the replication connection. */
enum class ECrowdyCppSendOutcome : uint8
{
	/** No connection is installed, so there was nothing to send it through and the message was dropped. */
	NotRouted,

	/** Accepted for sending by the connection. Accepted is not delivered: the wire is still UDP. */
	Sent,

	/**
	 * Routed and refused. The message is dropped and the reason is logged.
	 *
	 * The reason is usually that the message cannot be carried as written, but it also covers a connection that is
	 * down or an outbound queue that is full, so a refusal is not by itself proof that the message was malformed.
	 */
	Refused
};

/**
 * Everything needed to open a replication connection, in Unreal terms.
 *
 * It is filled in by whoever owns the game's configuration and credentials rather than read from here, because the
 * developer settings and the authentication plane live above this module. Nothing in it is retained past the call.
 */
struct FCrowdyCppConnectionRequest
{
	int64 AppId = 0;

	/** The app-scoped token the connection signs with. Exactly 64 characters, or the connection will not open. */
	FString Token;
	int64 GameTokenId = 0;

	/** ISO-8601, as the mint reports it. Empty or unparseable leaves the connection with no proactive refresh. */
	FString TokenExpiresAtIso8601;

	/**
	 * Where the server assignment is issued: the app's own datacenter endpoint. Empty falls back to DiscoveryUrl,
	 * which every datacenter answers, so a connection attempted before an app has been resolved still reaches a
	 * server rather than nothing.
	 */
	FString GameApiUrl;

	/** The shared origin, so a connection whose endpoint dies can be told where to go instead of retrying it. */
	FString DiscoveryUrl;

	/** How long the connection may go without receiving anything before it re-assigns. Zero disables the watchdog. */
	float SilenceTimeoutSeconds = 15.f;

	/**
	 * Whether to take the assignment's IPv6 address. The connection picks one address family and does not fall back
	 * to the other, so this is only set when IPv6 has been chosen deliberately.
	 */
	bool bPreferIpv6 = false;

	/**
	 * Ask the game to rotate the app token. Called on the game thread when the connection reports its token is due
	 * or expired; the rotation itself is asynchronous, and the new token is picked up from the session once it
	 * lands. Rotation lives above this module, which is why it arrives as a callback.
	 */
	TFunction<void()> RequestTokenRefresh;

	/**
	 * Tell the game the app has no capacity for another player. Called on the game thread when the server answers
	 * the assignment that way, which is an answer rather than a failure: there is no address to connect to, so the
	 * player should be told instead of watching a connection retry into a server that has no room.
	 */
	TFunction<void()> ReportAppFull;
};

/**
 * Owns the replication connection the running game sends its UDP traffic through, in both directions.
 *
 * There is one connection per game instance, for the same reason there is one API client: it owns a network thread
 * and a queue that has to be drained on the game thread, so a second one is another teardown to get right.
 *
 * It also owns the inbound direction: an installed connection is polled once per frame on the game thread and every
 * message it delivers is handed to the parser and the reception-layer registry.
 *
 * The connection is installed from outside through SetConnection or opened through OpenConnection. Until one exists
 * every message reports NotRouted and is dropped, since there is nowhere else for it to go.
 *
 * Threading. Sends arrive from worker threads, because the dispatch paths that build these messages run as tasks, so
 * the connection pointer is read under a lock and the connection itself accepts a send from any thread. Everything
 * else belongs to the game thread, inbound delivery included.
 */
UCLASS()
class CROWDYNET_API UCrowdyCppReplicationSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	// Nothing is built at initialization, so there is no Initialize override; the connection arrives from outside and
	// is disposed here.
	virtual void Deinitialize() override;

	/** Resolve the host from any object with a world. Null outside a game instance. */
	static UCrowdyCppReplicationSubsystem* Get(const UObject* WorldContextObject);

	/**
	 * Install the connection to route through, or clear it by passing null. Disposes whatever was installed before,
	 * which completes its teardown, so in-flight queued sends on the old connection do not reach the wire.
	 *
	 * Installing a connection also installs this subsystem's own handler set on it and starts polling it once per
	 * frame, so anything the caller had set on it is replaced. Clearing stops the polling.
	 *
	 * Installing the connection that is already installed does nothing, rather than disposing it: disposal is
	 * terminal, so treating an idempotent re-install as a replacement would leave the live connection closed and
	 * still installed.
	 *
	 * Safe to call from inside delivery, which is the case that looks unlikely and is not: a message this subsystem
	 * delivered can reach anything, including whatever decides to reconnect. Disposing the outgoing connection while
	 * its own drain is on the stack would destroy the handler that is running and block the game thread joining its
	 * network thread, so during a drain the disposal is held over until the drain returns.
	 *
	 * Game thread only.
	 */
	void SetConnection(TSharedPtr<FCrowdyCppReplication> InConnection);

	/**
	 * Build a connection from this request and start it, or restart the one already installed.
	 *
	 * A connection that is already installed and still usable is reconnected in place rather than replaced, because
	 * replacing it would discard the queued sends and the session it is already signing under. It is rebuilt when
	 * the request describes different credentials, since those are fixed for the life of a connection.
	 *
	 * Returns false when no connection could be built, which in practice means a token that cannot sign. The outcome
	 * of the attempt itself arrives later as a state change, not from here: this returns as soon as the connection
	 * has been asked to open.
	 *
	 * Game thread only.
	 */
	bool OpenConnection(const FCrowdyCppConnectionRequest& Request);

	/**
	 * Close whatever is installed and route nothing until a connection is opened again.
	 *
	 * A connection signs with the token it was opened with rather than reading the session each time, so a session
	 * that has ended has to be closed here explicitly: clearing the credentials elsewhere does not reach it.
	 *
	 * Game thread only.
	 */
	void CloseConnection();

	/**
	 * Install rotated token material on the installed connection, so it signs with the current token rather than the
	 * one it opened with.
	 *
	 * Returns false when there was no connection to install it on, which is the caller's cue that a rotation
	 * arriving before the first connection still leaves it to open one.
	 *
	 * Game thread only.
	 */
	bool InstallToken(const FString& Token, int64 GameTokenId, const FString& ExpiresAtIso8601);

	/**
	 * Where an inbound message goes: the parser that turns a frame into a typed message, the registry that hands it
	 * to the reception layers, and the UDP subsystem that owns the received-message counter the network stats read.
	 *
	 * Both plain pointers belong to UCrowdySDKSubsystem. Pass null for all three to detach, which whoever injected
	 * them must do before they are destroyed. Until they are injected an installed connection still runs, and its
	 * messages are counted and dropped with one warning rather than delivered nowhere silently.
	 *
	 * Game thread only.
	 */
	void SetReceiveTarget(FCrowdyMessageParser* InParser, FCrowdyServiceRegistry* InRegistry,
		UCrowdyUDPSubsystem* InUdpSubsystem);

	/** The installed connection, or null. Held by shared pointer so a caller cannot outlive it by accident. */
	TSharedPtr<FCrowdyCppReplication> GetConnection() const;

	/** True when a connection is installed, meaning the next message has somewhere to go. */
	bool IsRouting() const;

	/** True while an installed connection is being drained once per frame. */
	bool IsPolling() const;

	/**
	 * Offer one message to the connection. Returns NotRouted when no connection is installed, in which case the
	 * message was dropped and one warning per connection cycle says so.
	 *
	 * A Refused outcome is a malformed or unsendable message rather than a transport hiccup, so it is logged as a
	 * warning and dropped.
	 */
	ECrowdyCppSendOutcome TrySendMessage(const ICrowdyMessage& Message);

	/**
	 * Stop sending actor updates for these ids and hand them back into this process after a delay
	 * instead, so a crowd larger than this machine's uplink can carry can still be observed from it.
	 *
	 * Development tooling. It refuses to arm in a Shipping build, and it never intercepts an id it was
	 * not given, so a session running it still round-trips its own player through the real relay.
	 *
	 * Game thread only.
	 */
	void ArmLocalDelivery(TConstArrayView<FCrowdyActorId> ActorIds, const FCrowdyLoopbackSettings& Settings);

	/** Go back to sending everything. Whatever had not been delivered yet is thrown away. */
	void DisarmLocalDelivery();

	bool IsLocalDeliveryArmed() const { return Loopback.IsArmed(); }

	FCrowdyLoopbackStats GetLocalDeliveryStats() const { return Loopback.GetStats(); }

	/** Zero the local-delivery counters so the next reading describes one interval rather than the run. */
	void ResetLocalDeliveryCounters() { Loopback.ResetCounters(); }

	/**
	 * What one delivered message has cost since the last reset, measured over the whole drain, and how that
	 * compares with the two budgets it is spent against.
	 *
	 * This is where crowdy.net.receive.maxmessages comes from: the count is meant to sit just above what the time
	 * budget affords, and only a reading of the whole drain can say what that is. Game thread only.
	 */
	FCrowdyDrainCost GetInboundDrainCost() const;

	/** The above as one line, so the count, the budget and the measurement are described in one place. */
	FString DescribeInboundDrainCost() const;

	/** Start a fresh cost window, so the next reading describes an interval rather than the run and its warm-up. */
	void ResetInboundDrainCost() { InboundDrainCost.Reset(); }

private:
	/** Install the delivery handlers on a connection about to be installed. */
	void InstallReceiveHandlers(const TSharedPtr<FCrowdyCppReplication>& Target);

	/** Drain the connection on the game thread, so reception layers run where engine objects are safe to touch. */
	bool TickPollConnection(float DeltaTime);

	/** Decode one delivered frame and hand it to the reception layers, exactly as the receive loop does. */
	void DeliverFrame(const FCrowdyFrame& Frame);

	/**
	 * Deliver whatever local delivery is holding and whose delay has run out.
	 *
	 * It works to the same message and time budgets the real drain does, which is the point of having it
	 * rather than delivering everything the moment it is due: a locally delivered crowd has to be one
	 * this client could genuinely have taken off the network, or the frame cost measured against it
	 * describes nothing.
	 */
	bool TickDrainLocalDelivery(float DeltaTime);

	/**
	 * Log a server error frame together with the send the transport believes provoked it. Without the attribution
	 * an error reads as a code and a sequence number, which says that something was rejected but not what, and the
	 * sequence alone is not something a reader can trace back to a call site.
	 */
	void ReportSendError(const struct FCrowdyCppSendError& Error);

	/**
	 * Say something, occasionally, about inbound traffic the drain could not keep up with: drains the frame budget
	 * cut short while the connection still held messages, and messages the connection then had to throw away. A
	 * truncated drain is what leaves messages behind for a later one to lose, so the two are reported together.
	 */
	void ReportInboundPressure(const FCrowdyCppReplication& Polled);

	/**
	 * Publish once-per-second throughput and message-count deltas to the UDP subsystem's stat counters, so
	 * "stat crowdyudp" reflects the connection this subsystem is actually driving.
	 *
	 * The connection's own counters are cumulative since it opened, so a reconnect restarts them at zero; a
	 * plain subtraction across that reset would read as a large negative rate for one report, so the delta is
	 * clamped to the current reading whenever it would otherwise go negative.
	 */
	void ReportNetworkStats(const FCrowdyCppReplication& Polled);

	/** Turn a connection lifecycle change into the connection state and delegates the game already watches. */
	void ApplyConnectionState(ECrowdyCppConnState State);

	/** The credentials the installed connection opened with, so a request naming different ones rebuilds it. */
	FString OpenedWithToken;
	int64 OpenedWithAppId = 0;

	/**
	 * Whether the last assignment answered that the app has no room. It changes what a failed connection means:
	 * a full app is an answer and retrying into it achieves nothing, where any other failure is worth recovering
	 * from. Game thread only.
	 */
	bool bLastAssignmentReportedAppFull = false;

	mutable FCriticalSection ConnectionMutex;

	TSharedPtr<FCrowdyCppReplication> Connection;

	/**
	 * Whether the "nothing to send through" warning has been logged. Sends run at frame rate, so this says it once
	 * per connection cycle instead of once per message.
	 */
	bool bWarnedAboutMissingConnection = false;

	/** Game thread only, so no lock: they are written by the injector and read by the poll on the same thread. */
	FCrowdyMessageParser* ReceiveParser = nullptr;
	FCrowdyServiceRegistry* ReceiveRegistry = nullptr;
	TWeakObjectPtr<UCrowdyUDPSubsystem> ReceiveStats;

	/**
	 * Client notifications handed to the connection since the last stats report. Sends happen on any thread but the
	 * stats target above may only be touched from the game thread, so the count is accumulated here and drained by
	 * the poll.
	 */
	std::atomic<int64> PendingClientNotifiesSent { 0 };

	/** As above, said once rather than once per delivered message. */
	bool bWarnedAboutMissingReceiveTarget = false;

	FTSTicker::FDelegateHandle PollTickerHandle;

	/**
	 * Local delivery drains on its own ticker rather than inside the connection poll, so that it works
	 * whether or not a connection is installed.
	 */
	FTSTicker::FDelegateHandle LoopbackTickerHandle;

	FCrowdyCppLoopback Loopback;

	/**
	 * The frame's delivery budget, shared by the connection drain and the local-delivery drain rather
	 * than handed to each of them in full.
	 *
	 * Sharing it is not tidiness. Both tickers run every frame, and local delivery is deliberately
	 * partial (the local player still round-trips through the relay), so both are live in the same
	 * frame; a budget each would let one frame deliver twice what the client being measured can. The
	 * whole point of local delivery is that the crowd it shows is one this client could genuinely have
	 * taken off the network, and a doubled ceiling would inflate exactly that number.
	 *
	 * Game thread only. Keyed on the frame counter so whichever drain runs first opens the budget.
	 */
	void BeginFrameDrainBudget();
	int32 RemainingDrainMessages() const;
	double RemainingDrainSeconds() const;
	void ConsumeDrainBudget(int32 Messages, double Seconds);

	uint64 DrainBudgetFrame = 0;
	int32 DrainMessagesUsedThisFrame = 0;
	double DrainSecondsUsedThisFrame = 0.0;

	/**
	 * Fed by both drains, since both spend the one budget and the reading has to describe all of it.
	 *
	 * Reset by whoever reports it, so two readers in one session each get a shorter window rather than a wrong
	 * figure: it is a rate, and a rate survives being sampled over a smaller interval. Game thread only.
	 */
	FCrowdyDrainCostMeter InboundDrainCost;

	/** True for the duration of a drain, so a connection replaced from inside delivery is not disposed under it. */
	bool bDraining = false;

	/** Connections replaced during a drain, disposed as soon as it finishes. */
	TArray<TSharedPtr<FCrowdyCppReplication>> DeferredDisposal;

	/** The dropped-message count last reported, so only an increase is worth saying anything about. */
	int64 LastReportedRingDropped = 0;

	/**
	 * Drains since the last report that were cut short, split by which budget ended them.
	 *
	 * Two counters rather than one because the two have opposite answers. Spending the whole message allowance means
	 * the count is the ceiling and raising it is the fix. Running out of time means delivery costs more per message
	 * than the frame can afford, and raising the count only spends more of the frame on the same backlog. Reported as
	 * a count rather than a measurement: what matters to a live gate is that drains are being cut short at all, and
	 * that it is happening ahead of the drops reported alongside them.
	 *
	 * Game thread only.
	 */
	int64 MessageBudgetTruncatedDrains = 0;
	int64 TimeBudgetTruncatedDrains = 0;

	double LastInboundPressureReportSeconds = 0.0;

	/**
	 * Error frames are server-supplied and unsigned, so a peer that wanted to could produce them as fast as the
	 * drain will deliver them. These two hold the reporting to a few lines a second and say how many were left
	 * out, so a real problem is still legible and a flood cannot become the problem itself. Game thread only.
	 */
	double SendErrorWindowStartedSeconds = 0.0;
	int32 SendErrorsReportedThisWindow = 0;
	int32 SendErrorsSuppressedThisWindow = 0;

	/**
	 * The subset of the connection's cumulative counters ReportNetworkStats turns into a rate. A plain copy
	 * rather than the bridge's own stats type, so this header does not have to include the bridge module's
	 * public headers just to hold five integers.
	 */
	struct FCounterSnapshot
	{
		int64 BytesSent = 0;
		int64 BytesReceived = 0;
		int64 DatagramsSent = 0;
		int64 DatagramsReceived = 0;
		int64 MessagesSent = 0;
	};

	/** What the connection's cumulative counters read at the last stats report, so the next one is a difference. */
	FCounterSnapshot LastStatsReading;

	double LastStatsReportSeconds = 0.0;
};
