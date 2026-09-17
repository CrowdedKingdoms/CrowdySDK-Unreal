// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Replication/GameModel/Effect/CrowdyGameModelFunctionInput.h" // FCrowdyGameModelNotification

// Plain editor-side records the console works with. They mirror the GraphQL response
// shapes closely enough that swapping to generated codegen stays a field-by-field copy.
// Not UStructs nothing here is reflected, replicated, or seen by Blueprint.

struct FStudioOrg
{
	int64 OrgId = 0;
	FString Name;
	FString Slug;

	// Effective permission keys the signed-in token holds in this org (union across its roles;
	// the full set for super admins). The console greys the management actions a token can't
	// perform, e.g. "manage_apps" gates creating an app, "manage_environments" gates linking one.
	TArray<FString> Permissions;
};

struct FStudioApp
{
	int64 AppId = 0;
	int64 OrgId = 0;
	FString Name;
	FString Slug;
	FString Status;
	FString Visibility;
	FString Description;
	FString OrgName;
	FString OrgSlug;
	FString CreatedAt;
	FString UpdatedAt;

	// Endpoints as the server reports them. WsUrl is never derived from HttpUrl by
	// string surgery it is populated from server data (the app/env or platformConfig).
	FString GameApiUrl;
	FString SplitMode;

	// From appDiscovery, not the app record: the shard's datacenter ("or", "va") and the WS endpoint. Empty
	// until discovery answers; a list refetch keeps them.
	FString DatacenterCode;
	FString GameApiWsUrl;

	// Only the single-app query returns these; the list leaves them empty and a refetch keeps them.
	FString DeploymentTarget;
	FString RuntimeStatus;
	FString RuntimeDenialReason;
	int64 ReservedUdpBytesPerSec = 0;
	int64 ReservedGraphqlOpsPerSec = 0;
};

// One datacenter this deployment can place a new app in (placeableDatacenters).
struct FStudioDatacenter
{
	FString Code;
	FString GameApiUrl;
	FString GameApiWsUrl;
	bool bPlaceable = false;
	// DatacenterServingStatus as sent: SERVING, NOT_SERVING or UNKNOWN. Kept as text because UNKNOWN is
	// not an outage and must never be shown as one.
	FString Serving;
	int32 AppShardCount = 0;
};

// A read-back of the live UCrowdySDKDeveloperSettings, used by the Config view to show
// the before/after diff without the UI reaching into the settings object directly.
struct FStudioSettingsSnapshot
{
	int64 AppId = 0;
	int64 OrgId = 0;
	FString DiscoveryUrl;
	FString GameApiHttpUrl;
	FString GameApiWsUrl;
};

// The per-app team/channel creation + default-membership policy. Both teams and channels use the
// same shape (group_type distinguishes them); set on the game plane (needs a game-capable token).
struct FStudioGroupPolicy
{
	int64 AppId = 0;
	FString GroupType;
	FString CreationPolicy;
	FString DefaultMembershipPolicy;
	int32 MaxMembers = 0;
	int32 MaxGroupsPerUser = 0;
};

// A team or channel as the game plane reports it, for the read/list parts of the editor views.
struct FStudioGroup
{
	int64 GroupId = 0;
	FString Name;
	FString Description;
	FString GroupType;
	FString MembershipPolicy;
	FString Status;
};

// Which kind of group a detail/edit operation is for. Teams and channels share the same shape on the
// wire but route to different ops (teamMembers vs channelMembers, createTeamRole vs createChannelRole),
// so the controller takes this to pick the right operation instead of duplicating every method.
enum class ECrowdyGroupKind : uint8
{
	Team,
	Channel
};

// One member of a team or channel (read for the drill-in panel, edited by add/remove/set-roles).
// RoleNames are flattened from the member's roles[] for display; RoleIds back the role-assignment edit.
struct FStudioGroupMember
{
	int64 GroupMemberId = 0;
	int64 UserId = 0;
	FString Status;
	TArray<FString> RoleNames;
	TArray<int64> RoleIds;
};

// One role of a team or channel. Permissions are the group-management keys this role grants (from the
// FIXED group-permission set: manage_members, manage_roles, manage_group, send_messages). System roles
// (e.g. the built-in leader) cannot be renamed, re-ranked, or deleted.
struct FStudioGroupRole
{
	int64 GroupRoleId = 0;
	FString RoleName;
	int32 Rank = 0;
	bool bIsSystem = false;
	TArray<FString> Permissions;
};

// A chunk address. Each axis is the BigInt scalar on the wire (a signed 64-bit decimal string);
// +1 on an axis is one chunk (16 voxels) further along it.
struct FStudioChunk
{
	int64 X = 0;
	int64 Y = 0;
	int64 Z = 0;
};

