// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Misc/DateTime.h"
#include "Model/CrowdyStudioTypes.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"

#include "CrowdyCppClient.h"

class FJsonObject;
class FCrowdyCppAdminClientHost;
class FCrowdyLoopbackAuthServer;

#if WITH_DEV_AUTOMATION_TESTS
struct FCrowdyStudioControllerTestAccess;
#endif

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

	// Passwordless sign-in that yields a mint-capable SESSION token, exactly like email/dev login (so
	// unlike an org token it can mint an app token and unlock game-plane authoring). Both drive the OAuth
	// dance in the SYSTEM browser per the native-client docs - never an embedded webview - and capture
	// the redirect on a 127.0.0.1 loopback listener. FetchAvailableProviders lists which providers the
	// server has enabled, so the sign-in view renders one button per provider instead of hard-coding them.
	void FetchAvailableProviders();
	void SignInWithSocial(const FString& Provider);
	// Emails a one-time sign-in link whose redirect the loopback captures.
	void SignInWithMagicLink(const FString& Email);

	void SignOut();

	// The signed-in developer's own account. Only a session sign-in has one; an organization token does not.
	bool CanManageAccount() const { return bSignedIn && AuthScope == ECrowdyStudioAuthScope::Session; }
	void FetchMyProfile();
	void SetMyName(const FString& Gamertag, const FString& Disambiguation);
	void ChangeMyPassword(const FString& CurrentPassword, const FString& NewPassword);
	void AddMyPassword(const FString& NewPassword);
	// Ends every session of the account on every device, this editor's included, then signs out here.
	void SignOutEverywhere();
	const FString& GetMyEmail() const { return MyEmail; }
	const FString& GetMyGamertag() const { return MyGamertag; }
	const FString& GetMyDisambiguation() const { return MyDisambiguation; }

	// Organizations
	void FetchMyOrganizations();
	void CreateOrganization(const FString& Name, const FString& Slug);

	// Apps
	// Lists every app the token can see (myApps) - no org needed, ids never hand-typed.
	void FetchApps();
	// Datacenter is the permanent placement code from GetPlaceableDatacenters(); the server refuses a create without
	// one. OnDone runs once with the new app's id, or 0 when the create failed (the status line carries the reason).
	void CreateApp(int64 OrgId, const FString& Name, const FString& Slug, const FString& Datacenter,
		const FString& Description, TFunction<void(int64 /*CreatedAppId*/)> OnDone = TFunction<void(int64)>());
	// Only the non-empty fields are sent; the server leaves the rest as they are. Description is optional so an
	// unset value means unchanged while an empty one clears it. Status LIVE publishes, DRAFT unpublishes, and a
	// status change is also how an archived app is restored.
	void UpdateApp(int64 AppId, const FString& Name, const TOptional<FString>& Description, const FString& Status, const FString& Visibility);
	void ArchiveApp(int64 AppId);
	void FetchApp(int64 AppId);
	// The datacenters this deployment can place a new app in. Cached after the first answer; the create dialog
	// reads GetPlaceableDatacenters() and calls this again only to retry a failed read.
	void FetchPlaceableDatacenters();
	const TArray<FStudioDatacenter>& GetPlaceableDatacenters() const { return PlaceableDatacenters; }
	bool HasFetchedPlaceableDatacenters() const { return bPlaceableDatacentersFetched; }
	bool DidPlaceableDatacentersFail() const { return bPlaceableDatacentersFailed; }
	// platformConfig.freeAppsPerOrg (0 until the platform config has been read).
	int32 GetFreeAppsPerOrg() const { return FreeAppsPerOrg; }

	//  Config Sync
	void SyncConfig();
	FStudioSettingsSnapshot GetCurrentSettings() const;

	// What SyncConfig would leave the settings as, computed without writing anything, so the
	// Project view can show an honest before/after diff. A sync changes the app id, the org, and
	// the game endpoints (taken from the app's own routing); the management URL is owned by the
	// backend selector below.
	FStudioSettingsSnapshot BuildProposedSettings() const;

	// Backend selector (Dev/Prod/Custom): which Crowdy deployment's shared origin the editor and the
	// runtime talk to. The game endpoints follow from the selected app's routing, not from this.
	FString GetBackendMode() const;
	void SetBackendMode(const FString& Mode);
	FString GetCustomDiscoveryUrl() const;
	void SetCustomDiscoveryUrl(const FString& Url);
	FString GetEffectiveDiscoveryUrl() const;

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

	// App access: the app's feature keys, the access tiers, and which features each tier grants. Features and grants
	// are Game API fields, so they bear the app token (the management bearer for an organization-token sign-in); the
	// tier list is a management read.
	// Load the three lists for the selected app, unless they are already the selected app's. Selecting an app empties
	// them, so the Access page calls this when it is actually shown and an app the user never opens that page for
	// costs no reads.
	//
	// The "already loaded" mark is set when the reads are ISSUED, not when they arrive, because the caller runs from
	// the paint path and an unmarked call would re-issue every frame. One automatic attempt per app: if it fails, the
	// status line carries the reason and Refresh is the retry.
	void EnsureAccessListsLoaded();
	void FetchFeatures();
	void DefineFeature(const FString& FeatureKey, const FString& Description);
	void FetchTierFeatures();
	// Read-only list of the app's access tiers, so the tier-feature grant can use a tier dropdown instead of a
	// hand-typed tier id. Tiers are created/edited in the web console.
	void FetchAppAccessTiers();
	void GrantTierFeature(int64 TierId, const FString& FeatureKey);
	void RevokeTierFeature(int64 TierId, const FString& FeatureKey);

	// True when a game-plane operation could be sent right now: a fresh app token is held, or a session sign-in can mint
	// one. False for an organization-token sign-in, which cannot mint.
	bool CanUseGamePlane() const;
	// True when feature and tier-grant operations could be sent right now: an organization-token sign-in (they go out on
	// the management bearer) or anything CanUseGamePlane accepts.
	bool CanUseAppAccess() const;
	// True while any of the selected app's access lists is being read, and before they have been requested at all, so
	// a page can tell "not here yet" from "this app has none". Counted, so overlapping refreshes do not clear each other.
	bool AreAccessListsLoading() const;
	// True while a feature define, grant or revoke is awaiting its reply.
	bool IsAccessWriteInFlight() const { return AccessWritesInFlight > 0; }

	// Selection
	void SelectOrg(int64 OrgId);
	void SelectApp(int64 AppId);

	int64 GetSelectedOrgId() const { return SelectedOrgId; }
	int64 GetSelectedAppId() const { return SelectedAppId; }

	TSharedPtr<FStudioOrg> GetSelectedOrg() const;
	TSharedPtr<FStudioApp> GetSelectedApp() const;

	// State the views read 
	const TArray<TSharedPtr<FStudioOrg>>& GetOrganizations() const { return Organizations; }
	const TArray<TSharedPtr<FStudioApp>>& GetApps() const { return Apps; }

	/**
	 * Whether an app list is being fetched right now.
	 *
	 * The list alone cannot answer this. An empty list means "the server said you have none" and "I have not
	 * finished asking" equally well, and a view that only reads the count states the first while the second is
	 * true: a slow sign-in shows "No apps yet. Create one below" to someone who has apps.
	 */
	bool IsFetchingApps() const { return AppsFetchInFlight > 0; }
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

	const TArray<TSharedPtr<FStudioAppFeature>>& GetFeatures() const { return Features; }
	const TArray<TSharedPtr<FStudioTierFeature>>& GetTierFeatures() const { return TierFeatures; }
	const TArray<TSharedPtr<FStudioAccessTier>>& GetAccessTiers() const { return AccessTiers; }

	bool IsSignedIn() const { return bSignedIn; }

	// True when the selected org's effective permissions include PermissionKey. The console uses this
	// to grey the management actions a token can't perform. With no org selected, or before the org's
	// permissions have been fetched, it returns false so an action stays disabled until its permission
	// is confirmed present.
	bool HasOrgPermission(const FString& PermissionKey) const;

	// Convenience gate for the one management-plane mutation the native console still issues itself, creating an
	// app. Everything else admin moved to the web console.
	bool CanManageApps() const;
	// The current bearer token - used to single-sign-on the embedded web console (injected as
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
	FSimpleMulticastDelegate OnPlaceableDatacentersChanged;
	FSimpleMulticastDelegate OnConfigChanged;
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
	FSimpleMulticastDelegate OnFeaturesChanged;
	FSimpleMulticastDelegate OnTierFeaturesChanged;
	FSimpleMulticastDelegate OnAccessTiersChanged;
	FSimpleMulticastDelegate OnMyProfileChanged;
	FOnStudioStatusMessage OnStatusMessage;

