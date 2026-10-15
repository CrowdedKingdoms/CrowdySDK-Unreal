#pragma once

#include "Kismet/BlueprintAsyncActionBase.h"
#include "Queries/Data/Channels/Types/FCrowdyChannel.h"
#include "Queries/Data/Channels/Types/FCrowdyChannelMember.h"
#include "Queries/Data/Channels/Types/FCrowdyChannelMembership.h"
#include "Queries/Data/Channels/Types/FCrowdyChannelRole.h"
#include "Queries/Data/Channels/Types/FCrowdyChannelPolicy.h"
#include "Queries/Data/Channels/Types/FCrowdyChannelError.h"
#include "CrowdyChannelsQueryActions.generated.h"

USTRUCT(BlueprintType)
struct FCrowdyChannelsResult
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly)
	TArray<FCrowdyChannel> Channels;
};

USTRUCT(BlueprintType)
struct FCrowdyMyChannelsResult
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly)
	TArray<FCrowdyChannelMembership> Memberships;
};

USTRUCT(BlueprintType)
struct FCrowdyChannelMembersResult
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly)
	TArray<FCrowdyChannelMember> Members;
};

USTRUCT(BlueprintType)
struct FCrowdyChannelRolesResult
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly)
	TArray<FCrowdyChannelRole> Roles;
};

// One signature serves On Success and On Error, because a Blueprint async node builds its data pins from the first delegate only.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FChannelAsyncResult, FCrowdyChannel, Channel, FCrowdyChannelError, Error, FString, Message);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FChannelsAsyncResult, FCrowdyChannelsResult, Channels, FCrowdyChannelError, Error, FString, Message);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FMyChannelsAsyncResult, FCrowdyMyChannelsResult, Memberships, FCrowdyChannelError, Error, FString, Message);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FChannelMemberAsyncResult, FCrowdyChannelMember, Member, FCrowdyChannelError, Error, FString, Message);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FChannelMembersAsyncResult, FCrowdyChannelMembersResult, Members, FCrowdyChannelError, Error, FString, Message);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FChannelRoleAsyncResult, FCrowdyChannelRole, Role, FCrowdyChannelError, Error, FString, Message);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FChannelRolesAsyncResult, FCrowdyChannelRolesResult, Roles, FCrowdyChannelError, Error, FString, Message);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FChannelPolicyAsyncResult, FCrowdyChannelPolicy, Policy, FCrowdyChannelError, Error, FString, Message);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FChannelAsyncOnError, FCrowdyChannelError, Error, FString, Message);


UCLASS()
class CROWDYSERVICES_API UCrowdyChannels_GetMyChannels : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FMyChannelsAsyncResult OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FMyChannelsAsyncResult OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Channels|Queries", DisplayName="Get My Channels")
	static UCrowdyChannels_GetMyChannels* GetMyChannels(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	UFUNCTION()
	void HandleSuccess(const TArray<FCrowdyChannelMembership>& Memberships);
	UFUNCTION()
	void HandleError(FCrowdyChannelError Error, FString Message);
};


UCLASS()
class CROWDYSERVICES_API UCrowdyChannels_GetChannel : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FChannelAsyncResult OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FChannelAsyncResult OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Channels|Queries", DisplayName="Get Channel")
	static UCrowdyChannels_GetChannel* GetChannel(UObject* WorldContextObject, int64 ChannelId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 ChannelId = 0;
	UFUNCTION()
	void HandleSuccess(FCrowdyChannel Channel);
	UFUNCTION()
	void HandleError(FCrowdyChannelError Error, FString Message);
};


UCLASS()
class CROWDYSERVICES_API UCrowdyChannels_GetChannels : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FChannelsAsyncResult OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FChannelsAsyncResult OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Channels|Queries", DisplayName="Get All Channels")
	static UCrowdyChannels_GetChannels* GetChannels(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	UFUNCTION()
	void HandleSuccess(const TArray<FCrowdyChannel>& Channels);
	UFUNCTION()
	void HandleError(FCrowdyChannelError Error, FString Message);
};


UCLASS()
class CROWDYSERVICES_API UCrowdyChannels_GetChannelMembers : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FChannelMembersAsyncResult OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FChannelMembersAsyncResult OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Channels|Queries", DisplayName="Get Channel Members")
	static UCrowdyChannels_GetChannelMembers* GetChannelMembers(UObject* WorldContextObject, int64 ChannelId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 ChannelId = 0;
	UFUNCTION()
	void HandleSuccess(const TArray<FCrowdyChannelMember>& Members);
	UFUNCTION()
	void HandleError(FCrowdyChannelError Error, FString Message);
};


UCLASS()
class CROWDYSERVICES_API UCrowdyChannels_GetChannelRoles : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FChannelRolesAsyncResult OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FChannelRolesAsyncResult OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Channels|Queries", DisplayName="Get Channel Roles")
	static UCrowdyChannels_GetChannelRoles* GetChannelRoles(UObject* WorldContextObject, int64 ChannelId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 ChannelId = 0;
	UFUNCTION()
	void HandleSuccess(const TArray<FCrowdyChannelRole>& Roles);
	UFUNCTION()
	void HandleError(FCrowdyChannelError Error, FString Message);
};


UCLASS()
class CROWDYSERVICES_API UCrowdyChannels_GetChannelPolicy : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FChannelPolicyAsyncResult OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FChannelPolicyAsyncResult OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Channels|Queries", DisplayName="Get Channel Policy")
	static UCrowdyChannels_GetChannelPolicy* GetChannelPolicy(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	UFUNCTION()
	void HandleSuccess(FCrowdyChannelPolicy Policy);
	UFUNCTION()
	void HandleError(FCrowdyChannelError Error, FString Message);
};


UCLASS()
class CROWDYSERVICES_API UCrowdyChannels_GetPendingJoinRequests : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FChannelMembersAsyncResult OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FChannelMembersAsyncResult OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Channels|Queries", DisplayName="Get Pending Join Requests")
	static UCrowdyChannels_GetPendingJoinRequests* GetPendingJoinRequests(UObject* WorldContextObject, int64 ChannelId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 ChannelId = 0;
	UFUNCTION()
	void HandleSuccess(const TArray<FCrowdyChannelMember>& Members);
	UFUNCTION()
	void HandleError(FCrowdyChannelError Error, FString Message);
};
