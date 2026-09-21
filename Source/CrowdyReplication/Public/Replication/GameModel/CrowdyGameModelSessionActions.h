// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Replication/GameModel/CrowdyGameModelSessionTypes.h"
#include "CrowdyGameModelSessionActions.generated.h"

// Every Failed pin carries the same thing: why the server (or the SDK) refused, as an enum to switch on plus
// the exact code and a message for a human.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FCrowdySessionFailureOutcome, FCrowdyModelFailure, Failure);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FCrowdySessionOutcome, FCrowdyGameModelSession, Session);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FCrowdySessionParticipantOutcome, FCrowdyGameModelSessionParticipant, Participant);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FCrowdySessionsOutcome, const TArray<FCrowdyGameModelSession>&, Sessions);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FCrowdySessionSnapshotOutcome, FCrowdyGameModelSessionSnapshot, Snapshot);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FCrowdySessionEventsOutcome, const TArray<FCrowdyGameModelSessionEvent>&, Events);

/**
 * Creates a session (a match, room or lobby) with the local player as its first participant and host. By default
 * it becomes the active session, so later session nodes may leave their Session Id empty, and it is watched, so
 * On Game Session Changed fires for it with full detail.
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyCreateSessionAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Sessions & Turns")
	FCrowdySessionOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Sessions & Turns")
	FCrowdySessionFailureOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext",
		AutoCreateRefTerm = "Options,ParticipantUserIds", AdvancedDisplay = "ParticipantUserIds,MetadataJson"),
		Category = "Crowdy SDK|Game Model|Sessions & Turns", DisplayName = "Create Game Session")
	static UCrowdyCreateSessionAction* CreateSession(UObject* WorldContext, const FString& Name,
		const FCrowdyGameModelCreateSessionOptions& Options, const TArray<int64>& ParticipantUserIds,
		bool bMakeActive = true, bool bWatchForChanges = true, const FString& MetadataJson = FString());

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString Name;
	TArray<int64> ParticipantUserIds;
	FString MetadataJson;
	FCrowdyGameModelCreateSessionOptions Options;
	bool bMakeActive = true;
	bool bWatchForChanges = true;
};

/**
 * Joins the local player into a session; joining one you are already in reconnects you. By default it becomes the
 * active session and is watched. Succeeded carries your roster row; the SDK remembers what a later Leave needs.
 * Failed says why: Full, Locked, Closed, Ended...
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyJoinSessionAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Sessions & Turns")
	FCrowdySessionParticipantOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Sessions & Turns")
	FCrowdySessionFailureOutcome Failed;

	// Role is a free label your game gives the player (empty for the server's default). bBindPresenceToMyActor
	// ties this player's presence to their current replicated actor instead of any actor of theirs.
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext",
		AdvancedDisplay = "Role,bBindPresenceToMyActor"),
		Category = "Crowdy SDK|Game Model|Sessions & Turns", DisplayName = "Join Game Session")
	static UCrowdyJoinSessionAction* JoinSession(UObject* WorldContext, const FString& SessionId,
		bool bMakeActive = true, bool bWatchForChanges = true, const FString& Role = FString(),
		bool bBindPresenceToMyActor = false);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString SessionId;
	FString Role;
	bool bMakeActive = true;
	bool bWatchForChanges = true;
	bool bBindPresenceToMyActor = false;
};

/** Leaves a session you created or joined on this client, and stops watching it. */
UCLASS()
class CROWDYREPLICATION_API UCrowdyLeaveSessionAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Sessions & Turns")
	FCrowdySessionParticipantOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Sessions & Turns")
	FCrowdySessionFailureOutcome Failed;

	// Incarnation 0 means the one the SDK remembered from your Create or Join; only a client that read it from a
	// snapshot needs to pass one.
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext",
		AdvancedDisplay = "Incarnation"),
		Category = "Crowdy SDK|Game Model|Sessions & Turns", DisplayName = "Leave Game Session")
	static UCrowdyLeaveSessionAction* LeaveSession(UObject* WorldContext, const FString& SessionId = FString(),
		int32 Incarnation = 0);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString SessionId;
	int32 Incarnation = 0;
};

/**
 * Host only. Open lets anyone in, Locked lets only players already in reconnect (lock when the match starts),
 * Closed lets nobody in. With bRefuseIfHostChanged the call is refused (HostTermStale) if the host changed since
 * this client last read the session, so a replaced host never acts by mistake.
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdySetSessionAdmissionAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Sessions & Turns")
	FCrowdySessionOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Sessions & Turns")
	FCrowdySessionFailureOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext",
		AdvancedDisplay = "bRefuseIfHostChanged"),
		Category = "Crowdy SDK|Game Model|Sessions & Turns", DisplayName = "Set Game Session Admission")
	static UCrowdySetSessionAdmissionAction* SetSessionAdmission(UObject* WorldContext, ECrowdySessionAdmission Admission,
		const FString& SessionId = FString(), bool bRefuseIfHostChanged = true);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString SessionId;
	ECrowdySessionAdmission Admission = ECrowdySessionAdmission::Open;
	bool bRefuseIfHostChanged = true;
};

/** Host only. Hands the host role to another player who is in the session. */
UCLASS()
class CROWDYREPLICATION_API UCrowdyTransferSessionHostAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Sessions & Turns")
	FCrowdySessionOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Sessions & Turns")
	FCrowdySessionFailureOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext",
		AdvancedDisplay = "bRefuseIfHostChanged"),
		Category = "Crowdy SDK|Game Model|Sessions & Turns", DisplayName = "Transfer Game Session Host")
	static UCrowdyTransferSessionHostAction* TransferSessionHost(UObject* WorldContext, int64 ToUserId,
		const FString& SessionId = FString(), bool bRefuseIfHostChanged = true);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString SessionId;
	int64 ToUserId = 0;
	bool bRefuseIfHostChanged = true;
};

