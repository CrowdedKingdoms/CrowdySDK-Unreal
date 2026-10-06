#pragma once

#include "CoreMinimal.h"
#include "CrowdyExecDeveloperApi.h"

class UCrowdyServerObjectDefinition;

/** What a deploy of the project's Server Object types sends: their crates as they are on disk, a root, and the manifest naming them. */
namespace CrowdyExecDeploy
{
	/** The root every project type sits under on the platform, which needs exactly one root per app. It keeps no state. */
	inline const TCHAR* const RootTypeName = TEXT("root");

	enum class ECrateState : uint8
	{
		UpToDate,
		NotGenerated,
		OutOfDate,
		/** The definition itself does not bake. */
		Invalid
	};

	struct FTypeState
	{
		FString TypeName;
		FString AssetPath;
		ECrateState State = ECrateState::NotGenerated;
		/** Why it cannot be built, for any state but UpToDate. */
		FString Problem;
	};

	struct FProjectDeploy
	{
		TArray<FTypeState> Types;
		TArray<CrowdyExecDeveloper::FBuildCrate> Crates;
		FString ManifestJson;
		/** Everything that stops a build; empty when the deploy can go ahead. */
		TArray<FString> Problems;
	};

	/** Every Server Object definition asset in the project, loaded, sorted by type name. */
	TArray<UCrowdyServerObjectDefinition*> FindProjectDefinitions();

	/** Where a type's crate is on disk and whether it matches what Generate Server Code would write now. */
	FTypeState InspectType(const UCrowdyServerObjectDefinition& Definition, const FString& CrateDirectory);

	/** The crate of the root type, written by the SDK and never on disk. */
	CrowdyExecDeveloper::FBuildCrate MakeRootCrate();

	/** The manifest for Definitions under the root: each a hub named by its type, callable by players, with its save interval, idle timeout, the types it Can Call and, for Crowdy Team members, the players.read scope. Deterministic. */
	FString MakeManifest(TConstArrayView<const UCrowdyServerObjectDefinition*> Definitions);

	/** Reads every definition's crate from CrateDirectoryFor(definition), adds the root crate and the manifest, and lists every problem. */
	FProjectDeploy AssembleProject(TConstArrayView<const UCrowdyServerObjectDefinition*> Definitions,
		TFunctionRef<FString(const UCrowdyServerObjectDefinition&)> CrateDirectoryFor);

	/** The platform's build limits, checked before sending: at most 16 crates, 64 files a crate, 2 MB of source in all. */
	bool CheckBuildLimits(TConstArrayView<CrowdyExecDeveloper::FBuildCrate> Crates, FString& OutProblem);
}
