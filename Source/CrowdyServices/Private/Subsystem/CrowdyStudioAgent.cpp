#include "Subsystem/CrowdyStudioAgent.h"
#include "CrowdyCppClient.h"
#include "CrowdyServiceApiSupport.h"
#include "CrowdyServicesLog.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/GameInstance.h"
#include "Misc/DateTime.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

using namespace CrowdyServiceApi;

namespace
{
	const TCHAR* const StudioAgentLogName = TEXT("CrowdyStudioAgent");
	const TCHAR* const StudioAgentNotSignedIn = TEXT("The player has no app token yet.");
	const TCHAR* const StudioAgentNoClient = TEXT("The Crowdy API client is unavailable, so the request was not sent.");
	const TCHAR* const StudioAgentChanged = TEXT("The app or the player changed while the call was in flight.");
	const TCHAR* const StudioAgentNoAnswer = TEXT("The answer holds nothing.");
	const TCHAR* const StudioAgentInsecureApi = TEXT("The API address is not https, so the agent was not started.");
	const TCHAR* const StudioAgentNoModels = TEXT("No agent model is available for this app yet, so the Studio agent cannot start.");

	const TCHAR* StudioAgentResultName(ECrowdyStudioResult Result)
	{
		switch (Result)
		{
		case ECrowdyStudioResult::Success:     return TEXT("Success");
		case ECrowdyStudioResult::NotSignedIn: return TEXT("NotSignedIn");
		case ECrowdyStudioResult::Denied:      return TEXT("Denied");
		case ECrowdyStudioResult::NotFound:    return TEXT("NotFound");
		case ECrowdyStudioResult::Refused:     return TEXT("Refused");
		case ECrowdyStudioResult::Failed:      return TEXT("Failed");
		case ECrowdyStudioResult::BadReply:    return TEXT("BadReply");
		case ECrowdyStudioResult::NotBuilt:    return TEXT("NotBuilt");
		case ECrowdyStudioResult::NoModel:     return TEXT("NoModel");
		}
		return TEXT("Unknown");
	}

	// Only the result and the platform's code are logged: tokens, ids and the platform's text stay out.
	void StudioAgentWarn(const TCHAR* Call, ECrowdyStudioResult Result, const FString& Code)
	{
		UE_CLOG(Result != ECrowdyStudioResult::Success, LogCrowdyServices, Warning, TEXT("[%s] %s ended %s%s%s"), StudioAgentLogName, Call,
			StudioAgentResultName(Result), Code.IsEmpty() ? TEXT("") : TEXT(": "), *Code);
	}

	template <typename AnswerType>
	void StudioAgentFinish(const TCHAR* Call, const TFunction<void(const AnswerType&)>& OnDone, AnswerType Answer, ECrowdyStudioResult Result,
		const FString& Message, const FString& Code = FString())
	{
		Answer.Result = Result;
		Answer.Message = Message;
		StudioAgentWarn(Call, Result, Code);
		OnDone(Answer);
	}

	// A failed call answers an empty result, so nothing from a refusal reads as data.
	template <typename AnswerType>
	TFunction<void(ECrowdyStudioResult, const FString&, const FString&)> StudioAgentFailWith(const TCHAR* Call, const TFunction<void(const AnswerType&)>& OnDone)
	{
		return [Call, OnDone](ECrowdyStudioResult Result, const FString& Message, const FString& Code)
		{
			StudioAgentFinish(Call, OnDone, AnswerType(), Result, Message, Code);
		};
	}

	ECrowdyStudioResult StudioAgentFailureResult(const FCrowdyCppJsonResult& Result)
	{
		if (Result.ErrorCode.IsEmpty() || Result.ErrorMessage == FCrowdyCppClient::CanceledErrorMessage())
		{
			return ECrowdyStudioResult::Failed;
		}
		if (NamesCode(Result, TEXT("NOT_FOUND")) || Result.ErrorCode.EndsWith(TEXT("_NOT_FOUND"), ESearchCase::CaseSensitive))
		{
			return ECrowdyStudioResult::NotFound;
		}
		static const TCHAR* const DeniedCodes[] = {TEXT("FORBIDDEN"), TEXT("UNAUTHORIZED"), TEXT("UNAUTHENTICATED"), TEXT("NOT_ALLOWED"), TEXT("SCOPE_MISSING"),
			TEXT("AGENT_PERMISSION_DENIED"), TEXT("AGENT_DISABLED"), TEXT("AGENT_APP_KILLED"), TEXT("AGENT_OPERATOR_KILLED")};
		for (const TCHAR* Code : DeniedCodes)
		{
			if (NamesCode(Result, Code))
			{
				return ECrowdyStudioResult::Denied;
			}
		}
		return ECrowdyStudioResult::Refused;
	}

