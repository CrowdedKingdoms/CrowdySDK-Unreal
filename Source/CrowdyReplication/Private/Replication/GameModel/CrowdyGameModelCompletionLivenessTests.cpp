#include "Replication/GameModel/CrowdyGameModelSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyCompletionLivenessTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Every Game Model call resolves an endpoint + bearer token before anything reaches the wire. A subsystem built
	// with NewObject has no world and therefore no game session, so each call below fails that resolve and runs its
	// completion inline - which is what makes the real completions drivable with no server and no world. Whitelist
	// the resolve's error (whichever of the three fires first for the local configuration) so the failures these
	// tests deliberately provoke do not fail the tests themselves.
	void AllowMissingApiContext(FAutomationTestBase& Test)
	{
		Test.AddExpectedError(TEXT("Game API endpoint is empty|No UCrowdyGameSession|No app-scoped game token"),
			EAutomationExpectedErrorFlags::Contains, 0);
	}
}

// The completion-side liveness resolve. The Game API client is owned by the game instance and outlives this world
// subsystem, so a completion can land after the world has torn down: the object may still resolve while every cache
// it would write into has already been cleared. The resolve therefore keys on the world session, not on object
// liveness, and it is only ever a gate on TOUCHING STATE - never on notifying the caller.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelResolveLiveTest,
	"CrowdySDK.GameModel.ResolveLive", CrowdyCompletionLivenessTestFlags)
bool FCrowdyGameModelResolveLiveTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model))
	{
		return false;
	}
	const TWeakObjectPtr<UCrowdyGameModelSubsystem> Weak(Model);

	// No world session has started yet, so the object resolves but is not safe to write into.
	TestNull(TEXT("no session yet resolves to null"), UCrowdyGameModelSubsystem::ResolveLive(Weak));

	Model->BeginWorldSessionForTest();
	TestTrue(TEXT("a live session resolves to the subsystem"),
		UCrowdyGameModelSubsystem::ResolveLive(Weak) == Model);

	// The session ends (world teardown) while the object is still perfectly alive: the resolve must stop handing it
	// out, because that is exactly the window in which a late completion would write into cleared caches.
	Model->ForceEndWorldSessionForTest();
	TestTrue(TEXT("the object itself is still alive"), Weak.IsValid());
	TestNull(TEXT("an ended session resolves to null"), UCrowdyGameModelSubsystem::ResolveLive(Weak));

	// A null weak pointer is the ordinary destroyed-object case.
	TestNull(TEXT("a null weak pointer resolves to null"),
		UCrowdyGameModelSubsystem::ResolveLive(TWeakObjectPtr<UCrowdyGameModelSubsystem>()));
	return true;
}

// A completion must ALWAYS tell its caller the outcome, whether or not the world session survived the round-trip: a
// latent Blueprint action stays rooted on the game instance until its completion runs, so a dropped completion
// leaks the action with its exec pins never firing. This drives the real subsystem entry points (not a stand-in),
// each of which completes inline here because the API-context resolve fails, and counts the callbacks on both sides
// of world teardown. It fails if any of these completions grows an early return ahead of its caller's callback.
//
// What it does NOT cover: the state-writing half of each completion (the cache merge, the OnRep, the watch-set
// update) only runs on a SUCCESSFUL server result, which needs a live world, a game-instance client host and a real
// in-flight request. Those bodies stay exercised only by the live gates, not by this suite.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelCompletionAlwaysNotifiesTest,
	"CrowdySDK.GameModel.CompletionAlwaysNotifies", CrowdyCompletionLivenessTestFlags)
bool FCrowdyGameModelCompletionAlwaysNotifiesTest::RunTest(const FString& Parameters)
{
	AllowMissingApiContext(*this);

	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model))
	{
		return false;
	}

	// InvokeAndApply reads the entity's bound container before it calls out, so bind one.
	const FGuid NetID = FGuid::NewGuid();
	Model->BindEntityContainer(NetID, TEXT("cid-liveness"));

	int32 Notified = 0;
	auto RunEntryPoints = [Model, NetID, &Notified]()
	{
		Notified = 0;
		Model->InvokeAndApply(NetID, TEXT("fn"), nullptr, FString(),
			[&Notified](FCrowdyInvokeResult) { ++Notified; });
		Model->InvokeOnContainer(TEXT("cid-liveness"), TEXT("fn"), nullptr, FString(),
			[&Notified](FCrowdyInvokeResult) { ++Notified; });
		Model->CreateDataContainer(TEXT("Chest"), TEXT("Chest"), FString(), FString(),
			[&Notified](bool, const FString&) { ++Notified; });
		Model->PullDataContainer(TEXT("cid-liveness"), [&Notified](bool) { ++Notified; });
		Model->SetDataProperty(TEXT("cid-liveness"), TEXT("hp"), TEXT("int"), TEXT("1"),
			[&Notified](bool) { ++Notified; });
		Model->DeleteContainer(TEXT("cid-liveness"), [&Notified](bool) { ++Notified; });
	};

	constexpr int32 ExpectedCallbacks = 6;

	Model->BeginWorldSessionForTest();
	RunEntryPoints();
	TestEqual(TEXT("every caller is notified while the session is live"), Notified, ExpectedCallbacks);

	// The world tears down with the requests already issued. Each completion still has to run and still has to
	// report, even though there is no longer any cache for it to write into.
	Model->ForceEndWorldSessionForTest();
	RunEntryPoints();
	TestEqual(TEXT("every caller is still notified after the session ended"), Notified, ExpectedCallbacks);

	return true;
}

// The collection entry points fan several requests out and join them, and they resolve the subsystem BEFORE they
// look at the result, so an ended world session takes a different branch through the real production code than a
// live one. Both branches must still complete the caller exactly once - a completion that could be skipped would
// strand the join forever and leave the calling node rooted with its pins unfired.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelCompletionJoinCompletesTest,
	"CrowdySDK.GameModel.CompletionJoinCompletes", CrowdyCompletionLivenessTestFlags)
bool FCrowdyGameModelCompletionJoinCompletesTest::RunTest(const FString& Parameters)
{
	AllowMissingApiContext(*this);

	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model))
	{
		return false;
	}

	int32 Notified = 0;
	auto RunCollectionEntryPoints = [Model, &Notified]()
	{
		Notified = 0;
		Model->AddToCollection(TEXT("cid-parent"), TEXT("Bag"), TEXT("cid-item"), TEXT("contains"),
			[&Notified](bool) { ++Notified; });
		Model->RemoveFromCollection(TEXT("cid-parent"), TEXT("Bag"), TEXT("cid-item"), TEXT("contains"),
			[&Notified](bool) { ++Notified; });
		Model->GetCollectionWithState(TEXT("cid-parent"), TEXT("contains"), 8,
			[&Notified](bool, const TArray<FCrowdyCollectionItem>&) { ++Notified; });
	};

	constexpr int32 ExpectedCallbacks = 3;

	Model->BeginWorldSessionForTest();
	RunCollectionEntryPoints();
	TestEqual(TEXT("each collection caller is notified once while the session is live"), Notified, ExpectedCallbacks);

	Model->ForceEndWorldSessionForTest();
	RunCollectionEntryPoints();
	TestEqual(TEXT("each collection caller is notified once after the session ended"), Notified, ExpectedCallbacks);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
