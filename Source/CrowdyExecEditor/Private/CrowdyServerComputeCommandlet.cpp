#include "CrowdyServerComputeCommandlet.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "CoreGlobals.h"
#include "CrowdyExecBuildAndDeploy.h"
#include "CrowdyExecCodegen.h"
#include "CrowdyExecDeploy.h"
#include "CrowdyExecDeveloperApi.h"
#include "CrowdyExecDeveloperCall.h"
#include "CrowdyExecLog.h"
#include "CrowdyExecRevisions.h"
#include "CrowdyGameKitStarters.h"
#include "CrowdyServerComputeSettings.h"
#include "CrowdyServerObjectDefinition.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace CrowdyServerComputeCommandletDetail
{
	namespace Dev = CrowdyExecDeveloper;

	const TCHAR* const Usage = TEXT("-run=CrowdyServerCompute -op=status | versions | changes | deploy -yes [-batch=N] [-rebuild] | starters -yes [-batch=N] | activate -version=N -yes | disable -type=T -yes")
		TEXT(" | enable -type=T -yes | restart -type=T -yes | instances | call -type=T -fn=F [-key=K] [-inputs=<json or @file>] -yes | logs [-type=T] [-flow=F] [-limit=N] | stats [-type=T]")
		TEXT(" | grant -player=<user id> -bundle=<json or @file> [-xp=N] [-ref=R] -yes | deposit -player=<user id> -bundle=<json or @file> [-ref=R] -yes (in-game soft currency, never real money)")
		TEXT(" | addstarter -starter=world|matchmaker|session|leaderboard|player|market|npcs|minigame|tactics|director|instance|territory|zone|deck|arena|race|ball [-config=<json or @file>]")
		TEXT(" | removestarter -starter=world|matchmaker|session|leaderboard|player|market|npcs|minigame|tactics|director|instance|territory|zone|deck|arena|race|ball -yes")
		TEXT(" | batch -ops=<file> (one op's arguments per line, # comments; add -batchcontinue to a line to go on after its failure), and -out=<file> for a JSON result");

	const TCHAR* const ConfirmError = TEXT("This changes the app's live server code or what it holds; pass -yes to confirm.");

	constexpr double ShortTimeoutSeconds = 60.0;
	constexpr double BuildTimeoutSeconds = 15.0 * 60.0;
	// Enough for the platform's busy waits (59 seconds in all) and the round trips between them.
	constexpr double ChangeTimeoutSeconds = 180.0;

	struct FRun
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		TSharedPtr<Dev::FClient> Client;
		/** The project as a deploy sends it, recorded beside each type's crate once the deploy succeeds. */
		CrowdyExecDeploy::FProjectDeploy Sending;
		TMap<FString, FString> CrateDirectories;
		FString Params;
		FString Error;
		FString ErrorCode;
		double Deadline = 0.0;
		bool bChangeSent = false;
		bool bRecord = false;
		bool bDone = false;
		bool bOk = false;
	};

	using FRunRef = TSharedRef<FRun>;

	void Step(const FString& Line)
	{
		UE_LOG(LogCrowdyExec, Display, TEXT("Server Compute: %s"), *Line);
	}

	void Succeed(const FRunRef& Run)
	{
		Run->bOk = true;
		Run->bDone = true;
	}

	void Fail(const FRunRef& Run, const FString& Message, const FString& Code = FString())
	{
		Run->Error = Message;
		Run->ErrorCode = Code;
		Run->bDone = true;
	}

	void FailWith(const FRunRef& Run, const Dev::FError& Error)
	{
		Fail(Run, Error.Message, Error.Code);
	}

	/** Marks a change to the app as sent, with at least ChangeTimeoutSeconds left for its answer. */
	void SendingChange(FRun& Run)
	{
		Run.bChangeSent = true;
		Run.Deadline = FMath::Max(Run.Deadline, FPlatformTime::Seconds() + ChangeTimeoutSeconds);
	}

	TSharedPtr<FJsonValue> NumberOrNull(const TOptional<double>& Value)
	{
		if (!Value.IsSet())
		{
			return MakeShared<FJsonValueNull>();
		}
		return MakeShared<FJsonValueNumber>(Value.GetValue());
	}

	TOptional<double> IntOrUnset(const TOptional<int32>& Value)
	{
		return Value.IsSet() ? TOptional<double>(Value.GetValue()) : TOptional<double>();
	}

	TSharedPtr<FJsonValue> StringOrNull(const FString& Value)
	{
		if (Value.IsEmpty())
		{
			return MakeShared<FJsonValueNull>();
		}
		return MakeShared<FJsonValueString>(Value);
	}

	TArray<TSharedPtr<FJsonValue>> StringValues(const TArray<FString>& Items)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& Item : Items)
		{
			Values.Add(MakeShared<FJsonValueString>(Item));
		}
		return Values;
	}

	const TCHAR* LevelName(int32 Level)
	{
		switch (Level)
		{
		case 0: return TEXT("error");
		case 1: return TEXT("warning");
		case 2: return TEXT("info");
		default: return TEXT("debug");
		}
	}

	const TCHAR* CrateStateName(CrowdyExecDeploy::ECrateState State)
	{
		switch (State)
		{
		case CrowdyExecDeploy::ECrateState::UpToDate: return TEXT("UpToDate");
		case CrowdyExecDeploy::ECrateState::NotGenerated: return TEXT("NotGenerated");
		case CrowdyExecDeploy::ECrateState::OutOfDate: return TEXT("OutOfDate");
		default: return TEXT("Invalid");
		}
	}

	FString StatusLine(const Dev::FAppStatus& Status)
	{
		const FString Active = Status.ActiveVersion.IsSet() ? FString::FromInt(Status.ActiveVersion.GetValue()) : FString(TEXT("none"));
		const FString Off = Status.DisabledTypes.IsEmpty() ? FString(TEXT("none")) : FString::Join(Status.DisabledTypes, TEXT(", "));
		const FString Paused = Status.BudgetPauseReason.IsEmpty() ? FString(TEXT("paused")) : FString::Printf(TEXT("paused (%s)"), *Status.BudgetPauseReason);
		const auto Count = [](const TOptional<int32>& Value) { return Value.IsSet() ? FString::FromInt(Value.GetValue()) : FString(TEXT("?")); };
		return FString::Printf(TEXT("active version %s; server code %s; types switched off: %s; budget %s; instances %s of %s, %s of %s MB reserved"), *Active,
			Status.bDisabled ? TEXT("switched off") : TEXT("on"), *Off, Status.bBudgetPaused ? *Paused : TEXT("not paused"), *Count(Status.Instances),
			*Count(Status.InstanceLimit), *Count(Status.ReservedMb), *Count(Status.MaxReservedMb));
	}

	void SetStatus(FJsonObject& Out, const Dev::FAppStatus& Status)
	{
		TOptional<double> Active;
		if (Status.ActiveVersion.IsSet())
		{
			Active = Status.ActiveVersion.GetValue();
		}
		Out.SetField(TEXT("activeVersion"), NumberOrNull(Active));
		Out.SetBoolField(TEXT("disabled"), Status.bDisabled);
		Out.SetArrayField(TEXT("disabledTypes"), StringValues(Status.DisabledTypes));
		Out.SetBoolField(TEXT("budgetPaused"), Status.bBudgetPaused);
		Out.SetField(TEXT("budgetPauseReason"), StringOrNull(Status.BudgetPauseReason));
		Out.SetField(TEXT("instances"), NumberOrNull(IntOrUnset(Status.Instances)));
		Out.SetField(TEXT("instanceLimit"), NumberOrNull(IntOrUnset(Status.InstanceLimit)));
		Out.SetField(TEXT("reservedMb"), NumberOrNull(IntOrUnset(Status.ReservedMb)));
		Out.SetField(TEXT("maxReservedMb"), NumberOrNull(IntOrUnset(Status.MaxReservedMb)));
	}

	TArray<TSharedPtr<FJsonValue>> ArtifactRows(TConstArrayView<Dev::FBuildArtifact> Artifacts)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const Dev::FBuildArtifact& Artifact : Artifacts)
		{
			const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("crate"), Artifact.Crate);
			Row->SetStringField(TEXT("digest"), Artifact.Digest);
			Row->SetNumberField(TEXT("sizeBytes"), static_cast<double>(Artifact.SizeBytes));
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		return Rows;
	}

	void SetBuild(FJsonObject& Out, const Dev::FBuild& Build)
	{
		Out.SetStringField(TEXT("buildId"), Build.BuildId);
		Out.SetStringField(TEXT("buildStatus"), Build.Status);
		Out.SetStringField(TEXT("buildLog"), Build.Log);
		Out.SetField(TEXT("sdkVersion"), StringOrNull(Build.SdkVersion));
		Out.SetArrayField(TEXT("artifacts"), ArtifactRows(Build.Artifacts));
	}

	void SetWarnings(FJsonObject& Out, const TArray<FString>& Warnings)
	{
		Out.SetArrayField(TEXT("warnings"), StringValues(Warnings));
		for (const FString& Warning : Warnings)
		{
			Step(TEXT("warning: ") + Warning);
		}
	}

	void SetTypes(FJsonObject& Out, const CrowdyExecDeploy::FProjectDeploy& Project)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const CrowdyExecDeploy::FTypeState& Type : Project.Types)
		{
			const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("typeName"), Type.TypeName);
			Row->SetStringField(TEXT("asset"), Type.AssetPath);
			Row->SetStringField(TEXT("state"), CrateStateName(Type.State));
			Row->SetStringField(TEXT("problem"), Type.Problem);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
			const FString Problem = Type.Problem.IsEmpty() ? FString() : TEXT(": ") + Type.Problem;
			Step(FString::Printf(TEXT("%s (%s): %s%s"), *Type.TypeName, *Type.AssetPath, CrateStateName(Type.State), *Problem));
		}
		Out.SetArrayField(TEXT("types"), Rows);
		Out.SetArrayField(TEXT("problems"), StringValues(Project.Problems));
		SetWarnings(Out, Project.Warnings);
	}

	void OnStatus(const FRunRef& Run, const Dev::TResult<Dev::FAppStatus>& Answer)
	{
		if (!Answer.bOk)
		{
			FailWith(Run, Answer.Error);
			return;
		}
		SetStatus(*Run->Result, Answer.Value);
		Step(StatusLine(Answer.Value));
		Succeed(Run);
	}

	void RunStatus(const FRunRef& Run)
	{
		Run->Client->Status([Run](const Dev::TResult<Dev::FAppStatus>& Answer) { OnStatus(Run, Answer); });
	}

	void OnVersions(const FRunRef& Run, const Dev::TResult<TArray<Dev::FVersion>>& Answer)
	{
		if (!Answer.bOk)
		{
			FailWith(Run, Answer.Error);
			return;
		}
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const Dev::FVersion& Version : Answer.Value)
		{
			const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetNumberField(TEXT("version"), Version.Version);
			Row->SetStringField(TEXT("createdBy"), Version.CreatedBy);
			Row->SetStringField(TEXT("createdAt"), Version.CreatedAt.ToIso8601());
			Row->SetNumberField(TEXT("types"), Version.Types);
			Row->SetBoolField(TEXT("active"), Version.bActive);
			Row->SetStringField(TEXT("manifestJson"), Version.ManifestJson);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
			Step(FString::Printf(TEXT("version %d%s: %d types, by %s at %s"), Version.Version, Version.bActive ? TEXT(" (active)") : TEXT(""),
				Version.Types, *Version.CreatedBy, *Version.CreatedAt.ToIso8601()));
		}
		Run->Result->SetArrayField(TEXT("versions"), Rows);
		Succeed(Run);
	}

	void RunVersions(const FRunRef& Run)
	{
		Run->Client->Versions([Run](const Dev::TResult<TArray<Dev::FVersion>>& Answer) { OnVersions(Run, Answer); });
	}

	/** Every Server Object type in the project as a deploy would send it, and where each one's crate is. */
	CrowdyExecDeploy::FProjectDeploy AssembleFromDisk(TArray<const UCrowdyServerObjectDefinition*>& OutDefinitions, TMap<FString, FString>& OutCrateDirectories)
	{
		// A commandlet starts before the asset registry has finished its scan.
		IAssetRegistry::GetChecked().SearchAllAssets(true);
		for (const UCrowdyServerObjectDefinition* Definition : CrowdyExecDeploy::FindProjectDefinitions())
		{
			OutDefinitions.Add(Definition);
			OutCrateDirectories.Add(Definition->TypeName, CrowdyExecCodegen::GetCrateDirectory(*Definition));
		}
		OutCrateDirectories.Add(CrowdyExecDeploy::RootTypeName, FPaths::Combine(CrowdyExecCodegen::GetServerDirectory(), CrowdyExecDeploy::RootTypeName));
		return CrowdyExecDeploy::AssembleProject(OutDefinitions, [](const UCrowdyServerObjectDefinition& Definition) { return CrowdyExecCodegen::GetCrateDirectory(Definition); });
	}

	// -inputs= and -config= are JSON, or @<file> holding it, since a shell mangles quotes.
	bool ReadJson(const FString& Params, const TCHAR* Switch, FString& OutJson, FString& OutError)
	{
		FParse::Value(*Params, Switch, OutJson, false);
		if (!OutJson.StartsWith(TEXT("@")))
		{
			return true;
		}
		const FString Path = FPaths::ConvertRelativePathToFull(FPaths::LaunchDir(), OutJson.RightChop(1));
		if (!FFileHelper::LoadFileToString(OutJson, *Path))
		{
			OutError = FString::Printf(TEXT("could not read the %s file %s"), Switch, *Path);
			return false;
		}
		return true;
	}

	/** Writes to the project, not the app, so it needs no connection and no -yes. */
	void AddStarter(const FRunRef& Run)
	{
		FString Starter;
		FParse::Value(*Run->Params, TEXT("-starter="), Starter);
		Run->Result->SetStringField(TEXT("starter"), Starter);
		FString Config;
		FString Error;
		if (!ReadJson(Run->Params, TEXT("-config="), Config, Error))
		{
			Fail(Run, Error);
			return;
		}
		IAssetRegistry::GetChecked().SearchAllAssets(true);
		const TArray<UCrowdyServerObjectDefinition*> Found = CrowdyExecDeploy::FindProjectDefinitions();
		TArray<FString> Added;
		const bool bAdded = CrowdyGameKitStarters::AddStarter(Starter, TArray<const UCrowdyServerObjectDefinition*>(Found), CrowdyExecCodegen::GetServerDirectory(), Added, Error, Config);
		Run->Result->SetArrayField(TEXT("added"), StringValues(Added));
		Step(Added.IsEmpty() ? FString::Printf(TEXT("added nothing for %s"), *Starter) : FString::Printf(TEXT("added %s"), *FString::Join(Added, TEXT(", "))));
		if (!bAdded)
		{
			Fail(Run, Error);
			return;
		}
		Succeed(Run);
	}

	/** Writes to the project, not the app, but deletes the starters' server code with any edits, so it needs -yes. */
	void RemoveStarter(const FRunRef& Run)
	{
		FString Starter;
		FParse::Value(*Run->Params, TEXT("-starter="), Starter);
		Run->Result->SetStringField(TEXT("starter"), Starter);
		IAssetRegistry::GetChecked().SearchAllAssets(true);
		const TArray<UCrowdyServerObjectDefinition*> Found = CrowdyExecDeploy::FindProjectDefinitions();
		FString Problem;
		const TArray<UCrowdyServerObjectDefinition*> Plan = CrowdyGameKitStarters::PlanRemove(Starter, Found, Problem);
		const FString Names = FString::JoinBy(Plan, TEXT(", "), [](const UCrowdyServerObjectDefinition* Definition) { return Definition->TypeName; });
		if (!FParse::Param(*Run->Params, TEXT("yes")))
		{
			Fail(Run, Problem.IsEmpty() ? FString::Printf(TEXT("This deletes %s: their definitions and server code, edits included; pass -yes to confirm."), *Names) : Problem);
			return;
		}
		Step(FString::Printf(TEXT("removing %s"), *Names));
		TArray<FString> Removed;
		FString Error;
		const bool bRemoved = CrowdyGameKitStarters::RemoveStarter(Starter, Found, CrowdyExecCodegen::GetServerDirectory(), Removed, Error);
		Run->Result->SetArrayField(TEXT("removed"), StringValues(Removed));
		Step(Removed.IsEmpty() ? FString::Printf(TEXT("removed nothing for %s"), *Starter) : FString::Printf(TEXT("removed %s"), *FString::Join(Removed, TEXT(", "))));
		if (!bRemoved)
		{
			Fail(Run, Error);
			return;
		}
		Succeed(Run);
	}

	void RecordRevisions(FRun& Run, TConstArrayView<Dev::FBuildArtifact> Artifacts, int32 Version)
	{
		if (!Run.bRecord)
		{
			return;
		}
		TArray<FString> Errors;
		CrowdyExecRevisions::RecordDeploy(Run.Sending, Artifacts, Version, FDateTime::UtcNow(), GetDefault<UCrowdyServerComputeSettings>()->GetRevisionsToKeep(),
			[&Run](const FString& TypeName) { return Run.CrateDirectories.FindRef(TypeName); }, Errors);
		Run.Result->SetArrayField(TEXT("recordErrors"), StringValues(Errors));
		for (const FString& Error : Errors)
		{
			UE_LOG(LogCrowdyExec, Warning, TEXT("Server Compute: the deploy record was not saved for %s"), *Error);
		}
		if (Errors.IsEmpty())
		{
			Step(FString::Printf(TEXT("recorded the deployed revision of %d types"), Run.Sending.Types.Num()));
		}
	}

	TArray<TSharedPtr<FJsonValue>> BuildRows(TConstArrayView<Dev::FBuild> Builds)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const Dev::FBuild& Build : Builds)
		{
			const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			SetBuild(*Row, Build);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		return Rows;
	}

	void OnDeployDone(const FRunRef& Run, const CrowdyExecBuildAndDeploy::FOutcome& Outcome)
	{
		using CrowdyExecBuildAndDeploy::EStep;
		Run->Result->SetArrayField(TEXT("builds"), BuildRows(Outcome.Builds));
		if (!Outcome.Builds.IsEmpty())
		{
			SetBuild(*Run->Result, Outcome.Builds.Last());
		}
		// Every crate's module, built or reused, where one build's artifacts were before builds split.
		Run->Result->SetArrayField(TEXT("artifacts"), ArtifactRows(Outcome.Artifacts));
		TArray<TSharedPtr<FJsonValue>> Staged;
		for (const int32 Version : Outcome.StagedVersions)
		{
			Staged.Add(MakeShared<FJsonValueNumber>(Version));
		}
		Run->Result->SetArrayField(TEXT("stagedVersions"), Staged);
		Run->Result->SetNumberField(TEXT("restoredVersion"), Outcome.RestoredVersion);
		FString Unchanged;
		if (Outcome.Step == EStep::DeployFailed && Outcome.Error.bNoAnswer)
		{
			Unchanged = TEXT(" It had no answer, so it may still apply; -op=status shows the active version.");
		}
		else if (Outcome.RestoredVersion > 0)
		{
			Unchanged = FString::Printf(TEXT(" Version %d is active again, as before the deploy."), Outcome.RestoredVersion);
		}
		else if (!Outcome.StagedVersions.IsEmpty())
		{
			Unchanged = FString::Printf(TEXT(" Version %d is active: it runs what ran before the deploy, plus stand-ins nothing calls."), Outcome.StagedVersions.Last());
		}
		if (Outcome.Step == EStep::BuildFailed)
		{
			const Dev::FBuild& Build = Outcome.Builds.Last();
			UE_LOG(LogCrowdyExec, Display, TEXT("%s"), *Build.Log);
			Fail(Run, FString::Printf(TEXT("The build %s %s, so nothing was deployed; its log is above and in buildLog"), *Build.BuildId, *Build.Status));
			return;
		}
		if (Outcome.Step != EStep::Deployed)
		{
			Fail(Run, Outcome.Error.Message + Unchanged, Outcome.Error.Code);
			return;
		}
		Run->Result->SetNumberField(TEXT("version"), Outcome.Version);
		const FString StagedLine = Outcome.StagedVersions.IsEmpty() ? FString() : FString::Printf(TEXT(" (versions %s stored its earlier builds)"),
			*FString::JoinBy(Outcome.StagedVersions, TEXT(", "), [](int32 Version) { return FString::FromInt(Version); }));
		Step(FString::Printf(TEXT("deployed as version %d, now the app's active server code%s"), Outcome.Version, *StagedLine));
		RecordRevisions(*Run, Outcome.Artifacts, Outcome.Version);
		Succeed(Run);
	}

	struct FBuildSeen
	{
		int32 Index = INDEX_NONE;
		FString Status;
		double StartedAt = 0.0;
	};

	void LogBuild(FRun& Run, FBuildSeen& Seen, int32 Index, int32 Count, const Dev::FBuild& Build)
	{
		if (Index != Seen.Index)
		{
			// Each build has the whole build time of its own.
			Seen = FBuildSeen{Index, Build.Status, FPlatformTime::Seconds()};
			Run.Deadline = FMath::Max(Run.Deadline, Seen.StartedAt + BuildTimeoutSeconds);
			Step(FString::Printf(TEXT("build %d of %d: %s %s"), Index + 1, Count, *Build.BuildId, *Build.Status));
			return;
		}
		if (Build.Status.Equals(Seen.Status, ESearchCase::CaseSensitive))
		{
			return;
		}
		Seen.Status = Build.Status;
		const FString Against = Build.IsSucceeded() ? FString::Printf(TEXT(" against ckx-sdk %s"), Build.SdkVersion.IsEmpty() ? TEXT("(not said)") : *Build.SdkVersion) : FString();
		Step(FString::Printf(TEXT("build %s %s%s after %.0f s"), *Build.BuildId, *Build.Status, *Against, FPlatformTime::Seconds() - Seen.StartedAt));
	}

	/** -batch=N: at most N crates a build (1 to 16, default 16). */
	int32 CratesPerBuild(const FRun& Run)
	{
		int32 Batch = 16;
		FParse::Value(*Run.Params, TEXT("-batch="), Batch);
		return FMath::Clamp(Batch, 1, 16);
	}

	void BuildAndDeploy(const FRunRef& Run, CrowdyExecBuildAndDeploy::FPlan Plan, const FString& ManifestJson, const TOptional<CrowdyExecBuildAndDeploy::FLive>& Live)
	{
		Run->Result->SetStringField(TEXT("manifestJson"), ManifestJson);
		TArray<TSharedPtr<FJsonValue>> Reused;
		for (const TPair<FString, FString>& Crate : Plan.Reused)
		{
			const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("crate"), Crate.Key);
			Row->SetStringField(TEXT("digest"), Crate.Value);
			Reused.Add(MakeShared<FJsonValueObject>(Row));
		}
		Run->Result->SetArrayField(TEXT("reused"), Reused);
		const FString PlanProblem = CrowdyExecBuildAndDeploy::FindPlanProblem(Plan, ManifestJson, Live);
		if (!PlanProblem.IsEmpty())
		{
			Fail(Run, PlanProblem);
			return;
		}
		int32 Crates = 0;
		for (const TArray<Dev::FBuildCrate>& Build : Plan.Builds)
		{
			Crates += Build.Num();
		}
		Step(FString::Printf(TEXT("building %d %s of server code in %d %s; %d unchanged, named by their digest"), Crates, Crates == 1 ? TEXT("piece") : TEXT("pieces"),
			Plan.Builds.Num(), Plan.Builds.Num() == 1 ? TEXT("build") : TEXT("builds"), Plan.Reused.Num()));
		const TSharedRef<FBuildSeen> Seen = MakeShared<FBuildSeen>();
		CrowdyExecBuildAndDeploy::FCallbacks Callbacks;
		Callbacks.OnBuild = [Run, Seen](int32 Index, int32 Count, const Dev::FBuild& Build) { LogBuild(*Run, *Seen, Index, Count, Build); };
		Callbacks.OnDeploying = [Run]()
		{
			Step(TEXT("every build succeeded; deploying"));
			SendingChange(*Run);
		};
		Callbacks.OnDone = [Run](const CrowdyExecBuildAndDeploy::FOutcome& Outcome) { OnDeployDone(Run, Outcome); };
		CrowdyExecBuildAndDeploy::Run(Run->Client.ToSharedRef(), MoveTemp(Plan), ManifestJson, Live.Get(CrowdyExecBuildAndDeploy::FLive()), MoveTemp(Callbacks),
			static_cast<float>(BuildTimeoutSeconds));
	}

	/** The active version; unset when the versions or its manifest could not be read. */
	TOptional<CrowdyExecBuildAndDeploy::FLive> LiveOf(const Dev::TResult<TArray<Dev::FVersion>>& Versions)
	{
		if (!Versions.bOk)
		{
			return {};
		}
		return CrowdyExecBuildAndDeploy::FindLive(Versions.Value);
	}

	TArray<CrowdyExecRevisions::FRevision> LoadHistoryOrWarn(const FString& TypeName, const FString& CrateDirectory);

	void DeployProject(const FRunRef& Run, const Dev::TResult<TArray<Dev::FVersion>>& Versions)
	{
		if (!Versions.bOk)
		{
			Step(FString::Printf(TEXT("the versions could not be read (%s), so every piece of server code is built"), *Versions.Error.Message));
		}
		// -rebuild builds every crate, for code that should pick up the platform's current toolchain.
		const bool bRebuild = FParse::Param(*Run->Params, TEXT("rebuild"));
		const TSet<FString> Uploaded = Versions.bOk && !bRebuild ? CrowdyExecRevisions::UploadedDigests(Versions.Value) : TSet<FString>();
		CrowdyExecBuildAndDeploy::FPlan Plan = CrowdyExecBuildAndDeploy::MakePlan(Run->Sending.Crates, Uploaded,
			[&Run](const FString& CrateName) { return LoadHistoryOrWarn(CrateName, Run->CrateDirectories.FindRef(CrateName)); }, CratesPerBuild(*Run));
		BuildAndDeploy(Run, MoveTemp(Plan), Run->Sending.ManifestJson, LiveOf(Versions));
	}

	void RunDeploy(const FRunRef& Run)
	{
		TArray<const UCrowdyServerObjectDefinition*> Definitions;
		Run->Sending = AssembleFromDisk(Definitions, Run->CrateDirectories);
		SetTypes(*Run->Result, Run->Sending);
		if (!Run->Sending.Problems.IsEmpty())
		{
			Fail(Run, FString::Join(Run->Sending.Problems, TEXT("\n")));
			return;
		}
		Step(Run->Sending.Types.IsEmpty() ? FString(TEXT("deploying the Crowdy SDK's root only: the project has no Server Object types"))
			: FString::Printf(TEXT("deploying the project's %d Server Object types"), Run->Sending.Types.Num()));
		Run->bRecord = true;
		Run->Client->Versions([Run](const Dev::TResult<TArray<Dev::FVersion>>& Versions) { DeployProject(Run, Versions); });
	}

	const TCHAR* ChangeName(CrowdyExecRevisions::EChange Change)
	{
		switch (Change)
		{
		case CrowdyExecRevisions::EChange::Unchanged: return TEXT("unchanged");
		case CrowdyExecRevisions::EChange::Changed: return TEXT("changed");
		case CrowdyExecRevisions::EChange::New: return TEXT("new");
		case CrowdyExecRevisions::EChange::Unknown: return TEXT("unknown");
		case CrowdyExecRevisions::EChange::Removed: return TEXT("removed");
		default: return TEXT("not_compared");
		}
	}

	/** A history that cannot be read counts as none, so its type reads unknown rather than stopping the comparison. */
	TArray<CrowdyExecRevisions::FRevision> LoadHistoryOrWarn(const FString& TypeName, const FString& CrateDirectory)
	{
		TArray<CrowdyExecRevisions::FRevision> History;
		FString Error;
		if (!CrateDirectory.IsEmpty() && !CrowdyExecRevisions::LoadHistory(CrateDirectory, History, Error))
		{
			UE_LOG(LogCrowdyExec, Warning, TEXT("Server Compute: %s: %s"), *TypeName, *Error);
		}
		return History;
	}

	void OnChanges(const FRunRef& Run, const Dev::TResult<TArray<Dev::FVersion>>& Answer)
	{
		if (!Answer.bOk)
		{
			FailWith(Run, Answer.Error);
			return;
		}
		TArray<const UCrowdyServerObjectDefinition*> Definitions;
		TMap<FString, FString> CrateDirectories;
		const CrowdyExecDeploy::FProjectDeploy Project = AssembleFromDisk(Definitions, CrateDirectories);
		const Dev::FVersion* Live = Answer.Value.FindByPredicate([](const Dev::FVersion& Version) { return Version.bActive; });
		const TArray<CrowdyExecRevisions::FTypeChange> Changes = CrowdyExecRevisions::Compare(Project, Definitions, Live, true,
			[&CrateDirectories](const FString& TypeName) { return LoadHistoryOrWarn(TypeName, CrateDirectories.FindRef(TypeName)); });
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const CrowdyExecRevisions::FTypeChange& Change : Changes)
		{
			const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("type"), Change.TypeName);
			Row->SetStringField(TEXT("change"), ChangeName(Change.Change));
			Row->SetArrayField(TEXT("what"), StringValues(Change.What));
			TOptional<double> LiveRevision;
			if (Change.LiveRevision != INDEX_NONE)
			{
				LiveRevision = static_cast<double>(Change.LiveRevision);
			}
			Row->SetField(TEXT("liveRevision"), NumberOrNull(LiveRevision));
			Rows.Add(MakeShared<FJsonValueObject>(Row));
			const FString What = Change.What.IsEmpty() ? FString() : FString::Printf(TEXT(" (%s)"), *FString::Join(Change.What, TEXT(", ")));
			const FString Revision = LiveRevision.IsSet() ? FString::Printf(TEXT(", live revision %d"), Change.LiveRevision) : FString();
			Step(FString::Printf(TEXT("%s: %s%s%s"), *Change.TypeName, ChangeName(Change.Change), *What, *Revision));
		}
		// A type whose server code is not ready differs from what a deploy would send once it is, so it counts as a change.
		const bool bReady = Project.Problems.IsEmpty();
		const bool bHasChanges = CrowdyExecRevisions::HasChanges(Changes) || !bReady;
		Run->Result->SetNumberField(TEXT("liveVersion"), Live ? Live->Version : 0);
		Run->Result->SetArrayField(TEXT("types"), Rows);
		Run->Result->SetArrayField(TEXT("problems"), StringValues(Project.Problems));
		SetWarnings(*Run->Result, Project.Warnings);
		Run->Result->SetBoolField(TEXT("ready"), bReady);
		Run->Result->SetBoolField(TEXT("hasChanges"), bHasChanges);
		const TCHAR* const Summary = !bReady ? TEXT("not ready to deploy; see problems") : (bHasChanges ? TEXT("a deploy would change what runs") : TEXT("nothing changed"));
		Step(FString::Printf(TEXT("live version %s; %s"), Live ? *FString::FromInt(Live->Version) : TEXT("none"), Summary));
		Succeed(Run);
	}

	void RunChanges(const FRunRef& Run)
	{
		Run->Client->Versions([Run](const Dev::TResult<TArray<Dev::FVersion>>& Answer) { OnChanges(Run, Answer); });
	}

	void OnStarters(const FRunRef& Run, const Dev::TResult<Dev::FStarterPack>& Pack)
	{
		if (!Pack.bOk)
		{
			FailWith(Run, Pack.Error);
			return;
		}
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const Dev::FStarter& Starter : Pack.Value.Starters)
		{
			const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("nodeType"), Starter.NodeType);
			Row->SetStringField(TEXT("description"), Starter.Description);
			Row->SetStringField(TEXT("crate"), Starter.Crate.Name);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
			Step(FString::Printf(TEXT("starter %s: %s"), *Starter.NodeType, *Starter.Description));
		}
		Run->Result->SetArrayField(TEXT("starters"), Rows);
		CrowdyExecBuildAndDeploy::FPlan Plan = CrowdyExecBuildAndDeploy::MakePlan(Pack.Value.GetCrates(), TSet<FString>(),
			[](const FString&) { return TArray<CrowdyExecRevisions::FRevision>(); }, CratesPerBuild(*Run));
		Run->Client->Versions([Run, Plan = MoveTemp(Plan), ManifestJson = Pack.Value.ManifestJson](const Dev::TResult<TArray<Dev::FVersion>>& Versions) mutable
		{
			BuildAndDeploy(Run, MoveTemp(Plan), ManifestJson, LiveOf(Versions));
		});
	}

	void RunStarters(const FRunRef& Run)
	{
		Run->Client->Starters([Run](const Dev::TResult<Dev::FStarterPack>& Pack) { OnStarters(Run, Pack); });
	}

	void RunActivate(const FRunRef& Run)
	{
		int32 Version = 0;
		if (!FParse::Value(*Run->Params, TEXT("-version="), Version) || Version < 1)
		{
			Fail(Run, TEXT("activate needs -version=N, the version to make the app's active server code"));
			return;
		}
		Run->Result->SetNumberField(TEXT("version"), Version);
		Step(FString::Printf(TEXT("making version %d the app's active server code"), Version));
		SendingChange(*Run);
		Run->Client->ActivateVersion(Version, [Run](const Dev::TResult<Dev::FAppStatus>& Answer) { OnStatus(Run, Answer); });
	}

	void RunSwitch(const FRunRef& Run, bool bEnabled)
	{
		FString Type;
		if (!FParse::Value(*Run->Params, TEXT("-type="), Type) || Type.IsEmpty())
		{
			Fail(Run, FString::Printf(TEXT("%s needs -type=T, the Server Object type to switch %s"), bEnabled ? TEXT("enable") : TEXT("disable"), bEnabled ? TEXT("on") : TEXT("off")));
			return;
		}
		Run->Result->SetStringField(TEXT("type"), Type);
		Run->Result->SetBoolField(TEXT("enabled"), bEnabled);
		Step(FString::Printf(TEXT("switching %s %s"), *Type, bEnabled ? TEXT("on") : TEXT("off")));
		SendingChange(*Run);
		Run->Client->SetEnabled(bEnabled, Type, [Run](const Dev::TResult<Dev::FAppStatus>& Answer) { OnStatus(Run, Answer); });
	}

	void RunDisable(const FRunRef& Run)
	{
		RunSwitch(Run, false);
	}

	void RunEnable(const FRunRef& Run)
	{
		RunSwitch(Run, true);
	}

	void RunRestart(const FRunRef& Run)
	{
		FString Type;
		if (!FParse::Value(*Run->Params, TEXT("-type="), Type) || Type.IsEmpty())
		{
			Fail(Run, TEXT("restart needs -type=T, the Server Object type whose running instances move to the active version"));
			return;
		}
		Run->Result->SetStringField(TEXT("type"), Type);
		Step(FString::Printf(TEXT("moving %s's running instances to the active version"), *Type));
		SendingChange(*Run);
		Run->Client->RestartType(Type, [Run](const Dev::TResult<Dev::FRestart>& Answer)
		{
			if (!Answer.bOk)
			{
				FailWith(Run, Answer.Error);
				return;
			}
			Run->Result->SetNumberField(TEXT("stopped"), Answer.Value.Stopped);
			Step(FString::Printf(TEXT("%s: %d running instances stopped; each starts again on the active version at its next call"), *Answer.Value.NodeType, Answer.Value.Stopped));
			Succeed(Run);
		});
	}

	const UCrowdyServerObjectDefinition* FindProjectType(const FString& Type)
	{
		IAssetRegistry::GetChecked().SearchAllAssets(true);
		const TArray<UCrowdyServerObjectDefinition*> Definitions = CrowdyExecDeploy::FindProjectDefinitions();
		UCrowdyServerObjectDefinition* const* Found = Definitions.FindByPredicate([&Type](const UCrowdyServerObjectDefinition* Each)
		{
			return Each && Each->TypeName.Equals(Type, ESearchCase::CaseSensitive);
		});
		return Found ? *Found : nullptr;
	}

	/** Calls Function as the developer and reports its answer; a Game Kit starter's empty Ref is filled with a fresh one first. */
	void SendDeveloperCall(const FRunRef& Run, const UCrowdyServerObjectDefinition& Definition, const FCrowdyServerFunction& Function, const FString& Key, FInstancedStruct Inputs)
	{
		const FString Pointless = CrowdyExecDeveloperCall::FindPointlessCall(Definition, Key, Inputs);
		if (!Pointless.IsEmpty())
		{
			Fail(Run, Pointless);
			return;
		}
		FString Ref;
		CrowdyExecDeveloperCall::FillFreshRef(Definition, Function, Inputs, Ref);
		Run->Result->SetStringField(TEXT("type"), Definition.TypeName);
		Run->Result->SetStringField(TEXT("key"), Key);
		Run->Result->SetStringField(TEXT("fn"), Function.Name.ToString());
		if (!Ref.IsEmpty())
		{
			Run->Result->SetStringField(TEXT("ref"), Ref);
		}
		Step(FString::Printf(TEXT("calling %s on %s \"%s\" as the developer%s"), *Function.Name.ToString(), *Definition.TypeName, *Key,
			Ref.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", ref %s"), *Ref)));
		SendingChange(*Run);
		CrowdyExecDeveloperCall::Call(*Run->Client, Definition, Function, Key, Inputs, [Run](const CrowdyExecDeveloperCall::FResult& Result)
		{
			Run->Result->SetStringField(TEXT("status"), Result.Status);
			Run->Result->SetStringField(TEXT("message"), Result.Message);
			Run->Result->SetStringField(TEXT("reply"), Result.ReplyJson);
			Step(FString::Printf(TEXT("%s%s%s"), *Result.Status, Result.Message.IsEmpty() ? TEXT("") : *(TEXT(": ") + Result.Message),
				Result.ReplyJson.IsEmpty() ? TEXT("") : *(TEXT(", reply ") + Result.ReplyJson)));
			if (!Result.bOk)
			{
				Fail(Run, FString::Printf(TEXT("%s: %s"), *Result.Status, *Result.Message), Result.Status);
				return;
			}
			Succeed(Run);
		});
	}

	/** grant and deposit: in-game soft currency and items into one player's player starter, never real money. */
	void RunSoftCurrency(const FRunRef& Run, const TCHAR* Method)
	{
		FString Type = TEXT("player");
		FString Player;
		FString Ref;
		FString XpText;
		FParse::Value(*Run->Params, TEXT("-type="), Type);
		FParse::Value(*Run->Params, TEXT("-player="), Player);
		FParse::Value(*Run->Params, TEXT("-ref="), Ref);
		FParse::Value(*Run->Params, TEXT("-xp="), XpText);
		int64 Xp = 0;
		if (!XpText.IsEmpty() && (!LexTryParseString(Xp, *XpText) || Xp < 0 || LexToString(Xp) != XpText))
		{
			Fail(Run, FString::Printf(TEXT("-xp=%s is not a whole number of 0 or more"), *XpText));
			return;
		}
		FString BundleJson;
		FString Error;
		if (!ReadJson(Run->Params, TEXT("-bundle="), BundleJson, Error))
		{
			Fail(Run, Error);
			return;
		}
		if (Player.IsEmpty() || BundleJson.TrimStartAndEnd().IsEmpty())
		{
			Fail(Run, FString::Printf(TEXT("%s needs -player=<user id> and -bundle=<json or @file>, such as {\"Currency\": {\"gold\": 100}, \"Items\": {\"wood\": 5}}"), Method));
			return;
		}
		TSharedPtr<FJsonObject> Bundle;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(BundleJson), Bundle) || !Bundle)
		{
			Fail(Run, FString::Printf(TEXT("-bundle is not a JSON object: %s"), *BundleJson));
			return;
		}
		// The converter skips a name it does not know, which would send an empty bundle without a word.
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Bundle->Values)
		{
			if (!Field.Key.Equals(TEXT("Items"), ESearchCase::IgnoreCase) && !Field.Key.Equals(TEXT("Currency"), ESearchCase::IgnoreCase))
			{
				Fail(Run, FString::Printf(TEXT("-bundle has %s, which is neither Items nor Currency: write it as {\"Currency\": {\"gold\": 100}, \"Items\": {\"wood\": 5}}"), *Field.Key));
				return;
			}
		}
		const UCrowdyServerObjectDefinition* Definition = FindProjectType(Type);
		const FCrowdyServerFunction* Function = Definition ? Definition->Functions.FindByPredicate([Method](const FCrowdyServerFunction& Each)
		{
			return Each.GetMethodName().Equals(Method, ESearchCase::CaseSensitive);
		}) : nullptr;
		if (!Function || !Definition->IsGameKitStarter())
		{
			Fail(Run, FString::Printf(TEXT("the project has no %s starter with %s; add the player starter first"), *Type, Method));
			return;
		}
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("Ref"), Ref);
		Root->SetObjectField(TEXT("Bundle"), Bundle);
		if (Xp != 0)
		{
			Root->SetNumberField(TEXT("Xp"), static_cast<double>(Xp));
		}
		FString InputsJson;
		FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&InputsJson));
		FInstancedStruct Inputs;
		if (!CrowdyExecDeveloperCall::InputsFromJson(*Function, InputsJson, Inputs, Error))
		{
			Fail(Run, Error);
			return;
		}
		Step(FString::Printf(TEXT("%s in-game soft currency and items to player %s (never real money)"), Method, *Player));
		SendDeveloperCall(Run, *Definition, *Function, Player, MoveTemp(Inputs));
	}

	void RunGrant(const FRunRef& Run)
	{
		RunSoftCurrency(Run, TEXT("grant"));
	}

	void RunDeposit(const FRunRef& Run)
	{
		RunSoftCurrency(Run, TEXT("deposit"));
	}

	void RunCall(const FRunRef& Run)
	{
		FString Type;
		FString Key;
		FString FunctionName;
		FParse::Value(*Run->Params, TEXT("-type="), Type);
		FParse::Value(*Run->Params, TEXT("-key="), Key);
		FParse::Value(*Run->Params, TEXT("-fn="), FunctionName);
		if (Type.IsEmpty() || FunctionName.IsEmpty())
		{
			Fail(Run, TEXT("call needs -type=T -fn=F and -key=K (the Instance Id, not needed for a type with Only One Instance), with -inputs=<json or @file>"));
			return;
		}
		const UCrowdyServerObjectDefinition* Definition = FindProjectType(Type);
		const FCrowdyServerFunction* Function = Definition ? Definition->FindFunction(FName(*FunctionName)) : nullptr;
		if (!Function)
		{
			Fail(Run, Definition ? FString::Printf(TEXT("%s has no Server Function %s"), *Type, *FunctionName) : FString::Printf(TEXT("the project has no Server Object type %s"), *Type));
			return;
		}
		if (!CrowdyExecDeveloperCall::IsForDevelopers(*Definition, *Function))
		{
			Fail(Run, FString::Printf(TEXT("%s is Callable By players; call it from a game. Only Server Only and Developers Only functions are called here."), *FunctionName));
			return;
		}
		FInstancedStruct Inputs;
		FString InputsJson;
		FString Error;
		if (!ReadJson(Run->Params, TEXT("-inputs="), InputsJson, Error) || !CrowdyExecDeveloperCall::InputsFromJson(*Function, InputsJson, Inputs, Error, Definition))
		{
			Fail(Run, Error);
			return;
		}
		SendDeveloperCall(Run, *Definition, *Function, Key, MoveTemp(Inputs));
	}

	void OnInstances(const FRunRef& Run, const Dev::TResult<TArray<Dev::FInstance>>& Answer)
	{
		if (!Answer.bOk)
		{
			FailWith(Run, Answer.Error);
			return;
		}
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const Dev::FInstance& Instance : Answer.Value)
		{
			const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("instanceId"), Instance.InstanceId);
			Row->SetStringField(TEXT("nodeType"), Instance.NodeType);
			Row->SetStringField(TEXT("key"), Instance.Key);
			Row->SetStringField(TEXT("kind"), Instance.Kind);
			Row->SetStringField(TEXT("phase"), Instance.Phase);
			Row->SetField(TEXT("host"), StringOrNull(Instance.Host));
			Row->SetField(TEXT("heldBack"), StringOrNull(Instance.HeldBack));
			Row->SetStringField(TEXT("epoch"), LexToString(Instance.Epoch));
			Row->SetNumberField(TEXT("sinceMs"), Instance.SinceMs);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
			Step(FString::Printf(TEXT("%s %s: %s, epoch %lld%s"), *Instance.NodeType, *Instance.Key, *Instance.Phase, Instance.Epoch,
				Instance.HeldBack.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", held back: %s"), *Instance.HeldBack)));
		}
		Run->Result->SetArrayField(TEXT("instances"), Rows);
		Step(FString::Printf(TEXT("%d instances placed"), Rows.Num()));
		Succeed(Run);
	}

	void RunInstances(const FRunRef& Run)
	{
		Run->Client->Instances([Run](const Dev::TResult<TArray<Dev::FInstance>>& Answer) { OnInstances(Run, Answer); });
	}

	void OnLogs(const FRunRef& Run, const Dev::TResult<TArray<Dev::FLogLine>>& Answer)
	{
		if (!Answer.bOk)
		{
			FailWith(Run, Answer.Error);
			return;
		}
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const Dev::FLogLine& Line : Answer.Value)
		{
			const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("id"), Line.Id);
			Row->SetStringField(TEXT("nodeType"), Line.NodeType);
			Row->SetStringField(TEXT("key"), Line.Key);
			Row->SetNumberField(TEXT("level"), Line.Level);
			Row->SetStringField(TEXT("levelName"), LevelName(Line.Level));
			Row->SetStringField(TEXT("host"), Line.Host);
			Row->SetStringField(TEXT("at"), Line.At.ToIso8601());
			Row->SetField(TEXT("flow"), StringOrNull(Line.Flow));
			Row->SetStringField(TEXT("text"), Line.Text);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
			Step(FString::Printf(TEXT("%s %s %s %s %s %s"), *Line.At.ToIso8601(), LevelName(Line.Level), *Line.NodeType, *Line.Key,
				Line.Flow.IsEmpty() ? TEXT("-") : *Line.Flow, *Line.Text));
		}
		Run->Result->SetArrayField(TEXT("lines"), Rows);
		Step(FString::Printf(TEXT("%d log lines, newest first"), Rows.Num()));
		Succeed(Run);
	}

	void RunLogs(const FRunRef& Run)
	{
		Dev::FLogQuery Query;
		FParse::Value(*Run->Params, TEXT("-type="), Query.NodeType);
		FParse::Value(*Run->Params, TEXT("-flow="), Query.Flow);
		FParse::Value(*Run->Params, TEXT("-limit="), Query.Limit);
		Run->Result->SetStringField(TEXT("type"), Query.NodeType);
		Run->Result->SetStringField(TEXT("flow"), Query.Flow);
		Run->Result->SetNumberField(TEXT("limit"), Query.Limit);
		Run->Client->Logs(Query, [Run](const Dev::TResult<TArray<Dev::FLogLine>>& Answer) { OnLogs(Run, Answer); });
	}

	void OnStats(const FRunRef& Run, const Dev::TResult<TArray<Dev::FEndpointStat>>& Answer)
	{
		if (!Answer.bOk)
		{
			FailWith(Run, Answer.Error);
			return;
		}
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const Dev::FEndpointStat& Stat : Answer.Value)
		{
			const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("nodeType"), Stat.NodeType);
			Row->SetStringField(TEXT("method"), Stat.Method);
			Row->SetNumberField(TEXT("calls"), Stat.Calls);
			Row->SetNumberField(TEXT("appErrors"), Stat.AppErrors);
			Row->SetNumberField(TEXT("busy"), Stat.Busy);
			Row->SetNumberField(TEXT("denied"), Stat.Denied);
			Row->SetNumberField(TEXT("deadlineExceeded"), Stat.DeadlineExceeded);
			Row->SetNumberField(TEXT("otherErrors"), Stat.OtherErrors);
			Row->SetField(TEXT("latencyMsAvg"), NumberOrNull(Stat.LatencyMsAvg));
			Row->SetField(TEXT("latencyMsMax"), NumberOrNull(Stat.LatencyMsMax));
			Rows.Add(MakeShared<FJsonValueObject>(Row));
			Step(FString::Printf(TEXT("%s %s: %.0f calls, %.0f errors, %.0f busy, %.0f denied, %.0f timeouts, average %.1f ms, max %.1f ms"),
				*Stat.NodeType, *Stat.Method, Stat.Calls, Stat.AppErrors + Stat.OtherErrors, Stat.Busy, Stat.Denied, Stat.DeadlineExceeded,
				Stat.LatencyMsAvg.Get(0.0), Stat.LatencyMsMax.Get(0.0)));
		}
		Run->Result->SetArrayField(TEXT("stats"), Rows);
		Succeed(Run);
	}

	void RunStats(const FRunRef& Run)
	{
		FString Type;
		FParse::Value(*Run->Params, TEXT("-type="), Type);
		Run->Result->SetStringField(TEXT("type"), Type);
		Run->Client->EndpointStats(Type, 0, [Run](const Dev::TResult<TArray<Dev::FEndpointStat>>& Answer) { OnStats(Run, Answer); });
	}

	struct FOperation
	{
		const TCHAR* Name;
		void (*Start)(const FRunRef&);
		double TimeoutSeconds;
		/** Replaces or switches the app's live server code, so it runs only with -yes. */
		bool bChangesLiveCode;
	};

	const FOperation Operations[] = {
		{TEXT("status"), &RunStatus, ShortTimeoutSeconds, false},
		{TEXT("versions"), &RunVersions, ShortTimeoutSeconds, false},
		{TEXT("changes"), &RunChanges, ShortTimeoutSeconds, false},
		{TEXT("deploy"), &RunDeploy, BuildTimeoutSeconds, true},
		{TEXT("starters"), &RunStarters, BuildTimeoutSeconds, true},
		{TEXT("activate"), &RunActivate, ChangeTimeoutSeconds, true},
		{TEXT("disable"), &RunDisable, ChangeTimeoutSeconds, true},
		{TEXT("enable"), &RunEnable, ChangeTimeoutSeconds, true},
		{TEXT("restart"), &RunRestart, ChangeTimeoutSeconds, true},
		{TEXT("instances"), &RunInstances, ShortTimeoutSeconds, false},
		{TEXT("call"), &RunCall, ChangeTimeoutSeconds, true},
		{TEXT("grant"), &RunGrant, ChangeTimeoutSeconds, true},
		{TEXT("deposit"), &RunDeposit, ChangeTimeoutSeconds, true},
		{TEXT("logs"), &RunLogs, ShortTimeoutSeconds, false},
		{TEXT("stats"), &RunStats, ShortTimeoutSeconds, false}};

	const FOperation* FindOperation(const FString& Name)
	{
		for (const FOperation& Operation : Operations)
		{
			if (Name.Equals(Operation.Name, ESearchCase::IgnoreCase))
			{
				return &Operation;
			}
		}
		return nullptr;
	}

	void Start(const FRunRef& Run, const FOperation& Operation)
	{
		FString Error;
		Run->Client = Dev::FClient::Create(Error);
		if (!Run->Client)
		{
			Fail(Run, Error);
			return;
		}
		Run->Result->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), Run->Client->GetAppId()));
		Step(FString::Printf(TEXT("%s for app %lld"), Operation.Name, Run->Client->GetAppId()));
		Operation.Start(Run);
	}

	/** A commandlet has no engine loop, so the ticker (which also ticks the HTTP module) is driven here until the operation ends or its deadline passes. */
	bool Pump(const FRun& Run)
	{
		double Last = FPlatformTime::Seconds();
		while (!Run.bDone && !IsEngineExitRequested())
		{
			const double Now = FPlatformTime::Seconds();
			if (Now > Run.Deadline)
			{
				return false;
			}
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FTSTicker::GetCoreTicker().Tick(static_cast<float>(Now - Last));
			Last = Now;
			FPlatformProcess::Sleep(0.01f);
		}
		return Run.bDone;
	}

	bool WriteResult(const TSharedRef<FJsonObject>& Result, const FString& OutPath)
	{
		const FString Path = FPaths::ConvertRelativePathToFull(FPaths::LaunchDir(), OutPath);
		FString Text;
		FJsonSerializer::Serialize(Result, TJsonWriterFactory<>::Create(&Text));
		if (!FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			UE_LOG(LogCrowdyExec, Error, TEXT("Server Compute: could not write %s"), *Path);
			return false;
		}
		Step(FString::Printf(TEXT("wrote %s"), *Path));
		return true;
	}

	bool Finish(const FRunRef& Run)
	{
		Run->Result->SetBoolField(TEXT("ok"), Run->bOk);
		Run->Result->SetStringField(TEXT("error"), Run->Error);
		Run->Result->SetStringField(TEXT("errorCode"), Run->ErrorCode);
		const FString Code = Run->ErrorCode.IsEmpty() ? FString() : FString::Printf(TEXT(" [%s]"), *Run->ErrorCode);
		UE_CLOG(Run->bOk, LogCrowdyExec, Display, TEXT("Server Compute: done"));
		UE_CLOG(!Run->bOk, LogCrowdyExec, Error, TEXT("Server Compute: %s%s"), *Run->Error, *Code);
		FString OutPath;
		if (!FParse::Value(*Run->Params, TEXT("-out="), OutPath))
		{
			return Run->bOk;
		}
		return WriteResult(Run->Result, OutPath) && Run->bOk;
	}

	/** Runs one operation from its argument list with its own client and deadline, writes its -out file and returns whether it succeeded. */
	bool RunOne(const FString& Params)
	{
		const FRunRef Run = MakeShared<FRun>();
		Run->Params = Params;
		FString OperationName;
		FParse::Value(*Params, TEXT("-op="), OperationName);
		Run->Result->SetStringField(TEXT("op"), OperationName);
		if (OperationName.Equals(TEXT("addstarter"), ESearchCase::IgnoreCase))
		{
			AddStarter(Run);
			return Finish(Run);
		}
		if (OperationName.Equals(TEXT("removestarter"), ESearchCase::IgnoreCase))
		{
			RemoveStarter(Run);
			return Finish(Run);
		}
		const FOperation* Operation = FindOperation(OperationName);
		if (!Operation)
		{
			Fail(Run, FString::Printf(TEXT("Unknown operation '%s'. Usage: %s"), *OperationName, Usage));
			return Finish(Run);
		}
		if (Operation->bChangesLiveCode && !FParse::Param(*Params, TEXT("yes")))
		{
			Fail(Run, ConfirmError);
			return Finish(Run);
		}
		Run->Deadline = FPlatformTime::Seconds() + Operation->TimeoutSeconds;
		Start(Run, *Operation);
		if (Pump(*Run))
		{
			return Finish(Run);
		}
		if (Run->bChangeSent)
		{
			Fail(Run, FString::Printf(TEXT("%s: no answer in time; the change may still apply"), Operation->Name));
			return Finish(Run);
		}
		Fail(Run, FString::Printf(TEXT("%s did not finish in time"), Operation->Name));
		return Finish(Run);
	}

	bool RunBatch(const FString& Params)
	{
		FString OpsPath;
		if (!FParse::Value(*Params, TEXT("-ops="), OpsPath))
		{
			UE_LOG(LogCrowdyExec, Error, TEXT("Server Compute: batch needs -ops=<file>"));
			return false;
		}
		const FString Path = FPaths::ConvertRelativePathToFull(FPaths::LaunchDir(), OpsPath);
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			UE_LOG(LogCrowdyExec, Error, TEXT("Server Compute: could not read the ops file %s"), *Path);
			return false;
		}
		TArray<FString> Lines;
		FString Error;
		if (!UCrowdyServerComputeCommandlet::ParseBatchLines(Text, Lines, Error))
		{
			UE_LOG(LogCrowdyExec, Error, TEXT("Server Compute: %s: %s"), *Path, *Error);
			return false;
		}
		bool bAllOk = true;
		for (int32 Index = 0; Index < Lines.Num(); ++Index)
		{
			const bool bOk = RunOne(Lines[Index]);
			FString OperationName;
			FParse::Value(*Lines[Index], TEXT("-op="), OperationName);
			UE_LOG(LogCrowdyExec, Display, TEXT("Server Compute batch: %d/%d -op=%s %s"), Index + 1, Lines.Num(), *OperationName, bOk ? TEXT("ok") : TEXT("failed"));
			if (bOk)
			{
				continue;
			}
			bAllOk = false;
			if (!FParse::Param(*Lines[Index], TEXT("batchcontinue")))
			{
				break;
			}
		}
		return bAllOk;
	}
}

