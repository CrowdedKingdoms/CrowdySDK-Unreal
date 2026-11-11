#include "Subsystem/CrowdyMods.h"
#include "Containers/Ticker.h"
#include "CrowdyCppClient.h"
#include "CrowdyNativeExec.h"
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
	const TCHAR* const ModsLogName = TEXT("CrowdyMods");
	const TCHAR* const ModsNotSignedIn = TEXT("The player has no app token yet.");
	const TCHAR* const ModsNoClient = TEXT("The Crowdy API client is unavailable, so the request was not sent.");
	const TCHAR* const ModsAppChanged = TEXT("The app changed while the call was in flight.");
	const TCHAR* const ModsNoAnswer = TEXT("The answer holds nothing.");

	const TCHAR* ModsResultName(ECrowdyModResult Result)
	{
		switch (Result)
		{
		case ECrowdyModResult::Success:     return TEXT("Success");
		case ECrowdyModResult::NotSignedIn: return TEXT("NotSignedIn");
		case ECrowdyModResult::Denied:      return TEXT("Denied");
		case ECrowdyModResult::NotFound:    return TEXT("NotFound");
		case ECrowdyModResult::Refused:     return TEXT("Refused");
		case ECrowdyModResult::Failed:      return TEXT("Failed");
		case ECrowdyModResult::BadReply:    return TEXT("BadReply");
		}
		return TEXT("Unknown");
	}

	// Only the result and the platform's code are logged: ids, logs and the platform's text stay out.
	void ModsWarn(const TCHAR* Call, ECrowdyModResult Result, const FString& Code)
	{
		UE_CLOG(Result != ECrowdyModResult::Success, LogCrowdyServices, Warning, TEXT("[%s] %s ended %s%s%s"), ModsLogName, Call, ModsResultName(Result),
			Code.IsEmpty() ? TEXT("") : TEXT(": "), *Code);
	}

	template <typename AnswerType>
	void ModsFinish(const TCHAR* Call, const TFunction<void(const AnswerType&)>& OnDone, AnswerType Answer, ECrowdyModResult Result, const FString& Message,
		const FString& Code = FString())
	{
		Answer.Result = Result;
		Answer.Message = Message;
		ModsWarn(Call, Result, Code);
		OnDone(Answer);
	}

	// A failed call answers an empty result, so nothing from a refusal reads as data.
	template <typename AnswerType>
	TFunction<void(ECrowdyModResult, const FString&, const FString&)> ModsFailWith(const TCHAR* Call, const TFunction<void(const AnswerType&)>& OnDone)
	{
		return [Call, OnDone](ECrowdyModResult Result, const FString& Message, const FString& Code)
		{
			ModsFinish(Call, OnDone, AnswerType(), Result, Message, Code);
		};
	}

	ECrowdyModResult ModsFailureResult(const FCrowdyCppJsonResult& Result)
	{
		if (Result.ErrorCode.IsEmpty() || Result.ErrorMessage == FCrowdyCppClient::CanceledErrorMessage())
		{
			return ECrowdyModResult::Failed;
		}
		if (NamesCode(Result, TEXT("NOT_FOUND")) || Result.ErrorCode.EndsWith(TEXT("_NOT_FOUND"), ESearchCase::CaseSensitive))
		{
			return ECrowdyModResult::NotFound;
		}
		static const TCHAR* const DeniedCodes[] = {
			TEXT("FORBIDDEN"), TEXT("UNAUTHORIZED"), TEXT("UNAUTHENTICATED"), TEXT("NOT_ALLOWED"), TEXT("SCOPE_MISSING"),
			TEXT("PLAYER_WALLET_EMPTY"), TEXT("PLAYER_SPEND_CAP")};
		for (const TCHAR* Code : DeniedCodes)
		{
			if (NamesCode(Result, Code))
			{
				return ECrowdyModResult::Denied;
			}
		}
		return ECrowdyModResult::Refused;
	}

	bool ModsRefusalIsDenial(const FString& Refusal)
	{
		return Refusal.Contains(TEXT("FORBIDDEN"), ESearchCase::CaseSensitive) || Refusal.Contains(TEXT("UNAUTHORIZED"), ESearchCase::CaseSensitive)
			|| Refusal.Contains(TEXT("UNAUTHENTICATED"), ESearchCase::CaseSensitive) || Refusal.Contains(TEXT("ACCESS_"), ESearchCase::CaseSensitive)
			|| Refusal.Contains(TEXT("NOT_ALLOWED"), ESearchCase::CaseSensitive);
	}

	ECrowdyModResult ModsCallResult(ECrowdyNativeExecStatus Status)
	{
		switch (Status)
		{
		case ECrowdyNativeExecStatus::Ok:               return ECrowdyModResult::Success;
		case ECrowdyNativeExecStatus::Denied:           return ECrowdyModResult::Denied;
		case ECrowdyNativeExecStatus::NotFound:         return ECrowdyModResult::NotFound;
		case ECrowdyNativeExecStatus::Unavailable:
		case ECrowdyNativeExecStatus::DeadlineExceeded:
		case ECrowdyNativeExecStatus::Busy:
		case ECrowdyNativeExecStatus::Moved:
		case ECrowdyNativeExecStatus::RateLimited:
		case ECrowdyNativeExecStatus::AppPaused:        return ECrowdyModResult::Failed;
		default:                                        return ECrowdyModResult::Refused;
		}
	}

	TSharedPtr<FJsonObject> ModsAnswerObject(const FCrowdyCppJsonResult& Result, const TCHAR* Field)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		return Result.Data.IsValid() && Result.Data->TryGetObjectField(Field, Object) ? *Object : nullptr;
	}

	const TArray<TSharedPtr<FJsonValue>>* ModsAnswerArray(const FCrowdyCppJsonResult& Result, const TCHAR* Field)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		return Result.Data.IsValid() && Result.Data->TryGetArrayField(Field, Array) ? Array : nullptr;
	}

	TSharedPtr<FJsonObject> ModsElement(const TSharedPtr<FJsonValue>& Value)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		return Value.IsValid() && Value->TryGetObject(Object) ? *Object : nullptr;
	}

	// A DateTime arrives as ISO-8601 text.
	bool ModsReadTime(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, int64& OutMs)
	{
		FString Text;
		FDateTime Time;
		if (!Object->TryGetStringField(Field, Text) || !FDateTime::ParseIso8601(*Text, Time))
		{
			return false;
		}
		OutMs = (Time - FDateTime(1970, 1, 1)).GetTicks() / ETimespan::TicksPerMillisecond;
		return true;
	}

	bool ModsReadText(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FString& Out)
	{
		const TSharedPtr<FJsonValue> Value = Object->TryGetField(Field);
		if (!Value.IsValid() || Value->Type != EJson::String)
		{
			return false;
		}
		Out = Value->AsString();
		return true;
	}

	// TryGetBoolField reads "yes" or "1" as true; only a JSON boolean is a boolean here.
	bool ModsReadBool(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, bool& Out)
	{
		const TSharedPtr<FJsonValue> Value = Object.IsValid() ? Object->TryGetField(Field) : nullptr;
		if (!Value.IsValid() || Value->Type != EJson::Boolean)
		{
			return false;
		}
		Out = Value->AsBool();
		return true;
	}

	bool ModsReadMod(const TSharedPtr<FJsonObject>& Object, FCrowdyMod& Out)
	{
		if (!Object.IsValid())
		{
			return false;
		}
		// Null while the mod may run, else the reason it may not.
		const TSharedPtr<FJsonValue> Blocked = Object->TryGetField(TEXT("blocked"));
		if (!Blocked.IsValid() || (!Blocked->IsNull() && Blocked->Type != EJson::String))
		{
			return false;
		}
		Out.bBlocked = !Blocked->IsNull();
		Out.BlockedReason = Out.bBlocked ? Blocked->AsString() : FString();
		return ModsReadText(Object, TEXT("modId"), Out.ModId) && !Out.ModId.IsEmpty() && ReadWholeInt64(Object, TEXT("gridId"), Out.GridId)
			&& ReadWholeInt64(Object, TEXT("ownerId"), Out.OwnerUserId) && ModsReadText(Object, TEXT("name"), Out.Name)
			&& ReadWholeInt64(Object, TEXT("version"), Out.Version) && ModsReadText(Object, TEXT("digest"), Out.Digest)
			&& ModsReadBool(Object, TEXT("enabled"), Out.bEnabled) && ModsReadBool(Object, TEXT("running"), Out.bRunning)
			&& ModsReadTime(Object, TEXT("updatedAt"), Out.UpdatedAtMs);
	}

	// Every row must read, so a list is never shorter than the platform's without saying so.
	bool ModsReadList(const FCrowdyCppJsonResult& Result, const TCHAR* Field, TArray<FCrowdyMod>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Rows = ModsAnswerArray(Result, Field);
		if (!Rows)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Row : *Rows)
		{
			if (!ModsReadMod(ModsElement(Row), Out.AddDefaulted_GetRef()))
			{
				return false;
			}
		}
		return true;
	}

	bool ModsReadStatus(const FString& Text, ECrowdyModBuildStatus& Out)
	{
		if (Text.Equals(TEXT("queued"), ESearchCase::CaseSensitive))
		{
			Out = ECrowdyModBuildStatus::Queued;
			return true;
		}
		if (Text.Equals(TEXT("building"), ESearchCase::CaseSensitive))
		{
			Out = ECrowdyModBuildStatus::Building;
			return true;
		}
		if (Text.Equals(TEXT("succeeded"), ESearchCase::CaseSensitive))
		{
			Out = ECrowdyModBuildStatus::Succeeded;
			return true;
		}
		if (Text.Equals(TEXT("failed"), ESearchCase::CaseSensitive))
		{
			Out = ECrowdyModBuildStatus::Failed;
			return true;
		}
		return false;
	}

	bool ModsReadBuild(const TSharedPtr<FJsonObject>& Object, FCrowdyModBuild& Out)
	{
		FString Status;
		if (!Object.IsValid() || !ModsReadText(Object, TEXT("buildId"), Out.BuildId) || Out.BuildId.IsEmpty()
			|| !ModsReadText(Object, TEXT("status"), Status) || !ModsReadStatus(Status, Out.Status))
		{
			return false;
		}
		// Both are null until the build has started or finished.
		ModsReadText(Object, TEXT("log"), Out.Log);
		ModsReadText(Object, TEXT("sdkVersion"), Out.SdkVersion);
		return true;
	}

	bool ModsReadFile(const TSharedPtr<FJsonValue>& Value, FCrowdyModFile& Out)
	{
		const TSharedPtr<FJsonObject> Object = ModsElement(Value);
		return Object.IsValid() && ModsReadText(Object, TEXT("path"), Out.Path) && ModsReadText(Object, TEXT("content"), Out.Content);
	}

	bool ModsPathAllowed(const FString& Path)
	{
		if (Path.Equals(TEXT("Cargo.toml"), ESearchCase::CaseSensitive) || Path.Equals(TEXT("README.md"), ESearchCase::CaseSensitive))
		{
			return true;
		}
		const bool bSource = Path.StartsWith(TEXT("src/"), ESearchCase::CaseSensitive) && Path.EndsWith(TEXT(".rs"), ESearchCase::CaseSensitive);
		return bSource && Path.Len() > 7 && !Path.Contains(TEXT("\\")) && !Path.Contains(TEXT("//")) && !Path.Contains(TEXT("/../"))
			&& !Path.Contains(TEXT("/./"));
	}

	// Why the crate cannot be sent, or empty when it can.
	FString ModsCheckFiles(const TArray<FCrowdyModFile>& Files)
	{
		if (Files.Num() > UCrowdyMods::MaxFiles)
		{
			return FString::Printf(TEXT("A crate holds at most %d files; this one has %d, so it was not sent."), UCrowdyMods::MaxFiles, Files.Num());
		}
		int64 Bytes = 0;
		for (const FCrowdyModFile& File : Files)
		{
			if (!ModsPathAllowed(File.Path))
			{
				return FString::Printf(TEXT("A crate holds only Cargo.toml, README.md and .rs files under src/, not \"%s\", so it was not sent."), *File.Path);
			}
			Bytes += FTCHARToUTF8(*File.Content, File.Content.Len()).Length();
		}
		if (Bytes > UCrowdyMods::MaxSourceBytes)
		{
			return FString::Printf(TEXT("A crate holds at most %lld bytes of source; this one has %lld, so it was not sent."), UCrowdyMods::MaxSourceBytes, Bytes);
		}
		return FString();
	}

	TSharedPtr<FJsonObject> ModsGridVariables(int64 GridId, const FString& Name)
	{
		TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
		Variables->SetStringField(TEXT("gridId"), BigInt(GridId));
		Variables->SetStringField(TEXT("name"), Name);
		return Variables;
	}

	// A mod answered for another grid or name than the one asked for is not an answer to the call.
	bool ModsIsAsked(const FCrowdyMod& Mod, int64 GridId, const FString& Name)
	{
		return Mod.GridId == GridId && Mod.Name.Equals(Name, ESearchCase::CaseSensitive);
	}
}

