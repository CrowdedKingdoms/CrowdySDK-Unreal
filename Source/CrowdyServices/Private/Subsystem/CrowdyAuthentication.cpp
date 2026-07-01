#include "Subsystem/CrowdyAuthentication.h"
#include "CrowdyServicesLog.h"
#include "Subsystem/Data/CrowdyAuthSaveGame.h"
#include "Auth/FCrowdyLoopbackAuthServer.h"
#include "Security/FCrowdySecretFile.h"
#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "Kismet/GameplayStatics.h"
#include "Network/GraphQL/CrowdyQuerySubsystem.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Core/GraphQL/Enums/EQueryResponseType.h"
#include "Core/GraphQL/Enums/EGraphQLQuery.h"
#include "Queries/Authentication/FLoginResponse.h"
#include "Queries/Authentication/FRegisterResponse.h"
#include "Queries/Authentication/FAuthResponseBase.h"
#include "Queries/Authentication/FAppTokenResponseBase.h"
#include "Queries/Authentication/FRequestLoginLinkRequest.h"
#include "Queries/Authentication/FRequestLoginLinkResponse.h"
#include "Queries/Authentication/FCompleteLoginLinkRequest.h"
#include "Queries/Authentication/FDevLoginRequest.h"
#include "Queries/Authentication/FMintAppTokenRequest.h"
#include "Queries/Authentication/FRefreshAppTokenRequest.h"
#include "Queries/Authentication/FSocialLoginStartRequest.h"
#include "Queries/Authentication/FSocialLoginStartResponse.h"
#include "Queries/Authentication/FSocialLoginCompleteRequest.h"
#include "Queries/Authentication/FSocialLoginCompleteResponse.h"
#include "Queries/Authentication/FAvailableLoginProvidersRequest.h"
#include "Queries/Authentication/FAvailableLoginProvidersResponse.h"
#include "Queries/Authentication/FMyIdentitiesRequest.h"
#include "Queries/Authentication/FMyIdentitiesResponse.h"
#include "Queries/Authentication/FLinkIdentityRequest.h"
#include "Queries/Authentication/FLinkIdentityResponse.h"
#include "Queries/Authentication/FUnlinkIdentityRequest.h"
#include "Queries/Authentication/FUnlinkIdentityResponse.h"

// Pre-WP4b plaintext SaveGame slot. Kept only so a returning user is migrated to the encrypted
// vault once (read it, then scrub it); nothing writes to this slot anymore.
static const FString LegacyAuthSlot = TEXT("CrowdyAuth");
static const int32   AuthUserIndex  = 0;

// DPAPI-encrypted session vault (WP4b). Holds only the long-lived, mint-capable SESSION token;
// the short-lived app token is never written to disk.
static FString GetAuthVaultPath()
{
	return FPaths::ProjectSavedDir() / TEXT("CrowdySDK/session.bin");
}

// How long the loopback listener waits for the user to click the magic link before giving up.
static constexpr double MagicLinkTimeoutSeconds = 180.0;

// OAuth consent (pick an account, review scopes) can take longer than clicking an email link.
static constexpr double SocialSignInTimeoutSeconds = 300.0;

// The mint response's gameApiUrl is a BARE HOST, but the Game API GraphQL is served at /graphql —
// using it raw makes every Game-API POST 404. Append the path when missing (idempotent; trims a
// trailing slash). Path-append only, no host derivation. Mirrors FCrowdyConfigSync::EnsureGraphqlPath,
// which lives in the editor-only Studio module and so cannot be shared into the runtime.
static FString EnsureGameApiGraphqlPath(const FString& Url)
{
	FString Normalized = Url;
	Normalized.RemoveFromEnd(TEXT("/"));
	if (!Normalized.IsEmpty() && !Normalized.EndsWith(TEXT("/graphql")))
	{
		Normalized += TEXT("/graphql");
	}
	return Normalized;
}

// Lifecycle

void UCrowdyAuthentication::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
}

void UCrowdyAuthentication::Deinitialize()
{
	CancelProactiveRefresh();

	// Tear down the loopback listener (unbinds its route, cancels timers) on the game thread.
	LoopbackServer.Reset();

	FScopeLock Lock(&CallbackMutex);
	PendingCallbacks.Empty();
	Super::Deinitialize();
}

