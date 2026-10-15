#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Subsystem/CrowdyAuthentication.h"
#include "CrowdyAuthenticationActions.generated.h"

// One signature serves On Success and On Error, because a Blueprint async node builds its data pins from the first delegate only.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FAuthAsyncResult, FCrowdyAuthResult, Result, FString, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FRecordPlayerConsentsAsyncResult, FString, Message);

UCLASS()
class CROWDYSERVICES_API UCrowdyAuth_Login : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FAuthAsyncResult OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FAuthAsyncResult OnError;

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
	FAuthAsyncResult OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FAuthAsyncResult OnError;

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
	FAuthAsyncResult OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FAuthAsyncResult OnError;

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
 * Opens a loopback listener and emails a one-time sign-in link; fires OnSuccess once the user
 * completes it. Waits on the browser round-trip up to the runtime's magic-link timeout, then
 * OnError fires.
 */
UCLASS()
class CROWDYSERVICES_API UCrowdyAuth_BeginMagicLinkSignIn : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FAuthAsyncResult OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FAuthAsyncResult OnError;

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
	FAuthAsyncResult OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FAuthAsyncResult OnError;

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

/**
 * Records the signed-in player's acceptance of the legal documents and their age-of-majority attestation. Call it
 * only from the player's own explicit accept. A sign-in waiting on the consent then completes through its own pins.
 */
UCLASS()
class CROWDYSERVICES_API UCrowdyAuth_RecordPlayerConsents : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FRecordPlayerConsentsAsyncResult OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FRecordPlayerConsentsAsyncResult OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Authentication", DisplayName="Record Player Consents")
	static UCrowdyAuth_RecordPlayerConsents* RecordPlayerConsents(UObject* WorldContextObject, bool bAcceptLegal,
		bool bAttestAgeOfMajority);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	bool bAcceptLegal = false;
	bool bAttestAgeOfMajority = false;

	UFUNCTION() void HandleSuccess();
	UFUNCTION() void HandleError(FString Message);
};
