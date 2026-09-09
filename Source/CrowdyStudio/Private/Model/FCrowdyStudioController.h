// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Misc/DateTime.h"
#include "Model/CrowdyStudioTypes.h"
#include "GameModel/CrowdyApplySelection.h" // FCrowdyApplyPlan, FCrowdyApplyPlanInput, FCrowdyApplyUnit
#include "GameModel/CrowdyGameModelDelete.h" // FCrowdyDeletePlan, FCrowdyDeleteOp, FCrowdyDeleteOutcome, FCrowdyDeleteLiveCount
#include "GameModel/CrowdyModelLoadState.h" // ECrowdyModelFamily, ECrowdyModelLoadState, FCrowdyFamilyLoad
#include "GameModel/CrowdyModelSnapshot.h" // FCrowdyModelSnapshot
#include "GameModel/CrowdySchemaSync.h" // FCrowdySchemaSyncReport, FCrowdySchemaTypeUpsert/PropUpsert
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"
#include "UObject/StrongObjectPtr.h"

#include "CrowdyCppClient.h"

class FJsonObject;
class FCrowdyCppAdminClientHost;
class FCrowdyLoopbackAuthServer;
class UCrowdyGameKitConfig;
struct FCrowdySchemaSyncRun;
struct FStreamableHandle;

#if WITH_DEV_AUTOMATION_TESTS
struct FCrowdyStudioControllerTestAccess;
#endif

enum class ECrowdyStudioAuthScope : uint8
{
	None,
	Session,
	OrgToken
};

// A readiness signal for the Game Model setup strip (App / Session channel / Schema). Unknown means "not determined
// yet" (e.g. no plan has run to resolve the session channel), so the strip can show a neutral dot rather than a
// false red. Advisory means there is something to look at that no button on the strip can change, so it is told
// apart from NotReady rather than painted as an action the reader cannot take.
enum class ECrowdyStudioReadiness : uint8
{
	Unknown,
	NotReady,
	Ready,
	Advisory
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

	// Organizations 
	void FetchMyOrganizations();
	void CreateOrganization(const FString& Name, const FString& Slug);

	// Apps
	// Lists every app the token can see (myApps) - no org needed, ids never hand-typed.
	void FetchApps();
	void CreateApp(int64 OrgId, const FString& Name, const FString& Slug, const FString& Status, const FString& Visibility);
	void UpdateApp(int64 AppId, const FString& Name, const FString& Status, const FString& Visibility);
	void ArchiveApp(int64 AppId);
	void FetchApp(int64 AppId);

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

	// Game model (game plane, app-admin). The design-time schema the runtime consumes: container
	// types and their properties, sandboxed functions, feature keys, and tier-feature grants.
	void FetchContainerTypes();
	// ExpectedAppId is the app the caller's on-screen editor was filled from. A container type usually carries the
	// SAME name in a development and a production app, since both are deployed from the same project, so a write has
	// to be aimed at the app its subject was read from rather than at whatever happens to be selected when the button
	// is pressed. No default value, so every call site is compiler-forced to answer.
	void UpsertContainerType(const FString& TypeName, const FString& DisplayName, const FString& Description,
		const FString& InstantiableBy, const FString& DefaultPropertyVisibility, int64 ExpectedAppId);
	// Loads one container type's attributes, lazily on selection, and caches them per type. Attributes are never
	// fetched for every type at once: one query per type across a large schema would be dozens of round trips before
	// anything renders. A request already in flight for the same type is coalesced onto the one round-trip, unless
	// bForceRefresh is set: a write to the type has to re-read it, and the read already in flight was issued before
	// that write, so it would answer with the pre-write list.
	void FetchPropertyDefs(const FString& TypeName, bool bForceRefresh = false);
	// ExpectedAppId as on UpsertContainerType above, and for the same reason.
	void UpsertPropertyDef(const FString& TypeName, const FString& Key, const FString& ValueType,
		const FString& DefaultValueJson, const FString& Visibility, const FString& Writable, const FString& Description,
		int64 ExpectedAppId);
	void FetchFunctions(const FString& ContainerTypeFilter);
	// Parameters/mutations/invoke-policy are passed as JSON the server compiles; an empty string omits them.
	// ExpectedAppId as on UpsertContainerType above, and for the same reason.
	void UpsertFunction(const FString& Name, const FString& ContainerTypeName, const FString& Description,
		const FString& ReturnType, const FString& InvokeScope, const FString& ReturnExpression,
		const FString& ParametersJson, const FString& MutationsJson, const FString& InvokePolicyJson,
		int64 ExpectedAppId);

	// The four schema deletes and the live-model delete below are DESTRUCTIVE and not undoable: the Game Model API
	// has no soft delete for any of them. Every one takes ExpectedAppId, the app the caller's on-screen list was read
	// from, and refuses when the selection has moved on since, so nothing can be deleted from an app the user was not
	// looking at. That check is about APP identity only; it cannot catch a control acting on a row that is no longer
	// the one on screen, so the caller still has to read its target from the widget at the moment of the click.
	//
	// OwningTypeName on the function delete is the model the function belongs to. The mutation itself takes only
	// (appId, name), and whether the server scopes that name by model is not settled, so the parameter is not passed
	// on the wire: it is here so nothing between the row and this call has to guess which model a bare name came from,
	// and so the status line and the refresh that follow can name the right one. A caller must NOT read it as a
	// promise that the delete is confined to that model. Where the same function name exists on more than one model,
	// the review's own finding says plainly that every one of them may go.
	void DeleteFunction(const FString& OwningTypeName, const FString& Name, int64 ExpectedAppId);
	// Deleting a model removes its attributes with it, and is REFUSED while live models of it exist or while
	// functions are still bound to it. Both refusals arrive as GraphQL errors, not as a silent success.
	void DeleteContainerType(const FString& TypeName, int64 ExpectedAppId);
	// Deleting an attribute refuses nothing and cascades nothing. The values already stored for it on every live
	// model stay behind, and every expression naming the key keeps naming it. The caller's pre-flight is the only
	// guard that exists anywhere for this one.
	void DeletePropertyDef(const FString& TypeName, const FString& Key, int64 ExpectedAppId);
	// Deleting an automation removes its event triggers with it.
	void DeleteAutomation(const FString& Name, int64 ExpectedAppId);
	// Reads every automation for the selected app, then (chained) every event trigger. A trigger references its
	// automation by id while the views key them by name, so the automations must be in hand before the triggers are
	// parsed. OnAutomationsChanged fires once, after both reads have settled.
	void FetchAutomations();
	// Load the three app-wide game-model lists (container types, functions, automations) for the selected app, unless
	// they are already the selected app's. Selecting an app empties them and nothing used to refill them, so the Game
	// Model page opened onto an empty browser with nothing on screen saying a button press was needed. The page calls
	// this when it is actually shown, so an app the user never opens that page for costs no reads.
	//
	// The "already loaded" mark is set when the reads are ISSUED, not when they arrive, because the caller runs from
	// the paint path and an unmarked call would re-issue every frame. One automatic attempt per app: if it fails, the
	// status line carries the reason and Refresh is the retry.
	void EnsureGameModelListsLoaded();
	// Whether the call above has anything to do. Pure and static so the rule is exercised with no controller, no token
	// and no server: an app must be selected, and the lists must not already be that app's. A zero LoadedAppId means
	// nothing has been loaded for any app, which is also where an app switch leaves it.
	static bool ShouldLoadGameModelLists(int64 SelectedAppId, int64 LoadedAppId);
	void FetchFeatures();
	void DefineFeature(const FString& FeatureKey, const FString& Description);
	void FetchTierFeatures();
	// Read-only list of the app's access tiers (management plane), so the tier-feature grant can use
	// a tier dropdown instead of a hand-typed tier id. Tiers are created/edited in the web console.
	void FetchAppAccessTiers();
	void GrantTierFeature(int64 TierId, const FString& FeatureKey);
	void RevokeTierFeature(int64 TierId, const FString& FeatureKey);
	// Ask the server whether the selected app's game model hangs together, and report what it says.
	//
	// Run automatically after every authoring write that lands, because that is when the answer changes and when a
	// developer can act on it. It is also the only check that sees problems BETWEEN objects: writing a function that
	// calls one that does not exist succeeds, and nothing about that write is wrong until you look at the pair.
	//
	// bQuiet suppresses the status line for a clean result, so a follow-up lint does not overwrite the message the
	// write itself just posted. Findings are reported either way.
	void RunGameModelLint(bool bQuiet = false);
	const FStudioLintReport& GetGameModelLintReport() const { return GameModelLint; }

