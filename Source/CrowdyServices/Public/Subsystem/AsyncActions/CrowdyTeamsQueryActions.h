#pragma once

#include "Kismet/BlueprintAsyncActionBase.h"
#include "Queries/Data/Teams/Types/FCrowdyGroup.h"
#include "Queries/Data/Teams/Types/FCrowdyGroupMember.h"
#include "Queries/Data/Teams/Types/FCrowdyGroupMembership.h"
#include "Queries/Data/Teams/Types/FCrowdyGroupRole.h"
#include "Queries/Data/Teams/Types/FCrowdyAppGroupPolicy.h"
#include "Queries/Data/Teams/Types/FCrowdyTeamError.h"
#include "CrowdyTeamsQueryActions.generated.h"

USTRUCT(BlueprintType)
struct FCrowdyTeamsResult
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly)
	TArray<FCrowdyGroup> Groups;
};

USTRUCT(BlueprintType)
struct FCrowdyMyTeamsResult
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly)
	TArray<FCrowdyGroupMembership> Memberships;
};

USTRUCT(BlueprintType)
struct FCrowdyTeamMembersResult
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly)
	TArray<FCrowdyGroupMember> Members;
};

USTRUCT(BlueprintType)
struct FCrowdyTeamRolesResult
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly)
	TArray<FCrowdyGroupRole> Roles;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FTeamAsyncOnSuccess, FCrowdyGroup, Group);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FTeamsAsyncOnSuccess, FCrowdyTeamsResult, Result);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FMyTeamsAsyncOnSuccess, FCrowdyMyTeamsResult, Result);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FMemberAsyncOnSuccess, FCrowdyGroupMember, Member);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FMembersAsyncOnSuccess, FCrowdyTeamMembersResult, Result);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FRoleAsyncOnSuccess, FCrowdyGroupRole, Role);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FRolesAsyncOnSuccess, FCrowdyTeamRolesResult, Result);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FPolicyAsyncOnSuccess, FCrowdyAppGroupPolicy, Policy);

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FVoidAsyncOnSuccess);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FTeamsAsyncOnError, FCrowdyTeamError, Error, FString, Message);


UCLASS()
class CROWDYSERVICES_API UCrowdyTeams_GetMyTeams : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FMyTeamsAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FTeamsAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Teams|Queries", DisplayName="Get My Teams")
	static UCrowdyTeams_GetMyTeams* GetMyTeams(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	UFUNCTION()
	void HandleSuccess(TArray<FCrowdyGroupMembership> Memberships);
	UFUNCTION()
	void HandleError(FCrowdyTeamError Error, FString Message);
};


UCLASS()
class CROWDYSERVICES_API UCrowdyTeams_GetTeam : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FTeamAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FTeamsAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Teams|Queries", DisplayName="Get Team")
	static UCrowdyTeams_GetTeam* GetTeam(UObject* WorldContextObject, int64 GroupId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 GroupId = 0;
	UFUNCTION()
	void HandleSuccess(FCrowdyGroup Group);
	UFUNCTION()
	void HandleError(FCrowdyTeamError Error, FString Message);
};


UCLASS()
class CROWDYSERVICES_API UCrowdyTeams_GetTeams : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FTeamsAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FTeamsAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Teams|Queries", DisplayName="Get All Teams")
	static UCrowdyTeams_GetTeams* GetTeams(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	UFUNCTION()
	void HandleSuccess(TArray<FCrowdyGroup> Groups);
	UFUNCTION()
	void HandleError(FCrowdyTeamError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyTeams_GetTeamMembers : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FMembersAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FTeamsAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Teams|Queries", DisplayName="Get Team Members")
	static UCrowdyTeams_GetTeamMembers* GetTeamMembers(UObject* WorldContextObject, int64 GroupId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 GroupId = 0;
	UFUNCTION()
	void HandleSuccess(TArray<FCrowdyGroupMember> Members);
	UFUNCTION()
	void HandleError(FCrowdyTeamError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyTeams_GetTeamRoles : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FRolesAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FTeamsAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Teams|Queries", DisplayName="Get Team Roles")
	static UCrowdyTeams_GetTeamRoles* GetTeamRoles(UObject* WorldContextObject, int64 GroupId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 GroupId = 0;
	UFUNCTION()
	void HandleSuccess(TArray<FCrowdyGroupRole> Roles);
	UFUNCTION()
	void HandleError(FCrowdyTeamError Error, FString Message);
};


UCLASS()
class CROWDYSERVICES_API UCrowdyTeams_GetTeamPolicy : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FPolicyAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FTeamsAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Teams|Queries", DisplayName="Get Team Policy")
	static UCrowdyTeams_GetTeamPolicy* GetTeamPolicy(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	UFUNCTION()
	void HandleSuccess(FCrowdyAppGroupPolicy Policy);
	UFUNCTION()
	void HandleError(FCrowdyTeamError Error, FString Message);
};


UCLASS()
class CROWDYSERVICES_API UCrowdyTeams_GetPendingJoinRequests : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FMembersAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FTeamsAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Teams|Queries", DisplayName="Get Pending Join Requests")
	static UCrowdyTeams_GetPendingJoinRequests* GetPendingJoinRequests(UObject* WorldContextObject, int64 GroupId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 GroupId = 0;
	UFUNCTION()
	void HandleSuccess(TArray<FCrowdyGroupMember> Members);
	UFUNCTION()
	void HandleError(FCrowdyTeamError Error, FString Message);
};
