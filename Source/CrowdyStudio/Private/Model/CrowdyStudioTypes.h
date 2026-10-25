// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

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

	bool operator==(const FStudioSettingsSnapshot& Other) const
	{
		return AppId == Other.AppId && OrgId == Other.OrgId
			&& DiscoveryUrl.Equals(Other.DiscoveryUrl, ESearchCase::CaseSensitive)
			&& GameApiHttpUrl.Equals(Other.GameApiHttpUrl, ESearchCase::CaseSensitive)
			&& GameApiWsUrl.Equals(Other.GameApiWsUrl, ESearchCase::CaseSensitive);
	}
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

// An app feature key that access tiers can be granted.
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
