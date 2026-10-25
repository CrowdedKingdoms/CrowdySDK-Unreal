#include "CrowdyExecBuildAndDeploy.h"

#include "CrowdyExecDeploy.h"

namespace CrowdyExecBuildAndDeployDetail
{
	using namespace CrowdyExecBuildAndDeploy;
	using namespace CrowdyExecDeveloper;

	struct FRunState
	{
		TSharedPtr<FClient> Client;
		FPlan Plan;
		FString ManifestJson;
		FLive Live;
		/** The active version's manifest read just before staging. */
		FString StagingFrom;
		FCallbacks Callbacks;
		float BuildTimeoutSeconds = 600.f;
		float PollSeconds = 3.f;
		FOutcome Outcome;
	};

	using FRunRef = TSharedRef<FRunState>;

	void Finish(const FRunRef& Run, EStep Step, const FError& Error = FError())
	{
		Run->Outcome.Step = Step;
		Run->Outcome.Error = Error;
		if (Run->Callbacks.OnDone)
		{
			Run->Callbacks.OnDone(Run->Outcome);
		}
	}

	FError Problem(const FString& Message)
	{
		FError Error;
		Error.Message = Message;
		return Error;
	}

	void ReportBuild(const FRunRef& Run, int32 Index, const FBuild& Build)
	{
		if (Run->Callbacks.OnBuild)
		{
			Run->Callbacks.OnBuild(Index, Run->Plan.Builds.Num(), Build);
		}
	}

	void DeployAll(const FRunRef& Run)
	{
		TMap<FString, FString> Digests = Run->Plan.Reused;
		const int32 Last = Run->Plan.Builds.Num() - 1;
		for (int32 Index = 0; Index < Last; ++Index)
		{
			for (const FBuildArtifact& Artifact : Run->Outcome.Builds[Index].Artifacts)
			{
				Digests.Add(Artifact.Crate, Artifact.Digest);
			}
		}
		const FString Manifest = CrowdyExecDeploy::NameByDigest(Run->ManifestJson, Digests);
		if (Manifest.IsEmpty())
		{
			Finish(Run, EStep::DeployFailed, Problem(TEXT("The manifest could not be read to name the stored modules")));
			return;
		}
		if (Run->Callbacks.OnFinalDeploy)
		{
			Run->Callbacks.OnFinalDeploy();
		}
		const FString BuildId = Last >= 0 ? Run->Outcome.Builds[Last].BuildId : FString();
		Run->Client->Deploy(Manifest, BuildId, [Run](const TResult<int32>& Deployed)
		{
			if (!Deployed.bOk)
			{
				Finish(Run, EStep::DeployFailed, Deployed.Error);
				return;
			}
			Run->Outcome.Version = Deployed.Value;
			Finish(Run, EStep::Deployed);
		});
	}

	void Stage(const FRunRef& Run, int32 Index)
	{
		if (Index >= Run->Plan.Builds.Num() - 1)
		{
			DeployAll(Run);
			return;
		}
		FString StagingProblem;
		const FString Staging = CrowdyExecDeploy::MakeStagingManifest(Run->StagingFrom, Run->ManifestJson, Run->Plan.Builds[Index], StagingProblem);
		if (Staging.IsEmpty())
		{
			Finish(Run, EStep::StagingFailed, Problem(StagingProblem));
			return;
		}
		Run->Client->Deploy(Staging, Run->Outcome.Builds[Index].BuildId, [Run, Index](const TResult<int32>& Staged)
		{
			if (!Staged.bOk)
			{
				Finish(Run, EStep::StagingFailed, Staged.Error);
				return;
			}
			Run->Outcome.StagedVersions.Add(Staged.Value);
			// With nothing live there is nothing to go back to; the deploy follows at once.
			if (Run->Live.Version <= 0)
			{
				Stage(Run, Index + 1);
				return;
			}
			Run->Client->ActivateVersion(Run->Live.Version, [Run, Index](const TResult<FAppStatus>& Restored)
			{
				if (!Restored.bOk)
				{
					FError Error = Restored.Error;
					Error.Message = FString::Printf(TEXT("Version %d could not be made active again after storing a build: %s"), Run->Live.Version, *Error.Message);
					Finish(Run, EStep::StagingFailed, Error);
					return;
				}
				Run->Outcome.RestoredVersion = Run->Live.Version;
				Stage(Run, Index + 1);
			});
		});
	}

	// A staging deploy runs what is live, so it must be what is live now, not what was read before the builds.
	void StageFromActive(const FRunRef& Run)
	{
		Run->Client->Versions([Run](const TResult<TArray<FVersion>>& Versions)
		{
			if (!Versions.bOk)
			{
				Finish(Run, EStep::StagingFailed, Versions.Error);
				return;
			}
			const TOptional<FLive> Active = FindLive(Versions.Value);
			if (!Active.IsSet() || Active->Version != Run->Live.Version)
			{
				const FString Now = !Active.IsSet() ? FString(TEXT("not known")) : Active->Version > 0 ? FString::Printf(TEXT("version %d"), Active->Version) : FString(TEXT("none"));
				Finish(Run, EStep::StagingFailed, Problem(FString::Printf(TEXT("The app's active version is %s, not version %d as when the deploy started, so nothing was deployed; refresh, then deploy again"),
					*Now, Run->Live.Version)));
				return;
			}
			Run->StagingFrom = Active->ManifestJson;
			Stage(Run, 0);
		});
	}

