#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Templates/PimplPtr.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Core/GraphQL/Interfaces/ICrowdyQueryReceptionLayer.h"
#include "Internal/FCrowdyDataRegistry.h"
#include "Queries/Authentication/FCrowdyUserIdentity.h"
#include "CrowdyAuthentication.generated.h"

class UCrowdySDKBridgeSubsystem;
class UCrowdyQuerySubsystem;
class UCrowdyGameSession;
class UCrowdyAuthSaveGame;
class FCrowdyLoopbackAuthServer;
struct FAuthResponseBase;
struct FAppTokenResponseBase;

USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyAuthResult
{
	GENERATED_BODY()

	/** Identity SESSION token (management-plane). The SDK stores it; gameplay uses
	 *  the app-scoped token minted from it, not this. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Authentication")
	FString GameToken;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Authentication")
	int64 UserID = 0;
};

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnAuthSuccess, FCrowdyAuthResult, Result);
DECLARE_DYNAMIC_DELEGATE_OneParam(FOnAuthError, FString, Message);
DECLARE_DYNAMIC_DELEGATE_TwoParams(FOnLoginLinkSent, bool, bSent, FString, DevToken);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnAuthLoginEvent, FCrowdyAuthResult, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnAuthLoginFailed, FString, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnAuthRegisterEvent, FCrowdyAuthResult, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnAuthRegisterFailed, FString, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnAuthSessionRestored, FCrowdyAuthResult, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnAuthSessionRestoreFailed, FString, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnAppTokenRefreshed);

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnLoginProvidersReceived, const TArray<FString>&, Providers);
DECLARE_DYNAMIC_DELEGATE_OneParam(FOnIdentitiesReceived, const TArray<FCrowdyUserIdentity>&, Identities);
DECLARE_DYNAMIC_DELEGATE_OneParam(FOnIdentityLinked, FCrowdyUserIdentity, Identity);
DECLARE_DYNAMIC_DELEGATE_OneParam(FOnIdentityUnlinked, bool, bRemoved);

/*
 * Owns sign-in, the app-token lifecycle, and session persistence. Injected by
 * UCrowdySDKSubsystem during initialization.
 *
 * Crowded Kingdoms uses a two-token model: a sign-in returns an identity SESSION
 * token (management-plane), from which a short-lived app-scoped GAMEPLAY token is
 * minted. Every sign-in method below password Login/Register, DevLogin, and the
 * magic-link CompleteLoginLink converges on ONE pipeline:
 *
 *   sign-in -> store SESSION token -> mintAppToken -> store APP token + adopt the
 *   per-app Game endpoints -> fire the success delegate (the SDK then requests UDP
 *   access with the APP token).
 *
 * The SESSION token is the only thing persisted; the APP token stays in memory and
 * is refreshed proactively (before expiry) and reactively (on UDP TOKEN_EXPIRED).
 *
 * For Blueprint, prefer the latent UCrowdyAuth_* nodes (Subsystem/AsyncActions/CrowdyAuthenticationActions.h)
 * over wiring the FOnAuthSuccess/FOnAuthError delegate params below by hand — same calls underneath,
 * single node with Success/Error exec pins.
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

	/** Fires on successful sign-in (password login, dev login, or magic-link). */
	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Authentication")
	FOnAuthLoginEvent OnLogin;

	/** Fires when a sign-in attempt fails (sign-in or its app-token mint). */
	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Authentication")
	FOnAuthLoginFailed OnLoginFailed;

	/** Fires on successful registration (account created + signed in). */
	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Authentication")
	FOnAuthRegisterEvent OnRegister;

	/** Fires when a register attempt fails. */
	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Authentication")
	FOnAuthRegisterFailed OnRegisterFailed;

	/** Fires when a saved session is successfully restored (and re-minted). */
	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Authentication")
	FOnAuthSessionRestored OnSessionRestored;

	/** Fires when RestoreSession finds no saved data or the token is empty. */
	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Authentication")
	FOnAuthSessionRestoreFailed OnSessionRestoreFailed;

	/** Fires after the app-scoped token is rotated (proactive timer or reactive
	 *  recovery). The SDK requests UDP access, so the new token re-assigns the
	 *  Buddy session. */
	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Authentication")
	FOnAppTokenRefreshed OnAppTokenRefreshed;

	/**
	 * Password sign-in. One option among the passwordless methods; returns the
	 * same SESSION token and feeds the same mint pipeline. Persists the SESSION
	 * token on success so RestoreSession can resume later.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Authentication")
	void Login(const FString& Email, const FString& Password,
	           FOnAuthSuccess OnSuccess, FOnAuthError OnError);

	/**
	 * Password registration. Creates the account and signs in; feeds the mint
	 * pipeline exactly like Login.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Authentication")
	void Register(const FString& Email, const FString& Password,
	              FOnAuthSuccess OnSuccess, FOnAuthError OnError);

	/**
	 * Dev-bypass sign-in (DEV_AUTH_BYPASS server only; FORBIDDEN in production).
	 * Returns a SESSION token for an email with no verification, then mints.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Authentication")
	void DevLogin(const FString& Email, FOnAuthSuccess OnSuccess, FOnAuthError OnError);

	/**
	 * Magic-link step 1: email a one-time sign-in link. OnLinkSent reports sent
	 * (always true; no account enumeration) and, in dev, the devToken to pass
	 * straight to CompleteLoginLink. RedirectUri is the native loopback the OS
	 * hands back to; leave empty to use the server default.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Authentication")
	void RequestLoginLink(const FString& Email, const FString& RedirectUri,
	                      FOnLoginLinkSent OnLinkSent, FOnAuthError OnError);

	/**
	 * Magic-link step 2: complete sign-in with the one-time token from the link
	 * (or the devToken in dev). Feeds the mint pipeline.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Authentication")
	void CompleteLoginLink(const FString& Token, FOnAuthSuccess OnSuccess, FOnAuthError OnError);

	/**
	 * One-call magic-link sign-in. Opens a loopback listener on 127.0.0.1, requests the email
	 * link with that loopback as the redirect, and completes automatically when the user clicks
	 * the link (its redirect lands on the listener). In dev the server returns a devToken that
	 * short-circuits the email/browser entirely. OnSuccess fires once signed in; OnError on
	 * failure or if the user never returns (timeout). For granular control use RequestLoginLink +
	 * CompleteLoginLink directly.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Authentication")
	void BeginMagicLinkSignIn(const FString& Email, FOnAuthSuccess OnSuccess, FOnAuthError OnError);

	/**
	 * Query the enabled federated sign-in providers (availableLoginProviders). Public;
	 * use this to build the sign-in UI (which social buttons to show) instead of
	 * hard-coding providers. The dev mock provider appears only under the server bypass.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Authentication")
	void GetAvailableLoginProviders(FOnLoginProvidersReceived OnResult, FOnAuthError OnError);

	/**
	 * One-call social (OAuth) sign-in. Opens a loopback listener, calls socialLoginStart to get the
	 * provider authorize URL + CSRF state, opens that URL in the browser, and completes automatically
	 * when the provider redirects back to the listener (socialLoginComplete -> the shared mint
	 * pipeline). Provider comes from GetAvailableLoginProviders (e.g. "google"). Result on OnLogin.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Authentication")
	void BeginSocialSignIn(const FString& Provider, FOnAuthSuccess OnSuccess, FOnAuthError OnError);

	/**
	 * List the signed-in user's linked sign-in identities (myIdentities). Requires an active session.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Authentication")
	void GetMyIdentities(FOnIdentitiesReceived OnResult, FOnAuthError OnError);

	/**
	 * Link an additional social identity to the signed-in account. Runs the same loopback/browser
	 * flow as BeginSocialSignIn, then calls linkIdentity (it does NOT start a new session). Requires
	 * an active session to attach the identity to.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Authentication")
	void BeginLinkIdentity(const FString& Provider, FOnIdentityLinked OnResult, FOnAuthError OnError);

	/**
	 * Unlink a federated identity by identityId (from GetMyIdentities). The server refuses to remove
	 * the last remaining sign-in method. Requires an active session.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Authentication")
	void UnlinkIdentity(const FString& IdentityId, FOnIdentityUnlinked OnResult, FOnAuthError OnError);

	/**
	 * Rotate the app-scoped token for the current app (refreshAppToken). Called
	 * automatically before expiry; exposed for manual use. On failure it falls
	 * back to re-minting from the SESSION token.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Authentication")
	void RefreshAppToken();

	/**
	 * Re-mint the app token from the stored SESSION token and re-assign. Called by
	 * the SDK when the server reports UDP TOKEN_EXPIRED (error 32).
	 */
	void RecoverExpiredAppToken();

	/**
	 * Checks for a previously saved session. If one exists, restores the SESSION
	 * token, re-mints an app token, and fires OnSessionRestored (the SDK then
	 * requests UDP access). Returns false immediately (calling OnError) if no
	 * saved data is found. Call this manually on startup.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Authentication")
	bool RestoreSession(FOnAuthSuccess OnSuccess, FOnAuthError OnError);

	/**
	 * Deletes the saved session from disk and cancels the refresh timer. Call this
	 * on logout so the next startup does not attempt to restore a stale token.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Authentication")
	void ClearSavedSession();

	/**
	 * Returns true if a non-empty saved session exists on disk.
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy SDK|Authentication")
	bool HasSavedSession() const;

private:
	/** Which sign-in started a mint, so the pipeline knows which delegate to fire. */
	enum class EAuthFlow : uint8
	{
		Login,
		Register,
		MagicLink,
		Social,
		DevLogin,
		Restore,
		Refresh,   // proactively rotate / reactive recovery no per-call delegate
	};

	UPROPERTY()
	UCrowdyQuerySubsystem* QuerySubsystem = nullptr;

	UPROPERTY()
	UCrowdyGameSession* GameSession = nullptr;

	/** Loopback HTTP listener for the magic-link / social redirect, created on first use.
	 *  TPimplPtr so a forward-declared type works as a UObject member the deleter is captured by
	 *  MakePimpl in the .cpp where the type is complete, so the UHT-generated special members never
	 *  delete an incomplete type. */
	TPimplPtr<FCrowdyLoopbackAuthServer> LoopbackServer;

	mutable FCriticalSection CallbackMutex;
	TMap<EQueryResponseType, TArray<TFunction<void(TSharedPtr<ICrowdyQueryResponse>)>>> PendingCallbacks;

	FTSTicker::FDelegateHandle RefreshTickerHandle;

	void PushCallback(EQueryResponseType Type, TFunction<void(TSharedPtr<ICrowdyQueryResponse>)> Callback);
	void FireCallback(TSharedPtr<ICrowdyQueryResponse> Response);

	/** Stage 1: store the SESSION token, persist it, and kick off the app-token mint. */
	void BeginMintPipeline(const FString& SessionToken, int64 SessionGameTokenID, int64 UserID,
	                       EAuthFlow Flow, FOnAuthSuccess OnSuccess, FOnAuthError OnError);

	/** Dispatch mintAppToken (bearer = SESSION token) for the current AppID. */
	void DispatchMint(EAuthFlow Flow, FOnAuthSuccess OnSuccess, FOnAuthError OnError);

	/** Dispatch refreshAppToken (bearer = current APP token). */
	void DispatchRefresh();

	/** Stage 2: store the APP token + per-app endpoints, schedule refresh, fire success. */
	void ApplyAppTokenAndFinish(const FAppTokenResponseBase& Token, EAuthFlow Flow, FOnAuthSuccess OnSuccess);

	/** Fire the failure delegate that matches Flow. */
	void FailFlow(const FString& Message, EAuthFlow Flow, FOnAuthError OnError);

	/** Social step 2: socialLoginComplete -> the shared mint pipeline (EAuthFlow::Social -> OnLogin). */
	void CompleteSocialLogin(const FString& Provider, const FString& Code, const FString& State,
	                         FOnAuthSuccess OnSuccess, FOnAuthError OnError);

	/** Link step 2: linkIdentity with the captured code+state (attaches to the current session; no mint). */
	void CompleteLinkIdentity(const FString& Provider, const FString& Code, const FString& State,
	                          FOnIdentityLinked OnResult, FOnAuthError OnError);

	/** True while an interactive (browser/loopback) sign-in or link is mid-flight: from the first
	 *  dispatch until the listener is armed (bLoopbackFlowPending), then while the listener is live
	 *  (LoopbackServer->IsActive()). Serializes the single loopback listener across the magic-link,
	 *  social, and link flows. Game-thread-only, like the rest of this class. */
	bool bLoopbackFlowPending = false;
	bool IsInteractiveSignInBusy() const;

	void ScheduleProactiveRefresh(const FString& ExpiresAtIso8601);
	void CancelProactiveRefresh();

	/** True while a mint or refresh response is still pending used to debounce
	 *  overlapping rotations (e.g. an err-32 storm or a proactive/reactive overlap). */
	bool IsTokenRotationInFlight() const;

	void SaveSession(const FString& SessionToken, int64 SessionGameTokenID, int64 UserID) const;

	/** Read the persisted session: the DPAPI-encrypted vault first, then a pre-WP4b plaintext
	 *  slot for one-time migration. Read-only (no writes/scrub); returns a transient save object
	 *  or nullptr. The next SaveSession re-persists encrypted and scrubs the plaintext copy. */
	UCrowdyAuthSaveGame* LoadVaultSave() const;
};
