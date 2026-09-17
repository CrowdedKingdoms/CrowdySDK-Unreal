#include "Replication/GameModel/CrowdyGameModelSubsystem.h"
#include "Replication/GameModel/CrowdyActiveSessionMemory.h"
#include "Engine/GameInstance.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

namespace
{
	constexpr EAutomationTestFlags CrowdySessionContextTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

// The one session-resolution rule, verified for every ordering: an explicit (non-empty) id always wins; with no
// explicit id the active (default) session is used; with neither the result is empty (an app-global call). This is
// the single point of truth every subsystem boundary defers to, so if this order is right the whole plane agrees.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySessionIdResolutionOrderTest,
	"CrowdySDK.Replication.SessionIdResolutionOrder", CrowdySessionContextTestFlags)
bool FCrowdySessionIdResolutionOrderTest::RunTest(const FString& Parameters)
{
	// Explicit present -> explicit wins, regardless of the active session.
	TestEqual(TEXT("explicit wins over active"),
		UCrowdyGameModelSubsystem::ResolveSessionId(TEXT("explicit-1"), TEXT("active-1")), FString(TEXT("explicit-1")));
	TestEqual(TEXT("explicit wins over empty active"),
		UCrowdyGameModelSubsystem::ResolveSessionId(TEXT("explicit-1"), FString()), FString(TEXT("explicit-1")));

	// No explicit -> the active session is used.
	TestEqual(TEXT("active used when no explicit"),
		UCrowdyGameModelSubsystem::ResolveSessionId(FString(), TEXT("active-1")), FString(TEXT("active-1")));

	// Neither -> empty (an app-global call).
	TestEqual(TEXT("empty when neither present"),
		UCrowdyGameModelSubsystem::ResolveSessionId(FString(), FString()), FString());

	return true;
}

// This world's copy of the session context: the explicit clear (ClearActiveSession) and the teardown path
// (Deinitialize) both leave it empty; with no game-instance memory attached a fresh world starts empty. The last-error
// cache follows the same lifecycle. A full PIE world teardown is exercised by GATE-S10-DefaultSessionPIE; here the
// clear method and the Deinitialize reset are driven directly on a transient subsystem (no world, no HTTP).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySessionClearedOnWorldTeardownTest,
	"CrowdySDK.Replication.SessionClearedOnWorldTeardown", CrowdySessionContextTestFlags)
bool FCrowdySessionClearedOnWorldTeardownTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model))
	{
		return false;
	}

	// A fresh subsystem has no active session and no error.
	TestEqual(TEXT("active session starts empty"), Model->GetActiveSession(), FString());
	TestEqual(TEXT("last error starts empty"), Model->GetLastModelError(), FString());

	// Set, then read back.
	Model->SetActiveSession(TEXT("session-42"));
	TestEqual(TEXT("active session set"), Model->GetActiveSession(), FString(TEXT("session-42")));
	Model->SetLastModelError(TEXT("boom"));
	TestEqual(TEXT("last error set"), Model->GetLastModelError(), FString(TEXT("boom")));

	// The explicit clear empties the active session (the last error is left for a UI to read until overwritten).
	Model->ClearActiveSession();
	TestEqual(TEXT("active session cleared explicitly"), Model->GetActiveSession(), FString());

	// The teardown path clears both per-world members, so a re-used subsystem object never leaks a stale session or
	// error into the next world.
	Model->SetActiveSession(TEXT("session-99"));
	Model->SetLastModelError(TEXT("late error"));
	Model->Deinitialize();
	TestEqual(TEXT("active session cleared on teardown"), Model->GetActiveSession(), FString());
	TestEqual(TEXT("last error cleared on teardown"), Model->GetLastModelError(), FString());

	return true;
}

// A map travel tears one world's subsystem down and initializes another's; the active session rides the game
// instance between them, so the next map's placed entities register inside it. A left or ended session is
// forgotten so it does not follow the player, and an unrelated session's leave forgets nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySessionSurvivesTravelTest,
	"CrowdySDK.Replication.SessionSurvivesTravel", CrowdySessionContextTestFlags)
bool FCrowdySessionSurvivesTravelTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>(GetTransientPackage()));
	UCrowdyActiveSessionMemory* Memory = NewObject<UCrowdyActiveSessionMemory>(Instance.Get());
	UCrowdyGameModelSubsystem* LobbyWorld = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	UCrowdyGameModelSubsystem* MatchWorld = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	if (!TestNotNull(TEXT("memory"), Memory) || !TestNotNull(TEXT("lobby"), LobbyWorld) || !TestNotNull(TEXT("match"), MatchWorld))
	{
		return false;
	}

	LobbyWorld->AttachSessionMemoryForTest(Memory);
	TestEqual(TEXT("nothing to restore at first"), LobbyWorld->GetActiveSession(), FString());
	LobbyWorld->SetActiveSession(TEXT("s-1"));
	TestEqual(TEXT("memory mirrors the set"), Memory->SessionId, FString(TEXT("s-1")));
	LobbyWorld->Deinitialize();
	TestEqual(TEXT("the old world's copy is cleared"), LobbyWorld->GetActiveSession(), FString());
	TestEqual(TEXT("the memory is not"), Memory->SessionId, FString(TEXT("s-1")));

	MatchWorld->AttachSessionMemoryForTest(Memory);
	TestEqual(TEXT("the next world restores the session"), MatchWorld->GetActiveSession(), FString(TEXT("s-1")));

	MatchWorld->ForgetActiveSessionIfForTest(TEXT("s-other"));
	TestEqual(TEXT("leaving another session keeps the active one"), MatchWorld->GetActiveSession(), FString(TEXT("s-1")));
	MatchWorld->ForgetActiveSessionIfForTest(TEXT("s-1"));
	TestEqual(TEXT("leaving the active session forgets it"), MatchWorld->GetActiveSession(), FString());
	TestEqual(TEXT("and the memory too"), Memory->SessionId, FString());

	UCrowdyGameModelSubsystem* LaterWorld = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	LaterWorld->AttachSessionMemoryForTest(Memory);
	TestEqual(TEXT("a later world inherits nothing"), LaterWorld->GetActiveSession(), FString());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
