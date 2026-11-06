#include "Subsystem/CrowdySafety.h"
#include "CrowdyServiceApiSupport.h"
#include "CrowdyCppClient.h"
#include "CrowdyServicesLog.h"
#include "Engine/GameInstance.h"
#include "Misc/DateTime.h"
#include "Subsystem/CrowdyAuthentication.h"

using namespace CrowdyServiceApi;

namespace
{
	constexpr ECrowdyCppApiDomain SafetyDomain = ECrowdyCppApiDomain::Safety;
	constexpr const TCHAR* SafetyLogName = TEXT("CrowdySafety");

	// A scope this SDK does not know, from a newer API, reads as Off: the safest thing to show.
	ECrowdyCommunicationScope ReadScope(const TSharedPtr<FJsonObject>& Json, const TCHAR* Field)
	{
		const FString Value = Json->GetStringField(Field);
		if (Value == TEXT("EVERYONE"))
		{
			return ECrowdyCommunicationScope::Everyone;
		}
		return Value == TEXT("CONTACTS") ? ECrowdyCommunicationScope::Contacts : ECrowdyCommunicationScope::Off;
	}

	// A choice the player has not made is null, which means Everyone.
	ECrowdyCommunicationScope ReadChoice(const TSharedPtr<FJsonObject>& Json, const TCHAR* Field)
	{
		FString Value;
		if (!Json->TryGetStringField(Field, Value))
		{
			return ECrowdyCommunicationScope::Everyone;
		}
		return ReadScope(Json, Field);
	}

	const TCHAR* ScopeName(ECrowdyCommunicationScope Scope)
	{
		switch (Scope)
		{
		case ECrowdyCommunicationScope::Everyone: return TEXT("EVERYONE");
		case ECrowdyCommunicationScope::Contacts: return TEXT("CONTACTS");
		default: return TEXT("OFF");
		}
	}

	const TCHAR* SettingField(ECrowdyCommunicationSetting Setting)
	{
		switch (Setting)
		{
		case ECrowdyCommunicationSetting::VoiceReceive: return TEXT("voiceReceive");
		case ECrowdyCommunicationSetting::VideoSend: return TEXT("videoSend");
		case ECrowdyCommunicationSetting::VideoReceive: return TEXT("videoReceive");
		default: return TEXT("voiceSend");
		}
	}

	const TCHAR* ReasonName(ECrowdySafetyReportReason Reason)
	{
		switch (Reason)
		{
		case ECrowdySafetyReportReason::ChildSafety: return TEXT("CHILD_SAFETY");
		case ECrowdySafetyReportReason::Harassment: return TEXT("HARASSMENT");
		case ECrowdySafetyReportReason::SexualContent: return TEXT("SEXUAL_CONTENT");
		case ECrowdySafetyReportReason::Hate: return TEXT("HATE");
		case ECrowdySafetyReportReason::SelfHarm: return TEXT("SELF_HARM");
		case ECrowdySafetyReportReason::Cheating: return TEXT("CHEATING");
		default: return TEXT("OTHER");
		}
	}

	bool ReadUserId(const TSharedPtr<FJsonObject>& Json, int64& OutUserId)
	{
		FString Value;
		return Json.IsValid() && Json->TryGetStringField(TEXT("userId"), Value) && LexTryParseString(OutUserId, *Value);
	}

	// Blueprint nodes and C++ callers answer alike when the call cannot start.
	void FailUnavailable(const FOnSafetyError& OnError)
	{
		OnError.ExecuteIfBound(ClientUnavailableError<FCrowdySafetyError>());
	}

	// A status this SDK does not know is refused: nothing may read as an email sent when it was not.
	struct FConsentAnswer
	{
		ECrowdyConsentStatus Status = ECrowdyConsentStatus::NotRequired;

		static bool ParseFromJson(const TSharedPtr<FJsonObject>& Json, FConsentAnswer& Out)
		{
			FString Value;
			if (!Json.IsValid() || !Json->TryGetStringField(TEXT("status"), Value))
			{
				return false;
			}
			if (Value == TEXT("REQUESTED"))
			{
				Out.Status = ECrowdyConsentStatus::Requested;
				return true;
			}
			if (Value == TEXT("ALREADY_GRANTED"))
			{
				Out.Status = ECrowdyConsentStatus::AlreadyGranted;
				return true;
			}
			Out.Status = ECrowdyConsentStatus::NotRequired;
			return Value == TEXT("NOT_REQUIRED");
		}
	};
}