void UCrowdyAuthentication::InjectDependencies(FCrowdyDataRegistry* InRegistry,
                                               UCrowdyQuerySubsystem* InQuerySubsystem,
                                               UCrowdyGameSession* InGameSession)
{
	if (InRegistry) InRegistry->RegisterLayer(this);
	QuerySubsystem = InQuerySubsystem;
	GameSession    = InGameSession;
}

// Reception layer

TArray<EQueryResponseType> UCrowdyAuthentication::GetSupportedResponseType() const
{
	return {
		EQueryResponseType::Login,
		EQueryResponseType::Register,
		EQueryResponseType::RequestLoginLink,
		EQueryResponseType::CompleteLoginLink,
		EQueryResponseType::DevLogin,
		EQueryResponseType::MintAppToken,
		EQueryResponseType::RefreshAppToken,
		EQueryResponseType::SocialLoginStart,
		EQueryResponseType::SocialLoginComplete,
		EQueryResponseType::AvailableLoginProviders,
		EQueryResponseType::MyIdentities,
		EQueryResponseType::LinkIdentity,
		EQueryResponseType::UnlinkIdentity,
	};
}

void UCrowdyAuthentication::OnResponseReceived(TSharedPtr<ICrowdyQueryResponse> Response)
{
	if (!Response.IsValid()) return;
	FireCallback(Response);
}

// Sign-in entry points
//
// Each method dispatches its mutation and queues a callback that, on success,
// hands the SESSION token to the one shared mint pipeline (BeginMintPipeline).

void UCrowdyAuthentication::Login(const FString& Email, const FString& Password,
                                  FOnAuthSuccess OnSuccess, FOnAuthError OnError)
{
	if (!QuerySubsystem)
	{
		OnError.ExecuteIfBound(TEXT("Query subsystem unavailable"));
		return;
	}

	PushCallback(EQueryResponseType::Login, [this, OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
		{
			const FLoginResponse& R = static_cast<FLoginResponse&>(*Resp);
			// Under the two-token model R.token is the identity SESSION token.
			BeginMintPipeline(R.GameToken, R.GameTokenID, R.UserID, EAuthFlow::Login, OnSuccess, OnError);
		}
		else
		{
			FailFlow(Resp->GetError(), EAuthFlow::Login, OnError);
		}
	});

	TMap<FString, FString> Vars;
	Vars.Add(TEXT("email"),    Email);
	Vars.Add(TEXT("password"), Password);
	QuerySubsystem->ExecuteQueryByID(EGraphQLQuery::Login, Vars, false, false);
}

void UCrowdyAuthentication::Register(const FString& Email, const FString& Password,
                                     FOnAuthSuccess OnSuccess, FOnAuthError OnError)
{
	if (!QuerySubsystem)
	{
		OnError.ExecuteIfBound(TEXT("Query subsystem unavailable"));
		return;
	}

	PushCallback(EQueryResponseType::Register, [this, OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
		{
			const FRegisterResponse& R = static_cast<FRegisterResponse&>(*Resp);
			// FRegisterResponse carries no gameTokenId; 0 is a safe placeholder.
			BeginMintPipeline(R.GameToken, 0, R.UserID, EAuthFlow::Register, OnSuccess, OnError);
		}
		else
		{
			FailFlow(Resp->GetError(), EAuthFlow::Register, OnError);
		}
	});

	TMap<FString, FString> Vars;
	Vars.Add(TEXT("email"),    Email);
	Vars.Add(TEXT("password"), Password);
	QuerySubsystem->ExecuteQueryByID(EGraphQLQuery::Register, Vars, false, false);
}