struct UCrowdyMods::FBuild
{
	FCrowdyModBuild Current;
	double StartedAt = 0.0;
	bool bHasStatus = false;
	bool bFinished = false;
	TFunction<void(const FCrowdyModBuild&)> OnProgress;
	TFunction<void(const FCrowdyModBuild&)> OnDone;

	// Keeps what the platform said so far, so a failure after the build started still carries its id, status and log.
	void Finish(ECrowdyModResult Result, const FString& Message, const FString& Code = FString())
	{
		if (bFinished)
		{
			return;
		}
		bFinished = true;
		ModsFinish(TEXT("Build Mod"), OnDone, Current, Result, Message, Code);
	}
};

void UCrowdyMods::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	LiveSessionToken = MakeShared<uint8>(0);
}

void UCrowdyMods::Deinitialize()
{
	// Released first, so the calls the closes cancel answer nobody.
	LiveSessionToken.Reset();
	const TArray<TSharedPtr<FCrowdyNativeExecConnection>> Closing = MoveTemp(CallConnections);
	CallConnections.Reset();
	for (const TSharedPtr<FCrowdyNativeExecConnection>& Connection : Closing)
	{
		Connection->Close();
	}
	Super::Deinitialize();
}

#if WITH_DEV_AUTOMATION_TESTS
void UCrowdyMods::InitializeForTest(const TSharedPtr<FCrowdyCppClient>& Client, int64 AppId, const FString& GatewayDomain)
{
	LiveSessionToken = MakeShared<uint8>(0);
	ClientForTest = Client;
	AppIdForTest = AppId;
	GatewayDomainForTest = GatewayDomain;
}
#endif