	void FetchGameModelPolicy();
	void SetGameModelPolicy(const FString& SessionCreationPolicy, const FString& DefaultParticipantRole);
	// Bulk import: SeedJson is a SeedGameModelInput object (minus appId, which is injected here).
	void SeedGameModel(const FString& SeedJson);

	// Schema sync (code -> server, game plane, app-admin). Reflects every meta=(CrowdyContainer) class's
	// meta=(CrowdyModel) attributes AND every UCrowdyEffect asset's compiled function, structurally diffing
	// both against the live server schema. PlanSchemaSync is a DRY RUN (reads + diffs, no writes) whose result
	// is in GetSchemaSyncReport(); ApplySchemaSync then writes only the planned deltas via idempotent
	// gameModelUpsert* ops (container types + property defs + functions, including the functions' model-changed
	// notifications).
	void PlanSchemaSync();
	void ApplySchemaSync();

	// The pending plan as the selection layer reads it: the five upsert arrays by pointer, pinned to the app the plan
	// was computed for. The arrays are the controller's, so this is only valid for as long as the caller holds it and
	// nothing re-plans underneath.
	FCrowdyApplyPlanInput MakeSchemaApplyPlanInput() const;
	// What the next apply sends, by identity key rather than by index: creating the app's session channel rebuilds all
	// five pending arrays from a fresh diff, and an index into the old arrays would then name a different entity.
	void SetSchemaApplySelection(const TArray<FString>& UnitKeys);
	// Back to everything, which is what makes an apply nobody narrowed identical to the all-or-nothing one.
	void ClearSchemaApplySelection();
	const TArray<FString>& GetSchemaApplySelection() const { return SchemaApplySelection; }
	// Send one reviewed selection. Refused unless the plan's own app, ExpectedAppId and the live selection all agree,
	// and unless the plan is sendable: two comparisons against two sources, exactly as CommitDeletePlan takes, because
	// a guard comparing one of them to itself would agree however stale the sheet on screen had become.
	//
	// Returns whether the send was issued. A caller that closes its review surface afterwards has to be told, or a
	// refusal throws away the reviewed selection over a press that wrote nothing.
	bool ApplySchemaSelection(const FCrowdyApplyPlan& Plan, int64 ExpectedAppId);
	// Whether a plan is running for the app currently on screen, and what it is doing. A plan is several phases long
	// and every one of them is asynchronous, so without this the page looks idle from the click until the report
	// appears. Scoped to the selection on purpose: a plan whose app was switched away is already superseded and will
	// never repaint anything, so showing it as busy under the new app would report activity that leads nowhere.
	bool IsSchemaPlanInFlight() const { return SchemaPlanBusyAppId != 0 && SchemaPlanBusyAppId == SelectedAppId; }
	// Present tense, one short phrase, for a label beside the indicator. Empty when no plan is running.
	const FString& GetSchemaPlanPhase() const { return SchemaPlanPhase; }
	const FCrowdySchemaSyncReport& GetSchemaSyncReport() const { return SchemaSyncReport; }
	bool HasPendingSchemaSync() const { return PendingSyncTypeUpserts.Num() + PendingSyncPropUpserts.Num() + PendingSyncFunctionUpserts.Num() + PendingSyncAutomationUpserts.Num() + PendingSyncTriggerUpserts.Num() > 0; }
	// DESTRUCTIVELY delete the server-only container types + property defs + functions the last plan
	// surfaced (the sync itself never deletes). manage_apps-gated, pinned to the planned app; the view confirms
	// before calling. A type with live containers / bound functions is refused server-side and surfaces as an error.
	void PruneServerOnlySchema();
	bool HasPendingPrune() const { return PendingPruneTypes.Num() + PendingPruneProps.Num() + PendingPruneFunctions.Num() + PendingPruneAutomations.Num() > 0; }

	// The last finished plan for the SELECTED app, or null when no plan has finished for it. This is what lets the
	// Models browser say where each model, attribute, function and automation came from without re-running a plan.
	// Retained per app, so switching away and back does not lose the last verdict.
	//
	// In memory only, deliberately: it describes one moment of one server's schema against one working copy of the
	// project, and a copy of that read back from disk at the next editor start would be a confident-looking answer
	// about a server and a project that have both moved on since. Pressing Preview changes is what produces one.
	TSharedPtr<const FCrowdyModelSnapshot> GetModelSnapshot() const;

	// Deploy a Game Kit config (its authored genre layers) against the selected app: the emit produces one bundle
	// of container types, property defs, functions, and automations, which the CrowdyReplication deploy facade
	// seeds server-side. Game plane, app-admin: mints/refreshes the app token first (needs a session sign-in that
	// can mint), then runs the async deploy. OnDone reports the final outcome and a human-readable message on the
	// game thread. Writing to the live server, so the view confirms before calling.
	void DeployGameKit(const class UCrowdyGameKitConfig* Config,
		TFunction<void(bool /*bOk*/, const FString& /*Message*/)> OnDone);

	// The Studio-owned Game Kit config edited inline in the Deploy Kit card. A transient object (never a content
	// asset) created on first use and held for the editor session, so a designer configures genre layers in the
	// Studio and deploys without creating an asset. Never null after this returns.
	class UCrowdyGameKitConfig* GetKitDeployConfig();

	// Live runtime state (read-only): the runtime's instantiated containers, and one container's
	// visible property values. Game plane; works even outside PIE (it reads server state, not the
	// editor's play world). Filters are optional (empty = all).
	//
	// Limit/Offset page over the server's stable created-at ordering. A Limit of 0 or less asks for no page at
	// all and reads every container matching the filters, which is what a caller that does not page wants.
	// bAppend keeps the containers already read and adds the new page to them; a container already held is
	// skipped, because a window that shifted (something was deleted between two reads) hands back a row that
	// is already on screen.
	void FetchContainers(const FString& TypeNameFilter, const FString& SessionIdFilter,
		int32 Limit = 0, int32 Offset = 0, bool bAppend = false);
	void FetchContainerState(const FString& ContainerId);
	// DESTRUCTIVE and not undoable: the Game Model API has no soft delete for a live instance, and its stored
	// property values go with it. ExpectedAppId is the app the caller's on-screen list was read from; the delete
	// is refused if the selection has moved on since.
	void DeleteContainer(const FString& ContainerId, int64 ExpectedAppId);