	// 2xx with no list is a malformed body; no answer at all is a failed request.
	ECrowdyStudioResult StudioAgentModelsFailure(const FCrowdyCppStudioModelsResult& Result)
	{
		if (Result.HttpStatus == 0 || Result.HttpStatus >= 500)
		{
			return ECrowdyStudioResult::Failed;
		}
		if (Result.HttpStatus >= 200 && Result.HttpStatus < 300)
		{
			return ECrowdyStudioResult::BadReply;
		}
		// The platform's answer when no model is available to the app, from its own policy or the platform's.
		if (Result.ErrorCode.Equals(TEXT("AGENT_MODEL_NOT_ALLOWED"), ESearchCase::CaseSensitive))
		{
			return ECrowdyStudioResult::NoModel;
		}
		if (Result.HttpStatus == 401 || Result.HttpStatus == 403)
		{
			return ECrowdyStudioResult::Denied;
		}
		return Result.HttpStatus == 404 ? ECrowdyStudioResult::NotFound : ECrowdyStudioResult::Refused;
	}

	FString StudioAgentModelsCode(const FCrowdyCppStudioModelsResult& Result)
	{
		return Result.ErrorCode.IsEmpty() ? FString::Printf(TEXT("http %d"), Result.HttpStatus) : Result.ErrorCode;
	}

	TSharedPtr<FJsonObject> StudioAgentAnswerObject(const FCrowdyCppJsonResult& Result, const TCHAR* Field)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		return Result.Data.IsValid() && Result.Data->TryGetObjectField(Field, Object) ? *Object : nullptr;
	}

	// Absent or null reads as 0; text that is not an ISO-8601 time is unread.
	bool StudioAgentReadOptTime(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, int64& OutMs)
	{
		const TSharedPtr<FJsonValue> Value = Object->TryGetField(Field);
		if (!Value.IsValid() || Value->IsNull())
		{
			OutMs = 0;
			return true;
		}
		FString Text;
		FDateTime Time;
		if (!Object->TryGetStringField(Field, Text) || !FDateTime::ParseIso8601(*Text, Time))
		{
			return false;
		}
		OutMs = (Time - FDateTime(1970, 1, 1)).GetTicks() / ETimespan::TicksPerMillisecond;
		return true;
	}

	bool StudioAgentReadConsent(const TSharedPtr<FJsonObject>& Object, int64 AppId, FCrowdyStudioConsent& Out)
	{
		int64 Answered = 0;
		if (!Object.IsValid() || !ReadWholeInt64(Object, TEXT("appId"), Answered) || Answered != AppId)
		{
			return false;
		}
		return ReadStrictBool(Object, TEXT("consented"), Out.bConsented) && StudioAgentReadOptTime(Object, TEXT("consentedAt"), Out.ConsentedAtMs);
	}

	// A payer this SDK does not know reads as Unknown, so a value the platform adds later does not break the call.
	ECrowdyStudioPayer StudioAgentPayer(const FString& Text)
	{
		if (Text.Equals(TEXT("PLAYER"), ESearchCase::CaseSensitive))
		{
			return ECrowdyStudioPayer::Player;
		}
		if (Text.Equals(TEXT("ORG"), ESearchCase::CaseSensitive))
		{
			return ECrowdyStudioPayer::App;
		}
		return Text.Equals(TEXT("PLATFORM"), ESearchCase::CaseSensitive) ? ECrowdyStudioPayer::Platform : ECrowdyStudioPayer::Unknown;
	}

	// A day limit the platform leaves null reads as 0.
	bool StudioAgentReadUsage(const TSharedPtr<FJsonObject>& Object, int64 AppId, FCrowdyStudioModelUsage& Out)
	{
		int64 Answered = 0;
		FString Payer;
		if (!Object.IsValid() || !ReadWholeInt64(Object, TEXT("appId"), Answered) || Answered != AppId || !Object->TryGetStringField(TEXT("payerKind"), Payer))
		{
			return false;
		}
		Out.Payer = StudioAgentPayer(Payer);
		const bool bCounts = ReadWholeInt64(Object, TEXT("todayRequests"), Out.TodayRequests) && ReadWholeInt64(Object, TEXT("todayChargeMicrousd"), Out.TodayChargeMicroUsd);
		const TSharedPtr<FJsonValue> Limit = Object->TryGetField(TEXT("dayLimitMicrousd"));
		const bool bLimit = !Limit.IsValid() || Limit->IsNull() ? true : ReadWholeInt64(Object, TEXT("dayLimitMicrousd"), Out.DayLimitMicroUsd);
		return bCounts && bLimit && Out.TodayRequests >= 0 && Out.TodayChargeMicroUsd >= 0 && Out.DayLimitMicroUsd >= 0;
	}

	// The scheme and authority of an absolute URL, lowercased; empty when it is not http(s), not https when bHttpsOnly, or names no plain host.
	FString StudioAgentUrlOrigin(const FString& Url, bool bHttpsOnly)
	{
		int32 SchemeEnd = INDEX_NONE;
		if (!Url.FindChar(TEXT(':'), SchemeEnd) || !Url.Mid(SchemeEnd).StartsWith(TEXT("://")))
		{
			return FString();
		}
		const FString Scheme = Url.Left(SchemeEnd).ToLower();
		if (Scheme != TEXT("https") && (bHttpsOnly || Scheme != TEXT("http")))
		{
			return FString();
		}
		const FString Rest = Url.Mid(SchemeEnd + 3);
		int32 AuthorityEnd = 0;
		while (AuthorityEnd < Rest.Len() && Rest[AuthorityEnd] != TEXT('/') && Rest[AuthorityEnd] != TEXT('?') && Rest[AuthorityEnd] != TEXT('#'))
		{
			const TCHAR Char = Rest[AuthorityEnd];
			if (Char == TEXT('@') || Char == TEXT('\\') || FChar::IsWhitespace(Char))
			{
				return FString();
			}
			++AuthorityEnd;
		}
		if (AuthorityEnd == 0 || Rest[0] == TEXT(':'))
		{
			return FString();
		}
		return Scheme + TEXT("://") + Rest.Left(AuthorityEnd).ToLower();
	}

