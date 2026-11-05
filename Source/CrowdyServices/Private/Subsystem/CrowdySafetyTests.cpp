#include "CrowdySafetyTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyCppClient.h"
#include "Engine/GameInstance.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	constexpr EAutomationTestFlags SafetyTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	const TCHAR* const SettingsAnswer = TEXT(
		"{\"data\":{\"myCommSettings\":{\"appId\":\"31337\",\"isMinor\":true,\"voiceSend\":\"CONTACTS\",\"voiceReceive\":\"OFF\","
		"\"videoSend\":\"OFF\",\"videoReceive\":\"OFF\",\"voiceAllowed\":\"CONTACTS\",\"videoAllowed\":\"SOMETHING_NEW\","
		"\"choices\":{\"voiceSend\":\"EVERYONE\",\"voiceReceive\":\"OFF\",\"videoSend\":null,\"videoReceive\":\"CONTACTS\"},"
		"\"voiceConsent\":true,\"videoConsent\":false}}}");

	const TCHAR* const SetSettingAnswer = TEXT(
		"{\"data\":{\"setMyCommSettings\":{\"appId\":\"31337\",\"isMinor\":false,\"voiceSend\":\"OFF\",\"voiceReceive\":\"OFF\","
		"\"videoSend\":\"OFF\",\"videoReceive\":\"OFF\",\"voiceAllowed\":\"EVERYONE\",\"videoAllowed\":\"EVERYONE\","
		"\"choices\":{\"voiceSend\":null,\"voiceReceive\":\"OFF\",\"videoSend\":null,\"videoReceive\":null},"
		"\"voiceConsent\":false,\"videoConsent\":false}}}");

	struct FSafetyRig
	{
		TStrongObjectPtr<UGameInstance> Instance;
		TStrongObjectPtr<UCrowdySafety> Safety;
		TStrongObjectPtr<UCrowdySafetyTestListener> Listener;
		TSharedPtr<FCrowdyCppClient> Client;

		explicit FSafetyRig(const TCHAR* Answer, int32 Status = 200)
			: Instance(NewObject<UGameInstance>(GetTransientPackage(), NAME_None, RF_Transient))
			, Safety(NewObject<UCrowdySafety>(Instance.Get()))
			, Listener(NewObject<UCrowdySafetyTestListener>(GetTransientPackage(), NAME_None, RF_Transient))
			, Client(FCrowdyCppClient::MakeForTest(Answer, Status))
		{
			if (Client.IsValid())
			{
				Client->SetGameToken(TEXT("app-token"));
				Client->SetManagementToken(TEXT("session-token"));
				Safety->InitializeForTest(Client, 31337);
			}
		}

		FOnSafetyDone Done() { FOnSafetyDone D; D.BindDynamic(Listener.Get(), &UCrowdySafetyTestListener::HandleDone); return D; }
		FOnSafetyError Failed() { FOnSafetyError E; E.BindDynamic(Listener.Get(), &UCrowdySafetyTestListener::HandleError); return E; }

		FString SentBody() const
		{
			FString Body;
			Client->GetLastTestRequestBody(Body);
			return Body;
		}

		FString SentBearer() const
		{
			FString Url;
			FString Authorization;
			Client->GetLastTestRequest(Url, Authorization);
			return Authorization;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySafetyReadsCommunicationSettingsTest,
	"CrowdySDK.Safety.ReadsCommunicationSettings", SafetyTestFlags)

bool FCrowdySafetyReadsCommunicationSettingsTest::RunTest(const FString& Parameters)
{
	FSafetyRig Rig(SettingsAnswer);
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}

	FOnCommunicationSettings OnSettings;
	OnSettings.BindDynamic(Rig.Listener.Get(), &UCrowdySafetyTestListener::HandleSettings);
	Rig.Safety->GetCommunicationSettings(OnSettings, Rig.Failed());
	TestTrue(TEXT("the app is named"), Rig.SentBody().Contains(TEXT("31337")));
	TestTrue(TEXT("it carries the app token, not the session"), Rig.SentBearer().Contains(TEXT("app-token")));
	Rig.Client->Poll();

	const FCrowdyCommunicationSettings& Got = Rig.Listener->Settings;
	TestEqual(TEXT("one answer"), Rig.Listener->SettingsAnswers, 1);
	TestTrue(TEXT("a minor"), Got.bIsMinor);
	TestTrue(TEXT("voice send"), Got.VoiceSend == ECrowdyCommunicationScope::Contacts);
	TestTrue(TEXT("voice allowed"), Got.VoiceAllowed == ECrowdyCommunicationScope::Contacts);
	TestTrue(TEXT("a scope from a newer API reads as off"), Got.VideoAllowed == ECrowdyCommunicationScope::Off);
	TestTrue(TEXT("voice consent"), Got.bVoiceConsent);
	TestTrue(TEXT("a choice stays apart from what applies"), Got.Choices.VoiceSend == ECrowdyCommunicationScope::Everyone);
	TestTrue(TEXT("a chosen off"), Got.Choices.VoiceReceive == ECrowdyCommunicationScope::Off);
	TestTrue(TEXT("no choice means everyone"), Got.Choices.VideoSend == ECrowdyCommunicationScope::Everyone);
	TestTrue(TEXT("a chosen scope"), Got.Choices.VideoReceive == ECrowdyCommunicationScope::Contacts);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySafetySetsOneSettingTest,
	"CrowdySDK.Safety.SetsOneSettingAndLeavesTheOthers", SafetyTestFlags)

