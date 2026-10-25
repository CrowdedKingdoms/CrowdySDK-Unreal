#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Subsystem/CrowdyAuthentication.h"
#include "Subsystem/AsyncActions/CrowdyAuthenticationActions.h"
#include "CrowdyAccountActions.generated.h"

// One signature serves On Success and On Error, because a Blueprint async node builds its data pins from the first delegate only.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FAccountAsyncResult, FCrowdyAccountError, Error, FString, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FAccessAsyncResult, FCrowdyAppAccess, Access, FCrowdyAccountError, Error, FString, Message);

/** The pins and answers every account node shares. */
UCLASS(Abstract)
class CROWDYSERVICES_API UCrowdyAccountAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FAccountAsyncResult OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FAccountAsyncResult OnError;

	virtual void Activate() override;

protected:
	TWeakObjectPtr<UObject> WorldContextObject;

	/** Sends the call this node stands for. */
	virtual void Send(UCrowdyAuthentication& Auth, const FOnAccountDone& Done, const FOnAccountError& Failed) {}

	UFUNCTION() void HandleDone();
	UFUNCTION() void HandleError(FCrowdyAccountError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyAccount_RequestPasswordReset : public UCrowdyAccountAction
{
	GENERATED_BODY()

public:
	/** Emails a password-reset link. Works signed out; succeeds whether or not Email has an account. */
	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Account", DisplayName="Request Password Reset")
	static UCrowdyAccount_RequestPasswordReset* RequestPasswordReset(UObject* WorldContextObject, const FString& Email);

protected:
	virtual void Send(UCrowdyAuthentication& Auth, const FOnAccountDone& Done, const FOnAccountError& Failed) override;

private:
	FString Email;
};

UCLASS()
class CROWDYSERVICES_API UCrowdyAccount_ResendConfirmationEmail : public UCrowdyAccountAction
{
	GENERATED_BODY()

public:
	/** Emails the address confirmation again. Works signed out; succeeds whether or not Email has an account. */
	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Account", DisplayName="Resend Confirmation Email")
	static UCrowdyAccount_ResendConfirmationEmail* ResendConfirmationEmail(UObject* WorldContextObject, const FString& Email);

protected:
	virtual void Send(UCrowdyAuthentication& Auth, const FOnAccountDone& Done, const FOnAccountError& Failed) override;

private:
	FString Email;
};

UCLASS()
class CROWDYSERVICES_API UCrowdyAccount_ChangePassword : public UCrowdyAccountAction
{
	GENERATED_BODY()

public:
	/** Changes the signed-in player's password and signs out their other devices. */
	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Account", DisplayName="Change Password")
	static UCrowdyAccount_ChangePassword* ChangePassword(UObject* WorldContextObject, const FString& CurrentPassword,
		const FString& NewPassword);

protected:
	virtual void Send(UCrowdyAuthentication& Auth, const FOnAccountDone& Done, const FOnAccountError& Failed) override;

private:
	friend class FCrowdyAccountPasswordIsClearedAfterSendTest;

	FString CurrentPassword;
	FString NewPassword;
};

UCLASS()
class CROWDYSERVICES_API UCrowdyAccount_AddPassword : public UCrowdyAccountAction
{
	GENERATED_BODY()

public:
	/** Adds a password to an account made by magic link or social sign-in. */
	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Account", DisplayName="Add Password")
	static UCrowdyAccount_AddPassword* AddPassword(UObject* WorldContextObject, const FString& NewPassword);

protected:
	virtual void Send(UCrowdyAuthentication& Auth, const FOnAccountDone& Done, const FOnAccountError& Failed) override;

private:
	friend class FCrowdyAccountPasswordIsClearedAfterSendTest;

	FString NewPassword;
};

UCLASS()
class CROWDYSERVICES_API UCrowdyAccount_DeleteAccount : public UCrowdyAccountAction
{
	GENERATED_BODY()

public:
	/** Deletes the signed-in player's account and signs out. Cannot be undone, so confirm with the player first. */
	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Account", DisplayName="Delete Account")
	static UCrowdyAccount_DeleteAccount* DeleteAccount(UObject* WorldContextObject);

protected:
	virtual void Send(UCrowdyAuthentication& Auth, const FOnAccountDone& Done, const FOnAccountError& Failed) override;
};

UCLASS()
class CROWDYSERVICES_API UCrowdyAccount_SignOutEverywhere : public UCrowdyAccountAction
{
	GENERATED_BODY()

public:
	/** Ends every session of the signed-in player on every device, this one included. */
	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Account", DisplayName="Sign Out Everywhere")
	static UCrowdyAccount_SignOutEverywhere* SignOutEverywhere(UObject* WorldContextObject);

protected:
	virtual void Send(UCrowdyAuthentication& Auth, const FOnAccountDone& Done, const FOnAccountError& Failed) override;
};

UCLASS()
class CROWDYSERVICES_API UCrowdyAccount_GetMyAccess : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FAccessAsyncResult OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FAccessAsyncResult OnError;

	/** Reads the signed-in player's access to this app, also while the app refuses them. */
	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Account", DisplayName="Get My Access")
	static UCrowdyAccount_GetMyAccess* GetMyAccess(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;

	UFUNCTION() void HandleRead(FCrowdyAppAccess Access);
	UFUNCTION() void HandleError(FCrowdyAccountError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyAccount_ClaimFreeAccess : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FAuthAsyncResult OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FAuthAsyncResult OnError;

	/** Claims this app's free access for a player sign-in refused for having none, then finishes signing in. */
	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Account", DisplayName="Claim Free Access")
	static UCrowdyAccount_ClaimFreeAccess* ClaimFreeAccess(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;

	UFUNCTION() void HandleSuccess(FCrowdyAuthResult Result);
	UFUNCTION() void HandleError(FString Message);
};