	void Build(const FRunRef& Run, int32 Index)
	{
		if (Index >= Run->Plan.Builds.Num())
		{
			if (Run->Callbacks.OnDeploying)
			{
				Run->Callbacks.OnDeploying();
			}
			if (Run->Plan.Builds.Num() > 1)
			{
				StageFromActive(Run);
				return;
			}
			DeployAll(Run);
			return;
		}
		Run->Client->Build(Run->Plan.Builds[Index], [Run, Index](const TResult<FBuild>& Started)
		{
			if (!Started.bOk)
			{
				Finish(Run, EStep::BuildNotStarted, Started.Error);
				return;
			}
			Run->Outcome.Builds.Add(Started.Value);
			ReportBuild(Run, Index, Started.Value);
			Run->Client->WaitForBuild(Started.Value.BuildId,
				[Run, Index](const TResult<FBuild>& Finished)
				{
					if (!Finished.bOk)
					{
						Finish(Run, EStep::BuildLost, Finished.Error);
						return;
					}
					Run->Outcome.Builds.Last() = Finished.Value;
					if (!Finished.Value.IsSucceeded())
					{
						Finish(Run, EStep::BuildFailed);
						return;
					}
					Run->Outcome.Artifacts.Append(Finished.Value.Artifacts);
					Build(Run, Index + 1);
				},
				[Run, Index](const FBuild& Progress) { ReportBuild(Run, Index, Progress); },
				Run->PollSeconds, Run->BuildTimeoutSeconds);
		});
	}
}

TSet<FString> CrowdyExecBuildAndDeploy::DigestsToReuse(TConstArrayView<CrowdyExecDeveloper::FVersion> Versions, bool bVersionsKnown, bool bRebuildAll)
{
	if (!bVersionsKnown || bRebuildAll)
	{
		return TSet<FString>();
	}
	return CrowdyExecRevisions::UploadedDigests(Versions);
}

CrowdyExecBuildAndDeploy::FPlan CrowdyExecBuildAndDeploy::MakePlan(TConstArrayView<CrowdyExecDeveloper::FBuildCrate> Crates, const TSet<FString>& Uploaded,
	TFunctionRef<TArray<CrowdyExecRevisions::FRevision>(const FString& CrateName)> HistoryFor, int32 MaxCratesPerBuild)
{
	FPlan Plan;
	TArray<CrowdyExecDeveloper::FBuildCrate> ToBuild;
	for (const CrowdyExecDeveloper::FBuildCrate& Crate : Crates)
	{
		const FString Digest = Uploaded.IsEmpty() ? FString() : CrowdyExecRevisions::FindUploadedDigest(HistoryFor(Crate.Name), Crate, Uploaded);
		if (Digest.IsEmpty())
		{
			ToBuild.Add(Crate);
			continue;
		}
		Plan.Reused.Add(Crate.Name, Digest);
	}
	Plan.Builds = CrowdyExecDeploy::SplitIntoBuilds(ToBuild, MaxCratesPerBuild);
	return Plan;
}

TOptional<CrowdyExecBuildAndDeploy::FLive> CrowdyExecBuildAndDeploy::FindLive(TConstArrayView<CrowdyExecDeveloper::FVersion> Versions)
{
	const CrowdyExecDeveloper::FVersion* Active = Versions.FindByPredicate([](const CrowdyExecDeveloper::FVersion& Version) { return Version.bActive; });
	if (!Active)
	{
		return FLive();
	}
	if (Active->ManifestJson.IsEmpty())
	{
		return {};
	}
	FLive Live;
	Live.Version = Active->Version;
	Live.ManifestJson = Active->ManifestJson;
	return Live;
}

FString CrowdyExecBuildAndDeploy::FindPlanProblem(const FPlan& Plan, const FString& ManifestJson, const TOptional<FLive>& Live)
{
	if (Plan.Builds.Num() < 2)
	{
		return FString();
	}
	if (!Live.IsSet())
	{
		return TEXT("This deploy is built in parts, each first stored by a version that runs what runs now, and what runs now could not be read; refresh, then deploy again");
	}
	FString Problem;
	CrowdyExecDeploy::MakeStagingManifest(Live->ManifestJson, ManifestJson, {}, Problem);
	return Problem;
}

void CrowdyExecBuildAndDeploy::Run(const TSharedRef<CrowdyExecDeveloper::FClient>& Client, FPlan Plan, FString ManifestJson, FLive Live, FCallbacks Callbacks,
	float BuildTimeoutSeconds, float PollSeconds)
{
	using namespace CrowdyExecBuildAndDeployDetail;
	const FRunRef State = MakeShared<FRunState>();
	State->Client = Client;
	State->Plan = MoveTemp(Plan);
	State->ManifestJson = MoveTemp(ManifestJson);
	State->Live = MoveTemp(Live);
	State->Callbacks = MoveTemp(Callbacks);
	State->BuildTimeoutSeconds = BuildTimeoutSeconds;
	State->PollSeconds = PollSeconds;
	for (const TPair<FString, FString>& Reused : State->Plan.Reused)
	{
		CrowdyExecDeveloper::FBuildArtifact& Artifact = State->Outcome.Artifacts.AddDefaulted_GetRef();
		Artifact.Crate = Reused.Key;
		Artifact.Digest = Reused.Value;
	}
	Build(State, 0);
}
