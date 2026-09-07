// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "GameModel/CrowdyGameModelDelete.h"
#include "GameModel/CrowdySchemaSync.h"
#include "GameModel/CrowdyStudioFunctionMarshalling.h"
#include "Model/CrowdyStudioControllerTestAccess.h"
#include "Model/CrowdyStudioTypes.h"
#include "Model/FCrowdyStudioController.h"

// The controller half of the Models browser's delete surface: which calls are refused before anything is issued, what
// a stopped commit leaves behind, and what the pre-flight's scoped read is allowed to touch.
//
// A default-constructed controller has no session sign-in, so every game-plane operation fails closed inside the call
// that issues it, with its own status message. That is what makes an asynchronous surface observable here: a call that
// got past its guards leaves the sign-in message, and a call that was refused leaves the refusal instead. The two
// messages are different, which is what shows a refusal came from the check under test rather than from a guard the
// two calls share.
namespace
{
	constexpr EAutomationTestFlags CrowdyStudioDeleteRoutingTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// The sentence every app-mismatch refusal on this surface contains. The wording around it differs per entity, and
	// deliberately so, but this much is the shared claim being asserted.
	const TCHAR* CrowdyDeleteRoutingWrongAppPhrase = TEXT("different app");

	// Evidence complete enough for a plan to be judged: the three lists read, so an empty one means "this app has
	// none" rather than "nobody asked".
	FCrowdyDeleteEvidence CrowdyDeleteRoutingEvidence(int64 AppId)
	{
		FCrowdyDeleteEvidence Evidence;
		Evidence.AppId = AppId;
		Evidence.bTypesRead = true;
		Evidence.bFunctionsRead = true;
		Evidence.bAutomationsRead = true;
		return Evidence;
	}

	// Automations are the one kind that carries no blocker of its own: nothing refuses their delete and nothing here
	// declares them, so a plan built from them is committable and the walk itself is what is under test.
	TArray<FCrowdyDeleteMark> CrowdyDeleteRoutingAutomationMarks(int32 Count)
	{
		TArray<FCrowdyDeleteMark> Marks;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FString Name = FString::Printf(TEXT("automation_%02d"), Index);
			Marks.Add(CrowdyGameModelDelete::MarkAutomation(Name, Name));
		}
		return Marks;
	}

	FStudioFunction CrowdyDeleteRoutingServerFunction(const FString& Name, const FString& ContainerTypeName)
	{
		FStudioFunction Function;
		Function.Name = Name;
		Function.ContainerTypeName = ContainerTypeName;
		return Function;
	}

	// Two operations are the same operation when they name the same mutation with the same arguments in the same
	// order. Case-sensitive throughout: every one of these values is a server key.
	bool CrowdyDeleteRoutingOpsMatch(const FCrowdyDeleteOp& A, const FCrowdyDeleteOp& B)
	{
		if (A.Kind != B.Kind
			|| A.bImplied != B.bImplied
			|| !A.OperationName.Equals(B.OperationName, ESearchCase::CaseSensitive)
			|| !A.ResultField.Equals(B.ResultField, ESearchCase::CaseSensitive)
			|| A.StringArgs.Num() != B.StringArgs.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < A.StringArgs.Num(); ++Index)
		{
			if (!A.StringArgs[Index].Key.Equals(B.StringArgs[Index].Key, ESearchCase::CaseSensitive)
				|| !A.StringArgs[Index].Value.Equals(B.StringArgs[Index].Value, ESearchCase::CaseSensitive))
			{
				return false;
			}
		}
		return true;
	}

	bool CrowdyDeleteRoutingHasArg(const FCrowdyDeleteOp& Op, const TCHAR* Key)
	{
		return Op.StringArgs.ContainsByPredicate([Key](const TPair<FString, FString>& Arg)
			{ return Arg.Key.Equals(Key, ESearchCase::CaseSensitive); });
	}
}

// Every destructive entry point names the app its caller read the entity from, and goes out only if that app is still
// the selected one. A model, an attribute and an automation all carry names that a development and a production app
// share, because both were deployed from the same assets, so a delete aimed by name alone lands on whichever app is
// selected at the moment of the click. There is no undo.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioDeleteRefusesAcrossAppsTest,
	"CrowdySDK.CrowdyStudio.DeleteRefusesAcrossApps", CrowdyStudioDeleteRoutingTestFlags)

bool FCrowdyStudioDeleteRefusesAcrossAppsTest::RunTest(const FString& /*Parameters*/)
{
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedApp(*Controller, 200);

	// Each delete is called twice: once naming another app, once naming the selected one. The first must be refused by
	// the app check; the second must get past it and be stopped only by the missing sign-in, which says something
	// else. Without the second call an early return shared by both would look exactly like the check working.
	Controller->DeleteContainerType(TEXT("Knight"), /*ExpectedAppId*/ 100);
	TestTrue(TEXT("Deleting a model listed for another app is refused"),
		Controller->GetStatusMessage().Contains(CrowdyDeleteRoutingWrongAppPhrase));
	Controller->DeleteContainerType(TEXT("Knight"), /*ExpectedAppId*/ 200);
	TestFalse(TEXT("Deleting a model of the selected app is not refused by the app check"),
		Controller->GetStatusMessage().Contains(CrowdyDeleteRoutingWrongAppPhrase));

	Controller->DeletePropertyDef(TEXT("Knight"), TEXT("hp"), /*ExpectedAppId*/ 100);
	TestTrue(TEXT("Deleting an attribute listed for another app is refused"),
		Controller->GetStatusMessage().Contains(CrowdyDeleteRoutingWrongAppPhrase));
	Controller->DeletePropertyDef(TEXT("Knight"), TEXT("hp"), /*ExpectedAppId*/ 200);
	TestFalse(TEXT("Deleting an attribute of the selected app is not refused by the app check"),
		Controller->GetStatusMessage().Contains(CrowdyDeleteRoutingWrongAppPhrase));

	Controller->DeleteAutomation(TEXT("regen_tick"), /*ExpectedAppId*/ 100);
	TestTrue(TEXT("Deleting an automation listed for another app is refused"),
		Controller->GetStatusMessage().Contains(CrowdyDeleteRoutingWrongAppPhrase));
	Controller->DeleteAutomation(TEXT("regen_tick"), /*ExpectedAppId*/ 200);
	TestFalse(TEXT("Deleting an automation of the selected app is not refused by the app check"),
		Controller->GetStatusMessage().Contains(CrowdyDeleteRoutingWrongAppPhrase));

	Controller->DeleteFunction(TEXT("Knight"), TEXT("regen"), /*ExpectedAppId*/ 100);
	TestTrue(TEXT("Deleting a function listed for another app is refused"),
		Controller->GetStatusMessage().Contains(CrowdyDeleteRoutingWrongAppPhrase));
	Controller->DeleteFunction(TEXT("Knight"), TEXT("regen"), /*ExpectedAppId*/ 200);
	TestFalse(TEXT("Deleting a function of the selected app is not refused by the app check"),
		Controller->GetStatusMessage().Contains(CrowdyDeleteRoutingWrongAppPhrase));

	Controller->DeleteContainer(TEXT("container-1"), /*ExpectedAppId*/ 100);
	TestTrue(TEXT("Deleting a live model listed for another app is refused"),
		Controller->GetStatusMessage().Contains(CrowdyDeleteRoutingWrongAppPhrase));
	Controller->DeleteContainer(TEXT("container-1"), /*ExpectedAppId*/ 200);
	TestFalse(TEXT("Deleting a live model of the selected app is not refused by the app check"),
		Controller->GetStatusMessage().Contains(CrowdyDeleteRoutingWrongAppPhrase));

	return true;
}

