#include "Interpolation/CrowdyInterpolationTiming.h"

namespace
{
	// Bounds are authored, but they reach here through config and can be authored backwards, in which
	// case a clamp between them would silently return whichever the caller happened to pass second.
	float LowerBound(const FCrowdyInterpolationTuning& Tuning)
	{
		return FMath::Max(0.0f, FMath::Min(Tuning.MinDuration, Tuning.MaxDuration));
	}

	float UpperBound(const FCrowdyInterpolationTuning& Tuning)
	{
		return FMath::Max(LowerBound(Tuning), FMath::Max(Tuning.MinDuration, Tuning.MaxDuration));
	}

	float ClampToTuning(const float Seconds, const FCrowdyInterpolationTuning& Tuning)
	{
		return FMath::Clamp(Seconds, LowerBound(Tuning), UpperBound(Tuning));
	}

	// A non-finite value survives every arithmetic step and defeats every comparison, so one that
	// reached the history would sit in it for the next few updates and pull the median with it. Timing
	// is derived from values that arrive over the wire, so this is reachable from a forged packet.
	bool IsUsableInterval(const float IntervalSeconds)
	{
		return FMath::IsFinite(IntervalSeconds) && IntervalSeconds >= 0.0f;
	}
}

void FCrowdyArrivalIntervalHistory::Record(const float IntervalSeconds)
{
	if (!IsUsableInterval(IntervalSeconds))
		return;

	Samples[NextSlot] = IntervalSeconds;
	NextSlot = static_cast<uint8>((NextSlot + 1) % SampleCount);
	Count = static_cast<uint8>(FMath::Min<int32>(Count + 1, SampleCount));
}

float FCrowdyArrivalIntervalHistory::PlanningInterval() const
{
	if (Count == 0)
		return 0.0f;

	float Sorted[SampleCount];
	for (int32 Index = 0; Index < Count; ++Index)
	{
		Sorted[Index] = Samples[Index];
	}

	// Five elements at most, so an insertion sort beats anything with a comparator behind it.
	for (int32 Index = 1; Index < Count; ++Index)
	{
		const float Value = Sorted[Index];
		int32 Position = Index - 1;
		while (Position >= 0 && Sorted[Position] > Value)
		{
			Sorted[Position + 1] = Sorted[Position];
			--Position;
		}
		Sorted[Position + 1] = Value;
	}

	// Second largest once there are enough samples to have a second largest, so the single worst sample
	// is discarded and everything below it is still covered.
	//
	// With only one or two samples it takes the LARGEST instead. That asymmetry is deliberate: while
	// little is known about the connection, erring towards a longer segment costs a few milliseconds of
	// extra lag, and erring towards a shorter one costs a visible stall. Only one of those is worth
	// risking on a guess.
	const int32 Index = Count <= 2 ? Count - 1 : Count - 2;
	return Sorted[Index];
}

void FCrowdyArrivalIntervalHistory::Reset()
{
	for (float& Sample : Samples)
	{
		Sample = 0.0f;
	}

	Count = 0;
	NextSlot = 0;
}

FCrowdySegmentTiming CrowdyInterpolationTiming::OpenSegment(
	FCrowdyArrivalIntervalHistory& History,
	double& LastArrivalSeconds,
	const double ArrivalSeconds,
	const float DeclaredDuration,
	const FCrowdyInterpolationTuning& Tuning)
{
	// Recorded before any early return, so that a stretch of sender-declared durations still leaves a
	// usable history behind for the moment the sender stops declaring them.
	if (LastArrivalSeconds > 0.0 && ArrivalSeconds >= LastArrivalSeconds)
	{
		History.Record(static_cast<float>(ArrivalSeconds - LastArrivalSeconds));
	}

	LastArrivalSeconds = ArrivalSeconds;

	FCrowdySegmentTiming Timing;

	if (DeclaredDuration >= 0.0f)
	{
		// Bounded from above only. A duration longer than the ceiling would leave the entity gliding
		// for that long, or standing frozen mid-stride if the sender never speaks again, so it is
		// refused. A very short one is a request to snap, which is a reasonable thing for a sender to
		// want on a teleport and harms nobody, so the floor that protects the measured estimate from
		// a burst is not applied to a duration somebody asked for on purpose.
		Timing.Duration = FMath::Min(DeclaredDuration, UpperBound(Tuning));

		// A sender that names its own duration is describing how it wants the segment played, not how
		// fast it is moving, so nothing here can say the two differ.
		Timing.VelocityScale = 1.0f;
		return Timing;
	}

	const float TypicalInterval = History.PlanningInterval();

	if (!Tuning.bAdaptive || TypicalInterval <= 0.0f)
	{
		// Authored rather than received, so it is taken as given beyond refusing a value that would
		// make the segment play backwards or not at all.
		Timing.Duration = FMath::IsFinite(Tuning.FirstUpdateDuration) ? FMath::Max(0.0f, Tuning.FirstUpdateDuration) : 0.0f;
		Timing.VelocityScale = 1.0f;
		return Timing;
	}

	Timing.Duration = ClampToTuning(TypicalInterval * FMath::Max(0.0f, Tuning.Overshoot), Tuning);

	// Taken from the duration finally used rather than from the overshoot, so that a duration the
	// bounds moved still reports the speed the entity will genuinely appear to travel at.
	Timing.VelocityScale = Timing.Duration / TypicalInterval;

	return Timing;
}
