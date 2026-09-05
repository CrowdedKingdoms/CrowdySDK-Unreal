// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Replication/GameModel/CrowdyGameModelSessionTypes.h"
#include "CrowdyGameModelSessionActions.generated.h"

// CreateSession and SetSessionTurn both resolve to one session (or a default-constructed one on failure); one
// delegate type covers both Succeeded and Failed for each.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FCrowdySessionOutcome, FCrowdyGameModelSession, Session);
// JoinSession's façade is bOk-only; there is no payload to carry.
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FCrowdyJoinSessionOutcome);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FCrowdySessionsOutcome, TArray<FCrowdyGameModelSession>, Sessions);

/** Creates a new Game Model session (a match, battle, or room scope) that containers can attach to. */
UCLASS()
class CROWDYREPLICATION_API UCrowdyCreateSessionAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Sessions & Turns")
	FCrowdySessionOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Sessions & Turns")
	FCrowdySessionOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Sessions & Turns", DisplayName = "Create Game Session")
	static UCrowdyCreateSessionAction* CreateSession(UObject* WorldContext, const FString& Name,
		const TArray<int64>& ParticipantUserIds, const FString& MetadataJson);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString Name;
	TArray<int64> ParticipantUserIds;
	FString MetadataJson;
};

/** Joins the local user into an existing session, optionally under a role label. */
UCLASS()
class CROWDYREPLICATION_API UCrowdyJoinSessionAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Sessions & Turns")
	FCrowdyJoinSessionOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Sessions & Turns")
	FCrowdyJoinSessionOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Sessions & Turns", DisplayName = "Join Game Session")
	static UCrowdyJoinSessionAction* JoinSession(UObject* WorldContext, const FString& SessionId, const FString& Role = FString());

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString SessionId;
	FString Role;
};

/**
 * Sets (bClearTurn = false) or clears (bClearTurn = true) whose turn it is in a session; bClearTurn maps to
 * the façade's bHasUserId (inverted: clearing means bHasUserId=false, so the server nulls the turn holder).
 * The returned session carries the new turn holder either way.
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdySetSessionTurnAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Sessions & Turns")
	FCrowdySessionOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Sessions & Turns")
	FCrowdySessionOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Sessions & Turns", DisplayName = "Set Game Session Turn")
	static UCrowdySetSessionTurnAction* SetSessionTurn(UObject* WorldContext, const FString& SessionId,
		int64 UserId, bool bClearTurn = false);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString SessionId;
	int64 UserId = 0;
	bool bClearTurn = false;
};

/** Lists sessions, optionally filtered by Status (server-defined; empty = all). */
UCLASS()
class CROWDYREPLICATION_API UCrowdyListSessionsAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Sessions & Turns")
	FCrowdySessionsOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Sessions & Turns")
	FCrowdySessionsOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Sessions & Turns", DisplayName = "List Game Sessions")
	static UCrowdyListSessionsAction* ListSessions(UObject* WorldContext, const FString& Status = FString());

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString Status;
};

/** Reads one session by id: its status, current turn holder, and metadata. */
UCLASS()
class CROWDYREPLICATION_API UCrowdyGetSessionAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Sessions & Turns")
	FCrowdySessionOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Sessions & Turns")
	FCrowdySessionOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Sessions & Turns", DisplayName = "Get Game Session")
	static UCrowdyGetSessionAction* GetSession(UObject* WorldContext, const FString& SessionId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString SessionId;
};