// The three save paths carry the same parameter for the same reason. An editor is filled from one app and the user can
// switch app with it still open, so a save aimed at the selection writes one app's authoring into another, and an
// upsert is create-or-update: it will happily invent the entity in the app that was never meant to have it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioSchemaSaveRefusesAcrossAppsTest,
	"CrowdySDK.CrowdyStudio.SchemaSaveRefusesAcrossApps", CrowdyStudioDeleteRoutingTestFlags)

bool FCrowdyStudioSchemaSaveRefusesAcrossAppsTest::RunTest(const FString& /*Parameters*/)
{
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedApp(*Controller, 200);

	Controller->UpsertContainerType(TEXT("Knight"), TEXT("Knight"), FString(), FString(), FString(),
		/*ExpectedAppId*/ 100);
	TestTrue(TEXT("Saving a model loaded from another app is refused"),
		Controller->GetStatusMessage().Contains(CrowdyDeleteRoutingWrongAppPhrase));
	Controller->UpsertContainerType(TEXT("Knight"), TEXT("Knight"), FString(), FString(), FString(),
		/*ExpectedAppId*/ 200);
	TestFalse(TEXT("Saving a model of the selected app is not refused by the app check"),
		Controller->GetStatusMessage().Contains(CrowdyDeleteRoutingWrongAppPhrase));

	Controller->UpsertPropertyDef(TEXT("Knight"), TEXT("hp"), TEXT("int"), FString(), FString(), FString(), FString(),
		/*ExpectedAppId*/ 100);
	TestTrue(TEXT("Saving an attribute loaded from another app is refused"),
		Controller->GetStatusMessage().Contains(CrowdyDeleteRoutingWrongAppPhrase));
	Controller->UpsertPropertyDef(TEXT("Knight"), TEXT("hp"), TEXT("int"), FString(), FString(), FString(), FString(),
		/*ExpectedAppId*/ 200);
	TestFalse(TEXT("Saving an attribute of the selected app is not refused by the app check"),
		Controller->GetStatusMessage().Contains(CrowdyDeleteRoutingWrongAppPhrase));

	Controller->UpsertFunction(TEXT("regen"), TEXT("Knight"), FString(), FString(), FString(), FString(), FString(),
		FString(), FString(), /*ExpectedAppId*/ 100);
	TestTrue(TEXT("Saving a function loaded from another app is refused"),
		Controller->GetStatusMessage().Contains(CrowdyDeleteRoutingWrongAppPhrase));
	Controller->UpsertFunction(TEXT("regen"), TEXT("Knight"), FString(), FString(), FString(), FString(), FString(),
		FString(), FString(), /*ExpectedAppId*/ 200);
	TestFalse(TEXT("Saving a function of the selected app is not refused by the app check"),
		Controller->GetStatusMessage().Contains(CrowdyDeleteRoutingWrongAppPhrase));

	return true;
}

// The staged commit compares two sources against the selection: the app the plan's evidence was gathered for, and the
// app the widget that drew the sheet believes it is showing. Comparing either one to itself would agree however stale
// the sheet had become. A refusal also has to announce, because the review continues from the finished signal and one
// that never arrives leaves the page saying a delete is under way forever.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioDeleteCommitRefusesUnlessBothAppsMatchTest,
	"CrowdySDK.CrowdyStudio.DeleteCommitRefusesUnlessBothAppsMatch", CrowdyStudioDeleteRoutingTestFlags)

bool FCrowdyStudioDeleteCommitRefusesUnlessBothAppsMatchTest::RunTest(const FString& /*Parameters*/)
{
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedApp(*Controller, 100);

	int32 Finished = 0;
	Controller->OnDeleteCommitFinished.AddLambda([&Finished]() { ++Finished; });

	const FCrowdyDeletePlan PlanForThisApp = CrowdyGameModelDelete::BuildPlan(
		CrowdyDeleteRoutingAutomationMarks(2), CrowdyDeleteRoutingEvidence(100));
	if (!TestTrue(TEXT("A plan of two automations is committable to begin with"), PlanForThisApp.bCommittable))
	{
		return false;
	}

	// The widget disagrees with the selection.
	Controller->CommitDeletePlan(PlanForThisApp, /*ExpectedAppId*/ 200);
	TestTrue(TEXT("A commit whose caller names another app is refused"),
		Controller->GetStatusMessage().Contains(CrowdyDeleteRoutingWrongAppPhrase));
	TestEqual(TEXT("The refusal announces, so nothing is left waiting on it"), Finished, 1);
	TestEqual(TEXT("A refused commit leaves no remainder"), Controller->GetDeleteRemainder().Num(), 0);
	TestEqual(TEXT("A refused commit leaves no outcome to report"),
		Controller->GetLastDeleteOutcome().Total, 0);

	// The plan's own evidence disagrees with the selection.
	const FCrowdyDeletePlan PlanForAnotherApp = CrowdyGameModelDelete::BuildPlan(
		CrowdyDeleteRoutingAutomationMarks(2), CrowdyDeleteRoutingEvidence(999));
	Controller->CommitDeletePlan(PlanForAnotherApp, /*ExpectedAppId*/ 100);
	TestTrue(TEXT("A commit whose plan was built for another app is refused"),
		Controller->GetStatusMessage().Contains(CrowdyDeleteRoutingWrongAppPhrase));
	TestEqual(TEXT("That refusal announces too"), Finished, 2);

	// A plan holding a blocker is refused here as well as in the sheet, so the rule holds away from the widget that
	// renders it. Nothing has counted this model's live models, so nothing can say whether the server will refuse.
	TArray<FCrowdyDeleteMark> ModelMark;
	ModelMark.Add(CrowdyGameModelDelete::MarkModel(TEXT("Knight"), TEXT("Knight")));
	const FCrowdyDeletePlan BlockedPlan = CrowdyGameModelDelete::BuildPlan(ModelMark, CrowdyDeleteRoutingEvidence(100));
	if (TestFalse(TEXT("A model with no live count is not committable"), BlockedPlan.bCommittable))
	{
		Controller->CommitDeletePlan(BlockedPlan, /*ExpectedAppId*/ 100);
		TestTrue(TEXT("A blocked plan is refused by the controller too"),
			Controller->GetStatusMessage().Contains(TEXT("blocking")));
		TestEqual(TEXT("A blocked commit announces"), Finished, 3);
		TestEqual(TEXT("A blocked commit issues nothing and leaves no outcome"),
			Controller->GetLastDeleteOutcome().Total, 0);
	}

	TestFalse(TEXT("Nothing is running after three refusals"), Controller->IsDeleteCommitInFlight());

	return true;
}

