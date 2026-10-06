#include "CrowdyServerComputeService.h"

#include "CrowdyExecCodegen.h"
#include "CrowdyExecLog.h"
#include "CrowdyServerCodeFiles.h"
#include "CrowdyServerComputeSettings.h"
#include "CrowdyServerObjectDefinition.h"
#include "CrowdyStudioModule.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Style/CrowdyStudioStyle.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

#define LOCTEXT_NAMESPACE "CrowdyServerComputeService"

namespace CrowdyServerComputeServiceDetail
{
	using CrowdyExecRevisions::EChange;
	using CrowdyExecRevisions::FTypeChange;

	TSharedPtr<FCrowdyServerComputeService> Instance;
	const TArray<CrowdyExecRevisions::FRevision> NoRevisions;
	constexpr float RefreshDelaySeconds = 0.25f;
	/** Coming back to the editor re-reads the types, for code edited elsewhere, but not more often than this. */
	constexpr double ActivationRefreshSeconds = 2.0;

	FText VersionNumber(int32 Version)
	{
		return FText::AsNumber(Version, &FNumberFormattingOptions::DefaultNoGrouping());
	}

	FText ErrorText(const FText& What, const CrowdyExecDeveloper::FError& Error)
	{
		return FText::Format(LOCTEXT("ErrorLine", "{0}: {1}"), What, FText::FromString(Error.Message));
	}

	bool Confirm(const FText& Message, const FText& Title)
	{
		return FMessageDialog::Open(EAppMsgType::YesNo, EAppReturnType::No, Message, Title) == EAppReturnType::Yes;
	}

	TArray<const UCrowdyServerObjectDefinition*> ProjectDefinitions()
	{
		TArray<const UCrowdyServerObjectDefinition*> Definitions;
		for (const UCrowdyServerObjectDefinition* Definition : CrowdyExecDeploy::FindProjectDefinitions())
		{
			Definitions.Add(Definition);
		}
		return Definitions;
	}

	int32 CountOf(TConstArrayView<FTypeChange> Changes, EChange Change)
	{
		int32 Count = 0;
		for (const FTypeChange& Candidate : Changes)
		{
			Count += Candidate.Change == Change ? 1 : 0;
		}
		return Count;
	}

	/** "a, b, c" of the types with that change; with what differs for a changed type: "a (server code, save interval)". */
	FString NamesOf(TConstArrayView<FTypeChange> Changes, EChange Change)
	{
		TArray<FString> Names;
		for (const FTypeChange& Candidate : Changes)
		{
			if (Candidate.Change != Change)
			{
				continue;
			}
			Names.Add(Candidate.What.IsEmpty() ? Candidate.TypeName : FString::Printf(TEXT("%s (%s)"), *Candidate.TypeName, *FString::Join(Candidate.What, TEXT(", "))));
		}
		return FString::Join(Names, TEXT(", "));
	}

	void AddSentence(TArray<FText>& Lines, TConstArrayView<FTypeChange> Changes, EChange Change, const FText& Format)
	{
		const FString Names = NamesOf(Changes, Change);
		if (!Names.IsEmpty())
		{
			Lines.Add(FText::Format(Format, FText::FromString(Names)));
		}
	}

	/** Only a clean success gives way to the change summary once something changes again; a failure or a warning stays until the next deploy. */
	bool IsCleanSuccess(const FLinearColor& Color)
	{
		return Color.Equals(FCrowdyStudioStyle::Success());
	}
}

FCrowdyServerComputeService& FCrowdyServerComputeService::Get()
{
	using namespace CrowdyServerComputeServiceDetail;
	if (Instance)
	{
		return *Instance;
	}
	Instance = MakeShared<FCrowdyServerComputeService>();
	if (FSlateApplication::IsInitialized())
	{
		Instance->ActivationHandle = FSlateApplication::Get().OnApplicationActivationStateChanged().AddSP(Instance.ToSharedRef(), &FCrowdyServerComputeService::HandleActivationChanged);
	}
	return *Instance;
}

