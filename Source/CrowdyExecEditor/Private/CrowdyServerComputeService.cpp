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
#include "ProfilingDebugging/CpuProfilerTrace.h"
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

TArray<FString> CrowdyServerComputeMove::TypesOnPreviousVersion(TConstArrayView<CrowdyExecDeveloper::FInstance> Before, TConstArrayView<CrowdyExecDeveloper::FInstance> After,
	TConstArrayView<FString> ChangedTypes, int32& OutRunning)
{
	const auto PlacementOf = [](const CrowdyExecDeveloper::FInstance& Instance)
	{
		return FString::Printf(TEXT("%s\n%s\n%lld"), *Instance.NodeType, *Instance.Key, Instance.Epoch);
	};
	TSet<FString> Placed;
	for (const CrowdyExecDeveloper::FInstance& Instance : Before)
	{
		Placed.Add(PlacementOf(Instance));
	}
	OutRunning = 0;
	TArray<FString> Types;
	for (const CrowdyExecDeveloper::FInstance& Instance : After)
	{
		if (Instance.Phase != TEXT("running") || !ChangedTypes.Contains(Instance.NodeType) || !Placed.Contains(PlacementOf(Instance)))
		{
			continue;
		}
		Types.AddUnique(Instance.NodeType);
		++OutRunning;
	}
	return Types;
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
	++InstancesRequest;
	Status.Reset();
	Versions.Reset();
	Instances.Reset();
	++MoveRequest;
	TypesToMove.Reset();
	RunningToMove = 0;
	bInstancesLoaded = false;
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
	RefreshInstances();
}

void FCrowdyServerComputeService::RefreshInstances()
{
	if (!Client)
	{
		return;
	}
	const int32 Request = ++InstancesRequest;
	TWeakPtr<FCrowdyServerComputeService> WeakThis = AsShared();
	Client->Instances([WeakThis, Request](const CrowdyExecDeveloper::TResult<TArray<CrowdyExecDeveloper::FInstance>>& Result)
	{
		const TSharedPtr<FCrowdyServerComputeService> Self = WeakThis.Pin();
		// A failed read keeps the last one: instances are a hint beside each type, never a reason to block anything.
		if (!Self || Request != Self->InstancesRequest || !Result.bOk)
		{
			return;
		}
		++Self->DataGeneration;
		Self->Instances = Result.Value;
		Self->bInstancesLoaded = true;
		Self->ChangedEvent.Broadcast();
	});
}

int32 FCrowdyServerComputeService::CountRunning(const FString& TypeName) const
{
	int32 Count = 0;
	for (const CrowdyExecDeveloper::FInstance& Instance : Instances)
	{
		const bool bLive = Instance.Phase == TEXT("running") || Instance.Phase == TEXT("starting");
		Count += bLive && Instance.NodeType.Equals(TypeName, ESearchCase::CaseSensitive) ? 1 : 0;
	}
	return Count;
}

bool FCrowdyServerComputeService::RestartType(const FString& TypeName)
{
	using namespace CrowdyServerComputeServiceDetail;
	if (IsBusy() || !Client)
	{
		return false;
	}
	const CrowdyExecDeveloper::FVersion* Live = GetLiveVersion();
	const FText Version = Live ? FText::Format(LOCTEXT("RestartToVersion", "version {0}"), VersionNumber(Live->Version)) : LOCTEXT("RestartToActive", "the active version");
	const FText Message = FText::Format(LOCTEXT("ConfirmRestart", "Move {0}'s running Server Objects on {1} to {2} now?\n\nEach is saved and stopped, and starts again on {2} at its next call. Players watching one see it read again; a call that arrives while it stops may be answered Unavailable."),
		FText::FromString(TypeName), GetAppTarget(), Version);
	if (!Confirm(Message, LOCTEXT("ConfirmRestartTitle", "Restart")))
	{
		return false;
	}
	return RestartTypes({TypeName});
}

void FCrowdyServerComputeService::MoveRunningToActive()
{
	if (IsBusy() || TypesToMove.IsEmpty())
	{
		return;
	}
	TArray<FString> Types = MoveTemp(TypesToMove);
	DismissMove();
	RestartTypes(MoveTemp(Types));
}

