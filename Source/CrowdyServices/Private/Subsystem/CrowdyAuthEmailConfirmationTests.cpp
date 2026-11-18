#include "CrowdyAuthTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyCppClient.h"
#include "Engine/GameInstance.h"
#include "Misc/AutomationTest.h"
#include "Subsystem/CrowdyAuthentication.h"
#include "Subsystem/CrowdyGameSession.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	constexpr EAutomationTestFlags EmailConfirmationTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	const TCHAR* const UnconfirmedAnswer = TEXT(
		"{\"errors\":[{\"message\":\"Confirm your email first.\",\"extensions\":{\"code\":\"EMAIL_CONFIRMATION_REQUIRED\"}}],\"data\":null}");

	// Far-out expiry so the rotation it arms cannot fire during a test.
	const TCHAR* const MintedAnswer = TEXT(
		"{\"data\":{\"mintAppToken\":{\"token\":\"app-xyz\",\"gameTokenId\":\"5150\",\"appId\":\"31337\","
		"\"expiresAt\":\"2099-01-01T00:00:00Z\",\"gameApiUrl\":\"https://game.test\","
		"\"gameApiWsUrl\":\"wss://game.test\",\"launchUrl\":\"https://launch.test\"}}}");

	struct FEmailConfirmationRig
	{
		TStrongObjectPtr<UGameInstance> Instance;
		TStrongObjectPtr<UCrowdyAuthentication> Auth;
		TStrongObjectPtr<UCrowdyGameSession> Session;
		TStrongObjectPtr<UCrowdyAuthTestListener> Listener;
		TSharedPtr<FCrowdyCppClient> Client;

		explicit FEmailConfirmationRig(TArray<TPair<int32, FString>> Script)
			: Instance(NewObject<UGameInstance>(GetTransientPackage(), NAME_None, RF_Transient))
			, Auth(NewObject<UCrowdyAuthentication>(Instance.Get()))
			, Session(NewObject<UCrowdyGameSession>(Instance.Get()))
			, Listener(NewObject<UCrowdyAuthTestListener>(GetTransientPackage(), NAME_None, RF_Transient))
			, Client(FCrowdyCppClient::MakeForTest(MintedAnswer, 200))
		{
			if (!Client.IsValid())
			{
				return;
			}
			Client->SetTestResponseScript(MoveTemp(Script));
			Client->SetManagementToken(TEXT("session-abc"));
			Client->SetGameToken(TEXT("app-old"));

			Session->SetAppID(31337);
			Session->SetUserID(7781);
			Session->SetSessionToken(TEXT("session-abc"));
			Session->SetGameToken(TEXT("app-old"));
			Auth->InitializeForTest(Client, Session.Get());

			Auth->OnEmailConfirmationRequired.AddDynamic(Listener.Get(), &UCrowdyAuthTestListener::HandleEmailConfirmationRequired);
			Auth->OnLoginFailed.AddDynamic(Listener.Get(), &UCrowdyAuthTestListener::HandleLoginFailed);
		}

		~FEmailConfirmationRig()
		{
			Auth->Deinitialize();
		}

		void Drain()
		{
			for (int32 Pass = 0; Client && Pass < 8 && Client->NumPendingRequests() > 0; ++Pass)
			{
				Client->Poll();
			}
		}

		void Mint()
		{
			FOnAuthSuccess OnSuccess;
			OnSuccess.BindDynamic(Listener.Get(), &UCrowdyAuthTestListener::HandleFlowSuccess);
			FOnAuthError OnError;
			OnError.BindDynamic(Listener.Get(), &UCrowdyAuthTestListener::HandleFlowError);
			Auth->MintForTest(OnSuccess, OnError);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEmailConfirmationFailsTheSignInTest,
	"CrowdySDK.Auth.EmailConfirmation.FailsTheSignIn", EmailConfirmationTestFlags)

bool FCrowdyEmailConfirmationFailsTheSignInTest::RunTest(const FString& Parameters)
{
	FEmailConfirmationRig Rig({ { 403, UnconfirmedAnswer } });
	if (!TestTrue(TEXT("the test client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Mint();
	Rig.Drain();

	TestEqual(TEXT("the game is told once"), Rig.Listener->EmailConfirmationRequests, 1);
	TestEqual(TEXT("the sign-in fails once"), Rig.Listener->FlowErrors.Num(), 1);
	TestTrue(TEXT("with words that say to confirm the email"),
		Rig.Listener->FlowErrors.Num() == 1 && Rig.Listener->FlowErrors[0].Contains(TEXT("Confirm your email")));
	TestEqual(TEXT("and the login failure is broadcast"), Rig.Listener->LoginFailures.Num(), 1);
	TestFalse(TEXT("access is not ended: confirming fixes it"), Rig.Session->HasAccessEnded());
	TestFalse(TEXT("nothing re-arms a rotation"), Rig.Auth->IsRotationTimerArmedForTest());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEmailConfirmationStopsRotationTest,
	"CrowdySDK.Auth.EmailConfirmation.StopsRotation", EmailConfirmationTestFlags)

bool FCrowdyEmailConfirmationStopsRotationTest::RunTest(const FString& Parameters)
{
	FEmailConfirmationRig Rig({ { 200, MintedAnswer }, { 403, UnconfirmedAnswer } });
	if (!TestTrue(TEXT("the test client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Mint();
	Rig.Drain();
	if (!TestTrue(TEXT("a minted token arms the rotation"), Rig.Auth->IsRotationTimerArmedForTest()))
	{
		return false;
	}

	Rig.Auth->RefreshForTest();
	Rig.Drain();

	TestEqual(TEXT("a refused rotation tells the game"), Rig.Listener->EmailConfirmationRequests, 1);
	TestFalse(TEXT("the rotation stops"), Rig.Auth->IsRotationTimerArmedForTest());
	TestEqual(TEXT("and nothing re-mints"), Rig.Client->NumPendingRequests(), 0);
	TestFalse(TEXT("access is not ended"), Rig.Session->HasAccessEnded());

	// The connection asks for a refresh near expiry, and an expired token asks for recovery: neither may re-mint.
	Rig.Auth->RefreshAppToken();
	TestEqual(TEXT("a requested refresh sends nothing"), Rig.Client->NumPendingRequests(), 0);
	Rig.Auth->RecoverExpiredAppToken();
	TestEqual(TEXT("an expired-token recovery sends nothing"), Rig.Client->NumPendingRequests(), 0);
	TestEqual(TEXT("and the game is not told again"), Rig.Listener->EmailConfirmationRequests, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEmailConfirmationClearsOnSignInTest,
	"CrowdySDK.Auth.EmailConfirmation.ClearsOnSignIn", EmailConfirmationTestFlags)

bool FCrowdyEmailConfirmationClearsOnSignInTest::RunTest(const FString& Parameters)
{
	FEmailConfirmationRig Rig({ { 403, UnconfirmedAnswer }, { 200, MintedAnswer } });
	if (!TestTrue(TEXT("the test client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Mint();
	Rig.Drain();
	Rig.Mint();
	Rig.Drain();
	TestEqual(TEXT("the second sign-in, after confirming, succeeds"), Rig.Listener->FlowSuccesses, 1);

	Rig.Auth->RefreshAppToken();
	TestEqual(TEXT("and token refresh works again"), Rig.Client->NumPendingRequests(), 1);
	Rig.Drain();
	return true;
}

#endif