	// Count the live models of each named model for a delete pre-flight, one bounded read per model, and report
	// every count together once they have all settled.
	//
	// SIDE-EFFECT FREE, and that is the whole reason it exists rather than the caller reusing FetchContainers. The
	// shared container list is filtered by whatever was last typed into the Live tab's boxes, so counting live models
	// from it would report zero for a model with four hundred instances and turn a refusal nobody was warned about
	// into a green light. Nothing here writes Containers, ContainerState, the paging state or the filters, and
	// nothing here broadcasts OnContainersChanged.
	//
	// PerTypeLimit bounds each read, so a model with a million instances cannot turn a pre-flight into a stall; a
	// count that hit the limit comes back as a floor rather than a total and says so.
	//
	// OnDone RUNS EXACTLY ONCE and ALWAYS, including when no app is selected, when a read fails, and when the app
	// selection changes while the reads are outstanding. The caller continues from this completion, so a path that
	// silently declines to fire it strands a review saying a check is under way with no way to start another. A model
	// whose read never landed comes back Unknown, never zero. AppId is the app the counts were read for, so a caller
	// can drop a completion for an app it is no longer showing.
	void CountLiveModelsScoped(const TArray<FString>& TypeNames, int32 PerTypeLimit,
		TFunction<void(int64 /*AppId*/, TArray<FCrowdyDeleteLiveCount>&& /*Counts*/)> OnDone);

	// Run one delete plan: ONE sequential walk over the plan's ops, in the order the plan fixed, that STOPS on the
	// first failure rather than skipping ahead, because a later op may depend on the one that failed. Everything up
	// to the stop is committed and idempotent, and GetDeleteRemainder is exactly what a second press has to finish.
	//
	// A reply of false means the entity was not there, which is an idempotent no-op and counts as success; only a
	// GraphQL or transport failure, or a reply whose shape carries no answer at all, stops the walk.
	//
	// Refused unless BOTH the plan's own app and ExpectedAppId match the live selection. Two comparisons against two
	// sources on purpose: the plan carries the app its evidence was gathered for, ExpectedAppId is the app the widget
	// that drew the sheet believes it is showing, and a guard that compared one of those to itself would agree no
	// matter how stale the sheet on screen had become. Also refused for a plan that is not committable, so the rule
	// that a blocker cannot be committed is enforced away from the widget that renders it.
	void CommitDeletePlan(const FCrowdyDeletePlan& Plan, int64 ExpectedAppId);
	// Whether a commit walk is running, and for the app currently on screen. Scoped to the selection for the same
	// reason the schema plan's indicator is: a walk whose app was switched away will never repaint anything here.
	bool IsDeleteCommitInFlight() const;
	// How far the running walk has got, and how far it has to go, for a progress line. Both zero when none is
	// running.
	int32 GetDeleteCommitCompleted() const { return DeleteCommitCompleted; }
	int32 GetDeleteCommitTotal() const { return DeleteCommitTotal; }
	// The last commit's result, whether it finished or stopped. Total zero means none has run for this app.
	const FCrowdyDeleteOutcome& GetLastDeleteOutcome() const { return LastDeleteOutcome; }
	// The ops a stopped commit did not run, in order, starting with the one that failed. Empty after a walk that
	// finished. This is the exact remainder a second press runs, not a recomputed guess at one.
	const TArray<FCrowdyDeleteOp>& GetDeleteRemainder() const { return DeleteRemainder; }

	// Delete every live model this app holds, or every one of a single model when TypeName is set. DESTRUCTIVE and
	// not undoable, one sequential delete at a time, stopping on the first failure. It reads its own pages rather
	// than the Live tab's list, which is filtered, and drains until a read comes back empty or a bound is hit.
	void PurgeContainers(const FString& TypeName, int64 ExpectedAppId);
	// Stop after the delete in flight. What is already deleted stays deleted, and pressing again clears the rest.
	void CancelContainerPurge();
	bool IsContainerPurgeInFlight() const;
	// How many deletes have settled, for a progress line. There is no total to show it against.
	int32 GetContainerPurgeCompleted() const { return ContainerPurgeCompleted; }
	const FCrowdyDeleteOutcome& GetLastContainerPurgeOutcome() const { return LastContainerPurgeOutcome; }

	// Selection
	void SelectOrg(int64 OrgId);
	void SelectApp(int64 AppId);

	int64 GetSelectedOrgId() const { return SelectedOrgId; }
	int64 GetSelectedAppId() const { return SelectedAppId; }
	// The app a pending schema plan was pinned to (0 when none). The apply/prune confirm dialog names this,
	// not the live selection, so it can never misidentify the app it is about to write to.
	int64 GetPlannedSyncAppId() const { return PlannedSyncAppId; }

	// The Game Model page's last-open tab ("models" / "live" / "advanced"), persisted across editor sessions.
	// The view owns validating/falling back an unrecognized or empty key; the controller only round-trips it.
	const FString& GetLastGameModelTab() const;
	void SetLastGameModelTab(const FString& TabKey);

	// Setup-strip readiness for the Game Model view. App = an app is selected; Session channel = the app's
	// __crowdy_session_<appId> channel is known to exist (resolved by a plan or just created); Schema = the last
	// plan found zero drift. Session channel + schema read Unknown until a plan has run.
	ECrowdyStudioReadiness GetAppReadiness() const { return SelectedAppId != 0 ? ECrowdyStudioReadiness::Ready : ECrowdyStudioReadiness::NotReady; }
	ECrowdyStudioReadiness GetSessionChannelReadiness() const { return SessionChannelReadiness; }
	ECrowdyStudioReadiness GetSchemaReadiness() const;

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

	const TArray<TSharedPtr<FStudioContainerType>>& GetContainerTypes() const { return ContainerTypes; }
	// The attributes of the most recently loaded container type. A convenience mirror of the per-type cache for a view
	// that only ever shows one type at a time; a view that shows several must use GetPropertyDefsForType.
	const TArray<TSharedPtr<FStudioPropertyDef>>& GetPropertyDefs() const { return PropertyDefs; }
	// One container type's cached attributes, or null when that type has never been loaded. Null and an empty array
	// mean different things: null is "not loaded yet, ask for it", empty is "loaded, this type has no attributes".
	const TArray<TSharedPtr<FStudioPropertyDef>>* GetPropertyDefsForType(const FString& TypeName) const;
	// The functions of whatever container type was asked for last. A view that shows one type at a time wants this.
	const TArray<TSharedPtr<FStudioFunction>>& GetFunctions() const { return Functions; }
	// Every function of the app, unaffected by another view narrowing the list above to a single container type. A view
	// that shows more than one type at a time must use this one.
	const TArray<TSharedPtr<FStudioFunction>>& GetUnfilteredFunctions() const { return UnfilteredFunctions; }
	const TArray<TSharedPtr<FStudioAutomation>>& GetAutomations() const { return Automations; }
	// The event triggers of the automations above. A trigger names its automation, and an automation may have none:
	// interval and cron automations carry their schedule on the automation itself.
	const TArray<TSharedPtr<FStudioAutomationTrigger>>& GetAutomationTriggers() const { return AutomationTriggers; }

