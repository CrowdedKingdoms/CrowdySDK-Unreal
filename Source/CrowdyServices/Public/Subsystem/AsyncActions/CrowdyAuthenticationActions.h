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

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDevLoginDelegateOnSuccess, FCrowdyAuthResult, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDevLoginDelegateOnError, FString, Message);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FBeginMagicLinkSignInDelegateOnSuccess, FCrowdyAuthResult, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FBeginMagicLinkSignInDelegateOnError, FString, Message);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FBeginSocialSignInDelegateOnSuccess, FCrowdyAuthResult, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FBeginSocialSignInDelegateOnError, FString, Message);

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

/**
 * Passwordless sign-in using only an email, no browser step. Only works against a dev server
 * (DEV_AUTH_BYPASS); a production server rejects it with FORBIDDEN, surfaced via OnError.
 */
UCLASS()
class CROWDYSERVICES_API UCrowdyAuth_DevLogin : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FDevLoginDelegateOnSuccess OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FDevLoginDelegateOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Authentication", DisplayName="Dev Login")
	static UCrowdyAuth_DevLogin* DevLogin(UObject* WorldContextObject, const FString& Email);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString Email;

	UFUNCTION() void HandleSuccess(FCrowdyAuthResult Result);
	UFUNCTION() void HandleError(FString Message);
};

/**
 * Opens a loopback listener and emails a one-time sign-in link (or, on a dev server, completes
 * immediately with no email); fires OnSuccess once the user completes it. Waits on the browser
 * round-trip up to the runtime's magic-link timeout, then OnError fires.
 */
UCLASS()
class CROWDYSERVICES_API UCrowdyAuth_BeginMagicLinkSignIn : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FBeginMagicLinkSignInDelegateOnSuccess OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FBeginMagicLinkSignInDelegateOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Authentication", DisplayName="Magic Link Sign In")
	static UCrowdyAuth_BeginMagicLinkSignIn* BeginMagicLinkSignIn(UObject* WorldContextObject, const FString& Email);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString Email;

	UFUNCTION() void HandleSuccess(FCrowdyAuthResult Result);
	UFUNCTION() void HandleError(FString Message);
};

/**
 * Opens Provider's consent page in the system browser and fires OnSuccess once the redirect
 * completes on the loopback listener. Get valid Provider values from
 * UCrowdyAuthentication::GetAvailableLoginProviders.
 */
UCLASS()
class CROWDYSERVICES_API UCrowdyAuth_BeginSocialSignIn : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FBeginSocialSignInDelegateOnSuccess OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FBeginSocialSignInDelegateOnError OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Authentication", DisplayName="Social Sign In")
	static UCrowdyAuth_BeginSocialSignIn* BeginSocialSignIn(UObject* WorldContextObject, const FString& Provider);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString Provider;

	UFUNCTION() void HandleSuccess(FCrowdyAuthResult Result);
	UFUNCTION() void HandleError(FString Message);
};