// The walk is sequential by requirement, not by convenience: a later operation can depend on an earlier one, since the
// server refuses a model delete while a function is still bound to it. So a failure stops the walk where it happened
// rather than skipping ahead, and the remainder starts AT that operation, which did not complete.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioDeleteWalkStopsAtTheFirstFailureTest,
	"CrowdySDK.CrowdyStudio.DeleteWalkStopsAtTheFirstFailure", CrowdyStudioDeleteRoutingTestFlags)

bool FCrowdyStudioDeleteWalkStopsAtTheFirstFailureTest::RunTest(const FString& /*Parameters*/)
{
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedApp(*Controller, 100);

	int32 Finished = 0;
	int32 Progress = 0;
	Controller->OnDeleteCommitFinished.AddLambda([&Finished]() { ++Finished; });
	Controller->OnDeleteCommitProgress.AddLambda([&Progress]() { ++Progress; });

	const FCrowdyDeletePlan Plan = CrowdyGameModelDelete::BuildPlan(
		CrowdyDeleteRoutingAutomationMarks(3), CrowdyDeleteRoutingEvidence(100));
	if (!TestEqual(TEXT("Three marked automations are three operations"), Plan.Ops.Num(), 3)
		|| !TestTrue(TEXT("The plan is committable"), Plan.bCommittable))
	{
		return false;
	}

	// With no session sign-in the first operation fails inside the call that issues it, which is exactly the shape of a
	// server refusal: one operation answered, and the walk decides what to do next.
	Controller->CommitDeletePlan(Plan, /*ExpectedAppId*/ 100);

	const FCrowdyDeleteOutcome& Outcome = Controller->GetLastDeleteOutcome();
	TestEqual(TEXT("The walk ends exactly once"), Finished, 1);
	TestTrue(TEXT("Starting and ending are both announced as progress"), Progress >= 2);
	TestEqual(TEXT("The outcome is pinned to the app the walk ran against"), Outcome.AppId, static_cast<int64>(100));
	TestEqual(TEXT("The walk set out to run every operation"), Outcome.Total, 3);
	TestTrue(TEXT("It stopped"), Outcome.bStopped);
	TestFalse(TEXT("The server answering is not a cancellation"), Outcome.bStoppedByCancel);
	// The load-bearing pair: nothing completed, and it stopped where it failed. A walk that carried on to the next
	// operation would report a later index and a shorter remainder.
	TestEqual(TEXT("Nothing completed"), Outcome.Completed, 0);
	TestEqual(TEXT("It stopped on the first operation"), Outcome.StoppedAtIndex, 0);
	TestEqual(TEXT("The stop names the operation that did not complete"),
		Outcome.StoppedOnDescription, Plan.Ops[0].Describe);

	const TArray<FCrowdyDeleteOp>& Remainder = Controller->GetDeleteRemainder();
	if (TestEqual(TEXT("Everything is left to do"), Remainder.Num(), 3))
	{
		for (int32 Index = 0; Index < 3; ++Index)
		{
			TestTrue(FString::Printf(TEXT("Remainder entry %d is the plan's own operation %d"), Index, Index),
				CrowdyDeleteRoutingOpsMatch(Remainder[Index], Plan.Ops[Index]));
		}
	}

	TestFalse(TEXT("The walk is no longer running"), Controller->IsDeleteCommitInFlight());

	return true;
}

// A stopped walk's remainder is what a second press runs, so it has to start at the operation that failed and hold
// every operation after it, in the same order. Starting one later would silently skip the operation the walk stopped
// on, and that is the one a second press exists to finish.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioDeleteWalkRemainderStartsAtTheFailedOpTest,
	"CrowdySDK.CrowdyStudio.DeleteWalkRemainderStartsAtTheFailedOp", CrowdyStudioDeleteRoutingTestFlags)

bool FCrowdyStudioDeleteWalkRemainderStartsAtTheFailedOpTest::RunTest(const FString& /*Parameters*/)
{
	const TArray<FCrowdyDeleteOp> Ops = CrowdyGameModelDelete::BuildOps(
		CrowdyDeleteRoutingAutomationMarks(5), CrowdyDeleteRoutingEvidence(100));
	if (!TestEqual(TEXT("Five marked automations are five operations"), Ops.Num(), 5))
	{
		return false;
	}

	// A walk two operations in, waiting on the third. Nothing here can hold a reply open long enough for the public
	// entry point to leave a walk in that state, so it is seeded: the teardown below only exists for a walk mid-flight.
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedApp(*Controller, 200);
	FCrowdyStudioControllerTestAccess::SeedInFlightDeleteWalk(*Controller, /*AppId*/ 100, Ops,
		/*Completed*/ 2, /*AlreadyGone*/ 1);

	int32 Finished = 0;
	Controller->OnDeleteCommitFinished.AddLambda([&Finished]() { ++Finished; });

	// The app switch ends it: a reply issued for one app is dropped once the selection has moved, so nothing else
	// would ever finish this walk and the page would report a delete that is still running.
	FCrowdyStudioControllerTestAccess::ClearAppScopedState(*Controller);

	const FCrowdyDeleteOutcome& Outcome = Controller->GetLastDeleteOutcome();
	TestEqual(TEXT("The walk ends exactly once"), Finished, 1);
	TestEqual(TEXT("The outcome keeps the app the walk ran against"), Outcome.AppId, static_cast<int64>(100));
	TestEqual(TEXT("It stopped on the operation after the last completed one"), Outcome.StoppedAtIndex, 2);
	TestEqual(TEXT("The completed count survives"), Outcome.Completed, 2);
	// Reported separately rather than folded into the completed count: "two deleted, one was already gone" and "three
	// deleted" are different facts.
	TestEqual(TEXT("The already-gone count survives"), Outcome.AlreadyGone, 1);
	TestEqual(TEXT("The stop names the operation that did not complete"),
		Outcome.StoppedOnDescription, Ops[2].Describe);

	const TArray<FCrowdyDeleteOp>& Remainder = Controller->GetDeleteRemainder();
	if (TestEqual(TEXT("Three operations are left"), Remainder.Num(), 3))
	{
		TestTrue(TEXT("The remainder starts at the operation that failed"),
			CrowdyDeleteRoutingOpsMatch(Remainder[0], Ops[2]));
		TestTrue(TEXT("It keeps the order the walk would have run"),
			CrowdyDeleteRoutingOpsMatch(Remainder[1], Ops[3]));
		TestTrue(TEXT("It runs to the end of the plan"),
			CrowdyDeleteRoutingOpsMatch(Remainder[2], Ops[4]));
	}

	TestEqual(TEXT("No walk is pinned to any app afterwards"),
		FCrowdyStudioControllerTestAccess::GetDeleteCommitAppId(*Controller), static_cast<int64>(0));

	return true;
}