void FCrowdyServerComputeService::DismissMove()
{
	++MoveRequest;
	TypesToMove.Reset();
	RunningToMove = 0;
	ChangedEvent.Broadcast();
}

bool FCrowdyServerComputeService::RestartTypes(TArray<FString> TypeNames)
{
	using namespace CrowdyServerComputeServiceDetail;
	if (TypeNames.IsEmpty() || !TryBeginAction())
	{
		return false;
	}
	struct FRestarts
	{
		int32 Waiting = 0;
		int32 Stopped = 0;
		TArray<FString> Failed;
	};
	const TSharedRef<FRestarts> Restarts = MakeShared<FRestarts>();
	Restarts->Waiting = TypeNames.Num();
	SetDeployMessage(LOCTEXT("Restarting", "Moving running Server Objects to the active version..."), FCrowdyStudioStyle::TextSecondary());
	TWeakPtr<FCrowdyServerComputeService> WeakThis = AsShared();
	for (const FString& TypeName : TypeNames)
	{
		Client->RestartType(TypeName, [WeakThis, Restarts, TypeName](const CrowdyExecDeveloper::TResult<CrowdyExecDeveloper::FRestart>& Result)
		{
			Restarts->Stopped += Result.bOk ? Result.Value.Stopped : 0;
			if (!Result.bOk)
			{
				Restarts->Failed.Add(FString::Printf(TEXT("%s: %s"), *TypeName, *Result.Error.Message));
			}
			const TSharedPtr<FCrowdyServerComputeService> Self = WeakThis.Pin();
			if (--Restarts->Waiting > 0 || !Self)
			{
				return;
			}
			Self->EndAction();
			Self->RefreshInstances();
			if (!Restarts->Failed.IsEmpty())
			{
				Self->SetDeployMessage(FText::Format(LOCTEXT("RestartFailed", "Could not move every type's running Server Objects: {0}"),
					FText::FromString(FString::Join(Restarts->Failed, TEXT("; ")))), FCrowdyStudioStyle::Danger());
				return;
			}
			Self->SetDeployMessage(FText::Format(LOCTEXT("Restarted", "{0} running Server {0}|plural(one=Object,other=Objects) stopped; each starts again on the active version at its next call."),
				Restarts->Stopped), FCrowdyStudioStyle::Success());
		});
	}
	return true;
}

void FCrowdyServerComputeService::OfferToMove(TArray<FString> ChangedTypes, TSharedRef<FDeploySnapshot> Before)
{
	if (!Client || ChangedTypes.IsEmpty())
	{
		return;
	}
	TWeakPtr<FCrowdyServerComputeService> WeakThis = AsShared();
	const int32 Request = ++MoveRequest;
	if (!Before->bAnswered)
	{
		Before->OnAnswered = [WeakThis, Request, ChangedTypes = MoveTemp(ChangedTypes), Before]()
		{
			const TSharedPtr<FCrowdyServerComputeService> Self = WeakThis.Pin();
			if (Self && Request == Self->MoveRequest)
			{
				Self->OfferToMove(ChangedTypes, Before);
			}
		};
		return;
	}
	Client->Instances([WeakThis, Request, ChangedTypes = MoveTemp(ChangedTypes), Before](const CrowdyExecDeveloper::TResult<TArray<CrowdyExecDeveloper::FInstance>>& Result)
	{
		const TSharedPtr<FCrowdyServerComputeService> Self = WeakThis.Pin();
		if (!Self || Request != Self->MoveRequest || !Result.bOk)
		{
			return;
		}
		// Unknown instances from before the deploy: every running one of a changed type is offered rather than none.
		Self->TypesToMove = CrowdyServerComputeMove::TypesOnPreviousVersion(Before->bKnown ? Before->Instances : Result.Value, Result.Value, ChangedTypes, Self->RunningToMove);
		++Self->DataGeneration;
		Self->ChangedEvent.Broadcast();
	});
}