FCrowdySafetyError FCrowdySafetyError::FromFailure(const FString& Message, const FString& ServerCode)
{
	FCrowdySafetyError Error;
	Error.Message = Message;
	Error.Code = ECrowdySafetyErrorCode::ServerError;
	if (ServerCode == TEXT("PARENTAL_CONSENT_UNAVAILABLE"))
	{
		Error.Code = ECrowdySafetyErrorCode::ConsentUnavailable;
	}
	else if (ServerCode == TEXT("SOCIAL_LIMIT_REACHED"))
	{
		Error.Code = ECrowdySafetyErrorCode::LimitReached;
	}
	else if (ServerCode == TEXT("RATE_LIMITED"))
	{
		Error.Code = ECrowdySafetyErrorCode::RateLimited;
	}
	else if (ServerCode.IsEmpty())
	{
		Error.Code = ECrowdySafetyErrorCode::NetworkError;
	}
	return Error;
}

bool FCrowdyCommunicationSettings::ParseFromJson(const TSharedPtr<FJsonObject>& Json, FCrowdyCommunicationSettings& Out)
{
	if (!Json.IsValid())
	{
		return false;
	}

	Out.bIsMinor = Json->GetBoolField(TEXT("isMinor"));
	Out.VoiceSend = ReadScope(Json, TEXT("voiceSend"));
	Out.VoiceReceive = ReadScope(Json, TEXT("voiceReceive"));
	Out.VideoSend = ReadScope(Json, TEXT("videoSend"));
	Out.VideoReceive = ReadScope(Json, TEXT("videoReceive"));
	const TSharedPtr<FJsonObject>* Choices = nullptr;
	if (Json->TryGetObjectField(TEXT("choices"), Choices))
	{
		Out.Choices.VoiceSend = ReadChoice(*Choices, TEXT("voiceSend"));
		Out.Choices.VoiceReceive = ReadChoice(*Choices, TEXT("voiceReceive"));
		Out.Choices.VideoSend = ReadChoice(*Choices, TEXT("videoSend"));
		Out.Choices.VideoReceive = ReadChoice(*Choices, TEXT("videoReceive"));
	}
	Out.VoiceAllowed = ReadScope(Json, TEXT("voiceAllowed"));
	Out.VideoAllowed = ReadScope(Json, TEXT("videoAllowed"));
	Out.bVoiceConsent = Json->GetBoolField(TEXT("voiceConsent"));
	Out.bVideoConsent = Json->GetBoolField(TEXT("videoConsent"));
	return true;
}

bool FCrowdyPlayerContact::ParseFromJson(const TSharedPtr<FJsonObject>& Json, FCrowdyPlayerContact& Out)
{
	if (!ReadUserId(Json, Out.UserId))
	{
		return false;
	}
	Out.bAccepted = Json->GetStringField(TEXT("state")) == TEXT("ACCEPTED");
	return true;
}

bool FCrowdyContactRequest::ParseFromJson(const TSharedPtr<FJsonObject>& Json, FCrowdyContactRequest& Out)
{
	if (!ReadUserId(Json, Out.UserId))
	{
		return false;
	}
	Out.bIncoming = Json->GetStringField(TEXT("direction")) == TEXT("INCOMING");
	return true;
}

bool FCrowdyPlayerBlock::ParseFromJson(const TSharedPtr<FJsonObject>& Json, FCrowdyPlayerBlock& Out)
{
	return ReadUserId(Json, Out.UserId);
}