void UCrowdyAuthentication::DevLogin(const FString& Email, FOnAuthSuccess OnSuccess, FOnAuthError OnError)
{
	if (!QuerySubsystem)
	{
		OnError.ExecuteIfBound(TEXT("Query subsystem unavailable"));
		return;
	}

	PushCallback(EQueryResponseType::DevLogin, [this, OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
		{
			const FAuthResponseBase& R = static_cast<FAuthResponseBase&>(*Resp);
			BeginMintPipeline(R.SessionToken, R.SessionGameTokenID, R.UserID, EAuthFlow::DevLogin, OnSuccess, OnError);
		}
		else
		{
			FailFlow(Resp->GetError(), EAuthFlow::DevLogin, OnError);
		}
	});

	FDevLoginRequest Request;
	Request.Email = Email;
	Request.PrepareQuery();
	QuerySubsystem->ExecuteQueryWithBody(Request.GetQueryType(), Request.InlineQueryBody,
		Request.RuntimeVariables, Request.bIncludeAuthToken);
}

void UCrowdyAuthentication::RequestLoginLink(const FString& Email, const FString& RedirectUri,
                                             FOnLoginLinkSent OnLinkSent, FOnAuthError OnError)
{
	if (!QuerySubsystem)
	{
		OnError.ExecuteIfBound(TEXT("Query subsystem unavailable"));
		return;
	}

	PushCallback(EQueryResponseType::RequestLoginLink, [OnLinkSent, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
		{
			const FRequestLoginLinkResponse& R = static_cast<FRequestLoginLinkResponse&>(*Resp);
			OnLinkSent.ExecuteIfBound(R.bSent, R.DevToken);
		}
		else
		{
			OnError.ExecuteIfBound(Resp->GetError());
		}
	});

	FRequestLoginLinkRequest Request;
	Request.Email       = Email;
	Request.RedirectUri = RedirectUri;
	Request.PrepareQuery();
	QuerySubsystem->ExecuteQueryWithBody(Request.GetQueryType(), Request.InlineQueryBody,
		Request.RuntimeVariables, Request.bIncludeAuthToken);
}

void UCrowdyAuthentication::CompleteLoginLink(const FString& Token, FOnAuthSuccess OnSuccess, FOnAuthError OnError)
{
	if (!QuerySubsystem)
	{
		OnError.ExecuteIfBound(TEXT("Query subsystem unavailable"));
		return;
	}

	PushCallback(EQueryResponseType::CompleteLoginLink, [this, OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
		{
			const FAuthResponseBase& R = static_cast<FAuthResponseBase&>(*Resp);
			BeginMintPipeline(R.SessionToken, R.SessionGameTokenID, R.UserID, EAuthFlow::MagicLink, OnSuccess, OnError);
		}
		else
		{
			FailFlow(Resp->GetError(), EAuthFlow::MagicLink, OnError);
		}
	});

	FCompleteLoginLinkRequest Request;
	Request.Token = Token;
	Request.PrepareQuery();
	QuerySubsystem->ExecuteQueryWithBody(Request.GetQueryType(), Request.InlineQueryBody,
		Request.RuntimeVariables, Request.bIncludeAuthToken);
}

