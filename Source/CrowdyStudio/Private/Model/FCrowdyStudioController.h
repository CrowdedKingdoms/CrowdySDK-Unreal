// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Misc/DateTime.h"
#include "Model/CrowdyStudioTypes.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"

class FJsonObject;
class FCrowdyLoopbackAuthServer;

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
	// Out-of-line so the TUniquePtr<FCrowdyLoopbackAuthServer> member can hold a forward-declared type
	// (the deleter is instantiated in the .cpp, where the loopback header is included).
	~FCrowdyStudioController();

	// Restores a remembered token from the vault and validates it. Kept out of the
	// constructor so the views can bind their delegates before anything broadcasts.
	void Initialize();

	//Sign-in
	void SignInWithToken(const FString& OrgToken);
	void LoginWithEmail(const FString& Email, const FString& Password);
	// Dev-only, passwordless sign-in. Mirrors LoginWithEmail but issues the devLogin mutation; the
	// server only honours it when running with DEV_AUTH_BYPASS (otherwise FORBIDDEN surfaces as an error).
	void SignInWithDevLogin(const FString& Email);

	// Passwordless sign-in that yields a mint-capable SESSION token, exactly like email/dev login (so
	// unlike an org token it can mint an app token and unlock game-plane authoring). Both drive the OAuth
	// dance in the SYSTEM browser per the native-client docs — never an embedded webview — and capture
	// the redirect on a 127.0.0.1 loopback listener. FetchAvailableProviders lists which providers the
	// server has enabled, so the sign-in view renders one button per provider instead of hard-coding them.
	void FetchAvailableProviders();
	void SignInWithSocial(const FString& Provider);
	// Emails a one-time sign-in link whose redirect the loopback captures; in dev a devToken returned by
	// the server short-circuits the email/browser round-trip and completes immediately.
	void SignInWithMagicLink(const FString& Email);

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
	// Team/channel policy and CRUD live only on the Game API, so these post to the game endpoint
	// bearing the app-scoped token minted for the selected app (see SendGame). A session sign-in
	// (email/dev/magic link) can mint that token; an org token is management-scoped and cannot, so
	// its game ops are rejected.
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
	// The enabled federated sign-in providers (availableLoginProviders), driving the social buttons.
	const TArray<FString>& GetLoginProviders() const { return LoginProviders; }
	bool IsBusy() const { return InFlightCount > 0; }
	const FString& GetStatusMessage() const { return StatusMessage; }
	bool LastStatusWasError() const { return bStatusWasError; }

	//Delegates the views bind to
	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnStudioStatusMessage, const FString& /*Message*/, bool /*bIsError*/);

	FSimpleMulticastDelegate OnSignInStateChanged;
	// Fired when the enabled social sign-in providers are (re)fetched, so the sign-in view rebuilds its
	// provider buttons.
	FSimpleMulticastDelegate OnLoginProvidersChanged;
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
	// bReportErrors=false suppresses the error status toast (used by the best-effort provider probe,
	// which must degrade silently on a backend that doesn't expose availableLoginProviders); OnFailure
	// still runs.
	void SendManagement(const FString& Query, const TSharedPtr<FJsonObject>& Variables,
		TFunction<void(const TSharedPtr<FJsonObject>& /*Envelope*/)> OnSuccess,
		TFunction<void()> OnFailure = TFunction<void()>(), bool bReportErrors = true);

	// Same contract as SendManagement, but posts to the game endpoint. Game API ops authorize with
	// the app-scoped token (minted from the session token via mintAppToken), NOT the session token
	// itself — the two-token model. Mints (or refreshes) that token first when it is missing or near
	// expiry, then posts; an org-token sign-in cannot mint, so its game ops surface a clear error.
	void SendGame(const FString& Query, const TSharedPtr<FJsonObject>& Variables,
		TFunction<void(const TSharedPtr<FJsonObject>& /*Envelope*/)> OnSuccess);
	// The actual game-endpoint POST, once an app token is in hand. Split out of SendGame so the
	// async "mint then post" path can re-enter it without re-checking the token.
	void PostGame(const FString& Query, const TSharedPtr<FJsonObject>& Variables,
		TFunction<void(const TSharedPtr<FJsonObject>& /*Envelope*/)> OnSuccess);

	// Mint the app-scoped game token for the selected app from the session token (mintAppToken,
	// management plane). OnDone(true) once a token for the *current* app is in hand; OnDone(false) on
	// failure (after a descriptive status). Concurrent requests fold into a single in-flight mint
	// (no redundant mutations, no GameAppToken write race); a mint whose app no longer matches the
	// current selection is discarded and re-issued for the new app so a waiting op still gets a token.
	void MintAppToken(TFunction<void(bool /*bMinted*/)> OnDone);
	// Begin one mintAppToken round-trip for SelectedAppId. Precondition: CanMintAppToken(), not already
	// in flight, with at least one queued waiter. Its completion applies the token (if still current)
	// and flushes the waiters via FinishMint.
	void StartMint();
	// Resolve one mint round-trip: clear the in-flight flag, then either re-issue for the now-current
	// app (if MintedAppId went stale and waiters remain) or flush every queued waiter with bReady.
	void FinishMint(int64 MintedAppId, bool bReady);
	// Drop any app-scoped token/endpoint/expiry. Called on sign-out, app switch, and every sign-in
	// entry so a prior identity's app token never bleeds into the next one. Does NOT touch the
	// in-flight mint machinery (an in-flight mint resolves itself and won't apply once stale).
	void ClearAppToken();
	// Mint the app token (if a session sign-in allows it) and only then announce OnSelectedAppChanged,
	// so the game-plane views load with a valid token. Used on app-select and on post-sign-in restore.
	void AnnounceAppContext();
	// True when the current sign-in can mint an app token: a session-scoped sign-in with an app selected.
	bool CanMintAppToken() const;
	// True when the app token must be (re)minted before a game op: none held, or past its early-refresh
	// window before expiry.
	bool NeedsAppTokenRefresh() const;

	// Shared success tail for every SESSION-scoped sign-in (email, dev, social, magic-link): store the
	// token + scope + user, persist to the vault, remember, broadcast, and pull orgs/apps/environments.
	// One place so the four paths cannot drift.
	void FinishSessionSignIn(const FString& Token, int64 InUserId, const FString& StatusMsg);

	// Social step 2 (from the loopback-captured code) and magic-link step 2 (from the loopback token or
	// the dev short-circuit token): exchange for the SESSION token, then FinishSessionSignIn.
	void CompleteSocialSignIn(const FString& Provider, const FString& Code, const FString& State);
	// StatusMsg is the message shown on success (defaults to a plain "Signed in."); the dev short-circuit
	// passes a message explaining why it signed in without an email.
	void CompleteMagicLink(const FString& Token, const FString& StatusMsg = FString());

	// Create the loopback listener on first interactive sign-in (lazy).
	void EnsureLoopback();
	// True while an interactive (browser/loopback) sign-in is mid-flight: from reserving the redirect URI
	// (bLoopbackFlowPending) until the listener is armed, then while it is live (LoopbackServer->IsActive).
	// Serializes the single listener across the social and magic-link flows. Game-thread-only.
	bool IsInteractiveSignInBusy() const;

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

	// Loopback HTTP listener for the social / magic-link redirect, created on first interactive sign-in.
	// A plain TUniquePtr is fine here (the controller is not a UObject, so the UHT special-member
	// constraint that forced TPimplPtr in the runtime auth does not apply); the out-of-line dtor
	// instantiates the deleter where the type is complete.
	TUniquePtr<FCrowdyLoopbackAuthServer> LoopbackServer;
	// Covers the reserve -> arm window that LoopbackServer->IsActive() alone cannot. Game-thread-only.
	bool bLoopbackFlowPending = false;
	// The enabled federated providers (availableLoginProviders); empty until fetched, or if none.
	TArray<FString> LoginProviders;

	// App-scoped game token + its authoritative per-app endpoint, minted from the session token for
	// the selected app. Game API ops bear this token (never AuthToken). Cleared on app-switch/sign-out.
	FString GameAppToken;
	FString GameApiUrlOverride;
	FDateTime GameAppTokenExpiresAt;
	bool bHaveAppTokenExpiry = false;

	// Single-in-flight-mint serialization (all game-thread): waiters queued while a mint runs are
	// flushed together when it resolves, so a burst of game ops triggers at most one mintAppToken.
	bool bMintInFlight = false;
	int64 MintInFlightAppId = 0;
	TArray<TFunction<void(bool /*bReady*/)>> PendingMintWaiters;

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