void FCrowdyServerComputeService::Shutdown()
{
	using namespace CrowdyServerComputeServiceDetail;
	if (!Instance)
	{
		return;
	}
	FTSTicker::RemoveTicker(Instance->RefreshTicker);
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().OnApplicationActivationStateChanged().Remove(Instance->ActivationHandle);
	}
	Instance.Reset();
}

bool FCrowdyServerComputeService::FClientKey::Matches(const FClientKey& Other) const
{
	return AppId == Other.AppId && Environment == Other.Environment
		&& DiscoveryUrl.Equals(Other.DiscoveryUrl, ESearchCase::CaseSensitive) && Token.Equals(Other.Token, ESearchCase::CaseSensitive);
}

FCrowdyServerComputeService::FClientKey FCrowdyServerComputeService::ReadClientKey()
{
	const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>();
	FClientKey Key;
	Key.DiscoveryUrl = Settings->GetDiscoveryUrl();
	Key.Token = CrowdyStudioAuth::GetSignedInToken();
	Key.AppId = Settings->AppID;
	Key.Environment = Settings->Environment;
	return Key;
}

bool FCrowdyServerComputeService::RenewClientIfStale()
{
	if (IsBusy())
	{
		return false;
	}
	FClientKey Key = ReadClientKey();
	if (Client && Key.Matches(ClientKey))
	{
		return false;
	}
	ClientKey = MoveTemp(Key);
	Client = CrowdyExecDeveloper::FClient::Create(ClientError);
	ResetAppState();
	Refresh();
	ChangedEvent.Broadcast();
	return true;
}

FText FCrowdyServerComputeService::GetAppTarget() const
{
	return FText::Format(LOCTEXT("AppTarget", "App {0} on {1}"),
		FText::AsNumber(ClientKey.AppId, &FNumberFormattingOptions::DefaultNoGrouping()), UEnum::GetDisplayValueAsText(ClientKey.Environment));
}

void FCrowdyServerComputeService::ResetAppState()
{
	// Answers still on their way were asked for the previous app, so they are dropped.
	++StatusRequest;
	++VersionsRequest;
	Status.Reset();
	Versions.Reset();
	StatusError = FText::GetEmpty();
	VersionsError = FText::GetEmpty();
	BuildLog.Reset();
	DeployMessage = FText::GetEmpty();
	DeployColor = FLinearColor::White;
	bCountDeploySeconds = false;
	bVersionsLoaded = false;
	bLastBuildFailed = false;
	Recompare();
}

void FCrowdyServerComputeService::Refresh()
{
	RefreshTypes();
	RefreshStatus();
	RefreshVersions();
}

void FCrowdyServerComputeService::RefreshTypes()
{
	const TArray<const UCrowdyServerObjectDefinition*> Found = CrowdyServerComputeServiceDetail::ProjectDefinitions();
	Definitions.Reset();
	CrateDirectories.Reset();
	for (const UCrowdyServerObjectDefinition* Definition : Found)
	{
		Definitions.Add(Definition);
		CrateDirectories.Add(Definition->TypeName, CrowdyExecCodegen::GetCrateDirectory(*Definition));
	}
	Project = CrowdyExecDeploy::AssembleProject(Found, [](const UCrowdyServerObjectDefinition& Definition)
	{
		return CrowdyExecCodegen::GetCrateDirectory(Definition);
	});
	TMap<FString, FHistory> Kept = MoveTemp(Histories);
	Histories.Reset();
	for (const TPair<FString, FString>& Crate : CrateDirectories)
	{
		const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*CrowdyExecRevisions::GetHistoryPath(Crate.Value));
		FHistory* Old = Kept.Find(Crate.Key);
		if (Old && Old->Directory.Equals(Crate.Value, ESearchCase::CaseSensitive) && Old->Stamp == Stamp)
		{
			Histories.Add(Crate.Key, MoveTemp(*Old));
			continue;
		}
		FHistory& History = Histories.Add(Crate.Key);
		History.Directory = Crate.Value;
		History.Stamp = Stamp;
		FString Error;
		const bool bLoaded = CrowdyExecRevisions::LoadHistory(Crate.Value, History.Revisions, Error);
		UE_CLOG(!bLoaded, LogCrowdyExec, Warning, TEXT("Server Compute: %s: %s"), *Crate.Key, *Error);
	}
	TypesReadSeconds = FPlatformTime::Seconds();
	Recompare();
	ChangedEvent.Broadcast();
}