void FCrowdyServerComputeService::RefreshTypes()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FCrowdyServerComputeService::RefreshTypes);
	const double StartSeconds = FPlatformTime::Seconds();
	const TArray<const UCrowdyServerObjectDefinition*> Found = CrowdyServerComputeServiceDetail::ProjectDefinitions();
	Definitions.Reset();
	CrateDirectories.Reset();
	for (const UCrowdyServerObjectDefinition* Definition : Found)
	{
		Definitions.Add(Definition);
		CrateDirectories.Add(Definition->TypeName, CrowdyExecCodegen::GetCrateDirectory(*Definition));
	}
	// The SDK's root has no definition in the project; its history sits beside the project's crates.
	CrateDirectories.Add(CrowdyExecDeploy::RootTypeName, FPaths::Combine(CrowdyExecCodegen::GetServerDirectory(), CrowdyExecDeploy::RootTypeName));
	const double FoundSeconds = FPlatformTime::Seconds();
	Project = CrowdyExecDeploy::AssembleProject(Found, [](const UCrowdyServerObjectDefinition& Definition)
	{
		return CrowdyExecCodegen::GetCrateDirectory(Definition);
	});
	const double AssembledSeconds = FPlatformTime::Seconds();
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
	UE_CLOG(CrowdyExecTrace::Exec(), LogCrowdyExec, Log, TEXT("exec: Server Compute read %d types in %.1f ms (find %.1f ms, assemble %.1f ms, histories %.1f ms)"), Found.Num(),
		(TypesReadSeconds - StartSeconds) * 1000.0, (FoundSeconds - StartSeconds) * 1000.0, (AssembledSeconds - FoundSeconds) * 1000.0, (TypesReadSeconds - AssembledSeconds) * 1000.0);
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
	// A rollback or a switch changes what "the new version" is, so an offer to move onto it no longer holds.
	++MoveRequest;
	TypesToMove.Reset();
	RunningToMove = 0;
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
	if (Project.Types.IsEmpty())
	{
		Lines.Add(FText::Format(LOCTEXT("ConfirmDeployRootOnly", "Deploying replaces the active server code of {0}. This project has no Server Object types, so only the Crowdy SDK's root (Players Online) is deployed."), GetAppTarget()));
	}
	else
	{
		Lines.Add(FText::Format(LOCTEXT("ConfirmDeploy", "Deploying replaces the active server code of {1} with this project's {0} Server Object {0}|plural(one=type,other=types)."),
			Project.Types.Num(), GetAppTarget()));
	}
	AddSentence(Lines, Changes, EChange::Removed, LOCTEXT("ConfirmRemoves", "It removes {0}, which this project does not have; their Server Objects stop. Their saved state is kept and is used again if you deploy them again."));
	AddSentence(Lines, Changes, EChange::New, LOCTEXT("ConfirmAdds", "It adds {0}."));
	AddSentence(Lines, Changes, EChange::Changed, LOCTEXT("ConfirmChanges", "It changes {0}."));
	AddSentence(Lines, Changes, EChange::Unknown, LOCTEXT("ConfirmReplacesUnknown", "It replaces live code this project has no record of in {0}."));
	for (const FTypeChange& Change : Changes)
	{
		const FString KindSentence = CrowdyExecRevisions::DescribeKindChange(Change);
		if (!KindSentence.IsEmpty())
		{
			Lines.Add(FText::FromString(KindSentence));
		}
	}
	if (!CrowdyExecRevisions::HasChanges(Changes))
	{
		Lines.Add(LOCTEXT("ConfirmNotKnown", "What the app runs now could not be read, so what this changes is not known."));
	}
	const bool bLiveKnown = bVersionsLoaded && VersionsError.IsEmpty();
	CrowdyExecBuildAndDeploy::FPlan Plan = CrowdyExecBuildAndDeploy::MakePlan(Project.Crates,
		bLiveKnown ? CrowdyExecRevisions::UploadedDigests(Versions) : TSet<FString>(), [this](const FString& CrateName) { return GetHistory(CrateName); });
	if (!Plan.Reused.IsEmpty())
	{
		TArray<FString> Names;
		Plan.Reused.GetKeys(Names);
		Names.Sort();
		Lines.Add(FText::Format(LOCTEXT("ConfirmReused", "The server code of {0} is as it was deployed before, so it is not built again."), FText::FromString(FString::Join(Names, TEXT(", ")))));
	}
	if (Plan.Builds.Num() > 1)
	{
		Lines.Add(FText::Format(LOCTEXT("ConfirmSplit", "The server code is built in {0} parts. Each part but the last is first stored by a version that runs what runs now, so the version history gains {1} more {1}|plural(one=version,other=versions)."),
			Plan.Builds.Num(), Plan.Builds.Num() - 1));
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
	if (!Project.Warnings.IsEmpty())
	{
		Lines.Add(FText::Format(LOCTEXT("ConfirmDeployWarnings", "Check before deploying:\n{0}"), FText::FromString(FString::Join(Project.Warnings, TEXT("\n")))));
	}
	if (!Confirm(FText::Join(FText::FromString(TEXT("\n\n")), Lines), LOCTEXT("ConfirmDeployTitle", "Deploy")))
	{
		return false;
	}
	Sending = Project;
	SendingDirectories = CrateDirectories;
	SendingChanged.Reset();
	for (const FTypeChange& Change : Changes)
	{
		if (Change.Change == EChange::Changed || Change.Change == EChange::Unknown)
		{
			SendingChanged.Add(Change.TypeName);
		}
	}
	if (!StartDeploy(MoveTemp(Plan), Project.ManifestJson, true))
	{
		Sending = CrowdyExecDeploy::FProjectDeploy();
		SendingDirectories.Reset();
		SendingChanged.Reset();
		return false;
	}
	return true;
}

