#include "CrowdyAuthTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyCppClient.h"
#include "Engine/GameInstance.h"
#include "Misc/AutomationTest.h"
#include "Subsystem/CrowdyAuthentication.h"
#include "Subsystem/CrowdyGameSession.h"
#include "UI/CrowdyConsentPromptSubsystem.h"
#include "UObject/StrongObjectPtr.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

// A token refused until the player records their consent holds the flow that asked for it instead of failing it, and
// the player's answer decides how it ends. Everything here runs against a scripted client, so the only network the
// tests see is the sequence of answers each one scripts.
namespace
{
	constexpr EAutomationTestFlags ConsentTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	const TCHAR* const ConsentRefusal = TEXT(
		"{\"errors\":[{\"message\":\"Accept the current terms first.\","
		"\"extensions\":{\"code\":\"LEGAL_ACCEPTANCE_REQUIRED\"}}],\"data\":null}");

	// The client unwraps an answer's data only when it holds a single field, so each operation gets its own body.
	const TCHAR* const ConsentRecorded = TEXT("{\"data\":{\"recordPlayerConsents\":true}}");

	// Answers whatever is not scripted. The expiry is far out so the rotation timer a successful mint arms cannot fire
	// during a test.
	const TCHAR* const MintAnswer = TEXT(
		"{\"data\":{\"mintAppToken\":{\"token\":\"app-xyz\",\"gameTokenId\":\"5150\",\"appId\":\"31337\","
		"\"expiresAt\":\"2099-01-01T00:00:00Z\",\"gameApiUrl\":\"https://game.test\","
		"\"gameApiWsUrl\":\"wss://game.test\",\"launchUrl\":\"https://launch.test\"}}}");

	/** Sets bShowConsentPrompt for its lifetime, then restores it. */
	struct FConsentPromptSettingScope
	{
		UCrowdySDKDeveloperSettings* Settings = GetMutableDefault<UCrowdySDKDeveloperSettings>();
		bool bPrevious = Settings->bShowConsentPrompt;

		explicit FConsentPromptSettingScope(bool bShow)
		{
			Settings->bShowConsentPrompt = bShow;
		}

		~FConsentPromptSettingScope()
		{
			Settings->bShowConsentPrompt = bPrevious;
		}
	};

	/** An authentication subsystem with a stored session token, whose every call is answered by a scripted client. */
	struct FConsentRig
	{
		TStrongObjectPtr<UGameInstance> Instance;
		TStrongObjectPtr<UCrowdyAuthentication> Auth;
		TStrongObjectPtr<UCrowdyGameSession> Session;
		TStrongObjectPtr<UCrowdyAuthTestListener> Listener;
		TSharedPtr<FCrowdyCppClient> Client;

		explicit FConsentRig(int32 Refusals, bool bAnswerConsent = true)
			: Instance(NewObject<UGameInstance>(GetTransientPackage(), NAME_None, RF_Transient))
			, Auth(NewObject<UCrowdyAuthentication>(Instance.Get()))
			, Session(NewObject<UCrowdyGameSession>(Instance.Get()))
			, Listener(NewObject<UCrowdyAuthTestListener>(GetTransientPackage(), NAME_None, RF_Transient))
			, Client(FCrowdyCppClient::MakeForTest(MintAnswer, 200))
		{
			if (!Client.IsValid())
			{
				return;
			}

			TArray<TPair<int32, FString>> Script;
			for (int32 Index = 0; Index < Refusals; ++Index)
			{
				Script.Emplace(403, FString(ConsentRefusal));
			}
			if (bAnswerConsent)
			{
				Script.Emplace(200, FString(ConsentRecorded));
			}
			Client->SetTestResponseScript(MoveTemp(Script));
			Client->SetManagementToken(TEXT("session-abc"));
			Client->SetGameToken(TEXT("app-old"));

			Session->SetAppID(31337);
			Session->SetUserID(7781);
			Session->SetSessionToken(TEXT("session-abc"));
			Session->SetGameToken(TEXT("app-old"));
			Auth->InitializeForTest(Client, Session.Get());

			Auth->OnLegalAcceptanceRequired.AddDynamic(Listener.Get(), &UCrowdyAuthTestListener::HandleLegalAcceptanceRequired);
			Auth->OnLegalAcceptanceEnded.AddDynamic(Listener.Get(), &UCrowdyAuthTestListener::HandleLegalAcceptanceEnded);
			Auth->OnLogin.AddDynamic(Listener.Get(), &UCrowdyAuthTestListener::HandleLogin);
			Auth->OnLoginFailed.AddDynamic(Listener.Get(), &UCrowdyAuthTestListener::HandleLoginFailed);
			Auth->OnAppTokenRefreshed.AddDynamic(Listener.Get(), &UCrowdyAuthTestListener::HandleRefreshed);
		}

