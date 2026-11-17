#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/GameInstance.h"
#include "Misc/AutomationTest.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Subsystem/CrowdySDKSubsystem.h"
#include "Subsystem/CrowdyStudioAgent.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "Utils/HelperFunctions.h"

// A bare subsystem with no Leader subsystem listening: Logout must clear the session's host id itself, at once.
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

// The agent's sign-out hand-off rides on the SDK's Logout, so a Logout that stopped calling it must fail here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySDKLogoutHandsTheStudioAgentAnEmptyTokenTest,
	"CrowdySDK.SDK.LogoutHandsTheStudioAgentAnEmptyToken",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdySDKLogoutHandsTheStudioAgentAnEmptyTokenTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>(GetTransientPackage()));
	UCrowdyGameSession* Session = NewObject<UCrowdyGameSession>(Instance.Get());
	Session->SetUserID(7);
	Session->SetGameToken(TEXT("app-token"));

	UCrowdyStudioAgent* Agent = NewObject<UCrowdyStudioAgent>(Instance.Get());
	Agent->InitializeForTest(nullptr, Session, nullptr);
	TArray<FString> Tokens;
	int64 LastUser = 0;
	Agent->OnAgentTokenChanged.AddLambda([&Tokens, &LastUser](int64 UserId, const FString& Token)
	{
		Tokens.Add(Token);
		LastUser = UserId;
	});

	UCrowdySDKSubsystem* Sdk = NewObject<UCrowdySDKSubsystem>(Instance.Get());
	Sdk->SetGameSessionForTest(Session);
	Sdk->SetStudioAgentForTest(Agent);

	Sdk->Logout();
	if (!TestEqual(TEXT("Logout fires the agent's token event once"), Tokens.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("with an empty token"), Tokens[0].IsEmpty());
	TestEqual(TEXT("for the user who left"), LastUser, static_cast<int64>(7));
	return true;
}

#endif