#if !UE_BUILD_SHIPPING
	bool StudioAgentIsLoopback(const FString& Origin)
	{
		const FString Authority = Origin.Mid(Origin.Find(TEXT("://")) + 3);
		static const TCHAR* const Hosts[] = {TEXT("localhost"), TEXT("127.0.0.1"), TEXT("[::1]")};
		for (const TCHAR* Host : Hosts)
		{
			if (Authority == Host || Authority.StartsWith(FString(Host) + TEXT(":")))
			{
				return true;
			}
		}
		return false;
	}
#endif

	// The API origin the agent may carry a token to: https, or plain http to this machine in a build that is not Shipping; else empty.
	FString StudioAgentSecureApiOrigin(const FString& Url)
	{
		const FString Origin = StudioAgentUrlOrigin(Url, false);
		if (Origin.StartsWith(TEXT("https://")))
		{
			return Origin;
		}
#if !UE_BUILD_SHIPPING
		if (!Origin.IsEmpty() && StudioAgentIsLoopback(Origin))
		{
			return Origin;
		}
#endif
		return FString();
	}

	// The settings' Studio origin and wallet URL once checked, or why they cannot be handed out.
	bool StudioAgentCheckSettings(FString& OutStudioOrigin, FString& OutWalletUrl, FString& OutProblem)
	{
		const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>();
		const FString StudioText = Settings->StudioOrigin.TrimStartAndEnd();
		OutStudioOrigin = StudioAgentUrlOrigin(StudioText, true);
		if (OutStudioOrigin.IsEmpty() || (StudioText.Len() > OutStudioOrigin.Len() && StudioText.Mid(OutStudioOrigin.Len()) != TEXT("/")))
		{
			OutProblem = TEXT("The Studio Origin setting is not an https origin, so the agent was not started.");
			return false;
		}
		OutWalletUrl = Settings->StudioWalletUrl.TrimStartAndEnd();
		if (StudioAgentUrlOrigin(OutWalletUrl, true).IsEmpty())
		{
			OutProblem = TEXT("The Wallet URL setting is not an https address, so the agent was not started.");
			return false;
		}
		return true;
	}

	void StudioAgentFillModels(const TArray<FCrowdyCppStudioModel>& Models, TArray<FCrowdyStudioModel>& Out)
	{
		for (const FCrowdyCppStudioModel& Model : Models)
		{
			FCrowdyStudioModel& Read = Out.AddDefaulted_GetRef();
			Read.ModelId = Model.Id;
			Read.Name = Model.Name;
			Read.ContextWindow = Model.ContextWindow;
			Read.InputModalities = Model.InputModalities;
		}
	}

	const TCHAR* StudioAgentActionName(ECrowdyStudioRunMode Action)
	{
		return Action == ECrowdyStudioRunMode::Live ? TEXT("deploy_live") : TEXT("draft_test");
	}
}