void UCrowdyAuthentication::BeginMagicLinkSignIn(const FString& Email, FOnAuthSuccess OnSuccess, FOnAuthError OnError)
{
	if (!QuerySubsystem)
	{
		FailFlow(TEXT("Query subsystem unavailable"), EAuthFlow::MagicLink, OnError);
		return;
	}
	if (Email.IsEmpty())
	{
		FailFlow(TEXT("Email is required"), EAuthFlow::MagicLink, OnError);
		return;
	}

	// Reject a re-entrant call while a magic-link flow is already armed. Otherwise re-Start()ing the
	// listener would strand the first flow's queued GraphQL callback and could mis-route its
	// OnSuccess/OnError to the second attempt's listener.
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
	// sign-in (M2) will pass the server-issued state here instead.
	const FString RedirectUri = LoopbackServer->Start(FString(), MagicLinkTimeoutSeconds,
		OnTokenCaptured, OnListenerError);
	if (RedirectUri.IsEmpty())
	{
		// Start already reported the failure via OnListenerError.
		return;
	}

	// Inline the requestLoginLink dispatch (rather than calling the public RequestLoginLink, whose
	// dynamic delegate cannot take a lambda) so we can branch on the dev shortcut.
	PushCallback(EQueryResponseType::RequestLoginLink,
		[this, OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (!Resp->IsValid())
		{
			if (LoopbackServer.IsValid()) { LoopbackServer->Stop(); }
			FailFlow(Resp->GetError(), EAuthFlow::MagicLink, OnError);
			return;
		}

		const FRequestLoginLinkResponse& R = static_cast<FRequestLoginLinkResponse&>(*Resp);

		if (!R.DevToken.IsEmpty())
		{
			// Dev: no email/browser round-trip — complete immediately and drop the listener.
			if (LoopbackServer.IsValid()) { LoopbackServer->Stop(); }
			CompleteLoginLink(R.DevToken, OnSuccess, OnError);
			return;
		}

		// Prod: the email is on its way. The armed listener captures the link's redirect and
		// OnSuccess fires later from CompleteLoginLink's mint; nothing to do here but wait.
		UE_CLOG(CrowdyServicesTrace::Services(), LogCrowdyServices, Log,
			TEXT("[CrowdyAuth] Magic-link email requested; awaiting loopback callback."));
	});

	FRequestLoginLinkRequest Request;
	Request.Email       = Email;
	Request.RedirectUri = RedirectUri;
	Request.PrepareQuery();
	QuerySubsystem->ExecuteQueryWithBody(Request.GetQueryType(), Request.InlineQueryBody,
		Request.RuntimeVariables, Request.bIncludeAuthToken);
}

// Social sign-in + identities

void UCrowdyAuthentication::GetAvailableLoginProviders(FOnLoginProvidersReceived OnResult, FOnAuthError OnError)
{
	if (!QuerySubsystem)
	{
		OnError.ExecuteIfBound(TEXT("Query subsystem unavailable"));
		return;
	}

	PushCallback(EQueryResponseType::AvailableLoginProviders,
		[OnResult, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
		{
			const FAvailableLoginProvidersResponse& R = static_cast<FAvailableLoginProvidersResponse&>(*Resp);
			OnResult.ExecuteIfBound(R.Providers);
		}
		else
		{
			OnError.ExecuteIfBound(Resp->GetError());
		}
	});

	FAvailableLoginProvidersRequest Request;
	Request.PrepareQuery();
	QuerySubsystem->ExecuteQueryWithBody(Request.GetQueryType(), Request.InlineQueryBody,
		Request.RuntimeVariables, Request.bIncludeAuthToken);
}

void UCrowdyAuthentication::BeginSocialSignIn(const FString& Provider, FOnAuthSuccess OnSuccess, FOnAuthError OnError)
{
	if (!QuerySubsystem)
	{
		FailFlow(TEXT("Query subsystem unavailable"), EAuthFlow::Social, OnError);
		return;
	}
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
	// arrives in its response — so reserve the sticky loopback URI first and arm the route later.
	const FString RedirectUri = LoopbackServer->ReserveRedirectUri();
	if (RedirectUri.IsEmpty())
	{
		FailFlow(TEXT("Could not open a local sign-in port."), EAuthFlow::Social, OnError);
		return;
	}

	// Cover the reserve -> arm window; once armed, LoopbackServer->IsActive() takes over the guard.
	bLoopbackFlowPending = true;

	PushCallback(EQueryResponseType::SocialLoginStart,
		[this, Provider, OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (!Resp->IsValid())
		{
			bLoopbackFlowPending = false;
			FailFlow(Resp->GetError(), EAuthFlow::Social, OnError);
			return;
		}

		const FSocialLoginStartResponse& R = static_cast<FSocialLoginStartResponse&>(*Resp);
		const FString State        = R.State;
		const FString AuthorizeUrl = R.AuthorizeUrl;

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

		const FString ArmedUri = LoopbackServer.IsValid()
			? LoopbackServer->Start(State, SocialSignInTimeoutSeconds, OnCodeCaptured, OnListenerError)
			: FString();

		// The reserve -> arm window is now closed (either the listener is armed, or Start reported).
		bLoopbackFlowPending = false;

		if (ArmedUri.IsEmpty())
		{
			return; // Start already reported the failure via OnListenerError.
		}

		// Open the provider's consent page; its redirect lands on the armed listener.
		FPlatformProcess::LaunchURL(*AuthorizeUrl, nullptr, nullptr);

		UE_CLOG(CrowdyServicesTrace::Services(), LogCrowdyServices, Log,
			TEXT("[CrowdyAuth] Social sign-in: opened provider consent; awaiting loopback callback."));
	});

	FSocialLoginStartRequest Request;
	Request.Provider    = Provider;
	Request.RedirectUri = RedirectUri;
	Request.PrepareQuery();
	QuerySubsystem->ExecuteQueryWithBody(Request.GetQueryType(), Request.InlineQueryBody,
		Request.RuntimeVariables, Request.bIncludeAuthToken);
}

