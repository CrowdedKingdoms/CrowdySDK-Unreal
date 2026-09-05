#pragma once

#include "CoreMinimal.h"

/** One reading of what the inbound receive drain cost, and what a drain window buys at that cost. */
struct FCrowdyDrainCost
{
	int64 Messages = 0;
	double Seconds = 0.0;
	double MicrosecondsPerMessage = 0.0;

	/**
	 * Drains that delivered something, so a reader can see how close one gets to the allowance.
	 *
	 * Without it the cost alone cannot say whether the allowance is being reached, and the two figures a run
	 * reports today, delivered per second and drains that spent their whole allowance, have disagreed by a fifth.
	 */
	int64 Drains = 0;
	double MessagesPerDrain = 0.0;

	/** How many messages the window pays for at this cost. Zero when nothing has been measured yet. */
	int32 MessagesAffordedByWindow = 0;
};

/**
 * What the receive drain spent, accumulated so that crowdy.net.receive.maxmessages is derived from a measurement.
 *
 * It counts the whole drain rather than the scopes inside delivery, which cover about two thirds of it, and it is
 * the only reading that does.
 */
struct FCrowdyDrainCostMeter
{
	/** A drain that delivered nothing is poll overhead rather than a per-message cost, so it is left out. */
	void Record(const int32 Messages, const double Seconds)
	{
		if (Messages <= 0 || Seconds < 0.0)
		{
			return;
		}

		RecordedMessages += Messages;
		RecordedSeconds += Seconds;
		++RecordedDrains;
	}

	bool HasReading() const { return RecordedMessages > 0 && RecordedSeconds > 0.0; }

	FCrowdyDrainCost Read(const double WindowSeconds) const
	{
		FCrowdyDrainCost Cost;
		if (!HasReading())
		{
			return Cost;
		}

		Cost.Messages = RecordedMessages;
		Cost.Seconds = RecordedSeconds;
		Cost.Drains = RecordedDrains;
		Cost.MicrosecondsPerMessage = RecordedSeconds * 1000000.0 / static_cast<double>(RecordedMessages);
		Cost.MessagesPerDrain = static_cast<double>(RecordedMessages) / static_cast<double>(RecordedDrains);

		if (WindowSeconds <= 0.0)
		{
			return Cost;
		}

		// As a ratio rather than through the per-message figure, so the affordable count does not inherit that
		// division's rounding.
		const double Afforded = FMath::FloorToDouble(WindowSeconds * static_cast<double>(RecordedMessages) / RecordedSeconds);
		Cost.MessagesAffordedByWindow = static_cast<int32>(FMath::Clamp(Afforded, 0.0, static_cast<double>(MAX_int32)));
		return Cost;
	}

	/** Start a fresh window, so a reading describes an interval rather than the whole run and its warm-up. */
	void Reset()
	{
		RecordedMessages = 0;
		RecordedSeconds = 0.0;
		RecordedDrains = 0;
	}

private:
	int64 RecordedMessages = 0;
	double RecordedSeconds = 0.0;
	int64 RecordedDrains = 0;
};