bool FCrowdyAppRecording::ParseFromJson(const TSharedPtr<FJsonObject>& Json, FCrowdyAppRecording& Out)
{
	const TSharedPtr<FJsonObject>* Recording = nullptr;
	if (!Json.IsValid() || !Json->TryGetObjectField(TEXT("recording"), Recording))
	{
		return false;
	}
	// A part-answer must not read as "not recording", so all three fields are required.
	Out.bKnown = (*Recording)->TryGetBoolField(TEXT("enabled"), Out.bEnabled)
		&& (*Recording)->TryGetBoolField(TEXT("voice"), Out.bVoice)
		&& (*Recording)->TryGetBoolField(TEXT("video"), Out.bVideo);
	return Out.bKnown;
}

void UCrowdySafety::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	LiveSessionToken = MakeShared<uint8>(0);
	if (UCrowdyAuthentication* Authentication = Collection.InitializeDependency<UCrowdyAuthentication>())
	{
		Authentication->OnAppTokenRefreshed.AddDynamic(this, &UCrowdySafety::HandleAppTokenRefreshed);
	}
}

void UCrowdySafety::Deinitialize()
{
	LiveSessionToken.Reset();

	Super::Deinitialize();
}

int64 UCrowdySafety::GetAppId() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (AppIdForTest != 0)
	{
		return AppIdForTest;
	}
#endif
	return ResolveAppId(GetGameInstance());
}

FCrowdyCppClient* UCrowdySafety::ResolveClient() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (const TSharedPtr<FCrowdyCppClient> Override = ClientForTest.Pin())
	{
		return Override.Get();
	}
#endif
	return ResolveApiClient(GetGameInstance(), SafetyLogName);
}

#if WITH_DEV_AUTOMATION_TESTS
void UCrowdySafety::InitializeForTest(const TSharedPtr<FCrowdyCppClient>& Client, int64 AppId)
{
	LiveSessionToken = MakeShared<uint8>(0);
	ClientForTest = Client;
	AppIdForTest = AppId;
}
#endif

void UCrowdySafety::HandleAppTokenRefreshed()
{
	RefreshRecordingState();
}

void UCrowdySafety::RefreshRecordingState()
{
	FCrowdyCppClient* Client = ResolveClient();
	if (!Client)
	{
		return;
	}

	TWeakObjectPtr<UCrowdySafety> WeakThis(this);
	const int64 AppId = GetAppId();
	Client->RunOp(ECrowdyCppApiDomain::Apps, TEXT("AppRecording"), MakeVariables(),
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [WeakThis, AppId](FCrowdyCppJsonResult Result)
		{
			UCrowdySafety* Self = WeakThis.Get();
			if (!Self || Self->GetAppId() != AppId)
			{
				return;
			}

			FCrowdyAppRecording Read;
			FCrowdySafetyError Error;
			if (!ReadObject(Result, TEXT("app"), Read, Error))
			{
				UE_LOG(LogCrowdyServices, Warning, TEXT("Could not read what this app records: %s"), *Error.Message);
				return;
			}
			UE_LOG(LogCrowdyServices, Verbose, TEXT("This app records: log=%d voice=%d video=%d"), Read.bEnabled, Read.bVoice, Read.bVideo);
			if (Read == Self->Recording)
			{
				return;
			}
			Self->Recording = Read;
			Self->OnRecordingStateChanged.Broadcast(Read);
			Self->WarnIfRecordingUnannounced();
		}));
}

void UCrowdySafety::SetVoiceCapturing(bool bCapturing)
{
	bVoiceCapturing = bCapturing;
	WarnIfRecordingUnannounced();
}

void UCrowdySafety::WarnIfRecordingUnannounced()
{
	if (bWarnedAboutRecording || !bVoiceCapturing || !Recording.bVoice || OnRecordingStateChanged.IsBound())
	{
		return;
	}
	bWarnedAboutRecording = true;
	UE_LOG(LogCrowdyServices, Warning,
		TEXT("This app records voice, so players must be told before they speak and see an indicator while it records. Bind OnRecordingStateChanged on UCrowdySafety."));
}

TSharedPtr<FJsonObject> UCrowdySafety::MakeVariables() const
{
	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), BigInt(GetAppId()));
	return Variables;
}

