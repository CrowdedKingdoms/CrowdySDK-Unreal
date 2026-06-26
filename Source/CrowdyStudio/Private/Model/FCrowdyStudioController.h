// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Model/CrowdyStudioTypes.h"
#include "Templates/SharedPointer.h"

class FJsonObject;

enum class ECrowdyStudioAuthScope : uint8
{
	None,
	Session,
	OrgToken
};

/**
 * The brains of the console. Every server round-trip lives here, behind plain async
 * methods that update the controller's own state and then fire a delegate; the Slate
 * views stay dumb and just rebuild themselves from that state when a delegate fires.
 * One owner of the auth token, the org/app/environment lists, and the current selection.
 */
class FCrowdyStudioController : public TSharedFromThis<FCrowdyStudioController>
{
public:
	FCrowdyStudioController();

	// Restores a remembered token from the vault and validates it. Kept out of the
	// constructor so the views can bind their delegates before anything broadcasts.
	void Initialize();

	//Sign-in
	void SignInWithToken(const FString& OrgToken);
	void LoginWithEmail(const FString& Email, const FString& Password);
	void SignOut();

	// Organizations 
	void FetchMyOrganizations();
	void CreateOrganization(const FString& Name, const FString& Slug);

	// Apps
	// Lists every app the token can see (myApps) — no org needed, ids never hand-typed.
	void FetchApps();
	void CreateApp(int64 OrgId, const FString& Name, const FString& Slug, const FString& Status, const FString& Visibility);
	void UpdateApp(int64 AppId, const FString& Name, const FString& Status, const FString& Visibility);
	void ArchiveApp(int64 AppId);
	void FetchApp(int64 AppId);

	// Environments 
	// Provisioning/destroy/secrets are intentionally NOT here — those run in the web
	// console (see SCrowdyWebView), so the editor never holds the env private key.
	void FetchEnvironments(int64 OrgId);
	void LinkAppToEnvironment(int64 AppId, const FString& EnvironmentSlug);

	//  Config Sync
	void SyncConfig();
	FStudioSettingsSnapshot GetCurrentSettings() const;

	// What SyncConfig would leave the settings as, computed without writing anything, so the
	// Project view can show an honest before/after diff. A sync changes the app id, the org, and
	// the game endpoints (taken from the app's own routing); the management URL is owned by the
	// backend selector below.
	FStudioSettingsSnapshot BuildProposedSettings() const;

	// Backend selector (Dev/Prod/Custom): which Crowdy management plane the editor and the runtime
	// talk to. The game endpoints follow from the selected app's routing, not from this.
	FString GetBackendMode() const;
	void SetBackendMode(const FString& Mode);
	FString GetCustomManagementUrl() const;
	void SetCustomManagementUrl(const FString& Url);
	FString GetEffectiveManagementUrl() const;

	// Teams & channels (game plane)
	// Team/channel policy and CRUD live only on the Game API, so these post to the game
	// endpoint with the signed-in token (sign in with email/password a game-capable token
	// for these to authorize; an org token alone is management-scoped and will be rejected).
	void FetchTeams();
	void FetchTeamPolicy();
	// MaxMembers / MaxGroupsPerUser are caps; 0 means unlimited (sent to the server as null).
	void SetTeamPolicy(const FString& CreationPolicy, const FString& DefaultMembershipPolicy, int32 MaxMembers, int32 MaxGroupsPerUser);
	void CreateTeam(const FString& Name, const FString& Description, const FString& MembershipPolicy);
	void FetchChannels();
	void FetchChannelPolicy();
	void SetChannelPolicy(const FString& CreationPolicy, const FString& DefaultMembershipPolicy, int32 MaxMembers, int32 MaxGroupsPerUser);
	// MembershipPolicy is optional (empty = the app's default channel membership policy).
	void CreateChannel(const FString& Name, const FString& Description, bool bMembersCanSend, const FString& MembershipPolicy = FString());
	// One-click create of the well-known Reliable-RPC session channel (__crowdy_session_<appId>), with
	// members-can-send on and open membership, so a project can pre-seed it from the editor.
	void CreateSessionChannel();

