#pragma once

#include "CoreMinimal.h"
#include "CrowdyExecDeveloperApi.h"
#include "CrowdyExecRevisions.h"

/** One deploy from crates: those the platform already holds a module for are named by its digest, the rest are built in as many builds as the platform's limits need, and all of it is deployed as one version. */
namespace CrowdyExecBuildAndDeploy
{
	struct FPlan
	{
		/** The crates to build, one platform build each, in order. */
		TArray<TArray<CrowdyExecDeveloper::FBuildCrate>> Builds;
		/** Crate name to the digest of the module the platform holds for it, named instead of built. */
		TMap<FString, FString> Reused;
	};

	/** The digests a plan may reuse: those Versions name, none when the versions are not known or bRebuildAll asks for every crate to be built again. */
	TSet<FString> DigestsToReuse(TConstArrayView<CrowdyExecDeveloper::FVersion> Versions, bool bVersionsKnown, bool bRebuildAll);

	/** Reuses a crate's module when HistoryFor(crate name) recorded a digest for exactly its files that Uploaded has, and builds the rest, at most MaxCratesPerBuild a build. */
	FPlan MakePlan(TConstArrayView<CrowdyExecDeveloper::FBuildCrate> Crates, const TSet<FString>& Uploaded,
		TFunctionRef<TArray<CrowdyExecRevisions::FRevision>(const FString& CrateName)> HistoryFor, int32 MaxCratesPerBuild = 16);

	/** The app's active version as read before the deploy; Version 0 when nothing is live. */
	struct FLive
	{
		int32 Version = 0;
		FString ManifestJson;
	};

	/** The active version among Versions; unset when one is active but its manifest is not given, so what runs is not known. */
	TOptional<FLive> FindLive(TConstArrayView<CrowdyExecDeveloper::FVersion> Versions);

	/** Why Plan cannot be deployed over Live, checked before anything is built; empty when it can. */
	FString FindPlanProblem(const FPlan& Plan, const FString& ManifestJson, const TOptional<FLive>& Live);

	enum class EStep : uint8
	{
		Deployed,
		BuildNotStarted,
		/** The build's status could not be followed to its end. */
		BuildLost,
		/** A build finished failed; its log is the last build's. */
		BuildFailed,
		/** A build could not be stored before the deploy: the active version changed, or a staging deploy or the return to the live version was refused. */
		StagingFailed,
		DeployFailed
	};

	struct FOutcome
	{
		EStep Step = EStep::Deployed;
		/** Why it stopped, for every step but Deployed and BuildFailed. */
		CrowdyExecDeveloper::FError Error;
		int32 Version = 0;
		/** The versions the staging deploys made, oldest first; each ran what was live, plus stand-ins nothing calls. */
		TArray<int32> StagedVersions;
		/** The live version made active again after the last staging deploy; 0 when none was. */
		int32 RestoredVersion = 0;
		/** Every build sent, as it ended; the one that stopped the deploy last. */
		TArray<CrowdyExecDeveloper::FBuild> Builds;
		/** Each crate's module: those built and those reused. */
		TArray<CrowdyExecDeveloper::FBuildArtifact> Artifacts;
	};

	struct FCallbacks
	{
		/** Build Index of Count was sent, or its status was read. */
		TFunction<void(int32 Index, int32 Count, const CrowdyExecDeveloper::FBuild& Build)> OnBuild;
		/** Every build succeeded and the first change to the app, a staging deploy or the deploy, is about to be sent. */
		TFunction<void()> OnDeploying;
		/** The deploy itself is about to be sent. */
		TFunction<void()> OnFinalDeploy;
		TFunction<void(const FOutcome& Outcome)> OnDone;
	};

	/** Runs every build of Plan first, so a failed one deploys nothing. Then, for each build but the last: a staging deploy of the live version's types plus a stand-in per crate, which stores its modules, and the live version made active again. Then deploys ManifestJson with every reused and stored module named by digest and the last build's crates by name. Staging refuses when the active version is no longer Live. OnDone is called once. */
	void Run(const TSharedRef<CrowdyExecDeveloper::FClient>& Client, FPlan Plan, FString ManifestJson, FLive Live, FCallbacks Callbacks,
		float BuildTimeoutSeconds = 600.f, float PollSeconds = 3.f);
}
