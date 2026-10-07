// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * One property write the server applied inside a gameModelInvoke transaction. Values ride as
 * JSON-encoded strings (the *Json fields) exactly as the Game API returns them; callers JSON-parse
 * each value string against the property's declared type.
 */
struct FCrowdyMutationApplied
{
	// The container this write landed on, which is NOT always the invoke's own container: an effect may write
	// source.<attr> as well as self.<attr>, and each mutation names its own destination. Empty only when the
	// server predates the field, in which case the caller must fall back to the invoke's own container.
	FString ContainerId;
	FString Key;
	FString OldValueJson;
	FString NewValueJson;
};

/**
 * A runtime gameModelInvoke request. AppId is an int64 here but is serialized into the request as a
 * JSON STRING the Game API types appId as a BigInt! scalar and rejects a JSON number. The request
 * builder enforces that; do not stringify at call sites.
 */
struct FCrowdyInvokeRequest
{
	int64 AppId = 0;
	FString FunctionName;
	FString SelfContainerId;
	FString SessionId;                 // empty => omitted from the request => app-global scope
	TSharedPtr<FJsonObject> Params;    // serialized to paramsJson; null => "{}"
};

/**
 * Why a platform-attributed invoke failure happened, mirroring the server's PlayerFaultInfo.blame. Platform is the
 * SDK's or the infrastructure's own fault (a caller may reasonably retry the identical call); Author is the app's
 * own authored logic or policy (an identical retry fails identically); Budget is a spent allowance (nothing is
 * broken, but retrying will not help either). Unknown is not a fourth server value: it is what a caller sees when
 * no fault was reported at all, so Unknown must never be treated as license to retry.
 */
enum class ECrowdyPlayerFaultBlame : uint8
{
	Unknown,
	Platform,
	Author,
	Budget
};

// The server's blame vocabulary as an enum. Anything unrecognised, including an empty string, is Unknown, which is
// the only safe reading: an unattributed failure must never be treated as a licensed retry. Declared here so the
// in-band fault parse and the thrown-error path share one spelling of the vocabulary rather than each carrying a copy.
CROWDYNET_API ECrowdyPlayerFaultBlame CrowdyPlayerFaultBlameFromWireString(const FString& Wire);

// The canonical word for Blame: the server's spelling for a known value, "unknown" for Unknown.
CROWDYNET_API const TCHAR* CrowdyPlayerFaultBlameToWord(ECrowdyPlayerFaultBlame Blame);

/**
 * The parsed outcome of a gameModelInvoke. bTransportOk and bSuccess are deliberately separate so a
 * rolled-back invoke is never mistaken for a network failure:
 *   - bTransportOk == false                    : the request never reached the server, the HTTP call
 *                                                failed, or the GraphQL envelope carried errors[].
 *                                                ErrorMessage carries the first transport/GraphQL error.
 *   - bTransportOk == true,  bSuccess == false : the server ran the function, its logic or authority
 *                                                check failed, the transaction was rolled back, and
 *                                                ErrorMessage says why. NOT a network error do not
 *                                                retry it as one.
 *   - bTransportOk == true,  bSuccess == true  : committed; Mutations lists the applied writes.
 *
 * Blame and bRetryable answer a separate question from bTransportOk/bSuccess: whether the SAME call is worth
 * trying again. Only Blame == Platform with bRetryable == true is safe to retry unchanged; Author means the
 * identical call will fail identically, and Budget means nothing is broken but retrying spends nothing either.
 *
 * They arrive on two different channels and a caller should not care which. An authority denial or an evaluation
 * failure comes back in band (bTransportOk true, bSuccess false) carrying the server's PlayerFaultInfo. The
 * overload refusal, where writes to one hot container are refused rather than served late, is a THROWN GraphQL
 * error instead: bTransportOk false, no Mutations, and the same attribution in errors[].extensions.
 *
 * bRetryable is true only where the server both said so and said whose fault it was, so an unattributed network
 * failure reads as Unknown / false. That matters here more than on a read: an invoke may have committed before the
 * failure was reported, so repeating one on no information can apply a write twice.
 */