void UCrowdyAuthentication::CompleteSocialLogin(const FString& Provider, const FString& Code, const FString& State,
                                                FOnAuthSuccess OnSuccess, FOnAuthError OnError)
{
	if (!QuerySubsystem)
	{
		FailFlow(TEXT("Query subsystem unavailable"), EAuthFlow::Social, OnError);
		return;
	}

	PushCallback(EQueryResponseType::SocialLoginComplete,
		[this, OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
		{
			// Under the two-token model this token is the identity SESSION token.
			const FAuthResponseBase& R = static_cast<FAuthResponseBase&>(*Resp);
			BeginMintPipeline(R.SessionToken, R.SessionGameTokenID, R.UserID, EAuthFlow::Social, OnSuccess, OnError);
		}
		else
		{
			FailFlow(Resp->GetError(), EAuthFlow::Social, OnError);
		}
	});

	FSocialLoginCompleteRequest Request;
	Request.Provider = Provider;
	Request.Code     = Code;
	Request.State    = State;
	Request.PrepareQuery();
	QuerySubsystem->ExecuteQueryWithBody(Request.GetQueryType(), Request.InlineQueryBody,
		Request.RuntimeVariables, Request.bIncludeAuthToken);
}

void UCrowdyAuthentication::GetMyIdentities(FOnIdentitiesReceived OnResult, FOnAuthError OnError)
{
	if (!QuerySubsystem)
	{
		OnError.ExecuteIfBound(TEXT("Query subsystem unavailable"));
		return;
	}

	PushCallback(EQueryResponseType::MyIdentities,
		[OnResult, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
		{
			const FMyIdentitiesResponse& R = static_cast<FMyIdentitiesResponse&>(*Resp);
			OnResult.ExecuteIfBound(R.Identities);
		}
		else
		{
			OnError.ExecuteIfBound(Resp->GetError());
		}
	});

	FMyIdentitiesRequest Request;
	Request.PrepareQuery();
	QuerySubsystem->ExecuteQueryWithBody(Request.GetQueryType(), Request.InlineQueryBody,
		Request.RuntimeVariables, Request.bIncludeAuthToken);
}

void UCrowdyAuthentication::BeginLinkIdentity(const FString& Provider, FOnIdentityLinked OnResult, FOnAuthError OnError)
{
	if (!QuerySubsystem)
	{
		OnError.ExecuteIfBound(TEXT("Query subsystem unavailable"));
		return;
	}
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

	PushCallback(EQueryResponseType::SocialLoginStart,
		[this, Provider, OnResult, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (!Resp->IsValid())
		{
			bLoopbackFlowPending = false;
			OnError.ExecuteIfBound(Resp->GetError());
			return;
		}

		const FSocialLoginStartResponse& R = static_cast<FSocialLoginStartResponse&>(*Resp);
		const FString State        = R.State;
		const FString AuthorizeUrl = R.AuthorizeUrl;

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

		const FString ArmedUri = LoopbackServer.IsValid()
			? LoopbackServer->Start(State, SocialSignInTimeoutSeconds, OnCodeCaptured, OnListenerError)
			: FString();

		bLoopbackFlowPending = false;

		if (ArmedUri.IsEmpty())
		{
			return;
		}

		FPlatformProcess::LaunchURL(*AuthorizeUrl, nullptr, nullptr);

		UE_CLOG(CrowdyServicesTrace::Services(), LogCrowdyServices, Log,
			TEXT("[CrowdyAuth] Link identity: opened provider consent; awaiting loopback callback."));
	});

	FSocialLoginStartRequest Request;
	Request.Provider    = Provider;
	Request.RedirectUri = RedirectUri;
	Request.PrepareQuery();
	QuerySubsystem->ExecuteQueryWithBody(Request.GetQueryType(), Request.InlineQueryBody,
		Request.RuntimeVariables, Request.bIncludeAuthToken);
}