void UCrowdyStudioAgent::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	LiveSessionToken = MakeShared<uint8>(0);
	const UCrowdyGameSession* Session = Collection.InitializeDependency<UCrowdyGameSession>();
	LastUserId = Session ? Session->GetUserID() : 0;
	BindAuthentication(Collection.InitializeDependency<UCrowdyAuthentication>());
}

void UCrowdyStudioAgent::Deinitialize()
{
	LiveSessionToken.Reset();
	if (UCrowdyAuthentication* Auth = BoundAuth.Get())
	{
		Auth->OnLogin.RemoveAll(this);
		Auth->OnRegister.RemoveAll(this);
		Auth->OnSessionRestored.RemoveAll(this);
		Auth->OnAppTokenRefreshed.RemoveAll(this);
	}
	Super::Deinitialize();
}

#if WITH_DEV_AUTOMATION_TESTS
void UCrowdyStudioAgent::InitializeForTest(const TSharedPtr<FCrowdyCppClient>& Client, UCrowdyGameSession* Session, UCrowdyAuthentication* Auth)
{
	LiveSessionToken = MakeShared<uint8>(0);
	ClientForTest = Client;
	SessionForTest = Session;
	LastUserId = Session ? Session->GetUserID() : 0;
	BindAuthentication(Auth);
}
#endif

void UCrowdyStudioAgent::BindAuthentication(UCrowdyAuthentication* Auth)
{
	if (!Auth)
	{
		return;
	}
	BoundAuth = Auth;
	Auth->OnLogin.AddUniqueDynamic(this, &UCrowdyStudioAgent::HandleSignedIn);
	Auth->OnRegister.AddUniqueDynamic(this, &UCrowdyStudioAgent::HandleSignedIn);
	Auth->OnSessionRestored.AddUniqueDynamic(this, &UCrowdyStudioAgent::HandleSignedIn);
	Auth->OnAppTokenRefreshed.AddUniqueDynamic(this, &UCrowdyStudioAgent::HandleAppTokenRefreshed);
}

void UCrowdyStudioAgent::HandleSignedIn(FCrowdyAuthResult Result)
{
	PublishToken();
}

void UCrowdyStudioAgent::HandleAppTokenRefreshed()
{
	PublishToken();
}

void UCrowdyStudioAgent::HandleSignedOut()
{
	if (!LiveSessionToken.IsValid())
	{
		return;
	}
	const int64 UserId = LastUserId;
	LastUserId = 0;
	OnAgentTokenChanged.Broadcast(UserId, FString());
}

void UCrowdyStudioAgent::PublishToken()
{
	const UCrowdyGameSession* Session = ResolveSession();
	const FString Token = LiveSessionToken.IsValid() && Session ? Session->GetGameToken() : FString();
	if (Token.IsEmpty())
	{
		return;
	}
	LastUserId = Session->GetUserID();
	if (const FCrowdyCppClient* Client = ResolveClient())
	{
		NoteApiOrigin(StudioAgentUrlOrigin(CurrentApiEndpoint(*Client), false));
	}
	OnAgentTokenChanged.Broadcast(LastUserId, Token);
}

void UCrowdyStudioAgent::NoteApiOrigin(const FString& Origin)
{
	if (Origin.IsEmpty())
	{
		return;
	}
	const bool bMoved = !LastApiOrigin.IsEmpty() && LastApiOrigin != Origin;
	LastApiOrigin = Origin;
	if (bMoved)
	{
		OnAgentBootChanged.Broadcast();
	}
}

UCrowdyGameSession* UCrowdyStudioAgent::ResolveSession() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (UCrowdyGameSession* Override = SessionForTest.Get())
	{
		return Override;
	}
#endif
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UCrowdyGameSession>() : nullptr;
}