// A cancellation is this editor rebuilding its own connection; a stop is the server answering. Told the same way, the
// first sends a reader hunting for a blocker that was never there. Both outcomes below come from the real paths that
// produce them rather than from hand-built structs, so the wording cannot drift apart from the thing it describes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioDeleteWalkCancelAndStopReadDifferentlyTest,
	"CrowdySDK.CrowdyStudio.DeleteWalkCancelAndStopReadDifferently", CrowdyStudioDeleteRoutingTestFlags)

bool FCrowdyStudioDeleteWalkCancelAndStopReadDifferentlyTest::RunTest(const FString& /*Parameters*/)
{
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedApp(*Controller, 100);

	// The server-answered stop: a real commit whose first operation comes back failed.
	const FCrowdyDeletePlan Plan = CrowdyGameModelDelete::BuildPlan(
		CrowdyDeleteRoutingAutomationMarks(3), CrowdyDeleteRoutingEvidence(100));
	Controller->CommitDeletePlan(Plan, /*ExpectedAppId*/ 100);

	const FCrowdyDeleteOutcome ServerStop = Controller->GetLastDeleteOutcome();
	const FString ServerText = CrowdyGameModelDelete::StopText(ServerStop);
	TestTrue(TEXT("The server-answered stop is recorded as a stop"), ServerStop.bStopped);
	TestFalse(TEXT("The server-answered stop is not a cancellation"), ServerStop.bStoppedByCancel);

	// The cancellation: a walk pinned to one app while the selection has already moved to another.
	FCrowdyStudioControllerTestAccess::SetSelectedApp(*Controller, 200);
	FCrowdyStudioControllerTestAccess::SeedInFlightDeleteWalk(*Controller, /*AppId*/ 100, Plan.Ops,
		/*Completed*/ 1, /*AlreadyGone*/ 0);
	FCrowdyStudioControllerTestAccess::ClearAppScopedState(*Controller);

	const FCrowdyDeleteOutcome Canceled = Controller->GetLastDeleteOutcome();
	const FString CanceledText = CrowdyGameModelDelete::StopText(Canceled);
	TestTrue(TEXT("The teardown is recorded as a cancellation"), Canceled.bStoppedByCancel);

	TestFalse(TEXT("The two stops do not read the same"),
		CanceledText.Equals(ServerText, ESearchCase::CaseSensitive));
	TestTrue(TEXT("A cancellation says the server refused nothing"),
		CanceledText.Contains(TEXT("refused nothing")));
	TestFalse(TEXT("A server-answered stop does not claim the server refused nothing"),
		ServerText.Contains(TEXT("refused nothing")));
	// Both say the same thing about what to do next, because it is the same thing: the deletes already made are gone
	// and re-running them does nothing, so a second press finishes the rest rather than starting over.
	TestTrue(TEXT("A cancellation says a second press finishes it"), CanceledText.Contains(TEXT("again")));
	TestTrue(TEXT("A server-answered stop says a second press finishes it"), ServerText.Contains(TEXT("again")));

	// The teardown left the status showing the cancellation, and nothing refreshed over it: the walk's app is no longer
	// the selection, so re-reading there would fill the new app's page from the previous app's walk.
	TestEqual(TEXT("The cancellation is what the status is left saying"),
		Controller->GetStatusMessage(), CanceledText);

	return true;
}

// The pre-flight's count of a model's live models must come from its own read. The shared live list is filtered by
// whatever was last typed into the Live tab's boxes, so counting from it would report zero for a model with hundreds
// of instances, and a refusal nobody was warned about would read as a green light. That is only safe if the scoped
// read writes none of that state back.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioScopedLiveCountLeavesTheLiveListAloneTest,
	"CrowdySDK.CrowdyStudio.ScopedLiveCountLeavesTheLiveListAlone", CrowdyStudioDeleteRoutingTestFlags)

bool FCrowdyStudioScopedLiveCountLeavesTheLiveListAloneTest::RunTest(const FString& /*Parameters*/)
{
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedApp(*Controller, 100);
	FCrowdyStudioControllerTestAccess::SetContainers(*Controller,
		{ FCrowdyStudioControllerTestAccess::MakeContainer(TEXT("live-1")),
		  FCrowdyStudioControllerTestAccess::MakeContainer(TEXT("live-2")) });
	FCrowdyStudioControllerTestAccess::SetContainerWindow(*Controller, TEXT("Wolf"), TEXT("session-7"),
		/*Limit*/ 50, /*Offset*/ 100, /*PageSize*/ 50, /*bMayHaveMore*/ true);

	int32 ListRepaints = 0;
	Controller->OnContainersChanged.AddLambda([&ListRepaints]() { ++ListRepaints; });

	int32 Answered = 0;
	int64 AnsweredForApp = 0;
	TArray<FCrowdyDeleteLiveCount> Counts;
	Controller->CountLiveModelsScoped({ TEXT("Knight"), TEXT("Wolf") }, CrowdyDeleteLiveModelProbeLimit,
		[&Answered, &AnsweredForApp, &Counts](int64 AppId, TArray<FCrowdyDeleteLiveCount>&& Result)
		{
			++Answered;
			AnsweredForApp = AppId;
			Counts = MoveTemp(Result);
		});

	TestEqual(TEXT("The probe answers exactly once"), Answered, 1);
	TestEqual(TEXT("It answers for the app it was issued against"), AnsweredForApp, static_cast<int64>(100));

	// Every read failed, so nothing may come back claiming a count. A model with no entry reads as Unknown through the
	// evidence, which is the only lookup a plan uses, and Unknown is a blocker rather than a green light.
	for (const FCrowdyDeleteLiveCount& Count : Counts)
	{
		TestEqual(TEXT("A read that never landed reports no count"),
			static_cast<int32>(Count.State), static_cast<int32>(ECrowdyLiveCountState::Unknown));
	}

	FCrowdyDeleteEvidence Evidence = CrowdyDeleteRoutingEvidence(100);
	Evidence.LiveCounts = Counts;
	int32 KnightCount = -1;
	int32 WolfCount = -1;
	TestEqual(TEXT("A model whose read failed is Unknown, not zero"),
		static_cast<int32>(Evidence.FindLiveCount(TEXT("Knight"), KnightCount)),
		static_cast<int32>(ECrowdyLiveCountState::Unknown));
	TestEqual(TEXT("The second model is Unknown too"),
		static_cast<int32>(Evidence.FindLiveCount(TEXT("Wolf"), WolfCount)),
		static_cast<int32>(ECrowdyLiveCountState::Unknown));

	// Nothing the Live tab renders or pages from may have moved.
	if (TestEqual(TEXT("The live list still holds what it held"), Controller->GetContainers().Num(), 2))
	{
		TestEqual(TEXT("And holds the same entries in the same order"),
			Controller->GetContainers()[0]->ContainerId, FString(TEXT("live-1")));
		TestEqual(TEXT("And its last entry is untouched"),
			Controller->GetContainers()[1]->ContainerId, FString(TEXT("live-2")));
	}
	TestEqual(TEXT("The remembered type filter is untouched"),
		FCrowdyStudioControllerTestAccess::GetLastContainerTypeFilter(*Controller), FString(TEXT("Wolf")));
	TestEqual(TEXT("The remembered session filter is untouched"),
		FCrowdyStudioControllerTestAccess::GetLastContainerSessionFilter(*Controller), FString(TEXT("session-7")));
	TestEqual(TEXT("The remembered page limit is untouched"),
		FCrowdyStudioControllerTestAccess::GetLastContainerLimit(*Controller), 50);
	TestEqual(TEXT("The remembered page offset is untouched"),
		FCrowdyStudioControllerTestAccess::GetLastContainerOffset(*Controller), 100);
	TestEqual(TEXT("The remembered page size is untouched"),
		FCrowdyStudioControllerTestAccess::GetLastContainerPageSize(*Controller), 50);
	TestTrue(TEXT("The paging evidence is untouched, so load-more is still offered"),
		Controller->ContainersMayHaveMore());
	TestEqual(TEXT("The live list is never told to repaint"), ListRepaints, 0);
	TestFalse(TEXT("No probe is left outstanding"),
		FCrowdyStudioControllerTestAccess::HasOutstandingLiveCount(*Controller));

	return true;
}

