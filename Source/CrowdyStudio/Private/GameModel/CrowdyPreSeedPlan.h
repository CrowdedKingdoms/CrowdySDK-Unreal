#pragma once

#include "CoreMinimal.h"
#include "Data/CrowdyContainerManifest.h"
#include "Dom/JsonObject.h"
#include "Model/CrowdyStudioTypes.h" // FStudioContainerType

/** One server container row as the pre-seed plan reads it: identity only, no state. */
struct FCrowdyPreSeedServerRow
{
	FString ContainerId;
	FString TypeName;
	FString BindingKey;
	/** Empty for an app-global row. */
	FString SessionId;
};

enum class ECrowdyPreSeedRowKind : uint8
{
	/** A manifest row with no server row: Apply creates it. */
	Create,
	/** A manifest row the server already holds. */
	Existing,
	/** A server row of a manifest type, in scope, that no placement claims. Reported, never deleted. */
	Orphan,
};

struct FCrowdyPreSeedRow
{
	ECrowdyPreSeedRowKind Kind = ECrowdyPreSeedRowKind::Create;
	FString TypeName;
	FString BindingKey;
	FString DisplayName;
	/** Set for Existing and Orphan rows, and for a Create row once Apply created it. */
	FString ContainerId;
	/** The type keeps one row per key app-wide: read and written with no session, whatever scope was picked. */
	bool bAppScoped = false;
	/** The type is admin-instantiable or carries a bind policy, so Apply may seed it in bulk instead of ensuring it. */
	bool bSeedEligible = false;
};

/** One gameModelSeed call: the Create rows it carries, all of one scope. */
struct FCrowdyPreSeedBatch
{
	TArray<int32> RowIndices;
	bool bAppScoped = false;
};

/** The view-facing outcome of a plan or an apply, shaped like the schema sync's report. */
struct FCrowdyPreSeedReport
{
	bool bValid = false;
	bool bApplied = false;
	/** A durable banner: a failed plan, a partial apply, a missing manifest. */
	FString StatusNote;
	FString MapPackage;
	/** Empty means app-global. */
	FString ScopeSessionId;
	int32 ManifestRowCount = 0;
	TArray<FCrowdyPreSeedRow> Rows;
	int32 ToCreate = 0;
	/** Rows the server held before this plan's apply; a created row is counted under Created, not here. */
	int32 Existing = 0;
	int32 Orphans = 0;
	int32 Created = 0;
	int32 Failed = 0;
	TArray<FString> Warnings;
};

/** The pure half of pre-seeding: manifest rows against the server's rows for one scope. No I/O. */
class FCrowdyPreSeedPlan
{
public:
	/** Every distinct type name the manifest declares, in first-seen order: one paged read per entry. */
	static TArray<FString> DistinctTypes(const TArray<FCrowdyContainerManifestRow>& Manifest);

	/**
	 * Server rows outside ScopeSessionId are ignored, not orphans: a key is unique per (type, session) and another
	 * session's row is another session's business. Duplicate manifest rows become one Create plus a warning.
	 * Types are the server's container types: a row of an app-scoped type is in scope when it has no session, and
	 * each planned row records whether its type may be seeded.
	 */
	static void Diff(const TArray<FCrowdyContainerManifestRow>& Manifest, const TArray<FCrowdyPreSeedServerRow>& Server,
		const FString& ScopeSessionId, FCrowdyPreSeedReport& Out,
		const TArray<FStudioContainerType>& Types = TArray<FStudioContainerType>());

	static bool IsAppScoped(const FStudioContainerType& Type);

	/** The server seeds keyed rows only on an admin-instantiable type or one carrying a bind policy. */
	static bool IsSeedEligible(const FStudioContainerType& Type);

	/**
	 * Splits the Create rows: seed-eligible ones into batches of at most MaxPerBatch, session-scoped and app-scoped
	 * rows never sharing a batch, in row order; the rest into OutEnsureRows for the one-by-one ensure.
	 */
	static void SplitCreateRows(const TArray<FCrowdyPreSeedRow>& Rows, int32 MaxPerBatch,
		TArray<FCrowdyPreSeedBatch>& OutBatches, TArray<int32>& OutEnsureRows);

	/** gameModelSeed variables for one batch: tempId is the row index; no sessionId when SessionId is empty. */
	static TSharedPtr<FJsonObject> BuildSeedVariables(int64 AppId, const FString& SessionId,
		const TArray<FCrowdyPreSeedRow>& Rows, const TArray<int32>& RowIndices);

	/**
	 * Applies a seed result's idMapJson (a JSON object tempId -> containerId) to the batch's rows: a mapped row
	 * becomes Existing with its id, an unmapped one stays Create. Returns the rows mapped.
	 */
	static int32 ApplySeedIdMap(const FString& IdMapJson, const TArray<int32>& RowIndices, TArray<FCrowdyPreSeedRow>& Rows);

	/** The one-line count for the card. */
	static FString BuildCountLine(const FCrowdyPreSeedReport& Report);

	/** The full report text for the details panel. */
	static FString BuildReportText(const FCrowdyPreSeedReport& Report);
};