		// Cancels any rotation timer a test armed, so nothing outlives the test on the core ticker.
		~FConsentRig()
		{
			Auth->Deinitialize();
		}

		/** Delivers every answer, including those to requests a completion sends. */
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

		void Consent(bool bAcceptLegal = true, bool bAttestAgeOfMajority = true)
		{
			FOnLegalConsentsRecorded OnRecorded;
			OnRecorded.BindDynamic(Listener.Get(), &UCrowdyAuthTestListener::HandleConsentRecorded);
			FOnAuthError OnError;
			OnError.BindDynamic(Listener.Get(), &UCrowdyAuthTestListener::HandleConsentError);
			Auth->RecordPlayerConsents(bAcceptLegal, bAttestAgeOfMajority, OnRecorded, OnError);
		}

		FString LastRequestBody() const
		{
			FString Body;
			Client->GetLastTestRequestBody(Body);
			return Body;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthRefusedMintWaitsForConsentTest,
	"CrowdySDK.Auth.RefusedMintWaitsForConsent", ConsentTestFlags)

bool FCrowdyAuthRefusedMintWaitsForConsentTest::RunTest(const FString& Parameters)
{
	FConsentRig Rig(1);
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Mint();
	Rig.Drain();

	TestEqual(TEXT("the refusal is announced once"), Rig.Listener->LegalAcceptanceRequests, 1);
	TestTrue(TEXT("the sign-in is held"), Rig.Auth->IsLegalAcceptancePending());
	TestEqual(TEXT("the held sign-in has not failed"), Rig.Listener->FlowErrors.Num(), 0);
	TestEqual(TEXT("nor announced a failure"), Rig.Listener->LoginFailures.Num(), 0);
	TestEqual(TEXT("nor succeeded"), Rig.Listener->FlowSuccesses, 0);
	TestEqual(TEXT("the hold has not ended"), Rig.Listener->LegalAcceptanceEnds, 0);

	Rig.Consent();
	Rig.Drain();

	TestEqual(TEXT("the consent call reported no error"), Rig.Listener->ConsentErrors.Num(), 0);
	TestEqual(TEXT("the hold ends once"), Rig.Listener->LegalAcceptanceEnds, 1);
	TestEqual(TEXT("the consent is reported recorded"), Rig.Listener->ConsentsRecorded, 1);
	TestTrue(TEXT("the last request is the re-mint"), Rig.LastRequestBody().Contains(TEXT("mintAppToken")));
	TestEqual(TEXT("the original sign-in completes"), Rig.Listener->FlowSuccesses, 1);
	TestEqual(TEXT("through its usual event"), Rig.Listener->Logins, 1);
	TestTrue(TEXT("with the minted token installed"),
		Rig.Session->GetGameToken().Equals(TEXT("app-xyz"), ESearchCase::CaseSensitive));
	TestFalse(TEXT("nothing is held any more"), Rig.Auth->IsLegalAcceptancePending());
	TestEqual(TEXT("and nothing was announced again"), Rig.Listener->LegalAcceptanceRequests, 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthDeclinedConsentFailsTheSignInTest,
	"CrowdySDK.Auth.DeclinedConsentFailsTheSignIn", ConsentTestFlags)

bool FCrowdyAuthDeclinedConsentFailsTheSignInTest::RunTest(const FString& Parameters)
{
	FConsentRig Rig(1);
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Mint();
	Rig.Drain();
	if (!TestTrue(TEXT("the sign-in is held"), Rig.Auth->IsLegalAcceptancePending()))
	{
		return false;
	}

	Rig.Auth->DeclineLegalAcceptance();

	TestEqual(TEXT("the held sign-in fails"), Rig.Listener->FlowErrors.Num(), 1);
	TestEqual(TEXT("and announces it"), Rig.Listener->LoginFailures.Num(), 1);
	TestTrue(TEXT("with a reason the player can read"),
		Rig.Listener->FlowErrors.Num() == 1 && Rig.Listener->FlowErrors[0].Contains(TEXT("terms")));
	TestFalse(TEXT("nothing is held any more"), Rig.Auth->IsLegalAcceptancePending());
	TestEqual(TEXT("which is announced once"), Rig.Listener->LegalAcceptanceEnds, 1);
	TestEqual(TEXT("and nothing more is sent"), Rig.Client->NumPendingRequests(), 0);
	TestEqual(TEXT("the sign-in never succeeded"), Rig.Listener->FlowSuccesses, 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthRefusedRefreshIsTerminalForRotationTest,
	"CrowdySDK.Auth.RefusedRefreshIsTerminalForRotation", ConsentTestFlags)

bool FCrowdyAuthRefusedRefreshIsTerminalForRotationTest::RunTest(const FString& Parameters)
{
	FConsentRig Rig(1);
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	// Named, so a refusal that reached the counting below would be counted against the argument.
	Rig.Session->SetReplicationServer(TEXT("10.0.0.1"), 7777);

	Rig.Auth->RefreshAppToken();
	Rig.Drain();

	TestEqual(TEXT("the refusal is announced"), Rig.Listener->LegalAcceptanceRequests, 1);
	TestTrue(TEXT("the refresh is held"), Rig.Auth->IsLegalAcceptancePending());
	TestFalse(TEXT("no rotation retry is armed"), Rig.Auth->IsRotationTimerArmedForTest());
	TestEqual(TEXT("no retry was spent"), Rig.Auth->GetRotationRetriesUsedForTest(), 0);
	TestEqual(TEXT("the refusal is not counted against naming the server"),
		Rig.Auth->GetServerNamedRefusalsForTest(), 0);
	TestFalse(TEXT("and no re-mint was sent"), Rig.LastRequestBody().Contains(TEXT("mintAppToken")));

	// Every rotation trigger is a no-op while the refresh is held.
	Rig.Auth->RefreshAppToken();
	Rig.Auth->RecoverExpiredAppToken();
	TestEqual(TEXT("no trigger sends anything while held"), Rig.Client->NumPendingRequests(), 0);
	TestEqual(TEXT("nor announces again"), Rig.Listener->LegalAcceptanceRequests, 1);

	Rig.Consent();
	Rig.Drain();

	TestEqual(TEXT("the consent is reported recorded"), Rig.Listener->ConsentsRecorded, 1);
	TestEqual(TEXT("the held rotation completes as a refresh"), Rig.Listener->Refreshes, 1);
	TestFalse(TEXT("nothing is held any more"), Rig.Auth->IsLegalAcceptancePending());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthSecondRefusalIsNotAnnouncedAgainTest,
	"CrowdySDK.Auth.SecondRefusalIsNotAnnouncedAgain", ConsentTestFlags)

bool FCrowdyAuthSecondRefusalIsNotAnnouncedAgainTest::RunTest(const FString& Parameters)
{
	FConsentRig Rig(2);
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	// A rotation and a sign-in in flight together, both refused.
	Rig.Auth->RefreshAppToken();
	Rig.Mint();
	TestEqual(TEXT("both requests are in flight"), Rig.Client->NumPendingRequests(), 2);
	Rig.Drain();

	TestEqual(TEXT("the player is asked once"), Rig.Listener->LegalAcceptanceRequests, 1);
	TestTrue(TEXT("a flow is held"), Rig.Auth->IsLegalAcceptancePending());
	TestEqual(TEXT("the sign-in has not failed"), Rig.Listener->FlowErrors.Num(), 0);
	TestEqual(TEXT("nor announced a failure"), Rig.Listener->LoginFailures.Num(), 0);
	TestEqual(TEXT("taking over the hold does not end it"), Rig.Listener->LegalAcceptanceEnds, 0);

	Rig.Consent();
	Rig.Drain();

	// The sign-in, not the rotation, is the held flow, so its caller is the one answered.
	TestEqual(TEXT("the sign-in completes"), Rig.Listener->FlowSuccesses, 1);
	TestEqual(TEXT("through its usual event"), Rig.Listener->Logins, 1);
	TestEqual(TEXT("still asked only once"), Rig.Listener->LegalAcceptanceRequests, 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthRefusedRefreshBehindAHeldSignInTest,
	"CrowdySDK.Auth.RefusedRefreshBehindAHeldSignIn", ConsentTestFlags)

bool FCrowdyAuthRefusedRefreshBehindAHeldSignInTest::RunTest(const FString& Parameters)
{
	FConsentRig Rig(2);
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	// The sign-in is answered first, so the rotation's refusal arrives while the sign-in is already held.
	Rig.Mint();
	Rig.Auth->RefreshForTest();
	TestEqual(TEXT("both requests are in flight"), Rig.Client->NumPendingRequests(), 2);
	Rig.Drain();

	TestEqual(TEXT("the player is asked once"), Rig.Listener->LegalAcceptanceRequests, 1);
	TestEqual(TEXT("the held sign-in is not failed by the refresh"), Rig.Listener->FlowErrors.Num(), 0);
	TestEqual(TEXT("nor announced as failed"), Rig.Listener->LoginFailures.Num(), 0);

	Rig.Consent();
	Rig.Drain();

	TestEqual(TEXT("the sign-in completes"), Rig.Listener->FlowSuccesses, 1);
	TestEqual(TEXT("through its usual event"), Rig.Listener->Logins, 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthAppliedTokenEndsAHeldRefreshTest,
	"CrowdySDK.Auth.AppliedTokenEndsAHeldRefresh", ConsentTestFlags)

bool FCrowdyAuthAppliedTokenEndsAHeldRefreshTest::RunTest(const FString& Parameters)
{
	FConsentRig Rig(1, false);
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Auth->RefreshAppToken();
	Rig.Drain();
	if (!TestTrue(TEXT("the refresh is held"), Rig.Auth->IsLegalAcceptancePending()))
	{
		return false;
	}

	// A sign-in that started earlier lands with a fresh token while the rotation waits.
	Rig.Mint();
	Rig.Drain();

	TestEqual(TEXT("the sign-in completes"), Rig.Listener->FlowSuccesses, 1);
	TestFalse(TEXT("the held rotation is released"), Rig.Auth->IsLegalAcceptancePending());
	TestEqual(TEXT("and the hold is announced as ended"), Rig.Listener->LegalAcceptanceEnds, 1);
	TestTrue(TEXT("so rotation is armed again"), Rig.Auth->IsRotationTimerArmedForTest());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthConsentNeedsBothAnswersTest,
	"CrowdySDK.Auth.ConsentNeedsBothAnswers", ConsentTestFlags)

bool FCrowdyAuthConsentNeedsBothAnswersTest::RunTest(const FString& Parameters)
{
	FConsentRig Rig(0);
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Consent(true, false);
	Rig.Consent(false, true);

	TestEqual(TEXT("each half-answer is refused"), Rig.Listener->ConsentErrors.Num(), 2);
	TestEqual(TEXT("nothing is reported recorded"), Rig.Listener->ConsentsRecorded, 0);
	FString Url;
	FString Authorization;
	TestFalse(TEXT("and nothing is sent"), Rig.Client->GetLastTestRequest(Url, Authorization));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthLegalDocumentsAreListedTest,
	"CrowdySDK.Auth.LegalDocumentsAreListed", ConsentTestFlags)

bool FCrowdyAuthLegalDocumentsAreListedTest::RunTest(const FString& Parameters)
{
	const TArray<FCrowdyLegalDocument> Documents = UCrowdyAuthentication::GetLegalDocuments();

	TestEqual(TEXT("five documents"), Documents.Num(), 5);
	for (const FCrowdyLegalDocument& Document : Documents)
	{
		TestFalse(TEXT("each has a title"), Document.Title.IsEmpty());
		TestTrue(FString::Printf(TEXT("%s is an https link"), *Document.Url),
			Document.Url.StartsWith(TEXT("https://crowdedkingdoms.com/"), ESearchCase::CaseSensitive));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthHeadlessProcessFailsTheHeldSignInTest,
	"CrowdySDK.Auth.HeadlessProcessFailsTheHeldSignIn", ConsentTestFlags)

bool FCrowdyAuthHeadlessProcessFailsTheHeldSignInTest::RunTest(const FString& Parameters)
{
	FConsentRig Rig(1);
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	// The game instance has no viewport, so the prompt cannot open; it is the only listener, so nobody else can ask.
	const FConsentPromptSettingScope ShowPrompt(true);
	const TStrongObjectPtr<UCrowdyConsentPromptSubsystem> Prompt(
		NewObject<UCrowdyConsentPromptSubsystem>(Rig.Instance.Get()));
	Prompt->AuthenticationForTest = Rig.Auth.Get();
	Rig.Auth->OnLegalAcceptanceRequired.RemoveAll(Rig.Listener.Get());
	Rig.Auth->OnLegalAcceptanceRequired.AddDynamic(Prompt.Get(), &UCrowdyConsentPromptSubsystem::HandleLegalAcceptanceRequired);

	Rig.Mint();
	Rig.Drain();

	TestEqual(TEXT("the held sign-in fails"), Rig.Listener->FlowErrors.Num(), 1);
	TestEqual(TEXT("and announces it"), Rig.Listener->LoginFailures.Num(), 1);
	TestFalse(TEXT("nothing is held any more"), Rig.Auth->IsLegalAcceptancePending());
	TestEqual(TEXT("which is announced once"), Rig.Listener->LegalAcceptanceEnds, 1);
	TestEqual(TEXT("the sign-in never succeeded"), Rig.Listener->FlowSuccesses, 0);

	Rig.Auth->OnLegalAcceptanceRequired.RemoveAll(Prompt.Get());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthHeadlessProcessKeepsTheHoldForAnotherListenerTest,
	"CrowdySDK.Auth.HeadlessProcessKeepsTheHoldForAnotherListener", ConsentTestFlags)

bool FCrowdyAuthHeadlessProcessKeepsTheHoldForAnotherListenerTest::RunTest(const FString& Parameters)
{
	FConsentRig Rig(1);
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	// The rig's listener stays bound, standing in for a game that shows its own consent screen.
	const FConsentPromptSettingScope ShowPrompt(true);
	const TStrongObjectPtr<UCrowdyConsentPromptSubsystem> Prompt(
		NewObject<UCrowdyConsentPromptSubsystem>(Rig.Instance.Get()));
	Prompt->AuthenticationForTest = Rig.Auth.Get();
	Rig.Auth->OnLegalAcceptanceRequired.AddDynamic(Prompt.Get(), &UCrowdyConsentPromptSubsystem::HandleLegalAcceptanceRequired);

	Rig.Mint();
	Rig.Drain();

	TestEqual(TEXT("the other listener is asked"), Rig.Listener->LegalAcceptanceRequests, 1);
	TestTrue(TEXT("the sign-in stays held"), Rig.Auth->IsLegalAcceptancePending());
	TestEqual(TEXT("it has not failed"), Rig.Listener->FlowErrors.Num(), 0);
	TestEqual(TEXT("nor announced a failure"), Rig.Listener->LoginFailures.Num(), 0);

	Rig.Auth->OnLegalAcceptanceRequired.RemoveAll(Prompt.Get());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthMintCompletingAfterLogoutIsDroppedTest,
	"CrowdySDK.Auth.MintCompletingAfterLogoutIsDropped", ConsentTestFlags)

bool FCrowdyAuthMintCompletingAfterLogoutIsDroppedTest::RunTest(const FString& Parameters)
{
	FConsentRig Rig(0, false);
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	// Logout deletes the saved session, so this instance gets a vault and a slot of its own.
	Rig.Auth->SetInstanceSuffixForTest(TEXT(".autotest-stale-mint"));

	Rig.Mint();
	Rig.Auth->ClearSavedSession();
	Rig.Drain();

	TestEqual(TEXT("no login failure is broadcast into the next sign-in"), Rig.Listener->LoginFailures.Num(), 0);
	TestEqual(TEXT("the sign-in does not succeed"), Rig.Listener->FlowSuccesses, 0);
	TestEqual(TEXT("nor announce a login"), Rig.Listener->Logins, 0);
	TestEqual(TEXT("it fails once instead"), Rig.Listener->FlowErrors.Num(), 1);
	TestTrue(TEXT("the game token is unchanged"),
		Rig.Session->GetGameToken().Equals(TEXT("app-old"), ESearchCase::CaseSensitive));
	TestFalse(TEXT("no rotation is armed"), Rig.Auth->IsRotationTimerArmedForTest());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthMintAfterConsentCompletingAfterLogoutIsDroppedTest,
	"CrowdySDK.Auth.MintAfterConsentCompletingAfterLogoutIsDropped", ConsentTestFlags)

bool FCrowdyAuthMintAfterConsentCompletingAfterLogoutIsDroppedTest::RunTest(const FString& Parameters)
{
	FConsentRig Rig(1);
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Auth->SetInstanceSuffixForTest(TEXT(".autotest-stale-remint"));

	Rig.Mint();
	Rig.Drain();
	if (!TestTrue(TEXT("the sign-in is held"), Rig.Auth->IsLegalAcceptancePending()))
	{
		return false;
	}

	// One poll answers the consent and sends the re-mint, which is then still in flight when the player signs out.
	Rig.Consent();
	Rig.Client->Poll();
	if (!TestEqual(TEXT("the re-mint is in flight"), Rig.Client->NumPendingRequests(), 1))
	{
		return false;
	}

	Rig.Auth->ClearSavedSession();
	Rig.Drain();

	TestEqual(TEXT("no login failure is broadcast into the next sign-in"), Rig.Listener->LoginFailures.Num(), 0);
	TestEqual(TEXT("the sign-in does not succeed"), Rig.Listener->FlowSuccesses, 0);
	TestEqual(TEXT("nor announce a login"), Rig.Listener->Logins, 0);
	TestEqual(TEXT("it fails once instead"), Rig.Listener->FlowErrors.Num(), 1);
	TestTrue(TEXT("the game token is unchanged"),
		Rig.Session->GetGameToken().Equals(TEXT("app-old"), ESearchCase::CaseSensitive));
	TestFalse(TEXT("no rotation is armed"), Rig.Auth->IsRotationTimerArmedForTest());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthLogoutEndsABrowserSignInTest,
	"CrowdySDK.Auth.LogoutEndsABrowserSignIn", ConsentTestFlags)

bool FCrowdyAuthLogoutEndsABrowserSignInTest::RunTest(const FString& Parameters)
{
	FConsentRig Rig(0, false);
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Auth->SetInstanceSuffixForTest(TEXT(".autotest-logout-browser"));
	Rig.Auth->SetLoopbackFlowPendingForTest(true);
	if (!TestTrue(TEXT("a browser sign-in is under way"), Rig.Auth->IsInteractiveSignInBusyForTest()))
	{
		return false;
	}

	Rig.Auth->ClearSavedSession();

	TestFalse(TEXT("signing out ends it"), Rig.Auth->IsInteractiveSignInBusyForTest());
	TestEqual(TEXT("without announcing a failure"), Rig.Listener->LoginFailures.Num(), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
