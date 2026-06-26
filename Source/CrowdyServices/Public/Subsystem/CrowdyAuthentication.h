#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Core/GraphQL/Interfaces/ICrowdyQueryReceptionLayer.h"
#include "Internal/FCrowdyDataRegistry.h"
#include "CrowdyAuthentication.generated.h"

class UCrowdySDKBridgeSubsystem;
class UCrowdyQuerySubsystem;
class UCrowdyGameSession;

USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyAuthResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Authentication")
	FString GameToken;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Authentication")
	int64 UserID = 0;
};

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnAuthSuccess, FCrowdyAuthResult, Result);
DECLARE_DYNAMIC_DELEGATE_OneParam(FOnAuthError, FString, Message);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnAuthLoginEvent, FCrowdyAuthResult, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnAuthLoginFailed, FString, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnAuthRegisterEvent, FCrowdyAuthResult, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnAuthRegisterFailed, FString, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnAuthSessionRestored, FCrowdyAuthResult, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnAuthSessionRestoreFailed, FString, Message);

/*
 * Owns login/register request execution, credential persistence, and result
 * broadcasting. Injected by UCrowdySDKSubsystem during initialization.
 *
 * C++ usage per-call callbacks:
 *   FOnAuthSuccess S; S.BindDynamic(this, &UMyClass::HandleSuccess);
 *   FOnAuthError E; E.BindDynamic(this, &UMyClass::HandleError);
 *   Auth->Login(Email, Password, S, E);
 *
 * C++ usage multicast observers:
 *   Auth->OnLogin.AddDynamic(this, &UMyClass::HandleAnyLogin);
 *   Auth->Login(Email, Password, {}, {});
 */
UCLASS(BlueprintType, meta=(DisplayName="Crowdy Authentication"))
class CROWDYSERVICES_API UCrowdyAuthentication : public UGameInstanceSubsystem, public ICrowdyQueryReceptionLayer
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	void InjectDependencies(FCrowdyDataRegistry* InRegistry, UCrowdyQuerySubsystem* InQuerySubsystem,
	                        UCrowdyGameSession* InGameSession);

	virtual void OnResponseReceived(TSharedPtr<ICrowdyQueryResponse> Response) override;
	virtual TArray<EQueryResponseType> GetSupportedResponseType() const override;

	/** Fires on successful login. */
	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Authentication")
	FOnAuthLoginEvent OnLogin;

	/** Fires when a login attempt fails. */
	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Authentication")
	FOnAuthLoginFailed OnLoginFailed;

	/** Fires on successful registration. */
	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Authentication")
	FOnAuthRegisterEvent OnRegister;

	/** Fires when a register attempt fails. */
	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Authentication")
	FOnAuthRegisterFailed OnRegisterFailed;

	/** Fires when a saved session is successfully restored. */
	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Authentication")
	FOnAuthSessionRestored OnSessionRestored;

	/** Fires when RestoreSession finds no saved data or the token is empty. */
	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Authentication")
	FOnAuthSessionRestoreFailed OnSessionRestoreFailed;

	/**
	 * Executes a login query. Credentials are saved to disk on success so
	 * RestoreSession can resume the session later without re-entering credentials.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Authentication")
	void Login(const FString& Email, const FString& Password,
	           FOnAuthSuccess OnSuccess, FOnAuthError OnError);

	/**
	 * Executes a register query. Credentials are saved to disk on success.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Authentication")
	void Register(const FString& Email, const FString& Password,
	              FOnAuthSuccess OnSuccess, FOnAuthError OnError);

	/**
	 * Checks for a previously saved session. If one exists, restores GameSession,
	 * sets the auth token, kicks off the UDP handshake, and fires OnSuccess /
	 * OnSessionRestored, the same flow as a fresh login.
	 * Returns false immediately (calling OnError) if no saved data is found.
	 * Call this manually on startup; it is never triggered automatically.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Authentication")
	bool RestoreSession(FOnAuthSuccess OnSuccess, FOnAuthError OnError);

	/**
	 * Deletes the saved session from disk. Call this on logout so the next
	 * startup does not attempt to restore a stale token.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Authentication")
	void ClearSavedSession();

	/**
	 * Returns true if a non-empty saved session exists on disk.
	 * Does not restore anything. Use this to decide whether to show a
	 * "Resume Session" button before calling RestoreSession.
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy SDK|Authentication")
	bool HasSavedSession() const;

private:
	UPROPERTY()
	UCrowdyQuerySubsystem* QuerySubsystem = nullptr;

	UPROPERTY()
	UCrowdyGameSession* GameSession = nullptr;

	mutable FCriticalSection CallbackMutex;
	TMap<EQueryResponseType, TArray<TFunction<void(TSharedPtr<ICrowdyQueryResponse>)>>> PendingCallbacks;

	void PushCallback(EQueryResponseType Type, TFunction<void(TSharedPtr<ICrowdyQueryResponse>)> Callback);
	void FireCallback(TSharedPtr<ICrowdyQueryResponse> Response);

	void SaveCredentials(const FString& GameToken, int64 GameTokenID, int64 UserID) const;
	void ApplySessionAndKickUDP(const FString& GameToken, int64 GameTokenID, int64 UserID);
};
