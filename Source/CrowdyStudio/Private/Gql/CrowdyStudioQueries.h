// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Model/CrowdyStudioTypes.h"

class FJsonObject;

// Inline GraphQL bodies for the Phase 1 studio operations, one accessor per op so the
// Phase 2 codegen can replace them mechanically. Field/argument names match the live
// management schema (introspected from the dev endpoint): ids are the BigInt scalar,
// list/create ops take input objects, appsForOrg keys on the org slug, and status /
// visibility are the AppStatus / AppVisibility enums.
namespace CrowdyStudioGql
{
	FString LoginMutation();
	// Dev-only, passwordless sign-in: returns the same AuthResponse (identity SESSION token) as login.
	// The server throws FORBIDDEN unless it runs with DEV_AUTH_BYPASS=true.
	FString DevLoginMutation();
	// Mint a short-lived app-scoped GAMEPLAY token from the SESSION token (management plane, session
	// bearer). Game API ops authorize with THIS token, not the session token (the two-token model).
	// appId is the BigInt scalar sent as a JSON string; the response also carries the authoritative
	// per-app game endpoints. Follows the runtime mint (CrowdyNet FMintAppTokenRequest).
	FString MintAppTokenMutation();

	// Passwordless sign-in options that all return the same AuthResponse (management-plane SESSION
	// token) as login/devLogin, so they can mint an app token and unlock game-plane authoring. All are
	// PUBLIC (no bearer): at sign-in time AuthToken is empty and SendManagement omits the Authorization
	// header. Shapes mirror the runtime M2 requests (flat vars, input built inline in the body), which
	// are proven build-green — copied from the runtime, not re-derived from the docs.
	FString SocialLoginStartMutation();
	FString SocialLoginCompleteMutation();
	FString AvailableLoginProvidersQuery();
	FString RequestLoginLinkMutation();
	FString CompleteLoginLinkMutation();

	FString MyOrganizationsQuery();
	FString CreateOrganizationMutation();
	FString MyAppsQuery();
	FString CreateAppMutation();
	FString UpdateAppMutation();
	FString ArchiveAppMutation();
	FString AppQuery();
	FString OrgEnvironmentsQuery();
	FString LinkAppToEnvironmentMutation();

	// Teams & channels (game plane these ops are only on the Game API, so they need a
	// game-capable token; the editor sends them to the game endpoint).
	FString TeamsQuery();
	FString TeamPolicyQuery();
	FString SetTeamPolicyMutation();
	FString ChannelsQuery();
	FString ChannelPolicyQuery();
	FString SetChannelPolicyMutation();
	FString CreateChannelMutation();
	FString CreateTeamMutation();
	// Disband the team/channel itself (DESTRUCTIVE; cascades to members, roles, and grid grants).
	FString DeleteGroupMutation(bool bChannel);
	// Edit the team/channel itself (name / description / membership policy; omitted fields unchanged).
	FString UpdateGroupMutation(bool bChannel);

	// Team/channel drill-in + editing (game plane). bChannel picks the channel op (channelMembers,
	// createChannelRole, ...) over the team op (teamMembers, createTeamRole, ...); shapes are identical.
	FString GroupMembersQuery(bool bChannel);
	FString GroupRolesQuery(bool bChannel);
	FString AddGroupMemberMutation(bool bChannel);
	FString RemoveGroupMemberMutation(bool bChannel);
	FString SetGroupMemberRolesMutation(bool bChannel);
	FString CreateGroupRoleMutation(bool bChannel);
	FString UpdateGroupRoleMutation(bool bChannel);
	FString DeleteGroupRoleMutation(bool bChannel);

	// Spatial grid (game plane, app-admin). Grids are world regions voxel/runtime permissions are
	// scoped to; there is no list-all query, so they are discovered by scanning a chunk region.
	FString NearbyGridPermissionsQuery();
	FString GridPermissionLimitsQuery();
	FString GridGroupGrantsQuery();
	FString GridUserPermissionsQuery();
	FString CreateGridMutation();
	FString GrantGridPermissionsMutation();
	FString RevokeGridPermissionsMutation();
	FString SetGridPermissionLimitsMutation();
	FString AssignGroupToGridMutation();
	FString RevokeGroupFromGridMutation();

	// Runtime permission key catalog (management plane, PUBLIC, no args). The flat list of valid
	// permission keys grids draw from; turns the grid permission inputs into checkboxes.
	FString RuntimePermissionsQuery();

	// Game model (game plane, app-admin). The design-time data schema the runtime consumes:
	// container types, their property defs, sandboxed functions, feature keys, and tier grants.
	FString ContainerTypesQuery();
	FString PropertyDefsQuery();
	FString FunctionsQuery();
	FString FeaturesQuery();
	FString TierFeaturesQuery();
	FString AppAccessTiersQuery();
	FString GameModelPolicyQuery();
	// Live runtime state (read-only): the containers the runtime has instantiated, and one container's
	// visible property values. Game plane.
	FString ContainersQuery();
	FString ContainerStateQuery();
	FString UpsertContainerTypeMutation();
	FString UpsertPropertyDefMutation();
	FString UpsertFunctionMutation();
	FString DeleteFunctionMutation();
	FString DefineFeatureMutation();
	FString GrantTierFeatureMutation();
	FString RevokeTierFeatureMutation();
	FString SetGameModelPolicyMutation();
	FString SeedGameModelMutation();

