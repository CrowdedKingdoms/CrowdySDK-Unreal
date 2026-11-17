#include "Subsystem/CrowdyMods.h"
#include "Containers/Ticker.h"
#include "CrowdyCppClient.h"
#include "CrowdyCppStudio.h"
#include "CrowdyNativeExec.h"
#include "CrowdyServiceApiSupport.h"
#include "CrowdyServicesLog.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Misc/DateTime.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Subsystem/CrowdyModScripts.h"
#include "Subsystem/CrowdyStudioProjects.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

using namespace CrowdyServiceApi;

namespace
{
	const TCHAR* const ModsLogName = TEXT("CrowdyMods");
	const TCHAR* const ModsNotSignedIn = TEXT("The player has no app token yet.");
	const TCHAR* const ModsNoClient = TEXT("The Crowdy API client is unavailable, so the request was not sent.");
	const TCHAR* const ModsCallerChanged = TEXT("The player or app changed while the call was in flight.");
	const TCHAR* const ModsRunCallerChanged = TEXT("The player or app changed while the run went, so nothing more was sent.");
	const TCHAR* const ModsNoAnswer = TEXT("The answer holds nothing.");
	const TCHAR* const ModsNoProjects = TEXT("The Studio projects service is unavailable, so nothing was sent.");
	const TCHAR* const ModsRunCall = TEXT("Run Project");
	const TCHAR* const ModsStatusCall = TEXT("Get Project Status");
	constexpr int32 ModsMaxModNameChars = 48;

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
		case ECrowdyModResult::Busy:        return TEXT("Busy");
		case ECrowdyModResult::RevisionChanged: return TEXT("RevisionChanged");
		case ECrowdyModResult::NotBuilt:    return TEXT("NotBuilt");
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
		// One build per player at a time.
		if (NamesCode(Result, TEXT("RATE_LIMITED")))
		{
			return ECrowdyModResult::Busy;
		}
		// The platform's own failure, such as a full build queue: worth trying again, not a refusal.
		if (NamesCode(Result, TEXT("INTERNAL_SERVER_ERROR")))
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

	template <typename AnswerType>
	void ModsDeferAnswer(const TSharedPtr<uint8>& LiveToken, const TCHAR* Call, TFunction<void(const AnswerType&)> OnDone, AnswerType Answer,
		ECrowdyModResult Result, const FString& Message)
	{
		DeferWhileLive(LiveToken, [Call, OnDone = MoveTemp(OnDone), Answer = MoveTemp(Answer), Result, Message]()
		{
			ModsFinish(Call, OnDone, Answer, Result, Message);
		});
	}

	bool ModsReadClient(const TSharedPtr<FJsonObject>& Object, FCrowdyModClient& Out)
	{
		int64 Tick = 0;
		if (!Object.IsValid() || !ReadWholeInt64(Object, TEXT("tickIntervalMs"), Tick) || Tick < 0 || Tick > MAX_int32)
		{
			return false;
		}
		Out.TickIntervalMs = static_cast<int32>(Tick);
		return ModsReadText(Object, TEXT("modId"), Out.ModId) && !Out.ModId.IsEmpty() && ReadWholeInt64(Object, TEXT("gridId"), Out.GridId)
			&& ModsReadText(Object, TEXT("name"), Out.Name) && ReadWholeInt64(Object, TEXT("ownerId"), Out.OwnerUserId)
			&& ReadWholeInt64(Object, TEXT("clientVersion"), Out.ClientVersion) && ModsReadText(Object, TEXT("digest"), Out.Digest)
			&& ReadWholeInt64(Object, TEXT("sizeBytes"), Out.SizeBytes) && ModsReadText(Object, TEXT("capabilitySummaryJson"), Out.CapabilitySummaryJson)
			&& ModsReadText(Object, TEXT("capabilityHash"), Out.CapabilityHash) && !Out.CapabilityHash.IsEmpty()
			&& ModsReadTime(Object, TEXT("updatedAt"), Out.UpdatedAtMs);
	}

	// One execGridClientMods row of GridId: a CLIENT half the grid serves now.
	bool ModsReadServedClient(const TSharedPtr<FJsonValue>& Value, int64 GridId, FCrowdyModClient& Out)
	{
		const TSharedPtr<FJsonObject> Row = ModsElement(Value);
		return Row.IsValid() && ModsReadText(Row, TEXT("modId"), Out.ModId) && !Out.ModId.IsEmpty() && ModsReadText(Row, TEXT("name"), Out.Name)
			&& ReadWholeInt64(Row, TEXT("gridId"), Out.GridId) && Out.GridId == GridId && ReadWholeInt64(Row, TEXT("clientVersion"), Out.ClientVersion)
			&& ModsReadText(Row, TEXT("digest"), Out.Digest);
	}

	ECrowdyModResult ModsFromStudioResult(ECrowdyStudioResult Result)
	{
		switch (Result)
		{
		case ECrowdyStudioResult::Success:     return ECrowdyModResult::Success;
		case ECrowdyStudioResult::NotSignedIn: return ECrowdyModResult::NotSignedIn;
		case ECrowdyStudioResult::Denied:      return ECrowdyModResult::Denied;
		case ECrowdyStudioResult::NotFound:    return ECrowdyModResult::NotFound;
		case ECrowdyStudioResult::Refused:     return ECrowdyModResult::Refused;
		case ECrowdyStudioResult::BadReply:    return ECrowdyModResult::BadReply;
		case ECrowdyStudioResult::NotBuilt:    return ECrowdyModResult::NotBuilt;
		case ECrowdyStudioResult::NoModel:     return ECrowdyModResult::Refused;
		default:                               return ECrowdyModResult::Failed;
		}
	}

	bool ModsKindHasServer(ECrowdyStudioProjectKind Kind)
	{
		return Kind != ECrowdyStudioProjectKind::Client;
	}

	bool ModsKindHasClient(ECrowdyStudioProjectKind Kind)
	{
		return Kind != ECrowdyStudioProjectKind::Server;
	}

	// As the platform's mod names: 1 to 48 lowercase letters, digits, - and _.
	bool ModsIsModName(const FString& Name)
	{
		if (Name.IsEmpty() || Name.Len() > ModsMaxModNameChars)
		{
			return false;
		}
		for (const TCHAR Char : Name)
		{
			if (!(Char >= TEXT('a') && Char <= TEXT('z')) && !(Char >= TEXT('0') && Char <= TEXT('9')) && Char != TEXT('-') && Char != TEXT('_'))
			{
				return false;
			}
		}
		return true;
	}

