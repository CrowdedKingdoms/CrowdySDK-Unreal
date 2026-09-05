#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Templates/PimplPtr.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Queries/Authentication/FCrowdyUserIdentity.h"
#include "CrowdyAuthentication.generated.h"

class UCrowdySDKBridgeSubsystem;
class UCrowdyGameSession;
class UCrowdyAuthSaveGame;
class UCrowdyCppClientSubsystem;
class FCrowdyCppClient;
class FCrowdyLoopbackAuthServer;

/**
 * The app-token fields the sign-in pipeline adopts. Reducing a mint or refresh payload to this plain struct keeps the
 * shared tail (endpoint normalization, refresh scheduling, delegates) written once, whatever produced the token.
 */
struct FCrowdyAppTokenFields
{
	FString AppToken;
	int64   AppGameTokenID = 0;
	FString ExpiresAt;
	FString GameApiUrl;
	FString GameApiWsUrl;
	FString LaunchUrl;
};

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
DECLARE_DYNAMIC_DELEGATE_OneParam(FOnLoginLinkSent, bool, bSent);

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
 * minted. Every sign-in method below password Login/Register, social, and the
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
 * over wiring the FOnAuthSuccess/FOnAuthError delegate params below by hand: same calls underneath,
 * single node with Success/Error exec pins.
 */