struct FCrowdyInvokeResult
{
	bool bTransportOk = false;
	bool bSuccess = false;
	FString ReturnValueJson;
	FString ErrorMessage;
	TArray<FCrowdyMutationApplied> Mutations;
	ECrowdyPlayerFaultBlame Blame = ECrowdyPlayerFaultBlame::Unknown;
	bool bRetryable = false;
	FString FaultCode;

	// The server's own retry-after for a refusal that named one, in milliseconds. Set only on the thrown channel:
	// PlayerFaultInfo carries no timing field, so an in-band fault never has one and its retryable:true means "try
	// again", not "try again after N milliseconds".
	//
	// Only the CrowdyCPP path fills it. ParseInvokeEnvelope below cannot: it is handed the thrown channel as a flat
	// array of message strings with the extensions already discarded, so a result from that parser reads unset for
	// "this parser never saw it" rather than for "the server named no wait".
	//
	// Unset is not zero. Zero says the window has already rolled; unset says the server named no wait and the caller
	// owes it a local backoff. Read it as a deadline from receipt rather than an interval to reuse: it is what
	// REMAINED of a fixed window when the refusal was built, so a cached one is always too long.
	TOptional<int64> RetryAfterMs;

	// When the attempt that produced this result was sent, in the Game Model subsystem's local send order; 0 if unknown.
	uint64 DispatchSequence = 0;

	// How many times the Game Model subsystem sent this invoke, first send included: 1 on a busy refusal means it was never retried; 0 if never sent or not counted.
	int32 Attempts = 0;
};

/**
 * One runtime session (GmSession). BigInt user ids arrive as JSON strings on the wire and are parsed to int64;
 * bHasCurrentTurn distinguishes "no one's turn" (currentTurnUserId is null) from a real turn holder, so a
 * caller need not assume 0 is impossible as a user id. bHasMaxParticipants and bHasHost likewise separate a
 * null (unbounded / nobody joined) from a real 0. Revision rides the wire as a decimal string and compares as
 * an integer.
 */
struct FCrowdyGameSessionData
{
	FString SessionId;
	int64   AppId = 0;
	FString Name;
	FString Status;
	int64   CreatedByUserId = 0;
	int64   CurrentTurnUserId = 0;
	bool    bHasCurrentTurn = false;
	FString MetadataJson;
	FString Admission;               // "open" | "locked" | "closed"
	int32   MaxParticipants = 0;     // meaningful only when bHasMaxParticipants (null = unbounded)
	bool    bHasMaxParticipants = false;
	int32   ParticipantCount = 0;
	int64   HostUserId = 0;          // meaningful only when bHasHost (null while nobody is joined)
	bool    bHasHost = false;
	int32   HostTerm = 0;
	int64   Revision = 0;
	FString EndedAt;                 // empty while active
	FString EndReason;
	FString CreatedAt;
	FString Presence;                // "actor" | "none"
	int32   SeededContainerCount = 0; // meaningful only when bHasSeededContainerCount (the create response carries it; every other read is null)
	bool    bHasSeededContainerCount = false;
};

/** One session membership row (GmSessionParticipant). Incarnation increments on every re-join and gates a leave. */
struct FCrowdyGameSessionParticipantData
{
	FString SessionId;
	int64   UserId = 0;
	FString Role;
	FString State;                   // "joined" | "left"
	int32   Incarnation = 0;
	FString ActorUuid;
	FString JoinedAt;
	FString LeftAt;
	FString LeftReason;
};

/** One entry of a session's ordered change log (GmSessionEvent); Revision is the log position. */
struct FCrowdyGameSessionEventData
{
	int64   AppId = 0;
	FString SessionId;
	int64   Revision = 0;
	FString Kind;
	FString PayloadJson;
	FString CreatedAt;
};

/** A session plus its participants as of Revision (GmSessionSnapshot), the resync point for the event stream. */
struct FCrowdyGameSessionSnapshotData
{
	FCrowdyGameSessionData Session;
	TArray<FCrowdyGameSessionParticipantData> Participants;
	int64 Revision = 0;
};

