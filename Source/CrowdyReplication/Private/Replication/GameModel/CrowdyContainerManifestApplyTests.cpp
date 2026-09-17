#include "Replication/GameModel/CrowdyContainerManifestApply.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyManifestApplyTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	TArray<FCrowdyContainerManifestRow> ManifestApplyTestRows(int32 Count)
	{
		TArray<FCrowdyContainerManifestRow> Rows;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			FCrowdyContainerManifestRow& Row = Rows.AddDefaulted_GetRef();
			Row.TypeName = TEXT("Node");
			Row.BindingKey = FString::Printf(TEXT("key%d"), Index);
		}
		return Rows;
	}

	// A sender that parks every completion until the test releases it, so the cap is observable.
	struct FManifestApplyDeferredSender
	{
		TArray<FCrowdyManifestApplyRunner::FOnRowDone> Parked;
		int32 Sent = 0;

		FCrowdyManifestApplyRunner::FSend Bind()
		{
			return [this](const FCrowdyContainerManifestRow&, FCrowdyManifestApplyRunner::FOnRowDone Done)
			{
				++Sent;
				Parked.Add(MoveTemp(Done));
			};
		}

		void ReleaseOne(bool bOk, bool bCreated)
		{
			FCrowdyManifestApplyRunner::FOnRowDone Done = MoveTemp(Parked[0]);
			Parked.RemoveAt(0);
			FCrowdyManifestApplyOutcome Outcome;
			Outcome.bOk = bOk;
			Outcome.bCreated = bCreated;
			Outcome.Code = bOk ? TEXT("") : TEXT("FORBIDDEN");
			Done(Outcome);
		}
	};
}

// At most MaxInFlight rows are out at once; each completion releases the next; the result counts every row once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyManifestApplyCapTest,
	"CrowdySDK.GameModel.ManifestApplyCap", CrowdyManifestApplyTestFlags)
bool FCrowdyManifestApplyCapTest::RunTest(const FString& Parameters)
{
	FManifestApplyDeferredSender Sender;
	int32 Completions = 0;
	FCrowdyApplyManifestResult Final;
	const TSharedPtr<FCrowdyManifestApplyRunner> Runner = MakeShared<FCrowdyManifestApplyRunner>(
		ManifestApplyTestRows(20), 8, Sender.Bind(), [&](const FCrowdyApplyManifestResult& Result)
	{
		++Completions;
		Final = Result;
	});
	Runner->Start();
	TestEqual(TEXT("eight rows in flight at the cap"), Sender.Sent, 8);
	TestEqual(TEXT("active matches"), Runner->GetActive(), 8);

	Sender.ReleaseOne(true, true);
	TestEqual(TEXT("a completion releases exactly one more"), Sender.Sent, 9);

	while (Sender.Parked.Num() > 0)
	{
		Sender.ReleaseOne(true, Sender.Sent % 2 == 0);
	}
	TestEqual(TEXT("completed once"), Completions, 1);
	TestTrue(TEXT("done"), Runner->IsDone());
	TestEqual(TEXT("every row was sent"), Sender.Sent, 20);
	TestEqual(TEXT("created + existing is every row"), Final.Created + Final.Existing, 20);
	TestEqual(TEXT("nothing failed"), Final.Failed, 0);
	TestEqual(TEXT("nothing unanswered"), Final.Unanswered, 0);
	TestTrue(TEXT("complete"), Final.IsComplete());
	return true;
}

// The first failure stops new sends; rows in flight still settle; the result names the failure and the unsent count.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyManifestApplyStopsAtFirstFailureTest,
	"CrowdySDK.GameModel.ManifestApplyStopsAtFirstFailure", CrowdyManifestApplyTestFlags)
bool FCrowdyManifestApplyStopsAtFirstFailureTest::RunTest(const FString& Parameters)
{
	FManifestApplyDeferredSender Sender;
	int32 Completions = 0;
	FCrowdyApplyManifestResult Final;
	const TSharedPtr<FCrowdyManifestApplyRunner> Runner = MakeShared<FCrowdyManifestApplyRunner>(
		ManifestApplyTestRows(20), 4, Sender.Bind(), [&](const FCrowdyApplyManifestResult& Result)
	{
		++Completions;
		Final = Result;
	});
	Runner->Start();
	Sender.ReleaseOne(true, true);
	Sender.ReleaseOne(false, false);
	TestEqual(TEXT("no new send after the failure"), Sender.Sent, 5);
	TestEqual(TEXT("not complete while rows are in flight"), Completions, 0);

	while (Sender.Parked.Num() > 0)
	{
		Sender.ReleaseOne(true, false);
	}
	TestEqual(TEXT("completed once"), Completions, 1);
	TestEqual(TEXT("one failure"), Final.Failed, 1);
	TestEqual(TEXT("the failure names its row"), Final.Failures.Num() == 1 ? Final.Failures[0].BindingKey : FString(), FString(TEXT("key1")));
	TestEqual(TEXT("the failure carries the code"), Final.Failures.Num() == 1 ? Final.Failures[0].Code : FString(), FString(TEXT("FORBIDDEN")));
	TestEqual(TEXT("fifteen rows never answered"), Final.Unanswered, 15);
	TestEqual(TEXT("the rest settled"), Final.Created + Final.Existing, 4);
	TestFalse(TEXT("not complete"), Final.IsComplete());
	return true;
}