	// What one family's read is doing for the selected app. An empty list means "this app has none" only when the
	// family reads Loaded; NeverRequested, Loading and Failed each say something different about the same empty list,
	// and a surface that cannot tell them apart reports a server error as a design fact.
	ECrowdyModelLoadState GetFamilyLoadState(ECrowdyModelFamily Family) const;
	// One model's attributes, which are read per model rather than per app and so keep their own machinery. Answers
	// cache first, then a recorded failure, then a read in flight: a successful re-read must beat a stale failure.
	ECrowdyModelLoadState GetAttributeLoadState(const FString& TypeName) const;

	// Whether each of the three app-wide game-model lists has actually been READ for the selected app. An empty list
	// means "this app has none" only when the matching answer is true; otherwise it means nobody asked or the read
	// failed, and those are different answers a delete pre-flight depends on. True when a read LANDS (or when a plan
	// publishes what it read), never when one is issued, and false again with the lists on an app switch, so a read
	// that failed leaves the honest answer behind rather than an empty list that looks authoritative.
	//
	// Deliberately not derived from whether a plan was captured: the captured plan survives an app switch on purpose
	// while these lists do not, so a plan taken before a switch would vouch for lists that were emptied after it.
	bool HasReadContainerTypes() const { return GetFamilyLoadState(ECrowdyModelFamily::Models) == ECrowdyModelLoadState::Loaded; }
	bool HasReadFunctions() const { return GetFamilyLoadState(ECrowdyModelFamily::Functions) == ECrowdyModelLoadState::Loaded; }
	bool HasReadAutomations() const { return GetFamilyLoadState(ECrowdyModelFamily::Automations) == ECrowdyModelLoadState::Loaded; }

	const TArray<TSharedPtr<FStudioAppFeature>>& GetFeatures() const { return Features; }
	const TArray<TSharedPtr<FStudioTierFeature>>& GetTierFeatures() const { return TierFeatures; }
	const TArray<TSharedPtr<FStudioAccessTier>>& GetAccessTiers() const { return AccessTiers; }
	const FStudioGameModelPolicy& GetGameModelPolicy() const { return GameModelPolicy; }
	const TArray<TSharedPtr<FStudioContainer>>& GetContainers() const { return Containers; }
	// True when the last paged container read came back full, which is the only evidence that another page
	// exists: the query reports no count of any kind, so this is a "there may be more", never a total. False
	// after an unpaged read, which already returned everything.
	bool ContainersMayHaveMore() const { return bContainersMayHaveMore; }
	const FStudioContainerState& GetContainerState() const { return ContainerState; }
	const FString& GetSelectedContainerId() const { return SelectedContainerId; }

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
	// Fired whenever any container type's attributes land in the per-type cache, including the replies the flat mirror
	// above deliberately ignores because another view claimed it. A view reading the cache by type has to hear about
	// those too, or it waits forever on a "loading" state for attributes that are already in hand.
	FSimpleMulticastDelegate OnPropertyDefsCached;
	FSimpleMulticastDelegate OnFunctionsChanged;
	// Fired once both the automations and their triggers have settled, so a view never paints automations with the
	// trigger column still empty and then repaints it a moment later.
	FSimpleMulticastDelegate OnAutomationsChanged;
	FSimpleMulticastDelegate OnFeaturesChanged;
	FSimpleMulticastDelegate OnTierFeaturesChanged;
	FSimpleMulticastDelegate OnAccessTiersChanged;
	FSimpleMulticastDelegate OnGameModelPolicyChanged;
	FSimpleMulticastDelegate OnGameModelLintChanged;
	FSimpleMulticastDelegate OnContainersChanged;
	FSimpleMulticastDelegate OnContainerStateChanged;
	// Fired when a schema-sync plan or apply completes, so the game-model view rebuilds its report panel.
	FSimpleMulticastDelegate OnSchemaSyncReportChanged;
	// Fired when a plan starts, moves to another phase, or ends. Separate from the report delegate above because it
	// fires several times during one plan and carries no result: a view binds it to update a busy indicator, and
	// updating on this signal is what keeps that indicator off the per-paint path.
	FSimpleMulticastDelegate OnSchemaPlanProgress;
	// Fired when the snapshot GetModelSnapshot returns changes: a plan finished for the selected app, or the
	// selection moved to an app with a different one (or with none). Distinct from the report delegate above, which
	// also fires when a plan is merely cleared at the start of the next one.
	FSimpleMulticastDelegate OnModelSnapshotChanged;
	// Fired when a delete commit starts and after each of its ops settles, so a review can show how far it has got.
	// Carries no result: a view binds it to move a counter, and updating on this signal is what keeps that counter
	// off the per-paint path.
	FSimpleMulticastDelegate OnDeleteCommitProgress;
	// Fired ONCE when a commit ends, whether it finished or stopped. GetLastDeleteOutcome says which, and
	// GetDeleteRemainder says what is left. Every path that ends a walk fires this, including a refusal before any
	// op was issued: a review continues from this signal, so one that does not arrive leaves the page saying a
	// delete is under way forever.
	FSimpleMulticastDelegate OnDeleteCommitFinished;
	// The same pair for a live-model purge. Progress fires after each delete settles; finished fires ONCE from every
	// path that ends one, refusals included, EXCEPT the refusal that a purge is already running: announcing there
	// would tell the tab that the running purge had ended.
	FSimpleMulticastDelegate OnContainerPurgeProgress;
	FSimpleMulticastDelegate OnContainerPurgeFinished;
	FOnStudioStatusMessage OnStatusMessage;

private:
	// Schema-sync internals: the async read fan-out/join (ReadDesiredTypeProps + ReadCurrentFunctions, joined by
	// TryFinishSchemaPlan -> FinishSchemaPlan) and the sequential apply walk (RunSchemaUpserts). FCrowdySchemaSyncRun
	// holds one plan run's in-flight state.
	// The plan body.
	void PlanSchemaSyncInternal();
	// The plan proper, run once the project's container assets are resident: reflect the desired schema, then probe
	// and stream the effect assets. Split from the body above so those container assets can be STREAMED rather than
	// force-loaded behind a modal, which is what used to hold the game thread for the whole of the first press.
	// A completion that lands after the app selection changed is dropped before reaching here.
	void ContinueSchemaPlanAfterContainerLoad();
	// The rest of the plan, run once every effect asset the plan needs is resident: the effect gathers, the SDK's own
	// collection plumbing, and the server reads. Split from the body above so the assets can be STREAMED in rather
	// than force-loaded, which is what keeps the editor interactive on a project with many effects. A completion that
	// lands after the app selection changed is dropped, exactly as FinishSchemaPlan drops a superseded plan.
	void ContinueSchemaPlanAfterEffectStream(const TSharedRef<FCrowdySchemaSyncRun>& Run);
	void ReadDesiredTypeProps(const TSharedRef<FCrowdySchemaSyncRun>& Run);
	// Reads every server function for the pinned app into the run (one gameModelFunctions query, no type filter).
	void ReadCurrentFunctions(const TSharedRef<FCrowdySchemaSyncRun>& Run);
	// Reads every server automation for the pinned app, then (chained, since triggers reference their automation by id)
	// every automation event trigger, resolving each trigger's automation name from the automations just read.
	void ReadCurrentAutomations(const TSharedRef<FCrowdySchemaSyncRun>& Run);
	// Reads whether the app's default session channel (__crowdy_session_<appId>) exists, since an effect's channel
	// model-changed notification names it and a name the app does not have reaches nobody. Not found -> id stays 0,
	// which is the only thing the id is read for; Apply auto-creates the channel.
	void ReadSessionChannel(const TSharedRef<FCrowdySchemaSyncRun>& Run);
	// Calls FinishSchemaPlan once ALL read fans (type/prop, function, session channel) have completed for the run.
	void TryFinishSchemaPlan(const TSharedRef<FCrowdySchemaSyncRun>& Run);
	void FinishSchemaPlan(const TSharedRef<FCrowdySchemaSyncRun>& Run);
	// Publish the server schema a finished plan already read into the app-scoped lists the page renders from. These
	// are the same three reads the page's own Refresh issues, so this costs no round trip and leaves the browser
	// showing the exact schema the plan's verdict was computed against.
	void IngestPlanServerReads(const TSharedRef<FCrowdySchemaSyncRun>& Run);
	// A plan read failed (a SendGame OnFailure fired). Marks the run failed once, drops the half-built plan, and
	// leaves a "re-plan" banner in the report panel (the ephemeral status line already carries the server error).
	// A stale run (its app was switched away) is dropped silently. Context names the read for the banner.
	void FailSchemaPlan(const TSharedRef<FCrowdySchemaSyncRun>& Run, const FString& Context);
	// Sequential apply walk over a per-run op list ((query, variables) pairs). Each walk owns its ops, so a
	// stall just stops that walk (the pending upserts stay, so re-clicking Apply starts a fresh independent
	// walk) and there is no shared guard to latch. Types precede their props in the list.
	//
	// Sent is the closed set the ops were built from, one entry per op and in the same order. A walk may carry a
	// SUBSET of the plan, so the completion has to remove exactly what it wrote rather than everything that was
	// pending: clearing the lot would erase the deferred remainder and report an app with real drift as in sync.
	//
	// PlanGeneration is the plan the ops were built from. Nothing stops a fresh plan being started while a walk is in
	// flight, and the entries below are addressed by INDEX into arrays a plan rebuilds from a new diff, so a walk that
	// outlived its plan must not touch them. Compared at the completion; see SchemaPlanGeneration.
	void RunSchemaUpserts(const TSharedRef<TArray<TPair<FString, TSharedPtr<FJsonObject>>>>& Ops,
		const TSharedRef<const TArray<FCrowdyApplyUnit>>& Sent, int32 Index, uint64 PlanGeneration);
	// Take the applied entries out of the five pending arrays, addressed by kind and plan index. Removal runs from
	// the HIGHEST index down within each array, because removing a lower entry first shifts every entry after it and
	// the indices the walk was built from would then name the wrong upserts.
	void RemoveAppliedUpserts(const TArray<FCrowdyApplyUnit>& Sent);
	// Restate the report's per-kind counts and per-entity lines from the five pending arrays as they stand now. A
	// partial send shortens those arrays, and counts left describing the whole plan sit beside a banner saying how
	// many were not sent, so the panel states two contradicting numbers about the same app.
	void RestateSchemaReportFromPending();
	// Whether THIS closed set carries a function the project authors an SDK channel model-changed notification on.
	//
	// Answered from the keys the plan recorded off the DESIRED functions, never off the pending upserts. With no
	// session channel to resolve, the diff cannot author that notification at all: it strips it from every upsert and
	// substitutes the server's own, so an answer read back from the upserts is false for every possible selection -
	// exactly when the channel is missing, which is the only state the auto-create exists for.
	bool ClosedSetNeedsSessionChannel(const TArray<FCrowdyApplyUnit>& ClosedSet) const;
	// The prune's sequential delete walk (server-only property defs, then container types). Separate from the
	// upsert walk so its completion clears the prune list + reports "pruned" without touching the upsert plan.
	void RunSchemaPrune(const TSharedRef<TArray<TPair<FString, TSharedPtr<FJsonObject>>>>& Ops, int32 Index);
	// Drop any planned/in-flight sync state. Called on app switch so app A's plan can never apply to app B.
	void ClearSchemaSyncState();