bool FCrowdyServerComputeService::DeployStarters()
{
	using namespace CrowdyServerComputeServiceDetail;
	if (IsBusy() || RenewClientIfStale() || !Client)
	{
		return false;
	}
	TArray<FText> Lines;
	Lines.Add(FText::Format(LOCTEXT("ConfirmStarters", "Deploying the starter pack replaces the active server code of {0} with the platform's example Server Object types."), GetAppTarget()));
	const bool bLiveKnown = bVersionsLoaded && VersionsError.IsEmpty();
	const CrowdyExecDeveloper::FVersion* Live = bLiveKnown ? GetLiveVersion() : nullptr;
	TArray<CrowdyExecRevisions::FLiveType> Running;
	if (Live && CrowdyExecRevisions::ParseManifest(Live->ManifestJson, Running))
	{
		Running.RemoveAll([](const CrowdyExecRevisions::FLiveType& Type) { return Type.TypeName == CrowdyExecDeploy::RootTypeName; });
		const FString Names = FString::JoinBy(Running, TEXT(", "), [](const CrowdyExecRevisions::FLiveType& Type) { return Type.TypeName; });
		if (!Names.IsEmpty())
		{
			Lines.Add(FText::Format(LOCTEXT("ConfirmStartersRunning", "It removes {0}, which the active version runs, unless the starter pack has a type of the same name; that type then takes over its saved Server Objects. Their saved state is kept and is used again if you deploy them again."), FText::FromString(Names)));
		}
	}
	else if (Live || !bLiveKnown)
	{
		Lines.Add(LOCTEXT("ConfirmStartersNotKnown", "What the app runs now could not be read, so what this removes is not known."));
	}
	if (!Confirm(FText::Join(FText::FromString(TEXT("\n\n")), Lines), LOCTEXT("ConfirmStartersTitle", "Deploy starter pack")))
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
		Self->StartDeploy(CrowdyExecBuildAndDeploy::MakePlan(Result.Value.GetCrates(), TSet<FString>(), [](const FString&) { return TArray<CrowdyExecRevisions::FRevision>(); }),
			Result.Value.ManifestJson, false);
	});
	return true;
}

TOptional<CrowdyExecBuildAndDeploy::FLive> FCrowdyServerComputeService::GetLive() const
{
	if (!bVersionsLoaded || !VersionsError.IsEmpty())
	{
		return {};
	}
	return CrowdyExecBuildAndDeploy::FindLive(Versions);
}