void UCrowdyAuthentication::CompleteLinkIdentity(const FString& Provider, const FString& Code, const FString& State,
                                                 FOnIdentityLinked OnResult, FOnAuthError OnError)
{
	if (!QuerySubsystem)
	{
		OnError.ExecuteIfBound(TEXT("Query subsystem unavailable"));
		return;
	}

	PushCallback(EQueryResponseType::LinkIdentity,
		[OnResult, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
		{
			const FLinkIdentityResponse& R = static_cast<FLinkIdentityResponse&>(*Resp);
			OnResult.ExecuteIfBound(R.Identity);
		}
		else
		{
			OnError.ExecuteIfBound(Resp->GetError());
		}
	});

	FLinkIdentityRequest Request;
	Request.Provider = Provider;
	Request.Code     = Code;
	Request.State    = State;
	Request.PrepareQuery();
	QuerySubsystem->ExecuteQueryWithBody(Request.GetQueryType(), Request.InlineQueryBody,
		Request.RuntimeVariables, Request.bIncludeAuthToken);
}

void UCrowdyAuthentication::UnlinkIdentity(const FString& IdentityId, FOnIdentityUnlinked OnResult, FOnAuthError OnError)
{
	if (!QuerySubsystem)
	{
		OnError.ExecuteIfBound(TEXT("Query subsystem unavailable"));
		return;
	}
	if (IdentityId.IsEmpty())
	{
		OnError.ExecuteIfBound(TEXT("An identityId is required"));
		return;
	}

	PushCallback(EQueryResponseType::UnlinkIdentity,
		[OnResult, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
		{
			const FUnlinkIdentityResponse& R = static_cast<FUnlinkIdentityResponse&>(*Resp);
			OnResult.ExecuteIfBound(R.bRemoved);
		}
		else
		{
			OnError.ExecuteIfBound(Resp->GetError());
		}
	});

	FUnlinkIdentityRequest Request;
	Request.IdentityId = IdentityId;
	Request.PrepareQuery();
	QuerySubsystem->ExecuteQueryWithBody(Request.GetQueryType(), Request.InlineQueryBody,
		Request.RuntimeVariables, Request.bIncludeAuthToken);
}

bool UCrowdyAuthentication::IsInteractiveSignInBusy() const
{
	return bLoopbackFlowPending || (LoopbackServer.IsValid() && LoopbackServer->IsActive());
}

// Shared mint pipeline

void UCrowdyAuthentication::BeginMintPipeline(const FString& SessionToken, int64 SessionGameTokenID, int64 UserID,
                                              EAuthFlow Flow, FOnAuthSuccess OnSuccess, FOnAuthError OnError)
{
	// Store the SESSION token on the management plane only. It is never handed to
	// the Game API / UDP path — that is what an app-scoped token (minted below) is for.
	if (GameSession)
	{
		GameSession->SetUserID(UserID);
		GameSession->SetSessionGameTokenID(SessionGameTokenID);
		GameSession->SetSessionToken(SessionToken);
	}
	if (QuerySubsystem)
	{
		QuerySubsystem->SetSessionToken(SessionToken);
	}

	// Persist the SESSION token only (durable, mint-capable). The app token is
	// short-lived and stays in memory.
	SaveSession(SessionToken, SessionGameTokenID, UserID);

	DispatchMint(Flow, OnSuccess, OnError);
}

