// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CrowdyGameModelSessionTypes.generated.h"

/**
 * Blueprint-facing mirrors of the Game Model Tier B wire types (sessions, graph edges, free/data containers).
 * FCrowdyGameApiCodec (CrowdyNet) parses the GraphQL envelopes into its own plain
 * structs; UCrowdyGameModelSubsystem maps those into these BlueprintType structs at the façade boundary so
 * gameplay Blueprints see typed data, never raw JSON. BigInt user ids are surfaced as int64 (Blueprint's
 * Integer64); a user id is never 0 on the server, so 0 reads as "unset" for the optional turn holder, with an
 * explicit bHasCurrentTurn for callers that would rather not assume that.
 */

/** Where a session is in its life. Ended sessions stay readable; only Active ones accept joins and host actions. */
UENUM(BlueprintType)
enum class ECrowdySessionStatus : uint8
{
	Active,
	Completed,
	Abandoned,
	Unknown
};

/** Status filter for List Game Sessions. */
UENUM(BlueprintType)
enum class ECrowdySessionStatusFilter : uint8
{
	Active,
	Any,
	Completed,
	Abandoned
};

/** Who may still get in: Open (anyone the app admits), Locked (only players already in may reconnect), Closed (nobody). */
UENUM(BlueprintType)
enum class ECrowdySessionAdmission : uint8
{
	Open,
	Locked,
	Closed
};

/** Admission filter for List Game Sessions. */
UENUM(BlueprintType)
enum class ECrowdySessionAdmissionFilter : uint8
{
	Any,
	Open,
	Locked,
	Closed
};

/**
 * How the server decides a player is still there. Actor: a joined player with no fresh replicated actor in the
 * app is dropped from the roster after a grace period. None: nobody is ever dropped for absence, the roster's only
 * exits are leaving and ending; for turn-based play that never spawns an actor.
 */
UENUM(BlueprintType)
enum class ECrowdySessionPresence : uint8
{
	Actor,
	None
};

/**
 * The values the rows seeded into a new session start with. Defaults: the type's property defaults; App: a copy of
 * each app-scoped template row's current values.
 */
UENUM(BlueprintType)
enum class ECrowdySessionSeedState : uint8
{
	Defaults,
	App
};

/** Why a session ended. None while it is still active; EmptyTimeout when the server ended a session nobody was in. */
UENUM(BlueprintType)
enum class ECrowdySessionEndReason : uint8
{
	None,
	Completed,
	Abandoned,
	EmptyTimeout,
	Unknown
};

UENUM(BlueprintType)
enum class ECrowdySessionParticipantState : uint8
{
	Joined,
	Left,
	Unknown
};

/** Why a participant is no longer in the session. None while joined. */
UENUM(BlueprintType)
enum class ECrowdySessionLeftReason : uint8
{
	None,
	Left,
	PresenceExpired,
	SessionEnded,
	Kicked,
	Unknown
};

/** What a session change was. Unknown carries a kind this build does not know; its name is in KindName. */
UENUM(BlueprintType)
enum class ECrowdySessionEventKind : uint8
{
	Created,
	ParticipantJoined,
	ParticipantRejoined,
	ParticipantLeft,
	ParticipantExpired,
	HostChanged,
	AdmissionChanged,
	TurnChanged,
	Ended,
	Unknown
};

/**
 * Why a session call was refused, as something a Blueprint can switch on. Other covers a transport failure or a
 * server code this build does not know; the exact code and the human-readable message ride alongside.
 */
UENUM(BlueprintType)
enum class ECrowdySessionError : uint8
{
	None,
	NotSignedIn,
	NotAllowed,
	Full,
	Locked,
	Closed,
	Ended,
	NotParticipant,
	TargetNotParticipant,
	IncarnationStale,
	HostTermStale,
	Other
};

/** What a Failed pin carries: the refusal as an enum, the server's exact code, and its message for a human. */
USTRUCT(BlueprintType)
struct FCrowdyModelFailure
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	ECrowdySessionError Error = ECrowdySessionError::None;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	FString Code;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	FString Message;
};