bool FCrowdyServerComputeService::StartDeploy(CrowdyExecBuildAndDeploy::FPlan Plan, const FString& ManifestJson, bool bRecord)
{
	using namespace CrowdyServerComputeServiceDetail;
	const TOptional<CrowdyExecBuildAndDeploy::FLive> Live = GetLive();
	const FString PlanProblem = CrowdyExecBuildAndDeploy::FindPlanProblem(Plan, ManifestJson, Live);
	if (!PlanProblem.IsEmpty())
	{
		FinishDeploy(FText::FromString(PlanProblem), FCrowdyStudioStyle::Danger());
		return false;
	}
	bBusy = true;
	++MoveRequest;
	TypesToMove.Reset();
	RunningToMove = 0;
	BuildStartSeconds = FPlatformTime::Seconds();
	BuildLog.Reset();
	bLastBuildFailed = false;
	SetDeployMessage(Plan.Builds.IsEmpty() ? LOCTEXT("Deploying", "Deploying...") : LOCTEXT("SendingBuild", "Sending the server code..."), FCrowdyStudioStyle::TextSecondary());
	TWeakPtr<FCrowdyServerComputeService> WeakThis = AsShared();
	// Asked just before the deploy and not waited for: an instance placed after it has a higher epoch.
	const TSharedRef<FDeploySnapshot> Before = MakeShared<FDeploySnapshot>();
	CrowdyExecBuildAndDeploy::FCallbacks Callbacks;
	Callbacks.OnBuild = [WeakThis](int32 Index, int32 Count, const CrowdyExecDeveloper::FBuild& Build)
	{
		const TSharedPtr<FCrowdyServerComputeService> Service = WeakThis.Pin();
		if (!Service)
		{
			return;
		}
		const bool bQueued = Build.Status == TEXT("queued");
		const FText Progress = Count < 2
			? (bQueued ? LOCTEXT("BuildQueued", "Waiting for a builder... {0} s") : LOCTEXT("BuildRunning", "Building on the server... {0} s"))
			: FText::Format(bQueued ? LOCTEXT("BuildPartQueued", "Part {1} of {2}: waiting for a builder... {0} s") : LOCTEXT("BuildPartRunning", "Part {1} of {2}: building on the server... {0} s"),
				FText::FromString(TEXT("{0}")), Index + 1, Count);
		Service->SetDeployMessage(Progress, FCrowdyStudioStyle::TextSecondary(), true);
	};
	Callbacks.OnDeploying = [WeakThis]()
	{
		if (const TSharedPtr<FCrowdyServerComputeService> Service = WeakThis.Pin())
		{
			Service->SetDeployMessage(LOCTEXT("Deploying", "Deploying..."), FCrowdyStudioStyle::TextSecondary());
		}
	};
	Callbacks.OnFinalDeploy = [WeakThis, Before]()
	{
		const TSharedPtr<FCrowdyServerComputeService> Service = WeakThis.Pin();
		if (!Service)
		{
			return;
		}
		Service->Client->Instances([Before](const CrowdyExecDeveloper::TResult<TArray<CrowdyExecDeveloper::FInstance>>& Answer)
		{
			Before->bAnswered = true;
			Before->bKnown = Answer.bOk;
			Before->Instances = Answer.bOk ? Answer.Value : TArray<CrowdyExecDeveloper::FInstance>();
			const TFunction<void()> Then = MoveTemp(Before->OnAnswered);
			if (Then)
			{
				Then();
			}
		});
	};
	Callbacks.OnDone = [WeakThis, bRecord, Before](const CrowdyExecBuildAndDeploy::FOutcome& Outcome)
	{
		if (const TSharedPtr<FCrowdyServerComputeService> Service = WeakThis.Pin())
		{
			Service->OnDeployDone(Outcome, bRecord, Before);
		}
	};
	CrowdyExecBuildAndDeploy::Run(Client.ToSharedRef(), MoveTemp(Plan), ManifestJson, Live.Get(CrowdyExecBuildAndDeploy::FLive()), MoveTemp(Callbacks));
	return true;
}