	// Team/channel drill-in + editing. SelectGroup loads the chosen group's members and roles into the
	// shared detail state and fires OnGroupDetailChanged; the edit ops re-fetch that detail on success.
	// Kind picks the team vs channel operation (the wire shapes are identical).
	void SelectGroup(ECrowdyGroupKind Kind, int64 GroupId);
	void AddGroupMember(ECrowdyGroupKind Kind, int64 GroupId, int64 InUserId);
	void RemoveGroupMember(ECrowdyGroupKind Kind, int64 GroupId, int64 InUserId);
	void SetGroupMemberRoles(ECrowdyGroupKind Kind, int64 GroupId, int64 InUserId, const TArray<int64>& RoleIds);
	void CreateGroupRole(ECrowdyGroupKind Kind, int64 GroupId, const FString& RoleName, const TArray<FString>& Permissions, int32 Rank);
	// Edit a role's name (empty = unchanged), permission set (replaces it), and rank.
	void UpdateGroupRole(ECrowdyGroupKind Kind, int64 GroupRoleId, const FString& RoleName, const TArray<FString>& Permissions, int32 Rank);
	void DeleteGroupRole(ECrowdyGroupKind Kind, int64 GroupRoleId);
	// Disband the team/channel itself. DESTRUCTIVE; on success clears the detail and refreshes the list.
	void DeleteGroup(ECrowdyGroupKind Kind, int64 GroupId);
	// Edit the team/channel itself (rename / description / membership). Empty fields are left unchanged.
	void UpdateGroup(ECrowdyGroupKind Kind, int64 GroupId, const FString& Name, const FString& Description, const FString& MembershipPolicy);

	// Spatial grid (game plane, app-admin). Grids are world regions that voxel/runtime permissions
	// are scoped to. Discovered by scanning a chunk region; there is no list-all query.
	void FetchNearbyGrids(int64 InUserId, int64 LowX, int64 LowY, int64 LowZ, int64 HighX, int64 HighY, int64 HighZ);
	void CreateGrid(int64 C1X, int64 C1Y, int64 C1Z, int64 C2X, int64 C2Y, int64 C2Z);
	void FetchGridPermissionLimits(int64 GridId);
	void SetGridPermissionLimits(int64 GridId, const TArray<FString>& PermissionKeys);
	void FetchGridGroupGrants(int64 GridId, int64 GroupId);
	void AssignGroupToGrid(int64 GridId, int64 GroupId, bool bHasRole, int64 GroupRoleId, const TArray<FString>& PermissionKeys);
	void RevokeGroupFromGrid(int64 GridId, int64 GroupId, bool bHasRole, int64 GroupRoleId, const TArray<FString>& PermissionKeys);
	void FetchGridUserPermissions(int64 GridId, int64 InUserId);
	void GrantGridPermissions(int64 GridId, int64 InUserId, const TArray<FString>& PermissionKeys);
	void RevokeGridPermissions(int64 GridId, int64 InUserId, const TArray<FString>& PermissionKeys);
	// The runtime permission key catalog (management plane, PUBLIC). Fetched so the grid permission
	// inputs can be checkboxes drawn from the valid keys instead of free-typed comma lists.
	void FetchRuntimePermissions();

