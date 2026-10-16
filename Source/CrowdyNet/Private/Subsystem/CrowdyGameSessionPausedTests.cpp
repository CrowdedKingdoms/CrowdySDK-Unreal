#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/GameInstance.h"
#include "Misc/AutomationTest.h"
#include "Subsystem/CrowdyGameSession.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

// The paused state follows what the last SetAppPaused said: the flag and the reason move together, and saying the same thing again changes nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameSessionAppPausedStateTest,
	"CrowdySDK.CrowdyNet.GameSessionAppPausedState",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FCrowdyGameSessionAppPausedStateTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>(GetTransientPackage()));
	if (!TestTrue(TEXT("a game instance was created to own the session"), Instance.IsValid()))
	{
		return false;
	}
	const TStrongObjectPtr<UCrowdyGameSession> Session(NewObject<UCrowdyGameSession>(Instance.Get()));
	if (!TestTrue(TEXT("a session was created"), Session.IsValid()))
	{
		return false;
	}

	TestFalse(TEXT("a new session is not paused"), Session->IsAppPaused());
	TestTrue(TEXT("and has no reason"), Session->GetAppPausedReason().IsEmpty());

	Session->SetAppPaused(true, TEXT("out of funds"));
	TestTrue(TEXT("pausing sets the flag"), Session->IsAppPaused());
	TestEqualSensitive(TEXT("and the reason"), Session->GetAppPausedReason(), FString(TEXT("out of funds")));

	Session->SetAppPaused(true, TEXT("switched off by its owner"));
	TestTrue(TEXT("a new reason keeps it paused"), Session->IsAppPaused());
	TestEqualSensitive(TEXT("and replaces the reason"), Session->GetAppPausedReason(), FString(TEXT("switched off by its owner")));

	Session->SetAppPaused(true, TEXT("Switched off by its owner"));
	TestEqualSensitive(TEXT("a reason differing only in case is a change"), Session->GetAppPausedReason(), FString(TEXT("Switched off by its owner")));

	Session->SetAppPaused(false, FString());
	TestFalse(TEXT("resuming clears the flag"), Session->IsAppPaused());
	TestTrue(TEXT("and the reason"), Session->GetAppPausedReason().IsEmpty());
	return true;
}

#endif
