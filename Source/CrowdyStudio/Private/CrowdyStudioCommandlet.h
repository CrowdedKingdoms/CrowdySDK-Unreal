#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "Model/CrowdyStudioTypes.h"
#include "CrowdyStudioCommandlet.generated.h"

/** One parsed command line of the Crowdy Studio commandlet. */
struct FCrowdyStudioCommandRequest
{
	/** The operation, lower case: apps, createapp, sync, or one of the app-scoped operations in GetUsage. */
	FString Op;
	int64 OrgId = 0;
	/** 0 when an app-scoped operation was not given -app, which means the project's synced AppID. */
	int64 AppId = 0;
	FString Name;
	FString Slug;
	FString Datacenter;
	FString Description;
	/** -key: a feature key. */
	FString Key;
	int64 TierId = 0;
	/** -policy: a team or channel membership policy; empty when not given. */
	FString Policy;
	bool bMembersCanSend = true;
	int64 GridId = 0;
	/** -user: the grid scan's user or the user a grid grant targets; 0 when not given. */
	int64 UserId = 0;
	int64 GroupId = 0;
	int64 RoleId = 0;
	/** -low and -high for grids, -corner1 and -corner2 for creategrid. */
	FStudioChunk Low;
	FStudioChunk High;
	/** -keys: grid permission keys. */
	TArray<FString> Keys;
	/** -all: revokegrid removes every grant of the target; -nolimit: gridlimits lifts the whitelist. */
	bool bAllKeys = false;
	/** True for a channel operation; set by -kind for the group operations. */
	bool bChannel = false;
	/** -roles: the role ids setroles gives the member; -noroles takes them all away. */
	TArray<int64> RoleIds;
	bool bNoRoles = false;
	/** -perms: the group permissions of a role; -noperms leaves it with none. */
	TArray<FString> Perms;
	bool bNoPerms = false;
	int32 Rank = 0;
	bool bHasRank = false;
	/** -creation: who may create teams or channels. */
	FString Creation;
	/** -maxmembers and -maxgroupsperuser: -1 when not given, 0 for unlimited. */
	int32 MaxMembers = -1;
	int32 MaxGroupsPerUser = -1;
	bool bYes = false;
	/** -editoropen: write the project settings even while an Unreal Editor is running. */
	bool bEditorOpen = false;
};

/**
 * The Project page's app choice from the command line, as the developer signed in to Crowdy Studio on this machine:
 *   UnrealEditor-Cmd <project> -run=CrowdyStudio -op=<operation> [-out=<file>]
 * apps lists the account's organizations and their apps, the datacenters an app can be placed in, and the project's current
 * Network settings. createapp -org=<id> -name=<text> -slug=<text> -datacenter=<code> [-description=<text>] -yes creates an app
 * as the Create app sheet does. sync -app=<id> -yes writes that app into Config/DefaultGame.ini as Sync to project does.
 * features, definefeature, grantfeature and revokefeature are the Access page, teams and createteam the Teams page, channels and
 * createchannel the Channels page, grids, creategrid, grantgrid and revokegrid the Grid page; each runs against -app, or the
 * project's synced AppID when -app is left out, and reports which. groupdetail, addmember, removemember, setroles, createrole,
 * updaterole, deleterole, updategroup and deletegroup are a team's or channel's detail panel (-kind names which), setteampolicy and
 * setchannelpolicy its policy card, and gridlimits, gridgrants and gridpermissions the Grid page's whitelist and grant lists. Every operation that changes something describes what it would
 * do and changes nothing without -yes, and exits 1; one that would change nothing succeeds without it. A confirmed sync is refused
 * while an Unreal Editor is running unless -editoropen is given. -out writes the result as one JSON object. Exit code 0 on
 * success, 1 otherwise.
 */