/** One runtime session (a match, battle, or room scope) that containers can be attached to. */
USTRUCT(BlueprintType)
struct FCrowdyGameModelSession
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	FString SessionId;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	ECrowdySessionStatus Status = ECrowdySessionStatus::Unknown;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	int64 CreatedByUserId = 0;

	// The user whose turn it currently is, or 0 when no turn is set (see bHasCurrentTurn). Read is_current_turn
	// enforcement happens server-side; this is the client-side mirror a game gates input on.
	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	int64 CurrentTurnUserId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	bool bHasCurrentTurn = false;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model", AdvancedDisplay)
	FString MetadataJson;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	ECrowdySessionAdmission Admission = ECrowdySessionAdmission::Open;

	// The seat cap, meaningful only when bHasMaxParticipants; unbounded otherwise.
	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	int32 MaxParticipants = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	bool bHasMaxParticipants = false;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	int32 ParticipantCount = 0;

	// The host, meaningful only when bHasHost; nobody is host while no one is joined.
	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	int64 HostUserId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	bool bHasHost = false;

	// Increments on every host change. The SDK remembers the last one it read per session and sends it with host
	// actions, so a command from a host that has been replaced is refused instead of applied.
	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model", AdvancedDisplay)
	int32 HostTerm = 0;

	// The session's change-log position; a gap between two observed revisions means pull the snapshot again.
	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model", AdvancedDisplay)
	int64 Revision = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	ECrowdySessionEndReason EndReason = ECrowdySessionEndReason::None;

	// Empty while the session is active.
	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model", AdvancedDisplay)
	FString EndedAt;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model", AdvancedDisplay)
	FString CreatedAt;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	ECrowdySessionPresence Presence = ECrowdySessionPresence::Actor;

	// How many containers the create seeded from the app's templates, meaningful only when bHasSeededContainerCount:
	// the create response carries it and every other read leaves it unset.
	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model", AdvancedDisplay)
	int32 SeededContainerCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model", AdvancedDisplay)
	bool bHasSeededContainerCount = false;
};

/** One membership row of a session: who joined, in which role, and the incarnation a leave must name. */
USTRUCT(BlueprintType)
struct FCrowdyGameModelSessionParticipant
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	FString SessionId;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	int64 UserId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	FString Role;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	ECrowdySessionParticipantState State = ECrowdySessionParticipantState::Unknown;

	// Increments on every rejoin; a leave is refused unless it names the current one. The SDK remembers it.
	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model", AdvancedDisplay)
	int32 Incarnation = 0;

	// The replicated actor this player's presence follows, or empty when any of their actors counts.
	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model", AdvancedDisplay)
	FString ActorUuid;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model", AdvancedDisplay)
	FString JoinedAt;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model", AdvancedDisplay)
	FString LeftAt;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	ECrowdySessionLeftReason LeftReason = ECrowdySessionLeftReason::None;
};

/**
 * One session change. Kind says what happened; the typed fields below are filled from the change's detail when
 * the server sent it (UserId for a join / leave / expiry, HostUserId and PreviousHostUserId for a host change,
 * Admission for an admission change). A channel cue only says that something changed: bIsCue is true, the typed
 * fields are empty, and Get Game Session Snapshot has the detail.
 */
USTRUCT(BlueprintType)
struct FCrowdyGameModelSessionEvent
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	FString SessionId;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	int64 Revision = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	ECrowdySessionEventKind Kind = ECrowdySessionEventKind::Unknown;

	// The server's word for Kind; the only place an Unknown kind is readable.
	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model", AdvancedDisplay)
	FString KindName;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	bool bIsCue = false;

	// The player a join, rejoin, leave, expiry or turn change is about; 0 when the change names nobody.
	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	int64 UserId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	int64 HostUserId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	int64 PreviousHostUserId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	ECrowdySessionAdmission Admission = ECrowdySessionAdmission::Open;

	// The server's reason word for a host change or an end (host_left, completed, ...), empty when it sent none.
	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model", AdvancedDisplay)
	FString ReasonName;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model", AdvancedDisplay)
	int32 Incarnation = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model", AdvancedDisplay)
	int32 HostTerm = 0;

	// The change's full detail as the server sent it, for anything the typed fields do not carry.
	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model", AdvancedDisplay)
	FString PayloadJson;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model", AdvancedDisplay)
	FString CreatedAt;
};