// The review continues from the probe's completion, so a path that quietly declines to run it strands the review
// saying a check is under way with no way to start another. Every path that can end a probe has to answer, and only
// the first one may.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioScopedLiveCountAlwaysAnswersTest,
	"CrowdySDK.CrowdyStudio.ScopedLiveCountAlwaysAnswers", CrowdyStudioDeleteRoutingTestFlags)

bool FCrowdyStudioScopedLiveCountAlwaysAnswersTest::RunTest(const FString& /*Parameters*/)
{
	// No app selected: there is nothing to read and no id to scope a read to, and the completion still runs.
	{
		const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
		int32 Answered = 0;
		TArray<FCrowdyDeleteLiveCount> Counts;
		Controller->CountLiveModelsScoped({ TEXT("Knight") }, CrowdyDeleteLiveModelProbeLimit,
			[&Answered, &Counts](int64 /*AppId*/, TArray<FCrowdyDeleteLiveCount>&& Result)
			{
				++Answered;
				Counts = MoveTemp(Result);
			});
		TestEqual(TEXT("A probe with no app selected still answers"), Answered, 1);
		for (const FCrowdyDeleteLiveCount& Count : Counts)
		{
			TestEqual(TEXT("And claims no count for anything"),
				static_cast<int32>(Count.State), static_cast<int32>(ECrowdyLiveCountState::Unknown));
		}
	}

	// Nothing worth reading: the completion runs rather than the caller waiting on a read that was never issued.
	{
		const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
		FCrowdyStudioControllerTestAccess::SetSelectedApp(*Controller, 100);
		int32 Answered = 0;
		Controller->CountLiveModelsScoped({}, CrowdyDeleteLiveModelProbeLimit,
			[&Answered](int64 /*AppId*/, TArray<FCrowdyDeleteLiveCount>&&) { ++Answered; });
		TestEqual(TEXT("A probe with nothing to read still answers"), Answered, 1);
	}

	// A second probe over an outstanding one would share its pending list, and the first caller's completion would
	// then fire on the second caller's counts. The newcomer is answered immediately instead, with every model it asked
	// about reported Unknown, which is a blocker rather than a green light and clears by asking again.
	{
		const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
		FCrowdyStudioControllerTestAccess::SetSelectedApp(*Controller, 100);

		int32 FirstAnswered = 0;
		FCrowdyStudioControllerTestAccess::SeedOutstandingLiveCount(*Controller, /*AppId*/ 100, { TEXT("Knight") },
			[&FirstAnswered](int64 /*AppId*/, TArray<FCrowdyDeleteLiveCount>&&) { ++FirstAnswered; });

		int32 SecondAnswered = 0;
		TArray<FCrowdyDeleteLiveCount> SecondCounts;
		Controller->CountLiveModelsScoped({ TEXT("Wolf") }, CrowdyDeleteLiveModelProbeLimit,
			[&SecondAnswered, &SecondCounts](int64 /*AppId*/, TArray<FCrowdyDeleteLiveCount>&& Result)
			{
				++SecondAnswered;
				SecondCounts = MoveTemp(Result);
			});

		TestEqual(TEXT("The second caller is answered straight away"), SecondAnswered, 1);
		TestEqual(TEXT("The outstanding probe is not answered on the newcomer's behalf"), FirstAnswered, 0);
		TestTrue(TEXT("The outstanding probe is still outstanding"),
			FCrowdyStudioControllerTestAccess::HasOutstandingLiveCount(*Controller));
		if (TestEqual(TEXT("The refusal names the model it was asked about"), SecondCounts.Num(), 1))
		{
			TestEqual(TEXT("By name"), SecondCounts[0].TypeName, FString(TEXT("Wolf")));
			TestEqual(TEXT("With no count"),
				static_cast<int32>(SecondCounts[0].State), static_cast<int32>(ECrowdyLiveCountState::Unknown));
		}
	}

	return true;
}

// An app switch is the one path that can end a probe from outside. A reply issued for one app is dropped once the
// selection has moved, so nothing would decrement the outstanding work and the completion would never fire.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioAppSwitchAnswersAnOutstandingLiveCountTest,
	"CrowdySDK.CrowdyStudio.AppSwitchAnswersAnOutstandingLiveCount", CrowdyStudioDeleteRoutingTestFlags)