bool FCrowdySafetySetsOneSettingTest::RunTest(const FString& Parameters)
{
	FSafetyRig Rig(SetSettingAnswer);
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}

	FOnCommunicationSettings OnSettings;
	OnSettings.BindDynamic(Rig.Listener.Get(), &UCrowdySafetyTestListener::HandleSettings);
	Rig.Safety->SetCommunicationSetting(ECrowdyCommunicationSetting::VoiceReceive, ECrowdyCommunicationScope::Off, OnSettings, Rig.Failed());
	const FString Variables = Rig.SentBody().RightChop(Rig.SentBody().Find(TEXT("\"variables\"")));
	TestTrue(TEXT("the named setting goes"), Variables.Contains(TEXT("\"voiceReceive\":\"OFF\"")));
	TestFalse(TEXT("voice send stays out"), Variables.Contains(TEXT("voiceSend")));
	TestFalse(TEXT("video send stays out"), Variables.Contains(TEXT("videoSend")));
	TestFalse(TEXT("video receive stays out"), Variables.Contains(TEXT("videoReceive")));
	Rig.Client->Poll();
	TestEqual(TEXT("one answer"), Rig.Listener->SettingsAnswers, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySafetyConsentAnswerTest,
	"CrowdySDK.Safety.ConsentSaysWhetherAnEmailWent", SafetyTestFlags)