// Abort completes at once with what was answered, counting the rows in flight as unsent; a late answer is ignored.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyManifestApplyAbortTest,
	"CrowdySDK.GameModel.ManifestApplyAbort", CrowdyManifestApplyTestFlags)
bool FCrowdyManifestApplyAbortTest::RunTest(const FString& Parameters)
{
	FManifestApplyDeferredSender Sender;
	int32 Completions = 0;
	FCrowdyApplyManifestResult Final;
	const TSharedPtr<FCrowdyManifestApplyRunner> Runner = MakeShared<FCrowdyManifestApplyRunner>(
		ManifestApplyTestRows(10), 3, Sender.Bind(), [&](const FCrowdyApplyManifestResult& Result)
	{
		++Completions;
		Final = Result;
	});
	Runner->Start();
	Sender.ReleaseOne(true, true);
	Runner->Abort(TEXT("shutdown"), TEXT("teardown"));
	TestEqual(TEXT("completed once"), Completions, 1);
	TestEqual(TEXT("one created"), Final.Created, 1);
	TestEqual(TEXT("three in flight plus six never started are unanswered"), Final.Unanswered, 9);
	TestEqual(TEXT("the abort is the one failure"), Final.Failed, 1);

	while (Sender.Parked.Num() > 0)
	{
		Sender.ReleaseOne(true, true);
	}
	TestEqual(TEXT("late answers change nothing"), Completions, 1);
	TestEqual(TEXT("no new sends after abort"), Sender.Sent, 4);
	TestEqual(TEXT("late answers do not touch the in-flight count"), Runner->GetActive(), 0);

	// A cap of zero is clamped to one, so it cannot hang.
	FManifestApplyDeferredSender ZeroSender;
	int32 ZeroCompletions = 0;
	const TSharedPtr<FCrowdyManifestApplyRunner> Zero = MakeShared<FCrowdyManifestApplyRunner>(
		ManifestApplyTestRows(2), 0, ZeroSender.Bind(), [&ZeroCompletions](const FCrowdyApplyManifestResult&) { ++ZeroCompletions; });
	Zero->Start();
	TestEqual(TEXT("a zero cap still sends one"), ZeroSender.Sent, 1);
	ZeroSender.ReleaseOne(true, true);
	ZeroSender.ReleaseOne(true, true);
	TestEqual(TEXT("and completes"), ZeroCompletions, 1);
	return true;
}

// A retryable refusal parks the row and resumes on the schedule; the allowance check parks the whole runner; a
// row refused past its retry limit ends the apply as a failure that says it is retryable.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyManifestApplyPacingTest,
	"CrowdySDK.GameModel.ManifestApplyPacing", CrowdyManifestApplyTestFlags)