void UCrowdyAuthentication::DispatchMint(EAuthFlow Flow, FOnAuthSuccess OnSuccess, FOnAuthError OnError)
{
	if (!QuerySubsystem || !GameSession)
	{
		FailFlow(TEXT("SDK not initialised"), Flow, OnError);
		return;
	}

	const int64 AppID = GameSession->GetAppID();
	if (AppID <= 0)
	{
		FailFlow(TEXT("Invalid AppID for app-token mint"), Flow, OnError);
		return;
	}

	PushCallback(EQueryResponseType::MintAppToken, [this, Flow, OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
		{
			ApplyAppTokenAndFinish(static_cast<FAppTokenResponseBase&>(*Resp), Flow, OnSuccess);
		}
		else
		{
			FailFlow(Resp->GetError(), Flow, OnError);
		}
	});

	FMintAppTokenRequest Request;
	Request.AppID = AppID;
	Request.PrepareQuery();
	QuerySubsystem->ExecuteQueryWithBody(Request.GetQueryType(), Request.InlineQueryBody,
		Request.RuntimeVariables, Request.bIncludeAuthToken);
}

void UCrowdyAuthentication::DispatchRefresh()
{
	if (!QuerySubsystem)
		return;

	PushCallback(EQueryResponseType::RefreshAppToken, [this](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
		{
			ApplyAppTokenAndFinish(static_cast<FAppTokenResponseBase&>(*Resp), EAuthFlow::Refresh, FOnAuthSuccess());
		}
		else
		{
			// Refresh needs a still-valid app token as bearer; if it lapsed, re-mint
			// from the (longer-lived) session token instead.
			UE_LOG(LogCrowdyServices, Warning, TEXT("[CrowdyAuth] refreshAppToken failed (%s); re-minting from session."),
				*Resp->GetError());
			DispatchMint(EAuthFlow::Refresh, FOnAuthSuccess(), FOnAuthError());
		}
	});

	FRefreshAppTokenRequest Request;
	Request.PrepareQuery();
	QuerySubsystem->ExecuteQueryWithBody(Request.GetQueryType(), Request.InlineQueryBody,
		Request.RuntimeVariables, Request.bIncludeAuthToken);
}

void UCrowdyAuthentication::ApplyAppTokenAndFinish(const FAppTokenResponseBase& Token, EAuthFlow Flow, FOnAuthSuccess OnSuccess)
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
	}
	if (QuerySubsystem)
	{
		QuerySubsystem->SetAppToken(Token.AppToken);
		// Adopt the per-app Game endpoint that came with the token (per-app routing).
		if (!GameApiGraphqlUrl.IsEmpty())
		{
			QuerySubsystem->SetGameEndpoint(GameApiGraphqlUrl);
		}
	}

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
	case EAuthFlow::DevLogin:
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
	case EAuthFlow::DevLogin:
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

// App-token refresh

void UCrowdyAuthentication::RefreshAppToken()
{
	if (!QuerySubsystem || !GameSession)
		return;

	if (GameSession->GetGameToken().IsEmpty() && GameSession->GetSessionToken().IsEmpty())
		return;

	if (IsTokenRotationInFlight())
		return;

	DispatchRefresh();
}

void UCrowdyAuthentication::RecoverExpiredAppToken()
{
	if (!QuerySubsystem || !GameSession)
		return;

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
	// Callers do a check-then-dispatch; that is race-free only because every rotation
	// trigger runs on the game thread (the FTSTicker fires there, and the err-32 path
	// is marshaled to it in UCrowdySDKSubsystem::HandleTokenExpired). Keep it that way.
	FScopeLock Lock(&CallbackMutex);
	const TArray<TFunction<void(TSharedPtr<ICrowdyQueryResponse>)>>* Mint =
		PendingCallbacks.Find(EQueryResponseType::MintAppToken);
	const TArray<TFunction<void(TSharedPtr<ICrowdyQueryResponse>)>>* Refresh =
		PendingCallbacks.Find(EQueryResponseType::RefreshAppToken);
	return (Mint && Mint->Num() > 0) || (Refresh && Refresh->Num() > 0);
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

	UE_CLOG(CrowdyServicesTrace::Services(), LogCrowdyServices, Log,
		TEXT("[CrowdyAuth] App-token refresh scheduled in %.0fs."), DelaySeconds);
}

void UCrowdyAuthentication::CancelProactiveRefresh()
{
	if (RefreshTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(RefreshTickerHandle);
		RefreshTickerHandle.Reset();
	}
}