int64 UCrowdyMods::GetAppId() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (AppIdForTest != 0)
	{
		return AppIdForTest;
	}
#endif
	return ResolveAppId(GetGameInstance());
}

bool UCrowdyMods::HasAppToken() const
{
	return !GetAppToken().IsEmpty();
}

FString UCrowdyMods::GetAppToken() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (ClientForTest.IsValid())
	{
		return TEXT("app-token");
	}
#endif
	const UGameInstance* GameInstance = GetGameInstance();
	const UCrowdyGameSession* Session = GameInstance ? GameInstance->GetSubsystem<UCrowdyGameSession>() : nullptr;
	return Session ? Session->GetGameToken() : FString();
}

FCrowdyCppClient* UCrowdyMods::ResolveClient() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (const TSharedPtr<FCrowdyCppClient> Override = ClientForTest.Pin())
	{
		return Override.Get();
	}
#endif
	return ResolveApiClient(GetGameInstance(), ModsLogName);
}

double UCrowdyMods::Now() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (NowForTest >= 0.0)
	{
		return NowForTest;
	}
#endif
	return FPlatformTime::Seconds();
}

void UCrowdyMods::SendModOp(const TCHAR* Operation, const TSharedPtr<FJsonObject>& Variables, TFunction<void(const FCrowdyCppJsonResult&)> OnAnswer,
	TFunction<void(ECrowdyModResult, const FString&, const FString&)> OnFailed)
{
	const bool bSignedIn = HasAppToken();
	FCrowdyCppClient* Client = bSignedIn ? ResolveClient() : nullptr;
	if (!Client)
	{
		DeferWhileLive(LiveSessionToken, [OnFailed = MoveTemp(OnFailed), bSignedIn]()
		{
			OnFailed(bSignedIn ? ECrowdyModResult::Failed : ECrowdyModResult::NotSignedIn, bSignedIn ? ModsNoClient : ModsNotSignedIn, FString());
		});
		return;
	}
	const int64 AppId = GetAppId();
	Variables->SetStringField(TEXT("appId"), BigInt(AppId));

	TWeakObjectPtr<UCrowdyMods> WeakThis(this);
	RunOpDeferred(*Client, ECrowdyCppApiDomain::Exec, Operation, Variables, LiveSessionToken,
		[WeakThis, AppId, OnAnswer = MoveTemp(OnAnswer), OnFailed = MoveTemp(OnFailed)](FCrowdyCppJsonResult Result)
		{
			const UCrowdyMods* Self = WeakThis.Get();
			if (!Self || Self->GetAppId() != AppId)
			{
				OnFailed(ECrowdyModResult::Failed, ModsAppChanged, FString());
				return;
			}
			if (!Result.bTransportOk)
			{
				OnFailed(ModsFailureResult(Result), FailureMessage(Result), FailureCode(Result));
				return;
			}
			if (!Result.Data.IsValid())
			{
				OnFailed(ECrowdyModResult::BadReply, ModsNoAnswer, FString());
				return;
			}
			OnAnswer(Result);
		});
}

