#include "CrowdyAuthTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyCppClient.h"
#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Subsystem/CrowdyAuthDevCommands.h"
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthLinkStartAnsweringAfterLogoutIsDroppedTest,
	"CrowdySDK.Auth.LinkStartAnsweringAfterLogoutIsDropped", ConsentTestFlags)

bool FCrowdyAuthLinkStartAnsweringAfterLogoutIsDroppedTest::RunTest(const FString& Parameters)
{
	FConsentRig Rig(0, false);
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Auth->SetInstanceSuffixForTest(TEXT(".autotest-link-after-logout"));

	// An unusable authorize URL, so a start answer that is wrongly acted on ends in a different error and never opens a browser.
	TArray<TPair<int32, FString>> Script;
	Script.Emplace(200, FString(TEXT("{\"data\":{\"socialLoginStart\":{\"authorizeUrl\":\"ftp://id.test/auth\",\"state\":\"csrf-1\"}}}")));
	Rig.Client->SetTestResponseScript(MoveTemp(Script));

	FOnAuthError OnError;
	OnError.BindDynamic(Rig.Listener.Get(), &UCrowdyAuthTestListener::HandleFlowError);
	Rig.Auth->BeginLinkIdentity(TEXT("google"), FOnIdentityLinked(), OnError);
	if (!TestEqual(TEXT("the link start is in flight"), Rig.Client->NumPendingRequests(), 1))
	{
		return false;
	}

	Rig.Auth->ClearSavedSession();
	Rig.Drain();

	TestEqual(TEXT("the link fails once"), Rig.Listener->FlowErrors.Num(), 1);
	TestTrue(TEXT("because the player signed out"),
		Rig.Listener->FlowErrors.Num() == 1 && Rig.Listener->FlowErrors[0].Contains(TEXT("Signed out")));
	TestFalse(TEXT("no browser sign-in is left armed"), Rig.Auth->IsInteractiveSignInBusyForTest());

	return true;
}

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
namespace
{
	/** Sets the developer settings' app id for its lifetime, then restores it. 0 skips app discovery, so a sign-in's first request is the sign-in. */
	struct FAppIdSettingScope
	{
		UCrowdySDKDeveloperSettings* Settings = GetMutableDefault<UCrowdySDKDeveloperSettings>();
		int64 Previous = Settings->AppID;

		explicit FAppIdSettingScope(int64 AppId)
		{
			Settings->AppID = AppId;
		}

		~FAppIdSettingScope()
		{
			Settings->AppID = Previous;
		}
	};

