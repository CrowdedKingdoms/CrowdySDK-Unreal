#pragma once

#include "Kismet/BlueprintAsyncActionBase.h"
#include "Queries/Data/Avatar/Types/FCrowdyAvatar.h"
#include "Queries/Data/Avatar/Types/FCrowdyAppAvatarState.h"
#include "Queries/Data/Avatar/Types/FCrowdyAvatarError.h"
#include "CrowdyAvatarsQueryActions.generated.h"

// TArray<> cannot be used directly as a dynamic multicast delegate parameter —
// Blueprint's event generator produces incorrect property flags for array params,
// causing persistent "Signature Error" compilation failures. Wrap in a struct.

USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyAvatarList
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category="Crowdy")
	TArray<FCrowdyAvatar> Avatars;
};

USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyAppAvatarStateList
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category="Crowdy")
	TArray<FCrowdyAppAvatarState> AppStates;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FAvatarAsyncOnSuccess, FCrowdyAvatar, Avatar);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FAvatarsAsyncOnSuccess, FCrowdyAvatarList, Result);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FAppStateAsyncOnSuccess, FCrowdyAppAvatarState, AppState);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FAppStatesAsyncOnSuccess, FCrowdyAppAvatarStateList, Result);

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FAvatarVoidAsyncOnSuccess);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FAvatarsAsyncOnError, FCrowdyAvatarError, Error, FString, Message);

UCLASS()
class CROWDYSERVICES_API UCrowdyAvatars_GetMyAvatars : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FAvatarsAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FAvatarsAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Avatars|Queries", DisplayName="Get My Avatars")
	static UCrowdyAvatars_GetMyAvatars* GetMyAvatars(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	UFUNCTION()
	void HandleSuccess(TArray<FCrowdyAvatar> Avatars);
	UFUNCTION()
	void HandleError(FCrowdyAvatarError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyAvatars_GetAvatar : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FAvatarAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FAvatarsAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Avatars|Queries", DisplayName="Get Avatar")
	static UCrowdyAvatars_GetAvatar* GetAvatar(UObject* WorldContextObject, int64 AvatarId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 AvatarId = 0;
	UFUNCTION()
	void HandleSuccess(FCrowdyAvatar Avatar);
	UFUNCTION()
	void HandleError(FCrowdyAvatarError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyAvatars_GetUserAvatars : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FAvatarsAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FAvatarsAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Avatars|Queries", DisplayName="Get User Avatars")
	static UCrowdyAvatars_GetUserAvatars* GetUserAvatars(UObject* WorldContextObject, int64 UserId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 UserId = 0;
	UFUNCTION()
	void HandleSuccess(TArray<FCrowdyAvatar> Avatars);
	UFUNCTION()
	void HandleError(FCrowdyAvatarError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyAvatars_GetAvatarAppState : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FAppStateAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FAvatarsAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Avatars|Queries", DisplayName="Get Avatar App State")
	static UCrowdyAvatars_GetAvatarAppState* GetAvatarAppState(UObject* WorldContextObject, int64 AvatarId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 AvatarId = 0;
	UFUNCTION()
	void HandleSuccess(FCrowdyAppAvatarState AppState);
	UFUNCTION()
	void HandleError(FCrowdyAvatarError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyAvatars_GetAvatarAppStates : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FAppStatesAsyncOnSuccess OnSuccess;
	UPROPERTY(BlueprintAssignable)
	FAvatarsAsyncOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Avatars|Queries", DisplayName="Get Avatar App States")
	static UCrowdyAvatars_GetAvatarAppStates* GetAvatarAppStates(UObject* WorldContextObject,
	                                                             const TArray<int64>& AvatarIds);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	TArray<int64> AvatarIds;
	UFUNCTION()
	void HandleSuccess(TArray<FCrowdyAppAvatarState> AppStates);
	UFUNCTION()
	void HandleError(FCrowdyAvatarError Error, FString Message);
};