void UCrowdyMods::BuildMod(const FString& CrateName, const TArray<FCrowdyModFile>& Files, TFunction<void(const FCrowdyModBuild&)> OnProgress,
	TFunction<void(const FCrowdyModBuild&)> OnDone)
{
	const TSharedRef<FBuild> Build = MakeShared<FBuild>();
	Build->OnProgress = MoveTemp(OnProgress);
	Build->OnDone = MoveTemp(OnDone);
	const FString Problem = ModsCheckFiles(Files);
	if (!Problem.IsEmpty())
	{
		DeferWhileLive(LiveSessionToken, [Build, Problem]() { Build->Finish(ECrowdyModResult::Refused, Problem); });
		return;
	}

	TArray<TSharedPtr<FJsonValue>> FileValues;
	for (const FCrowdyModFile& File : Files)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("path"), File.Path);
		Entry->SetStringField(TEXT("content"), File.Content);
		FileValues.Add(MakeShared<FJsonValueObject>(Entry));
	}
	TSharedPtr<FJsonObject> Crate = MakeShared<FJsonObject>();
	Crate->SetStringField(TEXT("name"), CrateName);
	Crate->SetArrayField(TEXT("files"), FileValues);
	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("crate"), Crate);

	Build->StartedAt = Now();
	TWeakObjectPtr<UCrowdyMods> WeakThis(this);
	SendModOp(TEXT("ExecModBuild"), Variables,
		[WeakThis, Build](const FCrowdyCppJsonResult& Result)
		{
			if (UCrowdyMods* Self = WeakThis.Get())
			{
				Self->ReadBuild(Build, Result, TEXT("execModBuild"));
			}
		},
		[Build](ECrowdyModResult Result, const FString& Message, const FString& Code) { Build->Finish(Result, Message, Code); });
}