/** Optional create-session inputs. Each is omitted from the request, so the server default applies, at the noted sentinel. */
struct FCrowdyCreateSessionOptions
{
	int32   MaxParticipants = 0;     // <= 0 omitted (unbounded)
	FString Admission;               // empty omitted ("open")
	int32   EmptyTimeoutSec = -1;    // < 0 omitted (platform default); 0 disables the timeout
	FString Presence;                // empty omitted ("actor")
	FString IdempotencyKey;          // empty omitted
	TArray<FString> SeedFromAppTypeNames; // empty omitted; otherwise the new session copies every keyed app-scoped row of these types
	FString SeedInitialState;        // "defaults" | "app"; empty omitted ("defaults"); only sent with a non-empty type list
};

/** One directed relationship edge between two containers (GmEdge). bHasWeight separates weight 0 from absent. */
struct FCrowdyEdgeData
{
	FString EdgeId;
	FString FromContainerId;
	FString ToContainerId;
	FString RelationshipType;
	double  Weight = 0.0;
	bool    bHasWeight = false;
};

/**
 * A container-graph traversal (GmTraverseResult): the reachable container nodes (each the raw GmContainer JSON,
 * exactly as a container listing returns) plus the edges walked. Server clamps depth to a maximum of 5.
 */
struct FCrowdyTraverseData
{
	FString RootId;
	TArray<TSharedPtr<FJsonObject>> Nodes;
	TArray<FCrowdyEdgeData> Edges;
};

/**
 * Marshalling for the runtime Game Model surface of the Game API (invoke, container reads and writes,
 * sessions and turns, the container graph). It turns typed arguments into a GraphQL variables object and
 * turns a GraphQL response envelope into a typed result; it never makes a call, so the caller owns the
 * transport.
 *
 * Every member is a pure static with no networking or world dependency, so headless tests exercise the wire
 * contract (appId-as-string, transport-vs-logic discrimination, propertiesJson decoding) with canned JSON.
 *
 * Wire contract shared by every operation: appId is a BigInt! and is emitted as a JSON STRING, never a
 * number, and so is every other BigInt id (user ids, participant lists, turn holder); nullable inputs are
 * omitted entirely when empty. Each parser takes the envelope plus the transport's 2xx flag (bHttpOk) and
 * the envelope's errors[] (TransportErrors); a non-2xx call or any GraphQL error fails the parse.
 */
class CROWDYNET_API FCrowdyGameApiCodec
{
public:
	// Builds the { input: { appId, functionName, selfContainerId, sessionId?, paramsJson } } variables
	// object for gameModelInvoke. appId is written as a JSON STRING (BigInt!); sessionId is omitted when
	// empty (=> app-global); paramsJson is the compact-serialized Params (or "{}" when null).
	static TSharedPtr<FJsonObject> BuildInvokeVariables(const FCrowdyInvokeRequest& Req);

	// Parses a whole gameModelInvoke GraphQL envelope. See FCrowdyInvokeResult for the discrimination
	// contract between a transport failure and a rolled-back invoke.
	static FCrowdyInvokeResult ParseInvokeEnvelope(const TSharedPtr<FJsonObject>& Envelope,
		bool bHttpOk, const TArray<FString>& TransportErrors);

	// Parses gameModelContainerState's envelope: reads data.gameModelContainerState.propertiesJson
	// (a JSON-encoded string) and returns it decoded into OutState. False on a missing/!2xx/malformed
	// envelope, a null container, or unparseable propertiesJson.
	static bool ParseContainerStateEnvelope(const TSharedPtr<FJsonObject>& Envelope,
		bool bHttpOk, const TArray<FString>& TransportErrors, TSharedPtr<FJsonObject>& OutState);