UCrowdyServerComputeCommandlet::UCrowdyServerComputeCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
	// Otherwise any error line logged during the run turns a success into exit code 1.
	UseCommandletResultAsExitCode = true;
}

int32 UCrowdyServerComputeCommandlet::Main(const FString& Params)
{
	using namespace CrowdyServerComputeCommandletDetail;
	FString OperationName;
	FParse::Value(*Params, TEXT("-op="), OperationName);
	if (OperationName.Equals(TEXT("batch"), ESearchCase::IgnoreCase))
	{
		return RunBatch(Params) ? 0 : 1;
	}
	return RunOne(Params) ? 0 : 1;
}

bool UCrowdyServerComputeCommandlet::ParseBatchLines(const FString& Text, TArray<FString>& OutLines, FString& OutError)
{
	OutLines.Reset();
	TArray<FString> Raw;
	Text.ParseIntoArrayLines(Raw, false);
	for (int32 Index = 0; Index < Raw.Num(); ++Index)
	{
		const FString Line = Raw[Index].TrimStartAndEnd();
		if (Line.IsEmpty() || Line.StartsWith(TEXT("#")))
		{
			continue;
		}
		FString OperationName;
		FParse::Value(*Line, TEXT("-op="), OperationName);
		if (OperationName.Equals(TEXT("batch"), ESearchCase::IgnoreCase))
		{
			OutLines.Reset();
			OutError = FString::Printf(TEXT("line %d: a batch cannot run -op=batch"), Index + 1);
			return false;
		}
		OutLines.Add(Line);
	}
	if (OutLines.IsEmpty())
	{
		OutError = TEXT("the ops file holds no operations");
		return false;
	}
	return true;
}
