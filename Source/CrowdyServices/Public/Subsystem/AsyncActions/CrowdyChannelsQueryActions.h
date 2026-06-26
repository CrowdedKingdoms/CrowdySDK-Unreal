#pragma once

#include "Kismet/BlueprintAsyncActionBase.h"
#include "Queries/Data/Teams/Types/FCrowdyGroup.h"
#include "Queries/Data/Teams/Types/FCrowdyGroupMember.h"
#include "Queries/Data/Teams/Types/FCrowdyGroupMembership.h"
#include "Queries/Data/Teams/Types/FCrowdyGroupRole.h"
#include "Queries/Data/Teams/Types/FCrowdyAppGroupPolicy.h"
#include "Queries/Data/Teams/Types/FCrowdyTeamError.h"
#include "CrowdyChannelsQueryActions.generated.h"

USTRUCT(BlueprintType)
struct FCrowdyChannelsResult
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly)
	TArray<FCrowdyGroup> Channels;
};

USTRUCT(BlueprintType)
struct FCrowdyMyChannelsResult
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly)
	TArray<FCrowdyGroupMembership> Memberships;
};

USTRUCT(BlueprintType)
struct FCrowdyChannelMembersResult
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly)
	TArray<FCrowdyGroupMember> Members;
};

USTRUCT(BlueprintType)
struct FCrowdyChannelRolesResult
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly)
	TArray<FCrowdyGroupRole> Roles;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FChannelAsyncOnSuccess, FCrowdyGroup, Channel);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FChannelsAsyncOnSuccess, FCrowdyChannelsResult, Channels);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FMyChannelsAsyncOnSuccess, FCrowdyMyChannelsResult, Memberships);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FChannelMemberAsyncOnSuccess, FCrowdyGroupMember, Member);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FChannelMembersAsyncOnSuccess, FCrowdyChannelMembersResult, Members);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FChannelRoleAsyncOnSuccess, FCrowdyGroupRole, Role);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FChannelRolesAsyncOnSuccess, FCrowdyChannelRolesResult, Roles);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FChannelPolicyAsyncOnSuccess, FCrowdyAppGroupPolicy, Policy);

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FChannelVoidAsyncOnSuccess);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FChannelAsyncOnError, FCrowdyTeamError, Error, FString, Message);


UCLASS()
class CROWDYSERVICES_API UCrowdyChannels_GetMyChannels : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FMyChannelsAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FChannelAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Channels|Queries", DisplayName="Get My Channels")
	static UCrowdyChannels_GetMyChannels* GetMyChannels(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	UFUNCTION()
	void HandleSuccess(TArray<FCrowdyGroupMembership> Memberships);
	UFUNCTION()
	void HandleError(FCrowdyTeamError Error, FString Message);
};


UCLASS()
class CROWDYSERVICES_API UCrowdyChannels_GetChannel : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FChannelAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FChannelAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Channels|Queries", DisplayName="Get Channel")
	static UCrowdyChannels_GetChannel* GetChannel(UObject* WorldContextObject, int64 GroupId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 GroupId = 0;
	UFUNCTION()
	void HandleSuccess(FCrowdyGroup Channel);
	UFUNCTION()
	void HandleError(FCrowdyTeamError Error, FString Message);
};


UCLASS()
class CROWDYSERVICES_API UCrowdyChannels_GetChannels : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FChannelsAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FChannelAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Channels|Queries", DisplayName="Get All Channels")
	static UCrowdyChannels_GetChannels* GetChannels(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	UFUNCTION()
	void HandleSuccess(TArray<FCrowdyGroup> Channels);
	UFUNCTION()
	void HandleError(FCrowdyTeamError Error, FString Message);
};


UCLASS()
class CROWDYSERVICES_API UCrowdyChannels_GetChannelMembers : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FChannelMembersAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FChannelAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Channels|Queries", DisplayName="Get Channel Members")
	static UCrowdyChannels_GetChannelMembers* GetChannelMembers(UObject* WorldContextObject, int64 GroupId);

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
class CROWDYSERVICES_API UCrowdyChannels_GetChannelRoles : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FChannelRolesAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FChannelAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Channels|Queries", DisplayName="Get Channel Roles")
	static UCrowdyChannels_GetChannelRoles* GetChannelRoles(UObject* WorldContextObject, int64 GroupId);

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
class CROWDYSERVICES_API UCrowdyChannels_GetChannelPolicy : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FChannelPolicyAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FChannelAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Channels|Queries", DisplayName="Get Channel Policy")
	static UCrowdyChannels_GetChannelPolicy* GetChannelPolicy(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	UFUNCTION()
	void HandleSuccess(FCrowdyAppGroupPolicy Policy);
	UFUNCTION()
	void HandleError(FCrowdyTeamError Error, FString Message);
};


UCLASS()
class CROWDYSERVICES_API UCrowdyChannels_GetPendingJoinRequests : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FChannelMembersAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FChannelAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Channels|Queries", DisplayName="Get Pending Join Requests")
	static UCrowdyChannels_GetPendingJoinRequests* GetPendingJoinRequests(UObject* WorldContextObject, int64 GroupId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 GroupId = 0;
	UFUNCTION()
	void HandleSuccess(TArray<FCrowdyGroupMember> Members);
	UFUNCTION()
	void HandleError(FCrowdyTeamError Error, FString Message);
};