	// Parses gameModelContainers' envelope: reads data.gameModelContainers[] into OutContainers (each
	// the raw container object incl. containerId / ownerUserId / metadataJson). False on !2xx/malformed.
	static bool ParseContainersEnvelope(const TSharedPtr<FJsonObject>& Envelope,
		bool bHttpOk, const TArray<FString>& TransportErrors, TArray<TSharedPtr<FJsonObject>>& OutContainers);

	// Builds the { input: { appId, typeName, displayName, sessionId?, metadataJson? } } variables for
	// gameModelCreateContainer. appId is a JSON STRING (BigInt!); sessionId and metadataJson are omitted
	// when empty; displayName falls back to TypeName when empty (the field is non-null on the server).
	// ownerUserId is NEVER written: the server defaults it to the authenticated caller for member/owner
	// instantiation, so a client can never claim another user's container (fail-safe by omission).
	static TSharedPtr<FJsonObject> BuildCreateContainerVariables(int64 AppId, const FString& TypeName,
		const FString& DisplayName, const FString& SessionId, const FString& MetadataJson);

	// Parses gameModelCreateContainer's envelope: reads data.gameModelCreateContainer.containerId (+
	// ownerUserId). False on a missing/!2xx/malformed envelope or a null/absent container id.
	static bool ParseCreateContainerEnvelope(const TSharedPtr<FJsonObject>& Envelope,
		bool bHttpOk, const TArray<FString>& TransportErrors, FString& OutContainerId, int64& OutOwnerUserId);

	// Builds { input: { appId, typeName, bindingKey, displayName, sessionId?, metadataJson? } } for
	// gameModelEnsureContainer, the atomic get-or-create keyed by bindingKey within (appId, typeName,
	// sessionId). appId is a JSON STRING (BigInt!); bindingKey and displayName are non-null on the server
	// (displayName falls back to TypeName); sessionId and metadataJson are omitted when empty and are
	// ignored by the server when the row already exists. ownerUserId is NEVER written: the server pins it
	// to the caller for member/owner types and to null for admin types.
	static TSharedPtr<FJsonObject> BuildEnsureContainerVariables(int64 AppId, const FString& TypeName,
		const FString& BindingKey, const FString& DisplayName, const FString& SessionId, const FString& MetadataJson);

	// Parses gameModelEnsureContainer's envelope: reads data.gameModelEnsureContainer.container.containerId (+
	// ownerUserId) and .created (whether this call inserted the row). False on a missing/!2xx/malformed
	// envelope or a null/absent container id.
	static bool ParseEnsureContainerEnvelope(const TSharedPtr<FJsonObject>& Envelope,
		bool bHttpOk, const TArray<FString>& TransportErrors, FString& OutContainerId, int64& OutOwnerUserId,
		bool& OutCreated);

	// Builds { appId (JSON string), typeName, bindingKey, sessionId? } top-level variables for the
	// gameModelContainers get-by-key read: the single row ensured under BindingKey within (appId, typeName,
	// sessionId), or none.
	static TSharedPtr<FJsonObject> BuildReadContainerByKeyVariables(int64 AppId, const FString& TypeName,
		const FString& SessionId, const FString& BindingKey);

	// Parses the get-by-key envelope: the row of data.gameModelContainers whose bindingKey matches
	// ExpectedBindingKey, if any (a drifted server that ignores the filter and returns unrelated rows is rejected).
	// bOk=false on a !2xx/malformed envelope; bOk=true with OutFound=false when no matching row exists (a clean
	// "not yet created").
	static bool ParseReadContainerByKeyEnvelope(const TSharedPtr<FJsonObject>& Envelope,
		bool bHttpOk, const TArray<FString>& TransportErrors, const FString& ExpectedBindingKey, bool& OutFound,
		FString& OutContainerId, int64& OutOwnerUserId);

	// One row of a paged gameModelContainers list, identity only. SessionId is empty for an app-global row.
	struct FContainerRow
	{
		FString ContainerId;
		FString TypeName;
		FString BindingKey;
		FString SessionId;
		int64 OwnerUserId = 0;
	};

	// The server refuses a gameModelContainers limit above this.
	static constexpr int32 MaxContainersPerPage = 1000;