	// The delete commit's sequential walk. Owns its own copy of the ops, so a walk in flight cannot be changed by
	// anything the review does to its plan afterwards, and stopping one stops only that one.
	void RunDeleteWalk(const TSharedRef<TArray<FCrowdyDeleteOp>>& Ops, int32 Index);
	// What one operation's reply does to the walk: read it, count it, and either move to the next operation or end
	// the walk on it. Named rather than left inline in the send's lambda so the step that ADVANCES the walk can be
	// exercised without a transport; a suite that can only ever observe the first operation cannot tell "stops at
	// the first failure" from "never advances at all".
	void HandleDeleteOpReply(const TSharedRef<TArray<FCrowdyDeleteOp>>& Ops, int32 Index,
		const TSharedPtr<FJsonObject>& Envelope);
	void HandleDeleteOpFailure(const TSharedRef<TArray<FCrowdyDeleteOp>>& Ops, int32 Index, bool bWasCanceled);
	// End the walk exactly once: record the outcome and the remainder, refresh the lists the deletes changed, and
	// announce. StoppedAtIndex is INDEX_NONE for a walk that finished. Called from every path that can end one,
	// including a refusal before any op was issued, so the finished signal is never the one that fails to arrive.
	void FinishDeleteWalk(int32 StoppedAtIndex, bool bWasCanceled);
	// Re-read what a commit changed: the schema lists the browser renders, and the retained plan verdict, which is a
	// judgement about a server that has just moved. A verdict computed before a write is stale the moment the write
	// lands, and showing it anyway is a confident answer about state that no longer exists.
	void RefreshAfterDeleteCommit();

	// One model's bounded, side-effect-free live-model probe for a pre-flight. Its reply goes into the pending
	// results below and nowhere else.
	void ReadLiveModelCountFor(const FString& TypeName, int32 PerTypeLimit);
	// Run the pre-flight's completion exactly once and clear its state, whatever caused the end: every read landed,
	// a read failed, or the app changed under it. Models with no result are reported Unknown rather than zero, which
	// is the whole reason the state exists.
	void FinishLiveModelCount();

	// Empty every list, policy and selection that belongs to one app, and tell the views. Called on app switch so the
	// previous app's teams, channels and game-model schema can never stay on screen under the new app's name. Purely
	// local: it issues no request, so the caller stays in charge of what is re-fetched and when.
	void ClearAppScopedState();

	// Claim the newest read of one family and mark it as loading, pinned to the app it is issued for. Its reply
	// carries the (app, serial) pair back to AcceptFamilyReply, which is the only thing that may act on it.
	uint64 BeginFamilyRead(ECrowdyModelFamily Family);

	// Whether one family reply may still be acted on, recording the state it landed in when it may. All three terms
	// of the check are needed: an app the reply was issued for, the request it was issued as, and the app on screen
	// now. The app alone cannot answer it, because a user who switches away and back selects the SAME app, so the
	// ids compare equal again while the list that reply was issued against was emptied in between. Every family's
	// success AND failure handler goes through this before it touches anything, so the race has one answer.
	bool AcceptFamilyReply(ECrowdyModelFamily Family, int64 ReplyAppId, uint64 ReplySerial,
		ECrowdyModelLoadState Landed);

	// Record a family as read from something that is not one of its own reads: a finished plan publishes the same
	// server lists it just diffed against. It takes a fresh serial, so a read still in flight when the plan lands is
	// superseded rather than allowed to overwrite the newer picture the plan just published.
	void MarkFamilyLoadedFromPlan(ECrowdyModelFamily Family, int64 AppId);

	// Take a landed models read: drop it unless it is still the newest issued for the app on screen, then replace the
	// list and record it. Split out of the fetch so a reply can be landed with no transport.
	void IngestContainerTypes(TArray<TSharedPtr<FStudioContainerType>>&& Types, int64 RequestAppId,
		uint64 RequestSerial);

