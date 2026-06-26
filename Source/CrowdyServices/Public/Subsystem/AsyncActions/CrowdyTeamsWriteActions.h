#pragma once

#include "Kismet/BlueprintAsyncActionBase.h"
#include "Subsystem/AsyncActions/CrowdyTeamsQueryActions.h"
#include "Queries/Data/Teams/Enums/ECrowdyTeamMembershipPolicy.h"
#include "Queries/Data/Teams/Enums/ECrowdyTeamCreationPolicy.h"
#include "Queries/Data/Teams/Types/FCrowdyRolePermissions.h"
#include "CrowdyTeamsWriteActions.generated.h"

UCLASS()
class CROWDYSERVICES_API UCrowdyTeams_CreateTeam : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FTeamAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FTeamsAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Teams|Mutations", DisplayName="Create Team")
	static UCrowdyTeams_CreateTeam* CreateTeam(UObject* WorldContextObject, const FString& Name,
	                                           const FString& Description,
	                                           ECrowdyTeamMembershipPolicy MembershipPolicy);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString Name, Description;
	ECrowdyTeamMembershipPolicy MembershipPolicy = ECrowdyTeamMembershipPolicy::Open;
	UFUNCTION()
	void HandleSuccess(FCrowdyGroup Group);
	UFUNCTION()
	void HandleError(FCrowdyTeamError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyTeams_UpdateTeam : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FTeamAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FTeamsAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Teams|Mutations", DisplayName="Update Team")
	static UCrowdyTeams_UpdateTeam* UpdateTeam(UObject* WorldContextObject, int64 GroupId, const FString& Name,
	                                           const FString& Description);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 GroupId = 0;
	FString Name, Description;
	UFUNCTION()
	void HandleSuccess(FCrowdyGroup Group);
	UFUNCTION()
	void HandleError(FCrowdyTeamError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyTeams_DeleteTeam : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FVoidAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FTeamsAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Teams|Mutations", DisplayName="Delete Team")
	static UCrowdyTeams_DeleteTeam* DeleteTeam(UObject* WorldContextObject, int64 GroupId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 GroupId = 0;
	UFUNCTION()
	void HandleSuccess();
	UFUNCTION()
	void HandleError(FCrowdyTeamError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyTeams_JoinTeam : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FMemberAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FTeamsAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Teams|Mutations", DisplayName="Join Team")
	static UCrowdyTeams_JoinTeam* JoinTeam(UObject* WorldContextObject, int64 GroupId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 GroupId = 0;
	UFUNCTION()
	void HandleSuccess(FCrowdyGroupMember Member);
	UFUNCTION()
	void HandleError(FCrowdyTeamError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyTeams_RequestToJoinTeam : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FMemberAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FTeamsAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Teams|Mutations", DisplayName="Request to Join Team")
	static UCrowdyTeams_RequestToJoinTeam* RequestToJoinTeam(UObject* WorldContextObject, int64 GroupId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 GroupId = 0;
	UFUNCTION()
	void HandleSuccess(FCrowdyGroupMember Member);
	UFUNCTION()
	void HandleError(FCrowdyTeamError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyTeams_LeaveTeam : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FVoidAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FTeamsAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Teams|Mutations", DisplayName="Leave Team")
	static UCrowdyTeams_LeaveTeam* LeaveTeam(UObject* WorldContextObject, int64 GroupId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 GroupId = 0;
	UFUNCTION()
	void HandleSuccess();
	UFUNCTION()
	void HandleError(FCrowdyTeamError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyTeams_AddTeamMember : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FMemberAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FTeamsAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Teams|Mutations", DisplayName="Add Team Member")
	static UCrowdyTeams_AddTeamMember* AddTeamMember(UObject* WorldContextObject, int64 GroupId, int64 UserId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 GroupId = 0, UserId = 0;
	UFUNCTION()
	void HandleSuccess(FCrowdyGroupMember Member);
	UFUNCTION()
	void HandleError(FCrowdyTeamError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyTeams_RemoveTeamMember : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FVoidAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FTeamsAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Teams|Mutations", DisplayName="Remove Team Member")
	static UCrowdyTeams_RemoveTeamMember* RemoveTeamMember(UObject* WorldContextObject, int64 GroupId, int64 UserId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 GroupId = 0, UserId = 0;
	UFUNCTION()
	void HandleSuccess();
	UFUNCTION()
	void HandleError(FCrowdyTeamError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyTeams_SetTeamMemberRoles : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FMemberAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FTeamsAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Teams|Mutations", DisplayName="Set Team Member Roles")
	static UCrowdyTeams_SetTeamMemberRoles* SetTeamMemberRoles(UObject* WorldContextObject, int64 GroupId, int64 UserId,
	                                                           const TArray<int64>& RoleIds);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 GroupId = 0, UserId = 0;
	TArray<int64> RoleIds;
	UFUNCTION()
	void HandleSuccess(FCrowdyGroupMember Member);
	UFUNCTION()
	void HandleError(FCrowdyTeamError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyTeams_CreateTeamRole : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FRoleAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FTeamsAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Teams|Mutations", DisplayName="Create Team Role")
	static UCrowdyTeams_CreateTeamRole* CreateTeamRole(UObject* WorldContextObject, int64 GroupId,
	                                                   const FString& RoleName, FCrowdyRolePermissions Permissions,
	                                                   int32 Rank);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 GroupId = 0;
	FString RoleName;
	FCrowdyRolePermissions Permissions;
	int32 Rank = 0;
	UFUNCTION()
	void HandleSuccess(FCrowdyGroupRole Role);
	UFUNCTION()
	void HandleError(FCrowdyTeamError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyTeams_UpdateTeamRole : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FRoleAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FTeamsAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Teams|Mutations", DisplayName="Update Team Role")
	static UCrowdyTeams_UpdateTeamRole* UpdateTeamRole(UObject* WorldContextObject, int64 RoleId,
	                                                   const FString& RoleName, FCrowdyRolePermissions Permissions);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 RoleId = 0;
	FString RoleName;
	FCrowdyRolePermissions Permissions;
	UFUNCTION()
	void HandleSuccess(FCrowdyGroupRole Role);
	UFUNCTION()
	void HandleError(FCrowdyTeamError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyTeams_DeleteTeamRole : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FVoidAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FTeamsAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Teams|Mutations", DisplayName="Delete Team Role")
	static UCrowdyTeams_DeleteTeamRole* DeleteTeamRole(UObject* WorldContextObject, int64 RoleId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 RoleId = 0;
	UFUNCTION()
	void HandleSuccess();
	UFUNCTION()
	void HandleError(FCrowdyTeamError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyTeams_SetTeamPolicy : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FPolicyAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FTeamsAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Teams|Mutations", DisplayName="Set Team Policy")
	static UCrowdyTeams_SetTeamPolicy* SetTeamPolicy(UObject* WorldContextObject,
	                                                 ECrowdyTeamCreationPolicy CreationPolicy,
	                                                 ECrowdyTeamMembershipPolicy DefaultMembershipPolicy);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	ECrowdyTeamCreationPolicy CreationPolicy = ECrowdyTeamCreationPolicy::Anyone;
	ECrowdyTeamMembershipPolicy MembershipPolicy = ECrowdyTeamMembershipPolicy::Open;
	UFUNCTION()
	void HandleSuccess(FCrowdyAppGroupPolicy Policy);
	UFUNCTION()
	void HandleError(FCrowdyTeamError Error, FString Message);
};