	// A crate's name starts with a letter, so a module named otherwise builds as mod-<name>.
	FString ModsCrateName(const FString& Module)
	{
		return !Module.IsEmpty() && Module[0] >= TEXT('a') && Module[0] <= TEXT('z') ? Module : FString::Printf(TEXT("mod-%s"), *Module);
	}

	// Why the project's halves cannot run as mods, or empty when they can.
	FString ModsCheckModules(ECrowdyStudioProjectKind Kind, const FString& ServerModule, const FString& ClientModule)
	{
		if (ModsKindHasServer(Kind) && !ModsIsModName(ServerModule))
		{
			return FString::Printf(TEXT("The SERVER module name must be 1 to %d lowercase letters, digits, - and _ to run as a mod, so nothing was built."), ModsMaxModNameChars);
		}
		if (ModsKindHasClient(Kind) && !ModsIsModName(ClientModule))
		{
			return FString::Printf(TEXT("The CLIENT module name must be 1 to %d lowercase letters, digits, - and _ to build, so nothing was built."), ModsMaxModNameChars);
		}
		return FString();
	}

	// The module names pick the mods a run overwrites, so a name other than the one asked for refuses the run.
	FString ModsCheckExpectedModules(const FCrowdyStudioProject& Project, const FString& ExpectedServerModule, const FString& ExpectedClientModule)
	{
		if (!ExpectedServerModule.IsEmpty() && !Project.ServerModule.Equals(ExpectedServerModule, ESearchCase::CaseSensitive))
		{
			return FString::Printf(TEXT("The project's SERVER module is now \"%s\", not \"%s\", so nothing was built."), *Project.ServerModule, *ExpectedServerModule);
		}
		if (!ExpectedClientModule.IsEmpty() && !Project.ClientModule.Equals(ExpectedClientModule, ESearchCase::CaseSensitive))
		{
			return FString::Printf(TEXT("The project's CLIENT module is now \"%s\", not \"%s\", so nothing was built."), *Project.ClientModule, *ExpectedClientModule);
		}
		return FString();
	}

	// The crate files of one half, as the project read holds them.
	TArray<FCrowdyModFile> ModsTargetFiles(const TArray<FCrowdyStudioProjectFile>& Files, ECrowdyStudioTarget Target)
	{
		TArray<FCrowdyModFile> Crate;
		for (const FCrowdyStudioProjectFile& File : Files)
		{
			if (File.Target != Target || !ModsPathAllowed(File.Path))
			{
				continue;
			}
			FCrowdyModFile& Out = Crate.AddDefaulted_GetRef();
			Out.Path = File.Path;
			Out.Content = File.Content;
		}
		return Crate;
	}

	ECrowdyModDiagnosticSeverity ModsSeverity(ECrowdyCppStudioDiagnosticSeverity Severity)
	{
		switch (Severity)
		{
		case ECrowdyCppStudioDiagnosticSeverity::Error:   return ECrowdyModDiagnosticSeverity::Error;
		case ECrowdyCppStudioDiagnosticSeverity::Warning: return ECrowdyModDiagnosticSeverity::Warning;
		case ECrowdyCppStudioDiagnosticSeverity::Hint:    return ECrowdyModDiagnosticSeverity::Hint;
		default:                                          return ECrowdyModDiagnosticSeverity::Info;
		}
	}

	ECrowdyModDiagnosticSource ModsSource(ECrowdyCppStudioDiagnosticSource Source)
	{
		switch (Source)
		{
		case ECrowdyCppStudioDiagnosticSource::Rustc:   return ECrowdyModDiagnosticSource::Rustc;
		case ECrowdyCppStudioDiagnosticSource::Runtime: return ECrowdyModDiagnosticSource::Runtime;
		default:                                        return ECrowdyModDiagnosticSource::Local;
		}
	}

	FCrowdyModDiagnostic ModsDiagnostic(const FCrowdyCppStudioDiagnostic& Parsed)
	{
		FCrowdyModDiagnostic Out;
		Out.Target = Parsed.Target == ECrowdyCppStudioTarget::Client ? ECrowdyStudioTarget::Client : ECrowdyStudioTarget::Server;
		Out.Source = ModsSource(Parsed.Source);
		Out.Path = Parsed.Path;
		Out.Line = Parsed.Line;
		Out.Column = Parsed.Column;
		Out.EndLine = Parsed.EndLine;
		Out.EndColumn = Parsed.EndColumn;
		Out.Severity = ModsSeverity(Parsed.Severity);
		Out.Message = Parsed.Message;
		Out.Code = Parsed.Code;
		return Out;
	}

	void ModsAddHalf(TArray<FCrowdyModRunTarget>& Targets, ECrowdyStudioTarget Target, const FString& Module, const FString& SavedRevision)
	{
		FCrowdyModRunTarget& Added = Targets.AddDefaulted_GetRef();
		Added.Target = Target;
		Added.Module = Module;
		Added.SavedRevision = SavedRevision;
	}

	FCrowdyModRunTarget* ModsFindHalf(TArray<FCrowdyModRunTarget>& Targets, ECrowdyStudioTarget Target)
	{
		return Targets.FindByPredicate([Target](const FCrowdyModRunTarget& Entry) { return Entry.Target == Target; });
	}

	// The SERVER half as its mod row says it is.
	void ModsApplyServerRow(FCrowdyModRunTarget& Target, const FCrowdyMod& Mod)
	{
		Target.ModId = Mod.ModId;
		Target.Digest = Mod.Digest;
		Target.Version = Mod.Version;
		Target.Phase = Mod.bRunning && !Mod.bBlocked ? ECrowdyModRunPhase::Running : ECrowdyModRunPhase::Deployed;
		if (Mod.bBlocked)
		{
			Target.Message = FString::Printf(TEXT("Deployed, but blocked: %s"), *Mod.BlockedReason);
			return;
		}
		if (!Mod.bEnabled)
		{
			Target.Message = TEXT("Deployed, but switched off.");
			return;
		}
		Target.Message = Mod.bRunning ? FString() : FString(TEXT("Switched on; it starts on its first call."));
	}

	bool ModsClientRunsHere(const UGameInstance* GameInstance, const FCrowdyModClient& Served)
	{
		const UWorld* World = GameInstance ? GameInstance->GetWorld() : nullptr;
		const UCrowdyModScripts* Scripts = World ? World->GetSubsystem<UCrowdyModScripts>() : nullptr;
		if (!Scripts)
		{
			return false;
		}
		return Scripts->GetRunning().ContainsByPredicate([&Served](const FCrowdyModScript& Script)
		{
			return Script.ModId.Equals(Served.ModId, ESearchCase::CaseSensitive) && Script.Digest.Equals(Served.Digest, ESearchCase::CaseSensitive);
		});
	}