	// Take a landed functions read. The two lists it can fill are guarded separately, because they answer different
	// questions: the app-wide list may only be replaced by a whole-app read that is still the newest of those, and the
	// mirror only by the newest read of any kind, filtered or not. Nothing is written before both guards have had
	// their say, or a reply this controller decides against has already been applied by the time it is rejected.
	// Split out of the fetch so a reply can be landed with no transport.
	void IngestFunctions(TArray<TSharedPtr<FStudioFunction>>&& Parsed, bool bUnfiltered, int64 RequestAppId,
		uint64 RequestSerial, uint64 ReadSerial);

	// Store one container type's attributes in the per-type cache and mirror them into the flat list the single-type
	// views read. Split out of the fetch so the parse-and-store step has no request in it.
	void IngestPropertyDefs(const FString& TypeName, TArray<TSharedPtr<FStudioPropertyDef>>&& Defs);

	// Whether an attribute reply should still be acted on: its app must still be selected and its serial must still be
	// the newest issued for its type. Split out of the fetch so the rule can be exercised without a live reply.
	bool IsLatestPropertyDefRequest(const FString& TypeName, int64 RequestAppId, uint64 RequestSerial) const;

	// Add a freshly read page of containers to the ones already held, skipping any container already present.
	// Split out of the fetch so the de-duplication can be exercised without a live reply.
	void AppendContainers(const TArray<TSharedPtr<FStudioContainer>>& Page);

	// Whether a landed container page is allowed to answer "may another page exist".
	enum class EContainerPageEvidence : uint8
	{
		// A page read at the caller's page size. A full page is evidence of another; a short one is the end.
		Update,
		// A re-read of the whole window already on screen, issued after a delete. It comes back one container
		// short because one was deleted, which says nothing about whether the list ends there, so the answer the
		// last real page gave stands.
		Keep
	};

	// A page that came back full is the edge of what was asked for, so something may lie beyond it. An unpaged
	// read asked for no page at all and already returned everything.
	static bool PageMayHaveMore(int32 Limit, int32 PageSize) { return Limit > 0 && PageSize >= Limit; }

	// Fold a landed page into the held list and into the paging evidence. Split out of the read so both rules can
	// be exercised without a live reply.
	void IngestContainerPage(TArray<TSharedPtr<FStudioContainer>>&& Page, int32 Limit, bool bAppend,
		EContainerPageEvidence Evidence);

	// Issue one container read. FetchContainers is this with the paging evidence updated; the delete handler's
	// re-list is this with the evidence kept.
	void ReadContainers(const FString& TypeNameFilter, const FString& SessionIdFilter,
		int32 Limit, int32 Offset, bool bAppend, EContainerPageEvidence Evidence);

	// Claim the newest function read, whatever it was filtered to. Only the newest reply may repaint the mirror,
	// print its count or announce.
	uint64 BeginFunctionRead() { return ++NextFunctionReadSerial; }
	bool IsLatestFunctionRead(uint64 ReadSerial) const { return ReadSerial == NextFunctionReadSerial; }

	// Claim the newest container read. Only the newest reply is acted on.
	uint64 BeginContainerRead() { return ++NextContainerReadSerial; }
	// Whether a container reply should still be acted on. The app-id check cannot answer this on its own: a user
	// who switches away and back selects the same app again, so the ids compare equal while the list that reply
	// was issued against has been emptied in between. Split out of the read so the rule can be exercised without
	// a live reply.
	bool IsLatestContainerRead(uint64 ReadSerial) const { return ReadSerial == NextContainerReadSerial; }

	// The recognized Game Kit container-type prefixes for the selected app: the effective prefixes of the kit config
	// authored in the Deploy Kit card this session, unioned with any persisted from a prior deploy. A schema sync feeds
	// these into the diff so kit-deployed types/props/functions are never flagged as server-only prune candidates.
	TSet<FString> GatherRecognizedKitTypePrefixes() const;
	// Load/store the deployed kit type prefixes per app id in the per-project editor ini, so prune-protection survives
	// an editor restart even though the authored kit config is session-only. Store unions with the existing set.
	TSet<FString> LoadPersistedKitPrefixes(int64 AppId) const;
	void PersistDeployedKitPrefixes(int64 AppId, const TSet<FString>& Prefixes);
	// Load/store the EXACT deployed kit container-type and function names per app id, alongside the prefixes. These
	// protect an empty-prefix kit (Combat/Leaderboards/LivingWorld defaulting to no prefix), whose bare type/function
	// names no prefix rule can recognize, from a "Remove Server-Only" prune. Store unions with the existing set so
	// re-deploying one kit never drops another kit's names.
	TSet<FString> LoadPersistedKitTypeNames(int64 AppId) const;
	TSet<FString> LoadPersistedKitFunctionNames(int64 AppId) const;
	void PersistDeployedKitNames(int64 AppId, const TSet<FString>& TypeNames, const TSet<FString>& FunctionNames);
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

	// Right after sign-in, pull the app list so the console opens populated instead of empty.
	void FetchAppsForSignedInUser();

