#pragma once

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "CoreGlobals.h"
#include "CrowdyMassLog.h"
#include "Misc/OutputDeviceRedirector.h"

/**
 * Collects LogCrowdyMass output for as long as it is in scope, keeping the verbosity beside each line so a
 * case can ask for the warnings and the verdict separately.
 *
 * The lines themselves are the deliverable, so cases read them rather than a counter kept alongside, which
 * would still read green if the line were dropped. Reading the lines is also the only way to assert that a
 * particular line was NOT written: the automation framework's expected-message list can require a message,
 * but its zero-occurrence form means "one or more" rather than "never".
 *
 * One home for the cases that need it, because a second copy in another anonymous namespace in this module
 * would collide with the first under a unity build.
 */
class FCrowdyMassStateLogCapture : public FOutputDevice
{
public:

	FCrowdyMassStateLogCapture()
	{
		GLog->AddOutputDevice(this);
	}

	virtual ~FCrowdyMassStateLogCapture()
	{
		GLog->RemoveOutputDevice(this);
	}

	virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override
	{
		if (Category != LogCrowdyMass.GetCategoryName())
		{
			return;
		}

		FScopeLock Lock(&Mutex);
		Lines.Add(FCapturedLine{Verbosity, Message});
	}

	virtual bool CanBeUsedOnMultipleThreads() const override
	{
		return true;
	}

	// Warnings since the last take. Taking clears, so consecutive calls read consecutive stretches of the
	// run rather than the whole of it, which is what lets a case say "and then it was silent".
	TArray<FString> TakeWarnings()
	{
		return Take(ELogVerbosity::Warning);
	}

	TArray<FString> TakeLogs()
	{
		return Take(ELogVerbosity::Log);
	}

private:

	struct FCapturedLine
	{
		ELogVerbosity::Type Verbosity;
		FString Text;
	};

	TArray<FString> Take(const ELogVerbosity::Type Wanted)
	{
		GLog->FlushThreadedLogs();

		FScopeLock Lock(&Mutex);
		TArray<FString> Taken;
		for (int32 Index = Lines.Num() - 1; Index >= 0; --Index)
		{
			if (Lines[Index].Verbosity != Wanted)
			{
				continue;
			}

			Taken.Insert(Lines[Index].Text, 0);
			Lines.RemoveAt(Index);
		}
		return Taken;
	}

	FCriticalSection Mutex;
	TArray<FCapturedLine> Lines;
};

inline int32 CrowdyMassCountLinesContaining(const TArray<FString>& Lines, const FString& Needle)
{
	int32 Count = 0;
	for (const FString& Line : Lines)
	{
		Count += Line.Contains(Needle) ? 1 : 0;
	}
	return Count;
}

#endif // WITH_DEV_AUTOMATION_TESTS