bool FCrowdyManifestApplyPacingTest::RunTest(const FString& Parameters)
{
	FManifestApplyDeferredSender Sender;
	TArray<TFunction<void()>> Scheduled;
	TArray<float> ScheduledSeconds;
	bool bAllowanceOpen = true;
	int32 Completions = 0;
	FCrowdyApplyManifestResult Final;
	const TSharedPtr<FCrowdyManifestApplyRunner> Runner = MakeShared<FCrowdyManifestApplyRunner>(
		ManifestApplyTestRows(3), 8, Sender.Bind(), [&](const FCrowdyApplyManifestResult& Result)
	{
		++Completions;
		Final = Result;
	});
	Runner->SetPacing([&bAllowanceOpen]() { return bAllowanceOpen; },
		[&](float Seconds, TFunction<void()> Fn) { ScheduledSeconds.Add(Seconds); Scheduled.Add(MoveTemp(Fn)); });

	bAllowanceOpen = false;
	Runner->Start();
	TestEqual(TEXT("a closed allowance sends nothing"), Sender.Sent, 0);
	TestEqual(TEXT("and parks on the schedule"), Scheduled.Num(), 1);

	bAllowanceOpen = true;
	Scheduled.Pop()();
	TestEqual(TEXT("the resume sends every row"), Sender.Sent, 3);

	// Row 0 is rate limited: it goes back to the queue with backoff, the others complete.
	FCrowdyManifestApplyOutcome Limited;
	Limited.bRetryable = true;
	Limited.Code = TEXT("RATE_LIMITED");
	{
		FCrowdyManifestApplyRunner::FOnRowDone Done = MoveTemp(Sender.Parked[0]);
		Sender.Parked.RemoveAt(0);
		Done(Limited);
	}
	Sender.ReleaseOne(true, true);
	Sender.ReleaseOne(true, false);
	TestEqual(TEXT("not complete while a row waits to retry"), Completions, 0);
	TestEqual(TEXT("one retry scheduled"), Scheduled.Num(), 1);
	TestEqual(TEXT("the first retry waits one second"), ScheduledSeconds.Last(), 1.0f);

	Scheduled.Pop()();
	TestEqual(TEXT("the retry resends the row"), Sender.Sent, 4);
	Sender.ReleaseOne(true, true);
	TestEqual(TEXT("completed once"), Completions, 1);
	TestEqual(TEXT("three answered rows"), Final.Created + Final.Existing, 3);
	TestTrue(TEXT("complete"), Final.IsComplete());

	// Past the retry limit the row fails, and the failure is marked retryable for the caller.
	FManifestApplyDeferredSender Sender2;
	TArray<TFunction<void()>> Scheduled2;
	int32 Completions2 = 0;
	FCrowdyApplyManifestResult Final2;
	const TSharedPtr<FCrowdyManifestApplyRunner> Runner2 = MakeShared<FCrowdyManifestApplyRunner>(
		ManifestApplyTestRows(1), 8, Sender2.Bind(), [&](const FCrowdyApplyManifestResult& Result)
	{
		++Completions2;
		Final2 = Result;
	});
	Runner2->SetPacing([]() { return true; }, [&Scheduled2](float, TFunction<void()> Fn) { Scheduled2.Add(MoveTemp(Fn)); });
	Runner2->Start();
	for (int32 Attempt = 0; Attempt <= FCrowdyManifestApplyRunner::MaxRetriesPerRow; ++Attempt)
	{
		FCrowdyManifestApplyRunner::FOnRowDone Done = MoveTemp(Sender2.Parked[0]);
		Sender2.Parked.RemoveAt(0);
		Done(Limited);
		if (Scheduled2.Num() > 0)
		{
			Scheduled2.Pop()();
		}
	}
	TestEqual(TEXT("sent once plus every retry"), Sender2.Sent, FCrowdyManifestApplyRunner::MaxRetriesPerRow + 1);
	TestEqual(TEXT("completed once"), Completions2, 1);
	TestEqual(TEXT("one failure"), Final2.Failed, 1);
	TestTrue(TEXT("the failure is retryable"), Final2.Failures.Num() == 1 && Final2.Failures[0].bRetryable);
	return true;
}

// A sender that answers synchronously drives the whole apply from Start, and an empty manifest completes at once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyManifestApplySynchronousTest,
	"CrowdySDK.GameModel.ManifestApplySynchronous", CrowdyManifestApplyTestFlags)
bool FCrowdyManifestApplySynchronousTest::RunTest(const FString& Parameters)
{
	int32 Sent = 0;
	int32 Completions = 0;
	FCrowdyApplyManifestResult Final;
	TSharedPtr<FCrowdyManifestApplyRunner> Runner = MakeShared<FCrowdyManifestApplyRunner>(
		ManifestApplyTestRows(30), 8,
		[&Sent](const FCrowdyContainerManifestRow&, FCrowdyManifestApplyRunner::FOnRowDone Done)
		{
			++Sent;
			FCrowdyManifestApplyOutcome Outcome;
			Outcome.bOk = true;
			Outcome.bCreated = true;
			Done(Outcome);
		},
		[&](const FCrowdyApplyManifestResult& Result)
		{
			++Completions;
			Final = Result;
			// The completion may be the last holder of the runner elsewhere; here it is not, but Pump must survive it.
		});
	Runner->Start();
	TestEqual(TEXT("every row sent"), Sent, 30);
	TestEqual(TEXT("completed once"), Completions, 1);
	TestEqual(TEXT("all created"), Final.Created, 30);

	int32 EmptyCompletions = 0;
	const TSharedPtr<FCrowdyManifestApplyRunner> Empty = MakeShared<FCrowdyManifestApplyRunner>(
		TArray<FCrowdyContainerManifestRow>(), 8,
		[](const FCrowdyContainerManifestRow&, FCrowdyManifestApplyRunner::FOnRowDone) {},
		[&EmptyCompletions](const FCrowdyApplyManifestResult& Result) { ++EmptyCompletions; });
	Empty->Start();
	TestEqual(TEXT("an empty manifest completes at once"), EmptyCompletions, 1);
	return true;
}

#endif
