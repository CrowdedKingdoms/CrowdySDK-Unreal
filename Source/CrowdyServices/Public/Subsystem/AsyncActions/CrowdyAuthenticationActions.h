#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Subsystem/CrowdyAuthentication.h"
#include "CrowdyAuthenticationActions.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLoginDelegateOnSuccess, FCrowdyAuthResult, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLoginDelegateOnError, FString, Message);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FRegisterDelegateOnSuccess, FCrowdyAuthResult, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FRegisterDelegateOnFailure, FString, Message);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FRestoreSessionDelegateOnSuccess, FCrowdyAuthResult, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FRestoreSessionDelegateOnError, FString, Message);

UCLASS()
class CROWDYSERVICES_API UCrowdyAuth_Login : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FLoginDelegateOnSuccess OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FLoginDelegateOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Authentication", DisplayName="Login")
	static UCrowdyAuth_Login* Login(UObject* WorldContextObject, const FString& Email, const FString& Password);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString Email;
	FString Password;

	UFUNCTION() void HandleSuccess(FCrowdyAuthResult Result);
	UFUNCTION() void HandleError(FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyAuth_Register : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FRegisterDelegateOnSuccess OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FRegisterDelegateOnFailure OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Authentication", DisplayName="Register")
	static UCrowdyAuth_Register* Register(UObject* WorldContextObject, const FString& Email, const FString& Password);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString Email;
	FString Password;

	UFUNCTION() void HandleSuccess(FCrowdyAuthResult Result);
	UFUNCTION() void HandleError(FString Message);
};

/**
 * Checks for a saved session and restores it if one exists, triggering the
 * same UDP handshake flow as a fresh login. No credentials required.
 * OnError fires immediately if no saved session is found.
 */
UCLASS()
class CROWDYSERVICES_API UCrowdyAuth_RestoreSession : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FRestoreSessionDelegateOnSuccess OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FRestoreSessionDelegateOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Authentication", DisplayName="Restore Session")
	static UCrowdyAuth_RestoreSession* RestoreSession(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;

	UFUNCTION() void HandleSuccess(FCrowdyAuthResult Result);
	UFUNCTION() void HandleError(FString Message);
};