	// Game model (game plane, app-admin). The design-time schema the runtime consumes: container
	// types and their properties, sandboxed functions, feature keys, and tier-feature grants.
	void FetchContainerTypes();
	void UpsertContainerType(const FString& TypeName, const FString& DisplayName, const FString& Description,
		const FString& InstantiableBy, const FString& DefaultPropertyVisibility);
	void FetchPropertyDefs(const FString& TypeName);
	void UpsertPropertyDef(const FString& TypeName, const FString& Key, const FString& ValueType,
		const FString& DefaultValueJson, const FString& Visibility, const FString& Writable, const FString& Description);
	void FetchFunctions(const FString& ContainerTypeFilter);
	// Parameters/mutations/invoke-policy are passed as JSON the server compiles; an empty string omits them.
	void UpsertFunction(const FString& Name, const FString& ContainerTypeName, const FString& Description,
		const FString& ReturnType, const FString& InvokeScope, const FString& ReturnExpression,
		const FString& ParametersJson, const FString& MutationsJson, const FString& InvokePolicyJson);
	void DeleteFunction(const FString& Name);
	void FetchFeatures();
	void DefineFeature(const FString& FeatureKey, const FString& Description);
	void FetchTierFeatures();
	// Read-only list of the app's access tiers (management plane), so the tier-feature grant can use
	// a tier dropdown instead of a hand-typed tier id. Tiers are created/edited in the web console.
	void FetchAppAccessTiers();
	void GrantTierFeature(int64 TierId, const FString& FeatureKey);
	void RevokeTierFeature(int64 TierId, const FString& FeatureKey);
	void FetchGameModelPolicy();
	void SetGameModelPolicy(const FString& SessionCreationPolicy, const FString& DefaultParticipantRole);
	// Bulk import: SeedJson is a SeedGameModelInput object (minus appId, which is injected here).
	void SeedGameModel(const FString& SeedJson);

	// Live runtime state (read-only): the runtime's instantiated containers, and one container's
	// visible property values. Game plane; works even outside PIE (it reads server state, not the
	// editor's play world). Filters are optional (empty = all).
	void FetchContainers(const FString& TypeNameFilter, const FString& SessionIdFilter);
	void FetchContainerState(const FString& ContainerId);

	// Selection
	void SelectOrg(int64 OrgId);
	void SelectApp(int64 AppId);
	void SelectEnvironment(const FString& EnvironmentSlug);

	int64 GetSelectedOrgId() const { return SelectedOrgId; }
	int64 GetSelectedAppId() const { return SelectedAppId; }
	const FString& GetSelectedEnvironmentSlug() const { return SelectedEnvironmentSlug; }
	TSharedPtr<FStudioOrg> GetSelectedOrg() const;
	TSharedPtr<FStudioApp> GetSelectedApp() const;

	// State the views read 
	const TArray<TSharedPtr<FStudioOrg>>& GetOrganizations() const { return Organizations; }
	const TArray<TSharedPtr<FStudioApp>>& GetApps() const { return Apps; }
	const TArray<TSharedPtr<FStudioEnvironment>>& GetEnvironments() const { return Environments; }
	const TArray<TSharedPtr<FStudioGroup>>& GetTeams() const { return Teams; }
	const TArray<TSharedPtr<FStudioGroup>>& GetChannels() const { return Channels; }
	const FStudioGroupPolicy& GetTeamPolicy() const { return TeamPolicy; }
	const FStudioGroupPolicy& GetChannelPolicy() const { return ChannelPolicy; }
	const TArray<TSharedPtr<FStudioGroupMember>>& GetGroupMembers() const { return GroupMembers; }
	const TArray<TSharedPtr<FStudioGroupRole>>& GetGroupRoles() const { return GroupRoles; }
	int64 GetSelectedGroupId() const { return SelectedGroupId; }
	ECrowdyGroupKind GetSelectedGroupKind() const { return SelectedGroupKind; }

	const TArray<TSharedPtr<FStudioGrid>>& GetNearbyGrids() const { return NearbyGrids; }
	const TArray<FString>& GetGridWhitelistKeys() const { return GridWhitelistKeys; }
	const TArray<FString>& GetGridUserEffectiveKeys() const { return GridUserEffectiveKeys; }
	const TArray<TSharedPtr<FStudioGridGroupGrant>>& GetGridGroupGrants() const { return GridGroupGrants; }
	const TArray<FString>& GetRuntimePermissions() const { return RuntimePermissions; }

