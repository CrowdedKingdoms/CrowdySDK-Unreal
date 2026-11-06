#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/GameInstance.h"
#include "Misc/AutomationTest.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Subsystem/CrowdySDKSubsystem.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "Utils/HelperFunctions.h"

// A bare subsystem with no world stands in for signing out on a map whose host subsystem never saw the host: after a
// level travel, or on a map without networking. Logout must clear the session's host itself, at once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySDKLogoutClearsSessionHostTest,
	"CrowdySDK.SDK.LogoutClearsSessionHost",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdySDKLogoutClearsSessionHostTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>(GetTransientPackage()));
	UCrowdyGameSession* Session = NewObject<UCrowdyGameSession>(Instance.Get());
	Session->SetUserID(7);
	Session->SetHostID(UHelperFunctions::GetDeterministicID(42));

	UCrowdySDKSubsystem* Sdk = NewObject<UCrowdySDKSubsystem>(Instance.Get());
	Sdk->SetGameSessionForTest(Session);

	Sdk->Logout();
	TestFalse(TEXT("the session holds no host as soon as Logout returns"), Session->GetAppWriterID().IsValid());
	TestEqual(TEXT("the signed-in user is cleared"), Session->GetUserID(), static_cast<int64>(0));
	return true;
}

#endif
