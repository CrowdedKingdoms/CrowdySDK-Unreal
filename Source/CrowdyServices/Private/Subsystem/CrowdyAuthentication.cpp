#include "Subsystem/CrowdyAuthentication.h"
#include "CrowdyServicesLog.h"
#include "CrowdyCppClient.h"
#include "Subsystem/CrowdyAuthPayloads.h"
#include "Subsystem/Data/CrowdyAuthSaveGame.h"
#include "Auth/FCrowdyLoopbackAuthServer.h"
#include "Security/FCrowdySecretFile.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformProcess.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/GameInstance.h"
#include "Engine/EngineTypes.h"
#include "Network/CrowdyCpp/CrowdyCppClientSubsystem.h"
#include "Subsystem/CrowdyGameSession.h"
#include "CrowdyServiceApiSupport.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

using namespace CrowdyAuthPayloads;

// Legacy plaintext SaveGame slot base name, from before sessions were DPAPI-encrypted. Kept only so
// a returning user is migrated to the encrypted vault once (read it, then scrub it); nothing writes
// to this slot anymore.
static const FString LegacyAuthSlotBase = TEXT("CrowdyAuth");
static const int32   AuthUserIndex      = 0;

// How long the loopback listener waits for the user to click the magic link before giving up.
static constexpr double MagicLinkTimeoutSeconds = 180.0;

// OAuth consent (pick an account, review scopes) can take longer than clicking an email link.
static constexpr double SocialSignInTimeoutSeconds = 300.0;

// How soon a rotation that could not be made (or that failed) is attempted again, and how many such attempts are
// spent before the token is left to expire and recovered reactively instead.
static constexpr double RotationRetrySeconds = 30.0;
static constexpr int32  MaxRotationRetries   = 5;

// Consecutive server-reported refusals of a rotation that named the current replication server, past which the
// server stops being named. Two rather than one, because the two reasons a refusal happens look identical in the
// answer and only repetition tells them apart: a lapsed app token is refused once and is then cured by the
// re-mint that follows, so the next rotation names the server again and succeeds, while a Game API that does not
// know the argument refuses every attempt for as long as the client runs.
static constexpr int32  MaxServerNamedRefusals = 2;

void UCrowdyAuthentication::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Marks this subsystem's usable lifetime. Every completion handed to the shared API client holds it weakly, so a
	// request still in flight at teardown lands on nothing instead of on a subsystem whose state has been cleared.
	LiveSessionToken = MakeShared<uint8>(0);
}

void UCrowdyAuthentication::Deinitialize()
{
	// Released before anything else is torn down. The client belongs to a different game-instance subsystem with its
	// own ticker, and the order the two are deinitialized in is not guaranteed, so a completion can still arrive
	// after this point: it must not re-arm the refresh timer this call is about to cancel, and it must not broadcast
	// a sign-in result during shutdown.
	LiveSessionToken.Reset();

	CancelProactiveRefresh();

	// Tear down the loopback listener (unbinds its route, cancels timers) on the game thread.
	LoopbackServer.Reset();

	Super::Deinitialize();
}

void UCrowdyAuthentication::InjectDependencies(UCrowdyGameSession* InGameSession)
{
	GameSession = InGameSession;
}

UCrowdyCppClientSubsystem* UCrowdyAuthentication::GetCppClientHost()
{
	// Resolved from the game instance rather than from a world, because sign-in can run before any world exists.
	UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UCrowdyCppClientSubsystem>() : nullptr;
}

void UCrowdyAuthentication::PublishTokensToCppClient()
{
	if (UCrowdyCppClientSubsystem* Host = GetCppClientHost())
	{
		Host->SetManagementToken(GameSession ? GameSession->GetSessionToken() : FString());
		Host->SetGameToken(GameSession ? GameSession->GetGameToken() : FString());
	}
}

FCrowdyCppClient* UCrowdyAuthentication::ResolveAuthClient()
{
	UCrowdyCppClientSubsystem* Host = GetCppClientHost();
	if (!Host)
	{
		UE_LOG(LogCrowdyServices, Warning,
			TEXT("[CrowdyAuth] No API client host on this game instance; the call cannot proceed."));
		return nullptr;
	}

	PublishTokensToCppClient();

	// The configuration comes from the developer settings rather than from the per-app URL a mint returns, which is
	// what every other caller asks for. Asking for a different one would rebuild the shared client and cancel
	// whatever else was in flight on it, so agreeing on one source keeps the client built exactly once. A datacenter
	// the client moves to on its own is not a different configuration, so a redirect does not rebuild it either.
	FCrowdyCppClient* Client = Host->GetClient(CrowdyServiceApi::ResolveClientConfig());
	if (!Client)
	{
		UE_LOG(LogCrowdyServices, Warning,
			TEXT("[CrowdyAuth] Could not construct the API client; the call cannot proceed."));
		return nullptr;
	}

	return Client;
}

