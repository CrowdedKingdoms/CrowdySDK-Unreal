#pragma once

#include "CoreMinimal.h"

/**
 * How long a remote entity's next segment should be replayed over.
 *
 * The answer is the interval actually observed between arrivals, not a fixed setting. A segment that
 * took 130 ms to arrive but is replayed over 100 ms leaves the entity running ahead and then standing
 * still for the remaining 30 ms, so any wobble in arrival timing turns into visible stutter. Latency
 * itself is not the problem and does not appear here: a slow route delays every update equally and
 * leaves the interval between them unchanged.
 *
 * Kept apart from the subsystem that calls it because it is a pure function of arrival times and can
 * therefore be tested without a world, a Mass entity, or a network.
 */

/**
 * The last few observed intervals between arrivals, oldest overwritten first.
 *
 * An order statistic rather than an average is taken from these, because a route that drops packets
 * produces exactly the outliers an average is worst at: one dropped update looks like a doubled
 * interval and drags a mean upwards, while two updates arriving in the same frame drag it down.
 */
struct CROWDYMASS_API FCrowdyArrivalIntervalHistory
{
	static constexpr int32 SampleCount = 5;

	void Record(float IntervalSeconds);

	/**
	 * The interval to plan the next segment against, or zero when nothing has been recorded yet.
	 *
	 * Deliberately NOT the typical interval. It is the second largest of the samples, roughly an
	 * eightieth percentile, because what a segment has to outlast is not the average wait for the next
	 * update but very nearly the longest one. Planning against the middle sample means, by definition,
	 * that half of all intervals outlast the segment, and each of those halves is an entity arriving
	 * early and standing still.
	 *
	 * Taking the SECOND largest rather than the largest is what keeps a single dropped update from
	 * mattering: a drop shows up as one doubled interval, which is precisely the maximum this discards.
	 *
	 * The virtue over a fixed multiplier on the middle sample is that this measures the connection's
	 * spread instead of guessing it. A jittery route reports a wider gap between its middle and its
	 * second largest sample and is given more headroom automatically; a clean one reports almost no
	 * gap and pays almost no extra latency.
	 */
	float PlanningInterval() const;

	void Reset();

	bool IsEmpty() const { return Count == 0; }

private:

	float Samples[SampleCount] = {};
	uint8 Count = 0;
	uint8 NextSlot = 0;
};

/** Tuning for the estimate. Resolved from project settings, and overridable at runtime for tuning against a real connection. */
struct CROWDYMASS_API FCrowdyInterpolationTuning
{
	/**
	 * How much longer than the typical interval a segment is replayed over.
	 *
	 * Above 1 the next update arrives while the entity is still moving and simply retargets it from
	 * wherever it has got to, which is seamless. Below 1 the entity arrives early and waits, which is
	 * the stutter this exists to remove. Note that 1.0 is not the neutral choice it looks like: half
	 * of all intervals are longer than the typical one by definition, so an exact match still stalls
	 * on that half. The cost of overshooting is that the entity sits slightly further behind.
	 */
	float Overshoot = 1.2f;

	/** Bounds on the result, so neither a burst of arrivals nor a stall can produce an absurd duration. */
	float MinDuration = 0.05f;
	float MaxDuration = 0.5f;

	/** Used until an interval has been observed, which is every entity's first update. */
	float FirstUpdateDuration = 0.1f;

	/** When false, every segment uses FirstUpdateDuration, which is the behaviour that predates the estimate. */
	bool bAdaptive = true;
};

/** What the caller needs to know about the segment it just started. */
struct CROWDYMASS_API FCrowdySegmentTiming
{
	/** Seconds to replay the segment over. */
	float Duration = 0.1f;

	/**
	 * Multiplier turning the entity's on-screen speed back into the speed it is really travelling at.
	 *
	 * Replaying a segment over longer than it took to arrive means the entity crosses the ground more
	 * slowly than the player it stands for actually did, so the velocity derived from watching it move
	 * is low by exactly that factor. Animation picks a gait from that velocity, so without this a
	 * crowd would systematically walk where it should jog.
	 */
	float VelocityScale = 1.0f;
};

namespace CrowdyInterpolationTiming
{
	/**
	 * Fold one arrival into the history and answer how the segment it starts should be played.
	 *
	 * ArrivalSeconds is local time. The server's own timestamp is deliberately not used: the SDK
	 * treats it as unauthenticated, so it is forgeable, and local arrival is both unforgeable and the
	 * thing that actually governs whether motion looks smooth.
	 *
	 * A negative DeclaredDuration means the sender declared none. A declared one wins, but is still
	 * bounded, because it reaches here off the wire like everything else.
	 */
	CROWDYMASS_API FCrowdySegmentTiming OpenSegment(
		FCrowdyArrivalIntervalHistory& History,
		double& LastArrivalSeconds,
		double ArrivalSeconds,
		float DeclaredDuration,
		const FCrowdyInterpolationTuning& Tuning);
}
