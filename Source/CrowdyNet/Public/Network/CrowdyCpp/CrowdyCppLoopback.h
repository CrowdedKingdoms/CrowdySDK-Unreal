#pragma once

#include "Containers/Array.h"
#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Math/RandomStream.h"
#include "Serialization/CrowdyActorId.h"
#include "Templates/Function.h"

#include <atomic>

struct FCrowdyCppOutboundFrame;
struct FCrowdyFrame;

/**
 * How a locally delivered actor update is shaped before it comes back in.
 *
 * The latency is what a real update spends between two machines and is the reason this exists: an
 * update that arrives in the same call it was sent in would let interpolation and arrival-interval
 * estimation see a cadence no network produces, so every number measured against it would describe a
 * game nobody plays.
 */
struct FCrowdyLoopbackSettings
{
	/** The one-way delay every update waits out before it is delivered. */
	float LatencySeconds = 0.04f;

	/**
	 * How far either side of that delay an individual update may land. Held at or below the latency, so
	 * that no update is ever due before it was sent.
	 *
	 * Past half the sending interval this starts reordering a single sender's own stream, which one path
	 * between two machines rarely does, so a value that large is measuring something other than the
	 * network it stands in for.
	 */
	float JitterSeconds = 0.01f;

	/** The fraction of updates thrown away instead of delivered, as real loss would. */
	float LossFraction = 0.0f;

	/**
	 * A ceiling on frames waiting to be delivered. It exists because the sender runs on worker threads
	 * and the drain runs on the game thread under a fixed budget, so a stalled game thread would
	 * otherwise let the queue grow until the process ran out of memory.
	 */
	int32 MaxQueuedFrames = 65536;
};

/**
 * What the loopback did, counted since the last reset.
 *
 * Accepted and Delivered are reported apart because their difference is the whole diagnosis: a queue
 * that grows is the receiving client failing to keep up, which is exactly what a crowd this large is
 * being run to find out.
 */
struct FCrowdyLoopbackStats
{
	int64 Accepted = 0;
	int64 Delivered = 0;
	int64 DroppedToLoss = 0;
	int64 DroppedToQueueFull = 0;

	/**
	 * Drains that ended with frames still due, split by which budget ended them.
	 *
	 * Split because the two have opposite answers: spending the whole message allowance means the count
	 * is the ceiling and raising it is the fix, while running out of time means delivery costs more per
	 * message than the frame can afford and raising the count only spends more of the frame on the same
	 * backlog.
	 */
	int64 DrainsCutShortByMessageBudget = 0;
	int64 DrainsCutShortByTimeBudget = 0;

	int32 QueueDepth = 0;
	int32 LargestQueueDepth = 0;
	int32 LargestDrain = 0;
};

/**
 * Delivers a set of actor updates back into this process instead of sending them, after a delay.
 *
 * It exists so a crowd larger than one machine's uplink can carry may still be observed from that
 * machine. An actor update is 185 octets on the wire and there is no client-to-server bundling, so two
 * thousand simulated players is about thirty megabits a second upstream: a link limit rather than a
 * machine limit, and one no amount of local optimisation moves. Sending those updates nowhere and
 * handing them straight back removes the link from the measurement while leaving everything the
 * receiving client does with them exactly where it was.
 *
 * The updates that come back are the ones that were really built: the state payload has been through
 * the real serializer, the frame has been through the real send-side split, and what is handed back is
 * the same structure the transport hands to delivery for a message that genuinely arrived. What is not
 * exercised is the socket, the signing, and the relay, and with the relay goes the server's own
 * decision about which updates are relevant to this observer. So this measures what a client costs to
 * RECEIVE a crowd of a given size; it cannot measure what the server would have chosen to send.
 *
 * Interception is by actor id, never blanket, so a session running this still round-trips its own
 * player through the real relay while the simulated crowd stays local.
 *
 * Threading: sends arrive from worker threads and drains run on the game thread.
 */
class CROWDYNET_API FCrowdyCppLoopback
{
public:
	/**
	 * Take over delivery for these actor ids. An empty set arms nothing, since interception is by id
	 * and there would be no id to match.
	 */
	void Arm(TConstArrayView<FCrowdyActorId> InActorIds, const FCrowdyLoopbackSettings& InSettings);

	/** Stop intercepting and throw away whatever had not been delivered yet. */
	void Disarm();

	bool IsArmed() const { return bArmed.load(std::memory_order_relaxed); }

	/**
	 * Take this outbound frame instead of letting it reach the wire, and return true when it was taken.
	 * False means the caller still owns it, which covers a frame for an id this is not intercepting, a
	 * message that is not an actor update, and a frame thrown away as simulated loss.
	 *
	 * Callable from any thread.
	 */
	bool TryAcceptOutbound(const FCrowdyCppOutboundFrame& Frame);

	/**
	 * Deliver every frame whose delay has run out, under the same message and time budgets the real
	 * inbound drain works to. Sharing those budgets is the point rather than an implementation detail:
	 * a loopback that delivered everything the moment it was due would show a crowd the receiving
	 * client could never actually have taken off the network.
	 *
	 * Returns how many frames were delivered. Game thread only.
	 */
	int32 DrainDue(int32 MaxFrames, double MaxSeconds, TFunctionRef<void(const FCrowdyFrame&)> Deliver);

	FCrowdyLoopbackStats GetStats() const;

	/** Zero the counters, leaving the queue and the armed ids alone. */
	void ResetCounters();

#if WITH_DEV_AUTOMATION_TESTS
	/**
	 * Drive the delay from a value a test sets rather than from the platform clock.
	 *
	 * A latency this small measured against the real clock would need a test that sleeps, and a sleeping
	 * test asserts the machine's scheduler as much as the code. Set before any traffic and advanced
	 * between steps; not for use while sends are arriving from other threads.
	 */
	void SetTimeForTests(double InSeconds);
	void ClearTimeForTests();
#endif

private:
	double NowSeconds() const;

	struct FQueuedFrame
	{
		double DueSeconds = 0.0;
		int64 AppId = 0;
		int64 ChunkX = 0;
		int64 ChunkY = 0;
		int64 ChunkZ = 0;
		int64 TimestampMillis = 0;
		FCrowdyActorId Uuid;
		TArray<uint8> Payload;
		uint8 Opcode = 0;
		uint8 Sequence = 0;
	};

	/** Orders the queue by when a frame is due, so a jittered frame is delivered in the order it lands. */
	struct FDueFirst
	{
		bool operator()(const FQueuedFrame& A, const FQueuedFrame& B) const
		{
			return A.DueSeconds < B.DueSeconds;
		}
	};

	mutable FCriticalSection Mutex;

	/** Read on every send from a worker thread, so it is kept out of the lock. */
	std::atomic<bool> bArmed { false };

	TSet<FCrowdyActorId> InterceptedIds;
	FCrowdyLoopbackSettings Settings;
	TArray<FQueuedFrame> Queue;
	FCrowdyLoopbackStats Stats;

	/**
	 * Both the loss draw and the jitter draw, seeded rather than time-derived so that two runs of the
	 * same population lose and delay the same updates and can be compared. It is also why neither is
	 * drawn from the global stream: this one is only ever advanced under the lock.
	 */
	FRandomStream NoiseStream;

	uint8 NextSequence = 0;

	bool bUseTimeOverrideForTest = false;
	double TestTimeSeconds = 0.0;
};