void UCrowdyAuthentication::WithAppEndpointResolved(TFunction<void(FCrowdyCppClient&)> Continue, EAuthFlow Flow,
	FOnAuthError OnError, TFunction<void()> OnUnavailable)
{
	FCrowdyCppClient* Client = ResolveAuthClient();
	if (!Client)
	{
		if (OnUnavailable)
		{
			OnUnavailable();
		}
		FailFlow(TEXT("Authentication client unavailable."), Flow, OnError);
		return;
	}

	const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>();
	const int64 AppID = Settings ? Settings->AppID : 0;
	if (bAppEndpointResolved || AppID <= 0)
	{
		Continue(*Client);
		return;
	}

	// Discovery has to be asked of the SHARED origin specifically, not of whatever endpoint is configured. The
	// configured game URL is the previous answer to this very question: it was written by an earlier app sync, so it
	// can be stale, and it can name a different environment than the backend now selected. Asking it where the app
	// lives is asking the thing whose correctness is in doubt.
	//
	// Usually the same URL, in which case the host hands back the client it already had and nothing is rebuilt. When
	// they differ this rebuilds once here and once more after the answer is adopted, which is affordable because it
	// happens at the start of a sign-in, before anything else is in flight.
	const FString DiscoveryUrl = Settings ? Settings->GetDiscoveryUrl() : FString();
	if (!DiscoveryUrl.IsEmpty())
	{
		if (UCrowdyCppClientSubsystem* Host = GetCppClientHost())
		{
			FCrowdyCppClientConfig DiscoveryConfig;
			DiscoveryConfig.ApiUrl = DiscoveryUrl;
			DiscoveryConfig.DiscoveryUrl = DiscoveryUrl;
			if (FCrowdyCppClient* OnSharedOrigin = Host->GetClient(DiscoveryConfig))
			{
				Client = OnSharedOrigin;
			}
		}
	}

	TWeakObjectPtr<UCrowdyAuthentication> WeakThis(this);
	Client->ResolveAppEndpoints({LexToString(AppID)}, GuardSession<FCrowdyCppAppDiscoveryResult>(
		[WeakThis, Continue, Flow, OnError, OnUnavailable](FCrowdyCppAppDiscoveryResult Result)
		{
			UCrowdyAuthentication* Self = WeakThis.Get();
			if (!Self)
			{
				return;
			}

			// Marked resolved even on failure. Retrying discovery before every sign-in attempt would turn one bad
			// lookup into a permanent extra round trip on a path a redirect already recovers.
			Self->bAppEndpointResolved = true;

			if (Result.bOk && Result.Endpoints.Num() > 0 && Result.Endpoints[0].IsPlaced())
			{
				const FCrowdyCppAppEndpoint& Endpoint = Result.Endpoints[0];
				if (UCrowdySDKDeveloperSettings* Mutable = GetMutableDefault<UCrowdySDKDeveloperSettings>())
				{
					// Written to the settings rather than held here, because that is what every caller reads to
					// build its client. Not persisted to config: this is where the app lives right now, and
					// baking it into a shipped ini would outlive the next time an operator moves it.
					Mutable->GameApiHttpUrl = EnsureGameApiGraphqlPath(Endpoint.GameApiUrl);
					if (!Endpoint.GameApiWsUrl.IsEmpty())
					{
						Mutable->GameApiWsUrl = Endpoint.GameApiWsUrl;
					}
				}

				UE_CLOG(CrowdyServicesTrace::Services(), LogCrowdyServices, Log,
					TEXT("[CrowdyAuth] App %s is served from datacenter '%s' at %s; signing in there."),
					*Endpoint.AppID, *Endpoint.DatacenterCode, *Endpoint.GameApiUrl);
			}
			else if (!Result.bOk)
			{
				// Not a failure of the sign-in. The server redirects a misplaced request, so the worst case is the
				// slow path rather than a broken one, and saying so is worth more than refusing.
				UE_LOG(LogCrowdyServices, Warning,
					TEXT("[CrowdyAuth] Could not resolve where this app is served (%s); signing in against the shared origin and relying on a redirect."),
					*Result.ErrorMessage);
			}

			// Re-resolved rather than reused: adopting the endpoint above changes what the client should be, and
			// the host rebuilds it on the next ask. The old pointer would still be aimed at the shared origin.
			if (FCrowdyCppClient* Moved = Self->ResolveAuthClient())
			{
				// Give re-discovery a warm answer while the placement is known. It is read synchronously from
				// whichever thread first notices the endpoint has died, so it cannot be looked up on demand.
				if (const UCrowdySDKDeveloperSettings* Current = GetDefault<UCrowdySDKDeveloperSettings>())
				{
					Moved->SetRediscoveredEndpoint(Current->GetGameApiHttpUrl(), Current->GetGameApiWsUrl());
				}
				Continue(*Moved);
			}
			else
			{
				if (OnUnavailable)
				{
					OnUnavailable();
				}
				Self->FailFlow(TEXT("Authentication client unavailable."), Flow, OnError);
			}
		}));
}

// Each method issues its call on the API client and, on success, hands the SESSION
// token to the one shared mint pipeline (BeginMintPipeline).

void UCrowdyAuthentication::Login(const FString& Email, const FString& Password,
                                  FOnAuthSuccess OnSuccess, FOnAuthError OnError)
{
	// This subsystem and the client host share the game instance's lifetime, so a completion cannot arrive after
	// the subsystem is gone; the weak check covers only teardown ordering within that shutdown.
	TWeakObjectPtr<UCrowdyAuthentication> WeakThis(this);
	WithAppEndpointResolved([WeakThis, Email, Password, OnSuccess, OnError](FCrowdyCppClient& Client)
	{
		UCrowdyAuthentication* Issuer = WeakThis.Get();
		if (!Issuer) return;
		Client.SignInWithPassword(Email, Password, Issuer->GuardSession<FCrowdyCppAuthResult>(
			[WeakThis, OnSuccess, OnError](FCrowdyCppAuthResult Result)
			{
				UCrowdyAuthentication* Self = WeakThis.Get();
				if (!Self) return;
				if (Result.bOk)
				{
					Self->BeginMintPipeline(Result.SessionToken, Result.SessionGameTokenID, Result.UserID,
						EAuthFlow::Login, OnSuccess, OnError);
				}
				else
				{
					Self->FailFlow(Result.ErrorMessage, EAuthFlow::Login, OnError);
				}
			}));
	}, EAuthFlow::Login, OnError);
}

void UCrowdyAuthentication::Register(const FString& Email, const FString& Password,
                                     FOnAuthSuccess OnSuccess, FOnAuthError OnError)
{
	TWeakObjectPtr<UCrowdyAuthentication> WeakThis(this);
	WithAppEndpointResolved([WeakThis, Email, Password, OnSuccess, OnError](FCrowdyCppClient& Client)
	{
		UCrowdyAuthentication* Issuer = WeakThis.Get();
		if (!Issuer) return;
		// No gamertag is sent; the server assigns a default.
		Client.RegisterWithPassword(Email, Password, FString(), Issuer->GuardSession<FCrowdyCppAuthResult>(
			[WeakThis, OnSuccess, OnError](FCrowdyCppAuthResult Result)
			{
				UCrowdyAuthentication* Self = WeakThis.Get();
				if (!Self) return;
				if (Result.bOk)
				{
					Self->BeginMintPipeline(Result.SessionToken, Result.SessionGameTokenID, Result.UserID,
						EAuthFlow::Register, OnSuccess, OnError);
				}
				else
				{
					Self->FailFlow(Result.ErrorMessage, EAuthFlow::Register, OnError);
				}
			}));
	}, EAuthFlow::Register, OnError);
}

void UCrowdyAuthentication::RequestLoginLink(const FString& Email, const FString& RedirectUri,
                                             FOnLoginLinkSent OnLinkSent, FOnAuthError OnError)
{
	// Resolved first like the sign-ins, because the one-time token this issues is stored where it was issued: send
	// the link from one datacenter and redeem it against another and the redemption finds nothing.
	TWeakObjectPtr<UCrowdyAuthentication> WeakThis(this);
	WithAppEndpointResolved([WeakThis, Email, RedirectUri, OnLinkSent, OnError](FCrowdyCppClient& Client)
	{
		UCrowdyAuthentication* Issuer = WeakThis.Get();
		if (!Issuer) return;
		Client.RequestLoginLink(Email, RedirectUri, Issuer->GuardSession<FCrowdyCppJsonValueResult>(
			[OnLinkSent, OnError](FCrowdyCppJsonValueResult Result)
			{
				bool bSent = false;
				if (Result.bOk && ReadLoginLinkPayload(Result.Value, bSent))
				{
					OnLinkSent.ExecuteIfBound(bSent);
				}
				else
				{
					OnError.ExecuteIfBound(Result.bOk ? TEXT("Malformed requestLoginLink response") : Result.ErrorMessage);
				}
			}));
	}, EAuthFlow::MagicLink, OnError);
}