void FCrowdyServerComputeService::RequestRefreshTypes()
{
	if (RefreshTicker.IsValid())
	{
		return;
	}
	RefreshTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateSP(this, &FCrowdyServerComputeService::HandleDeferredRefresh),
		CrowdyServerComputeServiceDetail::RefreshDelaySeconds);
}

bool FCrowdyServerComputeService::HandleDeferredRefresh(float DeltaTime)
{
	RefreshTicker.Reset();
	RefreshTypes();
	return false;
}

void FCrowdyServerComputeService::HandleActivationChanged(bool bActive)
{
	if (bActive && FPlatformTime::Seconds() - TypesReadSeconds >= CrowdyServerComputeServiceDetail::ActivationRefreshSeconds)
	{
		RequestRefreshTypes();
	}
}

void FCrowdyServerComputeService::RefreshStatus()
{
	if (!Client)
	{
		return;
	}
	const int32 Request = ++StatusRequest;
	TWeakPtr<FCrowdyServerComputeService> WeakThis = AsShared();
	Client->Status([WeakThis, Request](const CrowdyExecDeveloper::TResult<CrowdyExecDeveloper::FAppStatus>& Result)
	{
		const TSharedPtr<FCrowdyServerComputeService> Self = WeakThis.Pin();
		if (!Self || Request != Self->StatusRequest)
		{
			return;
		}
		++Self->DataGeneration;
		if (!Result.bOk)
		{
			Self->StatusError = CrowdyServerComputeServiceDetail::ErrorText(LOCTEXT("StatusFailed", "Could not read the app's status"), Result.Error);
			Self->ChangedEvent.Broadcast();
			return;
		}
		Self->StatusError = FText::GetEmpty();
		Self->Status = Result.Value;
		Self->ChangedEvent.Broadcast();
	});
}

void FCrowdyServerComputeService::RefreshVersions()
{
	if (!Client)
	{
		return;
	}
	const int32 Request = ++VersionsRequest;
	TWeakPtr<FCrowdyServerComputeService> WeakThis = AsShared();
	Client->Versions([WeakThis, Request](const CrowdyExecDeveloper::TResult<TArray<CrowdyExecDeveloper::FVersion>>& Result)
	{
		const TSharedPtr<FCrowdyServerComputeService> Self = WeakThis.Pin();
		if (!Self || Request != Self->VersionsRequest)
		{
			return;
		}
		if (!Result.bOk)
		{
			Self->VersionsError = CrowdyServerComputeServiceDetail::ErrorText(LOCTEXT("VersionsFailed", "Could not read the versions"), Result.Error);
			Self->Recompare();
			Self->ChangedEvent.Broadcast();
			return;
		}
		Self->VersionsError = FText::GetEmpty();
		Self->Versions = Result.Value;
		Self->bVersionsLoaded = true;
		Self->Recompare();
		Self->ChangedEvent.Broadcast();
	});
}

void FCrowdyServerComputeService::ApplyStatus(const CrowdyExecDeveloper::FAppStatus& NewStatus)
{
	// This answer is the newest, so a status read still on its way would only put an older one back.
	++StatusRequest;
	++DataGeneration;
	Status = NewStatus;
	StatusError = FText::GetEmpty();
	RefreshVersions();
	ChangedEvent.Broadcast();
}

const CrowdyExecDeveloper::FVersion* FCrowdyServerComputeService::GetLiveVersion() const
{
	if (!bVersionsLoaded)
	{
		return nullptr;
	}
	return Versions.FindByPredicate([](const CrowdyExecDeveloper::FVersion& Version) { return Version.bActive; });
}

const CrowdyExecRevisions::FTypeChange* FCrowdyServerComputeService::FindChange(const FString& TypeName) const
{
	return Changes.FindByPredicate([&TypeName](const CrowdyExecRevisions::FTypeChange& Change) { return Change.TypeName.Equals(TypeName, ESearchCase::CaseSensitive); });
}