	/** Drop one app fetch from the in-flight count. Called on both the success and the failure path. */
	void ReleaseAppsFetch();

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
	// that needs to tell the two apart (RunSchemaUpserts, RunSchemaPrune) can read it from inside that same
	// OnFailure call. Only meaningful for the duration of that call; do not read it any later.
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
	// Attributes keyed by container type name: the authoritative store, so reading one type's attributes cannot
	// discard another's. A key present with an empty array means that type genuinely has none.
	TMap<FString, TArray<TSharedPtr<FStudioPropertyDef>>> PropertyDefsByType;
	// The type names with an attribute read in flight, so re-selecting a type cannot stack duplicate queries. A key is
	// removed when the read lands, succeeds or fails, otherwise that type could never be loaded again.
	TSet<FString> PropertyDefFetchesInFlight;
	// The serial of the most recently ISSUED read per type, and the counter it is drawn from. A reply whose serial is
	// no longer the latest for its type is discarded: it was superseded by a read issued after it, so it can only
	// carry an older picture. This is what makes a forced post-write re-read win over a read still in flight from
	// before the write, rather than the two racing.
	TMap<FString, uint64> PropertyDefLatestRequest;
	uint64 NextPropertyDefRequestSerial = 0;
	TArray<TSharedPtr<FStudioPropertyDef>> PropertyDefs;
	// The type the flat mirror is meant to be showing: the type most recently asked for. A reply for any other type
	// still fills the per-type cache but must not repaint the mirror, or the page ends up naming one type above
	// another type's attributes.
	FString PropertyDefsMirrorType;
	// The functions of the most recently requested filter. A view that lists one container type at a time narrows this
	// to that type, so it is a mirror of the last request rather than a picture of the app.
	TArray<TSharedPtr<FStudioFunction>> Functions;
	// Every function of the selected app, whatever the mirror above was last narrowed to. A view showing several types
	// at once has to read this: sharing the mirror would empty every other type's list the moment some other view
	// asked for one type's functions, and an emptied list is indistinguishable from a failed read.
	TArray<TSharedPtr<FStudioFunction>> UnfilteredFunctions;
	// Numbers EVERY function read, narrowed to one container type or not, so a reply that is no longer the newest
	// cannot repaint the mirror above. The family serial covers only the whole-app read, and a narrowed read claims
	// no family at all: without a number of its own it would land after a wider read and leave the mirror holding a
	// subset of the app under a heading that says otherwise. Moved on by an app switch as well, since switching away
	// and back selects the same app and the ids compare equal again.
	uint64 NextFunctionReadSerial = 0;
	// The app's automations and their event triggers, as browsed. These are read independently of a schema-sync plan,
	// which keeps its own snapshot, so the browse lists are populated even when no plan has ever run.
	TArray<TSharedPtr<FStudioAutomation>> Automations;
	TArray<TSharedPtr<FStudioAutomationTrigger>> AutomationTriggers;
	// The app the three lists above were loaded for, so the Game Model page can load itself once per app rather than
	// re-issuing the reads on every frame it is painted. Zero means nothing has been loaded for any app yet, which is
	// also the state an app switch returns it to.
	int64 GameModelListsAppId = 0;
	// What each family's read is doing, as opposed to a list being empty because nobody asked or because the read
	// failed. A slot is moved to Loading when a read is ISSUED and to Loaded or Failed when one LANDS, since the
	// whole point is to tell an app with no models from an app whose model read never came back. Reset with the
	// lists themselves on an app switch. Attributes has a slot for completeness but is answered per model by
	// GetAttributeLoadState, which reads the per-type machinery below.
	FCrowdyFamilyLoad FamilyLoads[static_cast<int32>(ECrowdyModelFamily::Count)];
	// Numbers the family reads so a reply that is no longer the newest can be dropped, including across an app
	// switched away from and back to, where the app ids compare equal again.
	uint64 NextFamilyReadSerial = 0;
	// The models whose attribute read came back an error. A TArray searched case-sensitively rather than a TSet:
	// TSet<FString> hashes case-insensitively and would fold two models whose names differ only in case into one.
	TArray<FString> PropertyDefFailedTypes;
	TArray<TSharedPtr<FStudioAppFeature>> Features;
	TArray<TSharedPtr<FStudioTierFeature>> TierFeatures;
	TArray<TSharedPtr<FStudioAccessTier>> AccessTiers;
	FStudioGameModelPolicy GameModelPolicy;
	FStudioLintReport GameModelLint;
	TArray<TSharedPtr<FStudioContainer>> Containers;
	FStudioContainerState ContainerState;
	FString SelectedContainerId;
	// The filters and page the last FetchContainers call used, so DeleteContainer's re-list reproduces what is on
	// screen instead of silently widening to every container for the app or narrowing to the last page loaded.
	FString LastContainerTypeFilter;
	FString LastContainerSessionFilter;
	// The limit and offset that read asked for (a limit of 0 means it asked for no page and read everything).
	int32 LastContainerLimit = 0;
	int32 LastContainerOffset = 0;
	// How many containers that read actually returned, before any already-held ones were skipped. Compared
	// against the limit to tell a full page (there may be more) from a short one (that was the end).
	int32 LastContainerPageSize = 0;
	// The answer that comparison last gave. Held rather than recomputed on demand because the delete handler's
	// re-list reads a window rather than a page, and its short result would otherwise read as the end of the list
	// and retire "load more" for good.
	bool bContainersMayHaveMore = false;
	// Numbers the container reads so a reply that is no longer the newest can be dropped. Moved on by an app
	// switch as well, so a read issued before one cannot fill the list the switch emptied.
	uint64 NextContainerReadSerial = 0;

	// Schema sync state. SchemaSyncReport is the last plan/apply summary the view renders; the Pending*
	// upserts are the deltas a plan computed, consumed by ApplySchemaSync (which builds a per-walk op list).
	FCrowdySchemaSyncReport SchemaSyncReport;
	TArray<FCrowdySchemaTypeUpsert> PendingSyncTypeUpserts;
	TArray<FCrowdySchemaPropUpsert> PendingSyncPropUpserts;
	TArray<FCrowdySchemaFunctionUpsert> PendingSyncFunctionUpserts;
	// Automation + trigger upserts a plan computed. Applied AFTER the function upserts (an automation references a
	// function; a trigger references an automation).
	TArray<FCrowdySchemaAutomationUpsert> PendingSyncAutomationUpserts;
	TArray<FCrowdySchemaTriggerUpsert> PendingSyncTriggerUpserts;
	// Server-only entities the last plan found: the opt-in prune deletes these (never the sync itself).
	TArray<FString> PendingPruneTypes;
	TArray<FCrowdySchemaPropRef> PendingPruneProps;
	// Scoped rather than bare: a function candidate carries the container type the diff identified it by, so the
	// status line, the refresh and anything that later offers the candidate for deletion name the right model
	// instead of guessing which one a bare name came from.
	TArray<FCrowdySchemaFunctionRef> PendingPruneFunctions;
	TArray<FString> PendingPruneAutomations;
	// What the next apply sends, by identity key, and whether anybody narrowed it at all. Held on the controller
	// rather than passed as an argument because creating the app's session channel re-enters ApplySchemaSync from
	// that create's own callback with no argument, so a selection passed down a parameter would be lost exactly when
	// it matters. bSchemaApplySelectionIsAll means everything the plan holds, which is what keeps an apply nobody
	// narrowed byte-identical to the all-or-nothing one.
	TArray<FString> SchemaApplySelection;
	bool bSchemaApplySelectionIsAll = true;
	// The app id the pending plan was computed against; Apply/Prune refuse if the selection changed since.
	int64 PlannedSyncAppId = 0;
	// Which plan the five arrays above hold, moved on every time they are replaced or dropped. An apply walk captures
	// it and compares at its completion: the walk addresses those arrays by index, and nothing latches out a
	// concurrent re-plan, so without this a reply landing after a re-plan would strip entries of the NEW plan at the
	// OLD plan's indices - entries that were never sent, and with the count possibly reaching zero and reporting an
	// app as in sync over a model that was never created. An app switch drops the plan, so it moves this too.
	uint64 SchemaPlanGeneration = 0;
	// True when the last plan depends on the app's session channel (an effect declares a channel notification) but
	// that channel does not exist yet. Apply auto-creates the channel once, then continues.
	bool bPlannedSyncNeedsSessionChannel = false;
	// The scoped (container type, name) keys of the DESIRED functions that carry an SDK channel model-changed
	// notification, recorded by the plan that computed the pending arrays. Held as the desired schema states them
	// because a function's upserts carry no trace of which channel they notify, and a partial apply still has to
	// answer whether THIS selection needs the channel provisioned. A TArray compared
	// case-sensitively rather than a TSet, since TSet<FString> hashes case-insensitively and two server keys that
	// differ only in case are two functions.
	TArray<FString> PlannedSessionChannelFunctionKeys;
	// The single-shot guard on that auto-create: true from the moment Apply kicks the channel create until the
	// resumed apply terminates, so the create's own callback can re-enter Apply without creating a second channel.
	// NOT reset by ClearSchemaSyncState: a plan landing mid-flight would otherwise reopen the create.
	bool bAutoCreatingSessionChannel = false;
	// Setup-strip readiness for the app's session channel: Unknown until a plan resolves it, then Ready/NotReady.
	ECrowdyStudioReadiness SessionChannelReadiness = ECrowdyStudioReadiness::Unknown;