void UCrowdyAuthentication::CompleteLoginLink(const FString& Token, FOnAuthSuccess OnSuccess, FOnAuthError OnError)
{
	TWeakObjectPtr<UCrowdyAuthentication> WeakThis(this);
	WithAppEndpointResolved([WeakThis, Token, OnSuccess, OnError](FCrowdyCppClient& Client)
	{
		UCrowdyAuthentication* Issuer = WeakThis.Get();
		if (!Issuer) return;
		Client.CompleteLoginLink(Token, Issuer->GuardSession<FCrowdyCppAuthResult>(
			[WeakThis, OnSuccess, OnError](FCrowdyCppAuthResult Result)
			{
				UCrowdyAuthentication* Self = WeakThis.Get();
				if (!Self) return;
				if (Result.bOk)
				{
					Self->BeginMintPipeline(Result.SessionToken, Result.SessionGameTokenID, Result.UserID,
						EAuthFlow::MagicLink, OnSuccess, OnError);
				}
				else
				{
					Self->FailFlow(Result.ErrorMessage, EAuthFlow::MagicLink, OnError);
				}
			}));
	}, EAuthFlow::MagicLink, OnError);
}

void UCrowdyAuthentication::BeginMagicLinkSignIn(const FString& Email, FOnAuthSuccess OnSuccess, FOnAuthError OnError)
{
	if (Email.IsEmpty())
	{
		FailFlow(TEXT("Email is required"), EAuthFlow::MagicLink, OnError);
		return;
	}

	// Reject a re-entrant call while a magic-link flow is already armed. Otherwise re-Start()ing the
	// listener would strand the first flow's pending completion and could mis-route its OnSuccess/OnError
	// to the second attempt's listener.
	if (IsInteractiveSignInBusy())
	{
		FailFlow(TEXT("A sign-in is already in progress"), EAuthFlow::MagicLink, OnError);
		return;
	}

	if (!LoopbackServer.IsValid())
	{
		LoopbackServer = MakePimpl<FCrowdyLoopbackAuthServer>();
	}

	// On the loopback callback (game thread, from the HttpServer ticker) feed the captured token
	// into the shared completion path. CompleteLoginLink runs the MagicLink flow -> mint -> OnLogin.
	FOnLoopbackToken OnTokenCaptured;
	OnTokenCaptured.BindLambda([this, OnSuccess, OnError](const FString& Token)
	{
		CompleteLoginLink(Token, OnSuccess, OnError);
	});

	FOnLoopbackError OnListenerError;
	OnListenerError.BindLambda([this, OnError](const FString& Message)
	{
		FailFlow(Message, EAuthFlow::MagicLink, OnError);
	});

	// Empty expected state: per the server contract the magic-link one-time token is itself the
	// single-use credential and the server does not round-trip a state param on this flow. Social
	// sign-in will pass the server-issued state here instead.
	const FString RedirectUri = LoopbackServer->Start(FString(), MagicLinkTimeoutSeconds,
		OnTokenCaptured, OnListenerError);
	if (RedirectUri.IsEmpty())
	{
		// Start already reported the failure via OnListenerError.
		return;
	}

	// Inline the requestLoginLink dispatch rather than calling the public RequestLoginLink, whose
	// dynamic delegate cannot take a lambda and so cannot tear the listener down on a failure.
	FCrowdyCppClient* Client = ResolveAuthClient();
	if (!Client)
	{
		// The listener was armed a moment ago and nothing will ever call back into it, so drop it here.
		LoopbackServer->Stop();
		FailFlow(TEXT("Authentication client unavailable."), EAuthFlow::MagicLink, OnError);
		return;
	}

	TWeakObjectPtr<UCrowdyAuthentication> WeakThis(this);
	Client->RequestLoginLink(Email, RedirectUri, GuardSession<FCrowdyCppJsonValueResult>(
		[WeakThis, OnError](FCrowdyCppJsonValueResult Result)
		{
			UCrowdyAuthentication* Self = WeakThis.Get();
			if (!Self) return;

			bool bSent = false;
			if (!Result.bOk || !ReadLoginLinkPayload(Result.Value, bSent))
			{
				if (Self->LoopbackServer.IsValid()) { Self->LoopbackServer->Stop(); }
				Self->FailFlow(
					Result.bOk ? TEXT("Malformed requestLoginLink response") : Result.ErrorMessage,
					EAuthFlow::MagicLink, OnError);
				return;
			}

			// The email is on its way. The armed listener captures the link's redirect and OnSuccess
			// fires later from CompleteLoginLink's mint; nothing to do here but wait.
			UE_CLOG(CrowdyServicesTrace::Services(), LogCrowdyServices, Log,
				TEXT("[CrowdyAuth] Magic-link email requested; awaiting loopback callback."));
		}));
}

void UCrowdyAuthentication::GetAvailableLoginProviders(FOnLoginProvidersReceived OnResult, FOnAuthError OnError)
{
	FCrowdyCppClient* Client = ResolveAuthClient();
	if (!Client)
	{
		OnError.ExecuteIfBound(TEXT("Authentication client unavailable."));
		return;
	}

	Client->ListLoginProviders(GuardSession<FCrowdyCppStringListResult>(
		[OnResult, OnError](FCrowdyCppStringListResult Result)
		{
			if (Result.bOk)
			{
				OnResult.ExecuteIfBound(Result.Values);
			}
			else
			{
				OnError.ExecuteIfBound(Result.ErrorMessage);
			}
		}));
}

void UCrowdyAuthentication::BeginSocialSignIn(const FString& Provider, FOnAuthSuccess OnSuccess, FOnAuthError OnError)
{
	if (Provider.IsEmpty())
	{
		FailFlow(TEXT("A provider is required"), EAuthFlow::Social, OnError);
		return;
	}
	if (IsInteractiveSignInBusy())
	{
		FailFlow(TEXT("A sign-in is already in progress"), EAuthFlow::Social, OnError);
		return;
	}

	if (!LoopbackServer.IsValid())
	{
		LoopbackServer = MakePimpl<FCrowdyLoopbackAuthServer>();
	}

	// socialLoginStart needs the redirectUri now, but the CSRF state to arm the listener with only
	// arrives in its response, so reserve the sticky loopback URI first and arm the route later.
	const FString RedirectUri = LoopbackServer->ReserveRedirectUri();
	if (RedirectUri.IsEmpty())
	{
		FailFlow(TEXT("Could not open a local sign-in port."), EAuthFlow::Social, OnError);
		return;
	}

	// Cover the reserve -> arm window; once armed, LoopbackServer->IsActive() takes over the guard.
	bLoopbackFlowPending = true;

	// Resolved before starting, for the same reason as the magic link: the CSRF state the provider round-trips is
	// held where socialLoginStart ran, so completing against a different datacenter would find no such state.
	//
	// The reserved port is live from here on, so every path out of the resolve has to clear the pending flag or the
	// next sign-in is refused as "already in progress" for the rest of the session.
	TWeakObjectPtr<UCrowdyAuthentication> WeakThis(this);
	WithAppEndpointResolved([WeakThis, Provider, RedirectUri, OnSuccess, OnError](FCrowdyCppClient& Client)
	{
		UCrowdyAuthentication* Issuer = WeakThis.Get();
		if (!Issuer)
		{
			return;
		}
		Client.SocialLoginStart(Provider, RedirectUri, Issuer->GuardSession<FCrowdyCppJsonValueResult>(
			[WeakThis, Provider, OnSuccess, OnError](FCrowdyCppJsonValueResult Result)
			{
				UCrowdyAuthentication* Self = WeakThis.Get();
				if (!Self) return;

				FString AuthorizeUrl;
				FString State;
				if (!Result.bOk || !ReadSocialStartPayload(Result.Value, AuthorizeUrl, State))
				{
					Self->bLoopbackFlowPending = false;
					Self->FailFlow(
						Result.bOk ? TEXT("Malformed socialLoginStart response") : Result.ErrorMessage,
						EAuthFlow::Social, OnError);
					return;
				}
				Self->OnSocialLoginStarted(Provider, AuthorizeUrl, State, OnSuccess, OnError);
			}));
	}, EAuthFlow::Social, OnError,
	[WeakThis]()
	{
		if (UCrowdyAuthentication* Self = WeakThis.Get())
		{
			Self->bLoopbackFlowPending = false;
		}
	});
}