const TArray<CrowdyExecRevisions::FRevision>& FCrowdyServerComputeService::GetHistory(const FString& TypeName) const
{
	const FHistory* History = Histories.Find(TypeName);
	return History ? History->Revisions : CrowdyServerComputeServiceDetail::NoRevisions;
}

FString FCrowdyServerComputeService::GetCrateDirectory(const FString& TypeName) const
{
	return CrateDirectories.FindRef(TypeName);
}

bool FCrowdyServerComputeService::IsSharedTypeName(const FString& TypeName) const
{
	int32 Count = 0;
	for (const TWeakObjectPtr<const UCrowdyServerObjectDefinition>& Definition : Definitions)
	{
		Count += Definition.IsValid() && Definition->TypeName.Equals(TypeName, ESearchCase::IgnoreCase) ? 1 : 0;
	}
	return Count > 1;
}

bool FCrowdyServerComputeService::TryBeginAction()
{
	if (IsBusy())
	{
		return false;
	}
	bActionBusy = true;
	ChangedEvent.Broadcast();
	return true;
}

void FCrowdyServerComputeService::EndAction()
{
	bActionBusy = false;
	ChangedEvent.Broadcast();
}

void FCrowdyServerComputeService::Recompare()
{
	++DataGeneration;
	const bool bLiveKnown = bVersionsLoaded && VersionsError.IsEmpty();
	TArray<const UCrowdyServerObjectDefinition*> Known;
	for (const TWeakObjectPtr<const UCrowdyServerObjectDefinition>& Definition : Definitions)
	{
		if (const UCrowdyServerObjectDefinition* Loaded = Definition.Get())
		{
			Known.Add(Loaded);
		}
	}
	Changes = CrowdyExecRevisions::Compare(Project, Known, GetLiveVersion(), bLiveKnown,
		[this](const FString& TypeName) { return GetHistory(TypeName); });
}

bool FCrowdyServerComputeService::CanDeploy() const
{
	const bool bLiveKnown = bVersionsLoaded && VersionsError.IsEmpty();
	return !IsBusy() && Client && Project.Problems.IsEmpty() && (CrowdyExecRevisions::HasChanges(Changes) || !bLiveKnown);
}

bool FCrowdyServerComputeService::Deploy()
{
	using namespace CrowdyServerComputeServiceDetail;
	if (IsBusy() || RenewClientIfStale())
	{
		return false;
	}
	RefreshTypes();
	if (!CanDeploy())
	{
		return false;
	}
	TArray<FText> Lines;
	Lines.Add(FText::Format(LOCTEXT("ConfirmDeploy", "Deploying replaces the active server code of {1} with this project's {0} Server Object {0}|plural(one=type,other=types)."),
		Project.Types.Num(), GetAppTarget()));
	AddSentence(Lines, Changes, EChange::New, LOCTEXT("ConfirmAdds", "It adds {0}."));
	AddSentence(Lines, Changes, EChange::Changed, LOCTEXT("ConfirmChanges", "It changes {0}."));
	AddSentence(Lines, Changes, EChange::Unknown, LOCTEXT("ConfirmReplacesUnknown", "It replaces live code this project has no record of in {0}."));
	AddSentence(Lines, Changes, EChange::Removed, LOCTEXT("ConfirmRemoves", "It removes {0}, which the project no longer has; its Server Objects stop."));
	if (!CrowdyExecRevisions::HasChanges(Changes))
	{
		Lines.Add(LOCTEXT("ConfirmNotKnown", "What the app runs now could not be read, so what this changes is not known."));
	}
	const TArray<FString> Unsaved = CrowdyServerCodeEdits::UnsavedFiles();
	if (!Unsaved.IsEmpty())
	{
		TArray<FString> Names;
		for (const FString& Path : Unsaved)
		{
			Names.Add(FPaths::GetCleanFilename(Path));
		}
		Lines.Add(FText::Format(LOCTEXT("ConfirmDeployUnsaved", "{0} Server Code {0}|plural(one=file has,other=files have) unsaved edits; Deploy sends the saved files.\n{1}"),
			Unsaved.Num(), FText::FromString(FString::Join(Names, TEXT("\n")))));
	}
	if (!Confirm(FText::Join(FText::FromString(TEXT("\n\n")), Lines), LOCTEXT("ConfirmDeployTitle", "Deploy")))
	{
		return false;
	}
	Sending = Project;
	SendingDirectories = CrateDirectories;
	StartBuild(Project.Crates, Project.ManifestJson, true);
	return true;
}