	struct FModsRunSlot
	{
		int32 Serial = 0;
		int64 GridId = 0;
		FString ProjectId;
		/** Where the run is, for a status read while it goes. */
		TArray<FCrowdyModRunTarget> Targets;
	};

	struct FModsDeployRecord
	{
		FString Revision;
		FString Digest;
	};

	// Process-wide, so two game instances of one app (as in a two-player editor session) never run it twice at once. Game thread only.
	struct FModsRunRegistry
	{
		TMap<int64, FModsRunSlot> InFlight;
		TMap<FString, FModsDeployRecord> Deployed;
		int32 NextSerial = 0;
	};

	FModsRunRegistry& ModsRuns()
	{
		static FModsRunRegistry Registry;
		return Registry;
	}

	FString ModsDeployKey(int64 AppId, int64 GridId, const FString& ProjectId, ECrowdyStudioTarget Target, const FString& Module)
	{
		return FString::Printf(TEXT("%lld/%lld/%s/%s/%s"), AppId, GridId, *ProjectId.ToLower(), Target == ECrowdyStudioTarget::Server ? TEXT("SERVER") : TEXT("CLIENT"), *Module);
	}

	// The revision a run in this process deployed under ServedDigest, or empty when the platform serves something else.
	FString ModsRunningRevision(const FString& Key, const FString& ServedDigest)
	{
		const FModsDeployRecord* Record = ModsRuns().Deployed.Find(Key);
		return Record && !ServedDigest.IsEmpty() && Record->Digest.Equals(ServedDigest, ESearchCase::CaseSensitive) ? Record->Revision : FString();
	}

	// A status read while a run goes shows the step the run is on.
	void ModsOverlayInFlight(TArray<FCrowdyModRunTarget>& Targets, const TArray<FCrowdyModRunTarget>& InFlight)
	{
		for (const FCrowdyModRunTarget& Running : InFlight)
		{
			FCrowdyModRunTarget* Read = ModsFindHalf(Targets, Running.Target);
			const bool bActive = Running.Phase == ECrowdyModRunPhase::Building || Running.Phase == ECrowdyModRunPhase::Deploying;
			if (Read && bActive)
			{
				Read->Phase = Running.Phase;
				Read->Message = Running.Message;
			}
		}
	}
}

struct UCrowdyMods::FBuild
{
	FCrowdyModBuild Current;
	double StartedAt = 0.0;
	int64 AppId = 0;
	int64 UserId = 0;
	const TCHAR* Call = TEXT("Build Mod");
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
		ModsFinish(Call, OnDone, Current, Result, Message, Code);
	}
};

// One Run Project, its steps in the order they run. Each step checks the run is still live before it touches anything.
struct UCrowdyMods::FRun : public TSharedFromThis<UCrowdyMods::FRun>
{
	TWeakObjectPtr<UCrowdyMods> Mods;
	TFunction<void(const FCrowdyModRun&)> OnDone;
	FCrowdyModRun Answer;
	FString ExpectedRevision;
	FString ExpectedServerModule;
	FString ExpectedClientModule;
	FString ServerModule;
	FString ClientModule;
	FString ClientBuildId;
	TArray<FCrowdyModFile> ServerFiles;
	TArray<FCrowdyModFile> ClientFiles;
	int64 AppId = 0;
	int64 UserId = 0;
	int32 Serial = 0;
	ECrowdyStudioProjectKind Kind = ECrowdyStudioProjectKind::Server;
	bool bFinished = false;

	UCrowdyMods* Alive();
	FCrowdyModRunTarget* Half(ECrowdyStudioTarget Target) { return ModsFindHalf(Answer.Targets, Target); }
	void SetPhase(ECrowdyStudioTarget Target, ECrowdyModRunPhase Phase, const FString& Message);
	void Publish();
	void AddLog(ECrowdyStudioTarget Target, const FString& Log);
	void Record(ECrowdyStudioTarget Target, const FString& Module, const FString& Digest);

	void ReadProject(const FCrowdyStudioProjectInfo& Info);
	void BuildClient();
	void ClientBuilt(const FCrowdyModBuild& Build);
	void BuildServer();
	void ServerBuilt(const FCrowdyModBuild& Build);
	void ServerDeployed(const FCrowdyModInfo& Info);
	void ServerSwitched(const FCrowdyModInfo& Info);
	void FindRide();
	void RideListed(const FCrowdyModList& List);
	void StarterRead(const FCrowdyModStarter& Starter);
	void StarterBuilt(const FCrowdyModBuild& Build);
	void StarterDeployed(const FCrowdyModInfo& Info);
	void SwitchRideOn();
	void RideSwitched(const FCrowdyModInfo& Info);
	void AttachClient();
	void ClientAttached(const FCrowdyModClientInfo& Info);
	void Consented(const FCrowdyCppJsonResult& Result);
	void ConsentRefused(ECrowdyModResult Result, const FString& Message);

	void Fail(ECrowdyStudioTarget Target, ECrowdyModRunPhase Phase, ECrowdyModResult Result, const FString& Message);
	void Finish(ECrowdyModResult Result, const FString& Message);
	void End();
};

// One Get Project Status: the project, then its SERVER mod, then its served CLIENT half.
struct UCrowdyMods::FStatusRead : public TSharedFromThis<UCrowdyMods::FStatusRead>
{
	TWeakObjectPtr<UCrowdyMods> Mods;
	TFunction<void(const FCrowdyModProjectStatus&)> OnDone;
	FCrowdyModProjectStatus Answer;
	FString ServerModule;
	FString ClientModule;
	int64 AppId = 0;
	int64 UserId = 0;
	ECrowdyStudioProjectKind Kind = ECrowdyStudioProjectKind::Server;
	bool bFinished = false;

	UCrowdyMods* Alive();
	FCrowdyModRunTarget* Half(ECrowdyStudioTarget Target) { return ModsFindHalf(Answer.Targets, Target); }
	void ReadProject(const FCrowdyStudioProjectInfo& Info);
	void ReadServer(const FCrowdyModList& List);
	void ReadClients();
	void ReadClientRows(const FCrowdyCppJsonResult& Result);
	void Finish(ECrowdyModResult Result, const FString& Message);
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
	// A run stops at its next step and answers nobody; its app is free at once.
	const TArray<TSharedPtr<FRun>> Ending = MoveTemp(Runs);
	Runs.Reset();
	for (const TSharedPtr<FRun>& Run : Ending)
	{
		Run->End();
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

void UCrowdyMods::SetStudioProjectsForTest(UCrowdyStudioProjects* Projects)
{
	ProjectsForTest = Projects;
}
#endif

UCrowdyStudioProjects* UCrowdyMods::ResolveStudioProjects() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (UCrowdyStudioProjects* Override = ProjectsForTest.Get())
	{
		return Override;
	}
#endif
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UCrowdyStudioProjects>() : nullptr;
}

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

int64 UCrowdyMods::GetUserId() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (ClientForTest.IsValid())
	{
		return UserIdForTest;
	}
#endif
	const UGameInstance* GameInstance = GetGameInstance();
	const UCrowdyGameSession* Session = GameInstance ? GameInstance->GetSubsystem<UCrowdyGameSession>() : nullptr;
	return Session ? Session->GetUserID() : 0;
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
	const int64 UserId = GetUserId();
	Variables->SetStringField(TEXT("appId"), BigInt(AppId));