bool FCrowdySafetyConsentAnswerTest::RunTest(const FString& Parameters)
{
	const auto Ask = [this](const TCHAR* Answer, TArray<ECrowdyConsentStatus>& OutAnswers, int32& OutErrors)
	{
		FSafetyRig Rig(Answer);
		if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
		{
			return;
		}
		FOnConsentRequested OnConsent;
		OnConsent.BindDynamic(Rig.Listener.Get(), &UCrowdySafetyTestListener::HandleConsent);
		Rig.Safety->RequestParentalConsent(ECrowdyConsentFeature::Video, TEXT("parent@a.test"), OnConsent, Rig.Failed());
		Rig.Client->Poll();
		OutAnswers = Rig.Listener->ConsentAnswers;
		OutErrors = Rig.Listener->Errors.Num();
	};

	TArray<ECrowdyConsentStatus> Answers;
	int32 Errors = 0;
	Ask(TEXT("{\"data\":{\"requestParentalConsent\":{\"appId\":\"31337\",\"feature\":\"VIDEO\",\"status\":\"REQUESTED\"}}}"), Answers, Errors);
	TestTrue(TEXT("the email went"), Answers.Num() == 1 && Answers[0] == ECrowdyConsentStatus::Requested);
	Ask(TEXT("{\"data\":{\"requestParentalConsent\":{\"appId\":\"31337\",\"feature\":\"VIDEO\",\"status\":\"ALREADY_GRANTED\"}}}"), Answers, Errors);
	TestTrue(TEXT("nothing sent, already granted"), Answers.Num() == 1 && Answers[0] == ECrowdyConsentStatus::AlreadyGranted);
	Ask(TEXT("{\"data\":{\"requestParentalConsent\":{\"appId\":\"31337\",\"feature\":\"VIDEO\",\"status\":\"NOT_REQUIRED\"}}}"), Answers, Errors);
	TestTrue(TEXT("nothing sent, not required"), Answers.Num() == 1 && Answers[0] == ECrowdyConsentStatus::NotRequired);
	Ask(TEXT("{\"data\":{\"requestParentalConsent\":{\"appId\":\"31337\",\"feature\":\"VIDEO\",\"status\":\"SOMETHING_NEW\"}}}"), Answers, Errors);
	TestEqual(TEXT("a status from a newer API is no answer"), Answers.Num(), 0);
	TestEqual(TEXT("it is refused"), Errors, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySafetyContactAnswerTest,
	"CrowdySDK.Safety.AContactRequestSaysWhetherItIsAccepted", SafetyTestFlags)

bool FCrowdySafetyContactAnswerTest::RunTest(const FString& Parameters)
{
	FSafetyRig Rig(TEXT("{\"data\":{\"requestContact\":{\"userId\":\"7\",\"state\":\"ACCEPTED\",\"createdAt\":\"2026-11-05T00:00:00Z\"}}}"));
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}

	FOnPlayerContact OnContact;
	OnContact.BindDynamic(Rig.Listener.Get(), &UCrowdySafetyTestListener::HandleContact);
	Rig.Safety->RequestContact(7, OnContact, Rig.Failed());
	Rig.Client->Poll();
	TestEqual(TEXT("the id"), Rig.Listener->Contact.UserId, 7LL);
	TestTrue(TEXT("they had already asked"), Rig.Listener->Contact.bAccepted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySafetySendsBlockTest,
	"CrowdySDK.Safety.SendsABlockWithItsUserIdAsAString", SafetyTestFlags)

bool FCrowdySafetySendsBlockTest::RunTest(const FString& Parameters)
{
	FSafetyRig Rig(TEXT("{\"data\":{\"blockPlayer\":{\"userId\":\"9007199254740993\",\"createdAt\":\"2026-11-05T00:00:00Z\"}}}"));
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Safety->BlockPlayer(9007199254740993LL, Rig.Done(), Rig.Failed());
	TestTrue(TEXT("the id survives as a string"), Rig.SentBody().Contains(TEXT("\"9007199254740993\"")));
	Rig.Client->Poll();
	TestEqual(TEXT("the block is done"), Rig.Listener->Dones, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySafetyReadsListsTest,
	"CrowdySDK.Safety.ReadsContactsAndBlocks", SafetyTestFlags)

bool FCrowdySafetyReadsListsTest::RunTest(const FString& Parameters)
{
	FSafetyRig Rig(TEXT("{\"data\":{\"myContacts\":[{\"userId\":\"7\",\"state\":\"ACCEPTED\"},{\"userId\":\"8\",\"state\":\"PENDING\"},{\"state\":\"ACCEPTED\"}]}}"));
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}

	FOnPlayerContacts OnContacts;
	OnContacts.BindDynamic(Rig.Listener.Get(), &UCrowdySafetyTestListener::HandleContacts);
	Rig.Safety->GetContacts(OnContacts, Rig.Failed());
	Rig.Client->Poll();
	if (TestEqual(TEXT("a row with no id is skipped"), Rig.Listener->Contacts.Num(), 2))
	{
		TestEqual(TEXT("the id"), Rig.Listener->Contacts[0].UserId, 7LL);
		TestTrue(TEXT("accepted"), Rig.Listener->Contacts[0].bAccepted);
		TestFalse(TEXT("pending"), Rig.Listener->Contacts[1].bAccepted);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySafetyFileReportTest,
	"CrowdySDK.Safety.FilesAReport", SafetyTestFlags)

bool FCrowdySafetyFileReportTest::RunTest(const FString& Parameters)
{
	FSafetyRig Rig(TEXT("{\"data\":{\"fileSafetyReport\":{\"reportId\":\"1\",\"filedAt\":\"2026-11-05T00:00:00Z\"}}}"));
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Safety->FileReport(42, ECrowdySafetyReportReason::SelfHarm, FString(), FString(), FDateTime(), Rig.Done(), Rig.Failed());
	const FString Body = Rig.SentBody();
	TestTrue(TEXT("the reason"), Body.Contains(TEXT("SELF_HARM")));
	TestTrue(TEXT("the app"), Body.Contains(TEXT("31337")));
	TestTrue(TEXT("an unset time is now"), Body.Contains(FString::Printf(TEXT("\"occurredAt\":\"%04d-"), FDateTime::UtcNow().GetYear())));
	TestFalse(TEXT("no empty details"), Body.Contains(TEXT("details")));
	TestFalse(TEXT("no empty actor"), Body.Contains(TEXT("reportedActorUuid")));
	Rig.Client->Poll();
	TestEqual(TEXT("the report is done"), Rig.Listener->Dones, 1);

	FSafetyRig Later(TEXT("{\"data\":{\"fileSafetyReport\":{\"reportId\":\"2\",\"filedAt\":\"2026-11-05T00:00:00Z\"}}}"));
	if (!TestTrue(TEXT("client built"), Later.Client.IsValid()))
	{
		return false;
	}
	Later.Safety->FileReport(42, ECrowdySafetyReportReason::Harassment, TEXT("kept following me"), TEXT("0123456789abcdef0123456789abcdef"),
		FDateTime(2026, 10, 30, 18, 4, 0), Later.Done(), Later.Failed());
	const FString LaterBody = Later.SentBody();
	TestTrue(TEXT("when it happened"), LaterBody.Contains(TEXT("\"occurredAt\":\"2026-10-30T18:04:00")));
	TestTrue(TEXT("their actor"), LaterBody.Contains(TEXT("\"reportedActorUuid\":\"0123456789abcdef0123456789abcdef\"")));

	FSafetyRig Ahead(TEXT("{\"data\":{\"fileSafetyReport\":{\"reportId\":\"3\",\"filedAt\":\"2026-11-05T00:00:00Z\"}}}"));
	if (!TestTrue(TEXT("client built"), Ahead.Client.IsValid()))
	{
		return false;
	}
	Ahead.Safety->FileReport(42, ECrowdySafetyReportReason::Other, FString(), FString(), FDateTime(2099, 1, 1), Ahead.Done(), Ahead.Failed());
	TestFalse(TEXT("a time in the future is now"), Ahead.SentBody().Contains(TEXT("2099-")));
	return true;
}

namespace
{
	const TCHAR* const RecordingVoiceAnswer = TEXT("{\"data\":{\"app\":{\"appId\":\"31337\",\"recording\":{\"enabled\":true,\"voice\":true,\"video\":false}}}}");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySafetyRecordingStateTest,
	"CrowdySDK.Safety.AnnouncesWhatTheAppRecords", SafetyTestFlags)

bool FCrowdySafetyRecordingStateTest::RunTest(const FString& Parameters)
{
	FSafetyRig Rig(RecordingVoiceAnswer);
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Safety->OnRecordingStateChanged.AddDynamic(Rig.Listener.Get(), &UCrowdySafetyTestListener::HandleRecording);
	Rig.Safety->RefreshRecordingState();
	TestTrue(TEXT("it carries the app token, not the session"), Rig.SentBearer().Contains(TEXT("app-token")));
	Rig.Client->Poll();
	TestTrue(TEXT("a read landed"), Rig.Safety->GetRecordingState().bKnown);
	TestTrue(TEXT("the log"), Rig.Safety->GetRecordingState().bEnabled);
	TestTrue(TEXT("voice"), Rig.Safety->GetRecordingState().bVoice);
	TestFalse(TEXT("not video"), Rig.Safety->GetRecordingState().bVideo);
	TestEqual(TEXT("a change is announced"), Rig.Listener->RecordingChanges, 1);

	Rig.Safety->RefreshRecordingState();
	Rig.Client->Poll();
	TestEqual(TEXT("the same answer is not"), Rig.Listener->RecordingChanges, 1);

	Rig.Safety->SetVoiceCapturing(true);
	TestFalse(TEXT("a game that listens gets no warning"), Rig.Safety->HasWarnedAboutRecording());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySafetyRecordingUnknownTest,
	"CrowdySDK.Safety.AnUnreadRecordingStateIsNotNotRecording", SafetyTestFlags)

bool FCrowdySafetyRecordingUnknownTest::RunTest(const FString& Parameters)
{
	FSafetyRig Partial(TEXT("{\"data\":{\"app\":{\"appId\":\"31337\",\"recording\":{\"enabled\":true,\"voice\":null,\"video\":false}}}}"));
	FSafetyRig Moved(RecordingVoiceAnswer);
	if (!TestTrue(TEXT("clients built"), Partial.Client.IsValid() && Moved.Client.IsValid()))
	{
		return false;
	}

	TestFalse(TEXT("nothing is known before a read"), Partial.Safety->GetRecordingState().bKnown);
	Partial.Safety->OnRecordingStateChanged.AddDynamic(Partial.Listener.Get(), &UCrowdySafetyTestListener::HandleRecording);
	Partial.Safety->RefreshRecordingState();
	Partial.Client->Poll();
	TestFalse(TEXT("a part-answer stays unknown"), Partial.Safety->GetRecordingState().bKnown);
	TestEqual(TEXT("and announces nothing"), Partial.Listener->RecordingChanges, 0);

	Moved.Safety->RefreshRecordingState();
	Moved.Safety->SetAppIdForTest(31338);
	Moved.Client->Poll();
	TestFalse(TEXT("an answer for the app it left is dropped"), Moved.Safety->GetRecordingState().bKnown);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySafetyRecordingWarningTest,
	"CrowdySDK.Safety.WarnsWhenVoiceOpensInARecordingAppWithNothingListening", SafetyTestFlags)

bool FCrowdySafetyRecordingWarningTest::RunTest(const FString& Parameters)
{
	FSafetyRig MicFirst(RecordingVoiceAnswer);
	FSafetyRig ReadFirst(RecordingVoiceAnswer);
	if (!TestTrue(TEXT("clients built"), MicFirst.Client.IsValid() && ReadFirst.Client.IsValid()))
	{
		return false;
	}

	MicFirst.Safety->SetVoiceCapturing(true);
	TestFalse(TEXT("nothing is known to be recorded yet"), MicFirst.Safety->HasWarnedAboutRecording());
	MicFirst.Safety->RefreshRecordingState();
	MicFirst.Client->Poll();
	TestTrue(TEXT("the read lands while the microphone is open"), MicFirst.Safety->HasWarnedAboutRecording());

	ReadFirst.Safety->RefreshRecordingState();
	ReadFirst.Client->Poll();
	TestFalse(TEXT("no microphone, no warning"), ReadFirst.Safety->HasWarnedAboutRecording());
	ReadFirst.Safety->SetVoiceCapturing(true);
	TestTrue(TEXT("the microphone opens in a recording app"), ReadFirst.Safety->HasWarnedAboutRecording());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySafetyRefusalsTest,
	"CrowdySDK.Safety.NamesItsRefusals", SafetyTestFlags)

bool FCrowdySafetyRefusalsTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("no consent service"), FCrowdySafetyError::FromFailure(TEXT("x"), TEXT("PARENTAL_CONSENT_UNAVAILABLE")).Code == ECrowdySafetyErrorCode::ConsentUnavailable);
	TestTrue(TEXT("too many contacts"), FCrowdySafetyError::FromFailure(TEXT("x"), TEXT("SOCIAL_LIMIT_REACHED")).Code == ECrowdySafetyErrorCode::LimitReached);
	TestTrue(TEXT("too many asks"), FCrowdySafetyError::FromFailure(TEXT("x"), TEXT("RATE_LIMITED")).Code == ECrowdySafetyErrorCode::RateLimited);
	TestTrue(TEXT("no code means it never got there"), FCrowdySafetyError::FromFailure(TEXT("x"), FString()).Code == ECrowdySafetyErrorCode::NetworkError);
	TestTrue(TEXT("anything else is the server's"), FCrowdySafetyError::FromFailure(TEXT("x"), TEXT("FORBIDDEN")).Code == ECrowdySafetyErrorCode::ServerError);

	FSafetyRig Rig(TEXT("{\"errors\":[{\"message\":\"No service.\",\"extensions\":{\"code\":\"PARENTAL_CONSENT_UNAVAILABLE\",\"reason\":\"not_configured\"}}],\"data\":null}"), 403);
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	FOnConsentRequested OnConsent;
	OnConsent.BindDynamic(Rig.Listener.Get(), &UCrowdySafetyTestListener::HandleConsent);
	Rig.Safety->RequestParentalConsent(ECrowdyConsentFeature::Voice, TEXT("parent@a.test"), OnConsent, Rig.Failed());
	Rig.Client->Poll();
	if (TestEqual(TEXT("the refusal arrives"), Rig.Listener->Errors.Num(), 1))
	{
		TestTrue(TEXT("with its code"), Rig.Listener->Errors[0].Code == ECrowdySafetyErrorCode::ConsentUnavailable);
		TestEqual(TEXT("and its reason"), Rig.Listener->Errors[0].Reason, FString(TEXT("not_configured")));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