void UCrowdyMods::PollBuild(const TSharedRef<FBuild>& Build)
{
	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("buildId"), Build->Current.BuildId);
	TWeakObjectPtr<UCrowdyMods> WeakThis(this);
	SendModOp(TEXT("ExecModBuildStatus"), Variables,
		[WeakThis, Build](const FCrowdyCppJsonResult& Result)
		{
			if (UCrowdyMods* Self = WeakThis.Get())
			{
				Self->ReadBuild(Build, Result, TEXT("execModBuildStatus"));
			}
		},
		[WeakThis, Build](ECrowdyModResult Result, const FString& Message, const FString& Code)
		{
			// The platform keeps building through a dropped status read, so only a refusal ends the wait.
			UCrowdyMods* Self = WeakThis.Get();
			if (Self && Result == ECrowdyModResult::Failed && Self->Now() - Build->StartedAt < BuildLimitSeconds)
			{
				Self->SchedulePoll(Build);
				return;
			}
			Build->Finish(Result, Message, Code);
		});
}

void UCrowdyMods::SchedulePoll(const TSharedRef<FBuild>& Build)
{
	TWeakObjectPtr<UCrowdyMods> WeakThis(this);
	TWeakPtr<uint8> Live = LiveSessionToken;
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakThis, Live, Build](float)
	{
		UCrowdyMods* Self = WeakThis.Get();
		if (Self && Live.IsValid())
		{
			Self->PollBuild(Build);
		}
		return false;
	}), BuildPollSeconds);
}

void UCrowdyMods::ReadBuild(const TSharedRef<FBuild>& Build, const FCrowdyCppJsonResult& Result, const TCHAR* Field)
{
	FCrowdyModBuild Read;
	if (!ModsReadBuild(ModsAnswerObject(Result, Field), Read)
		|| (!Build->Current.BuildId.IsEmpty() && !Read.BuildId.Equals(Build->Current.BuildId, ESearchCase::CaseSensitive)))
	{
		Build->Finish(ECrowdyModResult::BadReply, TEXT("The answer holds no well-formed status of this build."));
		return;
	}
	const bool bChanged = !Build->bHasStatus || Read.Status != Build->Current.Status;
	Build->bHasStatus = true;
	Build->Current.BuildId = Read.BuildId;
	Build->Current.Status = Read.Status;
	Build->Current.Log = Read.Log;
	Build->Current.SdkVersion = Read.SdkVersion;
	Build->Current.Result = ECrowdyModResult::Success;
	if (bChanged && Build->OnProgress)
	{
		Build->OnProgress(Build->Current);
	}
	// A progress listener may end the game instance, and with it the build.
	if (!LiveSessionToken.IsValid())
	{
		return;
	}
	if (Read.Status == ECrowdyModBuildStatus::Succeeded || Read.Status == ECrowdyModBuildStatus::Failed)
	{
		Build->Finish(ECrowdyModResult::Success, FString());
		return;
	}
	if (Now() - Build->StartedAt >= BuildLimitSeconds)
	{
		Build->Finish(ECrowdyModResult::Failed, TEXT("The build did not finish within 11 minutes."));
		return;
	}
	SchedulePoll(Build);
}