	TWeakObjectPtr<UCrowdyMods> WeakThis(this);
	RunOpDeferred(*Client, ECrowdyCppApiDomain::Exec, Operation, Variables, LiveSessionToken,
		[WeakThis, AppId, UserId, OnAnswer = MoveTemp(OnAnswer), OnFailed = MoveTemp(OnFailed)](FCrowdyCppJsonResult Result)
		{
			const UCrowdyMods* Self = WeakThis.Get();
			if (!Self || !Self->IsCaller(AppId, UserId))
			{
				OnFailed(ECrowdyModResult::Failed, ModsCallerChanged, FString());
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
	StartBuild(TEXT("Build Mod"), TEXT("ExecModBuild"), TEXT("execModBuild"), CrateName, Files, MoveTemp(OnProgress), MoveTemp(OnDone));
}

void UCrowdyMods::BuildClientMod(const FString& CrateName, const TArray<FCrowdyModFile>& Files, TFunction<void(const FCrowdyModBuild&)> OnProgress,
	TFunction<void(const FCrowdyModBuild&)> OnDone)
{
	StartBuild(TEXT("Build Client Mod"), TEXT("ExecModClientBuild"), TEXT("execModClientBuild"), CrateName, Files, MoveTemp(OnProgress), MoveTemp(OnDone));
}

void UCrowdyMods::StartBuild(const TCHAR* Call, const TCHAR* Operation, const TCHAR* Field, const FString& CrateName, const TArray<FCrowdyModFile>& Files,
	TFunction<void(const FCrowdyModBuild&)> OnProgress, TFunction<void(const FCrowdyModBuild&)> OnDone)
{
	const TSharedRef<FBuild> Build = MakeShared<FBuild>();
	Build->Call = Call;
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
	Build->AppId = GetAppId();
	Build->UserId = GetUserId();
	TWeakObjectPtr<UCrowdyMods> WeakThis(this);
	SendModOp(Operation, Variables,
		[WeakThis, Build, Field](const FCrowdyCppJsonResult& Result)
		{
			if (UCrowdyMods* Self = WeakThis.Get())
			{
				Self->ReadBuild(Build, Result, Field);
			}
		},
		[Build](ECrowdyModResult Result, const FString& Message, const FString& Code) { Build->Finish(Result, Message, Code); });
}

void UCrowdyMods::PollBuild(const TSharedRef<FBuild>& Build)
{
	// A status read is the player's own, so it is never sent for another player or app.
	if (!IsCaller(Build->AppId, Build->UserId))
	{
		Build->Finish(ECrowdyModResult::Failed, ModsCallerChanged);
		return;
	}
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
			const bool bSameCaller = Self && Self->IsCaller(Build->AppId, Build->UserId);
			if (bSameCaller && Result == ECrowdyModResult::Failed && Self->Now() - Build->StartedAt < BuildLimitSeconds)
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

void UCrowdyMods::DeployClientMod(int64 GridId, const FString& Name, const FString& BuildId, TFunction<void(const FCrowdyModClientInfo&)> OnDone)
{
	const TSharedPtr<FJsonObject> Variables = ModsGridVariables(GridId, Name);
	Variables->SetStringField(TEXT("buildId"), BuildId);
	SendModOp(TEXT("ExecModClientDeploy"), Variables,
		[GridId, Name, OnDone](const FCrowdyCppJsonResult& Result)
		{
			FCrowdyModClientInfo Info;
			const bool bRead = ModsReadClient(ModsAnswerObject(Result, TEXT("execModClientDeploy")), Info.Client);
			if (!bRead || Info.Client.GridId != GridId || !Info.Client.Name.Equals(Name, ESearchCase::CaseSensitive))
			{
				ModsFinish(TEXT("Deploy Client Mod"), OnDone, FCrowdyModClientInfo(), ECrowdyModResult::BadReply, TEXT("The answer holds no well-formed CLIENT half of that grid and name."));
				return;
			}
			ModsFinish(TEXT("Deploy Client Mod"), OnDone, Info, ECrowdyModResult::Success, FString());
		},
		ModsFailWith(TEXT("Deploy Client Mod"), OnDone));
}

void UCrowdyMods::RunProject(int64 GridId, const FString& ProjectId, const FString& ExpectedRevision, ECrowdyStudioRunMode Mode,
	TFunction<void(const FCrowdyModRun&)> OnDone)
{
	RunProject(GridId, ProjectId, ExpectedRevision, Mode, FString(), FString(), MoveTemp(OnDone));
}

void UCrowdyMods::RunProject(int64 GridId, const FString& ProjectId, const FString& ExpectedRevision, ECrowdyStudioRunMode Mode,
	const FString& ExpectedServerModule, const FString& ExpectedClientModule, TFunction<void(const FCrowdyModRun&)> OnDone)
{
	FCrowdyModRun Answer;
	Answer.Mode = Mode;
	Answer.GridId = GridId;
	Answer.ProjectId = ProjectId;
	const int64 AppId = GetAppId();
	FModsRunRegistry& Registry = ModsRuns();
	UCrowdyStudioProjects* Projects = ResolveStudioProjects();
	if (ExpectedRevision.IsEmpty())
	{
		ModsDeferAnswer(LiveSessionToken, ModsRunCall, MoveTemp(OnDone), MoveTemp(Answer), ECrowdyModResult::Refused, TEXT("A run names the revision it builds, so nothing was sent."));
		return;
	}
	if (GridId <= 0)
	{
		ModsDeferAnswer(LiveSessionToken, ModsRunCall, MoveTemp(OnDone), MoveTemp(Answer), ECrowdyModResult::Refused, TEXT("A run names a grid id above 0, so nothing was sent."));
		return;
	}
	if (!HasAppToken())
	{
		ModsDeferAnswer(LiveSessionToken, ModsRunCall, MoveTemp(OnDone), MoveTemp(Answer), ECrowdyModResult::NotSignedIn, ModsNotSignedIn);
		return;
	}
	if (Registry.InFlight.Contains(AppId))
	{
		ModsDeferAnswer(LiveSessionToken, ModsRunCall, MoveTemp(OnDone), MoveTemp(Answer), ECrowdyModResult::Busy, TEXT("Another run for this app is still going, so nothing was sent."));
		return;
	}
	if (!Projects)
	{
		ModsDeferAnswer(LiveSessionToken, ModsRunCall, MoveTemp(OnDone), MoveTemp(Answer), ECrowdyModResult::Failed, ModsNoProjects);
		return;
	}

	const TSharedRef<FRun> Run = MakeShared<FRun>();
	Run->Mods = this;
	Run->OnDone = MoveTemp(OnDone);
	Run->Answer = MoveTemp(Answer);
	Run->ExpectedRevision = ExpectedRevision;
	Run->ExpectedServerModule = ExpectedServerModule;
	Run->ExpectedClientModule = ExpectedClientModule;
	Run->AppId = AppId;
	Run->UserId = GetUserId();
	Run->Serial = ++Registry.NextSerial;
	FModsRunSlot& Slot = Registry.InFlight.Add(AppId);
	Slot.Serial = Run->Serial;
	Slot.GridId = GridId;
	Slot.ProjectId = ProjectId;
	Runs.Add(Run);
	Projects->GetProject(ProjectId, [Run](const FCrowdyStudioProjectInfo& Info) { Run->ReadProject(Info); });
}

UCrowdyMods* UCrowdyMods::FRun::Alive()
{
	if (bFinished)
	{
		return nullptr;
	}
	UCrowdyMods* Self = Mods.Get();
	if (!Self || !Self->LiveSessionToken.IsValid())
	{
		End();
		return nullptr;
	}
	// Every step is sent as the player and app the run started for, or not at all.
	if (!Self->IsCaller(AppId, UserId))
	{
		Finish(ECrowdyModResult::Failed, ModsRunCallerChanged);
		return nullptr;
	}
	return Self;
}

void UCrowdyMods::FRun::SetPhase(ECrowdyStudioTarget Target, ECrowdyModRunPhase Phase, const FString& Message)
{
	FCrowdyModRunTarget* Found = Half(Target);
	if (!Found)
	{
		return;
	}
	Found->Phase = Phase;
	Found->Message = Message;
	Publish();
}

void UCrowdyMods::FRun::Publish()
{
	FModsRunSlot* Slot = ModsRuns().InFlight.Find(AppId);
	if (Slot && Slot->Serial == Serial)
	{
		Slot->Targets = Answer.Targets;
	}
}

void UCrowdyMods::FRun::AddLog(ECrowdyStudioTarget Target, const FString& Log)
{
	if (Log.IsEmpty())
	{
		return;
	}
	if (!Answer.BuildLog.IsEmpty())
	{
		Answer.BuildLog.AppendChar(TEXT('\n'));
	}
	Answer.BuildLog.Append(Log);
	const ECrowdyCppStudioTarget Default = Target == ECrowdyStudioTarget::Client ? ECrowdyCppStudioTarget::Client : ECrowdyCppStudioTarget::Server;
	for (const FCrowdyCppStudioDiagnostic& Parsed : CrowdyCppStudio::ParseRustcDiagnostics(Log, Default))
	{
		Answer.Diagnostics.Add(ModsDiagnostic(Parsed));
	}
}

// The platform keeps no Studio revision, so the run remembers which revision it deployed under which digest.
void UCrowdyMods::FRun::Record(ECrowdyStudioTarget Target, const FString& Module, const FString& Digest)
{
	FModsDeployRecord& Deployed = ModsRuns().Deployed.FindOrAdd(ModsDeployKey(AppId, Answer.GridId, Answer.ProjectId, Target, Module));
	Deployed.Revision = ExpectedRevision;
	Deployed.Digest = Digest;
	FCrowdyModRunTarget* Found = Half(Target);
	if (Found)
	{
		Found->Digest = Digest;
		Found->RunningRevision = ExpectedRevision;
	}
}

void UCrowdyMods::FRun::ReadProject(const FCrowdyStudioProjectInfo& Info)
{
	if (!Alive())
	{
		return;
	}
	if (Info.Result != ECrowdyStudioResult::Success)
	{
		Finish(ModsFromStudioResult(Info.Result), Info.Message);
		return;
	}
	const FCrowdyStudioProject& Project = Info.Project;
	if (!Project.Revision.Equals(ExpectedRevision, ESearchCase::CaseSensitive))
	{
		Finish(ECrowdyModResult::RevisionChanged, FString::Printf(TEXT("The project's saved revision is %s, not %s, so nothing was built."), *Project.Revision, *ExpectedRevision));
		return;
	}
	const FString Renamed = ModsCheckExpectedModules(Project, ExpectedServerModule, ExpectedClientModule);
	if (!Renamed.IsEmpty())
	{
		Finish(ECrowdyModResult::RevisionChanged, Renamed);
		return;
	}
	const FString Problem = ModsCheckModules(Project.Kind, Project.ServerModule, Project.ClientModule);
	if (!Problem.IsEmpty())
	{
		Finish(ECrowdyModResult::Refused, Problem);
		return;
	}
	Kind = Project.Kind;
	ServerModule = Project.ServerModule;
	ClientModule = Project.ClientModule;
	Answer.Revision = ExpectedRevision;
	if (ModsKindHasServer(Kind))
	{
		ModsAddHalf(Answer.Targets, ECrowdyStudioTarget::Server, ServerModule, Project.Revision);
		ServerFiles = ModsTargetFiles(Info.Files, ECrowdyStudioTarget::Server);
	}
	if (!ModsKindHasClient(Kind))
	{
		BuildServer();
		return;
	}
	ModsAddHalf(Answer.Targets, ECrowdyStudioTarget::Client, ClientModule, Project.Revision);
	ClientFiles = ModsTargetFiles(Info.Files, ECrowdyStudioTarget::Client);
	BuildClient();
}

void UCrowdyMods::FRun::BuildClient()
{
	UCrowdyMods* Self = Alive();
	if (!Self)
	{
		return;
	}
	SetPhase(ECrowdyStudioTarget::Client, ECrowdyModRunPhase::Building, FString());
	Self->BuildClientMod(ModsCrateName(ClientModule), ClientFiles, nullptr, [This = AsShared()](const FCrowdyModBuild& Build) { This->ClientBuilt(Build); });
}

void UCrowdyMods::FRun::ClientBuilt(const FCrowdyModBuild& Build)
{
	if (!Alive())
	{
		return;
	}
	AddLog(ECrowdyStudioTarget::Client, Build.Log);
	if (Build.Result != ECrowdyModResult::Success)
	{
		Fail(ECrowdyStudioTarget::Client, ECrowdyModRunPhase::BuildFailed, Build.Result, Build.Message);
		return;
	}
	if (Build.Status != ECrowdyModBuildStatus::Succeeded)
	{
		SetPhase(ECrowdyStudioTarget::Server, ECrowdyModRunPhase::NotStarted, TEXT("Not built: the CLIENT half did not compile."));
		Fail(ECrowdyStudioTarget::Client, ECrowdyModRunPhase::BuildFailed, ECrowdyModResult::Success, TEXT("The CLIENT half did not compile, so nothing was published."));
		return;
	}
	ClientBuildId = Build.BuildId;
	SetPhase(ECrowdyStudioTarget::Client, ECrowdyModRunPhase::NotStarted, TEXT("Built; it is attached once its mod is switched on."));
	if (ModsKindHasServer(Kind))
	{
		BuildServer();
		return;
	}
	FindRide();
}

void UCrowdyMods::FRun::BuildServer()
{
	UCrowdyMods* Self = Alive();
	if (!Self)
	{
		return;
	}
	SetPhase(ECrowdyStudioTarget::Server, ECrowdyModRunPhase::Building, FString());
	Self->BuildMod(ModsCrateName(ServerModule), ServerFiles, nullptr, [This = AsShared()](const FCrowdyModBuild& Build) { This->ServerBuilt(Build); });
}

void UCrowdyMods::FRun::ServerBuilt(const FCrowdyModBuild& Build)
{
	UCrowdyMods* Self = Alive();
	if (!Self)
	{
		return;
	}
	AddLog(ECrowdyStudioTarget::Server, Build.Log);
	if (Build.Result != ECrowdyModResult::Success)
	{
		Fail(ECrowdyStudioTarget::Server, ECrowdyModRunPhase::BuildFailed, Build.Result, Build.Message);
		return;
	}
	if (Build.Status != ECrowdyModBuildStatus::Succeeded)
	{
		SetPhase(ECrowdyStudioTarget::Client, ECrowdyModRunPhase::NotStarted, TEXT("Not attached: the SERVER half did not compile."));
		Fail(ECrowdyStudioTarget::Server, ECrowdyModRunPhase::BuildFailed, ECrowdyModResult::Success, TEXT("The SERVER half did not compile, so it was not deployed."));
		return;
	}
	SetPhase(ECrowdyStudioTarget::Server, ECrowdyModRunPhase::Deploying, FString());
	Self->DeployMod(Answer.GridId, ServerModule, Build.BuildId, [This = AsShared()](const FCrowdyModInfo& Info) { This->ServerDeployed(Info); });
}

void UCrowdyMods::FRun::ServerDeployed(const FCrowdyModInfo& Info)
{
	UCrowdyMods* Self = Alive();
	if (!Self)
	{
		return;
	}
	if (Info.Result != ECrowdyModResult::Success)
	{
		Fail(ECrowdyStudioTarget::Server, ECrowdyModRunPhase::DeployFailed, Info.Result, Info.Message);
		return;
	}
	Record(ECrowdyStudioTarget::Server, ServerModule, Info.Mod.Digest);
	Self->SetModEnabled(Answer.GridId, ServerModule, true, [This = AsShared()](const FCrowdyModInfo& Switched) { This->ServerSwitched(Switched); });
}

void UCrowdyMods::FRun::ServerSwitched(const FCrowdyModInfo& Info)
{
	if (!Alive())
	{
		return;
	}
	if (Info.Result != ECrowdyModResult::Success)
	{
		Fail(ECrowdyStudioTarget::Server, ECrowdyModRunPhase::DeployFailed, Info.Result, FString::Printf(TEXT("Deployed, but not switched on: %s"), *Info.Message));
		return;
	}
	FCrowdyModRunTarget* Server = Half(ECrowdyStudioTarget::Server);
	if (Server)
	{
		ModsApplyServerRow(*Server, Info.Mod);
		Publish();
	}
	if (ModsKindHasClient(Kind))
	{
		AttachClient();
		return;
	}
	Finish(ECrowdyModResult::Success, FString());
}

// A CLIENT-only project's CLIENT half rides the player's mod named for its CLIENT module on this grid.
void UCrowdyMods::FRun::FindRide()
{
	UCrowdyMods* Self = Alive();
	if (!Self)
	{
		return;
	}
	SetPhase(ECrowdyStudioTarget::Client, ECrowdyModRunPhase::Deploying, TEXT("Finding the mod it rides."));
	Self->ListMyMods([This = AsShared()](const FCrowdyModList& List) { This->RideListed(List); });
}

void UCrowdyMods::FRun::RideListed(const FCrowdyModList& List)
{
	UCrowdyMods* Self = Alive();
	if (!Self)
	{
		return;
	}
	if (List.Result != ECrowdyModResult::Success)
	{
		Fail(ECrowdyStudioTarget::Client, ECrowdyModRunPhase::DeployFailed, List.Result, List.Message);
		return;
	}
	const FCrowdyMod* Ride = List.Mods.FindByPredicate([this](const FCrowdyMod& Mod) { return ModsIsAsked(Mod, Answer.GridId, ClientModule); });
	if (Ride && Ride->bEnabled)
	{
		AttachClient();
		return;
	}
	// Switching a mod off was its owner's choice, so a run never switches an existing one back on.
	if (Ride)
	{
		Fail(ECrowdyStudioTarget::Client, ECrowdyModRunPhase::DeployFailed, ECrowdyModResult::Refused,
			FString::Printf(TEXT("%s is switched off; switch it on yourself to run this project."), *ClientModule));
		return;
	}
	SetPhase(ECrowdyStudioTarget::Client, ECrowdyModRunPhase::Deploying, TEXT("Deploying the platform's starter as the mod it rides."));
	Self->GetStarter([This = AsShared()](const FCrowdyModStarter& Starter) { This->StarterRead(Starter); });
}

void UCrowdyMods::FRun::StarterRead(const FCrowdyModStarter& Starter)
{
	UCrowdyMods* Self = Alive();
	if (!Self)
	{
		return;
	}
	if (Starter.Result != ECrowdyModResult::Success)
	{
		Fail(ECrowdyStudioTarget::Client, ECrowdyModRunPhase::DeployFailed, Starter.Result, Starter.Message);
		return;
	}
	Self->BuildMod(ModsCrateName(ClientModule), Starter.Files, nullptr, [This = AsShared()](const FCrowdyModBuild& Build) { This->StarterBuilt(Build); });
}

void UCrowdyMods::FRun::StarterBuilt(const FCrowdyModBuild& Build)
{
	UCrowdyMods* Self = Alive();
	if (!Self)
	{
		return;
	}
	if (Build.Result != ECrowdyModResult::Success)
	{
		Fail(ECrowdyStudioTarget::Client, ECrowdyModRunPhase::DeployFailed, Build.Result, Build.Message);
		return;
	}
	if (Build.Status != ECrowdyModBuildStatus::Succeeded)
	{
		Fail(ECrowdyStudioTarget::Client, ECrowdyModRunPhase::DeployFailed, ECrowdyModResult::Failed, TEXT("The platform's starter did not build, so the CLIENT half has no mod to ride."));
		return;
	}
	Self->DeployMod(Answer.GridId, ClientModule, Build.BuildId, [This = AsShared()](const FCrowdyModInfo& Info) { This->StarterDeployed(Info); });
}

void UCrowdyMods::FRun::StarterDeployed(const FCrowdyModInfo& Info)
{
	if (!Alive())
	{
		return;
	}
	if (Info.Result != ECrowdyModResult::Success)
	{
		Fail(ECrowdyStudioTarget::Client, ECrowdyModRunPhase::DeployFailed, Info.Result, Info.Message);
		return;
	}
	SwitchRideOn();
}

// Only a switched-on mod's CLIENT half is served.
void UCrowdyMods::FRun::SwitchRideOn()
{
	UCrowdyMods* Self = Alive();
	if (!Self)
	{
		return;
	}
	Self->SetModEnabled(Answer.GridId, ClientModule, true, [This = AsShared()](const FCrowdyModInfo& Info) { This->RideSwitched(Info); });
}

void UCrowdyMods::FRun::RideSwitched(const FCrowdyModInfo& Info)
{
	if (!Alive())
	{
		return;
	}
	if (Info.Result != ECrowdyModResult::Success)
	{
		Fail(ECrowdyStudioTarget::Client, ECrowdyModRunPhase::DeployFailed, Info.Result, FString::Printf(TEXT("The mod it rides was not switched on: %s"), *Info.Message));
		return;
	}
	AttachClient();
}

void UCrowdyMods::FRun::AttachClient()
{
	UCrowdyMods* Self = Alive();
	if (!Self)
	{
		return;
	}
	const FString& Ride = ModsKindHasServer(Kind) ? ServerModule : ClientModule;
	SetPhase(ECrowdyStudioTarget::Client, ECrowdyModRunPhase::Deploying, FString());
	Self->DeployClientMod(Answer.GridId, Ride, ClientBuildId, [This = AsShared()](const FCrowdyModClientInfo& Info) { This->ClientAttached(Info); });
}

// The platform serves a CLIENT half only to a player who consented to it, its author included.
void UCrowdyMods::FRun::ClientAttached(const FCrowdyModClientInfo& Info)
{
	UCrowdyMods* Self = Alive();
	if (!Self)
	{
		return;
	}
	if (Info.Result != ECrowdyModResult::Success)
	{
		Fail(ECrowdyStudioTarget::Client, ECrowdyModRunPhase::DeployFailed, Info.Result, Info.Message);
		return;
	}
	FCrowdyModRunTarget* Client = Half(ECrowdyStudioTarget::Client);
	if (Client)
	{
		Client->ModId = Info.Client.ModId;
		Client->Version = Info.Client.ClientVersion;
	}
	Record(ECrowdyStudioTarget::Client, ClientModule, Info.Client.Digest);
	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("modId"), Info.Client.ModId);
	Variables->SetStringField(TEXT("capabilityHash"), Info.Client.CapabilityHash);
	Self->SendModOp(TEXT("ExecConsentClientMod"), Variables,
		[This = AsShared()](const FCrowdyCppJsonResult& Result) { This->Consented(Result); },
		[This = AsShared()](ECrowdyModResult Result, const FString& Message, const FString&) { This->ConsentRefused(Result, Message); });
}

void UCrowdyMods::FRun::Consented(const FCrowdyCppJsonResult& Result)
{
	if (!Alive())
	{
		return;
	}
	// A mutation's boolean is not its verdict: a refusal arrives as an error.
	if (!Result.Data->HasField(TEXT("execConsentClientMod")))
	{
		Fail(ECrowdyStudioTarget::Client, ECrowdyModRunPhase::DeployFailed, ECrowdyModResult::BadReply, TEXT("Attached, but the consent's answer holds nothing."));
		return;
	}
	SetPhase(ECrowdyStudioTarget::Client, ECrowdyModRunPhase::Deployed, TEXT("Attached and consented to; the grid serves it to the players standing in it."));
	Finish(ECrowdyModResult::Success, FString());
}

void UCrowdyMods::FRun::ConsentRefused(ECrowdyModResult Result, const FString& Message)
{
	if (!Alive())
	{
		return;
	}
	Fail(ECrowdyStudioTarget::Client, ECrowdyModRunPhase::DeployFailed, Result, FString::Printf(TEXT("Attached, but its author's consent was refused: %s"), *Message));
}

void UCrowdyMods::FRun::Fail(ECrowdyStudioTarget Target, ECrowdyModRunPhase Phase, ECrowdyModResult Result, const FString& Message)
{
	SetPhase(Target, Phase, Message);
	Finish(Result, Message);
}

void UCrowdyMods::FRun::Finish(ECrowdyModResult Result, const FString& Message)
{
	if (bFinished)
	{
		return;
	}
	End();
	ModsFinish(ModsRunCall, OnDone, Answer, Result, Message);
}

// Frees the app before any answer, so OnDone may start the next run.
void UCrowdyMods::FRun::End()
{
	bFinished = true;
	FModsRunRegistry& Registry = ModsRuns();
	const FModsRunSlot* Slot = Registry.InFlight.Find(AppId);
	if (Slot && Slot->Serial == Serial)
	{
		Registry.InFlight.Remove(AppId);
	}
	if (UCrowdyMods* Self = Mods.Get())
	{
		Self->Runs.RemoveAll([this](const TSharedPtr<FRun>& Run) { return Run.Get() == this; });
	}
}

void UCrowdyMods::GetProjectStatus(int64 GridId, const FString& ProjectId, TFunction<void(const FCrowdyModProjectStatus&)> OnDone)
{
	FCrowdyModProjectStatus Answer;
	Answer.GridId = GridId;
	Answer.ProjectId = ProjectId;
	UCrowdyStudioProjects* Projects = ResolveStudioProjects();
	if (GridId <= 0)
	{
		ModsDeferAnswer(LiveSessionToken, ModsStatusCall, MoveTemp(OnDone), MoveTemp(Answer), ECrowdyModResult::Refused, TEXT("A status read names a grid id above 0, so nothing was sent."));
		return;
	}
	if (!Projects)
	{
		ModsDeferAnswer(LiveSessionToken, ModsStatusCall, MoveTemp(OnDone), MoveTemp(Answer), ECrowdyModResult::Failed, ModsNoProjects);
		return;
	}
	const TSharedRef<FStatusRead> Read = MakeShared<FStatusRead>();
	Read->Mods = this;
	Read->OnDone = MoveTemp(OnDone);
	Read->Answer = MoveTemp(Answer);
	Read->AppId = GetAppId();
	Read->UserId = GetUserId();
	Projects->GetProject(ProjectId, [Read](const FCrowdyStudioProjectInfo& Info) { Read->ReadProject(Info); });
}

UCrowdyMods* UCrowdyMods::FStatusRead::Alive()
{
	UCrowdyMods* Self = Mods.Get();
	if (bFinished || !Self || !Self->LiveSessionToken.IsValid())
	{
		return nullptr;
	}
	if (!Self->IsCaller(AppId, UserId))
	{
		Finish(ECrowdyModResult::Failed, TEXT("The player or app changed while the status was read, so nothing more was sent."));
		return nullptr;
	}
	return Self;
}

void UCrowdyMods::FStatusRead::ReadProject(const FCrowdyStudioProjectInfo& Info)
{
	UCrowdyMods* Self = Alive();
	if (!Self)
	{
		return;
	}
	if (Info.Result != ECrowdyStudioResult::Success)
	{
		Finish(ModsFromStudioResult(Info.Result), Info.Message);
		return;
	}
	Kind = Info.Project.Kind;
	ServerModule = Info.Project.ServerModule;
	ClientModule = Info.Project.ClientModule;
	if (ModsKindHasServer(Kind))
	{
		ModsAddHalf(Answer.Targets, ECrowdyStudioTarget::Server, ServerModule, Info.Project.Revision);
	}
	if (ModsKindHasClient(Kind))
	{
		ModsAddHalf(Answer.Targets, ECrowdyStudioTarget::Client, ClientModule, Info.Project.Revision);
	}
	if (!ModsKindHasServer(Kind))
	{
		ReadClients();
		return;
	}
	TSharedRef<FStatusRead> This = AsShared();
	Self->ListMods(Answer.GridId, [This](const FCrowdyModList& List) { This->ReadServer(List); });
}

void UCrowdyMods::FStatusRead::ReadServer(const FCrowdyModList& List)
{
	if (!Alive())
	{
		return;
	}
	if (List.Result != ECrowdyModResult::Success)
	{
		Finish(List.Result, List.Message);
		return;
	}
	FCrowdyModRunTarget* Server = Half(ECrowdyStudioTarget::Server);
	const FCrowdyMod* Row = List.Mods.FindByPredicate([this](const FCrowdyMod& Mod) { return ModsIsAsked(Mod, Answer.GridId, ServerModule); });
	if (Server && Row)
	{
		ModsApplyServerRow(*Server, *Row);
		Server->RunningRevision = ModsRunningRevision(ModsDeployKey(AppId, Answer.GridId, Answer.ProjectId, ECrowdyStudioTarget::Server, ServerModule), Row->Digest);
	}
	if (Server && !Row)
	{
		Server->Message = TEXT("Not deployed on this grid.");
	}
	if (!ModsKindHasClient(Kind))
	{
		Finish(ECrowdyModResult::Success, FString());
		return;
	}
	ReadClients();
}

void UCrowdyMods::FStatusRead::ReadClients()
{
	UCrowdyMods* Self = Alive();
	if (!Self)
	{
		return;
	}
	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("gridId"), BigInt(Answer.GridId));
	Self->SendModOp(TEXT("ExecGridClientMods"), Variables,
		[This = AsShared()](const FCrowdyCppJsonResult& Result) { This->ReadClientRows(Result); },
		[This = AsShared()](ECrowdyModResult Result, const FString& Message, const FString&) { This->Finish(Result, Message); });
}

void UCrowdyMods::FStatusRead::ReadClientRows(const FCrowdyCppJsonResult& Result)
{
	UCrowdyMods* Self = Alive();
	if (!Self)
	{
		return;
	}
	const TArray<TSharedPtr<FJsonValue>>* Rows = ModsAnswerArray(Result, TEXT("execGridClientMods"));
	TArray<FCrowdyModClient> Served;
	bool bRead = Rows != nullptr;
	for (int32 Index = 0; bRead && Index < Rows->Num(); ++Index)
	{
		bRead = ModsReadServedClient((*Rows)[Index], Answer.GridId, Served.AddDefaulted_GetRef());
	}
	if (!bRead)
	{
		Finish(ECrowdyModResult::BadReply, TEXT("The answer holds no well-formed list of the grid's CLIENT halves."));
		return;
	}
	// A CLIENT half is served under the name of the mod it rides.
	const FString& Ride = ModsKindHasServer(Kind) ? ServerModule : ClientModule;
	const FCrowdyModClient* Row = Served.FindByPredicate([&Ride](const FCrowdyModClient& Entry) { return Entry.Name.Equals(Ride, ESearchCase::CaseSensitive); });
	FCrowdyModRunTarget* Client = Half(ECrowdyStudioTarget::Client);
	if (Client && !Row)
	{
		Client->Message = TEXT("Not served on this grid: not attached, or the mod it rides is switched off or blocked.");
	}
	if (Client && Row)
	{
		const bool bRuns = ModsClientRunsHere(Self->GetGameInstance(), *Row);
		Client->ModId = Row->ModId;
		Client->Digest = Row->Digest;
		Client->Version = Row->ClientVersion;
		Client->Phase = bRuns ? ECrowdyModRunPhase::Running : ECrowdyModRunPhase::Deployed;
		Client->Message = bRuns ? FString() : FString(TEXT("Served on this grid; not running on this computer."));
		Client->RunningRevision = ModsRunningRevision(ModsDeployKey(AppId, Answer.GridId, Answer.ProjectId, ECrowdyStudioTarget::Client, ClientModule), Row->Digest);
	}
	Finish(ECrowdyModResult::Success, FString());
}

void UCrowdyMods::FStatusRead::Finish(ECrowdyModResult Result, const FString& Message)
{
	if (bFinished)
	{
		return;
	}
	bFinished = true;
	if (Result != ECrowdyModResult::Success)
	{
		Answer.Targets.Reset();
	}
	const FModsRunSlot* Slot = ModsRuns().InFlight.Find(AppId);
	Answer.bRunInFlight = Slot && Slot->GridId == Answer.GridId && Slot->ProjectId.Equals(Answer.ProjectId, ESearchCase::IgnoreCase);
	if (Answer.bRunInFlight)
	{
		ModsOverlayInFlight(Answer.Targets, Slot->Targets);
	}
	ModsFinish(ModsStatusCall, OnDone, Answer, Result, Message);
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
