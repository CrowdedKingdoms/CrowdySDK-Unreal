#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Core/GraphQL/Interfaces/ICrowdyQueryReceptionLayer.h"
#include "Queries/Data/Teams/Types/FCrowdyGroup.h"
#include "Queries/Data/Teams/Types/FCrowdyGroupMember.h"
#include "Queries/Data/Teams/Types/FCrowdyGroupMembership.h"
#include "Queries/Data/Teams/Types/FCrowdyGroupRole.h"
#include "Queries/Data/Teams/Types/FCrowdyAppGroupPolicy.h"
#include "Queries/Data/Teams/Types/FCrowdyTeamError.h"
#include "Queries/Data/Teams/Enums/ECrowdyTeamCreationPolicy.h"
#include "Queries/Data/Teams/Enums/ECrowdyTeamMembershipPolicy.h"
#include "Queries/Data/Teams/Enums/ECrowdyTeamPermission.h"
#include "Queries/Data/Teams/Types/FCrowdyRolePermissions.h"
#include "CrowdyTeams.generated.h"

class UCrowdyQuerySubsystem;
class FCrowdyDataRegistry;

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnTeamSuccess, FCrowdyGroup, Team);

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnTeamsSuccess, TArray<FCrowdyGroup>, Teams);

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnTeamMemberSuccess, FCrowdyGroupMember, Member);

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnTeamMembersSuccess, TArray<FCrowdyGroupMember>, Members);

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnTeamRoleSuccess, FCrowdyGroupRole, Role);

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnTeamRolesSuccess, TArray<FCrowdyGroupRole>, Roles);

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnMyTeamsSuccess, TArray<FCrowdyGroupMembership>, Memberships);

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnTeamPolicySuccess, FCrowdyAppGroupPolicy, Policy);

DECLARE_DYNAMIC_DELEGATE(FOnTeamVoidSuccess);

DECLARE_DYNAMIC_DELEGATE_TwoParams(FOnTeamError, FCrowdyTeamError, Error, FString, Message);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnMyTeamsCacheChanged, TArray<FCrowdyGroupMembership>, Memberships);

UCLASS()
class CROWDYSERVICES_API UCrowdyTeams : public UGameInstanceSubsystem, public ICrowdyQueryReceptionLayer
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	void InjectDependencies(FCrowdyDataRegistry* InDataRegistry, UCrowdyQuerySubsystem* InQuerySubsystem);

	virtual void OnResponseReceived(TSharedPtr<ICrowdyQueryResponse> Response) override;
	virtual TArray<EQueryResponseType> GetSupportedResponseType() const override;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Teams")
	FOnMyTeamsCacheChanged OnMyTeamsCacheChanged;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Crowdy SDK|Teams|Cache")
	bool HasCachedTeams() const { return bCachePopulated; }

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Crowdy SDK|Teams|Cache")
	TArray<FCrowdyGroupMembership> GetCachedMyTeams() const { return CachedMyTeams; }

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Crowdy SDK|Teams|Cache")
	bool IsPlayerInTeam(int64 GroupId) const;

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Cache")
	bool GetMyTeamById(int64 GroupId, FCrowdyGroupMembership& OutMembership) const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Crowdy SDK|Teams|Cache")
	bool IsInAnyTeam() const;

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Cache")
	bool GetPrimaryMembership(FCrowdyGroupMembership& OutMembership) const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Crowdy SDK|Teams|Cache")
	bool HasPermissionInTeam(int64 GroupId, ECrowdyTeamPermission Permission) const;

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Queries|Request")
	void GetPendingJoinRequests(int64 GroupId, FOnTeamMembersSuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Queries|Team")
	void GetMyTeams(FOnMyTeamsSuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Queries|Team")
	void GetTeam(int64 GroupId, FOnTeamSuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Queries|Team")
	void GetTeams(FOnTeamsSuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Queries|Members")
	void GetTeamMembers(int64 GroupId, FOnTeamMembersSuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Queries|Roles")
	void GetTeamRoles(int64 GroupId, FOnTeamRolesSuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Queries|Policy")
	void GetTeamPolicy(FOnTeamPolicySuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Mutations|Team")
	void CreateTeam(const FString& Name, const FString& Description,
	                ECrowdyTeamMembershipPolicy MembershipPolicy,
	                FOnTeamSuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Mutations|Team")
	void UpdateTeam(int64 GroupId, const FString& Name, const FString& Description,
	                FOnTeamSuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Mutations|Team")
	void DeleteTeam(int64 GroupId, FOnTeamVoidSuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Mutations|Team")
	void JoinTeam(int64 GroupId, FOnTeamMemberSuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Mutations|Request")
	void RequestToJoinTeam(int64 GroupId, FOnTeamMemberSuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Mutations|Team")
	void LeaveTeam(int64 GroupId, FOnTeamVoidSuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Mutations|Members")
	void AddTeamMember(int64 GroupId, int64 UserId, FOnTeamMemberSuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Mutations|Members")
	void RemoveTeamMember(int64 GroupId, int64 UserId, FOnTeamVoidSuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Mutations|Roles")
	void CreateTeamRole(int64 GroupId, const FString& RoleName, FCrowdyRolePermissions Permissions,
	                    int32 Rank, FOnTeamRoleSuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Mutations|Roles")
	void UpdateTeamRole(int64 RoleId, const FString& RoleName, FCrowdyRolePermissions Permissions,
	                    FOnTeamRoleSuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Mutations|Roles")
	void DeleteTeamRole(int64 RoleId, FOnTeamVoidSuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Mutations|Roles")
	void SetTeamMemberRoles(int64 GroupId, int64 UserId, const TArray<int64>& RoleIds,
	                        FOnTeamMemberSuccess OnSuccess, FOnTeamError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Teams|Mutations|Policy")
	void SetTeamPolicy(ECrowdyTeamCreationPolicy CreationPolicy,
	                   ECrowdyTeamMembershipPolicy DefaultMembershipPolicy,
	                   FOnTeamPolicySuccess OnSuccess, FOnTeamError OnError);

private:
	UPROPERTY()
	UCrowdyQuerySubsystem* QuerySubsystem = nullptr;

	mutable FCriticalSection CallbackMutex;
	TMap<EQueryResponseType, TArray<TFunction<void(TSharedPtr<ICrowdyQueryResponse>)>>> PendingCallbacks;

	TArray<FCrowdyGroupMembership> CachedMyTeams;
	bool bCachePopulated = false;

	int64 GetAppId() const;

	void PushCallback(EQueryResponseType Type, TFunction<void(TSharedPtr<ICrowdyQueryResponse>)> Callback);
	void FireCallback(TSharedPtr<ICrowdyQueryResponse> Response);

	static TSharedPtr<FJsonObject> MakeVarsWithStringArray(
		const TMap<FString, FString>& ScalarFields,
		const TMap<FString, TArray<FString>>& StringArrayFields);
};
