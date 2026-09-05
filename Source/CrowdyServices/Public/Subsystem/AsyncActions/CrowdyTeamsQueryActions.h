#pragma once

#include "Kismet/BlueprintAsyncActionBase.h"
#include "Queries/Data/Teams/Types/FCrowdyTeam.h"
#include "Queries/Data/Teams/Types/FCrowdyTeamMember.h"
#include "Queries/Data/Teams/Types/FCrowdyTeamMembership.h"
#include "Queries/Data/Teams/Types/FCrowdyTeamRole.h"
#include "Queries/Data/Teams/Types/FCrowdyTeamPolicy.h"
#include "Queries/Data/Teams/Types/FCrowdyTeamError.h"
#include "CrowdyTeamsQueryActions.generated.h"

USTRUCT(BlueprintType)
struct FCrowdyTeamsResult
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly)
	TArray<FCrowdyTeam> Teams;
};

USTRUCT(BlueprintType)
struct FCrowdyMyTeamsResult
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly)
	TArray<FCrowdyTeamMembership> Memberships;
};

USTRUCT(BlueprintType)
struct FCrowdyTeamMembersResult
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly)
	TArray<FCrowdyTeamMember> Members;
};

USTRUCT(BlueprintType)
struct FCrowdyTeamRolesResult
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly)
	TArray<FCrowdyTeamRole> Roles;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FTeamAsyncOnSuccess, FCrowdyTeam, Team);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FTeamsAsyncOnSuccess, FCrowdyTeamsResult, Result);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FMyTeamsAsyncOnSuccess, FCrowdyMyTeamsResult, Result);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FMemberAsyncOnSuccess, FCrowdyTeamMember, Member);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FMembersAsyncOnSuccess, FCrowdyTeamMembersResult, Result);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FRoleAsyncOnSuccess, FCrowdyTeamRole, Role);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FRolesAsyncOnSuccess, FCrowdyTeamRolesResult, Result);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FPolicyAsyncOnSuccess, FCrowdyTeamPolicy, Policy);

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
	void HandleSuccess(TArray<FCrowdyTeamMembership> Memberships);
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
	static UCrowdyTeams_GetTeam* GetTeam(UObject* WorldContextObject, int64 TeamId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 TeamId = 0;
	UFUNCTION()
	void HandleSuccess(FCrowdyTeam Team);
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
	void HandleSuccess(TArray<FCrowdyTeam> Teams);
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
	static UCrowdyTeams_GetTeamMembers* GetTeamMembers(UObject* WorldContextObject, int64 TeamId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 TeamId = 0;
	UFUNCTION()
	void HandleSuccess(TArray<FCrowdyTeamMember> Members);
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
	static UCrowdyTeams_GetTeamRoles* GetTeamRoles(UObject* WorldContextObject, int64 TeamId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 TeamId = 0;
	UFUNCTION()
	void HandleSuccess(TArray<FCrowdyTeamRole> Roles);
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
	void HandleSuccess(FCrowdyTeamPolicy Policy);
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
	static UCrowdyTeams_GetPendingJoinRequests* GetPendingJoinRequests(UObject* WorldContextObject, int64 TeamId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 TeamId = 0;
	UFUNCTION()
	void HandleSuccess(TArray<FCrowdyTeamMember> Members);
	UFUNCTION()
	void HandleError(FCrowdyTeamError Error, FString Message);
};