void UCrowdyAuthentication::OnSocialLoginStarted(const FString& Provider, const FString& AuthorizeUrl,
                                                 const FString& State, FOnAuthSuccess OnSuccess, FOnAuthError OnError)
{
	// Two things must hold before a listener is armed at all. The state is the CSRF material the provider round-trips
	// and the listener treats an empty expected state as "accept any callback", so arming without one would let any
	// other process on the machine post an authorization code of its choosing. And the authorize URL is handed to the
	// platform's URL opener, which on some platforms is the shell, so it has to actually be a web address.
	if (State.IsEmpty() || !IsBrowserNavigableUrl(AuthorizeUrl))
	{
		bLoopbackFlowPending = false;
		FailFlow(TEXT("The sign-in provider returned an unusable authorization request."), EAuthFlow::Social, OnError);
		return;
	}

	// Arm the listener bound to the server-issued state (CSRF). On the captured code, complete
	// the social sign-in; on listener error/timeout, fail the flow.
	FOnLoopbackToken OnCodeCaptured;
	OnCodeCaptured.BindLambda([this, Provider, State, OnSuccess, OnError](const FString& Code)
	{
		CompleteSocialLogin(Provider, Code, State, OnSuccess, OnError);
	});

	FOnLoopbackError OnListenerError;
	OnListenerError.BindLambda([this, OnError](const FString& Message)
	{
		FailFlow(Message, EAuthFlow::Social, OnError);
	});

	// Cleared before Start, because Start reports a failure synchronously and a handler that reacts by retrying the
	// sign-in would otherwise be refused for a flow that has already ended.
	bLoopbackFlowPending = false;

	if (!LoopbackServer.IsValid())
	{
		// Nothing to arm and nothing to report it, so say so here rather than ending the flow in silence.
		FailFlow(TEXT("The local sign-in listener is unavailable."), EAuthFlow::Social, OnError);
		return;
	}

	const FString ArmedUri = LoopbackServer->Start(State, SocialSignInTimeoutSeconds, OnCodeCaptured, OnListenerError);
	if (ArmedUri.IsEmpty())
	{
		return; // Start already reported the failure via OnListenerError.
	}

	// Open the provider's consent page; its redirect lands on the armed listener.
	FPlatformProcess::LaunchURL(*AuthorizeUrl, nullptr, nullptr);

	UE_CLOG(CrowdyServicesTrace::Services(), LogCrowdyServices, Log,
		TEXT("[CrowdyAuth] Social sign-in: opened provider consent; awaiting loopback callback."));
}

void UCrowdyAuthentication::CompleteSocialLogin(const FString& Provider, const FString& Code, const FString& State,
                                                FOnAuthSuccess OnSuccess, FOnAuthError OnError)
{
	TWeakObjectPtr<UCrowdyAuthentication> WeakThis(this);
	WithAppEndpointResolved([WeakThis, Provider, Code, State, OnSuccess, OnError](FCrowdyCppClient& Client)
	{
		UCrowdyAuthentication* Issuer = WeakThis.Get();
		if (!Issuer) return;
		Client.SocialLoginComplete(Provider, Code, State, Issuer->GuardSession<FCrowdyCppAuthResult>(
			[WeakThis, OnSuccess, OnError](FCrowdyCppAuthResult Result)
			{
				UCrowdyAuthentication* Self = WeakThis.Get();
				if (!Self) return;
				if (Result.bOk)
				{
					Self->BeginMintPipeline(Result.SessionToken, Result.SessionGameTokenID, Result.UserID,
						EAuthFlow::Social, OnSuccess, OnError);
				}
				else
				{
					Self->FailFlow(Result.ErrorMessage, EAuthFlow::Social, OnError);
				}
			}));
	}, EAuthFlow::Social, OnError);
}

void UCrowdyAuthentication::GetMyIdentities(FOnIdentitiesReceived OnResult, FOnAuthError OnError)
{
	FCrowdyCppClient* Client = ResolveAuthClient();
	if (!Client)
	{
		OnError.ExecuteIfBound(TEXT("Authentication client unavailable."));
		return;
	}

	const int64 SignedInUserId = GameSession ? GameSession->GetUserID() : 0;
	Client->ListMyIdentities(GuardSession<FCrowdyCppJsonValueResult>(
		[OnResult, OnError, SignedInUserId](FCrowdyCppJsonValueResult Result)
		{
			const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
			if (!Result.bOk || !Result.Value.IsValid() || !Result.Value->TryGetArray(Array) || !Array)
			{
				OnError.ExecuteIfBound(Result.bOk ? TEXT("Malformed myIdentities response") : Result.ErrorMessage);
				return;
			}

			TArray<FCrowdyUserIdentity> Identities;
			Identities.Reserve(Array->Num());
			for (const TSharedPtr<FJsonValue>& Entry : *Array)
			{
				// An entry that is not a usable identity is skipped rather than surfaced blank: a null element is
				// ordinary GraphQL null-propagation, and a row with no id cannot be unlinked later anyway.
				FCrowdyUserIdentity Identity;
				if (ReadIdentity(Entry, SignedInUserId, Identity))
				{
					Identities.Add(MoveTemp(Identity));
				}
			}
			OnResult.ExecuteIfBound(Identities);
		}));
}

