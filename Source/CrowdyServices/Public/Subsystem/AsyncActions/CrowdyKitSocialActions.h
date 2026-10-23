#pragma once

#include "Kismet/BlueprintAsyncActionBase.h"
#include "Queries/Data/Teams/Enums/ECrowdyTeamMembershipPolicy.h"
#include "Subsystem/CrowdyKitSocial.h"
#include "CrowdyKitSocialActions.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FCrowdyKitGroupAsyncResult, FCrowdyKitGroup, Group, FCrowdyTeamError, Error, FString, Message);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FCrowdyKitMembersAsyncResult, FCrowdyKitGroup, Group, const TArray<FCrowdyTeamMember>&, Members, FCrowdyTeamError, Error, FString, Message);

/** The shape every Game Kit Social node shares: Completed with the group, Failed with an error and its reason. */
UCLASS(Abstract)
class CROWDYSERVICES_API UCrowdyKitSocialGroupAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FCrowdyKitGroupAsyncResult Completed;

	UPROPERTY(BlueprintAssignable)
	FCrowdyKitGroupAsyncResult Failed;

	virtual void Activate() override;

protected:
	TWeakObjectPtr<UObject> WorldContextObject;

	virtual void Run(UCrowdyKitSocial& Social, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail) PURE_VIRTUAL(UCrowdyKitSocialGroupAction::Run,);

private:
	void HandleDone(const FCrowdyKitGroup& Group);
	void HandleFailed(const FCrowdyTeamError& Error);
};

/** The shape of the two nodes that list a team: Completed with the group and its members. */
UCLASS(Abstract)
class CROWDYSERVICES_API UCrowdyKitSocialMembersAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FCrowdyKitMembersAsyncResult Completed;

	UPROPERTY(BlueprintAssignable)
	FCrowdyKitMembersAsyncResult Failed;

	virtual void Activate() override;

protected:
	TWeakObjectPtr<UObject> WorldContextObject;
	FCrowdyKitGroup Group;

	virtual void Run(UCrowdyKitSocial& Social, FCrowdyKitMembersDone OnDone, FCrowdyKitFailed OnFail) PURE_VIRTUAL(UCrowdyKitSocialMembersAction::Run,);

private:
	void HandleDone(const FCrowdyKitGroup& Answered, const TArray<FCrowdyTeamMember>& Members);
	void HandleFailed(const FCrowdyTeamError& Error);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyKitSocial_CreateParty : public UCrowdyKitSocialGroupAction
{
	GENERATED_BODY()

public:
	/** Creates the party: an invite-only team named party:Name and its chat channel. The caller leads it. Fails when either cannot be created, for example when the name is taken. */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"),
		Category = "Crowdy|Game Kit|Social", DisplayName = "Create Party")
	static UCrowdyKitSocial_CreateParty* CreateParty(UObject* WorldContextObject, const FString& Name);

protected:
	virtual void Run(UCrowdyKitSocial& Social, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail) override;

private:
	FString Name;
};

UCLASS()
class CROWDYSERVICES_API UCrowdyKitSocial_FindParty : public UCrowdyKitSocialGroupAction
{
	GENERATED_BODY()

public:
	/** Finds the party named exactly Name. Fails with Not Found when there is none. Anyone can create a team with the name first, so check Owner User Id is the player you expect. */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"),
		Category = "Crowdy|Game Kit|Social", DisplayName = "Find Party")
	static UCrowdyKitSocial_FindParty* FindParty(UObject* WorldContextObject, const FString& Name);

protected:
	virtual void Run(UCrowdyKitSocial& Social, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail) override;

private:
	FString Name;
};

UCLASS()
class CROWDYSERVICES_API UCrowdyKitSocial_InviteToParty : public UCrowdyKitSocialGroupAction
{
	GENERATED_BODY()

public:
	/** Adds the user to the party's team and chat channel. Needs the manage members permission on the party. */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"),
		Category = "Crowdy|Game Kit|Social", DisplayName = "Invite To Party")
	static UCrowdyKitSocial_InviteToParty* InviteToParty(UObject* WorldContextObject, const FCrowdyKitGroup& Group, int64 UserId);

