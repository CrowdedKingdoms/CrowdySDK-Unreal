// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "CrowdyLeaderboardsKitActions.generated.h"

class FJsonObject;
class UCrowdyGameModelSubsystem;

// A parsed, read-only view of one leaderboard entry, returned by Get Leaderboard. Position is the 1-based place
// after the client-side sort (leaderboard reads have no server-side ORDER BY), distinct from the optional stamped
// Rank property.
USTRUCT(BlueprintType)
struct FCrowdyLeaderboardEntry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Kits|Leaderboards")
	FString ContainerId;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Kits|Leaderboards")
	FString DisplayName;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Kits|Leaderboards")
	FString OwnerUserId;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Kits|Leaderboards")
	FString BoardId;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Kits|Leaderboards")
	int32 Score = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Kits|Leaderboards")
	int32 Season = 1;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Kits|Leaderboards")
	int32 Rank = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Kits|Leaderboards")
	int32 Position = 0;
};

// On success ReturnValueJson carries submit_score's server return (the kept-best or overwritten score) and
// ErrorMessage is empty; on failure ErrorMessage carries the server envelope's reason. submit_score is
// host-refereed by default, so a plain player call is expected to be denied and reads back verbatim on Failed.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FCrowdySubmitScoreOutcome, const FString&, ReturnValueJson, const FString&, ErrorMessage);

// On success ContainerId names the caller's entry for the board and ErrorMessage is empty; on failure ContainerId
// is empty and ErrorMessage explains why.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FCrowdyEnsureLeaderboardEntryOutcome, const FString&, ContainerId, const FString&, ErrorMessage);

// On success Entries is the board sorted best-first with 1-based positions (up to TopN) and ErrorMessage is empty;
// on failure Entries is empty and ErrorMessage explains why.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FCrowdyGetLeaderboardOutcome, const TArray<FCrowdyLeaderboardEntry>&, Entries, const FString&, ErrorMessage);

/**
 * Submit a score to a board for the local player: ensure the player's LeaderboardEntry for BoardId exists
 * (find-or-create), then invoke submit_score with the points. submit_score is a trusted call (host-refereed by
 * default per the kit's submitAuthority), so a plain player call is expected to be denied; that denial reads back
 * verbatim on Failed. On success ReturnValueJson carries the entry's kept-best (or overwritten) score.
 *
 * TypePrefix selects the deployed Leaderboards kit's type/function names (<Prefix>LeaderboardEntry /
 * snake_case(prefix) + "_submit_score"); leave it empty for a kit deployed with no prefix.
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdySubmitScoreAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Kits|Leaderboards")
	FCrowdySubmitScoreOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Kits|Leaderboards")
	FCrowdySubmitScoreOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Kits|Leaderboards", DisplayName = "Submit Score")
	static UCrowdySubmitScoreAction* SubmitScore(UObject* WorldContext, const FString& TypePrefix,
		const FString& BoardId, int32 Points, const FString& DisplayName = FString(),
		const FString& SessionId = FString());

	// The pure param marshaller: submit_score takes points as an int, which on the wire is a JSON number. Public +
	// static so it is headless-testable with no world/subsystem/HTTP.
	static TSharedPtr<FJsonObject> BuildSubmitScoreParams(int32 Points);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString TypePrefix;
	FString BoardId;
	int32 Points = 0;
	FString DisplayName;
	FString SessionId;
};

/**
 * Ensure the local player's LeaderboardEntry for a board exists, creating it if missing, and return its container
 * id. This is the shared find-or-create Submit Score also runs. A player has one entry per board; a match is the
 * caller's entry whose board_id equals BoardId. On create the board_id is seeded (fatal on rejection) and the
 * owner mirror is written best-effort (the server already pins the record owner).
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyEnsureLeaderboardEntryAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Kits|Leaderboards")
	FCrowdyEnsureLeaderboardEntryOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Kits|Leaderboards")
	FCrowdyEnsureLeaderboardEntryOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Kits|Leaderboards", DisplayName = "Ensure Leaderboard Entry")
	static UCrowdyEnsureLeaderboardEntryAction* EnsureLeaderboardEntry(UObject* WorldContext,
		const FString& TypePrefix, const FString& BoardId, const FString& DisplayName = FString(),
		const FString& SessionId = FString());

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString TypePrefix;
	FString BoardId;
	FString DisplayName;
	FString SessionId;
};

/**
 * Read a board's entries, sorted best-first with 1-based positions, up to TopN. Leaderboard reads have no
 * server-side ORDER BY, so this lists the entry type, pulls each row's state, keeps the rows whose board_id equals
 * BoardId, and sorts by score descending client-side. This is an N+1 read (one list plus one pull per row); a
 * batched or engine-computed path is a future optimization, fine for the few hundred entries a per-app board holds.
 *
 * TypePrefix selects the deployed Leaderboards kit's type name (<Prefix>LeaderboardEntry); leave it empty for a kit
 * deployed with no prefix.
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyGetLeaderboardAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Kits|Leaderboards")
	FCrowdyGetLeaderboardOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Kits|Leaderboards")
	FCrowdyGetLeaderboardOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Kits|Leaderboards", DisplayName = "Get Leaderboard")
	static UCrowdyGetLeaderboardAction* GetLeaderboard(UObject* WorldContext, const FString& TypePrefix,
		const FString& BoardId, int32 TopN = 10, const FString& SessionId = FString());

	// The pure parser: read one entry's board_id/score/season/rank out of a pulled container-state property map,
	// threading the row header (container id, display name, owner) through. Missing keys leave the struct defaults.
	// Public + static so it is headless-testable with no world/subsystem/HTTP.
	static FCrowdyLeaderboardEntry ParseLeaderboardEntry(const TSharedPtr<FJsonObject>& State,
		const FString& ContainerId, const FString& DisplayName, const FString& OwnerUserId);

	// The pure ranker: stable-sort the entries by score descending, then stamp each with its 1-based Position.
	// Public + static so the client-side ranking is headless-testable directly.
	static void RankEntries(TArray<FCrowdyLeaderboardEntry>& Entries);

	virtual void Activate() override;

private:
	// Pulls the next listed row's state, collecting the board's matching entries, then finalizes once the rows
	// drain (sort, rank, take TopN, broadcast).
	void PullNextRow();
	void FinishAndBroadcast();

	TWeakObjectPtr<UObject> WorldContextObject;
	FString TypePrefix;
	FString BoardId;
	int32 TopN = 10;
	FString SessionId;

	TWeakObjectPtr<UCrowdyGameModelSubsystem> Model;
	TArray<TSharedPtr<FJsonObject>> ListedRows;
	int32 RowIndex = 0;
	TArray<FCrowdyLeaderboardEntry> Collected;
};