void UCrowdyAuthentication::BeginLinkIdentity(const FString& Provider, FOnIdentityLinked OnResult, FOnAuthError OnError)
{
	if (Provider.IsEmpty())
	{
		OnError.ExecuteIfBound(TEXT("A provider is required"));
		return;
	}
	// Linking attaches to an existing account, so a session must already be established.
	if (!GameSession || GameSession->GetSessionToken().IsEmpty())
	{
		OnError.ExecuteIfBound(TEXT("Sign in before linking an identity"));
		return;
	}
	if (IsInteractiveSignInBusy())
	{
		OnError.ExecuteIfBound(TEXT("A sign-in is already in progress"));
		return;
	}

	if (!LoopbackServer.IsValid())
	{
		LoopbackServer = MakePimpl<FCrowdyLoopbackAuthServer>();
	}

	const FString RedirectUri = LoopbackServer->ReserveRedirectUri();
	if (RedirectUri.IsEmpty())
	{
		OnError.ExecuteIfBound(TEXT("Could not open a local sign-in port."));
		return;
	}

	bLoopbackFlowPending = true;

	FCrowdyCppClient* Client = ResolveAuthClient();
	if (!Client)
	{
		bLoopbackFlowPending = false;
		OnError.ExecuteIfBound(TEXT("Authentication client unavailable."));
		return;
	}

	TWeakObjectPtr<UCrowdyAuthentication> WeakThis(this);
	Client->SocialLoginStart(Provider, RedirectUri, GuardSession<FCrowdyCppJsonValueResult>(
		[WeakThis, Provider, OnResult, OnError](FCrowdyCppJsonValueResult Result)
		{
			UCrowdyAuthentication* Self = WeakThis.Get();
			if (!Self) return;

			FString AuthorizeUrl;
			FString State;
			if (!Result.bOk || !ReadSocialStartPayload(Result.Value, AuthorizeUrl, State))
			{
				Self->bLoopbackFlowPending = false;
				OnError.ExecuteIfBound(
					Result.bOk ? TEXT("Malformed socialLoginStart response") : Result.ErrorMessage);
				return;
			}
			Self->OnLinkIdentityStarted(Provider, AuthorizeUrl, State, OnResult, OnError);
		}));
}

void UCrowdyAuthentication::OnLinkIdentityStarted(const FString& Provider, const FString& AuthorizeUrl,
                                                  const FString& State, FOnIdentityLinked OnResult,
                                                  FOnAuthError OnError)
{
	// The same two preconditions as a social sign-in: without the server's CSRF state the listener would accept any
	// callback, and the authorize URL reaches the platform's URL opener.
	if (State.IsEmpty() || !IsBrowserNavigableUrl(AuthorizeUrl))
	{
		bLoopbackFlowPending = false;
		OnError.ExecuteIfBound(TEXT("The sign-in provider returned an unusable authorization request."));
		return;
	}

	FOnLoopbackToken OnCodeCaptured;
	OnCodeCaptured.BindLambda([this, Provider, State, OnResult, OnError](const FString& Code)
	{
		CompleteLinkIdentity(Provider, Code, State, OnResult, OnError);
	});

	FOnLoopbackError OnListenerError;
	OnListenerError.BindLambda([OnError](const FString& Message)
	{
		OnError.ExecuteIfBound(Message);
	});

	bLoopbackFlowPending = false;

	if (!LoopbackServer.IsValid())
	{
		OnError.ExecuteIfBound(TEXT("The local sign-in listener is unavailable."));
		return;
	}

	const FString ArmedUri = LoopbackServer->Start(State, SocialSignInTimeoutSeconds, OnCodeCaptured, OnListenerError);
	if (ArmedUri.IsEmpty())
	{
		return;
	}

	FPlatformProcess::LaunchURL(*AuthorizeUrl, nullptr, nullptr);

	UE_CLOG(CrowdyServicesTrace::Services(), LogCrowdyServices, Log,
		TEXT("[CrowdyAuth] Link identity: opened provider consent; awaiting loopback callback."));
}

void UCrowdyAuthentication::CompleteLinkIdentity(const FString& Provider, const FString& Code, const FString& State,
                                                 FOnIdentityLinked OnResult, FOnAuthError OnError)
{
	FCrowdyCppClient* Client = ResolveAuthClient();
	if (!Client)
	{
		OnError.ExecuteIfBound(TEXT("Authentication client unavailable."));
		return;
	}

	const int64 SignedInUserId = GameSession ? GameSession->GetUserID() : 0;
	Client->LinkIdentity(Provider, Code, State, GuardSession<FCrowdyCppJsonValueResult>(
		[OnResult, OnError, SignedInUserId](FCrowdyCppJsonValueResult Result)
		{
			// An identity with no id reads as a linked account the caller can never unlink, so it is an error
			// rather than a blank success.
			FCrowdyUserIdentity Identity;
			if (!Result.bOk || !ReadIdentity(Result.Value, SignedInUserId, Identity))
			{
				OnError.ExecuteIfBound(Result.bOk ? TEXT("Malformed linkIdentity response") : Result.ErrorMessage);
				return;
			}
			OnResult.ExecuteIfBound(Identity);
		}));
}

void UCrowdyAuthentication::UnlinkIdentity(const FString& IdentityId, FOnIdentityUnlinked OnResult, FOnAuthError OnError)
{
	if (IdentityId.IsEmpty())
	{
		OnError.ExecuteIfBound(TEXT("An identityId is required"));
		return;
	}

	FCrowdyCppClient* Client = ResolveAuthClient();
	if (!Client)
	{
		OnError.ExecuteIfBound(TEXT("Authentication client unavailable."));
		return;
	}

	Client->UnlinkIdentity(IdentityId, GuardSession<FCrowdyCppBoolResult>(
		[OnResult, OnError](FCrowdyCppBoolResult Result)
		{
			if (Result.bOk)
			{
				OnResult.ExecuteIfBound(Result.bValue);
			}
			else
			{
				OnError.ExecuteIfBound(Result.ErrorMessage);
			}
		}));
}

bool UCrowdyAuthentication::IsInteractiveSignInBusy() const
{
	return bLoopbackFlowPending || (LoopbackServer.IsValid() && LoopbackServer->IsActive());
}

void UCrowdyAuthentication::BeginMintPipeline(const FString& SessionToken, int64 SessionGameTokenID, int64 UserID,
                                              EAuthFlow Flow, FOnAuthSuccess OnSuccess, FOnAuthError OnError)
{
	// Store the SESSION token on the management plane only. It is never handed to
	// the Game API / UDP path: that is what an app-scoped token (minted below) is for.
	if (GameSession)
	{
		GameSession->SetUserID(UserID);
		GameSession->SetSessionGameTokenID(SessionGameTokenID);
		GameSession->SetSessionToken(SessionToken);
	}
	PublishTokensToCppClient();

	// Persist the SESSION token only (durable, mint-capable). The app token is
	// short-lived and stays in memory.
	SaveSession(SessionToken, SessionGameTokenID, UserID);

	DispatchMint(Flow, OnSuccess, OnError);
}

