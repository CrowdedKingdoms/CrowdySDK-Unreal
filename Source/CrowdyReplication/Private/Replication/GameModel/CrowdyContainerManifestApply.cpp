#include "Replication/GameModel/CrowdyContainerManifestApply.h"

FCrowdyManifestApplyRunner::FCrowdyManifestApplyRunner(TArray<FCrowdyContainerManifestRow> InRows, int32 InMaxInFlight,
	FSend InSend, FOnDone InOnDone)
	: Rows(MoveTemp(InRows))
	, MaxInFlight(FMath::Max(1, InMaxInFlight))
	, Send(MoveTemp(InSend))
	, OnDone(MoveTemp(InOnDone))
{
	RetriesByRow.SetNumZeroed(Rows.Num());
}

void FCrowdyManifestApplyRunner::SetPacing(FCanSend InCanSend, FSchedule InSchedule)
{
	CanSend = MoveTemp(InCanSend);
	Schedule = MoveTemp(InSchedule);
}

float FCrowdyManifestApplyRunner::RetryDelaySeconds(int32 RetryIndex)
{
	return FMath::Min(static_cast<float>(1 << FMath::Clamp(RetryIndex, 0, 4)), 11.0f);
}

void FCrowdyManifestApplyRunner::Start()
{
	if (bDone)
	{
		return;
	}
	if (Rows.Num() == 0)
	{
		Finish();
		return;
	}
	Pump();
}

void FCrowdyManifestApplyRunner::Abort(const FString& Code, const FString& Message)
{
	if (bDone)
	{
		return;
	}
	bStopped = true;
	// Nobody this runner still listens to will answer the rows in flight; they were sent, so an ensure may have
	// created them, and the honest count is unanswered rather than unsent.
	Result.Unanswered += Active;
	Active = 0;
	FCrowdyManifestRowFailure& Failure = Result.Failures.AddDefaulted_GetRef();
	Failure.Code = Code;
	Failure.Message = Message;
	Failure.bRetryable = true;
	++Result.Failed;
	Finish();
}

void FCrowdyManifestApplyRunner::Pump()
{
	// A send may complete synchronously and reach Finish, whose completion may drop the last outside reference,
	// so the pin keeps this alive until the loop returns; the flag flattens that re-entry into another lap.
	const TSharedPtr<FCrowdyManifestApplyRunner> Pin = AsShared();
	if (bPumping)
	{
		bPumpRequested = true;
		return;
	}
	bPumping = true;
	do
	{
		bPumpRequested = false;
		while (!bStopped && !bDone && !bWaiting && Active < MaxInFlight && (RetryQueue.Num() > 0 || Cursor < Rows.Num()))
		{
			// Pacing: with the allowance spent, park until the timer says try again rather than earn a refusal.
			if (CanSend && Schedule && !CanSend())
			{
				ScheduleResume(PaceWaitSeconds);
				break;
			}
			const int32 RowIndex = RetryQueue.Num() > 0 ? RetryQueue.Pop(EAllowShrinking::No) : Cursor++;
			++Active;
			TWeakPtr<FCrowdyManifestApplyRunner> WeakThis = AsShared();
			Send(Rows[RowIndex], [WeakThis, RowIndex](const FCrowdyManifestApplyOutcome& Outcome)
			{
				if (const TSharedPtr<FCrowdyManifestApplyRunner> Self = WeakThis.Pin())
				{
					Self->Settle(RowIndex, Outcome);
				}
			});
		}
	}
	while (bPumpRequested && !bDone);
	bPumping = false;

	if (!bDone && Active == 0 && !bWaiting && (bStopped || (RetryQueue.Num() == 0 && Cursor >= Rows.Num())))
	{
		Finish();
	}
}

void FCrowdyManifestApplyRunner::Settle(int32 RowIndex, const FCrowdyManifestApplyOutcome& Outcome)
{
	if (bDone)
	{
		return;
	}
	--Active;
	if (Outcome.bOk)
	{
		Outcome.bCreated ? ++Result.Created : ++Result.Existing;
		Pump();
		return;
	}
	// A retryable refusal committed nothing (the allowance gate sits before the ensure runs, and an existing row is
	// a read), so the row goes back to the queue and the runner waits before its next lap.
	if (Outcome.bRetryable && Schedule && RetriesByRow[RowIndex] < MaxRetriesPerRow && !bStopped)
	{
		const int32 Retry = RetriesByRow[RowIndex]++;
		RetryQueue.Add(RowIndex);
		ScheduleResume(RetryDelaySeconds(Retry));
		return;
	}
	++Result.Failed;
	bStopped = true;
	FCrowdyManifestRowFailure& Failure = Result.Failures.AddDefaulted_GetRef();
	Failure.TypeName = Rows[RowIndex].TypeName;
	Failure.BindingKey = Rows[RowIndex].BindingKey;
	Failure.Code = Outcome.Code;
	Failure.Message = Outcome.Message;
	Failure.bRetryable = Outcome.bRetryable;
	Pump();
}

void FCrowdyManifestApplyRunner::ScheduleResume(float Seconds)
{
	if (bWaiting)
	{
		return;
	}
	bWaiting = true;
	TWeakPtr<FCrowdyManifestApplyRunner> WeakThis = AsShared();
	Schedule(Seconds, [WeakThis]()
	{
		if (const TSharedPtr<FCrowdyManifestApplyRunner> Self = WeakThis.Pin())
		{
			Self->bWaiting = false;
			Self->Pump();
		}
	});
}

void FCrowdyManifestApplyRunner::Finish()
{
	bDone = true;
	Result.Unanswered += (Rows.Num() - Cursor) + RetryQueue.Num();
	RetryQueue.Reset();
	// Moved out first: the callback may release the last reference to this runner.
	const FOnDone Done = MoveTemp(OnDone);
	const FCrowdyApplyManifestResult Final = Result;
	if (Done)
	{
		Done(Final);
	}
}
