#pragma once

#include "Kismet/BlueprintAsyncActionBase.h"
#include "Subsystem/AsyncActions/CrowdyChannelsQueryActions.h"
#include "Queries/Data/Channels/Enums/ECrowdyChannelMembershipPolicy.h"
#include "Queries/Data/Channels/Enums/ECrowdyChannelCreationPolicy.h"
#include "Queries/Data/Channels/Types/FCrowdyChannelPermissions.h"
#include "CrowdyChannelsWriteActions.generated.h"

UCLASS()
class CROWDYSERVICES_API UCrowdyChannels_CreateChannel : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FChannelAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FChannelAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Channels|Mutations", DisplayName="Create Channel")
	static UCrowdyChannels_CreateChannel* CreateChannel(UObject* WorldContextObject, const FString& Name,
	                                                    const FString& Description,
	                                                    ECrowdyChannelMembershipPolicy MembershipPolicy,
	                                                    bool bMembersCanSend);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString Name, Description;
	ECrowdyChannelMembershipPolicy MembershipPolicy = ECrowdyChannelMembershipPolicy::Open;
	bool bMembersCanSend = true;
	UFUNCTION()
	void HandleSuccess(FCrowdyChannel Channel);
	UFUNCTION()
	void HandleError(FCrowdyChannelError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyChannels_UpdateChannel : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FChannelAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FChannelAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Channels|Mutations", DisplayName="Update Channel")
	static UCrowdyChannels_UpdateChannel* UpdateChannel(UObject* WorldContextObject, int64 ChannelId,
	                                                    const FString& Name, const FString& Description);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 ChannelId = 0;
	FString Name, Description;
	UFUNCTION()
	void HandleSuccess(FCrowdyChannel Channel);
	UFUNCTION()
	void HandleError(FCrowdyChannelError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyChannels_DeleteChannel : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FChannelVoidAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FChannelAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Channels|Mutations", DisplayName="Delete Channel")
	static UCrowdyChannels_DeleteChannel* DeleteChannel(UObject* WorldContextObject, int64 ChannelId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 ChannelId = 0;
	UFUNCTION()
	void HandleSuccess();
	UFUNCTION()
	void HandleError(FCrowdyChannelError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyChannels_JoinChannel : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FChannelMemberAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FChannelAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Channels|Mutations", DisplayName="Join Channel")
	static UCrowdyChannels_JoinChannel* JoinChannel(UObject* WorldContextObject, int64 ChannelId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 ChannelId = 0;
	UFUNCTION()
	void HandleSuccess(FCrowdyChannelMember Member);
	UFUNCTION()
	void HandleError(FCrowdyChannelError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyChannels_RequestToJoinChannel : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FChannelMemberAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FChannelAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Channels|Mutations", DisplayName="Request to Join Channel")
	static UCrowdyChannels_RequestToJoinChannel* RequestToJoinChannel(UObject* WorldContextObject, int64 ChannelId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 ChannelId = 0;
	UFUNCTION()
	void HandleSuccess(FCrowdyChannelMember Member);
	UFUNCTION()
	void HandleError(FCrowdyChannelError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyChannels_LeaveChannel : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FChannelVoidAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FChannelAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Channels|Mutations", DisplayName="Leave Channel")
	static UCrowdyChannels_LeaveChannel* LeaveChannel(UObject* WorldContextObject, int64 ChannelId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 ChannelId = 0;
	UFUNCTION()
	void HandleSuccess();
	UFUNCTION()
	void HandleError(FCrowdyChannelError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyChannels_AddChannelMember : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FChannelMemberAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FChannelAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Channels|Mutations", DisplayName="Add Channel Member")
	static UCrowdyChannels_AddChannelMember* AddChannelMember(UObject* WorldContextObject, int64 ChannelId, int64 UserId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 ChannelId = 0, UserId = 0;
	UFUNCTION()
	void HandleSuccess(FCrowdyChannelMember Member);
	UFUNCTION()
	void HandleError(FCrowdyChannelError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyChannels_RemoveChannelMember : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FChannelVoidAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FChannelAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Channels|Mutations", DisplayName="Remove Channel Member")
	static UCrowdyChannels_RemoveChannelMember* RemoveChannelMember(UObject* WorldContextObject, int64 ChannelId,
	                                                                int64 UserId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 ChannelId = 0, UserId = 0;
	UFUNCTION()
	void HandleSuccess();
	UFUNCTION()
	void HandleError(FCrowdyChannelError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyChannels_SetChannelMemberRoles : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FChannelMemberAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FChannelAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Channels|Mutations", DisplayName="Set Channel Member Roles")
	static UCrowdyChannels_SetChannelMemberRoles* SetChannelMemberRoles(UObject* WorldContextObject, int64 ChannelId,
	                                                                    int64 UserId, const TArray<int64>& RoleIds);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 ChannelId = 0, UserId = 0;
	TArray<int64> RoleIds;
	UFUNCTION()
	void HandleSuccess(FCrowdyChannelMember Member);
	UFUNCTION()
	void HandleError(FCrowdyChannelError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyChannels_CreateChannelRole : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FChannelRoleAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FChannelAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Channels|Mutations", DisplayName="Create Channel Role")
	static UCrowdyChannels_CreateChannelRole* CreateChannelRole(UObject* WorldContextObject, int64 ChannelId,
	                                                            const FString& RoleName,
	                                                            FCrowdyChannelPermissions Permissions, int32 Rank);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 ChannelId = 0;
	FString RoleName;
	FCrowdyChannelPermissions Permissions;
	int32 Rank = 0;
	UFUNCTION()
	void HandleSuccess(FCrowdyChannelRole Role);
	UFUNCTION()
	void HandleError(FCrowdyChannelError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyChannels_UpdateChannelRole : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FChannelRoleAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FChannelAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Channels|Mutations", DisplayName="Update Channel Role")
	static UCrowdyChannels_UpdateChannelRole* UpdateChannelRole(UObject* WorldContextObject, int64 ChannelRoleId,
	                                                            const FString& RoleName,
	                                                            FCrowdyChannelPermissions Permissions);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 ChannelRoleId = 0;
	FString RoleName;
	FCrowdyChannelPermissions Permissions;
	UFUNCTION()
	void HandleSuccess(FCrowdyChannelRole Role);
	UFUNCTION()
	void HandleError(FCrowdyChannelError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyChannels_DeleteChannelRole : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FChannelVoidAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FChannelAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Channels|Mutations", DisplayName="Delete Channel Role")
	static UCrowdyChannels_DeleteChannelRole* DeleteChannelRole(UObject* WorldContextObject, int64 ChannelRoleId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 ChannelRoleId = 0;
	UFUNCTION()
	void HandleSuccess();
	UFUNCTION()
	void HandleError(FCrowdyChannelError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyChannels_SetChannelPolicy : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FChannelPolicyAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FChannelAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Channels|Mutations", DisplayName="Set Channel Policy")
	static UCrowdyChannels_SetChannelPolicy* SetChannelPolicy(UObject* WorldContextObject,
	                                                          ECrowdyChannelCreationPolicy CreationPolicy,
	                                                          ECrowdyChannelMembershipPolicy DefaultMembershipPolicy);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	ECrowdyChannelCreationPolicy CreationPolicy = ECrowdyChannelCreationPolicy::Anyone;
	ECrowdyChannelMembershipPolicy MembershipPolicy = ECrowdyChannelMembershipPolicy::Open;
	UFUNCTION()
	void HandleSuccess(FCrowdyChannelPolicy Policy);
	UFUNCTION()
	void HandleError(FCrowdyChannelError Error, FString Message);
};