void UCrowdyAuthentication::DispatchMint(EAuthFlow Flow, FOnAuthSuccess OnSuccess, FOnAuthError OnError)
{
	if (!GameSession)
	{
		if (Flow == EAuthFlow::Refresh)
		{
			// Background rotation has no caller to answer, so it re-arms itself rather than ending the chain here.
			ScheduleRotationRetry(TEXT("no game session available"));
		}
		FailFlow(TEXT("SDK not initialised"), Flow, OnError);
		return;
	}

	const int64 AppID = GameSession->GetAppID();
	if (AppID <= 0)
	{
		FailFlow(TEXT("Invalid AppID for app-token mint"), Flow, OnError);
		return;
	}

	FCrowdyCppClient* Client = ResolveAuthClient();
	if (!Client)
	{
		if (Flow == EAuthFlow::Refresh)
		{
			// Background rotation has no caller to answer, so it re-arms itself instead of ending here.
			ScheduleRotationRetry(TEXT("no authentication client available"));
		}
		FailFlow(TEXT("Authentication client unavailable."), Flow, OnError);
		return;
	}

	TWeakObjectPtr<UCrowdyAuthentication> WeakThis(this);
	++CppRotationsInFlight;
	Client->MintAppToken(AppID, GuardSession<FCrowdyCppAppTokenResult>(
		[WeakThis, Flow, OnSuccess, OnError](FCrowdyCppAppTokenResult Result)
		{
			UCrowdyAuthentication* Self = WeakThis.Get();
			if (!Self) return;
			--Self->CppRotationsInFlight;
			if (Result.bOk)
			{
				Self->ApplyAppTokenAndFinish(MakeAppTokenFields(Result), Flow, OnSuccess);
			}
			else
			{
				const bool bWasCanceled = Result.ErrorMessage == FCrowdyCppClient::CanceledErrorMessage();
				if (Flow == EAuthFlow::Refresh)
				{
					Self->ScheduleRotationRetry(bWasCanceled
						? TEXT("the app-token mint was canceled")
						: TEXT("the app-token mint failed"));
				}

				// A cancellation means the request was abandoned (the client was rebuilt under it), which is internal
				// bookkeeping rather than something the user did wrong, so it is reported as an interruption instead
				// of surfacing the transport's own wording on an error pin.
				Self->FailFlow(bWasCanceled
					? FString(TEXT("Sign-in was interrupted. Please try again."))
					: Result.ErrorMessage, Flow, OnError);
			}
		}));
}

void UCrowdyAuthentication::DispatchRefresh()
{
	FCrowdyCppClient* Client = ResolveAuthClient();
	if (!Client)
	{
		// Rotation is a background task with no caller to answer, and re-minting would need the same client, so the
		// current token is left in place and the attempt is re-armed for a short while from now.
		UE_LOG(LogCrowdyServices, Warning,
			TEXT("[CrowdyAuth] App-token refresh skipped: no authentication client available."));
		ScheduleRotationRetry(TEXT("no authentication client available"));
		return;
	}

	// Naming the replication server the client is on lets the Game API install the replacement token there, so the
	// connection keeps its socket instead of re-assigning. Left empty when there is no assignment yet, and after a
	// server has refused the argument once; the call falls back to the plain rotation in both cases.
	FString CurrentServerIp4;
	int32 CurrentServerClientPort = 0;
	if (GameSession && !bRefreshRejectedCurrentServer)
	{
		CurrentServerIp4 = GameSession->GetReplicationServerIp4();
		CurrentServerClientPort = GameSession->GetReplicationServerClientPort();
	}
	const bool bNamedCurrentServer = !CurrentServerIp4.IsEmpty() && CurrentServerClientPort > 0;

	TWeakObjectPtr<UCrowdyAuthentication> WeakThis(this);
	++CppRotationsInFlight;
	Client->RefreshAppToken(CurrentServerIp4, CurrentServerClientPort, GuardSession<FCrowdyCppAppTokenResult>(
		[WeakThis, bNamedCurrentServer](FCrowdyCppAppTokenResult Result)
		{
			UCrowdyAuthentication* Self = WeakThis.Get();
			if (!Self) return;
			// Cleared before the re-mint below, so the debounce does not refuse the recovery it is asking for.
			--Self->CppRotationsInFlight;
			if (Result.bOk)
			{
				// A rotation that got through clears the refusal run, so only CONSECUTIVE refusals count.
				Self->ServerNamedRefusals = 0;
				Self->ApplyAppTokenAndFinish(MakeAppTokenFields(Result), EAuthFlow::Refresh, FOnAuthSuccess());
				return;
			}

			if (Result.ErrorMessage == FCrowdyCppClient::CanceledErrorMessage())
			{
				// The request was abandoned rather than refused, so the app token has not been shown to be stale
				// and re-minting would spend a round trip to learn nothing. Rotation is re-armed instead.
				UE_CLOG(CrowdyServicesTrace::Services(), LogCrowdyServices, Log,
					TEXT("[CrowdyAuth] App-token refresh was canceled; leaving the current token in place."));
				Self->ScheduleRotationRetry(TEXT("the app-token refresh was canceled"));
				return;
			}

			// A Game API older than ck-api v1.83.7 does not accept currentServer and refuses the whole mutation, so
			// every later rotation would pay the same failed round trip and re-mint. Stop naming the server once
			// that looks like what is happening.
			//
			// Counted only for an error the SERVER reported: a transport failure carries no code and says nothing
			// about the argument, and giving the feature up over a dropped connection would be a permanent answer
			// to a temporary problem.
			if (bNamedCurrentServer && !Result.ErrorCode.IsEmpty() && !Self->bRefreshRejectedCurrentServer)
			{
				++Self->ServerNamedRefusals;
				if (Self->ServerNamedRefusals >= MaxServerNamedRefusals)
				{
					Self->bRefreshRejectedCurrentServer = true;
					UE_LOG(LogCrowdyServices, Warning,
						TEXT("[CrowdyAuth] refreshAppToken was refused %d times in a row (%s) while naming the current "
							"replication server; later rotations will not name it, and the connection will re-assign "
							"after each one."),
						Self->ServerNamedRefusals, *Result.ErrorCode);
				}
			}

			// Refresh needs a still-valid app token as bearer; if it lapsed, re-mint
			// from the (longer-lived) session token instead.
			UE_LOG(LogCrowdyServices, Warning,
				TEXT("[CrowdyAuth] refreshAppToken failed (%s); re-minting from session."), *Result.ErrorMessage);
			Self->DispatchMint(EAuthFlow::Refresh, FOnAuthSuccess(), FOnAuthError());
		}));
}