	// Each parser takes the full response envelope ({ data, errors }) and digs into
	// `data` itself, returning false when the expected node is missing.
	bool ParseLogin(const TSharedPtr<FJsonObject>& Envelope, FString& OutToken, int64& OutUserId);
	// Mirrors ParseLogin but reads the devLogin field. Same AuthResponse shape, same out-params.
	bool ParseDevLogin(const TSharedPtr<FJsonObject>& Envelope, FString& OutToken, int64& OutUserId);
	// Reads the mintAppToken response: the app-scoped token plus the per-app game endpoints and the
	// token expiry (ISO-8601). Returns false when the token is missing.
	bool ParseAppToken(const TSharedPtr<FJsonObject>& Envelope, FString& OutToken, FString& OutGameApiUrl,
	                   FString& OutGameApiWsUrl, FString& OutExpiresAt);

	// socialLoginStart -> { authorizeUrl, state }. False when the node or authorizeUrl is missing.
	bool ParseSocialLoginStart(const TSharedPtr<FJsonObject>& Envelope, FString& OutAuthorizeUrl, FString& OutState);
	// socialLoginComplete / completeLoginLink both return the AuthResponse shape (token + user{userId});
	// each reads its own field. False when the token is missing.
	bool ParseSocialLoginComplete(const TSharedPtr<FJsonObject>& Envelope, FString& OutToken, int64& OutUserId);
	bool ParseCompleteLoginLink(const TSharedPtr<FJsonObject>& Envelope, FString& OutToken, int64& OutUserId);
	// requestLoginLink -> { sent, devToken }. In dev devToken is non-empty and short-circuits the email.
	bool ParseRequestLoginLink(const TSharedPtr<FJsonObject>& Envelope, bool& OutSent, FString& OutDevToken);
	// availableLoginProviders -> [String]: the enabled federated providers that drive the sign-in buttons.
	void ParseProviders(const TSharedPtr<FJsonObject>& Envelope, TArray<FString>& OutProviders);
	void ParseOrganizations(const TSharedPtr<FJsonObject>& Envelope, TArray<TSharedPtr<FStudioOrg>>& OutOrgs);
	TSharedPtr<FStudioOrg> ParseOrganization(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName);
	void ParseApps(const TSharedPtr<FJsonObject>& Envelope, TArray<TSharedPtr<FStudioApp>>& OutApps);
	TSharedPtr<FStudioApp> ParseApp(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName);
	void ParseEnvironments(const TSharedPtr<FJsonObject>& Envelope, TArray<TSharedPtr<FStudioEnvironment>>& OutEnvs);

	bool ParseGroupPolicy(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName, FStudioGroupPolicy& OutPolicy);
	void ParseGroups(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                 TArray<TSharedPtr<FStudioGroup>>& OutGroups);
	void ParseGroupMembers(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                       TArray<TSharedPtr<FStudioGroupMember>>& OutMembers);
	void ParseGroupRoles(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                     TArray<TSharedPtr<FStudioGroupRole>>& OutRoles);

	// Grid. Grant/revoke-user and limits results all expose a flat permissionKeys array, so one
	// key reader serves them; group grants and nearby scans get their own shapes.
	void ParseNearbyGrids(const TSharedPtr<FJsonObject>& Envelope, TArray<TSharedPtr<FStudioGrid>>& OutGrids);
	bool ParseCreateGrid(const TSharedPtr<FJsonObject>& Envelope, FStudioGrid& OutGrid, FString& OutError);
	void ParseGridPermissionKeys(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName, TArray<FString>& OutKeys);
	void ParseGridGroupGrants(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                          TArray<TSharedPtr<FStudioGridGroupGrant>>& OutGrants);
	void ParseRuntimePermissions(const TSharedPtr<FJsonObject>& Envelope, TArray<FString>& OutKeys);

	// Game model.
	void ParseContainerTypes(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                         TArray<TSharedPtr<FStudioContainerType>>& OutTypes);
	void ParsePropertyDefs(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                       TArray<TSharedPtr<FStudioPropertyDef>>& OutDefs);
	void ParseFunctions(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                    TArray<TSharedPtr<FStudioFunction>>& OutFns);
	bool ParseFunction(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName, FStudioFunction& OutFn);
	void ParseFeatures(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                   TArray<TSharedPtr<FStudioAppFeature>>& OutFeatures);
	void ParseTierFeatures(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                       TArray<TSharedPtr<FStudioTierFeature>>& OutGrants);
	void ParseAccessTiers(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                      TArray<TSharedPtr<FStudioAccessTier>>& OutTiers);
	bool ParseGameModelPolicy(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName, FStudioGameModelPolicy& OutPolicy);
	bool ParseSeedResult(const TSharedPtr<FJsonObject>& Envelope, FString& OutSummary);

	void ParseContainers(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                     TArray<TSharedPtr<FStudioContainer>>& OutContainers);
	bool ParseContainerState(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName, FStudioContainerState& OutState);
}
