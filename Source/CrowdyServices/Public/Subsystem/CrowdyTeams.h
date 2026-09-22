#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Queries/Data/Teams/Enums/ECrowdyTeamCreationPolicy.h"
#include "Queries/Data/Teams/Enums/ECrowdyTeamMembershipPolicy.h"
#include "Queries/Data/Teams/Enums/ECrowdyTeamPermission.h"
#include "Queries/Data/Teams/Types/FCrowdyTeam.h"
#include "Queries/Data/Teams/Types/FCrowdyTeamError.h"
#include "Queries/Data/Teams/Types/FCrowdyTeamMember.h"
#include "Queries/Data/Teams/Types/FCrowdyTeamMembership.h"
#include "Queries/Data/Teams/Types/FCrowdyTeamPermissions.h"
#include "Queries/Data/Teams/Types/FCrowdyTeamPolicy.h"
#include "Queries/Data/Teams/Types/FCrowdyTeamRole.h"
#include "CrowdyTeams.generated.h"

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnTeamSuccess, FCrowdyTeam, Team);

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnTeamsSuccess, const TArray<FCrowdyTeam>&, Teams);

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnTeamMemberSuccess, FCrowdyTeamMember, Member);

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnTeamMembersSuccess, const TArray<FCrowdyTeamMember>&, Members);

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnTeamRoleSuccess, FCrowdyTeamRole, Role);

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnTeamRolesSuccess, const TArray<FCrowdyTeamRole>&, Roles);

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnMyTeamsSuccess, const TArray<FCrowdyTeamMembership>&, Memberships);

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnTeamPolicySuccess, FCrowdyTeamPolicy, Policy);

DECLARE_DYNAMIC_DELEGATE(FOnTeamVoidSuccess);

DECLARE_DYNAMIC_DELEGATE_TwoParams(FOnTeamError, FCrowdyTeamError, Error, FString, Message);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnMyTeamsCacheChanged, const TArray<FCrowdyTeamMembership>&, Memberships);

/**
 * Teams: persistent named groups of players with roles and permissions, scoped to one app.
 *
 * Every call here is asynchronous and answers through exactly one of its two delegates, including when the request
 * never reaches the server. Only the signed-in player's own memberships are cached; everything else is read live.
 */
UCLASS()
class CROWDYSERVICES_API UCrowdyTeams : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Teams")
	FOnMyTeamsCacheChanged OnMyTeamsCacheChanged;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Crowdy SDK|Teams|Cache")
	bool HasCachedTeams() const { return bCachePopulated; }

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Crowdy SDK|Teams|Cache")
	TArray<FCrowdyTeamMembership> GetCachedMyTeams() const { return CachedMyTeams; }

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Crowdy SDK|Teams|Cache")
	bool IsPlayerInTeam(int64 TeamId) const;

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Cache")
	bool GetMyTeamById(int64 TeamId, FCrowdyTeamMembership& OutMembership) const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Crowdy SDK|Teams|Cache")
	bool IsInAnyTeam() const;

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Cache")
	bool GetPrimaryMembership(FCrowdyTeamMembership& OutMembership) const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Crowdy SDK|Teams|Cache")
	bool HasPermissionInTeam(int64 TeamId, ECrowdyTeamPermission Permission) const;

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Queries|Request")
	void GetPendingJoinRequests(int64 TeamId, FOnTeamMembersSuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Queries|Team")
	void GetMyTeams(FOnMyTeamsSuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Queries|Team")
	void GetTeam(int64 TeamId, FOnTeamSuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Queries|Team")
	void GetTeams(FOnTeamsSuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Queries|Members")
	void GetTeamMembers(int64 TeamId, FOnTeamMembersSuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Queries|Roles")
	void GetTeamRoles(int64 TeamId, FOnTeamRolesSuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Queries|Policy")
	void GetTeamPolicy(FOnTeamPolicySuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Mutations|Team")
	void CreateTeam(const FString& Name, const FString& Description,
	                ECrowdyTeamMembershipPolicy MembershipPolicy,
	                FOnTeamSuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Mutations|Team")
	void UpdateTeam(int64 TeamId, const FString& Name, const FString& Description,
	                FOnTeamSuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Mutations|Team")
	void DeleteTeam(int64 TeamId, FOnTeamVoidSuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Mutations|Team")
	void JoinTeam(int64 TeamId, FOnTeamMemberSuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Mutations|Request")
	void RequestToJoinTeam(int64 TeamId, FOnTeamMemberSuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Mutations|Team")
	void LeaveTeam(int64 TeamId, FOnTeamVoidSuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Mutations|Members")
	void AddTeamMember(int64 TeamId, int64 UserId, FOnTeamMemberSuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Mutations|Members")
	void RemoveTeamMember(int64 TeamId, int64 UserId, FOnTeamVoidSuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Mutations|Roles")
	void CreateTeamRole(int64 TeamId, const FString& RoleName, FCrowdyTeamPermissions Permissions,
	                    int32 Rank, FOnTeamRoleSuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Mutations|Roles")
	void UpdateTeamRole(int64 TeamRoleId, const FString& RoleName, FCrowdyTeamPermissions Permissions,
	                    FOnTeamRoleSuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Mutations|Roles")
	void DeleteTeamRole(int64 TeamRoleId, FOnTeamVoidSuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Mutations|Roles")
	void SetTeamMemberRoles(int64 TeamId, int64 UserId, const TArray<int64>& RoleIds,
	                        FOnTeamMemberSuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Mutations|Policy")
	void SetTeamPolicy(ECrowdyTeamCreationPolicy CreationPolicy,
	                   ECrowdyTeamMembershipPolicy DefaultMembershipPolicy,
	                   FOnTeamPolicySuccess OnSuccess, FOnTeamError OnError);

private:
	TArray<FCrowdyTeamMembership> CachedMyTeams;
	bool bCachePopulated = false;

	/**
	 * Marks this subsystem's usable lifetime. Every completion handed to the shared API client holds it weakly, so
	 * a request still in flight at teardown lands on nothing rather than on a subsystem whose state has been
	 * cleared.
	 */
	TSharedPtr<uint8> LiveSessionToken;

	int64 GetAppId() const;
};