// A grid: a named 3D box of chunks that world/voxel runtime permissions are scoped to. There is no
// "list every grid" query, so grids are discovered by scanning a region (nearbyGridPermissions);
// EffectivePermissionKeys is the scanned user's flattened permissions on the grid.
struct FStudioGrid
{
	int64 GridId = 0;
	int64 AppId = 0;
	FStudioChunk Low;
	FStudioChunk High;
	TArray<FString> EffectivePermissionKeys;
};

// One group (or group-role) to permission-key grant on a grid. GroupRoleId is unset for a grant
// that applies to the whole group rather than a single role.
struct FStudioGridGroupGrant
{
	int64 GridId = 0;
	int64 GroupId = 0;
	int64 GroupRoleId = 0;
	bool bHasRole = false;
	FString PermissionKey;
	FString ExpiresAt;
};

// A studio-defined container type: the schema for a kind of runtime entity (like a class).
struct FStudioContainerType
{
	int64 AppId = 0;
	FString TypeName;
	FString DisplayName;
	FString Description;
	FString InstantiableBy;
	FString DefaultPropertyVisibility;
	// "session" (rows live in a session) or "app" (one row per key app-wide). A server predating the field reads as session.
	FString Scope = TEXT("session");
	// Who may claim a bindingKey on this type; empty when the type carries no bind policy.
	FString BindPolicyJson;
	FString MetadataJson;
};

// A typed field on a container type, with its default, read visibility, and who may write it.
struct FStudioPropertyDef
{
	FString ContainerTypeName;
	FString Key;
	FString ValueType;
	FString DefaultValueJson;
	FString Visibility;
	FString Writable;
	FString Description;
};

// A typed parameter of a studio-defined function.
struct FStudioFunctionParam
{
	FString Name;
	FString ValueType;
	bool bRequired = true;
	FString DefaultValueJson;
	FString Description;
	int32 SortOrder = 0;
};

// One declared write a function performs: set Property on Target to Expression.
struct FStudioFunctionMutation
{
	FString Target;
	FString Property;
	FString Expression;
};

// A studio-defined function: a named, sandboxed behaviour over containers. The editor only authors
// its signature and body; the server compiles the expressions to an AST and runs them.
struct FStudioFunction
{
	FString FunctionId;
	FString Name;
	FString ContainerTypeName;
	FString Description;
	FString ReturnType;
	TArray<FStudioFunctionParam> Parameters;
	TArray<FStudioFunctionMutation> Mutations;
	FString ReturnExpression;
	FString InvokeScope;
	// Whether an automation may run this function autonomously (mirrors the server GmFunction.autonomousInvocable).
	// An effect that runs automatically lowers this true; a plain effect leaves it false.
	bool bAutonomousInvocable = false;
	FString InvokePolicyJson;
	TArray<FString> Warnings;
	// The function's declared model-driven notifications, read back verbatim. The effect front-end does not
	// author these yet, so the schema sync READS them only to re-emit them on an upsert and thereby
	// preserve any seed/console-authored notifications (gameModelUpsertFunction replaces the field on omission).
	TArray<FCrowdyGameModelNotification> Notifications;
	// The function's declared timers, read back so the diff can tell an unchanged set from a changed one. Without
	// this read-back the desired set would compare against nothing and every plan would report drift forever.
	TArray<FCrowdyGameModelTimer> Timers;
};

// A studio-defined automation (autonomous process / NPC): the server read-back of one gameModelUpsertAutomation,
// used by the schema sync to diff a code-authored automation against the live one. Only the authored fields are
// mirrored; the circuit-breaker runtime fields (circuitState, consecutiveFailures, lastError, lastRunAt, ...) are
// not read because they are server-owned state, not part of the desired schema. Numbers that are BigInt on the wire
// (appId, runAsUserId) are read as int64; the budget ints as int32. Enum-like fields are the raw server strings.
struct FStudioAutomation
{
	FString AutomationId;   // UUID
	FString Name;           // the upsert key
	FString Description;
	bool bEnabled = true;
	FString ActionKind;
	FString FunctionName;
	FString TargetMode;
	FString SelfContainerId;
	FString TargetTypeName;
	FString SessionId;
	FString ParamsJson;
	FString SelectorJson;
	FString TriggerType;
	FString ScheduleKind;
	int32 IntervalMs = 0;
	FString CronExpr;
	int32 MaxTargets = 0;
	int32 GasLimit = 0;
	int32 RunTimeoutMs = 0;
	int32 MaxRunsPerMinute = 0;
	int32 FailureThreshold = 0;
	int32 CooldownMs = 0;
};

// A studio-defined automation event trigger: the server read-back of one gameModelUpsertAutomationTrigger, for
// diffing a code-authored event trigger against the live one. Keyed by (automationName, onEvent) plus its filters.
struct FStudioAutomationTrigger
{
	FString TriggerId;      // UUID, when the server assigns one
	FString AutomationName;
	FString OnEvent;
	FString FunctionName;
	FString ContainerTypeName;
	FString PropertyKey;
	FString WriteSource;
	int32 DebounceMs = 0;
};