void UCrowdyMods::DeployMod(int64 GridId, const FString& Name, const FString& BuildId, TFunction<void(const FCrowdyModInfo&)> OnDone)
{
	const TSharedPtr<FJsonObject> Variables = ModsGridVariables(GridId, Name);
	Variables->SetStringField(TEXT("buildId"), BuildId);
	SendModOp(TEXT("ExecModDeploy"), Variables,
		[GridId, Name, OnDone](const FCrowdyCppJsonResult& Result)
		{
			FCrowdyModInfo Info;
			if (!ModsReadMod(ModsAnswerObject(Result, TEXT("execModDeploy")), Info.Mod) || !ModsIsAsked(Info.Mod, GridId, Name))
			{
				ModsFinish(TEXT("Deploy Mod"), OnDone, FCrowdyModInfo(), ECrowdyModResult::BadReply, TEXT("The answer holds no well-formed mod of that grid and name."));
				return;
			}
			ModsFinish(TEXT("Deploy Mod"), OnDone, Info, ECrowdyModResult::Success, FString());
		},
		ModsFailWith(TEXT("Deploy Mod"), OnDone));
}

void UCrowdyMods::SetModEnabled(int64 GridId, const FString& Name, bool bEnabled, TFunction<void(const FCrowdyModInfo&)> OnDone)
{
	const TSharedPtr<FJsonObject> Variables = ModsGridVariables(GridId, Name);
	Variables->SetBoolField(TEXT("enabled"), bEnabled);
	SendModOp(TEXT("ExecModSetEnabled"), Variables,
		[GridId, Name, OnDone](const FCrowdyCppJsonResult& Result)
		{
			FCrowdyModInfo Info;
			if (!ModsReadMod(ModsAnswerObject(Result, TEXT("execModSetEnabled")), Info.Mod) || !ModsIsAsked(Info.Mod, GridId, Name))
			{
				ModsFinish(TEXT("Set Mod Enabled"), OnDone, FCrowdyModInfo(), ECrowdyModResult::BadReply, TEXT("The answer holds no well-formed mod of that grid and name."));
				return;
			}
			ModsFinish(TEXT("Set Mod Enabled"), OnDone, Info, ECrowdyModResult::Success, FString());
		},
		ModsFailWith(TEXT("Set Mod Enabled"), OnDone));
}

void UCrowdyMods::ListMods(int64 GridId, TFunction<void(const FCrowdyModList&)> OnDone)
{
	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("gridId"), BigInt(GridId));
	SendModOp(TEXT("ExecMods"), Variables,
		[GridId, OnDone](const FCrowdyCppJsonResult& Result)
		{
			FCrowdyModList List;
			const bool bRead = ModsReadList(Result, TEXT("execMods"), List.Mods);
			if (!bRead || List.Mods.ContainsByPredicate([GridId](const FCrowdyMod& Mod) { return Mod.GridId != GridId; }))
			{
				ModsFinish(TEXT("List Mods"), OnDone, FCrowdyModList(), ECrowdyModResult::BadReply, TEXT("The answer holds no well-formed list of that grid's mods."));
				return;
			}
			ModsFinish(TEXT("List Mods"), OnDone, MoveTemp(List), ECrowdyModResult::Success, FString());
		},
		ModsFailWith(TEXT("List Mods"), OnDone));
}

void UCrowdyMods::ListMyMods(TFunction<void(const FCrowdyModList&)> OnDone)
{
	SendModOp(TEXT("ExecMyMods"), MakeShared<FJsonObject>(),
		[OnDone](const FCrowdyCppJsonResult& Result)
		{
			FCrowdyModList List;
			if (!ModsReadList(Result, TEXT("execMyMods"), List.Mods))
			{
				ModsFinish(TEXT("List My Mods"), OnDone, FCrowdyModList(), ECrowdyModResult::BadReply, TEXT("The answer holds no well-formed list of mods."));
				return;
			}
			ModsFinish(TEXT("List My Mods"), OnDone, MoveTemp(List), ECrowdyModResult::Success, FString());
		},
		ModsFailWith(TEXT("List My Mods"), OnDone));
}