	const TArray<TSharedPtr<FStudioContainerType>>& GetContainerTypes() const { return ContainerTypes; }
	const TArray<TSharedPtr<FStudioPropertyDef>>& GetPropertyDefs() const { return PropertyDefs; }
	const TArray<TSharedPtr<FStudioFunction>>& GetFunctions() const { return Functions; }
	const TArray<TSharedPtr<FStudioAppFeature>>& GetFeatures() const { return Features; }
	const TArray<TSharedPtr<FStudioTierFeature>>& GetTierFeatures() const { return TierFeatures; }
	const TArray<TSharedPtr<FStudioAccessTier>>& GetAccessTiers() const { return AccessTiers; }
	const FStudioGameModelPolicy& GetGameModelPolicy() const { return GameModelPolicy; }
	const TArray<TSharedPtr<FStudioContainer>>& GetContainers() const { return Containers; }
	const FStudioContainerState& GetContainerState() const { return ContainerState; }
	const FString& GetSelectedContainerId() const { return SelectedContainerId; }

	bool IsSignedIn() const { return bSignedIn; }

	// True when the selected org's effective permissions include PermissionKey. The console uses this
	// to grey the management actions a token can't perform. With no org selected, or before the org's
	// permissions have been fetched, it returns false so an action stays disabled until its permission
	// is confirmed present.
	bool HasOrgPermission(const FString& PermissionKey) const;

	// Convenience gates for the two management-plane mutations the native console still issues itself
	// (creating an app, linking an app to an environment). Everything else admin moved to the web console.
	bool CanManageApps() const;
	bool CanManageEnvironments() const;
	// The current bearer token — used to single-sign-on the embedded web console (injected as
	// its localStorage auth_token). Matches the web app's session when signed in via email/password.
	const FString& GetAuthToken() const { return AuthToken; }
	bool IsBusy() const { return InFlightCount > 0; }
	const FString& GetStatusMessage() const { return StatusMessage; }
	bool LastStatusWasError() const { return bStatusWasError; }

	//Delegates the views bind to
	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnStudioStatusMessage, const FString& /*Message*/, bool /*bIsError*/);

	FSimpleMulticastDelegate OnSignInStateChanged;
	FSimpleMulticastDelegate OnOrganizationsChanged;
	FSimpleMulticastDelegate OnAppsChanged;
	FSimpleMulticastDelegate OnEnvironmentsChanged;
	FSimpleMulticastDelegate OnConfigChanged;
	FSimpleMulticastDelegate OnBusyChanged;
	// Fired when the active app context changes (a different app selected, or the remembered app
	// becomes active after sign-in), so app-scoped views can auto-load without a manual Refresh.
	FSimpleMulticastDelegate OnSelectedAppChanged;
	FSimpleMulticastDelegate OnTeamsChanged;
	FSimpleMulticastDelegate OnChannelsChanged;
	FSimpleMulticastDelegate OnTeamPolicyChanged;
	FSimpleMulticastDelegate OnChannelPolicyChanged;
	// Fired when the selected team/channel's members or roles reload (the drill-in detail).
	FSimpleMulticastDelegate OnGroupDetailChanged;
	FSimpleMulticastDelegate OnNearbyGridsChanged;
	FSimpleMulticastDelegate OnGridDetailChanged;
	// Fired only when the selected grid's permission whitelist (limits) reloads, so the whitelist
	// checkboxes can resync from the server without other detail refreshes clobbering live edits.
	FSimpleMulticastDelegate OnGridWhitelistChanged;
	FSimpleMulticastDelegate OnRuntimePermissionsChanged;
	FSimpleMulticastDelegate OnContainerTypesChanged;
	FSimpleMulticastDelegate OnPropertyDefsChanged;
	FSimpleMulticastDelegate OnFunctionsChanged;
	FSimpleMulticastDelegate OnFeaturesChanged;
	FSimpleMulticastDelegate OnTierFeaturesChanged;
	FSimpleMulticastDelegate OnAccessTiersChanged;
	FSimpleMulticastDelegate OnGameModelPolicyChanged;
	FSimpleMulticastDelegate OnContainersChanged;
	FSimpleMulticastDelegate OnContainerStateChanged;
	FOnStudioStatusMessage OnStatusMessage;

