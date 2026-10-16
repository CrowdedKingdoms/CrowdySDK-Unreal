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

// One signature serves On Success and On Error, because a Blueprint async node builds its data pins from the first delegate only.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FTeamAsyncResult, FCrowdyTeam, Team, FCrowdyTeamError, Error, FString, Message);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FTeamsAsyncResult, FCrowdyTeamsResult, Result, FCrowdyTeamError, Error, FString, Message);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FMyTeamsAsyncResult, FCrowdyMyTeamsResult, Result, FCrowdyTeamError, Error, FString, Message);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FMemberAsyncResult, FCrowdyTeamMember, Member, FCrowdyTeamError, Error, FString, Message);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FMembersAsyncResult, FCrowdyTeamMembersResult, Result, FCrowdyTeamError, Error, FString, Message);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FRoleAsyncResult, FCrowdyTeamRole, Role, FCrowdyTeamError, Error, FString, Message);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FRolesAsyncResult, FCrowdyTeamRolesResult, Result, FCrowdyTeamError, Error, FString, Message);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FPolicyAsyncResult, FCrowdyTeamPolicy, Policy, FCrowdyTeamError, Error, FString, Message);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FTeamsAsyncOnError, FCrowdyTeamError, Error, FString, Message);


UCLASS()
class CROWDYSERVICES_API UCrowdyTeams_GetMyTeams : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FMyTeamsAsyncResult OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FMyTeamsAsyncResult OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Teams|Queries", DisplayName="Get My Teams")
	static UCrowdyTeams_GetMyTeams* GetMyTeams(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	UFUNCTION()
	void HandleSuccess(const TArray<FCrowdyTeamMembership>& Memberships);
	UFUNCTION()
	void HandleError(FCrowdyTeamError Error, FString Message);
};


UCLASS()
class CROWDYSERVICES_API UCrowdyTeams_GetTeam : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FTeamAsyncResult OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FTeamAsyncResult OnError;

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
	FTeamsAsyncResult OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FTeamsAsyncResult OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Teams|Queries", DisplayName="Get All Teams")
	static UCrowdyTeams_GetTeams* GetTeams(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	UFUNCTION()
	void HandleSuccess(const TArray<FCrowdyTeam>& Teams);
	UFUNCTION()
	void HandleError(FCrowdyTeamError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyTeams_GetTeamMembers : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FMembersAsyncResult OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FMembersAsyncResult OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Teams|Queries", DisplayName="Get Team Members")
	static UCrowdyTeams_GetTeamMembers* GetTeamMembers(UObject* WorldContextObject, int64 TeamId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 TeamId = 0;
	UFUNCTION()
	void HandleSuccess(const TArray<FCrowdyTeamMember>& Members);
	UFUNCTION()
	void HandleError(FCrowdyTeamError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyTeams_GetTeamRoles : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FRolesAsyncResult OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FRolesAsyncResult OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Teams|Queries", DisplayName="Get Team Roles")
	static UCrowdyTeams_GetTeamRoles* GetTeamRoles(UObject* WorldContextObject, int64 TeamId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 TeamId = 0;
	UFUNCTION()
	void HandleSuccess(const TArray<FCrowdyTeamRole>& Roles);
	UFUNCTION()
	void HandleError(FCrowdyTeamError Error, FString Message);
};


UCLASS()
class CROWDYSERVICES_API UCrowdyTeams_GetTeamPolicy : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FPolicyAsyncResult OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FPolicyAsyncResult OnError;

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
	FMembersAsyncResult OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FMembersAsyncResult OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Teams|Queries", DisplayName="Get Pending Join Requests")
	static UCrowdyTeams_GetPendingJoinRequests* GetPendingJoinRequests(UObject* WorldContextObject, int64 TeamId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 TeamId = 0;
	UFUNCTION()
	void HandleSuccess(const TArray<FCrowdyTeamMember>& Members);
	UFUNCTION()
	void HandleError(FCrowdyTeamError Error, FString Message);
};