void FCrowdyServerComputeService::OnDeployDone(const CrowdyExecBuildAndDeploy::FOutcome& Outcome, bool bRecord, TSharedRef<FDeploySnapshot> Before)
{
	using namespace CrowdyServerComputeServiceDetail;
	using CrowdyExecBuildAndDeploy::EStep;
	BuildLog = Outcome.Builds.IsEmpty() ? FString() : Outcome.Builds.Last().Log;
	bLastBuildFailed = Outcome.Step == EStep::BuildFailed;
	FText Unchanged;
	if (Outcome.Step == EStep::DeployFailed && Outcome.Error.bNoAnswer)
	{
		Unchanged = LOCTEXT("DeployMayApply", " It had no answer, so it may still apply; refresh to see the active version.");
	}
	else if (Outcome.RestoredVersion > 0)
	{
		Unchanged = FText::Format(LOCTEXT("LiveRestored", " Version {0} is active again, as before the deploy."), VersionNumber(Outcome.RestoredVersion));
	}
	else if (!Outcome.StagedVersions.IsEmpty())
	{
		Unchanged = FText::Format(LOCTEXT("StagedActive", " Version {0} is active: it runs what ran before the deploy, plus stand-ins nothing calls."), VersionNumber(Outcome.StagedVersions.Last()));
	}
	switch (Outcome.Step)
	{
	case EStep::BuildNotStarted:
		FinishDeploy(ErrorText(LOCTEXT("BuildFailedToStart", "The build could not start"), Outcome.Error), FCrowdyStudioStyle::Danger());
		return;
	case EStep::BuildLost:
		FinishDeploy(ErrorText(LOCTEXT("BuildLost", "Could not follow the build"), Outcome.Error), FCrowdyStudioStyle::Danger());
		return;
	case EStep::BuildFailed:
		FinishDeploy(LOCTEXT("BuildFailed", "The build failed. See the build output below."), FCrowdyStudioStyle::Danger());
		return;
	case EStep::StagingFailed:
	case EStep::DeployFailed:
		FinishDeploy(FText::Format(LOCTEXT("DeployFailedAfter", "{0}{1}"), Outcome.Step == EStep::StagingFailed
			? ErrorText(LOCTEXT("StagingFailed", "Built, but storing the builds failed"), Outcome.Error)
			: ErrorText(LOCTEXT("DeployFailed", "Built, but the deploy failed"), Outcome.Error), Unchanged), FCrowdyStudioStyle::Danger());
		if (!Outcome.StagedVersions.IsEmpty() || Outcome.Error.bNoAnswer)
		{
			RefreshStatus();
			RefreshVersions();
		}
		return;
	case EStep::Deployed:
		break;
	}
	TArray<FString> RecordErrors;
	if (bRecord)
	{
		CrowdyExecRevisions::RecordDeploy(Sending, Outcome.Artifacts, Outcome.Version, FDateTime::UtcNow(), GetDefault<UCrowdyServerComputeSettings>()->GetRevisionsToKeep(),
			[this](const FString& TypeName) { return SendingDirectories.FindRef(TypeName); }, RecordErrors);
	}
	Sending = CrowdyExecDeploy::FProjectDeploy();
	SendingDirectories.Reset();
	const FText DeployedLine = FText::Format(LOCTEXT("Deployed", "Deployed as version {0}."), VersionNumber(Outcome.Version));
	if (RecordErrors.IsEmpty())
	{
		FinishDeploy(DeployedLine, FCrowdyStudioStyle::Success());
	}
	else
	{
		FinishDeploy(FText::Format(LOCTEXT("DeployedNotRecorded", "{0} Its record could not be saved, so its changes cannot be compared: {1}"),
			DeployedLine, FText::FromString(FString::Join(RecordErrors, TEXT("; ")))), FCrowdyStudioStyle::Warning());
	}
	TArray<FString> ChangedTypes = bRecord ? MoveTemp(SendingChanged) : TArray<FString>();
	SendingChanged.Reset();
	RefreshTypes();
	RefreshStatus();
	RefreshVersions();
	RefreshInstances();
	OfferToMove(MoveTemp(ChangedTypes), Before);
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
	const FText Sends = Sent == 0 ? LOCTEXT("SendsRootOnly", "This project has no Server Object types, so deploying sends only the Crowdy SDK's root (Players Online).")
		: FText::Format(LOCTEXT("SendsTypes", "Deploying sends the project's {0} {0}|plural(one=type,other=types)."), Sent);
	if (!bVersionsLoaded || !VersionsError.IsEmpty())
	{
		return Sent == 0 ? Sends : FText::Format(LOCTEXT("ReadyToDeploy", "Ready to deploy {0} Server Object {0}|plural(one=type,other=types)."), Sent);
	}
	const CrowdyExecDeveloper::FVersion* Live = GetLiveVersion();
	if (!Live)
	{
		return FText::Format(LOCTEXT("NothingLive", "Nothing is deployed yet. {0}"), Sends);
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
	return FText::Format(LOCTEXT("ChangeSummary", "Since version {0}: {1}. {2}"),
		VersionNumber(Live->Version), FText::Join(FText::FromString(TEXT(", ")), Parts), Sends);
}

#undef LOCTEXT_NAMESPACE