protected:
	virtual void Run(UCrowdyKitSocial& Social, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail) override;

private:
	FCrowdyKitGroup Group;
	int64 UserId = 0;
};

UCLASS()
class CROWDYSERVICES_API UCrowdyKitSocial_JoinParty : public UCrowdyKitSocialGroupAction
{
	GENERATED_BODY()

public:
	/** Joins the party's team and chat channel as the signed-in player. */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"),
		Category = "Crowdy|Game Kit|Social", DisplayName = "Join Party")
	static UCrowdyKitSocial_JoinParty* JoinParty(UObject* WorldContextObject, const FCrowdyKitGroup& Group);

protected:
	virtual void Run(UCrowdyKitSocial& Social, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail) override;

private:
	FCrowdyKitGroup Group;
};

UCLASS()
class CROWDYSERVICES_API UCrowdyKitSocial_LeaveParty : public UCrowdyKitSocialGroupAction
{
	GENERATED_BODY()

public:
	/** Leaves the party's chat channel, then its team. */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"),
		Category = "Crowdy|Game Kit|Social", DisplayName = "Leave Party")
	static UCrowdyKitSocial_LeaveParty* LeaveParty(UObject* WorldContextObject, const FCrowdyKitGroup& Group);

protected:
	virtual void Run(UCrowdyKitSocial& Social, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail) override;

private:
	FCrowdyKitGroup Group;
};

UCLASS()
class CROWDYSERVICES_API UCrowdyKitSocial_GetPartyMembers : public UCrowdyKitSocialMembersAction
{
	GENERATED_BODY()

public:
	/** Lists the party's members. */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"),
		Category = "Crowdy|Game Kit|Social", DisplayName = "Get Party Members")
	static UCrowdyKitSocial_GetPartyMembers* GetPartyMembers(UObject* WorldContextObject, const FCrowdyKitGroup& Group);

protected:
	virtual void Run(UCrowdyKitSocial& Social, FCrowdyKitMembersDone OnDone, FCrowdyKitFailed OnFail) override;
};

UCLASS()
class CROWDYSERVICES_API UCrowdyKitSocial_CreateGuild : public UCrowdyKitSocialGroupAction
{
	GENERATED_BODY()

public:
	/** Creates the guild: a team named guild:Name under the membership policy, and its chat channel. The caller leads it. */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"),
		Category = "Crowdy|Game Kit|Social", DisplayName = "Create Guild")
	static UCrowdyKitSocial_CreateGuild* CreateGuild(UObject* WorldContextObject, const FString& Name,
		ECrowdyTeamMembershipPolicy MembershipPolicy = ECrowdyTeamMembershipPolicy::Request);

protected:
	virtual void Run(UCrowdyKitSocial& Social, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail) override;

private:
	FString Name;
	ECrowdyTeamMembershipPolicy MembershipPolicy = ECrowdyTeamMembershipPolicy::Request;
};

UCLASS()
class CROWDYSERVICES_API UCrowdyKitSocial_FindGuild : public UCrowdyKitSocialGroupAction
{
	GENERATED_BODY()

public:
	/** Finds the guild named exactly Name. Fails with Not Found when there is none. Anyone can create a team with the name first, so check Owner User Id is the player you expect. */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"),
		Category = "Crowdy|Game Kit|Social", DisplayName = "Find Guild")
	static UCrowdyKitSocial_FindGuild* FindGuild(UObject* WorldContextObject, const FString& Name);

protected:
	virtual void Run(UCrowdyKitSocial& Social, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail) override;

private:
	FString Name;
};

UCLASS()
class CROWDYSERVICES_API UCrowdyKitSocial_JoinGuild : public UCrowdyKitSocialGroupAction
{
	GENERATED_BODY()

public:
	/** Asks to join the guild's team. A member with the manage members permission accepts the request with Accept Into Guild. */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"),
		Category = "Crowdy|Game Kit|Social", DisplayName = "Join Guild")
	static UCrowdyKitSocial_JoinGuild* JoinGuild(UObject* WorldContextObject, const FCrowdyKitGroup& Group);

protected:
	virtual void Run(UCrowdyKitSocial& Social, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail) override;