	// The delete commit in flight: the app it is pinned to (0 when none), how far it has got, and how many ops it
	// set out to run. The app id is what makes the busy signal answer for the app on screen rather than for any
	// walk at all, so a commit whose app was switched away stops reporting itself under the new one.
	int64 DeleteCommitAppId = 0;
	int32 DeleteCommitCompleted = 0;
	int32 DeleteCommitTotal = 0;
	// How many of the completed ops replied that there was nothing there. Carried through to the outcome so the
	// closing line can say "16 deleted, 2 were already gone" rather than claiming eighteen deletions.
	int32 DeleteCommitAlreadyGone = 0;
	// The last commit's result and the ops it did not run, held past the walk so a review that reopens still has
	// them. The remainder starts with the op that failed, since that one did not complete.
	FCrowdyDeleteOutcome LastDeleteOutcome;
	TArray<FCrowdyDeleteOp> DeleteRemainder;

	// How many live models one purge read pulls per page. A page is drained and then re-read, never advanced.
	static constexpr int32 ContainerPurgePageSize = 200;
	// The most pages one purge will drain before stopping and saying so. The drain has no total to work against,
	// so this is its only bound that does not depend on the server running out of live models.
	static constexpr int32 ContainerPurgeMaxPasses = 200;

	void ReadContainerPurgePage();
	void RunContainerPurgeWalk(int32 Index);
	void HandleContainerPurgeReply(int32 Index, FString ContainerId, const TSharedPtr<FJsonObject>& Envelope);
	// Ends a purge exactly once, clearing its state before it announces so a listener may start another.
	void FinishContainerPurge(bool bStopped, bool bByCancel, const FString& StoppedOn);

	// The purge in flight: the app it is pinned to (0 when none), the model it is scoped to (empty for the whole
	// app), and the page it is working through. The serial numbers the purges, so a reply issued by one cannot be
	// counted into the next after an app switch restored equality.
	int64 ContainerPurgeAppId = 0;
	uint64 ContainerPurgeSerial = 0;
	FString ContainerPurgeTypeName;
	TArray<FString> ContainerPurgePageIds;
	int32 ContainerPurgeCompleted = 0;
	int32 ContainerPurgeAlreadyGone = 0;
	// Live models this page actually REMOVED. A page that removed none would be read back identically forever, so
	// it ends the drain; counting the already-gone replies here would keep that loop running.
	int32 ContainerPurgePageRemoved = 0;
	int32 ContainerPurgePasses = 0;
	bool bContainerPurgeCancelRequested = false;
	FCrowdyDeleteOutcome LastContainerPurgeOutcome;
	// The ops the running walk owns, held here rather than only captured by the walk because ending one has to
	// state the remainder and name what it stopped on. It doubles as the walk's identity: a reply that arrives
	// after its walk ended (the app was switched away and back, so the transport stopped dropping replies) finds
	// a different pointer here and is dropped instead of counted into whatever is running now.
	TSharedPtr<TArray<FCrowdyDeleteOp>> DeleteWalkOps;

	// The delete pre-flight's scoped live-model reads. The app they were issued for, the models whose replies are
	// still outstanding, what has landed so far, and the completion that must run exactly once. The completion is
	// held here rather than captured per read because every read's reply is dropped when the app changes, so
	// nothing would ever decrement the outstanding list and the caller would wait forever on a completion that no
	// reply is left to trigger; the app-switch path finishes it from here instead.
	int64 LiveCountAppId = 0;
	// Numbers the probes, so a read issued by one cannot be counted into the next. The app id alone cannot tell them
	// apart: a user who switches away and back selects the same app again, and a read the switch caused the transport
	// to drop is delivered after all once the ids match again. Its count would then be an arbitrarily old one, and an
	// old count that is lower than the truth is the one answer that makes a refused delete look safe.
	uint64 LiveCountSerial = 0;
	TArray<FString> LiveCountPending;
	TArray<FCrowdyDeleteLiveCount> LiveCountResults;
	TFunction<void(int64 /*AppId*/, TArray<FCrowdyDeleteLiveCount>&& /*Counts*/)> LiveCountCompletion;

	// The last finished plan per app, keyed by app id so one app's verdict can never be read under another's name.
	// Held by shared pointer to const: a view may keep reading the one it was handed while a fresh plan replaces the
	// entry here, and neither can then see the other half-built. Dropped on sign-out, since the next identity may
	// have no business seeing this one's schema.
	TMap<int64, TSharedPtr<const FCrowdyModelSnapshot>> ModelSnapshotsByApp;

	// The in-flight stream of the effect assets a plan needs to compile. Held so it can be cancelled if this
	// controller goes away first: its completion runs a lambda that touches the controller.
	TSharedPtr<FStreamableHandle> SchemaEffectStreamHandle;
	// Set from the moment a plan asks for that stream until its completion runs. A second Plan click for the SAME app
	// while it is set is refused rather than stacking a second stream over the same assets.
	bool bSchemaEffectStreamInFlight = false;
	// The app the in-flight stream's plan is pinned to. A stream started for another app is already superseded (its
	// completion is dropped), so refusing a plan on its account would promise the user a plan that never arrives.
	int64 SchemaEffectStreamAppId = 0;

	// The same pair for the container-asset stream that opens a plan, before any effect is looked at. No handle is
	// held: that stream is owned by the editor baker, which keeps its own one-at-a-time latch, and its completion
	// checks the pinned app before touching anything here.
	bool bSchemaContainerLoadInFlight = false;
	int64 SchemaContainerLoadAppId = 0;

	// The asynchronous wait for the asset registry's initial scan that opens a plan on a freshly launched editor.
	// The scan blocks everything a plan reads from the registry (the effect probe, the duplicate-name index), so the
	// plan subscribes to OnFilesLoaded and continues from there instead of stalling the game thread inside
	// WaitForCompletion. One pending wait at most: a plan for another app re-pins it, a plan for the same app is
	// refused like the stream latches above.
	bool bSchemaPlanWaitingOnRegistry = false;
	int64 SchemaRegistryWaitAppId = 0;
	FDelegateHandle SchemaRegistryFilesLoadedHandle;
	// Abandon a pending registry wait: unbind the delegate, drop the latch, and close the waiting phase. Called on
	// sign-out, because this wait is the one plan
	// continuation whose lifetime is open-ended (minutes on a cold editor): an app-id guard alone would let a plan
	// latched under one identity fire under the next one to sign in and select the same app id.
	void CancelSchemaRegistryWait();

	// The app a plan is currently running for, 0 when none. Covers the WHOLE plan, including the server-read phase
	// that the two stream latches above say nothing about, so a view can report activity for every part of it.
	int64 SchemaPlanBusyAppId = 0;
	FString SchemaPlanPhase;
	// Enter a plan phase, or leave the plan entirely when Phase is empty. Announces on OnSchemaPlanProgress; every
	// path that abandons a plan has to call it with an empty phase, or the page reports work that has stopped.
	void SetSchemaPlanPhase(int64 AppId, const FString& Phase);

	int64 SelectedOrgId = 0;
	int64 SelectedAppId = 0;

	// The transient, Studio-owned Game Kit config edited inline in the Deploy Kit card (never a content asset).
	// Lazily created + rooted for the editor session by GetKitDeployConfig.
	TStrongObjectPtr<UCrowdyGameKitConfig> KitDeployConfig;

	int32 InFlightCount = 0;
	FString StatusMessage;
	bool bStatusWasError = false;

#if WITH_DEV_AUTOMATION_TESTS
	// The app-scoped reset and the attribute cache are internal because no view may drive them directly, which also
	// leaves the automation tests no other way to exercise them.
	friend struct FCrowdyStudioControllerTestAccess;
#endif
};
