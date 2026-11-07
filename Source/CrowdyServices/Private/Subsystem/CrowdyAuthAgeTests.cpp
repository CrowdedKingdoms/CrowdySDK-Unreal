#include "CrowdyAuthTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyCppClient.h"
#include "Engine/GameInstance.h"
#include "Misc/AutomationTest.h"
#include "Subsystem/CrowdyAuthentication.h"
#include "Subsystem/CrowdyGameSession.h"
#include "UI/SCrowdyAgePrompt.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	constexpr EAutomationTestFlags AgeTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	FString Refusal(const TCHAR* Code)
	{
		return FString::Printf(TEXT("{\"errors\":[{\"message\":\"Refused by the server.\",\"extensions\":{\"code\":\"%s\"}}],\"data\":null}"), Code);
	}

	const TCHAR* const AgeRecorded = TEXT("{\"data\":{\"recordPlayerAge\":{\"userId\":\"1\",\"ageBracket\":\"13_17\",\"country\":\"US\"}}}");
	const TCHAR* const ConsentRecorded = TEXT("{\"data\":{\"recordPlayerConsents\":true}}");
	const TCHAR* const MintAnswer = TEXT(
		"{\"data\":{\"mintAppToken\":{\"token\":\"app-xyz\",\"gameTokenId\":\"5150\",\"appId\":\"31337\","
		"\"expiresAt\":\"2099-01-01T00:00:00Z\",\"gameApiUrl\":\"https://game.test\","
		"\"gameApiWsUrl\":\"wss://game.test\",\"launchUrl\":\"https://launch.test\"}}}");

	struct FAgeRig
	{
		TStrongObjectPtr<UGameInstance> Instance;
		TStrongObjectPtr<UCrowdyAuthentication> Auth;
		TStrongObjectPtr<UCrowdyGameSession> Session;
		TStrongObjectPtr<UCrowdyAuthTestListener> Listener;
		TSharedPtr<FCrowdyCppClient> Client;

		explicit FAgeRig(TArray<TPair<int32, FString>> Script)
			: Instance(NewObject<UGameInstance>(GetTransientPackage(), NAME_None, RF_Transient))
			, Auth(NewObject<UCrowdyAuthentication>(Instance.Get()))
			, Session(NewObject<UCrowdyGameSession>(Instance.Get()))
			, Listener(NewObject<UCrowdyAuthTestListener>(GetTransientPackage(), NAME_None, RF_Transient))
			, Client(FCrowdyCppClient::MakeForTest(MintAnswer, 200))
		{
			Auth->ClearAgeEntryLock();
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

			Auth->OnAgeRequired.AddDynamic(Listener.Get(), &UCrowdyAuthTestListener::HandleAgeRequired);
			Auth->OnAgeEntryEnded.AddDynamic(Listener.Get(), &UCrowdyAuthTestListener::HandleAgeEnded);
			Auth->OnLegalAcceptanceRequired.AddDynamic(Listener.Get(), &UCrowdyAuthTestListener::HandleLegalAcceptanceRequired);
			Auth->OnAppTokenRefreshed.AddDynamic(Listener.Get(), &UCrowdyAuthTestListener::HandleRefreshed);
			Session->OnAccessEnded.AddDynamic(Listener.Get(), &UCrowdyAuthTestListener::HandleAccessEnded);
		}

		~FAgeRig()
		{
			Auth->Deinitialize();
			Auth->ClearAgeEntryLock();
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

		void Age(int32 Year = 2012, int32 Month = 5, const FString& Country = TEXT("us"), bool bParental = true)
		{
			FOnPlayerAgeRecorded OnRecorded;
			OnRecorded.BindDynamic(Listener.Get(), &UCrowdyAuthTestListener::HandleAgeRecorded);
			FOnAuthError OnError;
			OnError.BindDynamic(Listener.Get(), &UCrowdyAuthTestListener::HandleAgeError);
			Auth->RecordPlayerAge(Year, Month, Country, bParental, OnRecorded, OnError);
		}

		void Consent()
		{
			FOnLegalConsentsRecorded OnRecorded;
			OnRecorded.BindDynamic(Listener.Get(), &UCrowdyAuthTestListener::HandleConsentRecorded);
			FOnAuthError OnError;
			OnError.BindDynamic(Listener.Get(), &UCrowdyAuthTestListener::HandleConsentError);
			Auth->RecordPlayerConsents(true, false, OnRecorded, OnError);
		}

		FString LastRequestBody() const
		{
			FString Body;
			Client->GetLastTestRequestBody(Body);
			return Body;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthAgeRequiredHoldsTheSignInTest,
	"CrowdySDK.Auth.AgeRequiredHoldsTheSignIn", AgeTestFlags)

bool FCrowdyAuthAgeRequiredHoldsTheSignInTest::RunTest(const FString& Parameters)
{
	FAgeRig Rig({ {403, Refusal(CrowdyCppAgeRequiredCode)}, {200, AgeRecorded} });
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Mint();
	Rig.Drain();
	TestEqual(TEXT("the player is asked once"), Rig.Listener->AgeRequests, 1);
	TestTrue(TEXT("the sign-in is held"), Rig.Auth->IsAgeEntryPending());
	TestFalse(TEXT("this is not a consent hold"), Rig.Auth->IsLegalAcceptancePending());
	TestEqual(TEXT("and has not failed"), Rig.Listener->FlowErrors.Num(), 0);

	Rig.Age();
	const FString Sent = Rig.LastRequestBody();
	TestTrue(TEXT("the id goes on the real request"), CrowdyCppRequestNeedsDeviceId(FTCHARToUTF8(*Sent)));
	TestTrue(TEXT("the month and year are sent"), Sent.Contains(TEXT("2012-05")));
	TestTrue(TEXT("the country is sent upper case"), Sent.Contains(TEXT("\"US\"")));
	TestTrue(TEXT("the parental confirmation is sent"), Sent.Contains(TEXT("parentalConfirmation")));

	Rig.Drain();
	TestEqual(TEXT("the age is reported recorded"), Rig.Listener->AgesRecorded, 1);
	TestEqual(TEXT("the hold ends"), Rig.Listener->AgeEnds, 1);
	TestEqual(TEXT("and the held sign-in completes"), Rig.Listener->FlowSuccesses, 1);
	TestFalse(TEXT("nothing is held any more"), Rig.Auth->IsAgeEntryPending());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthAgeThenConsentTest,
	"CrowdySDK.Auth.AgeThenConsentBothComplete", AgeTestFlags)

bool FCrowdyAuthAgeThenConsentTest::RunTest(const FString& Parameters)
{
	FAgeRig Rig({ {403, Refusal(CrowdyCppAgeRequiredCode)}, {200, AgeRecorded},
		{403, Refusal(CrowdyCppLegalAcceptanceRequiredCode)}, {200, ConsentRecorded} });
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Mint();
	Rig.Drain();
	Rig.Age();
	Rig.Drain();
	TestEqual(TEXT("the server's next question reaches the player"), Rig.Listener->LegalAcceptanceRequests, 1);
	TestTrue(TEXT("and holds the same sign-in"), Rig.Auth->IsLegalAcceptancePending());
	TestEqual(TEXT("which has not completed"), Rig.Listener->FlowSuccesses, 0);

	Rig.Consent();
	Rig.Drain();
	TestEqual(TEXT("the sign-in completes once"), Rig.Listener->FlowSuccesses, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthAgeRefusalEndsTheSignInTest,
	"CrowdySDK.Auth.AgeRefusalEndsTheSignIn", AgeTestFlags)

bool FCrowdyAuthAgeRefusalEndsTheSignInTest::RunTest(const FString& Parameters)
{
	FAgeRig Rig({ {403, Refusal(CrowdyCppAgeRequiredCode)}, {403, Refusal(CrowdyCppAgeRequirementNotMetCode)} });
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Mint();
	Rig.Drain();
	Rig.Age();
	Rig.Drain();

	const FString Text = CrowdyCppPlayerRefusalText(CrowdyCppAgeRequirementNotMetCode, FString());
	TestEqual(TEXT("the player is told"), Rig.Listener->AgeErrors.Num(), 1);
	TestEqual(TEXT("the sign-in fails"), Rig.Listener->FlowErrors.Num(), 1);
	if (Rig.Listener->AgeErrors.Num() == 1 && Rig.Listener->FlowErrors.Num() == 1)
	{
		TestEqual(TEXT("with the SDK's text, not the server's"), Rig.Listener->AgeErrors[0], Text);
		TestEqual(TEXT("on both paths"), Rig.Listener->FlowErrors[0], Text);
	}
	TestFalse(TEXT("the hold ends"), Rig.Auth->IsAgeEntryPending());
	TestTrue(TEXT("the install is locked"), Rig.Auth->IsAgeEntryLocked());
	TestEqual(TEXT("access ends"), Rig.Listener->AccessEndedReasons.Num(), 1);

	FString Unused;
	const int32 ErrorsBefore = Rig.Listener->AgeErrors.Num();
	const FString BodyBefore = Rig.LastRequestBody();
	Rig.Age(1990);
	TestEqual(TEXT("a second date is refused"), Rig.Listener->AgeErrors.Num(), ErrorsBefore + 1);
	TestEqual(TEXT("and never sent"), Rig.LastRequestBody(), BodyBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthAgeAlreadyKnownResumesTest,
	"CrowdySDK.Auth.AgeAlreadyKnownResumes", AgeTestFlags)

bool FCrowdyAuthAgeAlreadyKnownResumesTest::RunTest(const FString& Parameters)
{
	FAgeRig Rig({ {403, Refusal(CrowdyCppAgeRequiredCode)}, {409, Refusal(CrowdyCppConflictCode)} });
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Mint();
	Rig.Drain();
	Rig.Age();
	Rig.Drain();
	TestEqual(TEXT("no error is shown"), Rig.Listener->AgeErrors.Num(), 0);
	TestEqual(TEXT("the sign-in completes"), Rig.Listener->FlowSuccesses, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthAgeOtherFailureStaysHeldTest,
	"CrowdySDK.Auth.AgeOtherFailureStaysHeld", AgeTestFlags)

bool FCrowdyAuthAgeOtherFailureStaysHeldTest::RunTest(const FString& Parameters)
{
	FAgeRig Rig({ {403, Refusal(CrowdyCppAgeRequiredCode)}, {400, Refusal(TEXT("BAD_USER_INPUT"))} });
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Mint();
	Rig.Drain();
	Rig.Age();
	Rig.Drain();
	TestEqual(TEXT("the error is shown"), Rig.Listener->AgeErrors.Num(), 1);
	TestTrue(TEXT("the sign-in stays held"), Rig.Auth->IsAgeEntryPending());
	TestFalse(TEXT("and nothing is locked"), Rig.Auth->IsAgeEntryLocked());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthAgeBadInputSendsNothingTest,
	"CrowdySDK.Auth.AgeBadInputSendsNothing", AgeTestFlags)

bool FCrowdyAuthAgeBadInputSendsNothingTest::RunTest(const FString& Parameters)
{
	FAgeRig Rig({});
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Age(2012, 13);
	Rig.Age(2012, 0);
	Rig.Age(1200, 5);
	Rig.Age(2012, 5, TEXT("USA"));
	TestEqual(TEXT("each is refused"), Rig.Listener->AgeErrors.Num(), 4);
	FString Url;
	FString Authorization;
	TestFalse(TEXT("nothing is sent"), Rig.Client->GetLastTestRequest(Url, Authorization));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthDeclinedAgeFailsTheSignInTest,
	"CrowdySDK.Auth.DeclinedAgeFailsTheSignIn", AgeTestFlags)

bool FCrowdyAuthDeclinedAgeFailsTheSignInTest::RunTest(const FString& Parameters)
{
	FAgeRig Rig({ {403, Refusal(CrowdyCppAgeRequiredCode)} });
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Mint();
	Rig.Drain();
	Rig.Auth->DeclineAgeEntry();
	TestEqual(TEXT("the sign-in fails"), Rig.Listener->FlowErrors.Num(), 1);
	TestEqual(TEXT("the hold ends"), Rig.Listener->AgeEnds, 1);
	TestFalse(TEXT("declining is not a refusal"), Rig.Auth->IsAgeEntryLocked());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthTerritoryRefusalEndsAccessTest,
	"CrowdySDK.Auth.TerritoryRefusalEndsAccess", AgeTestFlags)

bool FCrowdyAuthTerritoryRefusalEndsAccessTest::RunTest(const FString& Parameters)
{
	FAgeRig Rig({ {403, Refusal(CrowdyCppTerritoryNotSupportedCode)} });
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Mint();
	Rig.Drain();
	TestEqual(TEXT("nothing is held"), Rig.Listener->AgeRequests, 0);
	TestEqual(TEXT("the sign-in fails once"), Rig.Listener->FlowErrors.Num(), 1);
	if (Rig.Listener->FlowErrors.Num() == 1)
	{
		TestEqual(TEXT("with the SDK's text"), Rig.Listener->FlowErrors[0],
			CrowdyCppPlayerRefusalText(CrowdyCppTerritoryNotSupportedCode, FString()));
	}
	TestEqual(TEXT("access ends"), Rig.Listener->AccessEndedReasons.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthRefusedRefreshStopsRotationTest,
	"CrowdySDK.Auth.PermanentRefusalStopsRotation", AgeTestFlags)

bool FCrowdyAuthRefusedRefreshStopsRotationTest::RunTest(const FString& Parameters)
{
	const TCHAR* const Codes[] = { CrowdyCppAgeRequirementNotMetCode, CrowdyCppTerritoryNotSupportedCode,
		CrowdyCppSafetyModeratorRequiredCode, CrowdyCppSafetyLogRequiredCode };

	for (const TCHAR* Code : Codes)
	{
		FAgeRig Rig({ {403, Refusal(Code)} });
		if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
		{
			return false;
		}

		Rig.Auth->RefreshAppToken();
		Rig.Drain();
		TestFalse(FString::Printf(TEXT("%s arms no retry"), Code), Rig.Auth->IsRotationTimerArmedForTest());
		TestFalse(FString::Printf(TEXT("%s sends no re-mint"), Code), Rig.LastRequestBody().Contains(TEXT("mintAppToken")));
		Rig.Auth->RefreshAppToken();
		TestEqual(FString::Printf(TEXT("%s stops later rotations"), Code), Rig.Client->NumPendingRequests(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthAgeRequiredOnRefreshHoldsTest,
	"CrowdySDK.Auth.AgeRequiredOnRefreshHolds", AgeTestFlags)

bool FCrowdyAuthAgeRequiredOnRefreshHoldsTest::RunTest(const FString& Parameters)
{
	FAgeRig Rig({ {403, Refusal(CrowdyCppAgeRequiredCode)}, {200, AgeRecorded} });
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Auth->RefreshAppToken();
	Rig.Drain();
	TestEqual(TEXT("the player is asked"), Rig.Listener->AgeRequests, 1);
	TestFalse(TEXT("no retry is armed"), Rig.Auth->IsRotationTimerArmedForTest());

	Rig.Age();
	Rig.Drain();
	TestEqual(TEXT("the held rotation completes"), Rig.Listener->Refreshes, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthLateAgeAnswerIsDroppedTest,
	"CrowdySDK.Auth.LateAgeAnswerIsDropped", AgeTestFlags)

bool FCrowdyAuthLateAgeAnswerIsDroppedTest::RunTest(const FString& Parameters)
{
	FAgeRig Rig({ {403, Refusal(CrowdyCppAgeRequiredCode)}, {403, Refusal(CrowdyCppAgeRequirementNotMetCode)} });
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Mint();
	Rig.Drain();
	Rig.Age();
	Rig.Auth->ClearSavedSession();
	Rig.Drain();
	TestEqual(TEXT("access does not end for whoever is next"), Rig.Listener->AccessEndedReasons.Num(), 0);
	TestTrue(TEXT("but the refusal still locks the install"), Rig.Auth->IsAgeEntryLocked());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthLockedInstallEndsTheHoldTest,
	"CrowdySDK.Auth.LockedInstallEndsTheHold", AgeTestFlags)

bool FCrowdyAuthLockedInstallEndsTheHoldTest::RunTest(const FString& Parameters)
{
	FAgeRig Rig({ {403, Refusal(CrowdyCppAgeRequirementNotMetCode)}, {403, Refusal(CrowdyCppAgeRequiredCode)} });
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Mint();
	Rig.Drain();
	TestTrue(TEXT("a refused mint locks too"), Rig.Auth->IsAgeEntryLocked());

	Rig.Mint();
	Rig.Drain();
	TestTrue(TEXT("a held sign-in on a locked install"), Rig.Auth->IsAgeEntryPending());
	Rig.Age();
	TestFalse(TEXT("does not hang"), Rig.Auth->IsAgeEntryPending());
	TestEqual(TEXT("both sign-ins are answered"), Rig.Listener->FlowErrors.Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthRefusedDateShowsNoServerTextTest,
	"CrowdySDK.Auth.RefusedDateShowsNoServerText", AgeTestFlags)

bool FCrowdyAuthRefusedDateShowsNoServerTextTest::RunTest(const FString& Parameters)
{
	FAgeRig Rig({ {403, Refusal(CrowdyCppAgeRequiredCode)}, {400, Refusal(TEXT("BAD_REQUEST"))} });
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Mint();
	Rig.Drain();
	Rig.Age();
	Rig.Drain();
	if (TestEqual(TEXT("one error"), Rig.Listener->AgeErrors.Num(), 1))
	{
		TestFalse(TEXT("the server's wording is not passed on"), Rig.Listener->AgeErrors[0].Contains(TEXT("Refused by the server")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthAppDocumentsAreListedAndRecordedTest,
	"CrowdySDK.Auth.AppDocumentsAreListedAndRecorded", AgeTestFlags)

bool FCrowdyAuthAppDocumentsAreListedAndRecordedTest::RunTest(const FString& Parameters)
{
	FAgeRig Rig({
		{200, TEXT("{\"data\":{\"playerLegalDocuments\":[{\"documentId\":\"t\",\"version\":\"1\",\"title\":\"Player Terms\",\"url\":\"https://a.test/t\"}]}}")},
		{200, TEXT("{\"data\":{\"appRequiredDocuments\":[{\"documentId\":\"r\",\"version\":\"1\",\"title\":\"Game rules\",\"url\":\"https://a.test/r\"}]}}")},
		{200, ConsentRecorded} });
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	FOnLegalDocuments OnDocuments;
	OnDocuments.BindDynamic(Rig.Listener.Get(), &UCrowdyAuthTestListener::HandleLegalDocuments);
	Rig.Auth->RequestLegalDocuments(OnDocuments);
	Rig.Drain();
	if (TestEqual(TEXT("the platform's and the app's"), Rig.Listener->LegalDocumentTitles.Num(), 2))
	{
		TestEqual(TEXT("platform first"), Rig.Listener->LegalDocumentTitles[0], FString(TEXT("Player Terms")));
		TestEqual(TEXT("then the app"), Rig.Listener->LegalDocumentTitles[1], FString(TEXT("Game rules")));
	}

	Rig.Consent();
	TestTrue(TEXT("the app is named on the consent"), Rig.LastRequestBody().Contains(TEXT("31337")));
	Rig.Drain();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthDeviceIdGoesOnlyWhereAskedTest,
	"CrowdySDK.Auth.DeviceIdGoesOnlyWhereAsked", AgeTestFlags)

bool FCrowdyAuthDeviceIdGoesOnlyWhereAskedTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("sign-up"), CrowdyCppRequestNeedsDeviceId(UTF8TEXTVIEW("{\"query\":\"mutation Register($r: RegisterUserInput!) {\"}")));
	TestTrue(TEXT("a recorded age"), CrowdyCppRequestNeedsDeviceId(UTF8TEXTVIEW("{\"query\":\"mutation RecordPlayerAge($i: X!) {\"}")));
	TestFalse(TEXT("not a sign-in"), CrowdyCppRequestNeedsDeviceId(UTF8TEXTVIEW("{\"query\":\"mutation Login($l: X!) {\"}")));
	TestFalse(TEXT("not a mint"), CrowdyCppRequestNeedsDeviceId(UTF8TEXTVIEW("{\"query\":\"mutation MintAppToken($m: X!) {\"}")));

	const FString Id = CrowdyCppDeviceId();
	TestTrue(TEXT("the id is long enough"), Id.Len() >= 16 && Id.Len() <= 128);
	TestEqual(TEXT("and stays the same"), CrowdyCppDeviceId(), Id);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAgePromptParentalLineTest,
	"CrowdySDK.Services.AgePrompt.ParentalLineOnlyFrom13To17", AgeTestFlags)

bool FCrowdyAgePromptParentalLineTest::RunTest(const FString& Parameters)
{
	const FDateTime Now(2026, 6, 15);
	struct FCase { const TCHAR* What; int32 Year; int32 Month; bool bLine; };
	const FCase Cases[] =
	{
		{ TEXT("no date"), 0, 0, false },
		{ TEXT("no month"), 2012, 0, false },
		{ TEXT("under 13 by a month, counted from the month's end"), 2013, 6, false },
		{ TEXT("13 last month"), 2013, 5, true },
		{ TEXT("17"), 2009, 1, true },
		{ TEXT("turns 18 at the end of this month"), 2008, 6, true },
		{ TEXT("18 last month"), 2008, 5, false },
		{ TEXT("an adult"), 1990, 1, false },
	};
	for (const FCase& Case : Cases)
	{
		TestEqual(Case.What, SCrowdyAgePrompt::DateNeedsParentalLine(Case.Year, Case.Month, Now), Case.bLine);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAgePromptAnswersTest,
	"CrowdySDK.Services.AgePrompt.PicksNothingAndNeedsEveryAnswer", AgeTestFlags)

bool FCrowdyAgePromptAnswersTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		return true;
	}

	int32 Continued = 0;
	const TSharedRef<SCrowdyAgePrompt> Prompt = SNew(SCrowdyAgePrompt)
		.OnContinue(FSimpleDelegate::CreateLambda([&Continued]() { ++Continued; }));

	TestEqual(TEXT("no month is picked"), Prompt->GetBirthMonth(), 0);
	TestEqual(TEXT("no year is picked"), Prompt->GetBirthYear(), 0);
	TestTrue(TEXT("no country is picked"), Prompt->GetCountryCode().IsEmpty());
	TestFalse(TEXT("so it cannot continue"), Prompt->CanContinue());

	Prompt->SetAnswersForTest(1990, 1, TEXT("US"));
	TestFalse(TEXT("no parental line yet"), Prompt->IsParentalLineShownForTest());
	Prompt->Continue();
	TestEqual(TEXT("an adult goes straight on"), Continued, 1);
	TestFalse(TEXT("and sends no confirmation"), Prompt->HasParentalConfirmation());

	Continued = 0;
	Prompt->SetAnswersForTest(FDateTime::UtcNow().GetYear() - 15, 1, TEXT("US"));
	TestFalse(TEXT("entering a date reveals nothing"), Prompt->IsParentalLineShownForTest());
	Prompt->Continue();
	TestEqual(TEXT("a minor does not go on yet"), Continued, 0);
	TestTrue(TEXT("the line shows"), Prompt->IsParentalLineShownForTest());
	TestFalse(TEXT("and must be ticked"), Prompt->CanContinue());
	Prompt->SetParentalCheckedForTest(true);
	Prompt->Continue();
	TestEqual(TEXT("then it goes on"), Continued, 1);
	TestTrue(TEXT("and the confirmation is sent"), Prompt->HasParentalConfirmation());

	Prompt->SetBusy(true);
	Prompt->Continue();
	TestEqual(TEXT("not while busy"), Continued, 1);
	Prompt->SetBusy(false);

	Prompt->ShowRefusal(FText::FromString(TEXT("No.")));
	TestTrue(TEXT("a refusal stays up"), Prompt->IsShowingRefusal());
	TestFalse(TEXT("and cannot be answered again"), Prompt->CanContinue());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
