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
		/** The type the others run under: RootTypeName, or the project's Game Kit root (world), whose crate is one of the project's. */
		FString Root = RootTypeName;
		/** Everything that stops a build; empty when the deploy can go ahead. */
		TArray<FString> Problems;
		/** What may go wrong after the deploy without stopping it, such as platform data the code uses without asking for it. */
		TArray<FString> Warnings;
	};

	/** Every Server Object definition asset in the project but the SDK's root, loaded, sorted by type name. */
	TArray<UCrowdyServerObjectDefinition*> FindProjectDefinitions();

	/** Where a type's crate is on disk and whether it matches what Generate Server Code would write now. */
	FTypeState InspectType(const UCrowdyServerObjectDefinition& Definition, const FString& CrateDirectory);

	/** The type Definition runs under: a Game Kit starter's Parent when Definitions has a type of that name, else Root. */
	FString ParentOf(const UCrowdyServerObjectDefinition& Definition, TConstArrayView<const UCrowdyServerObjectDefinition*> Definitions, const FString& Root);

	/** A Game Kit starter's Config as its manifest seed_b64; empty for an empty Config and for every other type. */
	FString SeedB64Of(const UCrowdyServerObjectDefinition& Definition);

	/** The Game Kit starter with no Parent, which is the app's root in the SDK's place (world); null when there is none. */
	const UCrowdyServerObjectDefinition* FindKitRoot(TConstArrayView<const UCrowdyServerObjectDefinition*> Definitions);

	/** The crate of the root type, generated from the SDK's Players Online definition and never on disk; no files when that does not load or generate. */
	CrowdyExecDeveloper::FBuildCrate MakeRootCrate();

	/** The manifest for Definitions under the root: each a hub (or a spoke for a Server Function Library) named by its type, open to players when they can reach it, with its save interval and idle timeout (a library: its replicas, concurrency and max_replicas), the types it Can Call (["*"] for any), its scopes and the server limits it changes. Deterministic. With a Game Kit root, that starter is the root and the SDK's is left out; a starter runs under its Parent when the project has it and carries its Config as its seed. */
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

	/** Reads every definition's crate from CrateDirectoryFor(definition), adds the root crate and the manifest, and lists every problem and warning. Any number of crates; a deploy splits them into builds. */
	FProjectDeploy AssembleProject(TConstArrayView<const UCrowdyServerObjectDefinition*> Definitions,
		TFunctionRef<FString(const UCrowdyServerObjectDefinition&)> CrateDirectoryFor);

	/** The platform's limits on one crate, checked before sending: 64 files and 2 MB of source, what one build takes. */
	bool CheckBuildLimits(TConstArrayView<CrowdyExecDeveloper::FBuildCrate> Crates, FString& OutProblem);

	/** Crates in order, split into the builds the platform takes: at most MaxCratesPerBuild (16 at most) and 2 MB of source each. */
	TArray<TArray<CrowdyExecDeveloper::FBuildCrate>> SplitIntoBuilds(TConstArrayView<CrowdyExecDeveloper::FBuildCrate> Crates, int32 MaxCratesPerBuild = 16);

	/** ManifestJson with each type whose crate Digests has (crate name to digest) named by that module digest instead; empty when it is not a manifest. */
	FString NameByDigest(const FString& ManifestJson, const TMap<FString, FString>& Digests);

	/** The type name prefix of the stand-ins a staging manifest adds. */
	inline const TCHAR* const StagingTypePrefix = TEXT("ck_staged_");

	/** A manifest that stores Crates' modules on the platform without changing what runs: the live manifest's types as they are, plus a stand-in hub per crate that players cannot reach and nothing calls (with nothing live, the first is the root). A live seed, which the platform gives back only as seed_bytes, is taken from DeployManifestJson's type of that name at that size. Empty with OutProblem when LiveManifestJson is not a manifest or a live seed cannot be kept. */
	FString MakeStagingManifest(const FString& LiveManifestJson, const FString& DeployManifestJson, TConstArrayView<CrowdyExecDeveloper::FBuildCrate> Crates, FString& OutProblem);
}