FCrowdyCppClient* UCrowdyStudioAgent::ResolveClient() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (const TSharedPtr<FCrowdyCppClient> Override = ClientForTest.Pin())
	{
		return Override.Get();
	}
#endif
	return ResolveApiClient(GetGameInstance(), StudioAgentLogName);
}

FString UCrowdyStudioAgent::CurrentApiEndpoint(const FCrowdyCppClient& Client) const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (!ApiEndpointForTest.IsEmpty())
	{
		return ApiEndpointForTest;
	}
#endif
	return Client.GetApiEndpoint();
}

int64 UCrowdyStudioAgent::GetAppId() const
{
	return ResolveAppId(ResolveSession());
}

FString UCrowdyStudioAgent::GetAgentToken() const
{
	const UCrowdyGameSession* Session = ResolveSession();
	return Session ? Session->GetGameToken() : FString();
}

void UCrowdyStudioAgent::SendAgentOp(const TCHAR* Operation, const TSharedPtr<FJsonObject>& Variables, TFunction<void(const FCrowdyCppJsonResult&)> OnAnswer,
	TFunction<void(ECrowdyStudioResult, const FString&, const FString&)> OnFailed)
{
	const bool bSignedIn = !GetAgentToken().IsEmpty();
	FCrowdyCppClient* Client = bSignedIn ? ResolveClient() : nullptr;
	if (!Client)
	{
		DeferWhileLive(LiveSessionToken, [OnFailed = MoveTemp(OnFailed), bSignedIn]()
		{
			OnFailed(bSignedIn ? ECrowdyStudioResult::Failed : ECrowdyStudioResult::NotSignedIn, bSignedIn ? StudioAgentNoClient : StudioAgentNotSignedIn, FString());
		});
		return;
	}
	const int64 AppId = GetAppId();
	const int64 UserId = ResolveSession()->GetUserID();

	TWeakObjectPtr<UCrowdyStudioAgent> WeakThis(this);
	RunOpDeferred(*Client, ECrowdyCppApiDomain::CrowdyStudioAgent, Operation, Variables, LiveSessionToken,
		[WeakThis, AppId, UserId, OnAnswer = MoveTemp(OnAnswer), OnFailed = MoveTemp(OnFailed)](FCrowdyCppJsonResult Result)
		{
			const UCrowdyStudioAgent* Self = WeakThis.Get();
			const UCrowdyGameSession* Session = Self ? Self->ResolveSession() : nullptr;
			if (!Session || Self->GetAppId() != AppId || Session->GetUserID() != UserId)
			{
				OnFailed(ECrowdyStudioResult::Failed, StudioAgentChanged, FString());
				return;
			}
			if (!Result.bTransportOk)
			{
				OnFailed(StudioAgentFailureResult(Result), FailureMessage(Result), FailureCode(Result));
				return;
			}
			if (!Result.Data.IsValid())
			{
				OnFailed(ECrowdyStudioResult::BadReply, StudioAgentNoAnswer, FString());
				return;
			}
			OnAnswer(Result);
		});
}