bool FCrowdyStudioAppSwitchAnswersAnOutstandingLiveCountTest::RunTest(const FString& /*Parameters*/)
{
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedApp(*Controller, 100);

	int32 Answered = 0;
	int64 AnsweredForApp = 0;
	TArray<FCrowdyDeleteLiveCount> Counts;
	FCrowdyStudioControllerTestAccess::SeedOutstandingLiveCount(*Controller, /*AppId*/ 100,
		{ TEXT("Knight"), TEXT("Wolf") },
		[&Answered, &AnsweredForApp, &Counts](int64 AppId, TArray<FCrowdyDeleteLiveCount>&& Result)
		{
			++Answered;
			AnsweredForApp = AppId;
			Counts = MoveTemp(Result);
		});

	FCrowdyStudioControllerTestAccess::SetSelectedApp(*Controller, 200);
	FCrowdyStudioControllerTestAccess::ClearAppScopedState(*Controller);

	TestEqual(TEXT("The outstanding probe is answered exactly once"), Answered, 1);
	TestEqual(TEXT("It answers for the app it was issued against, not the one now selected"),
		AnsweredForApp, static_cast<int64>(100));
	if (TestEqual(TEXT("Every model it was asked about is named"), Counts.Num(), 2))
	{
		// Named rather than left out, so a caller walking the results sees the models it asked about and cannot read
		// an absent entry as a count of zero.
		TestEqual(TEXT("The first model comes back with no count"),
			static_cast<int32>(Counts[0].State), static_cast<int32>(ECrowdyLiveCountState::Unknown));
		TestEqual(TEXT("The second model comes back with no count"),
			static_cast<int32>(Counts[1].State), static_cast<int32>(ECrowdyLiveCountState::Unknown));
	}
	TestFalse(TEXT("Nothing is left outstanding"),
		FCrowdyStudioControllerTestAccess::HasOutstandingLiveCount(*Controller));

	// Answering again would fire a completion the caller has already had, so only the first path to reach it may.
	FCrowdyStudioControllerTestAccess::ClearAppScopedState(*Controller);
	TestEqual(TEXT("A second reset does not answer again"), Answered, 1);

	return true;
}

// A probe's state is cleared before its completion runs, because that completion may start another probe and the new
// one must not be torn down by the teardown of the one it was started from. The two are told apart by what they
// return: a probe that really ran reports only what landed, while the refusal a second caller gets names every model
// it asked about.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioLiveCountCompletionMayStartAnotherProbeTest,
	"CrowdySDK.CrowdyStudio.LiveCountCompletionMayStartAnotherProbe", CrowdyStudioDeleteRoutingTestFlags)

bool FCrowdyStudioLiveCountCompletionMayStartAnotherProbeTest::RunTest(const FString& /*Parameters*/)
{
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedApp(*Controller, 100);

	int32 OuterAnswered = 0;
	int32 InnerAnswered = 0;
	TArray<FCrowdyDeleteLiveCount> InnerCounts;

	Controller->CountLiveModelsScoped({ TEXT("Knight") }, CrowdyDeleteLiveModelProbeLimit,
		[&Controller, &OuterAnswered, &InnerAnswered, &InnerCounts](int64 /*AppId*/, TArray<FCrowdyDeleteLiveCount>&&)
		{
			++OuterAnswered;
			Controller->CountLiveModelsScoped({ TEXT("Wolf") }, CrowdyDeleteLiveModelProbeLimit,
				[&InnerAnswered, &InnerCounts](int64 /*AppId*/, TArray<FCrowdyDeleteLiveCount>&& Result)
				{
					++InnerAnswered;
					InnerCounts = MoveTemp(Result);
				});
		});

	TestEqual(TEXT("The first probe answers once"), OuterAnswered, 1);
	TestEqual(TEXT("The probe started from its completion answers once too"), InnerAnswered, 1);
	// A probe refused as a second caller names every model it was asked about; one that really issued its reads
	// reports only what landed, and here nothing did. Anything named would mean the inner probe was turned away.
	TestEqual(TEXT("The second probe really ran rather than being turned away as a duplicate"), InnerCounts.Num(), 0);
	TestFalse(TEXT("Nothing is left outstanding"),
		FCrowdyStudioControllerTestAccess::HasOutstandingLiveCount(*Controller));

	return true;
}

// A prune candidate is read by a person deciding what to destroy, so a function has to carry the model the diff
// identified it by. Two models can carry a function of the same name, and a bare name cannot tell them apart: the list
// would show one line where there are two entities, and everything downstream would be guessing which model it meant.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioPruneCandidatesCarryScopedFunctionIdentityTest,
	"CrowdySDK.CrowdyStudio.PruneCandidatesCarryScopedFunctionIdentity", CrowdyStudioDeleteRoutingTestFlags)

bool FCrowdyStudioPruneCandidatesCarryScopedFunctionIdentityTest::RunTest(const FString& /*Parameters*/)
{
	// Nothing in code authors any of these, so all three are server-only.
	TArray<FStudioFunction> ServerFunctions;
	ServerFunctions.Add(CrowdyDeleteRoutingServerFunction(TEXT("regen"), TEXT("Knight")));
	ServerFunctions.Add(CrowdyDeleteRoutingServerFunction(TEXT("regen"), TEXT("Wolf")));
	ServerFunctions.Add(CrowdyDeleteRoutingServerFunction(TEXT("orphan"), FString()));

	FCrowdySchemaDelta Delta;
	FCrowdySchemaSync::DiffFunctions({}, ServerFunctions, Delta);

	if (!TestEqual(TEXT("Two same-named functions on two models are two candidates"),
		Delta.ServerOnlyFunctions.Num(), 3))
	{
		return false;
	}

	const FCrowdySchemaFunctionRef* OnKnight = Delta.ServerOnlyFunctions.FindByPredicate(
		[](const FCrowdySchemaFunctionRef& Ref)
		{ return Ref.ContainerTypeName.Equals(TEXT("Knight"), ESearchCase::CaseSensitive); });
	const FCrowdySchemaFunctionRef* OnWolf = Delta.ServerOnlyFunctions.FindByPredicate(
		[](const FCrowdySchemaFunctionRef& Ref)
		{ return Ref.ContainerTypeName.Equals(TEXT("Wolf"), ESearchCase::CaseSensitive); });

	if (TestNotNull(TEXT("One candidate belongs to the first model"), OnKnight)
		&& TestNotNull(TEXT("The other belongs to the second"), OnWolf))
	{
		TestEqual(TEXT("Both carry the same name"), OnKnight->Name, FString(TEXT("regen")));
		TestEqual(TEXT("And are told apart only by their model"), OnWolf->Name, FString(TEXT("regen")));
		TestTrue(TEXT("The first has a determined owner"), OnKnight->HasOwningType());
		TestTrue(TEXT("So does the second"), OnWolf->HasOwningType());
	}

	// An empty owner is UNDETERMINED, never "no model owns it": a server record with no binding and a read that never
	// carried one are indistinguishable here, and only one of those is safe to act on.
	const FCrowdySchemaFunctionRef* Undetermined = Delta.ServerOnlyFunctions.FindByPredicate(
		[](const FCrowdySchemaFunctionRef& Ref)
		{ return Ref.Name.Equals(TEXT("orphan"), ESearchCase::CaseSensitive); });
	if (TestNotNull(TEXT("A candidate whose read carried no binding is still listed"), Undetermined))
	{
		TestFalse(TEXT("And says its owner is undetermined"), Undetermined->HasOwningType());
	}

	// The membership test compares server keys, and FString comparison folds case by default, so two functions
	// differing only in case would read as one.
	TestTrue(TEXT("The name is found"), Delta.HasServerOnlyFunction(TEXT("regen")));
	TestFalse(TEXT("A name differing only in case is a different function"),
		Delta.HasServerOnlyFunction(TEXT("Regen")));

	// The controller half: the candidates survive into the prune with their identity intact, so two same-named
	// functions on two models produce two operations rather than being folded into one.
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedApp(*Controller, 100);

	// The candidates came from a plan pinned to one app; never delete from a different now-selected app.
	FCrowdyStudioControllerTestAccess::SeedPruneFunctions(*Controller, /*PlannedAppId*/ 999,
		Delta.ServerOnlyFunctions);
	Controller->PruneServerOnlySchema();
	TestTrue(TEXT("A prune of another app's plan is refused"),
		Controller->GetStatusMessage().Contains(CrowdyDeleteRoutingWrongAppPhrase));

	FCrowdyStudioControllerTestAccess::SeedPruneFunctions(*Controller, /*PlannedAppId*/ 100,
		Delta.ServerOnlyFunctions);
	TestTrue(TEXT("There is something to prune"), Controller->HasPendingPrune());
	Controller->PruneServerOnlySchema();
	// The walk stops on its first operation here, and the note it leaves states how many there were. Three candidates
	// keyed by name alone would collapse to two.
	TestTrue(TEXT("Every candidate becomes an operation of its own"),
		Controller->GetStatusMessage().Contains(TEXT("of 3 entity")));

	return true;
}