void UCrowdyAuthentication::ApplyAppTokenAndFinish(const FCrowdyAppTokenFields& Token, EAuthFlow Flow, FOnAuthSuccess OnSuccess)
{
	// The mint returns gameApiUrl as a bare host; the Game API GraphQL lives at /graphql, so
	// normalize once and adopt that as the per-app Game endpoint (else every Game-API POST 404s).
	const FString GameApiGraphqlUrl = EnsureGameApiGraphqlPath(Token.GameApiUrl);

	// The app-scoped token is the gameplay credential: Game API bearer, UDP HMAC key,
	// and (as gameTokenId) the UDP spatial message tail.
	if (GameSession)
	{
		GameSession->SetGameToken(Token.AppToken);
		GameSession->SetGameTokenID(Token.AppGameTokenID);
		GameSession->SetAppTokenExpiresAt(Token.ExpiresAt);
		GameSession->SetGameApiUrl(GameApiGraphqlUrl);
		GameSession->SetGameApiWsUrl(Token.GameApiWsUrl);
		GameSession->SetLaunchUrl(Token.LaunchUrl);

		// After SetGameToken, which clears the pair: a replacement token is authorized nowhere until the answer
		// that carried it names a server, and only a rotation that named one ever does.
		if (!Token.AuthorizedServerIp4.IsEmpty() && Token.AuthorizedServerClientPort > 0)
		{
			GameSession->SetAppTokenAuthorizedServer(Token.AuthorizedServerIp4, Token.AuthorizedServerClientPort);
		}
	}
	// The rotated token has to reach the shared client too, or the next call there would still carry the old one.
	// This is also the write-back the refresh path depends on, since that call deliberately does not install its
	// own result.
	PublishTokensToCppClient();

	// A rotation landed, so the next one starts with a full retry budget.
	RotationRetriesUsed = 0;
	ScheduleProactiveRefresh(Token.ExpiresAt);

	UE_CLOG(CrowdyServicesTrace::Services(), LogCrowdyServices, Log,
		TEXT("[CrowdyAuth] App token applied. AppGameTokenID=%lld ExpiresAt=%s"),
		Token.AppGameTokenID, *Token.ExpiresAt);

	FCrowdyAuthResult Result;
	Result.GameToken = GameSession ? GameSession->GetSessionToken() : FString();
	Result.UserID    = GameSession ? GameSession->GetUserID() : 0;

	OnSuccess.ExecuteIfBound(Result);

	switch (Flow)
	{
	case EAuthFlow::Login:
	case EAuthFlow::MagicLink:
	case EAuthFlow::Social:
		OnLogin.Broadcast(Result);
		break;
	case EAuthFlow::Register:
		OnRegister.Broadcast(Result);
		break;
	case EAuthFlow::Restore:
		OnSessionRestored.Broadcast(Result);
		break;
	case EAuthFlow::Refresh:
		OnAppTokenRefreshed.Broadcast();
		break;
	}
}

void UCrowdyAuthentication::FailFlow(const FString& Message, EAuthFlow Flow, FOnAuthError OnError)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("[CrowdyAuth] sign-in/mint failed: %s"), *Message);

	OnError.ExecuteIfBound(Message);

	switch (Flow)
	{
	case EAuthFlow::Login:
	case EAuthFlow::MagicLink:
	case EAuthFlow::Social:
		OnLoginFailed.Broadcast(Message);
		break;
	case EAuthFlow::Register:
		OnRegisterFailed.Broadcast(Message);
		break;
	case EAuthFlow::Restore:
		OnSessionRestoreFailed.Broadcast(Message);
		break;
	case EAuthFlow::Refresh:
		// Background token recovery; nothing user-facing to surface.
		break;
	}
}

void UCrowdyAuthentication::RefreshAppToken()
{
	if (!GameSession)
		return;

	if (GameSession->GetGameToken().IsEmpty() && GameSession->GetSessionToken().IsEmpty())
		return;

	if (IsTokenRotationInFlight())
		return;

	DispatchRefresh();
}

void UCrowdyAuthentication::RecoverExpiredAppToken()
{
	if (!GameSession)
	{
		UE_LOG(LogCrowdyServices, Warning,
			TEXT("[CrowdyAuth] TOKEN_EXPIRED but the SDK is not initialised; the token cannot be re-minted."));
		return;
	}

	if (GameSession->GetSessionToken().IsEmpty())
	{
		UE_LOG(LogCrowdyServices, Warning, TEXT("[CrowdyAuth] TOKEN_EXPIRED but no session token to re-mint from."));
		return;
	}

	// The server can emit TOKEN_EXPIRED on every in-flight packet; only one re-mint
	// should be outstanding at a time.
	if (IsTokenRotationInFlight())
		return;

	UE_CLOG(CrowdyServicesTrace::Services(), LogCrowdyServices, Log, TEXT("[CrowdyAuth] Recovering expired app token (re-mint)."));
	DispatchMint(EAuthFlow::Refresh, FOnAuthSuccess(), FOnAuthError());
}

bool UCrowdyAuthentication::IsTokenRotationInFlight() const
{
	// Callers do a check-then-dispatch; that is race-free only because every rotation trigger runs on
	// the game thread (the FTSTicker fires there, and so does the inbound message that reports an
	// expired token). Keep it that way.
	return CppRotationsInFlight > 0;
}

void UCrowdyAuthentication::ScheduleProactiveRefresh(const FString& ExpiresAtIso8601)
{
	CancelProactiveRefresh();

	if (ExpiresAtIso8601.IsEmpty())
		return;

	FDateTime ExpiresAt;
	if (!FDateTime::ParseIso8601(*ExpiresAtIso8601, ExpiresAt))
	{
		UE_LOG(LogCrowdyServices, Warning,
			TEXT("[CrowdyAuth] Could not parse app-token expiry '%s'; proactive refresh disabled."), *ExpiresAtIso8601);
		return;
	}

	// Rotate a minute before expiry; clamp so a near- or past-due expiry still retries
	// soon rather than scheduling in the past.
	constexpr double SafetyMarginSeconds = 60.0;
	const FTimespan Remaining = ExpiresAt - FDateTime::UtcNow();
	double DelaySeconds = Remaining.GetTotalSeconds() - SafetyMarginSeconds;
	DelaySeconds = FMath::Clamp(DelaySeconds, 5.0, 24.0 * 60.0 * 60.0);

	ArmRotationTimer(DelaySeconds);

	UE_CLOG(CrowdyServicesTrace::Services(), LogCrowdyServices, Log,
		TEXT("[CrowdyAuth] App-token refresh scheduled in %.0fs."), DelaySeconds);
}

void UCrowdyAuthentication::ArmRotationTimer(double DelaySeconds)
{
	CancelProactiveRefresh();

	TWeakObjectPtr<UCrowdyAuthentication> WeakThis(this);
	RefreshTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([WeakThis](float) -> bool
		{
			if (UCrowdyAuthentication* Self = WeakThis.Get())
			{
				Self->RefreshTickerHandle.Reset();
				Self->RefreshAppToken();
			}
			return false; // one-shot
		}), static_cast<float>(DelaySeconds));
}

void UCrowdyAuthentication::ScheduleRotationRetry(const TCHAR* Reason)
{
	// Deinitialize releases this before it cancels the timer, so a completion arriving during teardown cannot arm a
	// new one here.
	if (!LiveSessionToken.IsValid())
	{
		return;
	}

	if (RotationRetriesUsed >= MaxRotationRetries)
	{
		// Retrying past this would keep a permanent failure invisible. The token is left to lapse; the server then
		// reports it expired and the reactive re-mint takes over.
		UE_LOG(LogCrowdyServices, Warning,
			TEXT("[CrowdyAuth] App-token rotation still failing after %d attempts (%s); leaving the token to expire."),
			RotationRetriesUsed, Reason);
		return;
	}

	++RotationRetriesUsed;
	ArmRotationTimer(RotationRetrySeconds);

	UE_LOG(LogCrowdyServices, Warning,
		TEXT("[CrowdyAuth] App-token rotation retry %d of %d in %.0fs (%s)."),
		RotationRetriesUsed, MaxRotationRetries, RotationRetrySeconds, Reason);
}

