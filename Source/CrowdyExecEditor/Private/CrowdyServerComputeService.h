#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "CrowdyExecDeploy.h"
#include "CrowdyExecDeveloperApi.h"
#include "CrowdyExecRevisions.h"

class UCrowdyServerObjectDefinition;
enum class ECrowdyEnvironment : uint8;

/** The editor's one link to Server Compute, shared by the Server Compute page and every definition asset: the app's live status and versions, what changed since, and deploys. */
class FCrowdyServerComputeService : public TSharedFromThis<FCrowdyServerComputeService>
{
public:
	static FCrowdyServerComputeService& Get();
	/** Drops the service at module shutdown; answers still on their way are ignored. */
	static void Shutdown();

	/** Rebuilds the client when the sign-in, app or backend changed; true when it did, which drops every cached answer. Never while a deploy runs. */
	bool RenewClientIfStale();
	/** The client for the signed-in developer and the project's app; null when there is none (GetClientError says why). */
	TSharedPtr<CrowdyExecDeveloper::FClient> GetClient() const { return Client; }
	const FString& GetClientError() const { return ClientError; }
	/** "App 88318987311872 on Dev", for confirmations. */
	FText GetAppTarget() const;

	/** Re-reads the project's types, then the app's status and versions. */
	void Refresh();
	/** Re-reads only the project's types and recompares them with the versions already read. Synchronous: loads every definition and reads every crate. */
	void RefreshTypes();
	/** RefreshTypes on a later tick, once however often it is asked for meanwhile; for edits that come in bursts. */
	void RequestRefreshTypes();
	/** The status an action on the page answered with (switch, make active); versions are read again. */
	void ApplyStatus(const CrowdyExecDeveloper::FAppStatus& NewStatus);

	const TOptional<CrowdyExecDeveloper::FAppStatus>& GetStatus() const { return Status; }
	const TArray<CrowdyExecDeveloper::FVersion>& GetVersions() const { return Versions; }
	bool AreVersionsLoaded() const { return bVersionsLoaded; }
	/** The active version, or null when nothing is live or the versions are not read yet. */
	const CrowdyExecDeveloper::FVersion* GetLiveVersion() const;
	const FText& GetStatusError() const { return StatusError; }
	const FText& GetVersionsError() const { return VersionsError; }

	const CrowdyExecDeploy::FProjectDeploy& GetProject() const { return Project; }
	const TArray<CrowdyExecRevisions::FTypeChange>& GetChanges() const { return Changes; }
	const CrowdyExecRevisions::FTypeChange* FindChange(const FString& TypeName) const;
	/** The type's recorded revisions, newest first, as last read; empty when it has none. */
	const TArray<CrowdyExecRevisions::FRevision>& GetHistory(const FString& TypeName) const;
	/** Where each project type's crate is, by Type Name. */
	FString GetCrateDirectory(const FString& TypeName) const;
	/** True when more than one of the project's definitions has this Type Name (ignoring case, as folders on Windows do), so they share one crate folder. */
	bool IsSharedTypeName(const FString& TypeName) const;
	/** Changes whenever the types, their changes, the status or the versions change; not for deploy progress. Lets a listener skip rebuilding rows. */
	uint32 GetDataGeneration() const { return DataGeneration; }

	/** True when every type is ready and a deploy would change what runs (or what runs is not known). */
	bool CanDeploy() const;
	/** Asks first, naming what it adds, changes and removes, then builds and deploys every type and records their revisions. False when it did not start. */
	bool Deploy();
	/** Asks first, then deploys the platform's example types in place of the app's live version. */
	bool DeployStarters();
	/** A deploy, or another change to the live app, is under way. */
	bool IsBusy() const { return bBusy || bActionBusy; }
	/** Marks the service busy for another change to the live app (switch, make active), so no deploy starts meanwhile. False when already busy. */
	bool TryBeginAction();
	void EndAction();

	/** One line for the deploy area: why a deploy cannot go ahead, what it would change, its progress, or how it ended. */
	FText GetDeployLine() const;
	FLinearColor GetDeployLineColor() const;
	/** The last build's output; empty before the first build of this session. */
	const FString& GetBuildLog() const { return BuildLog; }
	bool DidLastBuildFail() const { return bLastBuildFailed; }

	/** Anything above changed. Broadcast on the game thread. */
	FSimpleMulticastDelegate& OnChanged() { return ChangedEvent; }

private:
	struct FClientKey
	{
		FString DiscoveryUrl;
		FString Token;
		int64 AppId = 0;
		ECrowdyEnvironment Environment{};

		bool Matches(const FClientKey& Other) const;
	};

	static FClientKey ReadClientKey();

	void ResetAppState();
	void RefreshStatus();
	void RefreshVersions();
	void Recompare();
	void StartBuild(const TArray<CrowdyExecDeveloper::FBuildCrate>& Crates, const FString& ManifestJson, bool bRecord);
	void OnBuildFinished(const CrowdyExecDeveloper::TResult<CrowdyExecDeveloper::FBuild>& Result, const FString& ManifestJson, bool bRecord);
	void SetDeployMessage(const FText& Message, const FLinearColor& Color, bool bCountSeconds = false);
	void FinishDeploy(const FText& Message, const FLinearColor& Color);
	FText GetReadinessHint() const;
	FText GetChangeSummary() const;

	TSharedPtr<CrowdyExecDeveloper::FClient> Client;
	FClientKey ClientKey;
	FString ClientError;

	TOptional<CrowdyExecDeveloper::FAppStatus> Status;
	TArray<CrowdyExecDeveloper::FVersion> Versions;
	FText StatusError;
	FText VersionsError;

	struct FHistory
	{
		FString Directory;
		/** The history file's time when read, so a copy written by a pull or the command line is read again. */
		FDateTime Stamp;
		TArray<CrowdyExecRevisions::FRevision> Revisions;
	};

	void HandleActivationChanged(bool bActive);
	bool HandleDeferredRefresh(float DeltaTime);

	CrowdyExecDeploy::FProjectDeploy Project;
	/** The project's definitions as last read, so comparing again loads no assets. */
	TArray<TWeakObjectPtr<const UCrowdyServerObjectDefinition>> Definitions;
	/** Their crate folders, by Type Name. */
	TMap<FString, FString> CrateDirectories;
	/** Read again only when a type, its folder or its history file's time changed. */
	TMap<FString, FHistory> Histories;
	TArray<CrowdyExecRevisions::FTypeChange> Changes;
	/** The project as it was sent, and its crate folders then, recorded once the deploy succeeds. */
	CrowdyExecDeploy::FProjectDeploy Sending;
	TMap<FString, FString> SendingDirectories;

	FTSTicker::FDelegateHandle RefreshTicker;
	FDelegateHandle ActivationHandle;
	double TypesReadSeconds = 0.0;
	uint32 DataGeneration = 0;

	FString BuildLog;
	FText DeployMessage;
	FLinearColor DeployColor = FLinearColor::White;
	double BuildStartSeconds = 0.0;
	int32 StatusRequest = 0;
	int32 VersionsRequest = 0;
	bool bVersionsLoaded = false;
	/** A deploy runs; its progress is the deploy line. */
	bool bBusy = false;
	bool bActionBusy = false;
	bool bCountDeploySeconds = false;
	bool bLastBuildFailed = false;

	FSimpleMulticastDelegate ChangedEvent;
};
