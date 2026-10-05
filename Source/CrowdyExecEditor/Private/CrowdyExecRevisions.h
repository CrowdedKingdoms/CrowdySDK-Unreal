#pragma once

#include "CoreMinimal.h"
#include "CrowdyExecDeploy.h"
#include "CrowdyExecDeveloperApi.h"

class UCrowdyServerObjectDefinition;

/** Each type's deployed server code, recorded beside its crate at every deploy, since the platform keeps only a build digest. */
namespace CrowdyExecRevisions
{
	/** One deployed state of a type's server code, newest first in its history. */
	struct FRevision
	{
		/** Identifies the sent files, ignoring line endings and whitespace at the end of each file. */
		FString Fingerprint;
		/** The app versions that ran it, newest first. */
		TArray<int32> Versions;
		/** The platform's build digests for it, newest first. */
		TArray<FString> Digests;
		/** When it was last deployed, UTC. */
		FDateTime DeployedAt;
		/** The crate as sent: Cargo.toml and src/*.rs, the type's logic as src/logic.rs. */
		TArray<CrowdyExecDeveloper::FBuildFile> Files;

		const CrowdyExecDeveloper::FBuildFile* FindFile(const FString& Path) const;
	};

	/** A type as it runs in the live version, read from that version's manifest. */
	struct FLiveType
	{
		FString TypeName;
		FString Digest;
		/** 0 when the manifest does not say. */
		int64 SaveIntervalMs = 0;
		int64 IdleTimeoutMs = 0;
	};

	enum class EChange : uint8
	{
		/** The live version runs this code with these settings. */
		Unchanged,
		/** The live version runs another revision of the code, or other settings. */
		Changed,
		/** Not in the live version. */
		New,
		/** The live version runs code this project has no record of, for example deployed from another copy of it. */
		Unknown,
		/** In the live version but no longer in the project: the next deploy removes it. */
		Removed,
		/** Not compared: its server code is not ready, or the live version is not known yet. */
		NotCompared
	};

	struct FTypeChange
	{
		FString TypeName;
		EChange Change = EChange::NotCompared;
		/** The live version, 0 when nothing is live. */
		int32 LiveVersion = 0;
		/** The index in the type's history of the revision the live version runs, INDEX_NONE when not there. */
		int32 LiveRevision = INDEX_NONE;
		/** For Changed, what differs, in plain words: "server code", "save interval", "idle timeout". */
		TArray<FString> What;
	};

	struct FDiffLine
	{
		enum class EKind : uint8 { Same, Added, Removed };
		EKind Kind = EKind::Same;
		FString Text;
	};

	/** Where a type's history is kept: revisions.json in its crate folder. */
	FString GetHistoryPath(const FString& CrateDirectory);

	/** The type's revisions, newest first; empty when it has none. False with OutError when the file exists but cannot be read. */
	bool LoadHistory(const FString& CrateDirectory, TArray<FRevision>& OutRevisions, FString& OutError);

	bool SaveHistory(const FString& CrateDirectory, TConstArrayView<FRevision> Revisions, FString& OutError);

	/** Deterministic over the files' paths and text; line endings and whitespace at the end of a file do not change it. */
	FString Fingerprint(TConstArrayView<CrowdyExecDeveloper::FBuildFile> Files);

	/** Puts Crate, deployed as Version with Digest at When, first in History: a revision with the same fingerprint moves up and gains the version and digest. Keeps the Keep newest. */
	void AddRevision(TArray<FRevision>& History, const CrowdyExecDeveloper::FBuildCrate& Crate, const FString& Digest, int32 Version, const FDateTime& When, int32 Keep);

	/** After Project deployed as Version: adds each sent type's revision, with its digest from Artifacts, to its history. Types whose history could not be written are named in OutErrors. */
	void RecordDeploy(const CrowdyExecDeploy::FProjectDeploy& Project, TConstArrayView<CrowdyExecDeveloper::FBuildArtifact> Artifacts, int32 Version,
		const FDateTime& When, int32 Keep, TFunctionRef<FString(const FString& TypeName)> CrateDirectoryFor, TArray<FString>& OutErrors);

	/** The types a manifest runs, the root included. False when it is not a manifest. */
	bool ParseManifest(const FString& ManifestJson, TArray<FLiveType>& OutTypes);

	/** Each project type against the live version (Live null: nothing live; bLiveKnown false: not compared), then each live type the project no longer has. */
	TArray<FTypeChange> Compare(const CrowdyExecDeploy::FProjectDeploy& Project, TConstArrayView<const UCrowdyServerObjectDefinition*> Definitions,
		const CrowdyExecDeveloper::FVersion* Live, bool bLiveKnown, TFunctionRef<TArray<FRevision>(const FString& TypeName)> HistoryFor);

	/** True when a deploy would change what runs: any type New, Changed, Unknown or Removed. */
	bool HasChanges(TConstArrayView<FTypeChange> Changes);

	/** The lines of Old and New, in order, each kept, added or removed. Very large inputs are compared as wholes. */
	TArray<FDiffLine> DiffLines(const FString& Old, const FString& New);
}