UCLASS(BlueprintType, meta=(DisplayName="Crowdy Authentication"))
class CROWDYSERVICES_API UCrowdyAuthentication : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	void InjectDependencies(UCrowdyGameSession* InGameSession);

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
	 * Magic-link step 1: email a one-time sign-in link. OnLinkSent reports sent
	 * (always true; no account enumeration). The token itself arrives only in the
	 * email. RedirectUri is the native loopback the OS hands back to; leave empty
	 * to use the server default.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Authentication")
	void RequestLoginLink(const FString& Email, const FString& RedirectUri,
	                      FOnLoginLinkSent OnLinkSent, FOnAuthError OnError);

	/**
	 * Magic-link step 2: complete sign-in with the one-time token from the link.
	 * Feeds the mint pipeline.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Authentication")
	void CompleteLoginLink(const FString& Token, FOnAuthSuccess OnSuccess, FOnAuthError OnError);

	/**
	 * One-call magic-link sign-in. Opens a loopback listener on 127.0.0.1, requests the email
	 * link with that loopback as the redirect, and completes automatically when the user clicks
	 * the link (its redirect lands on the listener). OnSuccess fires once signed in; OnError on
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
		Restore,
		Refresh,   // proactive rotation or reactive recovery; no per-call delegate
	};

	UPROPERTY()
	UCrowdyGameSession* GameSession = nullptr;

	/** Loopback HTTP listener for the magic-link / social redirect, created on first use.
	 *  TPimplPtr so a forward-declared type works as a UObject member: the deleter is captured by
	 *  MakePimpl in the .cpp where the type is complete, so the UHT-generated special members never
	 *  delete an incomplete type. */
	TPimplPtr<FCrowdyLoopbackAuthServer> LoopbackServer;

	FTSTicker::FDelegateHandle RefreshTickerHandle;

	/** Stage 1: store the SESSION token, persist it, and kick off the app-token mint. */
	void BeginMintPipeline(const FString& SessionToken, int64 SessionGameTokenID, int64 UserID,
	                       EAuthFlow Flow, FOnAuthSuccess OnSuccess, FOnAuthError OnError);

	/** Dispatch mintAppToken (bearer = SESSION token) for the current AppID. */
	void DispatchMint(EAuthFlow Flow, FOnAuthSuccess OnSuccess, FOnAuthError OnError);

	/** Dispatch refreshAppToken (bearer = current APP token). */
	void DispatchRefresh();

	/** Stage 2: store the APP token + per-app endpoints, schedule refresh, fire success. */
	void ApplyAppTokenAndFinish(const FCrowdyAppTokenFields& Token, EAuthFlow Flow, FOnAuthSuccess OnSuccess);

	/**
	 * The API client every authentication call is issued on. Null means there is no client host on this game instance
	 * or the client could not be built; the call cannot proceed and the caller reports the failure.
	 */
	FCrowdyCppClient* ResolveAuthClient();

	/**
	 * Resolve which datacenter this app is served from, then run Continue with a client pointed at it.
	 *
	 * Sign-in has to happen where the app lives. The shared origin is a multivalue DNS record over every
	 * datacenter's balancer, so a cold client's first request lands wherever DNS pointed it, and roughly half the
	 * time that is not the datacenter hosting the app; signing in there writes the session in the wrong place and
	 * then mints the app token across a WAN. Asking first is possible precisely because the app id is a build-time
	 * constant, known long before any credential is.
	 *
	 * Resolved once per session and cached, because placement changes only when an operator moves the app. Every
	 * failure path still runs Continue: an unresolvable placement is not a reason to refuse a sign-in, since the
	 * server answers a misplaced request with a redirect the client follows anyway. This only makes that the
	 * exception rather than the rule.
	 *
	 * OnUnavailable runs on the one path that does not reach Continue, so a caller holding something across the
	 * resolve (the social flow reserves a loopback port before calling) can release it. Without it that reservation
	 * would outlive the attempt and refuse every later sign-in as "already in progress".
	 */
	void WithAppEndpointResolved(TFunction<void(FCrowdyCppClient&)> Continue, EAuthFlow Flow, FOnAuthError OnError,
		TFunction<void()> OnUnavailable = TFunction<void()>());

	/** Whether WithAppEndpointResolved has already answered this session, successfully or not. */
	bool bAppEndpointResolved = false;

	/** The game-instance client host, or null outside a game instance. */
	UCrowdyCppClientSubsystem* GetCppClientHost();

	/** Push the tokens the pipeline currently holds onto the client host, so the next call issues under them. */
	void PublishTokensToCppClient();

	/** How many token mints or refreshes are in flight, so overlapping rotations can be debounced. */
	int32 CppRotationsInFlight = 0;

	/**
	 * Marks this subsystem's usable lifetime: created in Initialize, released first in Deinitialize. The API client
	 * lives in a different game-instance subsystem with its own ticker and the two are torn down in no guaranteed
	 * order, so a completion can still arrive after this one has stopped being usable. A weak-object check does not
	 * catch that, because the object is not collected the moment it is deinitialized.
	 */
	TSharedPtr<uint8> LiveSessionToken;

	/**
	 * Wrap a completion so it is dropped once this subsystem is torn down. Without it a late sign-in result would
	 * broadcast during shutdown, and a late token refresh would re-arm the timer Deinitialize just cancelled.
	 */
	template <typename ResultType>
	TFunction<void(ResultType)> GuardSession(TFunction<void(ResultType)> Body) const
	{
		TWeakPtr<uint8> Weak = LiveSessionToken;
		return [Weak, Body = MoveTemp(Body)](ResultType Result)
		{
			if (!Weak.IsValid())
			{
				return;
			}
			Body(MoveTemp(Result));
		};
	}

	/** Fire the failure delegate that matches Flow. */
	void FailFlow(const FString& Message, EAuthFlow Flow, FOnAuthError OnError);

	/** Social step 1 completion: arm the listener against the server's CSRF state, then open the consent page. */
	void OnSocialLoginStarted(const FString& Provider, const FString& AuthorizeUrl, const FString& State,
	                          FOnAuthSuccess OnSuccess, FOnAuthError OnError);

	/** Link step 1 completion: the same arm-then-open as a social sign-in, ending in a link rather than a session. */
	void OnLinkIdentityStarted(const FString& Provider, const FString& AuthorizeUrl, const FString& State,
	                           FOnIdentityLinked OnResult, FOnAuthError OnError);

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

	/** Arm the one-shot app-token rotation timer, replacing whatever it was armed for. */
	void ArmRotationTimer(double DelaySeconds);

	/**
	 * Re-arm rotation a short while after an attempt could not be made or failed. The timer is one-shot, so without
	 * this a single failed attempt would end token rotation for the rest of the session. Bounded: once the attempts
	 * are spent the token is left to expire and is re-minted when the server reports it expired.
	 */
	void ScheduleRotationRetry(const TCHAR* Reason);

	/** Rotation attempts spent since the last app token was applied. */
	int32 RotationRetriesUsed = 0;

	/** True while a mint or refresh response is still pending, used to debounce
	 *  overlapping rotations (e.g. an err-32 storm or a proactive/reactive overlap). */
	bool IsTokenRotationInFlight() const;

	void SaveSession(const FString& SessionToken, int64 SessionGameTokenID, int64 UserID) const;

	/** Read the persisted session: the DPAPI-encrypted vault first, then a legacy plaintext
	 *  slot for one-time migration. Read-only (no writes/scrub); returns a transient save object
	 *  or nullptr. The next SaveSession re-persists encrypted and scrubs the plaintext copy. */
	UCrowdyAuthSaveGame* LoadVaultSave() const;

	/** Full path to this instance's DPAPI-encrypted session vault. */
	FString GetAuthVaultPath() const;

	/** Legacy plaintext SaveGame slot name for one-time migration, scoped like the vault. */
	FString GetLegacyAuthSlotName() const;

	/** Discriminator appended to the vault path/slot so two in-process PIE clients never share one
	 *  session file (both would otherwise restore the same account, conflating identity). Empty
	 *  except under editor PIE, so standalone/packaged keep the original unsuffixed paths. */
	FString GetInstanceSuffix() const;
};
