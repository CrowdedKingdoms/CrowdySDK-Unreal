#include "Replication/GameModel/CrowdyGameModelSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyCompletionLivenessTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
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

#endif // WITH_DEV_AUTOMATION_TESTS
