#pragma once

#include "CoreMinimal.h"
#include "CrowdyExecDeveloperApi.h"

class UCrowdyServerObjectDefinition;

/** What a deploy of the project's Server Object types sends: their crates as they are on disk, a root, and the manifest naming them. */
namespace CrowdyExecDeploy
{
	/** The root every project type sits under on the platform, which needs exactly one root per app. It keeps Players Online. */
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
		/** What may go wrong after the deploy without stopping it, such as platform data the code uses without asking for it. */
		TArray<FString> Warnings;
	};

	/** Every Server Object definition asset in the project but the SDK's root, loaded, sorted by type name. */
	TArray<UCrowdyServerObjectDefinition*> FindProjectDefinitions();

	/** Where a type's crate is on disk and whether it matches what Generate Server Code would write now. */
	FTypeState InspectType(const UCrowdyServerObjectDefinition& Definition, const FString& CrateDirectory);

	/** The crate of the root type, generated from the SDK's Players Online definition and never on disk; no files when that does not load or generate. */
	CrowdyExecDeveloper::FBuildCrate MakeRootCrate();

	/** The manifest for Definitions under the root: each a hub (or a spoke for a Server Function Library) named by its type, open to players when they can reach it, with its save interval and idle timeout (a library: its replicas, concurrency and max_replicas), the types it Can Call (["*"] for any), its scopes and the server limits it changes. Deterministic. */
	FString MakeManifest(TConstArrayView<const UCrowdyServerObjectDefinition*> Definitions);

	/** A per-type limit the platform enforces, in manifest units. */
	struct FServerLimit
	{
		const TCHAR* Field = nullptr;
		int64 Value = 0;
		/** The platform's own; a manifest leaves out a limit at its default. */
		int64 Default = 0;
	};

	/** Each server limit for Definition in manifest order, brought into the platform's range. */
	TArray<FServerLimit> LimitsOf(const UCrowdyServerObjectDefinition& Definition);

	/** How many instances of a Server Function Library run and how many calls each answers at once, brought into the platform's range. */
	struct FScaling
	{
		int32 Replicas = 1;
		/** At least Replicas; the manifest names it only when it is more. */
		int32 MaxReplicas = 1;
		int32 Concurrency = 16;
	};

	FScaling ScalingOf(const UCrowdyServerObjectDefinition& Definition);

	/** The value the platform uses for a manifest limit (memory_mb, fuel_per_call, mailbox or deadline_ms): ManifestValue, brought into the field's range. Any other field is returned as given. */
	int64 ClampLimit(const FString& Field, int64 ManifestValue);

	/** Why the definition needs players.read whether or not Reads Players is on, such as "Members From is Crowdy Team"; empty when nothing forces it. */
	FString PlayersReadRequiredBecause(const UCrowdyServerObjectDefinition& Definition);

	/** A warning for each setting on a Developers Only or Server Only function that is never checked, since only players' calls are: Cooldown and Required Feature. */
	TArray<FString> FindCallerWarnings(const UCrowdyServerObjectDefinition& Definition);

	/** The platform data a type's server code may use, in the platform's order: what its definition asks for, and players.read for Crowdy Team members or records and a Required Feature. */
	TArray<FString> ScopesOf(const UCrowdyServerObjectDefinition& Definition);

	/** A warning for each scope the crate's code calls into without its definition asking for it. A call reached another way, such as through ctx.node or a variable holding ctx.world(), is not seen. */
	TArray<FString> FindScopeWarnings(const UCrowdyServerObjectDefinition& Definition, const CrowdyExecDeveloper::FBuildCrate& Crate);

	/** Reads every definition's crate from CrateDirectoryFor(definition), adds the root crate and the manifest, and lists every problem and warning. */
	FProjectDeploy AssembleProject(TConstArrayView<const UCrowdyServerObjectDefinition*> Definitions,
		TFunctionRef<FString(const UCrowdyServerObjectDefinition&)> CrateDirectoryFor);

	/** The platform's build limits, checked before sending: at most 16 crates, 64 files a crate, 2 MB of source in all. */
	bool CheckBuildLimits(TConstArrayView<CrowdyExecDeveloper::FBuildCrate> Crates, FString& OutProblem);
}