void UCrowdyStudioAgent::GetAgentBoot(TFunction<void(const FCrowdyStudioAgentBoot&)> OnDone)
{
	const bool bSignedIn = !GetAgentToken().IsEmpty();
	FString StudioOrigin;
	FString WalletUrl;
	FString Problem;
	const bool bSettings = bSignedIn && StudioAgentCheckSettings(StudioOrigin, WalletUrl, Problem);
	FCrowdyCppClient* Client = bSettings ? ResolveClient() : nullptr;
	const bool bSecure = Client && !StudioAgentSecureApiOrigin(CurrentApiEndpoint(*Client)).IsEmpty();
	if (!Client || !bSecure)
	{
		if (Client)
		{
			Problem = StudioAgentInsecureApi;
		}
		const bool bRefused = !bSettings || Client;
		const ECrowdyStudioResult Result = !bSignedIn ? ECrowdyStudioResult::NotSignedIn : (bRefused ? ECrowdyStudioResult::Refused : ECrowdyStudioResult::Failed);
		const FString Message = !bSignedIn ? FString(StudioAgentNotSignedIn) : (bRefused ? Problem : FString(StudioAgentNoClient));
		DeferWhileLive(LiveSessionToken, [OnDone = MoveTemp(OnDone), Result, Message]()
		{
			StudioAgentFinish(TEXT("Get Agent Boot"), OnDone, FCrowdyStudioAgentBoot(), Result, Message);
		});
		return;
	}

	const int64 AppId = GetAppId();
	const int64 UserId = ResolveSession()->GetUserID();
	TWeakObjectPtr<UCrowdyStudioAgent> WeakThis(this);
	const TSharedRef<bool> bInCall = MakeShared<bool>(true);
	TWeakPtr<uint8> WeakToken = LiveSessionToken;
	TFunction<void(FCrowdyCppStudioModelsResult)> Answer = [WeakThis, AppId, UserId, StudioOrigin, WalletUrl, OnDone](FCrowdyCppStudioModelsResult Models)
	{
		UCrowdyStudioAgent* Self = WeakThis.Get();
		const UCrowdyGameSession* Session = Self ? Self->ResolveSession() : nullptr;
		if (!Session || Self->GetAppId() != AppId || Session->GetUserID() != UserId)
		{
			StudioAgentFinish(TEXT("Get Agent Boot"), OnDone, FCrowdyStudioAgentBoot(), ECrowdyStudioResult::Failed, StudioAgentChanged);
			return;
		}
		if (!Models.bOk)
		{
			StudioAgentFinish(TEXT("Get Agent Boot"), OnDone, FCrowdyStudioAgentBoot(), StudioAgentModelsFailure(Models),
				Models.ErrorMessage, StudioAgentModelsCode(Models));
			return;
		}
		if (Models.Models.IsEmpty())
		{
			StudioAgentFinish(TEXT("Get Agent Boot"), OnDone, FCrowdyStudioAgentBoot(), ECrowdyStudioResult::NoModel, StudioAgentNoModels);
			return;
		}
		const FCrowdyCppClient* Now = Self->ResolveClient();
		const FString GraphQLUrl = Now ? Self->CurrentApiEndpoint(*Now) : FString();
		const FString ApiOrigin = StudioAgentSecureApiOrigin(GraphQLUrl);
		if (ApiOrigin.IsEmpty())
		{
			StudioAgentFinish(TEXT("Get Agent Boot"), OnDone, FCrowdyStudioAgentBoot(), ECrowdyStudioResult::Refused, StudioAgentInsecureApi);
			return;
		}
		Self->NoteApiOrigin(ApiOrigin);
		FCrowdyStudioAgentBoot Boot;
		Boot.AppId = AppId;
		Boot.GraphQLUrl = GraphQLUrl;
		Boot.ModelBaseUrl = ApiOrigin;
		Boot.ConnectOrigins.Add(ApiOrigin);
		Boot.StudioOrigin = StudioOrigin;
		Boot.WalletUrl = WalletUrl;
		StudioAgentFillModels(Models.Models, Boot.Models);
		StudioAgentFinish(TEXT("Get Agent Boot"), OnDone, MoveTemp(Boot), ECrowdyStudioResult::Success, FString());
	};
	Client->FetchStudioModels(AppId, GuardLifetime<FCrowdyCppStudioModelsResult>(LiveSessionToken,
		[bInCall, WeakToken, Answer = MoveTemp(Answer)](FCrowdyCppStudioModelsResult Result) mutable
		{
			if (!*bInCall)
			{
				Answer(MoveTemp(Result));
				return;
			}
			DeferWhileLive(WeakToken.Pin(), [Answer = MoveTemp(Answer), Result = MoveTemp(Result)]() mutable { Answer(MoveTemp(Result)); });
		}));
	*bInCall = false;
}

void UCrowdyStudioAgent::GetProviderConsent(TFunction<void(const FCrowdyStudioConsent&)> OnDone)
{
	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), BigInt(GetAppId()));
	SendConsent(TEXT("CrowdyStudioProviderConsent"), Variables, TEXT("crowdyStudioProviderConsent"), TEXT("Get Provider Consent"), TOptional<bool>(), MoveTemp(OnDone));
}

void UCrowdyStudioAgent::RecordProviderConsent(TFunction<void(const FCrowdyStudioConsent&)> OnDone, bool bConsented)
{
	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("appId"), BigInt(GetAppId()));
	Input->SetBoolField(TEXT("consented"), bConsented);
	SendConsent(TEXT("CrowdyStudioSetProviderConsent"), WrapInput(Input), TEXT("crowdyStudioSetProviderConsent"), TEXT("Record Provider Consent"),
		TOptional<bool>(bConsented), MoveTemp(OnDone));
}