bool FCrowdyServerComputeService::DeployStarters()
{
	using namespace CrowdyServerComputeServiceDetail;
	if (IsBusy() || RenewClientIfStale() || !Client)
	{
		return false;
	}
	if (!Confirm(FText::Format(LOCTEXT("ConfirmStarters", "Deploying the starter pack replaces the active server code of {0} with the platform's example Server Object types."), GetAppTarget()),
		LOCTEXT("ConfirmStartersTitle", "Deploy starter pack")))
	{
		return false;
	}
	bBusy = true;
	BuildLog.Reset();
	bLastBuildFailed = false;
	SetDeployMessage(LOCTEXT("FetchingStarters", "Fetching the starter pack..."), FCrowdyStudioStyle::TextSecondary());
	TWeakPtr<FCrowdyServerComputeService> WeakThis = AsShared();
	Client->Starters([WeakThis](const CrowdyExecDeveloper::TResult<CrowdyExecDeveloper::FStarterPack>& Result)
	{
		const TSharedPtr<FCrowdyServerComputeService> Self = WeakThis.Pin();
		if (!Self)
		{
			return;
		}
		if (!Result.bOk)
		{
			Self->FinishDeploy(ErrorText(LOCTEXT("StartersFailed", "Could not fetch the starter pack"), Result.Error), FCrowdyStudioStyle::Danger());
			return;
		}
		TArray<CrowdyExecDeveloper::FBuildCrate> Crates;
		for (const CrowdyExecDeveloper::FStarter& Starter : Result.Value.Starters)
		{
			Crates.Add(Starter.Crate);
		}
		Self->StartBuild(Crates, Result.Value.ManifestJson, false);
	});
	return true;
}

void FCrowdyServerComputeService::StartBuild(const TArray<CrowdyExecDeveloper::FBuildCrate>& Crates, const FString& ManifestJson, bool bRecord)
{
	using namespace CrowdyServerComputeServiceDetail;
	bBusy = true;
	BuildStartSeconds = FPlatformTime::Seconds();
	BuildLog.Reset();
	bLastBuildFailed = false;
	SetDeployMessage(LOCTEXT("SendingBuild", "Sending the server code..."), FCrowdyStudioStyle::TextSecondary());
	TWeakPtr<FCrowdyServerComputeService> WeakThis = AsShared();
	Client->Build(Crates, [WeakThis, ManifestJson, bRecord](const CrowdyExecDeveloper::TResult<CrowdyExecDeveloper::FBuild>& Result)
	{
		const TSharedPtr<FCrowdyServerComputeService> Self = WeakThis.Pin();
		if (!Self)
		{
			return;
		}
		if (!Result.bOk)
		{
			Self->FinishDeploy(ErrorText(LOCTEXT("BuildFailedToStart", "The build could not start"), Result.Error), FCrowdyStudioStyle::Danger());
			return;
		}
		Self->SetDeployMessage(LOCTEXT("Building", "Building on the server... {0} s"), FCrowdyStudioStyle::TextSecondary(), true);
		Self->Client->WaitForBuild(Result.Value.BuildId,
			[WeakThis, ManifestJson, bRecord](const CrowdyExecDeveloper::TResult<CrowdyExecDeveloper::FBuild>& Finished)
			{
				if (const TSharedPtr<FCrowdyServerComputeService> Service = WeakThis.Pin())
				{
					Service->OnBuildFinished(Finished, ManifestJson, bRecord);
				}
			},
			[WeakThis](const CrowdyExecDeveloper::FBuild& Build)
			{
				const TSharedPtr<FCrowdyServerComputeService> Service = WeakThis.Pin();
				if (!Service)
				{
					return;
				}
				const FText Progress = Build.Status == TEXT("queued") ? LOCTEXT("BuildQueued", "Waiting for a builder... {0} s") : LOCTEXT("BuildRunning", "Building on the server... {0} s");
				Service->SetDeployMessage(Progress, FCrowdyStudioStyle::TextSecondary(), true);
			});
	});
}