void UCrowdySafety::RunCommand(const TCHAR* Operation, const TSharedPtr<FJsonObject>& Variables, FOnSafetyDone OnSuccess, FOnSafetyError OnError)
{
	FCrowdyCppClient* Client = ResolveClient();
	if (!Client)
	{
		FailUnavailable(OnError);
		return;
	}

	Client->RunOp(SafetyDomain, Operation, Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdySafetyError Error;
			if (!ReadAcknowledgement(Result, Error))
			{
				OnError.ExecuteIfBound(Error);
				return;
			}
			OnSuccess.ExecuteIfBound();
		}));
}

void UCrowdySafety::RunUserCommand(const TCHAR* Operation, int64 UserId, FOnSafetyDone OnSuccess, FOnSafetyError OnError)
{
	const TSharedPtr<FJsonObject> Variables = MakeVariables();
	Variables->SetStringField(TEXT("userId"), BigInt(UserId));
	RunCommand(Operation, Variables, OnSuccess, OnError);
}

void UCrowdySafety::RunSettings(const TCHAR* Operation, const TCHAR* Field, const TSharedPtr<FJsonObject>& Variables, FOnCommunicationSettings OnSuccess, FOnSafetyError OnError)
{
	FCrowdyCppClient* Client = ResolveClient();
	if (!Client)
	{
		FailUnavailable(OnError);
		return;
	}

	const FString FieldName = Field;
	Client->RunOp(SafetyDomain, Operation, Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [FieldName, OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyCommunicationSettings Settings;
			FCrowdySafetyError Error;
			if (!ReadObject(Result, *FieldName, Settings, Error))
			{
				OnError.ExecuteIfBound(Error);
				return;
			}
			OnSuccess.ExecuteIfBound(Settings);
		}));
}

void UCrowdySafety::GetCommunicationSettings(FOnCommunicationSettings OnSuccess, FOnSafetyError OnError)
{
	RunSettings(TEXT("MyCommSettings"), TEXT("myCommSettings"), MakeVariables(), OnSuccess, OnError);
}

void UCrowdySafety::SetCommunicationSetting(ECrowdyCommunicationSetting Setting, ECrowdyCommunicationScope Scope, FOnCommunicationSettings OnSuccess, FOnSafetyError OnError)
{
	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(SettingField(Setting), ScopeName(Scope));

	const TSharedPtr<FJsonObject> Variables = MakeVariables();
	Variables->SetObjectField(TEXT("input"), Input);
	RunSettings(TEXT("SetMyCommSettings"), TEXT("setMyCommSettings"), Variables, OnSuccess, OnError);
}

void UCrowdySafety::RequestParentalConsent(ECrowdyConsentFeature Feature, const FString& ParentEmail, FOnConsentRequested OnSuccess, FOnSafetyError OnError)
{
	FCrowdyCppClient* Client = ResolveClient();
	if (!Client)
	{
		FailUnavailable(OnError);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeVariables();
	Variables->SetStringField(TEXT("feature"), Feature == ECrowdyConsentFeature::Video ? TEXT("VIDEO") : TEXT("VOICE"));
	Variables->SetStringField(TEXT("parentEmail"), ParentEmail);
	Client->RunOp(SafetyDomain, TEXT("RequestParentalConsent"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FConsentAnswer Answer;
			FCrowdySafetyError Error;
			if (!ReadObject(Result, TEXT("requestParentalConsent"), Answer, Error))
			{
				Error.Reason = Result.ErrorReason;
				OnError.ExecuteIfBound(Error);
				return;
			}
			OnSuccess.ExecuteIfBound(Answer.Status);
		}));
}

void UCrowdySafety::GetContacts(FOnPlayerContacts OnSuccess, FOnSafetyError OnError)
{
	FCrowdyCppClient* Client = ResolveClient();
	if (!Client)
	{
		FailUnavailable(OnError);
		return;
	}

	Client->RunOp(SafetyDomain, TEXT("MyContacts"), MakeVariables(),
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			TArray<FCrowdyPlayerContact> Contacts;
			FCrowdySafetyError Error;
			if (!ReadArray(Result, TEXT("myContacts"), Contacts, Error))
			{
				OnError.ExecuteIfBound(Error);
				return;
			}
			OnSuccess.ExecuteIfBound(Contacts);
		}));
}