/** Host only. Ends the session for everyone (they are all marked left, nobody can join) and stops watching it. */
UCLASS()
class CROWDYREPLICATION_API UCrowdyEndSessionAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Sessions & Turns")
	FCrowdySessionOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Sessions & Turns")
	FCrowdySessionFailureOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext",
		AdvancedDisplay = "Reason,bRefuseIfHostChanged"),
		Category = "Crowdy SDK|Game Model|Sessions & Turns", DisplayName = "End Game Session")
	static UCrowdyEndSessionAction* EndSession(UObject* WorldContext, const FString& SessionId = FString(),
		ECrowdySessionEndReason Reason = ECrowdySessionEndReason::Completed, bool bRefuseIfHostChanged = true);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString SessionId;
	ECrowdySessionEndReason Reason = ECrowdySessionEndReason::Completed;
	bool bRefuseIfHostChanged = true;
};

/**
 * Sets whose turn it is (the turn holder, the host or an app admin may), or clears it with bClearTurn. The
 * returned session carries the new turn holder.
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdySetSessionTurnAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Sessions & Turns")
	FCrowdySessionOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Sessions & Turns")
	FCrowdySessionFailureOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext",
		AdvancedDisplay = "bRefuseIfHostChanged"),
		Category = "Crowdy SDK|Game Model|Sessions & Turns", DisplayName = "Set Game Session Turn")
	static UCrowdySetSessionTurnAction* SetSessionTurn(UObject* WorldContext, int64 UserId,
		const FString& SessionId = FString(), bool bClearTurn = false, bool bRefuseIfHostChanged = true);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString SessionId;
	int64 UserId = 0;
	bool bClearTurn = false;
	bool bRefuseIfHostChanged = true;
};

/** Lists the app's sessions, active ones by default. Nothing pushes a new session to a client: refresh to see it. */
UCLASS()
class CROWDYREPLICATION_API UCrowdyListSessionsAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Sessions & Turns")
	FCrowdySessionsOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Sessions & Turns")
	FCrowdySessionFailureOutcome Failed;

	// HostUserId 0 means any host; Limit 0 means the server's page size.
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext",
		AdvancedDisplay = "HostUserId,Limit"),
		Category = "Crowdy SDK|Game Model|Sessions & Turns", DisplayName = "List Game Sessions")
	static UCrowdyListSessionsAction* ListSessions(UObject* WorldContext,
		ECrowdySessionStatusFilter Status = ECrowdySessionStatusFilter::Active,
		ECrowdySessionAdmissionFilter Admission = ECrowdySessionAdmissionFilter::Any,
		int64 HostUserId = 0, int32 Limit = 0);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	ECrowdySessionStatusFilter Status = ECrowdySessionStatusFilter::Active;
	ECrowdySessionAdmissionFilter Admission = ECrowdySessionAdmissionFilter::Any;
	int64 HostUserId = 0;
	int32 Limit = 0;
};

/** Reads one session: who hosts it, how many are in, whose turn it is. */
UCLASS()
class CROWDYREPLICATION_API UCrowdyGetSessionAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Sessions & Turns")
	FCrowdySessionOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Sessions & Turns")
	FCrowdySessionFailureOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Sessions & Turns", DisplayName = "Get Game Session")
	static UCrowdyGetSessionAction* GetSession(UObject* WorldContext, const FString& SessionId = FString());

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString SessionId;
};

/** Reads a session with everyone in it: the roster read after a change, or when the change feed skipped ahead. */
UCLASS()
class CROWDYREPLICATION_API UCrowdyGetSessionSnapshotAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Sessions & Turns")
	FCrowdySessionSnapshotOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Sessions & Turns")
	FCrowdySessionFailureOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Sessions & Turns", DisplayName = "Get Game Session Snapshot")
	static UCrowdyGetSessionSnapshotAction* GetSessionSnapshot(UObject* WorldContext, const FString& SessionId = FString());

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString SessionId;
};

/** Reads a session's change history after a revision (0 for all of it). */
UCLASS()
class CROWDYREPLICATION_API UCrowdyGetSessionEventsAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Sessions & Turns")
	FCrowdySessionEventsOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Sessions & Turns")
	FCrowdySessionFailureOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext",
		AdvancedDisplay = "Limit"),
		Category = "Crowdy SDK|Game Model|Sessions & Turns", DisplayName = "Get Game Session Events")
	static UCrowdyGetSessionEventsAction* GetSessionEvents(UObject* WorldContext, const FString& SessionId = FString(),
		int64 AfterRevision = 0, int32 Limit = 0);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString SessionId;
	int64 AfterRevision = 0;
	int32 Limit = 0;
};