	// Builds { appId, typeName?, sessionId?, limit, offset } for one page of a type's rows, or of every type's when
	// typeName is empty. sessionId is omitted when empty, which the server reads as "every scope", so the caller
	// keeps only rows whose own sessionId matches. limit is always sent and clamped to [1, MaxContainersPerPage]: an
	// unbounded page is what makes a large type unsafe.
	static TSharedPtr<FJsonObject> BuildListContainersByTypeVariables(int64 AppId, const FString& TypeName,
		const FString& SessionId, int32 Limit, int32 Offset);

	// Parses one page into rows; a row with no containerId or no bindingKey is dropped, and OutRawRowCount says how
	// many the server sent before the drop, which is what decides whether the page was full. bOk=false on a
	// !2xx/malformed envelope; bOk=true with no rows is an empty page.
	static bool ParseContainerRowsEnvelope(const TSharedPtr<FJsonObject>& Envelope, bool bHttpOk,
		const TArray<FString>& TransportErrors, TArray<FContainerRow>& OutRows, int32* OutRawRowCount = nullptr);

	// One row of a gameModelContainerStates read: identity plus the decoded visible properties.
	struct FContainerStateRow
	{
		FString ContainerId;
		FString TypeName;
		FString SessionId;      // empty for an app-scoped row
		int64 OwnerUserId = 0;  // 0 when null
		TSharedPtr<FJsonObject> State; // the decoded propertiesJson object; null when it did not parse
	};

	// The server serves at most this many ids per gameModelContainerStates call; a caller chunks a larger set.
	static constexpr int32 MaxContainerStatesPerCall = 500;

	// Builds the top-level { appId (BigInt string), containerIds: [String!]! } variables for one bulk state read.
	// Empty ids are dropped; duplicates are passed through (the server answers each once, in input order).
	static TSharedPtr<FJsonObject> BuildContainerStatesVariables(int64 AppId, const TArray<FString>& ContainerIds);

	// Parses data.gameModelContainerStates[] into rows. Unknown or invisible ids are simply absent, so a caller
	// matches rows back by ContainerId rather than by position. A row without a containerId is dropped; a row
	// whose propertiesJson does not parse is kept with a null State. bOk=false on a !2xx/malformed envelope.
	static bool ParseContainerStatesEnvelope(const TSharedPtr<FJsonObject>& Envelope, bool bTransportOk,
		const TArray<FString>& TransportErrors, TArray<FContainerStateRow>& OutRows);

	// Session lifecycle: create, join, leave, admission, host transfer, end, set/clear the turn, list, read one,
	// snapshot, and the event log. Int inputs (maxParticipants, emptyTimeoutSec, incarnation, expectedHostTerm,
	// limit) are JSON NUMBERS; every id is a BigInt STRING; afterRevision is a STRING holding the integer.