private:
	// Empty every list and selection that belongs to one app, and tell the views. Called on app switch so the
	// previous app's teams, channels, grids and access lists can never stay on screen under the new app's name. Purely
	// local: it issues no request, so the caller stays in charge of what is re-fetched and when.
	void ClearAppScopedState();

	FString ResolveDiscoveryBaseUrl() const;
	FString ResolveDiscoveryUrl() const;
	FString ResolveGameUrl() const;

	// The one place a console operation reaches the server. Resolves the shared API client, tracks the busy count,
	// maps a failure to a status message, and hands OnSuccess a { "data": ... } envelope. Plane picks which bearer
	// the call carries, which for the operations served at both endpoints is not something the endpoint can decide.
	//
	// A completion always runs and always tells the caller the outcome, including when the client could not be
	// resolved at all: several callers are counting replies, and one that never arrives stalls them for good.
	void IssueOperation(ECrowdyCppApiDomain Domain, const TCHAR* OperationName, ECrowdyCppTokenPlane Plane,
		const TSharedPtr<FJsonObject>& Variables,
		TFunction<void(const TSharedPtr<FJsonObject>& /*Envelope*/)> OnSuccess,
		TFunction<void()> OnFailure, bool bReportErrors);

	// Issues a named operation against the management endpoint with the current bearer token, tracks the
	// busy count, and only calls OnSuccess once the envelope is clean (HTTP ok + no
	// GraphQL errors). Any failure is turned into a status message for the views, then
	// OnFailure runs if the caller needs to undo something (e.g. a bad token).
	// bReportErrors=false suppresses the error status toast (used by the best-effort provider probe,
	// which must degrade silently on a backend that doesn't expose availableLoginProviders); OnFailure
	// still runs.
	//
	// Domain selects which generated operation set OperationName is looked up in; that lookup owns both the
	// GraphQL document and the endpoint, so neither is written out here. OnSuccess still receives a whole
	// { "data": ... } envelope, which is what every CrowdyStudioGql::Parse* function reads.
	void SendManagement(ECrowdyCppApiDomain Domain, const TCHAR* OperationName,
		const TSharedPtr<FJsonObject>& Variables,
		TFunction<void(const TSharedPtr<FJsonObject>& /*Envelope*/)> OnSuccess,
		TFunction<void()> OnFailure = TFunction<void()>(), bool bReportErrors = true);

	// SendManagement for an app-scoped operation: OnSuccess and OnFailure are dropped if the selected app has changed by the reply.
	void SendManagementForApp(ECrowdyCppApiDomain Domain, const TCHAR* OperationName,
		const TSharedPtr<FJsonObject>& Variables,
		TFunction<void(const TSharedPtr<FJsonObject>& /*Envelope*/)> OnSuccess,
		TFunction<void()> OnFailure = TFunction<void()>());

	// Same contract as SendManagement, but issues against the game endpoint. Game API ops authorize with
	// the app-scoped token (minted from the session token via mintAppToken), NOT the session token
	// itself - the two-token model. Mints (or refreshes) that token first when it is missing or near
	// expiry, then posts; an org-token sign-in cannot mint, so its game ops surface a clear error.
	// OnFailure (optional, default-empty so the ~40 existing call sites are unchanged) runs after the error status
	// on any failure: a missing game endpoint, a mint that could not proceed, or a GraphQL/HTTP error. It lets the
	// schema-sync flows react to a failed read/write (surface "plan failed" / "applied N of M") instead of the
	// silent stall a callback-only-on-success op leaves behind.
	//
	// Every game operation is scoped to the app selected when it is issued, so that app is pinned here and NEITHER
	// callback runs if the selection has moved by the time the reply lands. Without that, a reply for the previous
	// app arrives after the switch has already emptied the app-scoped state and writes the previous app's teams,
	// schema or functions straight back under the new app's name. A caller that must act across an app change
	// therefore cannot use this path.
	void SendGame(ECrowdyCppApiDomain Domain, const TCHAR* OperationName,
		const TSharedPtr<FJsonObject>& Variables,
		TFunction<void(const TSharedPtr<FJsonObject>& /*Envelope*/)> OnSuccess,
		TFunction<void()> OnFailure = TFunction<void()>());
	// Feature and tier-grant operations: the management plane for an organization-token sign-in, which the server
	// accepts as a full-scope org token, otherwise SendGame with the minted app token. Same callback contract as SendGame.
	void SendAppAccess(const TCHAR* OperationName, const TSharedPtr<FJsonObject>& Variables,
		TFunction<void(const TSharedPtr<FJsonObject>& /*Envelope*/)> OnSuccess,
		TFunction<void()> OnFailure = TFunction<void()>());
	// The actual game-endpoint issue, once an app token is in hand. Split out of SendGame so the
	// async "mint then post" path can re-enter it without re-checking the token.
	void PostGame(ECrowdyCppApiDomain Domain, const TCHAR* OperationName,
		const TSharedPtr<FJsonObject>& Variables,
		TFunction<void(const TSharedPtr<FJsonObject>& /*Envelope*/)> OnSuccess,
		TFunction<void()> OnFailure = TFunction<void()>());

	// Mint the app-scoped game token for the selected app from the session token (mintAppToken,
	// management plane). OnDone(true) once a token for the *current* app is in hand; OnDone(false) on
	// failure (after a descriptive status). Concurrent requests fold into a single in-flight mint
	// (no redundant mutations, no GameAppToken write race); a mint whose app no longer matches the
	// current selection is discarded and re-issued for the new app so a waiting op still gets a token.
	void MintAppToken(TFunction<void(bool /*bMinted*/)> OnDone);
	// Begin one mintAppToken round-trip for SelectedAppId. Precondition: CanMintAppToken(), not already
	// in flight, with at least one queued waiter. Its completion applies the token (if still current)
	// and flushes the waiters via FinishMint.
	// bConsentAsked is true on the single retry after the player accepted the terms, so a second refusal fails plainly.
	void StartMint(bool bConsentAsked = false);
	// A mint refused for missing consent: ask the player, then retry the mint once or fail the queued waiters.
	void AskConsentThenRemint(int64 MintForAppId, const TSharedRef<FCrowdyCppClient>& Client);
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

	// Right after sign-in, pull the app list so the console opens populated instead of empty.
	void FetchAppsForSignedInUser();
	// appDiscovery for one app: its datacenter code and WS endpoint, merged into the list entry.
	void FetchAppDiscovery(int64 AppId);
	void FetchPlatformConfig();

	/** Drop one app fetch from the in-flight count. Called on both the success and the failure path. */
	void ReleaseAppsFetch();

	void PersistSelection() const;
	class UCrowdyStudioUserSettings* GetUserSettings() const;

	FString AuthToken;
	ECrowdyStudioAuthScope AuthScope = ECrowdyStudioAuthScope::None;
	bool bSignedIn = false;
	int64 UserId = 0;

	FString MyEmail;
	FString MyGamertag;
	FString MyDisambiguation;

	// Issues one account call on the shared client under the session token, then reports Done or the failure.
	void RunAccountCall(const TCHAR* Doing, TFunction<FCrowdyCppRequestHandle(FCrowdyCppClient&, TFunction<void(FCrowdyCppBoolResult)>)> Issue,
		const FString& Done, TFunction<void()> OnSucceeded = TFunction<void()>());

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

	/**
	 * The single API client every console operation is issued on, and the shared origin it is bound to so that a
	 * backend switch rebuilds it. Owned for the console's lifetime rather than per call, because a sign-in, the
	 * mint that follows it, and the app reads that follow that all belong to one flow.
	 *
	 * This is deliberately not the runtime's game-instance client host: the console is usable with no play session
	 * running, which is most of the time, and a game-instance subsystem does not exist then.
	 *
	 * It is bound to the shared origin and not to the selected app's endpoint on purpose. See ResolveApiClient: the
	 * shared origin is the environment the user chose, while a stored per-app endpoint is derived data that can name
	 * a different one.
	 */
	TSharedPtr<FCrowdyCppAdminClientHost> ApiClientHost;
	FString ApiClientDiscoveryUrl;

	/**
	 * Clients replaced by an endpoint change that still have requests in flight. Usually only one of the two
	 * endpoints moves, so releasing the old client outright would cancel requests on the plane that did not move:
	 * a mint answering while the console's org, app and environment reads are still open would empty all three
	 * lists with nothing shown to explain it. A retired host keeps its own ticker, so its requests finish normally
	 * and are dropped here once they have drained.
	 */
	TArray<TSharedPtr<FCrowdyCppAdminClientHost>> RetiringApiClientHosts;

	/**
	 * The in-flight sign-in and provider probe, so a second attempt supersedes the first rather than racing it.
	 * Neither flow has any other guard: two Log In clicks would otherwise both complete, and whichever landed last
	 * would win the vault write, so a stale attempt could overwrite the identity the user actually signed in as.
	 */
	FCrowdyCppRequestHandle SignInRequest;
	FCrowdyCppRequestHandle ProvidersRequest;

	/**
	 * The client every call is issued on, or null if it could not be built, in which case the operation cannot
	 * proceed. Returned by shared pointer rather than raw: the caller's next statement can rebuild the client
	 * (anything that broadcasts can reach a controller op), and a raw pointer would dangle across that.
	 *
	 * Both bearers are installed on every resolve rather than once, so a sign-in or a mint that lands after the
	 * client was built is picked up, and neither plane can inherit whichever token the previous caller left behind.
	 */
	TSharedPtr<FCrowdyCppClient> ResolveApiClient(const TCHAR* OperationName);

	/**
	 * Turn a facade failure into something worth showing a user. A cancellation is the console tearing its own
	 * client down, not an answer from the server, so it must not be dressed up as one.
	 */
	static FString DescribeApiFailure(const FString& ErrorMessage, bool& bOutWasCanceled);

	// Whether the failure OnFailure() is about to report was a cancellation (the console retiring its own client)
	// rather than a genuine server/transport error. Set immediately before every OnFailure() call in
	// IssueOperation/SendGame/PostGame - true or false, never left over from an earlier operation - so a caller
	// that needs to tell the two apart can read it from inside that same OnFailure call. Only meaningful for the
	// duration of that call; do not read it any later.
	bool bLastFailureWasCanceled = false;

	/** Social step 1 completion: arm the listener against the server's CSRF state, then open the consent page. */
	void OnSocialLoginStarted(const FString& Provider, const FString& AuthorizeUrl, const FString& State);

	/** Magic-link step 1 completion: wait for the listener, or give up when nothing was sent. */
	void OnLoginLinkRequested(bool bSent);

	/** Adopt a freshly minted app token and its per-app endpoint, whichever transport produced it. */
	void ApplyMintedAppToken(int64 MintForAppId, const FString& Token, const FString& GameUrl,
	                         const FString& ExpiresAtStr);

	TArray<TSharedPtr<FStudioOrg>> Organizations;
	TArray<TSharedPtr<FStudioApp>> Apps;

	/** How many app-list fetches are in flight. Counted, so two overlapping refreshes do not clear each other. */
	int32 AppsFetchInFlight = 0;
	TArray<FStudioDatacenter> PlaceableDatacenters;
	bool bPlaceableDatacentersFetched = false;
	bool bPlaceableDatacentersFailed = false;
	int32 FreeAppsPerOrg = 0;
	// The placement codes and the free-slot count belong to one deployment: dropped on sign-out and on a backend switch.
	void ClearPlacementCache();
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

	TArray<TSharedPtr<FStudioAppFeature>> Features;
	TArray<TSharedPtr<FStudioTierFeature>> TierFeatures;
	TArray<TSharedPtr<FStudioAccessTier>> AccessTiers;
	// The app the three lists above were last requested for, so the Access page loads them once per app rather than
	// re-issuing the reads on every frame it is painted. Zero means nothing has been requested for any app yet, which
	// is also the state an app switch returns it to.
	int64 AccessListsAppId = 0;
	// Access reads and writes awaiting a reply. A reply for an app that was switched away from drops both callbacks, so
	// ClearAppScopedState zeroes them rather than trusting every issue to be matched by a release.
	int32 AccessReadsInFlight = 0;
	int32 AccessWritesInFlight = 0;
	void ReleaseAccessRead() { AccessReadsInFlight = FMath::Max(0, AccessReadsInFlight - 1); }
	void ReleaseAccessWrite() { AccessWritesInFlight = FMath::Max(0, AccessWritesInFlight - 1); }

	int64 SelectedOrgId = 0;
	int64 SelectedAppId = 0;

	int32 InFlightCount = 0;
	FString StatusMessage;
	bool bStatusWasError = false;

#if WITH_DEV_AUTOMATION_TESTS
	// The app-scoped reset is internal because no view may drive it directly, which also leaves the automation tests
	// no other way to exercise it.
	friend struct FCrowdyStudioControllerTestAccess;
#endif
};
