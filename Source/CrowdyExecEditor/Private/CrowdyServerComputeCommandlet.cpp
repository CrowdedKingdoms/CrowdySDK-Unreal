#include "CrowdyServerComputeCommandlet.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "CoreGlobals.h"
#include "CrowdyExecCodegen.h"
#include "CrowdyExecDeploy.h"
#include "CrowdyExecDeveloperApi.h"
#include "CrowdyExecLog.h"
#include "CrowdyExecRevisions.h"
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

	const TCHAR* const Usage = TEXT("-run=CrowdyServerCompute -op=status | versions | changes | deploy -yes | starters -yes | activate -version=N -yes | disable -type=T -yes")
		TEXT(" | enable -type=T -yes | logs [-type=T] [-flow=F] [-limit=N] | stats [-type=T], and -out=<file> for a JSON result");

	const TCHAR* const ConfirmError = TEXT("This replaces or switches the app's live server code; pass -yes to confirm.");

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
		return FString::Printf(TEXT("active version %s; server code %s; types switched off: %s; budget %s"), *Active,
			Status.bDisabled ? TEXT("switched off") : TEXT("on"), *Off, Status.bBudgetPaused ? TEXT("paused") : TEXT("not paused"));
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
	}

	void SetBuild(FJsonObject& Out, const Dev::FBuild& Build)
	{
		TArray<TSharedPtr<FJsonValue>> Artifacts;
		for (const Dev::FBuildArtifact& Artifact : Build.Artifacts)
		{
			const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("crate"), Artifact.Crate);
			Row->SetStringField(TEXT("digest"), Artifact.Digest);
			Row->SetNumberField(TEXT("sizeBytes"), static_cast<double>(Artifact.SizeBytes));
			Artifacts.Add(MakeShared<FJsonValueObject>(Row));
		}
		Out.SetStringField(TEXT("buildId"), Build.BuildId);
		Out.SetStringField(TEXT("buildStatus"), Build.Status);
		Out.SetStringField(TEXT("buildLog"), Build.Log);
		Out.SetArrayField(TEXT("artifacts"), Artifacts);
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
		return CrowdyExecDeploy::AssembleProject(OutDefinitions, [](const UCrowdyServerObjectDefinition& Definition) { return CrowdyExecCodegen::GetCrateDirectory(Definition); });
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

	void OnDeployed(const FRunRef& Run, TConstArrayView<Dev::FBuildArtifact> Artifacts, const Dev::TResult<int32>& Deployed)
	{
		if (!Deployed.bOk)
		{
			FailWith(Run, Deployed.Error);
			return;
		}
		Run->Result->SetNumberField(TEXT("version"), Deployed.Value);
		Step(FString::Printf(TEXT("deployed as version %d, now the app's active server code"), Deployed.Value));
		RecordRevisions(*Run, Artifacts, Deployed.Value);
		Succeed(Run);
	}

	void DeployBuild(const FRunRef& Run, const FString& ManifestJson, const Dev::TResult<Dev::FBuild>& Finished)
	{
		if (!Finished.bOk)
		{
			FailWith(Run, Finished.Error);
			return;
		}
		const Dev::FBuild& Build = Finished.Value;
		SetBuild(*Run->Result, Build);
		if (!Build.IsSucceeded())
		{
			UE_LOG(LogCrowdyExec, Display, TEXT("%s"), *Build.Log);
			Fail(Run, FString::Printf(TEXT("The build %s %s; its log is above and in buildLog"), *Build.BuildId, *Build.Status));
			return;
		}
		Step(FString::Printf(TEXT("build %s succeeded; deploying"), *Build.BuildId));
		SendingChange(*Run);
		Run->Client->Deploy(ManifestJson, Build.BuildId, [Run, Artifacts = Build.Artifacts](const Dev::TResult<int32>& Deployed) { OnDeployed(Run, Artifacts, Deployed); });
	}

	void LogProgress(FString& InOutSeen, double StartedAt, const Dev::FBuild& Progress)
	{
		if (Progress.Status.Equals(InOutSeen, ESearchCase::CaseSensitive))
		{
			return;
		}
		InOutSeen = Progress.Status;
		Step(FString::Printf(TEXT("build %s %s after %.0f s"), *Progress.BuildId, *Progress.Status, FPlatformTime::Seconds() - StartedAt));
	}

	void WaitAndDeploy(const FRunRef& Run, const FString& ManifestJson, const Dev::TResult<Dev::FBuild>& Started)
	{
		if (!Started.bOk)
		{
			FailWith(Run, Started.Error);
			return;
		}
		SetBuild(*Run->Result, Started.Value);
		Step(FString::Printf(TEXT("build %s %s"), *Started.Value.BuildId, *Started.Value.Status));
		const double StartedAt = FPlatformTime::Seconds();
		const TSharedRef<FString> Seen = MakeShared<FString>(Started.Value.Status);
		const float TimeLeft = static_cast<float>(FMath::Max(0.0, Run->Deadline - StartedAt));
		Run->Client->WaitForBuild(Started.Value.BuildId,
			[Run, ManifestJson](const Dev::TResult<Dev::FBuild>& Finished) { DeployBuild(Run, ManifestJson, Finished); },
			[Seen, StartedAt](const Dev::FBuild& Progress) { LogProgress(*Seen, StartedAt, Progress); }, 3.f, TimeLeft);
	}

	void BuildAndDeploy(const FRunRef& Run, const TArray<Dev::FBuildCrate>& Crates, const FString& ManifestJson)
	{
		Run->Result->SetStringField(TEXT("manifestJson"), ManifestJson);
		Step(FString::Printf(TEXT("building %d pieces of server code"), Crates.Num()));
		Run->Client->Build(Crates, [Run, ManifestJson](const Dev::TResult<Dev::FBuild>& Started) { WaitAndDeploy(Run, ManifestJson, Started); });
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
		Step(FString::Printf(TEXT("deploying the project's %d Server Object types"), Run->Sending.Types.Num()));
		Run->bRecord = true;
		BuildAndDeploy(Run, Run->Sending.Crates, Run->Sending.ManifestJson);
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
		TArray<Dev::FBuildCrate> Crates;
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const Dev::FStarter& Starter : Pack.Value.Starters)
		{
			Crates.Add(Starter.Crate);
			const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("nodeType"), Starter.NodeType);
			Row->SetStringField(TEXT("description"), Starter.Description);
			Row->SetStringField(TEXT("crate"), Starter.Crate.Name);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
			Step(FString::Printf(TEXT("starter %s: %s"), *Starter.NodeType, *Starter.Description));
		}
		Run->Result->SetArrayField(TEXT("starters"), Rows);
		BuildAndDeploy(Run, Crates, Pack.Value.ManifestJson);
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
	const FRunRef Run = MakeShared<FRun>();
	Run->Params = Params;
	FString OperationName;
	FParse::Value(*Params, TEXT("-op="), OperationName);
	Run->Result->SetStringField(TEXT("op"), OperationName);
	const FOperation* Operation = FindOperation(OperationName);
	if (!Operation)
	{
		Fail(Run, FString::Printf(TEXT("Unknown operation '%s'. Usage: %s"), *OperationName, Usage));
		return Finish(Run) ? 0 : 1;
	}
	if (Operation->bChangesLiveCode && !FParse::Param(*Params, TEXT("yes")))
	{
		Fail(Run, ConfirmError);
		return Finish(Run) ? 0 : 1;
	}
	Run->Deadline = FPlatformTime::Seconds() + Operation->TimeoutSeconds;
	Start(Run, *Operation);
	if (Pump(*Run))
	{
		return Finish(Run) ? 0 : 1;
	}
	if (Run->bChangeSent)
	{
		Fail(Run, FString::Printf(TEXT("%s: no answer in time; the change may still apply"), Operation->Name));
		return Finish(Run) ? 0 : 1;
	}
	Fail(Run, FString::Printf(TEXT("%s did not finish in time"), Operation->Name));
	return Finish(Run) ? 0 : 1;
}