	/** Reads a credentials file holding Content, then deletes it. */
	bool ReadCredentialsFrom(const FString& Content, FString& Email, FString& Password, FString& Error)
	{
		const FString Path = FPaths::CreateTempFilename(*FPaths::ProjectSavedDir(), TEXT("CrowdyAuthDev"), TEXT(".json"));
		FFileHelper::SaveStringToFile(Content, *Path);
		const bool bRead = CrowdyAuthDev::ReadCredentialsFile(Path, Email, Password, Error);
		IFileManager::Get().Delete(*Path);
		return bRead;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthDevCredentialsFileRefusalsTest,
	"CrowdySDK.Auth.DevCredentialsFileRefusals", ConsentTestFlags)

bool FCrowdyAuthDevCredentialsFileRefusalsTest::RunTest(const FString& Parameters)
{
	const FString Secret = TEXT("hunter2-test");
	FString Email;
	FString Password;
	FString Error;

	TestFalse(TEXT("a missing file is refused"), CrowdyAuthDev::ReadCredentialsFile(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CrowdyAuthDev-no-such-file.json")), Email, Password, Error));
	TestFalse(TEXT("with a reason"), Error.IsEmpty());

	TestFalse(TEXT("text that is not JSON is refused"), ReadCredentialsFrom(TEXT("not json {"), Email, Password, Error));
	TestTrue(TEXT("and says so"), Error.Contains(TEXT("not a JSON")));

	TestFalse(TEXT("a file with no password is refused"),
		ReadCredentialsFrom(TEXT("{\"email\":\"a@example.test\"}"), Email, Password, Error));
	TestTrue(TEXT("and names the password"), Error.Contains(TEXT("password")));
	TestTrue(TEXT("nothing is handed back"), Email.IsEmpty() && Password.IsEmpty());

	TestFalse(TEXT("a file with an empty password is refused"),
		ReadCredentialsFrom(TEXT("{\"email\":\"a@example.test\",\"password\":\"\"}"), Email, Password, Error));

	TestFalse(TEXT("a file with no email is refused"),
		ReadCredentialsFrom(FString::Printf(TEXT("{\"password\":\"%s\"}"), *Secret), Email, Password, Error));
	TestTrue(TEXT("and names the email"), Error.Contains(TEXT("email")));
	TestFalse(TEXT("without echoing the password"), Error.Contains(Secret));
	TestTrue(TEXT("nothing is handed back"), Email.IsEmpty() && Password.IsEmpty());

	TestTrue(TEXT("a complete file reads"), ReadCredentialsFrom(
		FString::Printf(TEXT("{\"email\":\"a@example.test\",\"password\":\"%s\"}"), *Secret), Email, Password, Error));
	TestEqual(TEXT("the email"), Email, TEXT("a@example.test"));
	TestEqual(TEXT("the password"), Password, Secret);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthDevSignInReachesLoginTest,
	"CrowdySDK.Auth.DevSignInReachesLogin", ConsentTestFlags)

bool FCrowdyAuthDevSignInReachesLoginTest::RunTest(const FString& Parameters)
{
	FConsentRig Rig(0, false);
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	const FAppIdSettingScope NoDiscovery(0);
	Rig.Auth->SetInstanceSuffixForTest(TEXT(".autotest-dev-signin"));

	// The mint after it is answered by the rig's canned answer.
	TArray<TPair<int32, FString>> Script;
	Script.Emplace(200, FString(TEXT(
		"{\"data\":{\"login\":{\"token\":\"session-dev\",\"gameTokenId\":\"902\",\"user\":{\"userId\":\"9001\"}}}}")));
	Rig.Client->SetTestResponseScript(MoveTemp(Script));

	CrowdyAuthDev::SignIn(*Rig.Auth, TEXT("a@example.test"), TEXT("hunter2-test"));
	Rig.Drain();

	TestEqual(TEXT("the sign-in announces a login"), Rig.Listener->Logins, 1);
	TestEqual(TEXT("and no failure"), Rig.Listener->LoginFailures.Num(), 0);
	TestEqual(TEXT("as the account the server named"), Rig.Session->GetUserID(), static_cast<int64>(9001));
	TestTrue(TEXT("holding the session the server issued"),
		Rig.Session->GetSessionToken().Equals(TEXT("session-dev"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("and the minted app token"),
		Rig.Session->GetGameToken().Equals(TEXT("app-xyz"), ESearchCase::CaseSensitive));

	Rig.Auth->ClearSavedSession();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthDevStatusNamesNoTokenTest,
	"CrowdySDK.Auth.DevStatusNamesNoToken", ConsentTestFlags)

bool FCrowdyAuthDevStatusNamesNoTokenTest::RunTest(const FString& Parameters)
{
	FConsentRig Rig(0, false);
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Session->SetAppTokenExpiresAt(TEXT("2099-01-01T00:00:00Z"));

	const FString Line = CrowdyAuthDev::DescribeStatus(*Rig.Auth, *Rig.Session);
	TestTrue(TEXT("signed in"), Line.Contains(TEXT("SignedIn=true")));
	TestTrue(TEXT("the user"), Line.Contains(TEXT("UserID=7781")));
	TestTrue(TEXT("the app token is reported held"), Line.Contains(TEXT("AppToken=set")));
	TestTrue(TEXT("with its expiry"), Line.Contains(TEXT("ExpiresAt=2099-01-01T00:00:00Z")));
	TestTrue(TEXT("not paused"), Line.Contains(TEXT("AppPaused=false")));
	TestFalse(TEXT("no app token text"), Line.Contains(TEXT("app-old")));
	TestFalse(TEXT("no session token text"), Line.Contains(TEXT("session-abc")));

	Rig.Session->SetAppPaused(true, TEXT("out of funds"));
	TestTrue(TEXT("a pause carries its reason"),
		CrowdyAuthDev::DescribeStatus(*Rig.Auth, *Rig.Session).Contains(TEXT("AppPaused=true (out of funds)")));

	Rig.Session->SetGameToken(FString());
	Rig.Session->SetAppPaused(false, FString());
	const FString SignedOut = CrowdyAuthDev::DescribeStatus(*Rig.Auth, *Rig.Session);
	TestTrue(TEXT("without an app token it is not signed in"), SignedOut.Contains(TEXT("SignedIn=false")));
	TestTrue(TEXT("and the token is reported unset"), SignedOut.Contains(TEXT("AppToken=unset")));

	return true;
}

namespace
{
	const TCHAR* const PasswordSignInAnswer =
		TEXT("{\"data\":{\"login\":{\"token\":\"session-new\",\"gameTokenId\":\"903\",\"user\":{\"userId\":\"9002\"}}}}");

	const TCHAR* const PasswordRefusal = TEXT(
		"{\"errors\":[{\"message\":\"Wrong email or password.\","
		"\"extensions\":{\"code\":\"INVALID_CREDENTIALS\"}}],\"data\":null}");

	/** A password sign-in from a signed-out start, answered by Script, with its delegates on the rig's listener. */
	void SignInWithPassword(FConsentRig& Rig, TArray<TPair<int32, FString>> Script)
	{
		Rig.Session->SetGameToken(FString());
		Rig.Client->SetTestResponseScript(MoveTemp(Script));
		FOnAuthSuccess OnSuccess;
		OnSuccess.BindDynamic(Rig.Listener.Get(), &UCrowdyAuthTestListener::HandleFlowSuccess);
		FOnAuthError OnError;
		OnError.BindDynamic(Rig.Listener.Get(), &UCrowdyAuthTestListener::HandleFlowError);
		Rig.Auth->Login(TEXT("a@example.test"), TEXT("hunter2-test"), OnSuccess, OnError);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthCancelSignInDropsAPasswordSignInTest,
	"CrowdySDK.Auth.CancelSignInDropsAPasswordSignIn", ConsentTestFlags)

bool FCrowdyAuthCancelSignInDropsAPasswordSignInTest::RunTest(const FString& Parameters)
{
	FConsentRig Rig(0, false);
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}
	const FAppIdSettingScope NoDiscovery(0);
	Rig.Auth->SetInstanceSuffixForTest(TEXT(".autotest-cancel-password"));

	TArray<TPair<int32, FString>> Script;
	Script.Emplace(200, FString(PasswordSignInAnswer));
	SignInWithPassword(Rig, MoveTemp(Script));
	if (!TestTrue(TEXT("the sign-in is pending"), Rig.Auth->IsSignInPending()))
	{
		return false;
	}

	Rig.Auth->CancelSignIn();
	TestFalse(TEXT("a cancel ends it at once"), Rig.Auth->IsSignInPending());
	Rig.Drain();

	TestEqual(TEXT("the server's late answer does not sign the player in"), Rig.Listener->FlowSuccesses, 0);
	TestEqual(TEXT("nor announce a login"), Rig.Listener->Logins, 0);
	TestFalse(TEXT("the player is not signed in"), Rig.Auth->IsSignedIn());
	TestTrue(TEXT("the caller hears once that it was cancelled"),
		Rig.Listener->FlowErrors.Num() == 1 && Rig.Listener->FlowErrors[0] == TEXT("Sign-in cancelled."));
	TestFalse(TEXT("the late answer's session is not installed"),
		Rig.Session->GetSessionToken().Equals(TEXT("session-new"), ESearchCase::CaseSensitive));
	TestFalse(TEXT("nor saved"), Rig.Auth->HasSavedSession());

	Rig.Auth->ClearSavedSession();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthCancelSignInKeepsTheSavedSessionTest,
	"CrowdySDK.Auth.CancelSignInKeepsTheSavedSession", ConsentTestFlags)

bool FCrowdyAuthCancelSignInKeepsTheSavedSessionTest::RunTest(const FString& Parameters)
{
	FConsentRig Rig(0, false);
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}
	const FAppIdSettingScope NoDiscovery(0);
	Rig.Auth->SetInstanceSuffixForTest(TEXT(".autotest-cancel-keeps"));

	// A first sign-in saves a session, then the game starts again signed out.
	TArray<TPair<int32, FString>> SignIn;
	SignIn.Emplace(200, FString(PasswordSignInAnswer));
	SignInWithPassword(Rig, MoveTemp(SignIn));
	Rig.Drain();
	if (!TestTrue(TEXT("the first sign-in saved its session"), Rig.Auth->HasSavedSession()))
	{
		return false;
	}
	Rig.Session->SetGameToken(FString());

	// The restore's mint is held for the terms, so it stays pending until the player answers or cancels.
	TArray<TPair<int32, FString>> Held;
	Held.Emplace(403, FString(ConsentRefusal));
	Rig.Client->SetTestResponseScript(MoveTemp(Held));
	FOnAuthError OnError;
	OnError.BindDynamic(Rig.Listener.Get(), &UCrowdyAuthTestListener::HandleFlowError);
	Rig.Auth->RestoreSession(FOnAuthSuccess(), OnError);
	Rig.Drain();
	if (!TestTrue(TEXT("the restore is held for the terms"), Rig.Auth->IsLegalAcceptancePending())
		|| !TestTrue(TEXT("and still pending"), Rig.Auth->IsSignInPending()))
	{
		return false;
	}

	Rig.Auth->CancelSignIn();

	TestTrue(TEXT("the saved session is kept"), Rig.Auth->HasSavedSession());
	TestFalse(TEXT("the hold is released"), Rig.Auth->IsLegalAcceptancePending());
	TestFalse(TEXT("nothing is pending"), Rig.Auth->IsSignInPending());
	TestTrue(TEXT("the restore's caller hears it was cancelled"),
		Rig.Listener->FlowErrors.Num() == 1 && Rig.Listener->FlowErrors[0] == TEXT("Sign-in cancelled."));
	TestTrue(TEXT("the restore's session stays installed, so a later Logout can end it on the server"),
		Rig.Session->GetSessionToken().Equals(TEXT("session-new"), ESearchCase::CaseSensitive));

	Rig.Auth->ClearSavedSession();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthCancelSignInForgetsANewSessionTest,
	"CrowdySDK.Auth.CancelSignInForgetsANewSession", ConsentTestFlags)

bool FCrowdyAuthCancelSignInForgetsANewSessionTest::RunTest(const FString& Parameters)
{
	FConsentRig Rig(0, false);
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}
	const FAppIdSettingScope NoDiscovery(0);
	Rig.Auth->SetInstanceSuffixForTest(TEXT(".autotest-cancel-forgets"));

	// The server accepts the password, so the new session is saved, then holds the mint for the terms.
	TArray<TPair<int32, FString>> Script;
	Script.Emplace(200, FString(PasswordSignInAnswer));
	Script.Emplace(403, FString(ConsentRefusal));
	SignInWithPassword(Rig, MoveTemp(Script));
	Rig.Drain();
	if (!TestTrue(TEXT("the sign-in is held for the terms"), Rig.Auth->IsLegalAcceptancePending())
		|| !TestTrue(TEXT("with its new session saved"), Rig.Auth->HasSavedSession()))
	{
		return false;
	}

	Rig.Auth->CancelSignIn();
	Rig.Drain();

	TestFalse(TEXT("the session the player cancelled is not kept for the next launch"), Rig.Auth->HasSavedSession());
	TestTrue(TEXT("nor held in memory"), Rig.Session->GetSessionToken().IsEmpty());
	TestTrue(TEXT("the caller hears once that it was cancelled"),
		Rig.Listener->FlowErrors.Num() == 1 && Rig.Listener->FlowErrors[0] == TEXT("Sign-in cancelled."));
	TestEqual(TEXT("and no failure event is broadcast, as after a sign-out"), Rig.Listener->LoginFailures.Num(), 0);
	TestFalse(TEXT("nothing is pending"), Rig.Auth->IsSignInPending());

	Rig.Auth->ClearSavedSession();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthSignInPendingFollowsTheFlowTest,
	"CrowdySDK.Auth.SignInPendingFollowsTheFlow", ConsentTestFlags)

bool FCrowdyAuthSignInPendingFollowsTheFlowTest::RunTest(const FString& Parameters)
{
	FConsentRig Rig(0, false);
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}
	const FAppIdSettingScope NoDiscovery(0);
	Rig.Auth->SetInstanceSuffixForTest(TEXT(".autotest-pending-follows"));

	TArray<TPair<int32, FString>> Refused;
	Refused.Emplace(401, FString(PasswordRefusal));
	SignInWithPassword(Rig, MoveTemp(Refused));
	TestTrue(TEXT("a sign-in is pending while it runs"), Rig.Auth->IsSignInPending());
	Rig.Drain();
	TestEqual(TEXT("the refused sign-in fails"), Rig.Listener->FlowErrors.Num(), 1);
	TestFalse(TEXT("and is no longer pending"), Rig.Auth->IsSignInPending());

	TArray<TPair<int32, FString>> SignIn;
	SignIn.Emplace(200, FString(PasswordSignInAnswer));
	SignInWithPassword(Rig, MoveTemp(SignIn));
	Rig.Drain();
	if (!TestTrue(TEXT("the next sign-in succeeds"), Rig.Auth->IsSignedIn()))
	{
		return false;
	}

	// Losing the app token later must not bring back a sign-in that already finished.
	Rig.Session->SetGameToken(FString());
	TestFalse(TEXT("a finished sign-in is not pending"), Rig.Auth->IsSignInPending());

	Rig.Auth->ClearSavedSession();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthCancelSignInLeavesASignedInPlayerTest,
	"CrowdySDK.Auth.CancelSignInLeavesASignedInPlayer", ConsentTestFlags)

bool FCrowdyAuthCancelSignInLeavesASignedInPlayerTest::RunTest(const FString& Parameters)
{
	FConsentRig Rig(0, false);
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}
	if (!TestTrue(TEXT("the rig starts signed in"), Rig.Auth->IsSignedIn()))
	{
		return false;
	}
	const FAppIdSettingScope NoDiscovery(0);

	// A second sign-in while signed in, still in flight when the game cancels.
	TArray<TPair<int32, FString>> Script;
	Script.Emplace(401, FString(PasswordRefusal));
	Rig.Client->SetTestResponseScript(MoveTemp(Script));
	Rig.Auth->Login(TEXT("a@example.test"), TEXT("hunter2-test"), FOnAuthSuccess(), FOnAuthError());
	TestFalse(TEXT("a signed-in player has no sign-in pending"), Rig.Auth->IsSignInPending());

	Rig.Auth->CancelSignIn();

	TestTrue(TEXT("still signed in"), Rig.Auth->IsSignedIn());
	TestTrue(TEXT("with the session token in place"),
		Rig.Session->GetSessionToken().Equals(TEXT("session-abc"), ESearchCase::CaseSensitive));
	Rig.Drain();
	return true;
}
#endif

#endif // WITH_DEV_AUTOMATION_TESTS
