#pragma once

#include "Kismet/BlueprintAsyncActionBase.h"
#include "Subsystem/AsyncActions/CrowdyAvatarsQueryActions.h"
#include "CrowdyAvatarsWriteActions.generated.h"

UCLASS()
class CROWDYSERVICES_API UCrowdyAvatars_CreateAvatar : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FAvatarAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FAvatarsAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Avatars|Mutations", DisplayName="Create Avatar")
	static UCrowdyAvatars_CreateAvatar* CreateAvatar(UObject* WorldContextObject, const FString& Name);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString Name;
	UFUNCTION()
	void HandleSuccess(FCrowdyAvatar Avatar);
	UFUNCTION()
	void HandleError(FCrowdyAvatarError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyAvatars_UpdateAvatar : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FAvatarAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FAvatarsAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Avatars|Mutations", DisplayName="Update Avatar")
	static UCrowdyAvatars_UpdateAvatar* UpdateAvatar(UObject* WorldContextObject, int64 AvatarId, const FString& Name);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 AvatarId = 0;
	FString Name;
	UFUNCTION()
	void HandleSuccess(FCrowdyAvatar Avatar);
	UFUNCTION()
	void HandleError(FCrowdyAvatarError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyAvatars_DeleteAvatar : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FAvatarVoidAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FAvatarsAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Avatars|Mutations", DisplayName="Delete Avatar")
	static UCrowdyAvatars_DeleteAvatar* DeleteAvatar(UObject* WorldContextObject, int64 AvatarId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 AvatarId = 0;
	UFUNCTION()
	void HandleSuccess();
	UFUNCTION()
	void HandleError(FCrowdyAvatarError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyAvatars_UpdatePublicAvatarState : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FAvatarAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FAvatarsAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Avatars|Mutations", DisplayName="Update Public Avatar State")
	static UCrowdyAvatars_UpdatePublicAvatarState* UpdatePublicAvatarState(
		UObject* WorldContextObject, int64 AvatarId, const FString& RawState);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 AvatarId = 0;
	FString RawState;
	UFUNCTION()
	void HandleSuccess(FCrowdyAvatar Avatar);
	UFUNCTION()
	void HandleError(FCrowdyAvatarError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyAvatars_UpdatePrivateAvatarState : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FAvatarAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FAvatarsAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Avatars|Mutations", DisplayName="Update Private Avatar State")
	static UCrowdyAvatars_UpdatePrivateAvatarState* UpdatePrivateAvatarState(
		UObject* WorldContextObject, int64 AvatarId, const FString& RawState);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 AvatarId = 0;
	FString RawState;
	UFUNCTION()
	void HandleSuccess(FCrowdyAvatar Avatar);
	UFUNCTION()
	void HandleError(FCrowdyAvatarError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyAvatars_UpdateAvatarAppState : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FAppStateAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FAvatarsAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Avatars|Mutations", DisplayName="Update Avatar App State")
	static UCrowdyAvatars_UpdateAvatarAppState* UpdateAvatarAppState(
		UObject* WorldContextObject, int64 AvatarId, const FString& RawState);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 AvatarId = 0;
	FString RawState;
	UFUNCTION()
	void HandleSuccess(FCrowdyAppAvatarState AppState);
	UFUNCTION()
	void HandleError(FCrowdyAvatarError Error, FString Message);
};