/** A session and its participants as of Revision, the resync point after a missed event. */
USTRUCT(BlueprintType)
struct FCrowdyGameModelSessionSnapshot
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	FCrowdyGameModelSession Session;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	TArray<FCrowdyGameModelSessionParticipant> Participants;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model", AdvancedDisplay)
	int64 Revision = 0;
};

/** Optional inputs to Create Game Session. The defaults are the server's. */
USTRUCT(BlueprintType)
struct FCrowdyGameModelCreateSessionOptions
{
	GENERATED_BODY()

	// 0 means no seat cap.
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Crowdy SDK|Game Model")
	int32 MaxParticipants = 0;

	// Locked lets only the initial participants in until the host opens it.
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Crowdy SDK|Game Model")
	ECrowdySessionAdmission Admission = ECrowdySessionAdmission::Open;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Crowdy SDK|Game Model")
	ECrowdySessionPresence Presence = ECrowdySessionPresence::Actor;

	// Seconds a session may sit with nobody in it before the server ends it. -1 means the server default
	// (5 minutes); 0 means never.
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Crowdy SDK|Game Model", AdvancedDisplay)
	int32 EmptyTimeoutSec = -1;

	// Empty means none; a repeated create with the same key returns the first session instead of a second one.
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Crowdy SDK|Game Model", AdvancedDisplay)
	FString IdempotencyKey;

	// Container types whose keyed app-scoped rows are copied into the new session as it is created. Empty seeds
	// nothing. Each type must be admin-instantiable or carry a bind policy; the whole create is refused otherwise.
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Crowdy SDK|Game Model")
	TArray<FString> SeedFromAppTypeNames;

	// What the seeded copies start with; only sent when SeedFromAppTypeNames is non-empty.
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Crowdy SDK|Game Model")
	ECrowdySessionSeedState SeedInitialState = ECrowdySessionSeedState::Defaults;
};

/** A directed relationship edge between two containers (inventory -> item, chest -> contents, tech-tree link). */
USTRUCT(BlueprintType)
struct FCrowdyContainerEdge
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	FString EdgeId;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	FString FromContainerId;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	FString ToContainerId;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	FString RelationshipType;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	float Weight = 0.0f;
};

/**
 * A free/data container reference (a container with no actor: an inventory, an item, a quest, a match). Carries
 * the identity + ownership metadata a UI needs; its property values are read separately by containerId through
 * the subsystem's per-container cache (UCrowdyGameModel::GetContainerInt/Float/... ).
 */
USTRUCT(BlueprintType)
struct FCrowdyContainerRef
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	FString ContainerId;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	FString TypeName;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	FString DisplayName;

	// The owning user, or 0 when the container is unowned.
	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	int64 OwnerUserId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	FString SessionId;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	FString MetadataJson;
};

/**
 * One item of a Model Collection with its visible state fetched in the same call ("Get Collection With Items'
 * State"). ContainerId + TypeName identify the item; StateJson is its visible property set as a JSON object
 * string (read a scalar out of it with UCrowdyGameModel::GetItemInt/Float/Bool/String, or parse it for a rich
 * field). Unlike a plain Get Collection this carries the state, so a UI shows a whole bag in one round-trip.
 */
USTRUCT(BlueprintType)
struct FCrowdyCollectionItem
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	FString ContainerId;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	FString TypeName;

	// The item's visible properties as a JSON object string ({"quantity":5,"item_id":"sword"}); empty if the
	// item's state could not be read. Read a scalar with UCrowdyGameModel::GetItem* helpers.
	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	FString StateJson;
};