	// participantUserIds is a JSON array of BigInt STRINGS; name/metadataJson omitted when empty; each option
	// is omitted at its sentinel (see FCrowdyCreateSessionOptions).
	static TSharedPtr<FJsonObject> BuildCreateSessionVariables(int64 AppId, const FString& Name,
		const TArray<int64>& ParticipantUserIds, const FString& MetadataJson,
		const FCrowdyCreateSessionOptions& Options = FCrowdyCreateSessionOptions());
	// role, actorUuid (the caller's own actor, 32 hex) and idempotencyKey are omitted when empty.
	static TSharedPtr<FJsonObject> BuildJoinSessionVariables(int64 AppId, const FString& SessionId, const FString& Role,
		const FString& ActorUuid = FString(), const FString& IdempotencyKey = FString());
	// incarnation is required (a JSON number): the value the join returned, so a stale leave is refused.
	static TSharedPtr<FJsonObject> BuildLeaveSessionVariables(int64 AppId, const FString& SessionId, int32 Incarnation,
		const FString& IdempotencyKey = FString());
	// expectedHostTerm (a JSON number) is omitted when <= 0 on each host-gated mutation below.
	static TSharedPtr<FJsonObject> BuildSetSessionAdmissionVariables(int64 AppId, const FString& SessionId,
		const FString& Admission, int32 ExpectedHostTerm);
	// toUserId is a BigInt string.
	static TSharedPtr<FJsonObject> BuildTransferSessionHostVariables(int64 AppId, const FString& SessionId,
		int64 ToUserId, int32 ExpectedHostTerm);
	// reason ("completed" | "abandoned") is omitted when empty.
	static TSharedPtr<FJsonObject> BuildEndSessionVariables(int64 AppId, const FString& SessionId,
		const FString& Reason, int32 ExpectedHostTerm);
	// userId is a BigInt string when bHasUserId; otherwise it is written as an explicit JSON null, which clears
	// the turn (the schema distinguishes an absent field, "unchanged", from an explicit null, "clear").
	static TSharedPtr<FJsonObject> BuildSetSessionTurnVariables(int64 AppId, const FString& SessionId,
		int64 UserId, bool bHasUserId, int32 ExpectedHostTerm = 0);
	// { appId (BigInt string), status?, admission?, hostUserId? (BigInt string), limit? (number) } top-level
	// variables; each filter is omitted when empty / 0.
	static TSharedPtr<FJsonObject> BuildListSessionsVariables(int64 AppId, const FString& Status,
		const FString& Admission = FString(), int64 HostUserId = 0, int32 Limit = 0);
	// { appId (BigInt string), sessionId } top-level variables.
	static TSharedPtr<FJsonObject> BuildGetSessionVariables(int64 AppId, const FString& SessionId);
	static TSharedPtr<FJsonObject> BuildSessionSnapshotVariables(int64 AppId, const FString& SessionId);
	// afterRevision is written as a STRING ("0" reads the whole log); limit (a number) is omitted when <= 0.
	static TSharedPtr<FJsonObject> BuildSessionEventsVariables(int64 AppId, const FString& SessionId,
		int64 AfterRevision, int32 Limit);
	// Subscription variables; afterRevision (a string) is omitted when !bHasAfterRevision.
	static TSharedPtr<FJsonObject> BuildSessionChangedVariables(int64 AppId, const FString& SessionId,
		int64 AfterRevision, bool bHasAfterRevision);

	// Parses one GmSession JSON object (BigInt fields read defensively as string-or-number, Int fields as
	// number-or-string). Never fails; an absent or null field keeps its default and leaves its bHas flag false.
	static FCrowdyGameSessionData ParseSessionObject(const TSharedPtr<FJsonObject>& SessionObj);
	static FCrowdyGameSessionParticipantData ParseSessionParticipantObject(const TSharedPtr<FJsonObject>& Obj);
	static FCrowdyGameSessionEventData ParseSessionEventObject(const TSharedPtr<FJsonObject>& Obj);
	// Envelope parsers for the single-session ops. FieldName picks the mutation/query field to read
	// ("gameModelCreateSession" / "gameModelSetSessionTurn" / "gameModelSetSessionAdmission" /
	// "gameModelTransferSessionHost" / "gameModelEndSession" / "gameModelSession").
	static bool ParseSessionEnvelope(const TSharedPtr<FJsonObject>& Envelope, bool bHttpOk,
		const TArray<FString>& TransportErrors, const TCHAR* FieldName, FCrowdyGameSessionData& OutSession);
	static bool ParseSessionsEnvelope(const TSharedPtr<FJsonObject>& Envelope, bool bHttpOk,
		const TArray<FString>& TransportErrors, TArray<FCrowdyGameSessionData>& OutSessions);
	// Reads data.gameModelJoinSession, a participant row.
	static bool ParseJoinSessionEnvelope(const TSharedPtr<FJsonObject>& Envelope, bool bHttpOk,
		const TArray<FString>& TransportErrors, FCrowdyGameSessionParticipantData& OutParticipant);
	// FieldName picks "gameModelJoinSession" / "gameModelLeaveSession".
	static bool ParseSessionParticipantEnvelope(const TSharedPtr<FJsonObject>& Envelope, bool bHttpOk,
		const TArray<FString>& TransportErrors, const TCHAR* FieldName, FCrowdyGameSessionParticipantData& OutParticipant);
	// Reads data.gameModelSessionSnapshot { revision, session, participants[] }.
	static bool ParseSessionSnapshotEnvelope(const TSharedPtr<FJsonObject>& Envelope, bool bHttpOk,
		const TArray<FString>& TransportErrors, FCrowdyGameSessionSnapshotData& OutSnapshot);
	// Reads data.gameModelSessionEvents[] in log order.
	static bool ParseSessionEventsEnvelope(const TSharedPtr<FJsonObject>& Envelope, bool bHttpOk,
		const TArray<FString>& TransportErrors, TArray<FCrowdyGameSessionEventData>& OutEvents);