// Every server id is a BigInt, so appId travels as a decimal STRING and never as a JSON number. It is also the one
// argument an operation cannot carry among its plain string arguments, which is what keeps it from being set the wrong
// way: the walk emits it itself, and an operation that carried its own would be emitting it as something else.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioDeleteAppIdIsAJsonStringTest,
	"CrowdySDK.CrowdyStudio.DeleteAppIdIsAJsonString", CrowdyStudioDeleteRoutingTestFlags)

bool FCrowdyStudioDeleteAppIdIsAJsonStringTest::RunTest(const FString& /*Parameters*/)
{
	// The single emitter every delete on this surface uses for appId.
	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	CrowdyStudioMarshalling::SetBigIntField(Variables, TEXT("appId"), 4300000000LL);

	const TSharedPtr<FJsonValue>* Emitted = Variables->Values.Find(TEXT("appId"));
	if (TestTrue(TEXT("appId is emitted"), Emitted != nullptr && Emitted->IsValid()))
	{
		TestEqual(TEXT("appId is a JSON string"),
			static_cast<int32>((*Emitted)->Type), static_cast<int32>(EJson::String));
	}
	FString Text;
	if (TestTrue(TEXT("appId reads back as text"), Variables->TryGetStringField(TEXT("appId"), Text)))
	{
		// A value past the 32-bit range, so a destination that truncated it would produce a different string rather
		// than the same one.
		TestEqual(TEXT("appId keeps its full BigInt value"), Text, FString(TEXT("4300000000")));
	}

	// No operation carries appId among its own arguments, for any kind. One that did would be written out as a plain
	// string field beside the BigInt the walk already emitted, and whichever went last would win.
	// The attribute belongs to a model that is NOT itself marked. Deleting a model already removes its attributes,
	// so an attribute of a marked model contributes no operation of its own and this would exercise four kinds
	// while claiming to cover five.
	TArray<FCrowdyDeleteMark> Marks;
	Marks.Add(CrowdyGameModelDelete::MarkAutomation(TEXT("regen_tick"), TEXT("regen_tick")));
	Marks.Add(CrowdyGameModelDelete::MarkLiveModel(TEXT("Knight"), TEXT("live-1"), TEXT("live-1")));
	Marks.Add(CrowdyGameModelDelete::MarkAttribute(TEXT("Wolf"), TEXT("hp"), TEXT("hp")));
	Marks.Add(CrowdyGameModelDelete::MarkFunction(TEXT("Knight"), TEXT("regen"), TEXT("regen")));
	Marks.Add(CrowdyGameModelDelete::MarkModel(TEXT("Knight"), TEXT("Knight")));

	const TArray<FCrowdyDeleteOp> Ops = CrowdyGameModelDelete::BuildOps(Marks, CrowdyDeleteRoutingEvidence(100));
	if (!TestEqual(TEXT("Each of the five kinds is one operation"), Ops.Num(), 5))
	{
		return false;
	}
	for (const FCrowdyDeleteOp& Op : Ops)
	{
		TestFalse(FString::Printf(TEXT("%s carries no appId of its own"), *Op.OperationName),
			CrowdyDeleteRoutingHasArg(Op, TEXT("appId")));
		TestTrue(FString::Printf(TEXT("%s carries at least one argument"), *Op.OperationName),
			Op.StringArgs.Num() > 0);
	}

	return true;
}

// The walk ADVANCES. Every other commit test here ends on operation zero, because a controller with no session
// sign-in fails every game-plane call inside the call that issues it, so "stops at the first failure" and "never
// advances at all" produce the same observations and no single change to the forward path can be proved red. This one
// lands a reply on the operation in flight and then checks that the walk moved to the next one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioDeleteWalkAdvancesPastEachReplyTest,
	"CrowdySDK.CrowdyStudio.DeleteWalkAdvancesPastEachReply", CrowdyStudioDeleteRoutingTestFlags)