void FCrowdyServerComputeService::OnBuildFinished(const CrowdyExecDeveloper::TResult<CrowdyExecDeveloper::FBuild>& Result, const FString& ManifestJson, bool bRecord)
{
	using namespace CrowdyServerComputeServiceDetail;
	if (!Result.bOk)
	{
		FinishDeploy(ErrorText(LOCTEXT("BuildLost", "Could not follow the build"), Result.Error), FCrowdyStudioStyle::Danger());
		return;
	}
	BuildLog = Result.Value.Log;
	bLastBuildFailed = !Result.Value.IsSucceeded();
	if (bLastBuildFailed)
	{
		FinishDeploy(LOCTEXT("BuildFailed", "The build failed. See the build output below."), FCrowdyStudioStyle::Danger());
		return;
	}

	SetDeployMessage(LOCTEXT("Deploying", "Deploying..."), FCrowdyStudioStyle::TextSecondary());
	const TArray<CrowdyExecDeveloper::FBuildArtifact> Artifacts = Result.Value.Artifacts;
	TWeakPtr<FCrowdyServerComputeService> WeakThis = AsShared();
	Client->Deploy(ManifestJson, Result.Value.BuildId, [WeakThis, Artifacts, bRecord](const CrowdyExecDeveloper::TResult<int32>& Deployed)
	{
		const TSharedPtr<FCrowdyServerComputeService> Self = WeakThis.Pin();
		if (!Self)
		{
			return;
		}
		if (!Deployed.bOk)
		{
			Self->FinishDeploy(ErrorText(LOCTEXT("DeployFailed", "Built, but the deploy failed"), Deployed.Error), FCrowdyStudioStyle::Danger());
			return;
		}
		TArray<FString> RecordErrors;
		if (bRecord)
		{
			CrowdyExecRevisions::RecordDeploy(Self->Sending, Artifacts, Deployed.Value, FDateTime::UtcNow(), GetDefault<UCrowdyServerComputeSettings>()->GetRevisionsToKeep(),
				[&Self](const FString& TypeName) { return Self->SendingDirectories.FindRef(TypeName); }, RecordErrors);
		}
		Self->Sending = CrowdyExecDeploy::FProjectDeploy();
		Self->SendingDirectories.Reset();
		const FText DeployedLine = FText::Format(LOCTEXT("Deployed", "Deployed as version {0}."), VersionNumber(Deployed.Value));
		if (RecordErrors.IsEmpty())
		{
			Self->FinishDeploy(DeployedLine, FCrowdyStudioStyle::Success());
		}
		else
		{
			Self->FinishDeploy(FText::Format(LOCTEXT("DeployedNotRecorded", "{0} Its record could not be saved, so its changes cannot be compared: {1}"),
				DeployedLine, FText::FromString(FString::Join(RecordErrors, TEXT("; ")))), FCrowdyStudioStyle::Warning());
		}
		Self->RefreshTypes();
		Self->RefreshStatus();
		Self->RefreshVersions();
	});
}

void FCrowdyServerComputeService::SetDeployMessage(const FText& Message, const FLinearColor& Color, bool bCountSeconds)
{
	DeployMessage = Message;
	DeployColor = Color;
	bCountDeploySeconds = bCountSeconds;
	ChangedEvent.Broadcast();
}

void FCrowdyServerComputeService::FinishDeploy(const FText& Message, const FLinearColor& Color)
{
	bBusy = false;
	SetDeployMessage(Message, Color);
}

FText FCrowdyServerComputeService::GetDeployLine() const
{
	if (bBusy)
	{
		return bCountDeploySeconds ? FText::Format(DeployMessage, FMath::RoundToInt(FPlatformTime::Seconds() - BuildStartSeconds)) : DeployMessage;
	}
	if (!Client)
	{
		return FText::Format(LOCTEXT("NoClient", "{0} Sign in and choose an app on Crowdy Studio's Project page."), FText::FromString(ClientError));
	}
	if (!Project.Problems.IsEmpty())
	{
		return GetReadinessHint();
	}
	if (!DeployMessage.IsEmpty() && !(CrowdyServerComputeServiceDetail::IsCleanSuccess(DeployColor) && CrowdyExecRevisions::HasChanges(Changes)))
	{
		return DeployMessage;
	}
	return GetChangeSummary();
}