private:
	FCrowdyKitGroup Group;
};

UCLASS()
class CROWDYSERVICES_API UCrowdyKitSocial_AcceptIntoGuild : public UCrowdyKitSocialGroupAction
{
	GENERATED_BODY()

public:
	/** Approves the user's join request on the guild's team and adds them to its chat channel. Needs the manage members permission. */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"),
		Category = "Crowdy|Game Kit|Social", DisplayName = "Accept Into Guild")
	static UCrowdyKitSocial_AcceptIntoGuild* AcceptIntoGuild(UObject* WorldContextObject, const FCrowdyKitGroup& Group, int64 UserId);

protected:
	virtual void Run(UCrowdyKitSocial& Social, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail) override;

private:
	FCrowdyKitGroup Group;
	int64 UserId = 0;
};

UCLASS()
class CROWDYSERVICES_API UCrowdyKitSocial_GetGuildRoster : public UCrowdyKitSocialMembersAction
{
	GENERATED_BODY()

public:
	/** Lists the guild's members, with those still awaiting a decision on their join request. */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"),
		Category = "Crowdy|Game Kit|Social", DisplayName = "Get Guild Roster")
	static UCrowdyKitSocial_GetGuildRoster* GetGuildRoster(UObject* WorldContextObject, const FCrowdyKitGroup& Group);

protected:
	virtual void Run(UCrowdyKitSocial& Social, FCrowdyKitMembersDone OnDone, FCrowdyKitFailed OnFail) override;
};

UCLASS()
class CROWDYSERVICES_API UCrowdyKitSocial_PromoteInGuild : public UCrowdyKitSocialGroupAction
{
	GENERATED_BODY()

public:
	/** Replaces the member's roles in the guild with Role Ids. Needs the manage roles permission. */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"),
		Category = "Crowdy|Game Kit|Social", DisplayName = "Promote In Guild")
	static UCrowdyKitSocial_PromoteInGuild* PromoteInGuild(UObject* WorldContextObject, const FCrowdyKitGroup& Group,
		int64 UserId, const TArray<int64>& RoleIds);

protected:
	virtual void Run(UCrowdyKitSocial& Social, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail) override;

private:
	FCrowdyKitGroup Group;
	int64 UserId = 0;
	TArray<int64> RoleIds;
};

UCLASS()
class CROWDYSERVICES_API UCrowdyKitSocial_FindOrCreateChatRoom : public UCrowdyKitSocialGroupAction
{
	GENERATED_BODY()

public:
	/** Finds the chat room named exactly Name, or creates it, and joins it unless the player is already in. The first player to create a room owns it, so any player can claim a name first. */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"),
		Category = "Crowdy|Game Kit|Social", DisplayName = "Find Or Create Chat Room")
	static UCrowdyKitSocial_FindOrCreateChatRoom* FindOrCreateChatRoom(UObject* WorldContextObject, const FString& Name);

protected:
	virtual void Run(UCrowdyKitSocial& Social, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail) override;

private:
	FString Name;
};

UCLASS()
class CROWDYSERVICES_API UCrowdyKitSocial_ClaimGuildTerritory : public UCrowdyKitSocialGroupAction
{
	GENERATED_BODY()

public:
	/**
	 * Grants the guild's members the permission keys on a grid; with a Role Id other than 0, only that role's holders. Empty keys
	 * grant access and update_voxel_data. Needs grid admin rights on the app, so it fails for a player who has none.
	 */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", AutoCreateRefTerm = "PermissionKeys"),
		Category = "Crowdy|Game Kit|Social", DisplayName = "Claim Guild Territory")
	static UCrowdyKitSocial_ClaimGuildTerritory* ClaimGuildTerritory(UObject* WorldContextObject, const FCrowdyKitGroup& Group,
		int64 GridId, const TArray<FString>& PermissionKeys, int64 RoleId = 0);

protected:
	virtual void Run(UCrowdyKitSocial& Social, FCrowdyKitGroupDone OnDone, FCrowdyKitFailed OnFail) override;

private:
	FCrowdyKitGroup Group;
	int64 GridId = 0;
	TArray<FString> PermissionKeys;
	int64 RoleId = 0;
};