private:
	FString ResolveManagementBaseUrl() const;
	FString ResolveManagementUrl() const;
	FString ResolveGameUrl() const;

	// Posts a query to the management endpoint with the current bearer token, tracks the
	// busy count, and only calls OnSuccess once the envelope is clean (HTTP ok + no
	// GraphQL errors). Any failure is turned into a status message for the views, then
	// OnFailure runs if the caller needs to undo something (e.g. a bad token).
	void SendManagement(const FString& Query, const TSharedPtr<FJsonObject>& Variables,
		TFunction<void(const TSharedPtr<FJsonObject>& /*Envelope*/)> OnSuccess,
		TFunction<void()> OnFailure = TFunction<void()>());

	// Same contract as SendManagement, but posts to the game endpoint. Team/channel ops live
	// only on the Game API, so the signed-in token must be game-capable to authorize them.
	void SendGame(const FString& Query, const TSharedPtr<FJsonObject>& Variables,
		TFunction<void(const TSharedPtr<FJsonObject>& /*Envelope*/)> OnSuccess);

	void SetStatus(const FString& Message, bool bIsError);
	void BeginRequest();
	void EndRequest();

	// Right after sign-in, pull the app list and (if an org is remembered) its environments,
	// so the console opens populated instead of empty.
	void FetchAppsAndEnvironments();

	void PersistSelection() const;
	class UCrowdyStudioUserSettings* GetUserSettings() const;

	FString AuthToken;
	ECrowdyStudioAuthScope AuthScope = ECrowdyStudioAuthScope::None;
	bool bSignedIn = false;
	int64 UserId = 0;

	TArray<TSharedPtr<FStudioOrg>> Organizations;
	TArray<TSharedPtr<FStudioApp>> Apps;
	TArray<TSharedPtr<FStudioEnvironment>> Environments;
	TArray<TSharedPtr<FStudioGroup>> Teams;
	TArray<TSharedPtr<FStudioGroup>> Channels;
	FStudioGroupPolicy TeamPolicy;
	FStudioGroupPolicy ChannelPolicy;
	TArray<TSharedPtr<FStudioGroupMember>> GroupMembers;
	TArray<TSharedPtr<FStudioGroupRole>> GroupRoles;
	int64 SelectedGroupId = 0;
	ECrowdyGroupKind SelectedGroupKind = ECrowdyGroupKind::Team;

	TArray<TSharedPtr<FStudioGrid>> NearbyGrids;
	TArray<FString> GridWhitelistKeys;
	TArray<FString> GridUserEffectiveKeys;
	TArray<TSharedPtr<FStudioGridGroupGrant>> GridGroupGrants;
	TArray<FString> RuntimePermissions;

	TArray<TSharedPtr<FStudioContainerType>> ContainerTypes;
	TArray<TSharedPtr<FStudioPropertyDef>> PropertyDefs;
	TArray<TSharedPtr<FStudioFunction>> Functions;
	TArray<TSharedPtr<FStudioAppFeature>> Features;
	TArray<TSharedPtr<FStudioTierFeature>> TierFeatures;
	TArray<TSharedPtr<FStudioAccessTier>> AccessTiers;
	FStudioGameModelPolicy GameModelPolicy;
	TArray<TSharedPtr<FStudioContainer>> Containers;
	FStudioContainerState ContainerState;
	FString SelectedContainerId;

	int64 SelectedOrgId = 0;
	int64 SelectedAppId = 0;
	FString SelectedEnvironmentSlug;

	int32 InFlightCount = 0;
	FString StatusMessage;
	bool bStatusWasError = false;
};