void UCrowdySafety::GetContactRequests(FOnContactRequests OnSuccess, FOnSafetyError OnError)
{
	FCrowdyCppClient* Client = ResolveClient();
	if (!Client)
	{
		FailUnavailable(OnError);
		return;
	}

	Client->RunOp(SafetyDomain, TEXT("ContactRequests"), MakeVariables(),
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			TArray<FCrowdyContactRequest> Requests;
			FCrowdySafetyError Error;
			if (!ReadArray(Result, TEXT("contactRequests"), Requests, Error))
			{
				OnError.ExecuteIfBound(Error);
				return;
			}
			OnSuccess.ExecuteIfBound(Requests);
		}));
}

void UCrowdySafety::RequestContact(int64 UserId, FOnPlayerContact OnSuccess, FOnSafetyError OnError)
{
	FCrowdyCppClient* Client = ResolveClient();
	if (!Client)
	{
		FailUnavailable(OnError);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeVariables();
	Variables->SetStringField(TEXT("userId"), BigInt(UserId));
	Client->RunOp(SafetyDomain, TEXT("RequestContact"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyPlayerContact Contact;
			FCrowdySafetyError Error;
			if (!ReadObject(Result, TEXT("requestContact"), Contact, Error))
			{
				OnError.ExecuteIfBound(Error);
				return;
			}
			OnSuccess.ExecuteIfBound(Contact);
		}));
}

void UCrowdySafety::AcceptContact(int64 UserId, FOnSafetyDone OnSuccess, FOnSafetyError OnError)
{
	RunUserCommand(TEXT("AcceptContact"), UserId, OnSuccess, OnError);
}

void UCrowdySafety::RemoveContact(int64 UserId, FOnSafetyDone OnSuccess, FOnSafetyError OnError)
{
	RunUserCommand(TEXT("RemoveContact"), UserId, OnSuccess, OnError);
}

void UCrowdySafety::GetBlocks(FOnPlayerBlocks OnSuccess, FOnSafetyError OnError)
{
	FCrowdyCppClient* Client = ResolveClient();
	if (!Client)
	{
		FailUnavailable(OnError);
		return;
	}

	Client->RunOp(SafetyDomain, TEXT("MyBlocks"), MakeVariables(),
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			TArray<FCrowdyPlayerBlock> Blocks;
			FCrowdySafetyError Error;
			if (!ReadArray(Result, TEXT("myBlocks"), Blocks, Error))
			{
				OnError.ExecuteIfBound(Error);
				return;
			}
			OnSuccess.ExecuteIfBound(Blocks);
		}));
}

void UCrowdySafety::BlockPlayer(int64 UserId, FOnSafetyDone OnSuccess, FOnSafetyError OnError)
{
	RunUserCommand(TEXT("BlockPlayer"), UserId, OnSuccess, OnError);
}

void UCrowdySafety::UnblockPlayer(int64 UserId, FOnSafetyDone OnSuccess, FOnSafetyError OnError)
{
	RunUserCommand(TEXT("UnblockPlayer"), UserId, OnSuccess, OnError);
}

void UCrowdySafety::FileReport(int64 ReportedUserId, ECrowdySafetyReportReason Reason, const FString& Details, const FString& ReportedActorUuid,
	FDateTime OccurredAt, FOnSafetyDone OnSuccess, FOnSafetyError OnError)
{
	const FDateTime Now = FDateTime::UtcNow();
	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("appId"), BigInt(GetAppId()));
	Input->SetStringField(TEXT("reportedUserId"), BigInt(ReportedUserId));
	Input->SetStringField(TEXT("reason"), ReasonName(Reason));
	Input->SetStringField(TEXT("occurredAt"), (OccurredAt.GetTicks() > 0 && OccurredAt < Now ? OccurredAt : Now).ToIso8601());
	if (!Details.IsEmpty())
	{
		Input->SetStringField(TEXT("details"), Details);
	}
	if (!ReportedActorUuid.IsEmpty())
	{
		Input->SetStringField(TEXT("reportedActorUuid"), ReportedActorUuid);
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);
	RunCommand(TEXT("FileSafetyReport"), Variables, OnSuccess, OnError);
}
