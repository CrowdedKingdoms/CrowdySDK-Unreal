#include "CrowdySDKAuthForwardingTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/GameInstance.h"
#include "Misc/AutomationTest.h"
#include "Subsystem/CrowdyAuthentication.h"
#include "Subsystem/CrowdySDKSubsystem.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

// The subsystem and the authentication subsystem here are bare objects (never initialised), so the test drives the
// delegate itself rather than a real restore, which would read the saved session on disk.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySDKSessionRestoreFailureReachesOnLoginTest,
	"CrowdySDK.SDK.SessionRestoreFailureReachesOnLogin",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdySDKSessionRestoreFailureReachesOnLoginTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>(GetTransientPackage()));
	UCrowdySDKSubsystem* Sdk = NewObject<UCrowdySDKSubsystem>(Instance.Get());
	UCrowdyAuthentication* Auth = NewObject<UCrowdyAuthentication>(Instance.Get());
	const TStrongObjectPtr<UCrowdySDKLoginRecorder> Recorder(
		NewObject<UCrowdySDKLoginRecorder>(GetTransientPackage(), NAME_None, RF_Transient));

	Sdk->OnLogin.AddDynamic(Recorder.Get(), &UCrowdySDKLoginRecorder::HandleLogin);
	Sdk->BindAuthenticationForTest(Auth);

	Auth->OnSessionRestoreFailed.Broadcast(TEXT("No saved session found"));
	Auth->OnSessionRestoreFailed.Broadcast(TEXT("The session expired"));

	if (!TestEqual(TEXT("each failure reaches OnLogin"), Recorder->Messages.Num(), 2))
	{
		return false;
	}

	TestFalse(TEXT("as a failed login"), Recorder->Successes[0] || Recorder->Successes[1]);
	TestEqual(TEXT("with the missing session's message"), Recorder->Messages[0], TEXT("No saved session found"));
	TestEqual(TEXT("and the expired one's"), Recorder->Messages[1], TEXT("The session expired"));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