void UCrowdyMods::GetModLogs(int64 GridId, const FString& Name, int32 Limit, TFunction<void(const FCrowdyModLogs&)> OnDone)
{
	if (Limit < 0 || Limit > MaxLogLines)
	{
		const FString Message = FString::Printf(TEXT("A log read asks for 1 to %d lines, or 0 for %d; %d was not sent."), MaxLogLines, DefaultLogLines, Limit);
		DeferWhileLive(LiveSessionToken, [OnDone = MoveTemp(OnDone), Message]()
		{
			ModsFinish(TEXT("Get Mod Logs"), OnDone, FCrowdyModLogs(), ECrowdyModResult::Refused, Message);
		});
		return;
	}
	const TSharedPtr<FJsonObject> Variables = ModsGridVariables(GridId, Name);
	Variables->SetNumberField(TEXT("limit"), Limit == 0 ? DefaultLogLines : Limit);
	SendModOp(TEXT("ExecModLogs"), Variables,
		[OnDone](const FCrowdyCppJsonResult& Result)
		{
			const TArray<TSharedPtr<FJsonValue>>* Rows = ModsAnswerArray(Result, TEXT("execModLogs"));
			FCrowdyModLogs Logs;
			bool bRead = Rows != nullptr;
			// The platform answers newest first; read from the end so the lines come out oldest first.
			for (int32 Index = bRead ? Rows->Num() - 1 : -1; bRead && Index >= 0; --Index)
			{
				const TSharedPtr<FJsonObject> Row = ModsElement((*Rows)[Index]);
				FCrowdyModLogLine& Line = Logs.Lines.AddDefaulted_GetRef();
				int64 Level = 0;
				bRead = Row.IsValid() && ReadWholeInt64(Row, TEXT("level"), Level) && Level >= MIN_int32 && Level <= MAX_int32
					&& ModsReadTime(Row, TEXT("at"), Line.AtMs) && ModsReadText(Row, TEXT("text"), Line.Text);
				Line.Level = static_cast<int32>(Level);
			}
			if (!bRead)
			{
				ModsFinish(TEXT("Get Mod Logs"), OnDone, FCrowdyModLogs(), ECrowdyModResult::BadReply, TEXT("The answer holds no well-formed log lines."));
				return;
			}
			ModsFinish(TEXT("Get Mod Logs"), OnDone, MoveTemp(Logs), ECrowdyModResult::Success, FString());
		},
		ModsFailWith(TEXT("Get Mod Logs"), OnDone));
}

void UCrowdyMods::DeleteMod(int64 GridId, const FString& Name, TFunction<void(const FCrowdyModDelete&)> OnDone)
{
	SendModOp(TEXT("ExecModDelete"), ModsGridVariables(GridId, Name),
		[OnDone](const FCrowdyCppJsonResult& Result)
		{
			FCrowdyModDelete Delete;
			if (!ModsReadBool(Result.Data, TEXT("execModDelete"), Delete.bDeleted))
			{
				ModsFinish(TEXT("Delete Mod"), OnDone, FCrowdyModDelete(), ECrowdyModResult::BadReply, TEXT("The answer holds no well-formed deletion."));
				return;
			}
			ModsFinish(TEXT("Delete Mod"), OnDone, Delete, ECrowdyModResult::Success, FString());
		},
		ModsFailWith(TEXT("Delete Mod"), OnDone));
}

void UCrowdyMods::GetStarter(TFunction<void(const FCrowdyModStarter&)> OnDone)
{
	SendModOp(TEXT("ExecModStarter"), MakeShared<FJsonObject>(),
		[OnDone](const FCrowdyCppJsonResult& Result)
		{
			const TSharedPtr<FJsonObject> Answer = ModsAnswerObject(Result, TEXT("execModStarter"));
			const TArray<TSharedPtr<FJsonValue>>* Files = nullptr;
			FCrowdyModStarter Starter;
			bool bRead = Answer.IsValid() && ModsReadText(Answer, TEXT("crate"), Starter.Name) && !Starter.Name.IsEmpty()
				&& Answer->TryGetArrayField(TEXT("files"), Files) && Files->Num() > 0;
			for (int32 Index = 0; bRead && Index < Files->Num(); ++Index)
			{
				bRead = ModsReadFile((*Files)[Index], Starter.Files.AddDefaulted_GetRef());
			}
			if (!bRead)
			{
				ModsFinish(TEXT("Get Starter"), OnDone, FCrowdyModStarter(), ECrowdyModResult::BadReply, TEXT("The answer holds no well-formed starter crate."));
				return;
			}
			// Callers write these paths to disk, so the starter must pass the same check as a build.
			const FString Problem = ModsCheckFiles(Starter.Files);
			if (!Problem.IsEmpty())
			{
				ModsFinish(TEXT("Get Starter"), OnDone, FCrowdyModStarter(), ECrowdyModResult::BadReply, FString::Printf(TEXT("The starter crate is not buildable: %s"), *Problem));
				return;
			}
			ModsFinish(TEXT("Get Starter"), OnDone, MoveTemp(Starter), ECrowdyModResult::Success, FString());
		},
		ModsFailWith(TEXT("Get Starter"), OnDone));
}