// One leaf of a function's invoke policy: a single authority requirement. Type is the rule kind; only
// the fields that kind uses are read (Feature for tier_feature, GroupId/Permission for group_permission,
// Key/GridId for grid_permission, Expression for condition; the flag rules use none). The editor edits
// the policy as a flat list of these joined by one top-level connector (and/or); nested trees or
// unknown rules are edited as raw JSON instead. Not parsed by the controller (the view round-trips it).
struct FStudioPolicyRule
{
	FString Type;
	FString Feature;
	FString GroupId;
	FString Permission;
	FString Key;
	FString GridId;
	FString Expression;
};

// An app feature key that functions can gate on and that access tiers can be granted.
struct FStudioAppFeature
{
	FString FeatureKey;
	FString Description;
};

// A grant of a feature key to an access tier (so users on that tier satisfy the feature gate).
struct FStudioTierFeature
{
	int64 TierId = 0;
	FString FeatureKey;
};

// An app access tier (read-only reference): its id and name plus the free/default flags. Used to
// turn the tier-feature grant's raw tier id into a dropdown. Tiers are created and edited in the web
// console; the editor only lists them.
struct FStudioAccessTier
{
	int64 TierId = 0;
	FString Name;
	bool bIsFree = false;
	bool bIsDefault = false;
	// The runtime permission keys this tier grants (a subset of the runtimePermissions catalog) and the
	// tier lifecycle ("active" / "archived"). Used by the effective-permissions simulator on the Grid page.
	TArray<FString> PermissionKeys;
	FString Status;
};

// The app's game-model runtime policy: who may open sessions and the default participant role.
struct FStudioGameModelPolicy
{
	int64 AppId = 0;
	FString SessionCreationPolicy;
	FString DefaultParticipantRole;
	bool bValid = false;
};

// One gameModelLint finding. Subject is the object's own name and is unique only WITHIN a kind, since an
// automation and a function can both be called on_join, so group on (Code, Subject).
struct FStudioLintFinding
{
	FString Code;
	FString Severity;      // "ERROR" or "WARNING", the server's vocabulary verbatim
	FString SubjectKind;
	FString Subject;
	FString Message;
	FString Remedy;        // empty when the server offered none
	int32 Count = 0;       // how many objects this row stands for, 0 when it stands only for itself

	bool IsError() const { return Severity.Equals(TEXT("ERROR"), ESearchCase::IgnoreCase); }
};

// Whether the app's game model hangs together, as gameModelLint answers it. Recomputed by the server on every
// call, so this is one answer rather than an accumulated state; bRan says whether an answer has arrived at all.
struct FStudioLintReport
{
	int64 AppId = 0;
	int32 ErrorCount = 0;
	int32 WarningCount = 0;
	// True when there are no ERRORS. Warnings do not make an app unclean: most are ordinary mid-edit states, such as
	// a seeded function calling another one written later in the same batch.
	bool bClean = false;
	bool bRan = false;
	TArray<FStudioLintFinding> Findings;
};

// What an Issues surface should say about a lint report. THREE answers, because an app nobody has linted holds
// the same empty findings list as one that came back clean, and telling them apart is the point.
enum class ECrowdyLintTabState : uint8
{
	NeverRun,
	Clean,
	HasFindings
};

// Keyed on the FINDINGS, not on bClean: bClean ignores warnings, so gating on it would leave every warning
// reported into the log and unreachable from the surface that exists to show it.
inline ECrowdyLintTabState CrowdyLintTabStateFor(const FStudioLintReport& Report)
{
	if (!Report.bRan)
	{
		return ECrowdyLintTabState::NeverRun;
	}
	return Report.Findings.Num() > 0 ? ECrowdyLintTabState::HasFindings : ECrowdyLintTabState::Clean;
}

// A live runtime container instance (an entity the runtime has spawned), as listed by
// gameModelContainers. Read-only; used by the Inspector-style live-state browser.
struct FStudioContainer
{
	FString ContainerId;   // UUID
	FString SessionId;     // owning session, or empty for an app-global container
	FString TypeName;
	FString DisplayName;
	int64 OwnerUserId = 0; // 0 when unowned
	FString MetadataJson;  // raw metadata object (may be empty); developer metadata for the container
	// The key this container was ensured under, or empty when it was created outright. A container held by a
	// binding key is recreated by the runtime the next time that key is ensured, so deleting one is not final.
	FString BindingKey;
};

// A Game Model session as listed by gameModelSessions, enough to pick one as a pre-seed scope.
struct FStudioSession
{
	FString SessionId;
	FString Name;
	FString Status;
};

// A live container's visible property values (gameModelContainerState), filtered server-side to what
// the calling token may see. PropertiesJson is the raw JSON object of those properties.
struct FStudioContainerState
{
	FString ContainerId;
	FString TypeName;
	FString DisplayName;
	int64 OwnerUserId = 0;
	FString PropertiesJson;
	bool bValid = false;
};