bool FCrowdyStudioDeleteWalkAdvancesPastEachReplyTest::RunTest(const FString& /*Parameters*/)
{
	const TArray<FCrowdyDeleteOp> Ops = CrowdyGameModelDelete::BuildOps(
		CrowdyDeleteRoutingAutomationMarks(3), CrowdyDeleteRoutingEvidence(100));
	if (!TestEqual(TEXT("Three marked automations are three operations"), Ops.Num(), 3))
	{
		return false;
	}

	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedApp(*Controller, 100);
	FCrowdyStudioControllerTestAccess::SeedInFlightDeleteWalk(*Controller, /*AppId*/ 100, Ops,
		/*Completed*/ 0, /*AlreadyGone*/ 0);

	int32 Finished = 0;
	Controller->OnDeleteCommitFinished.AddLambda([&Finished]() { ++Finished; });

	// Operation zero answers that the entity was there and is gone. The walk counts it and issues operation one,
	// which has no sign-in behind it and therefore fails inside the call that issues it, ending the walk on index 1.
	FCrowdyStudioControllerTestAccess::LandDeleteReply(*Controller, /*Index*/ 0, /*bDeleted*/ true);

	const FCrowdyDeleteOutcome& Outcome = Controller->GetLastDeleteOutcome();
	TestEqual(TEXT("The walk ends exactly once"), Finished, 1);
	TestEqual(TEXT("The answered operation counted"), Outcome.Completed, 1);
	TestEqual(TEXT("A delete that really happened is not reported as already gone"), Outcome.AlreadyGone, 0);
	// The load-bearing assertion: index ONE, not zero. A walk that never moved on would stop where it started.
	TestEqual(TEXT("And the walk moved to the next operation before stopping"), Outcome.StoppedAtIndex, 1);
	TestEqual(TEXT("The stop names the operation that did not complete"),
		Outcome.StoppedOnDescription, Ops[1].Describe);
	TestEqual(TEXT("Two operations are left"), Controller->GetDeleteRemainder().Num(), 2);
	TestFalse(TEXT("Nothing is still in flight"),
		FCrowdyStudioControllerTestAccess::HasDeleteWalkInFlight(*Controller));

	// A false boolean means the entity was not there. That is an idempotent no-op and a SUCCESS, which is exactly
	// what a second press after a partial commit sees on everything the first attempt already finished. Read as a
	// failure it would stop the walk on the one operation that needed no work at all.
	const TSharedRef<FCrowdyStudioController> Second = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedApp(*Second, 100);
	FCrowdyStudioControllerTestAccess::SeedInFlightDeleteWalk(*Second, /*AppId*/ 100, Ops,
		/*Completed*/ 0, /*AlreadyGone*/ 0);
	FCrowdyStudioControllerTestAccess::LandDeleteReply(*Second, /*Index*/ 0, /*bDeleted*/ false);

	const FCrowdyDeleteOutcome& Idempotent = Second->GetLastDeleteOutcome();
	TestEqual(TEXT("An entity that was not there still counts as done"), Idempotent.Completed, 1);
	TestEqual(TEXT("And is reported honestly as having been already gone"), Idempotent.AlreadyGone, 1);
	TestEqual(TEXT("The walk carried on past it"), Idempotent.StoppedAtIndex, 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioContainerPurgeRefusesAcrossAppsTest,
	"CrowdySDK.CrowdyStudio.ContainerPurgeRefusesAcrossApps", CrowdyStudioDeleteRoutingTestFlags)

bool FCrowdyStudioContainerPurgeRefusesAcrossAppsTest::RunTest(const FString& /*Parameters*/)
{
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedApp(*Controller, 100);

	int32 Finished = 0;
	Controller->OnContainerPurgeFinished.AddLambda([&Finished]() { ++Finished; });

	// The Live tab passes the app its list was READ for. A purge aimed at another app's list would empty the app
	// that happens to be selected now, and there is no undo.
	Controller->PurgeContainers(FString(), /*ExpectedAppId*/ 200);

	TestEqual(TEXT("The refusal is announced exactly once"), Finished, 1);
	TestEqual(TEXT("Nothing was pinned"),
		FCrowdyStudioControllerTestAccess::GetContainerPurgeAppId(*Controller), static_cast<int64>(0));
	TestFalse(TEXT("Nothing is running"), Controller->IsContainerPurgeInFlight());
	TestEqual(TEXT("Nothing was deleted"), Controller->GetLastContainerPurgeOutcome().Completed, 0);

	return true;
}

// The drain re-reads at offset zero and deletes what comes back, so a page it cannot actually empty would be read
// back identically forever. The guard is that a page which REMOVED nothing ends it, and an already-gone reply must
// not count as a removal: a server that keeps listing a live model while answering "it was not there" is exactly
// the case that loops.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioContainerPurgeStopsOnAPageItCannotEmptyTest,
	"CrowdySDK.CrowdyStudio.ContainerPurgeStopsOnAPageItCannotEmpty", CrowdyStudioDeleteRoutingTestFlags)

bool FCrowdyStudioContainerPurgeStopsOnAPageItCannotEmptyTest::RunTest(const FString& /*Parameters*/)
{
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedApp(*Controller, 100);

	int32 Finished = 0;
	Controller->OnContainerPurgeFinished.AddLambda([&Finished]() { ++Finished; });

	// One entry, because the walk advances on its own: with a longer page the next delete is issued (and, with no
	// session sign-in, fails inside the call) before the page is ever walked to its end, and the guard lives at
	// that end.
	FCrowdyStudioControllerTestAccess::SeedInFlightContainerPurge(*Controller, /*AppId*/ 100, FString(),
		{ TEXT("container-1") });

	// "It was not there", against a read that had just listed it. That is the shape that repeats forever.
	FCrowdyStudioControllerTestAccess::LandContainerPurgeReply(*Controller, 0, /*bDeleted*/ false);

	const FCrowdyDeleteOutcome& Outcome = Controller->GetLastContainerPurgeOutcome();
	TestEqual(TEXT("The purge ends exactly once"), Finished, 1);
	TestTrue(TEXT("It stopped rather than draining forever"), Outcome.bStopped);
	TestFalse(TEXT("And not as a cancellation"), Outcome.bStoppedByCancel);
	TestTrue(TEXT("The stop names the page it could not empty"),
		Outcome.StoppedOnDescription.Contains(TEXT("kept listing but did not remove")));
	// The reply still counts as settled work, and is reported honestly as having been already gone.
	TestEqual(TEXT("The reply counted"), Outcome.Completed, 1);
	TestEqual(TEXT("And is reported as already gone"), Outcome.AlreadyGone, 1);
	TestFalse(TEXT("Nothing is left running"), Controller->IsContainerPurgeInFlight());

	// The discriminator: the same page with a REAL removal is progress, so the drain reads the next page instead of
	// stopping on the guard. That read fails here, with no sign-in, so this still ends, but on the read.
	const TSharedRef<FCrowdyStudioController> Progressing = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedApp(*Progressing, 100);
	FCrowdyStudioControllerTestAccess::SeedInFlightContainerPurge(*Progressing, /*AppId*/ 100, FString(),
		{ TEXT("container-1") });
	FCrowdyStudioControllerTestAccess::LandContainerPurgeReply(*Progressing, 0, /*bDeleted*/ true);

	const FCrowdyDeleteOutcome& Moved = Progressing->GetLastContainerPurgeOutcome();
	TestEqual(TEXT("A real removal counted"), Moved.Completed, 1);
	TestEqual(TEXT("And is not reported as already gone"), Moved.AlreadyGone, 0);
	TestFalse(TEXT("A page that removed something never stops on the guard"),
		Moved.StoppedOnDescription.Contains(TEXT("kept listing but did not remove")));
	TestTrue(TEXT("It went on to read what was left"),
		Moved.StoppedOnDescription.Contains(TEXT("a read of what is left")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