	// Container graph: directed edges and traversals over them (inventories, chests, tech trees).

	// weight (a JSON number) is written only when bHasWeight; metadataJson omitted when empty.
	static TSharedPtr<FJsonObject> BuildAddEdgeVariables(int64 AppId, const FString& FromContainerId,
		const FString& ToContainerId, const FString& RelationshipType, double Weight, bool bHasWeight,
		const FString& MetadataJson);
	// depth is a JSON NUMBER (Int!), not a string; clamped to [1,5].
	static TSharedPtr<FJsonObject> BuildTraverseVariables(int64 AppId, const FString& RootId,
		const FString& RelationshipType, int32 Depth);

	static bool ParseAddEdgeEnvelope(const TSharedPtr<FJsonObject>& Envelope, bool bHttpOk,
		const TArray<FString>& TransportErrors, FCrowdyEdgeData& OutEdge);
	static bool ParseTraverseEnvelope(const TSharedPtr<FJsonObject>& Envelope, bool bHttpOk,
		const TArray<FString>& TransportErrors, FCrowdyTraverseData& OutResult);
	// Parses one GmEdge JSON object (weight read as an optional number).
	static FCrowdyEdgeData ParseEdgeObject(const TSharedPtr<FJsonObject>& EdgeObj);

	// Direct property write.
	// gameModelSetProperty: a direct write to a property whose writability (owner/admin) permits the caller.
	// ValueJson is already a JSON-encoded value string ("\"Aria\"", "42") and is passed through verbatim.
	static TSharedPtr<FJsonObject> BuildSetPropertyVariables(int64 AppId, const FString& ContainerId,
		const FString& Key, const FString& ValueType, const FString& ValueJson);

	static bool ParseSetPropertyEnvelope(const TSharedPtr<FJsonObject>& Envelope, bool bHttpOk,
		const TArray<FString>& TransportErrors, FString& OutContainerId);

	// Player-plane deletes (owner-or-admin, server-enforced). Flat-arg mutations returning Boolean!
	// (true = deleted; false = it did not exist -- an idempotent no-op). An authorization/refusal failure arrives
	// as a GraphQL error, so the parse fails. `deleteContainer` cascades the container's properties AND every edge
	// connected to it; `deleteEdge` needs the source-container owner (or admin). The args are TOP-LEVEL variables
	// (no input object).
	// Top-level { appId (BigInt string), containerId/edgeId } variables.
	static TSharedPtr<FJsonObject> BuildDeleteContainerVariables(int64 AppId, const FString& ContainerId);
	static TSharedPtr<FJsonObject> BuildDeleteEdgeVariables(int64 AppId, const FString& EdgeId);
	// Reads data.gameModelDelete*(Boolean!) into OutDeleted; false return = transport/parse failure or a GraphQL
	// error (authorization/refusal), true = a clean transport with a boolean result.
	static bool ParseDeleteContainerEnvelope(const TSharedPtr<FJsonObject>& Envelope, bool bHttpOk,
		const TArray<FString>& TransportErrors, bool& OutDeleted);
	static bool ParseDeleteEdgeEnvelope(const TSharedPtr<FJsonObject>& Envelope, bool bHttpOk,
		const TArray<FString>& TransportErrors, bool& OutDeleted);
};