UCLASS()
class UCrowdyStudioCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UCrowdyStudioCommandlet();
	virtual int32 Main(const FString& Params) override;

	/** The text an unknown operation is answered with: every operation Main accepts, with its switches. */
	static const TCHAR* GetUsage();

	/** Reads the operation and its switches; on a refusal, OutError names the switch. A missing -yes is not refused here, see NeedsConfirmation. */
	static bool ParseRequest(const FString& Params, FCrowdyStudioCommandRequest& OutRequest, FString& OutError);

	/** True for an operation that changes the app list, the project or an app's data and was not given -yes. */
	static bool NeedsConfirmation(const FCrowdyStudioCommandRequest& Request);

	/** The app an app-scoped operation runs against: -app, else ProjectAppId. 0 with OutError set when neither names one. */
	static int64 ResolveAppId(const FCrowdyStudioCommandRequest& Request, int64 ProjectAppId, FString& OutError);

	/** The reason the Access page would not take this tier-feature operation for these lists, or empty when it would. */
	static FString FindAccessProblem(const FCrowdyStudioCommandRequest& Request, const TArray<TSharedPtr<FStudioAppFeature>>& Features,
		const TArray<TSharedPtr<FStudioAccessTier>>& Tiers);

	static bool IsGranted(const TArray<TSharedPtr<FStudioTierFeature>>& Grants, int64 TierId, const FString& Key);

	/** The reason a defined feature is not as asked once the list is read back (missing, or another description), or empty when it is. */
	static FString FindDefineProblem(const FCrowdyStudioCommandRequest& Request, const TArray<TSharedPtr<FStudioAppFeature>>& Features);

	/** The fields of an existing team or channel that differ from what createteam or createchannel asked for, pointing at updategroup, or empty when it matches. A switch left out is not compared. */
	static FString FindGroupDifference(const FCrowdyStudioCommandRequest& Request, const FStudioGroup& Existing);

	/** The fields of an existing role that differ from what createrole asked for, pointing at updaterole, or empty when it matches. A switch left out is not compared. */
	static FString FindRoleDifference(const FCrowdyStudioCommandRequest& Request, const FStudioGroupRole& Existing);

	/** A refusal naming -group when it is neither a team nor a channel of the app, or empty when it is one. */
	static FString FindGridGroupProblem(int64 GroupId, const TArray<TSharedPtr<FStudioGroup>>& Teams, const TArray<TSharedPtr<FStudioGroup>>& Channels);

	/** After a user revoke, a refusal naming the first revoked key the user still holds, or empty. -all names no key, so it finds nothing. */
	static FString FindUnrevokedUserKey(const FCrowdyStudioCommandRequest& Request, const TArray<FString>& EffectiveKeys);

	/** After a group revoke, a refusal for the first grant in the revoke's scope that is still there, or empty. */
	static FString FindUnrevokedGroupGrant(const FCrowdyStudioCommandRequest& Request, const TArray<TSharedPtr<FStudioGridGroupGrant>>& Grants);

	/** A refusal naming -keys for the first of Keys that Catalog does not hold, or empty when all are held or the catalog is empty. */
	static FString FindUnknownPermissionKey(const TArray<FString>& Keys, const TArray<FString>& Catalog);

	static bool IsMembershipPolicy(const FString& Policy);

	/** The reason the Create app sheet would refuse this request for this account, or empty when it would not. */
	static FString FindCreateProblem(const FCrowdyStudioCommandRequest& Request, const TArray<TSharedPtr<FStudioOrg>>& Organizations,
		const TArray<FStudioDatacenter>& Datacenters);

	/** The reason a confirmed sync must not write Config/DefaultGame.ini now, or empty when it may. */
	static FString FindSyncWriteProblem(const FCrowdyStudioCommandRequest& Request, bool bEditorRunning);

	static TSharedPtr<FStudioApp> FindApp(const TArray<TSharedPtr<FStudioApp>>& Apps, int64 AppId);

	/** What Sync to project leaves the settings as for this app, without writing anything. */
	static FStudioSettingsSnapshot ProposeSync(const FStudioApp& App);
};