void UCrowdyAuthentication::CancelProactiveRefresh()
{
	if (RefreshTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(RefreshTickerHandle);
		RefreshTickerHandle.Reset();
	}
}

// The SESSION token is the long-lived, mint-capable credential, so it is encrypted at rest with
// DPAPI (per-user) rather than written to a plaintext SaveGame slot. The save object is serialized
// to a byte blob and that blob is encrypted; the schema (UCrowdyAuthSaveGame) is unchanged so the
// fields stay extensible.

FString UCrowdyAuthentication::GetInstanceSuffix() const
{
	// Under editor PIE each client runs in its own GameInstance but shares ProjectSavedDir with the
	// others, so an unsuffixed vault would be read/written by every window and all would restore the
	// same account. PIEInstance is the per-client discriminator. Standalone/packaged (GIsEditor==false)
	// and editor non-PIE worlds return "" so the path stays byte-identical to before this change.
	if (GIsEditor)
	{
		if (const UGameInstance* GI = GetGameInstance())
		{
			if (const FWorldContext* Ctx = GI->GetWorldContext())
			{
				if (Ctx->WorldType == EWorldType::PIE)
				{
					return FString::Printf(TEXT(".pie%d"), Ctx->PIEInstance);
				}
			}
		}
	}
	return FString();
}

// DPAPI-encrypted session vault. Holds only the long-lived, mint-capable SESSION token;
// the short-lived app token is never written to disk.
FString UCrowdyAuthentication::GetAuthVaultPath() const
{
	return FPaths::ProjectSavedDir() / FString::Printf(TEXT("CrowdySDK/session%s.bin"), *GetInstanceSuffix());
}

FString UCrowdyAuthentication::GetLegacyAuthSlotName() const
{
	return LegacyAuthSlotBase + GetInstanceSuffix();
}

UCrowdyAuthSaveGame* UCrowdyAuthentication::LoadVaultSave() const
{
	// Preferred: the DPAPI-encrypted vault.
	TArray<uint8> Blob;
	if (FCrowdySecretFile::LoadBytes(GetAuthVaultPath(), Blob))
	{
		if (UCrowdyAuthSaveGame* Save = Cast<UCrowdyAuthSaveGame>(UGameplayStatics::LoadGameFromMemory(Blob)))
		{
			return Save;
		}
	}

	// Fallback: a legacy plaintext slot, so a returning user is not forced to sign in again. The
	// next SaveSession (the mint pipeline persists immediately) re-writes it encrypted and scrubs
	// the plaintext copy.
	const FString LegacySlot = GetLegacyAuthSlotName();
	if (UGameplayStatics::DoesSaveGameExist(LegacySlot, AuthUserIndex))
	{
		if (UCrowdyAuthSaveGame* Save = Cast<UCrowdyAuthSaveGame>(
			UGameplayStatics::LoadGameFromSlot(LegacySlot, AuthUserIndex)))
		{
			return Save;
		}
	}

	return nullptr;
}

bool UCrowdyAuthentication::HasSavedSession() const
{
	const UCrowdyAuthSaveGame* Save = LoadVaultSave();
	return Save && !Save->SessionToken.IsEmpty();
}

bool UCrowdyAuthentication::IsSignedIn() const
{
	if (!GameSession)
	{
		return false;
	}

	// The app-scoped token is written only when the mint pipeline completes and is never persisted, so it is what
	// separates a live authenticated instance from a stored credential or a sign-in that failed after its identity
	// token was already stored.
	return GameSession->GetUserID() != 0 && !GameSession->GetGameToken().IsEmpty();
}

bool UCrowdyAuthentication::RestoreSession(FOnAuthSuccess OnSuccess, FOnAuthError OnError)
{
	const UCrowdyAuthSaveGame* Save = LoadVaultSave();

	if (!Save || Save->SessionToken.IsEmpty())
	{
		const FString Msg = TEXT("No saved session found");
		OnError.ExecuteIfBound(Msg);
		OnSessionRestoreFailed.Broadcast(Msg);
		return false;
	}

	UE_CLOG(CrowdyServicesTrace::Services(), LogCrowdyServices, Log,
		TEXT("[CrowdyAuth] Restoring session. UserID=%lld"), Save->UserID);

	// Restore re-mints a fresh app token from the persisted session token, then the
	// SDK requests UDP access, the same path as a fresh sign-in.
	BeginMintPipeline(Save->SessionToken, Save->SessionGameTokenID, Save->UserID, EAuthFlow::Restore, OnSuccess, OnError);
	return true;
}

void UCrowdyAuthentication::ClearSavedSession()
{
	CancelProactiveRefresh();

	FCrowdySecretFile::Delete(GetAuthVaultPath());

	// Also remove any legacy plaintext slot so logout fully forgets the credential.
	const FString LegacySlot = GetLegacyAuthSlotName();
	if (UGameplayStatics::DoesSaveGameExist(LegacySlot, AuthUserIndex))
	{
		UGameplayStatics::DeleteGameInSlot(LegacySlot, AuthUserIndex);
	}
}

void UCrowdyAuthentication::SaveSession(const FString& SessionToken, int64 SessionGameTokenID, int64 UserID) const
{
	UCrowdyAuthSaveGame* Save = Cast<UCrowdyAuthSaveGame>(
		UGameplayStatics::CreateSaveGameObject(UCrowdyAuthSaveGame::StaticClass()));
	if (!Save) return;

	Save->SessionToken       = SessionToken;
	Save->SessionGameTokenID = SessionGameTokenID;
	Save->UserID             = UserID;

	// Serialize to a blob, then DPAPI-encrypt it at rest. The SESSION token must never sit on disk
	// in the clear: it mints app tokens and is long-lived.
	TArray<uint8> Blob;
	const bool bSaved = UGameplayStatics::SaveGameToMemory(Save, Blob)
		&& FCrowdySecretFile::SaveBytes(GetAuthVaultPath(), Blob);

	if (bSaved)
	{
		UE_CLOG(CrowdyServicesTrace::Services(), LogCrowdyServices, Log,
			TEXT("[CrowdyAuth] Saved encrypted session vault. UserID=%lld"), UserID);

		// Scrub any legacy plaintext slot only once the encrypted copy is safely written, so a
		// write failure never destroys the user's only persisted session.
		const FString LegacySlot = GetLegacyAuthSlotName();
		if (UGameplayStatics::DoesSaveGameExist(LegacySlot, AuthUserIndex))
		{
			UGameplayStatics::DeleteGameInSlot(LegacySlot, AuthUserIndex);
		}
	}
	else
	{
		UE_LOG(LogCrowdyServices, Warning, TEXT("[CrowdyAuth] Failed to write encrypted session vault."));
	}
}