void UCrowdyMods::CallMod(int64 GridId, const FString& Name, const FString& Method, const FString& ParamsJson, TFunction<void(const FCrowdyModCall&)> OnDone)
{
	// No params travel as an empty map, as a Server Function with no inputs does.
	TArray<uint8> Payload;
	const bool bParams = CrowdyNativeExec::JsonToMsgpack(ParamsJson.IsEmpty() ? FString(TEXT("{}")) : ParamsJson, Payload);
	const bool bSignedIn = HasAppToken();
	FCrowdyCppClient* Client = bParams && bSignedIn ? ResolveClient() : nullptr;

	// The dial names no node type: the gateway refuses one that names a mod's.
	FCrowdyNativeExecOptions Options;
	const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>();
	Options.AllowedGatewayDomains = Settings->AllowedExecGatewayDomains;
	Options.CallTimeoutMs = Settings->GetServerObjectCallTimeoutMs();
	Options.OpenTimeoutMs = Settings->GetServerObjectOpenTimeoutMs();
#if WITH_DEV_AUTOMATION_TESTS
	if (!GatewayDomainForTest.IsEmpty())
	{
		Options.AllowedGatewayDomains.Add(GatewayDomainForTest);
	}
#endif
	TWeakObjectPtr<UCrowdyMods> WeakThis(this);
	const TSharedPtr<FCrowdyNativeExecConnection> Connection = Client ? Client->CreateExecConnection(GetAppId(), Options, [WeakThis]()
	{
		const UCrowdyMods* Self = WeakThis.Get();
		return Self ? Self->GetAppToken() : FString();
	}) : nullptr;
	if (!Connection.IsValid())
	{
		const ECrowdyModResult Result = !bParams ? ECrowdyModResult::Refused : !bSignedIn ? ECrowdyModResult::NotSignedIn : ECrowdyModResult::Failed;
		const FString Message = !bParams ? TEXT("The params are not one JSON value, so the call was not sent.")
			: !bSignedIn ? ModsNotSignedIn
			: Client ? TEXT("The gateway connection could not be made, so the call was not sent.") : ModsNoClient;
		DeferWhileLive(LiveSessionToken, [OnDone = MoveTemp(OnDone), Result, Message]()
		{
			ModsFinish(TEXT("Call Mod"), OnDone, FCrowdyModCall(), Result, Message);
		});
		return;
	}
	CallConnections.Add(Connection);

	const TSharedRef<bool> bInCall = MakeShared<bool>(true);
	const TWeakPtr<uint8> Live = LiveSessionToken;
	const TWeakPtr<FCrowdyNativeExecConnection> WeakConnection = Connection;
	Connection->Call(FString::Printf(TEXT("mod:%s"), *Name), BigInt(GridId), Method, MoveTemp(Payload),
		[WeakThis, Live, WeakConnection, bInCall, OnDone = MoveTemp(OnDone)](const FCrowdyNativeExecReply& Reply)
		{
			if (!Live.IsValid())
			{
				return;
			}
			const TSharedPtr<FCrowdyNativeExecConnection> Pinned = WeakConnection.Pin();
			if (UCrowdyMods* Self = WeakThis.Get())
			{
				Self->ReleaseCallConnection(Pinned);
			}
			FCrowdyModCall Answer;
			ECrowdyModResult Result = ModsCallResult(Reply.Status);
			const FString Code = CrowdyNativeExec::StatusName(Reply.Status);
			FString Message = Result == ECrowdyModResult::Success ? FString() : FString::Printf(TEXT("%s: %s"), *Code, *Reply.Message);
			// A refused dial fails the call as Unavailable; the platform's reason is on the connection.
			const FString Refusal = Reply.Status == ECrowdyNativeExecStatus::Unavailable && Pinned.IsValid() && !Pinned->IsConnected()
				? Pinned->GetRefusal() : FString();
			if (!Refusal.IsEmpty())
			{
				Result = ModsRefusalIsDenial(Refusal) ? ECrowdyModResult::Denied : ECrowdyModResult::Failed;
				Message = Refusal;
			}
			if (Result == ECrowdyModResult::Success && !CrowdyNativeExec::MsgpackToJson(Reply.Payload, Answer.ReplyJson))
			{
				Result = ECrowdyModResult::BadReply;
				Message = TEXT("The mod's answer is not one MessagePack value.");
			}
			if (Result != ECrowdyModResult::Success)
			{
				Answer.ReplyJson.Reset();
			}
			if (!*bInCall)
			{
				ModsFinish(TEXT("Call Mod"), OnDone, MoveTemp(Answer), Result, Message, Code);
				return;
			}
			DeferWhileLive(Live.Pin(), [OnDone, Answer = MoveTemp(Answer), Result, Message, Code]()
			{
				ModsFinish(TEXT("Call Mod"), OnDone, Answer, Result, Message, Code);
			});
		});
	*bInCall = false;
}

void UCrowdyMods::ReleaseCallConnection(const TSharedPtr<FCrowdyNativeExecConnection>& Connection)
{
	if (!Connection.IsValid() || CallConnections.Remove(Connection) == 0)
	{
		return;
	}
	// Closed on a later tick, never inside its own reply.
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Connection](float)
	{
		Connection->Close();
		return false;
	}));
}