FLinearColor FCrowdyServerComputeService::GetDeployLineColor() const
{
	if (bBusy)
	{
		return DeployColor;
	}
	if (!Client || !Project.Problems.IsEmpty())
	{
		return FCrowdyStudioStyle::Warning();
	}
	if (!DeployMessage.IsEmpty() && !(CrowdyServerComputeServiceDetail::IsCleanSuccess(DeployColor) && CrowdyExecRevisions::HasChanges(Changes)))
	{
		return DeployColor;
	}
	return FCrowdyStudioStyle::TextSecondary();
}

FText FCrowdyServerComputeService::GetReadinessHint() const
{
	if (Project.Types.IsEmpty())
	{
		return LOCTEXT("HintNoTypes", "Create a Server Object Definition asset to deploy.");
	}
	int32 NeedsCode = 0;
	int32 Broken = 0;
	for (const CrowdyExecDeploy::FTypeState& Type : Project.Types)
	{
		NeedsCode += (Type.State == CrowdyExecDeploy::ECrateState::NotGenerated || Type.State == CrowdyExecDeploy::ECrateState::OutOfDate) ? 1 : 0;
		Broken += Type.State == CrowdyExecDeploy::ECrateState::Invalid ? 1 : 0;
	}
	if (Broken > 0)
	{
		return FText::Format(LOCTEXT("HintBroken", "Fix the {0} {0}|plural(one=type,other=types) marked Problem first."), Broken);
	}
	if (NeedsCode > 0)
	{
		return FText::Format(LOCTEXT("HintGenerate", "Generate Server Code for {0} {0}|plural(one=type,other=types) first (right-click the definition in the Content Browser)."), NeedsCode);
	}
	return FText::FromString(Project.Problems[0]);
}

FText FCrowdyServerComputeService::GetChangeSummary() const
{
	using namespace CrowdyServerComputeServiceDetail;
	const int32 Sent = Project.Types.Num();
	if (!bVersionsLoaded || !VersionsError.IsEmpty())
	{
		return FText::Format(LOCTEXT("ReadyToDeploy", "Ready to deploy {0} Server Object {0}|plural(one=type,other=types)."), Sent);
	}
	const CrowdyExecDeveloper::FVersion* Live = GetLiveVersion();
	if (!Live)
	{
		return FText::Format(LOCTEXT("NothingLive", "Nothing is deployed yet. Deploying sends the project's {0} {0}|plural(one=type,other=types)."), Sent);
	}
	if (!CrowdyExecRevisions::HasChanges(Changes))
	{
		return FText::Format(LOCTEXT("NothingChanged", "Nothing changed since version {0}."), VersionNumber(Live->Version));
	}
	TArray<FText> Parts;
	const int32 ChangedCount = CountOf(Changes, EChange::Changed);
	const int32 NewCount = CountOf(Changes, EChange::New);
	const int32 UnknownCount = CountOf(Changes, EChange::Unknown);
	const int32 RemovedCount = CountOf(Changes, EChange::Removed);
	if (ChangedCount > 0)
	{
		Parts.Add(FText::Format(LOCTEXT("PartChanged", "{0} changed"), ChangedCount));
	}
	if (NewCount > 0)
	{
		Parts.Add(FText::Format(LOCTEXT("PartNew", "{0} new"), NewCount));
	}
	if (UnknownCount > 0)
	{
		Parts.Add(FText::Format(LOCTEXT("PartUnknown", "{0} running code not recorded here"), UnknownCount));
	}
	if (RemovedCount > 0)
	{
		Parts.Add(FText::Format(LOCTEXT("PartRemoved", "{0} removed"), RemovedCount));
	}
	return FText::Format(LOCTEXT("ChangeSummary", "Since version {0}: {1}. Deploying sends the project's {2} {2}|plural(one=type,other=types)."),
		VersionNumber(Live->Version), FText::Join(FText::FromString(TEXT(", ")), Parts), Sent);
}

#undef LOCTEXT_NAMESPACE