void UCrowdyStudioAgent::SendConsent(const TCHAR* Operation, const TSharedPtr<FJsonObject>& Variables, const TCHAR* Field, const TCHAR* Call,
	TOptional<bool> Expected, TFunction<void(const FCrowdyStudioConsent&)> OnDone)
{
	const int64 AppId = GetAppId();
	SendAgentOp(Operation, Variables,
		[OnDone, Field, Call, AppId, Expected](const FCrowdyCppJsonResult& Result)
		{
			FCrowdyStudioConsent Consent;
			if (!StudioAgentReadConsent(StudioAgentAnswerObject(Result, Field), AppId, Consent))
			{
				StudioAgentFinish(Call, OnDone, FCrowdyStudioConsent(), ECrowdyStudioResult::BadReply, TEXT("The answer holds no well-formed consent."));
				return;
			}
			if (Expected.IsSet() && Consent.bConsented != Expected.GetValue())
			{
				StudioAgentFinish(Call, OnDone, FCrowdyStudioConsent(), ECrowdyStudioResult::Refused, TEXT("The platform did not record the change."));
				return;
			}
			StudioAgentFinish(Call, OnDone, MoveTemp(Consent), ECrowdyStudioResult::Success, FString());
		},
		StudioAgentFailWith<FCrowdyStudioConsent>(Call, OnDone));
}

FText UCrowdyStudioAgent::GetConsentNotice() const
{
	return NSLOCTEXT("CrowdyStudioAgent", "ConsentNotice",
		"The Studio agent sends your mod project's source and your messages to a model provider through Crowded Kingdoms. It sends the screenshots it takes and any you share. It sends the names and positions of other players near you. Your own position and view reach the provider too. Each request it makes is charged, to your wallet or the app's wallet as the app decides, and the usage line shows who pays. Your agent sessions stay on this computer, in a folder with no lock: another account on the same Windows profile can read them.");
}

void UCrowdyStudioAgent::GetModelUsage(TFunction<void(const FCrowdyStudioModelUsage&)> OnDone)
{
	const int64 AppId = GetAppId();
	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), BigInt(AppId));
	Variables->SetNumberField(TEXT("limit"), 1);
	SendAgentOp(TEXT("CrowdyStudioModelUsage"), Variables,
		[OnDone, AppId](const FCrowdyCppJsonResult& Result)
		{
			FCrowdyStudioModelUsage Usage;
			if (!StudioAgentReadUsage(StudioAgentAnswerObject(Result, TEXT("crowdyStudioModelUsage")), AppId, Usage))
			{
				StudioAgentFinish(TEXT("Get Model Usage"), OnDone, FCrowdyStudioModelUsage(), ECrowdyStudioResult::BadReply, TEXT("The answer holds no well-formed usage."));
				return;
			}
			StudioAgentFinish(TEXT("Get Model Usage"), OnDone, MoveTemp(Usage), ECrowdyStudioResult::Success, FString());
		},
		StudioAgentFailWith<FCrowdyStudioModelUsage>(TEXT("Get Model Usage"), OnDone));
}

void UCrowdyStudioAgent::AuthorizeLiveModding(ECrowdyStudioRunMode Action, TFunction<void(const FCrowdyStudioLiveModding&)> OnDone)
{
	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), BigInt(GetAppId()));
	Variables->SetStringField(TEXT("action"), StudioAgentActionName(Action));
	SendAgentOp(TEXT("CrowdyStudioLiveModding"), Variables,
		[OnDone](const FCrowdyCppJsonResult& Result)
		{
			FCrowdyStudioLiveModding Live;
			if (!ReadStrictBool(Result.Data, TEXT("crowdyStudioLiveModding"), Live.bAllowed))
			{
				StudioAgentFinish(TEXT("Authorize Live Modding"), OnDone, FCrowdyStudioLiveModding(), ECrowdyStudioResult::BadReply, TEXT("The answer holds no verdict."));
				return;
			}
			const FString Message = Live.bAllowed ? FString() : FString(TEXT("The app's Allow Live Modding switch does not allow this action."));
			StudioAgentFinish(TEXT("Authorize Live Modding"), OnDone, MoveTemp(Live), ECrowdyStudioResult::Success, Message);
		},
		StudioAgentFailWith<FCrowdyStudioLiveModding>(TEXT("Authorize Live Modding"), OnDone));
}