// Session persistence
//
// The SESSION token is the long-lived, mint-capable credential, so it is encrypted at rest with
// DPAPI (per-user) rather than written to a plaintext SaveGame slot. The save object is serialized
// to a byte blob and that blob is encrypted; the schema (UCrowdyAuthSaveGame) is unchanged so the
// fields stay extensible.

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

	// Fallback: a pre-WP4b plaintext slot, so a returning user is not forced to sign in again. The
	// next SaveSession (the mint pipeline persists immediately) re-writes it encrypted and scrubs
	// the plaintext copy.
	if (UGameplayStatics::DoesSaveGameExist(LegacyAuthSlot, AuthUserIndex))
	{
		if (UCrowdyAuthSaveGame* Save = Cast<UCrowdyAuthSaveGame>(
			UGameplayStatics::LoadGameFromSlot(LegacyAuthSlot, AuthUserIndex)))
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
	// SDK requests UDP access — the same path as a fresh sign-in.
	BeginMintPipeline(Save->SessionToken, Save->SessionGameTokenID, Save->UserID, EAuthFlow::Restore, OnSuccess, OnError);
	return true;
}

void UCrowdyAuthentication::ClearSavedSession()
{
	CancelProactiveRefresh();

	FCrowdySecretFile::Delete(GetAuthVaultPath());

	// Also remove any legacy plaintext slot so logout fully forgets the credential.
	if (UGameplayStatics::DoesSaveGameExist(LegacyAuthSlot, AuthUserIndex))
	{
		UGameplayStatics::DeleteGameInSlot(LegacyAuthSlot, AuthUserIndex);
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
	// in the clear — it mints app tokens and is long-lived.
	TArray<uint8> Blob;
	const bool bSaved = UGameplayStatics::SaveGameToMemory(Save, Blob)
		&& FCrowdySecretFile::SaveBytes(GetAuthVaultPath(), Blob);

	if (bSaved)
	{
		UE_CLOG(CrowdyServicesTrace::Services(), LogCrowdyServices, Log,
			TEXT("[CrowdyAuth] Saved encrypted session vault. UserID=%lld"), UserID);

		// Scrub any pre-WP4b plaintext slot only once the encrypted copy is safely written, so a
		// write failure never destroys the user's only persisted session.
		if (UGameplayStatics::DoesSaveGameExist(LegacyAuthSlot, AuthUserIndex))
		{
			UGameplayStatics::DeleteGameInSlot(LegacyAuthSlot, AuthUserIndex);
		}
	}
	else
	{
		UE_LOG(LogCrowdyServices, Warning, TEXT("[CrowdyAuth] Failed to write encrypted session vault."));
	}
}

// Callback queue
//
// Responses arrive on a background thread via OnResponseReceived; each is matched
// FIFO to the callback queued when the matching request was sent, then run on the
// game thread.

void UCrowdyAuthentication::PushCallback(EQueryResponseType Type,
                                         TFunction<void(TSharedPtr<ICrowdyQueryResponse>)> Callback)
{
	FScopeLock Lock(&CallbackMutex);
	PendingCallbacks.FindOrAdd(Type).Add(MoveTemp(Callback));
}

void UCrowdyAuthentication::FireCallback(TSharedPtr<ICrowdyQueryResponse> Response)
{
	TFunction<void(TSharedPtr<ICrowdyQueryResponse>)> Callback;
	{
		FScopeLock Lock(&CallbackMutex);
		TArray<TFunction<void(TSharedPtr<ICrowdyQueryResponse>)>>* Queue =
			PendingCallbacks.Find(Response->GetResponseType());
		if (Queue && Queue->Num() > 0)
		{
			Callback = MoveTemp((*Queue)[0]);
			Queue->RemoveAt(0, 1, EAllowShrinking::No);
		}
	}

	if (Callback)
	{
		TSharedPtr<ICrowdyQueryResponse> Copy = Response;
		AsyncTask(ENamedThreads::GameThread, [Callback = MoveTemp(Callback), Copy]()
		{
			Callback(Copy);
		});
	}
}
