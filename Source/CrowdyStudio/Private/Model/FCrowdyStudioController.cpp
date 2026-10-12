// Fill out your copyright notice in the Description page of Project Settings.

#include "Model/FCrowdyStudioController.h"

#include "CrowdyStudioModule.h"
#include "Algo/AllOf.h"
#include "Auth/CrowdyAuthPayloadReaders.h"
#include "Auth/FCrowdyLoopbackAuthServer.h"
#include "Auth/FCrowdyTokenVault.h"
#include "ConfigSync/FCrowdyConfigSync.h"
#include "CrowdyCppClient.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Network/CrowdyCpp/CrowdyCppAdminClientHost.h"
#include "Gql/CrowdyStudioQueries.h"
#include "HAL/PlatformProcess.h"
#include "Misc/DateTime.h"
#include "Modules/ModuleManager.h"
#include "Settings/CrowdyStudioUserSettings.h"
#include "UI/CrowdyStudioConsent.h"
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "Web/FCrowdyStudioLinks.h"

namespace
{
	// Loopback wait windows for the interactive (browser) sign-ins. Mirrors the runtime auth constants:
	// magic-link waits for the user to open an emailed link; social waits on the provider consent page.
	static constexpr double MagicLinkTimeoutSeconds = 180.0;
	static constexpr double SocialSignInTimeoutSeconds = 300.0;

	// BigInt travels as a decimal string on the wire, never a JSON number.
	void SetBigIntField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, int64 Value)
	{
		Object->SetStringField(Field, FString::Printf(TEXT("%lld"), Value));
	}

	// A nullable Int cap: a positive value is sent as a JSON number; 0 (unlimited) is sent as JSON null
	// so the server clears any existing cap instead of treating it as a real zero.
	void SetCapField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, int32 Value)
	{
		if (Value > 0)
		{
			Object->SetNumberField(Field, Value);
		}
		else
		{
			Object->SetField(Field, MakeShared<FJsonValueNull>());
		}
	}

	void SetStringArrayField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		for (const FString& Value : Values)
		{
			Items.Add(MakeShared<FJsonValueString>(Value));
		}
		Object->SetArrayField(Field, Items);
	}

	// Optional string inputs are dropped when empty so a write leaves the server's existing value alone instead of
	// blanking it.
	void SetOptionalStringField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, const FString& Value)
	{
		if (!Value.IsEmpty())
		{
			Object->SetStringField(Field, Value);
		}
	}
}

FCrowdyStudioController::FCrowdyStudioController()
{
}

// Defined here (not defaulted in the header) so TUniquePtr<FCrowdyLoopbackAuthServer> can delete a type
// that is only forward-declared in the header - the loopback header is included in this .cpp.
FCrowdyStudioController::~FCrowdyStudioController()
{
}

void FCrowdyStudioController::Initialize()
{
	if (const UCrowdyStudioUserSettings* User = GetUserSettings())
	{
		SelectedOrgId = User->LastOrgId;
		SelectedAppId = User->LastAppId;
	}

	FString Saved;
	int32 SavedScope = 0;
	int64 SavedUserId = 0;
	if (FCrowdyTokenVault::Load(Saved, SavedScope, SavedUserId) && !Saved.IsEmpty())
	{
		// A remembered SESSION token is mint-capable, so restore it as session-scoped and run the same
		// tail as a fresh sign-in - that fetches apps and, via AnnounceAppContext, mints the app token
		// for the remembered app. Without this the token would be treated as an org token (which cannot
		// mint), so game-plane authoring would wrongly demand a re-sign-in after every editor launch.
		if (SavedScope == static_cast<int32>(ECrowdyStudioAuthScope::Session))
		{
			FinishSessionSignIn(Saved, SavedUserId, TEXT("Restored signed-in session."));
		}
		else
		{
			SignInWithToken(Saved);
		}
	}
}



void FCrowdyStudioController::SignInWithToken(const FString& OrgToken)
{
	if (OrgToken.IsEmpty())
	{
		SetStatus(TEXT("Enter an organization token to sign in."), true);
		return;
	}

	// An org token is management-only and can't mint; drop any app token a prior session sign-in left
	// so a game op can't bear it under this identity.
	ClearAppToken();

	AuthToken = OrgToken;
	AuthScope = ECrowdyStudioAuthScope::OrgToken;

	// The token proves itself by listing the organizations it can see.
	SendManagement(ECrowdyCppApiDomain::Organizations, TEXT("MyOrganizations"), nullptr,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseOrganizations(Envelope, Organizations);
			bSignedIn = true;
			FCrowdyTokenVault::Save(AuthToken, static_cast<int32>(ECrowdyStudioAuthScope::OrgToken), /*UserId*/ 0);
			if (UCrowdyStudioUserSettings* User = GetUserSettings())
			{
				User->bRememberToken = true;
				User->SaveConfig();
			}
			SetStatus(FString::Printf(TEXT("Signed in - %d organization(s)."), Organizations.Num()), false);
			OnSignInStateChanged.Broadcast();
			OnOrganizationsChanged.Broadcast();
			FetchAppsForSignedInUser();
		},
		[this]()
		{
			AuthToken.Empty();
			AuthScope = ECrowdyStudioAuthScope::None;
			bSignedIn = false;
			FCrowdyTokenVault::Clear();
			OnSignInStateChanged.Broadcast();
		});
}

FString FCrowdyStudioController::DescribeApiFailure(const FString& ErrorMessage, bool& bOutWasCanceled)
{
	bOutWasCanceled = ErrorMessage == FCrowdyCppClient::CanceledErrorMessage();
	if (bOutWasCanceled)
	{
		return TEXT("The request was cancelled.");
	}

	// The facade reports a bare non-2xx as "Server returned HTTP <code>". Recover the two the console has always
	// had specific advice for, since "sign in again" is the actionable half of an expired session.
	if (ErrorMessage.Contains(TEXT("HTTP 401")))
	{
		return TEXT("Authentication failed (HTTP 401). Sign in again.");
	}
	if (ErrorMessage.Contains(TEXT("HTTP 403")))
	{
		return TEXT("Permission denied (HTTP 403). Your account may lack access to this app.");
	}
	return ErrorMessage;
}

TSharedPtr<FCrowdyCppClient> FCrowdyStudioController::ResolveApiClient(const TCHAR* OperationName)
{
	const FString DiscoveryUrl = ResolveDiscoveryUrl();

	// Release any client retired earlier that has finished draining.
	RetiringApiClientHosts.RemoveAll([](const TSharedPtr<FCrowdyCppAdminClientHost>& Host)
	{
		return !Host.IsValid() || Host->GetClient()->NumPendingRequests() == 0;
	});

	// Keyed on the shared origin alone, because that is the only URL this client is built from. It changes on a
	// backend switch, which does have to rebuild rather than leave the client pointed at the previous server.
	if (ApiClientHost.IsValid() && ApiClientDiscoveryUrl != DiscoveryUrl)
	{
		// Retire the old client rather than releasing it. Only one endpoint usually moves, and releasing it here
		// would complete every request still in flight as canceled, including the ones aimed at the endpoint that
		// did not change. The retired host keeps its own ticker, so those requests still finish against the server
		// they were sent to. Clearing the bookkeeping first means a completion that re-enters this function sees a
		// clean slate rather than a half-torn-down one.
		RetiringApiClientHosts.Add(MoveTemp(ApiClientHost));
		ApiClientHost.Reset();
		ApiClientDiscoveryUrl.Reset();
		SignInRequest = FCrowdyCppRequestHandle();
		ProvidersRequest = FCrowdyCppRequestHandle();
	}

	if (!ApiClientHost.IsValid())
	{
		FCrowdyCppClientConfig ClientConfig;
		ClientConfig.DiscoveryUrl = DiscoveryUrl;

		// The shared origin, always, and never the stored per-app game URL.
		//
		// Those two are not interchangeable here even though one server now answers both. The shared origin is what
		// the backend selector names, so it is the environment the user actually chose. GameApiHttpUrl is derived
		// data written by an app sync: it can be stale, and it can name a different environment entirely, because
		// nothing forces an app synced earlier to belong to the backend selected now. Signing in against it sends
		// identity calls somewhere the user did not pick, and the way that surfaces is a schema error about a type
		// the other environment does not define rather than anything that names the real problem.
		//
		// The app-scoped work that genuinely needs the app's own datacenter is not served from here: those flows
		// build their own client for the endpoint the mint returns, and anything issued on this one that has to run
		// elsewhere gets a redirect the client follows on its own.
		ClientConfig.ApiUrl = DiscoveryUrl;

		ApiClientHost = FCrowdyCppAdminClientHost::Create(ClientConfig, FString());
		if (!ApiClientHost.IsValid())
		{
			UE_LOG(LogCrowdyStudio, Warning,
				TEXT("[studio] Could not construct the API client; the operation cannot proceed."));
			return nullptr;
		}
		ApiClientDiscoveryUrl = DiscoveryUrl;
	}
	else if (ApiClientDiscoveryUrl != DiscoveryUrl)
	{
		// Only reachable when a cancellation delivered during the disposal above rebuilt the client for a different
		// endpoint. Issuing on it would send this request somewhere it did not ask for, so refuse rather than aim
		// at the wrong server.
		UE_LOG(LogCrowdyStudio, Warning,
			TEXT("[studio] The API client was rebuilt for a different endpoint mid-teardown; not issuing on it."));
		return nullptr;
	}

	const TSharedRef<FCrowdyCppClient> Client = ApiClientHost->GetClient();

	// Installed per resolve rather than once, so a sign-in or a mint that lands after the client was built is picked
	// up, and neither plane can inherit whichever token the previous caller left behind. The sign-in mutations
	// themselves carry no bearer; the facade decides that per operation.
	Client->SetManagementToken(AuthToken);
	Client->SetGameToken(GameAppToken);

	if (CrowdyStudioTrace::Enabled())
	{
		UE_LOG(LogCrowdyStudio, Log, TEXT("[studio] op '%s' -> %s (discovery %s, shared API client)"),
			OperationName ? OperationName : TEXT("?"), *Client->GetApiEndpoint(), *DiscoveryUrl);
	}
	return Client;
}

void FCrowdyStudioController::LoginWithEmail(const FString& Email, const FString& Password)
{
	if (Email.IsEmpty() || Password.IsEmpty())
	{
		SetStatus(TEXT("Enter both an email and a password."), true);
		return;
	}

	const TSharedPtr<FCrowdyCppClient> Client = ResolveApiClient(TEXT("login"));
	if (!Client.IsValid())
	{
		SetStatus(TEXT("Authentication client unavailable."), true);
		return;
	}

	// Only once there is a client to sign in with: dropping the current identity first would leave the console
	// showing a signed-in session that no longer holds a bearer.
	AuthToken.Empty();
	AuthScope = ECrowdyStudioAuthScope::None;
	ClearAppToken();

	// A second attempt supersedes the first, so a slow wrong-password try cannot land after a correct one and
	// overwrite the identity just signed in as.
	Client->Cancel(SignInRequest);
	BeginRequest();
	TWeakPtr<FCrowdyStudioController> WeakThis = AsShared();
	SignInRequest = Client->SignInWithPassword(Email, Password,
		[WeakThis](FCrowdyCppAuthResult Result)
		{
			TSharedPtr<FCrowdyStudioController> Self = WeakThis.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			Self->EndRequest();
			Self->SignInRequest = FCrowdyCppRequestHandle();
			if (!Result.bOk)
			{
				bool bWasCanceled = false;
				const FString Message = DescribeApiFailure(Result.ErrorMessage, bWasCanceled);
				if (!bWasCanceled)
				{
					Self->SetStatus(Message, true);
				}
				return;
			}
			Self->FinishSessionSignIn(Result.SessionToken, Result.UserID, TEXT("Logged in."));
		});
}

void FCrowdyStudioController::FinishSessionSignIn(const FString& Token, int64 InUserId, const FString& StatusMsg)
{
	AuthToken = Token;
	AuthScope = ECrowdyStudioAuthScope::Session;
	UserId = InUserId;
	bSignedIn = true;
	FCrowdyTokenVault::Save(AuthToken, static_cast<int32>(ECrowdyStudioAuthScope::Session), UserId);
	if (UCrowdyStudioUserSettings* User = GetUserSettings())
	{
		User->bRememberToken = true;
		User->SaveConfig();
	}
	SetStatus(StatusMsg, false);
	OnSignInStateChanged.Broadcast();
	FetchMyOrganizations();
	FetchAppsForSignedInUser();
}

void FCrowdyStudioController::FetchAvailableProviders()
{
	// Best-effort and PUBLIC (no bearer is sent). A backend that does not support the query answers with an error;
	// degrade silently rather than greeting a signed-out user with a scary toast - the other sign-in options still
	// work, there are simply no social buttons.
	const TSharedPtr<FCrowdyCppClient> Client = ResolveApiClient(TEXT("availableLoginProviders"));
	if (!Client.IsValid())
	{
		// No client, so no providers and no buttons. Clear any stale list quietly.
		if (LoginProviders.Num() > 0)
		{
			LoginProviders.Reset();
			OnLoginProvidersChanged.Broadcast();
		}
		return;
	}

	// The probe runs on construction and again on a backend change; without superseding, the older one's
	// failure branch would blank the buttons the newer one just populated.
	Client->Cancel(ProvidersRequest);
	BeginRequest();
	TWeakPtr<FCrowdyStudioController> WeakThis = AsShared();
	ProvidersRequest = Client->ListLoginProviders(
		[WeakThis](FCrowdyCppStringListResult Result)
		{
			TSharedPtr<FCrowdyStudioController> Self = WeakThis.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			Self->EndRequest();
			Self->ProvidersRequest = FCrowdyCppRequestHandle();
			if (!Result.bOk)
			{
				// A cancellation means a newer probe is already running or the client was rebuilt, so leave the
				// current list alone rather than blanking buttons that are about to be repopulated.
				if (Result.ErrorMessage == FCrowdyCppClient::CanceledErrorMessage())
				{
					return;
				}
				// Silent degrade: no providers, no buttons, no toast.
				if (Self->LoginProviders.Num() > 0)
				{
					Self->LoginProviders.Reset();
					Self->OnLoginProvidersChanged.Broadcast();
				}
				return;
			}
			Self->LoginProviders = MoveTemp(Result.Values);
			Self->OnLoginProvidersChanged.Broadcast();
		});
}

void FCrowdyStudioController::SignInWithSocial(const FString& Provider)
{
	if (Provider.IsEmpty())
	{
		SetStatus(TEXT("Choose a sign-in provider."), true);
		return;
	}
	if (IsInteractiveSignInBusy())
	{
		SetStatus(TEXT("A browser sign-in is already in progress. Finish it or wait for it to time out."), true);
		return;
	}

	EnsureLoopback();

	// socialLoginStart needs the redirectUri now, but the CSRF state to arm the listener with only
	// arrives in its response - so reserve the sticky loopback URI first and arm the route later.
	const FString RedirectUri = LoopbackServer->ReserveRedirectUri();
	if (RedirectUri.IsEmpty())
	{
		SetStatus(TEXT("Could not open a local sign-in port. Close other Crowdy sessions and try again."), true);
		return;
	}

	// Cover the reserve -> arm window; once armed, LoopbackServer->IsActive() takes over the guard.
	bLoopbackFlowPending = true;

	const TSharedPtr<FCrowdyCppClient> Client = ResolveApiClient(TEXT("socialLoginStart"));
	if (!Client.IsValid())
	{
		// Nothing will ever call back into the listener reserved a moment ago, so drop it rather than let it hold
		// its port for the rest of the editor session (mirrors the equivalent branch in SignInWithMagicLink).
		LoopbackServer->Stop();
		// Clear the guard so the user can retry once the client is available.
		bLoopbackFlowPending = false;
		SetStatus(TEXT("Authentication client unavailable."), true);
		return;
	}

	// Drop any prior identity's tokens so nothing stale bleeds into this attempt, but only now that the attempt can
	// actually be made: clearing earlier would leave the console signed in with no bearer behind it.
	AuthToken.Empty();
	AuthScope = ECrowdyStudioAuthScope::None;
	ClearAppToken();

	BeginRequest();
	TWeakPtr<FCrowdyStudioController> WeakThis = AsShared();
	Client->SocialLoginStart(Provider, RedirectUri,
		[WeakThis, Provider](FCrowdyCppJsonValueResult Result)
		{
			TSharedPtr<FCrowdyStudioController> Self = WeakThis.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			Self->EndRequest();

			FString AuthorizeUrl;
			FString State;
			if (!Result.bOk || !CrowdyAuthPayloads::ReadSocialStartPayload(Result.Value, AuthorizeUrl, State))
			{
				Self->bLoopbackFlowPending = false;
				if (Result.bOk)
				{
					Self->SetStatus(TEXT("Could not start social sign-in (no authorize URL returned)."), true);
					return;
				}
				bool bWasCanceled = false;
				const FString Message = DescribeApiFailure(Result.ErrorMessage, bWasCanceled);
				if (!bWasCanceled)
				{
					Self->SetStatus(Message, true);
				}
				return;
			}
			Self->OnSocialLoginStarted(Provider, AuthorizeUrl, State);
		});
}

void FCrowdyStudioController::OnSocialLoginStarted(const FString& Provider, const FString& AuthorizeUrl,
                                                   const FString& State)
{
	// The state is the CSRF material the provider round-trips, and the listener treats an empty expected state as
	// "accept any callback", so arming without one would let anything else on this machine complete the sign-in.
	// The authorize URL is handed to the platform's URL opener, which on Windows is the shell, so it has to be a
	// web address rather than an arbitrary scheme.
	if (State.IsEmpty() || !CrowdyAuthPayloads::IsBrowserNavigableUrl(AuthorizeUrl))
	{
		bLoopbackFlowPending = false;
		SetStatus(TEXT("The sign-in provider returned an unusable authorization request."), true);
		return;
	}

	// Arm the listener bound to the server-issued state (CSRF). On the captured code, complete
	// the social sign-in; on listener error/timeout, surface it.
	FOnLoopbackToken OnCodeCaptured;
	OnCodeCaptured.BindLambda([this, Provider, State](const FString& Code)
	{
		CompleteSocialSignIn(Provider, Code, State);
	});
	FOnLoopbackError OnListenerError;
	OnListenerError.BindLambda([this](const FString& Message)
	{
		SetStatus(Message, true);
	});

	// The reserve -> arm window closes here either way, and it is cleared before Start so a handler that reacts to
	// a synchronous failure by retrying is not refused for a flow that has already ended.
	bLoopbackFlowPending = false;

	if (!LoopbackServer.IsValid())
	{
		SetStatus(TEXT("The local sign-in listener is unavailable."), true);
		return;
	}

	const FString ArmedUri = LoopbackServer->Start(State, SocialSignInTimeoutSeconds, OnCodeCaptured, OnListenerError);
	if (ArmedUri.IsEmpty())
	{
		return; // Start already reported the failure via OnListenerError.
	}

	// Open the provider's consent page in the SYSTEM browser (never an embedded webview - the
	// native-client docs are explicit, and providers reject embedded webviews). Its redirect
	// lands on the armed listener.
	FPlatformProcess::LaunchURL(*AuthorizeUrl, nullptr, nullptr);
	SetStatus(TEXT("Continue in your browser to finish signing in..."), false);
}

void FCrowdyStudioController::CompleteSocialSignIn(const FString& Provider, const FString& Code, const FString& State)
{
	const TSharedPtr<FCrowdyCppClient> Client = ResolveApiClient(TEXT("socialLoginComplete"));
	if (!Client.IsValid())
	{
		SetStatus(TEXT("Authentication client unavailable."), true);
		return;
	}

	Client->Cancel(SignInRequest);
	BeginRequest();
	TWeakPtr<FCrowdyStudioController> WeakThis = AsShared();
	SignInRequest = Client->SocialLoginComplete(Provider, Code, State,
		[WeakThis](FCrowdyCppAuthResult Result)
		{
			TSharedPtr<FCrowdyStudioController> Self = WeakThis.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			Self->EndRequest();
			Self->SignInRequest = FCrowdyCppRequestHandle();
			if (!Result.bOk)
			{
				bool bWasCanceled = false;
				const FString Message = DescribeApiFailure(Result.ErrorMessage, bWasCanceled);
				if (!bWasCanceled)
				{
					Self->SetStatus(Message, true);
				}
				return;
			}
			Self->FinishSessionSignIn(Result.SessionToken, Result.UserID, TEXT("Signed in."));
		});
}

void FCrowdyStudioController::SignInWithMagicLink(const FString& Email)
{
	if (Email.IsEmpty())
	{
		SetStatus(TEXT("Enter your email to get a sign-in link."), true);
		return;
	}
	if (IsInteractiveSignInBusy())
	{
		SetStatus(TEXT("A sign-in is already in progress. Finish it or wait for it to time out."), true);
		return;
	}

	EnsureLoopback();

	// Magic-link passes an EMPTY expected state (the one-time token is itself the single-use credential;
	// the server does not round-trip a state on this flow), so there is no reserve-then-arm split - arm
	// the listener directly and use the returned redirectUri for requestLoginLink.
	FOnLoopbackToken OnTokenCaptured;
	OnTokenCaptured.BindLambda([this](const FString& Token)
	{
		CompleteMagicLink(Token);
	});
	FOnLoopbackError OnListenerError;
	OnListenerError.BindLambda([this](const FString& Message)
	{
		SetStatus(Message, true);
	});

	const FString RedirectUri = LoopbackServer->Start(FString(), MagicLinkTimeoutSeconds,
		OnTokenCaptured, OnListenerError);
	if (RedirectUri.IsEmpty())
	{
		// Start already reported the failure via OnListenerError.
		return;
	}

	const TSharedPtr<FCrowdyCppClient> Client = ResolveApiClient(TEXT("requestLoginLink"));
	if (!Client.IsValid())
	{
		// Nothing will ever call back into the listener armed a moment ago, so drop it rather than let it linger
		// until timeout.
		LoopbackServer->Stop();
		SetStatus(TEXT("Authentication client unavailable."), true);
		return;
	}

	// Cleared only once the request can actually be sent, so a failure to obtain a client does not leave the console
	// signed in with no bearer behind it.
	AuthToken.Empty();
	AuthScope = ECrowdyStudioAuthScope::None;
	ClearAppToken();

	BeginRequest();
	TWeakPtr<FCrowdyStudioController> WeakThis = AsShared();
	Client->RequestLoginLink(Email, RedirectUri,
		[WeakThis](FCrowdyCppJsonValueResult Result)
		{
			TSharedPtr<FCrowdyStudioController> Self = WeakThis.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			Self->EndRequest();

			bool bSent = false;
			if (!Result.bOk || !CrowdyAuthPayloads::ReadLoginLinkPayload(Result.Value, bSent))
			{
				if (Self->LoopbackServer.IsValid())
				{
					Self->LoopbackServer->Stop();
				}
				if (!Result.bOk)
				{
					bool bWasCanceled = false;
					const FString Message = DescribeApiFailure(Result.ErrorMessage, bWasCanceled);
					if (!bWasCanceled)
					{
						Self->SetStatus(Message, true);
					}
				}
				else
				{
					Self->SetStatus(
						TEXT("Couldn't send a sign-in link. Check the email address and try again."), true);
				}
				return;
			}
			Self->OnLoginLinkRequested(bSent);
		});
}

void FCrowdyStudioController::OnLoginLinkRequested(bool bSent)
{
	if (!bSent)
	{
		// Nothing was actually sent (or a malformed 200): don't leave the listener armed for the
		// full timeout behind a misleading "check your email", and free the interactive guard so
		// the user can retry right away.
		if (LoopbackServer.IsValid())
		{
			LoopbackServer->Stop();
		}
		SetStatus(TEXT("Couldn't send a sign-in link. Check the email address and try again."), true);
		return;
	}

	// Prod: the email is on its way; the armed listener captures the link's redirect and the
	// sign-in completes later. Nothing to do here but tell the user to check their inbox.
	SetStatus(TEXT("Check your email for a sign-in link, then return here."), false);
}

void FCrowdyStudioController::CompleteMagicLink(const FString& Token, const FString& StatusMsg)
{
	const FString SuccessMsg = StatusMsg.IsEmpty() ? TEXT("Signed in.") : StatusMsg;

	const TSharedPtr<FCrowdyCppClient> Client = ResolveApiClient(TEXT("completeLoginLink"));
	if (!Client.IsValid())
	{
		SetStatus(TEXT("Authentication client unavailable."), true);
		return;
	}

	Client->Cancel(SignInRequest);
	BeginRequest();
	TWeakPtr<FCrowdyStudioController> WeakThis = AsShared();
	SignInRequest = Client->CompleteLoginLink(Token,
		[WeakThis, SuccessMsg](FCrowdyCppAuthResult Result)
		{
			TSharedPtr<FCrowdyStudioController> Self = WeakThis.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			Self->EndRequest();
			Self->SignInRequest = FCrowdyCppRequestHandle();
			if (!Result.bOk)
			{
				bool bWasCanceled = false;
				const FString Message = DescribeApiFailure(Result.ErrorMessage, bWasCanceled);
				if (!bWasCanceled)
				{
					Self->SetStatus(Message, true);
				}
				return;
			}
			Self->FinishSessionSignIn(Result.SessionToken, Result.UserID, SuccessMsg);
		});
}

void FCrowdyStudioController::EnsureLoopback()
{
	if (!LoopbackServer.IsValid())
	{
		LoopbackServer = MakeUnique<FCrowdyLoopbackAuthServer>();
	}
}

bool FCrowdyStudioController::IsInteractiveSignInBusy() const
{
	return bLoopbackFlowPending || (LoopbackServer.IsValid() && LoopbackServer->IsActive());
}

void FCrowdyStudioController::SignOut()
{
	FCrowdyTokenVault::Clear();

	AuthToken.Empty();
	AuthScope = ECrowdyStudioAuthScope::None;
	bSignedIn = false;
	UserId = 0;

	// Drop the credential from the transport too, and abandon anything still in flight under the old identity.
	// Clearing it only on the controller would leave the previous user's bearer installed on the client until some
	// later call happened to overwrite it.
	if (ApiClientHost.IsValid())
	{
		const TSharedRef<FCrowdyCppClient> Client = ApiClientHost->GetClient();
		Client->Cancel(SignInRequest);
		Client->Cancel(ProvidersRequest);
		Client->SetManagementToken(FString());
	}
	SignInRequest = FCrowdyCppRequestHandle();
	ProvidersRequest = FCrowdyCppRequestHandle();

	ClearAppToken();

	Organizations.Reset();
	Apps.Reset();
	ClearPlacementCache();
	SelectedOrgId = 0;
	// Cleared before the state below, not after: views compare the app they loaded for against the live selection to
	// decide whether their own editor panes are stale, and that comparison has to see the selection already gone.
	SelectedAppId = 0;

	// Signing out ends the app context as surely as switching apps does, so the previous identity's teams, channels,
	// grids and access lists must not stay on screen behind the sign-in page.
	ClearAppScopedState();

	if (UCrowdyStudioUserSettings* User = GetUserSettings())
	{
		User->bRememberToken = false;
		User->SaveConfig();
	}

	SetStatus(TEXT("Signed out."), false);
	OnSignInStateChanged.Broadcast();
	OnOrganizationsChanged.Broadcast();
	OnAppsChanged.Broadcast();
}



void FCrowdyStudioController::FetchMyOrganizations()
{
	SendManagement(ECrowdyCppApiDomain::Organizations, TEXT("MyOrganizations"), nullptr,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseOrganizations(Envelope, Organizations);
			SetStatus(FString::Printf(TEXT("%d organization(s)."), Organizations.Num()), false);
			OnOrganizationsChanged.Broadcast();
		});
}

void FCrowdyStudioController::CreateOrganization(const FString& Name, const FString& Slug)
{
	if (Name.IsEmpty() || Slug.IsEmpty())
	{
		SetStatus(TEXT("A new organization needs both a name and a slug."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("name"), Name);
	Input->SetStringField(TEXT("slug"), Slug);

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	SendManagement(ECrowdyCppApiDomain::Organizations, TEXT("CreateOrganization"), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			const TSharedPtr<FStudioOrg> Created = CrowdyStudioGql::ParseOrganization(Envelope, TEXT("createOrganization"));
			SetStatus(Created.IsValid()
				? FString::Printf(TEXT("Created organization '%s'."), *Created->Name)
				: TEXT("Created organization."), false);
			FetchMyOrganizations();
		});
}



void FCrowdyStudioController::FetchApps()
{
	// Counted rather than a bool: a sign-in and a manual refresh can both be in flight, and a bool would be cleared
	// by whichever finished first while the other was still running.
	++AppsFetchInFlight;

	// Broadcast on both paths, so the list repaints out of its loading state either way. A fetch that fails and
	// says nothing leaves a spinner turning for the rest of the session, which reads as a much worse problem than
	// the one that actually happened.
	SendManagement(ECrowdyCppApiDomain::Apps, TEXT("MyApps"), nullptr,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			ReleaseAppsFetch();
			// The list query carries neither the discovery fields nor the single-app detail, so a refresh
			// keeps what earlier per-app reads already filled in.
			TArray<TSharedPtr<FStudioApp>> Previous = MoveTemp(Apps);
			CrowdyStudioGql::ParseApps(Envelope, Apps);
			for (const TSharedPtr<FStudioApp>& App : Apps)
			{
				for (const TSharedPtr<FStudioApp>& Old : Previous)
				{
					if (Old->AppId != App->AppId) { continue; }
					App->DatacenterCode = Old->DatacenterCode;
					App->GameApiWsUrl = Old->GameApiWsUrl;
					App->DeploymentTarget = Old->DeploymentTarget;
					App->RuntimeStatus = Old->RuntimeStatus;
					App->RuntimeDenialReason = Old->RuntimeDenialReason;
					App->ReservedUdpBytesPerSec = Old->ReservedUdpBytesPerSec;
					App->ReservedGraphqlOpsPerSec = Old->ReservedGraphqlOpsPerSec;
					break;
				}
			}
			SetStatus(FString::Printf(TEXT("%d app(s)."), Apps.Num()), false);
			OnAppsChanged.Broadcast();
		},
		[this]()
		{
			ReleaseAppsFetch();
			OnAppsChanged.Broadcast();
		});
}

void FCrowdyStudioController::ReleaseAppsFetch()
{
	if (AppsFetchInFlight > 0)
	{
		--AppsFetchInFlight;
	}
}

void FCrowdyStudioController::CreateApp(int64 OrgId, const FString& Name, const FString& Slug,
	const FString& Datacenter, const FString& Description, TFunction<void(int64)> OnDone)
{
	auto Fail = [&OnDone](const TCHAR* Why, FCrowdyStudioController* Self)
	{
		Self->SetStatus(Why, true);
		if (OnDone) { OnDone(0); }
	};
	if (OrgId == 0) { Fail(TEXT("Select an organization before creating an app."), this); return; }
	if (Name.IsEmpty() || Slug.IsEmpty()) { Fail(TEXT("A new app needs both a name and a URL identifier."), this); return; }
	if (Datacenter.IsEmpty()) { Fail(TEXT("A new app needs a datacenter; the placement is permanent."), this); return; }

	const TSharedPtr<FJsonObject> Variables = CrowdyStudioGql::BuildCreateAppVariables(OrgId, Name, Slug, Datacenter, Description);

	SendManagement(ECrowdyCppApiDomain::Apps, TEXT("CreateApp"), Variables,
		[this, OnDone](const TSharedPtr<FJsonObject>& Envelope)
		{
			const TSharedPtr<FStudioApp> Created = CrowdyStudioGql::ParseApp(Envelope, TEXT("createApp"));
			SetStatus(Created.IsValid()
				? FString::Printf(TEXT("Created app '%s'."), *Created->Name)
				: TEXT("Created app."), false);
			if (Created.IsValid())
			{
				Apps.Insert(Created, 0);
				OnAppsChanged.Broadcast();
			}
			FetchApps();
			if (OnDone) { OnDone(Created.IsValid() ? Created->AppId : 0); }
		},
		[OnDone]()
		{
			if (OnDone) { OnDone(0); }
		});
}

void FCrowdyStudioController::UpdateApp(int64 AppId, const FString& Name, const TOptional<FString>& Description, const FString& Status, const FString& Visibility)
{
	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	if (!Name.IsEmpty())
	{
		Input->SetStringField(TEXT("name"), Name);
	}
	if (Description.IsSet())
	{
		Input->SetStringField(TEXT("description"), Description.GetValue());
	}
	if (!Status.IsEmpty())
	{
		Input->SetStringField(TEXT("status"), Status);
	}
	if (!Visibility.IsEmpty())
	{
		Input->SetStringField(TEXT("visibility"), Visibility);
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), AppId));
	Variables->SetObjectField(TEXT("input"), Input);

	SendManagement(ECrowdyCppApiDomain::Apps, TEXT("UpdateApp"), Variables,
		[this](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(TEXT("App updated."), false);
			FetchApps();
		});
}

void FCrowdyStudioController::ArchiveApp(int64 AppId)
{
	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), AppId));

	SendManagement(ECrowdyCppApiDomain::Apps, TEXT("ArchiveApp"), Variables,
		[this](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(TEXT("App archived."), false);
			FetchApps();
		});
}

void FCrowdyStudioController::FetchApp(int64 AppId)
{
	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), AppId));

	SendManagement(ECrowdyCppApiDomain::Apps, TEXT("App"), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			const TSharedPtr<FStudioApp> Detail = CrowdyStudioGql::ParseApp(Envelope, TEXT("app"));
			if (!Detail.IsValid())
			{
				return;
			}

			// Fold the endpoint detail back into the list entry the views already show. The discovery fields
			// (datacenter, WS endpoint) are not in the app query, so keep the ones already merged.
			bool bMerged = false;
			for (TSharedPtr<FStudioApp>& Existing : Apps)
			{
				if (Existing->AppId == Detail->AppId)
				{
					Detail->DatacenterCode = Existing->DatacenterCode;
					Detail->GameApiWsUrl = Existing->GameApiWsUrl;
					*Existing = *Detail;
					bMerged = true;
					break;
				}
			}
			if (!bMerged)
			{
				Apps.Add(Detail);
			}

			OnAppsChanged.Broadcast();
			FetchAppDiscovery(Detail->AppId);
		});
}

void FCrowdyStudioController::FetchAppDiscovery(int64 AppId)
{
	TArray<TSharedPtr<FJsonValue>> Ids;
	Ids.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), AppId)));
	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetArrayField(TEXT("appIds"), Ids);

	SendManagement(ECrowdyCppApiDomain::Apps, TEXT("AppDiscovery"), Variables,
		[this, AppId](const TSharedPtr<FJsonObject>& Envelope)
		{
			for (TSharedPtr<FStudioApp>& Existing : Apps)
			{
				if (Existing->AppId == AppId && CrowdyStudioGql::ParseAppDiscovery(Envelope, AppId, *Existing))
				{
					OnAppsChanged.Broadcast();
					return;
				}
			}
		},
		TFunction<void()>(), /*bReportErrors*/ false);
}

void FCrowdyStudioController::FetchPlaceableDatacenters()
{
	// Both paths broadcast: a fetch that fails and says nothing leaves the create sheet on "Loading
	// datacenters..." with no way to retry.
	SendManagement(ECrowdyCppApiDomain::Platform, TEXT("PlaceableDatacenters"), nullptr,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			bool bEnforced = false;
			CrowdyStudioGql::ParsePlaceableDatacenters(Envelope, PlaceableDatacenters, bEnforced);
			bPlaceableDatacentersFetched = true;
			bPlaceableDatacentersFailed = false;
			OnPlaceableDatacentersChanged.Broadcast();
		},
		[this]()
		{
			bPlaceableDatacentersFailed = true;
			OnPlaceableDatacentersChanged.Broadcast();
		});
}

void FCrowdyStudioController::ClearPlacementCache()
{
	PlaceableDatacenters.Reset();
	bPlaceableDatacentersFetched = false;
	bPlaceableDatacentersFailed = false;
	FreeAppsPerOrg = 0;
}

void FCrowdyStudioController::FetchPlatformConfig()
{
	SendManagement(ECrowdyCppApiDomain::Platform, TEXT("PlatformConfig"), nullptr,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			FreeAppsPerOrg = CrowdyStudioGql::ParseFreeAppsPerOrg(Envelope);
			OnAppsChanged.Broadcast();
		},
		TFunction<void()>(), /*bReportErrors*/ false);
}






void FCrowdyStudioController::SyncConfig()
{
	const TSharedPtr<FStudioApp> App = GetSelectedApp();
	if (!App.IsValid())
	{
		SetStatus(TEXT("Select an app before syncing to the project."), true);
		return;
	}

	FCrowdyConfigSync::ApplyAppToSettings(*App);
	const int32 LiveSessions = FCrowdyConfigSync::ApplyToRunningSessions();

	SetStatus(LiveSessions > 0
		? FString::Printf(TEXT("Synced to project settings and applied live to %d running session(s)."), LiveSessions)
		: TEXT("Synced to project settings. Takes effect on the next Play - no editor restart needed."), false);
	OnConfigChanged.Broadcast();
}

FStudioSettingsSnapshot FCrowdyStudioController::GetCurrentSettings() const
{
	return FCrowdyConfigSync::ReadCurrentSettings();
}

FStudioSettingsSnapshot FCrowdyStudioController::BuildProposedSettings() const
{
	// The single definition of what a sync writes lives in FCrowdyConfigSync, so the diff shown
	// here and the values actually written can't drift. With no app selected, nothing changes.
	if (const TSharedPtr<FStudioApp> App = GetSelectedApp())
	{
		return FCrowdyConfigSync::BuildProposedSettings(*App);
	}
	return GetCurrentSettings();
}

FString FCrowdyStudioController::GetBackendMode() const
{
	return FCrowdyConfigSync::GetBackendMode();
}

void FCrowdyStudioController::SetBackendMode(const FString& Mode)
{
	FCrowdyConfigSync::SetBackendMode(Mode);
	// The placement codes and free-slot count belong to one deployment; a Dev answer must not place a Prod app.
	ClearPlacementCache();
	SetStatus(TEXT("Backend changed. If your apps don't load, sign in again for this backend."), false);
	OnConfigChanged.Broadcast();
}

FString FCrowdyStudioController::GetCustomDiscoveryUrl() const
{
	return FCrowdyConfigSync::GetCustomDiscoveryUrl();
}

void FCrowdyStudioController::SetCustomDiscoveryUrl(const FString& Url)
{
	FCrowdyConfigSync::SetCustomDiscoveryUrl(Url);
	ClearPlacementCache();
	OnConfigChanged.Broadcast();
}

FString FCrowdyStudioController::GetEffectiveDiscoveryUrl() const
{
	return FCrowdyConfigSync::GetEffectiveDiscoveryUrl();
}


void FCrowdyStudioController::FetchTeams()
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app to list teams."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), SelectedAppId));

	SendGame(ECrowdyCppApiDomain::Teams, TEXT("Teams"), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseGroups(Envelope, TEXT("teams"), Teams);
			SetStatus(FString::Printf(TEXT("%d team(s)."), Teams.Num()), false);
			OnTeamsChanged.Broadcast();
		});
}

void FCrowdyStudioController::FetchTeamPolicy()
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app to read its team policy."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), SelectedAppId));

	SendGame(ECrowdyCppApiDomain::Teams, TEXT("TeamPolicy"), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseGroupPolicy(Envelope, TEXT("teamPolicy"), TeamPolicy);
			OnTeamPolicyChanged.Broadcast();
		});
}

void FCrowdyStudioController::SetTeamPolicy(const FString& CreationPolicy, const FString& DefaultMembershipPolicy, int32 MaxMembers, int32 MaxGroupsPerUser)
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app before setting team policy."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("appId"), SelectedAppId);
	Input->SetStringField(TEXT("creationPolicy"), CreationPolicy);
	Input->SetStringField(TEXT("defaultMembershipPolicy"), DefaultMembershipPolicy);
	SetCapField(Input, TEXT("maxMembers"), MaxMembers);
	SetCapField(Input, TEXT("maxGroupsPerUser"), MaxGroupsPerUser);

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	SendGame(ECrowdyCppApiDomain::Teams, TEXT("SetTeamPolicy"), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseGroupPolicy(Envelope, TEXT("setTeamPolicy"), TeamPolicy);
			SetStatus(TEXT("Team policy updated."), false);
			OnTeamPolicyChanged.Broadcast();
		});
}

void FCrowdyStudioController::FetchChannels()
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app to list channels."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), SelectedAppId));

	SendGame(ECrowdyCppApiDomain::Channels, TEXT("Channels"), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseGroups(Envelope, TEXT("channels"), Channels);
			SetStatus(FString::Printf(TEXT("%d channel(s)."), Channels.Num()), false);
			OnChannelsChanged.Broadcast();
		});
}

void FCrowdyStudioController::FetchChannelPolicy()
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app to read its channel policy."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), SelectedAppId));

	SendGame(ECrowdyCppApiDomain::Channels, TEXT("ChannelPolicy"), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseGroupPolicy(Envelope, TEXT("channelPolicy"), ChannelPolicy);
			OnChannelPolicyChanged.Broadcast();
		});
}

void FCrowdyStudioController::SetChannelPolicy(const FString& CreationPolicy, const FString& DefaultMembershipPolicy, int32 MaxMembers, int32 MaxGroupsPerUser)
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app before setting channel policy."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("appId"), SelectedAppId);
	Input->SetStringField(TEXT("creationPolicy"), CreationPolicy);
	Input->SetStringField(TEXT("defaultMembershipPolicy"), DefaultMembershipPolicy);
	SetCapField(Input, TEXT("maxMembers"), MaxMembers);
	SetCapField(Input, TEXT("maxGroupsPerUser"), MaxGroupsPerUser);

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	SendGame(ECrowdyCppApiDomain::Channels, TEXT("SetChannelPolicy"), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseGroupPolicy(Envelope, TEXT("setChannelPolicy"), ChannelPolicy);
			SetStatus(TEXT("Channel policy updated."), false);
			OnChannelPolicyChanged.Broadcast();
		});
}

void FCrowdyStudioController::CreateChannel(const FString& Name, const FString& Description, bool bMembersCanSend, const FString& MembershipPolicy)
{
	if (SelectedAppId == 0 || Name.IsEmpty())
	{
		SetStatus(TEXT("A new channel needs a selected app and a name."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("appId"), SelectedAppId);
	Input->SetStringField(TEXT("name"), Name);
	Input->SetStringField(TEXT("description"), Description);
	Input->SetBoolField(TEXT("membersCanSend"), bMembersCanSend);
	// Empty membership policy lets the server fall back to the app's default channel policy.
	if (MembershipPolicy.IsEmpty())
	{
		Input->SetField(TEXT("membershipPolicy"), MakeShared<FJsonValueNull>());
	}
	else
	{
		Input->SetStringField(TEXT("membershipPolicy"), MembershipPolicy);
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	SendGame(ECrowdyCppApiDomain::Channels, TEXT("CreateChannel"), Variables,
		[this](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(TEXT("Created channel."), false);
			FetchChannels();
		});
}

void FCrowdyStudioController::CreateTeam(const FString& Name, const FString& Description, const FString& MembershipPolicy)
{
	if (SelectedAppId == 0 || Name.IsEmpty())
	{
		SetStatus(TEXT("A new team needs a selected app and a name."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("appId"), SelectedAppId);
	Input->SetStringField(TEXT("name"), Name);
	Input->SetStringField(TEXT("description"), Description);
	if (MembershipPolicy.IsEmpty())
	{
		Input->SetField(TEXT("membershipPolicy"), MakeShared<FJsonValueNull>());
	}
	else
	{
		Input->SetStringField(TEXT("membershipPolicy"), MembershipPolicy);
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	SendGame(ECrowdyCppApiDomain::Teams, TEXT("CreateTeam"), Variables,
		[this](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(TEXT("Created team."), false);
			FetchTeams();
		});
}

void FCrowdyStudioController::CreateSessionChannel()
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app before creating its session channel."), true);
		return;
	}

	// Must match the runtime's deterministic name (UCrowdyChannels::GetSessionChannelName) so every
	// client converges on the same channel: __crowdy_session_<appId>.
	const FString SessionName = FString::Printf(TEXT("__crowdy_session_%lld"), SelectedAppId);
	CreateChannel(SessionName, TEXT("Reliable-RPC session channel (auto)"), /*bMembersCanSend*/ true, TEXT("open"));
}

void FCrowdyStudioController::SelectGroup(ECrowdyGroupKind Kind, int64 GroupId)
{
	SelectedGroupKind = Kind;
	SelectedGroupId = GroupId;
	GroupMembers.Reset();
	GroupRoles.Reset();
	OnGroupDetailChanged.Broadcast();

	if (GroupId == 0)
	{
		return;
	}

	const bool bChannel = (Kind == ECrowdyGroupKind::Channel);
	const TCHAR* MembersOp = bChannel ? TEXT("channelMembers") : TEXT("teamMembers");
	const TCHAR* RolesOp = bChannel ? TEXT("channelRoles") : TEXT("teamRoles");
	const ECrowdyCppApiDomain GroupDomain = bChannel ? ECrowdyCppApiDomain::Channels : ECrowdyCppApiDomain::Teams;

	const TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	SetBigIntField(Vars, TEXT("groupId"), GroupId);

	SendGame(GroupDomain, bChannel ? TEXT("ChannelMembers") : TEXT("TeamMembers"), Vars,
		[this, GroupId, MembersOp](const TSharedPtr<FJsonObject>& Envelope)
		{
			if (SelectedGroupId != GroupId)
			{
				return; // selection moved on before the reply arrived; drop the stale data
			}
			CrowdyStudioGql::ParseGroupMembers(Envelope, MembersOp, GroupMembers);
			OnGroupDetailChanged.Broadcast();
		});

	SendGame(GroupDomain, bChannel ? TEXT("ChannelRoles") : TEXT("TeamRoles"), Vars,
		[this, GroupId, RolesOp](const TSharedPtr<FJsonObject>& Envelope)
		{
			if (SelectedGroupId != GroupId)
			{
				return;
			}
			CrowdyStudioGql::ParseGroupRoles(Envelope, RolesOp, GroupRoles);
			OnGroupDetailChanged.Broadcast();
		});
}

void FCrowdyStudioController::AddGroupMember(ECrowdyGroupKind Kind, int64 GroupId, int64 InUserId)
{
	if (GroupId == 0 || InUserId == 0)
	{
		SetStatus(TEXT("Adding a member needs a selected group and a user id."), true);
		return;
	}

	const bool bChannel = (Kind == ECrowdyGroupKind::Channel);
	const TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	SetBigIntField(Vars, TEXT("groupId"), GroupId);
	SetBigIntField(Vars, TEXT("userId"), InUserId);

	SendGame(bChannel ? ECrowdyCppApiDomain::Channels : ECrowdyCppApiDomain::Teams,
		bChannel ? TEXT("AddChannelMember") : TEXT("AddTeamMember"), Vars,
		[this, Kind, GroupId](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(TEXT("Member added."), false);
			SelectGroup(Kind, GroupId);
		});
}

void FCrowdyStudioController::RemoveGroupMember(ECrowdyGroupKind Kind, int64 GroupId, int64 InUserId)
{
	if (GroupId == 0 || InUserId == 0)
	{
		SetStatus(TEXT("Removing a member needs a selected group and a user id."), true);
		return;
	}

	const bool bChannel = (Kind == ECrowdyGroupKind::Channel);
	const TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	SetBigIntField(Vars, TEXT("groupId"), GroupId);
	SetBigIntField(Vars, TEXT("userId"), InUserId);

	SendGame(bChannel ? ECrowdyCppApiDomain::Channels : ECrowdyCppApiDomain::Teams,
		bChannel ? TEXT("RemoveChannelMember") : TEXT("RemoveTeamMember"), Vars,
		[this, Kind, GroupId](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(TEXT("Member removed."), false);
			SelectGroup(Kind, GroupId);
		});
}

void FCrowdyStudioController::SetGroupMemberRoles(ECrowdyGroupKind Kind, int64 GroupId, int64 InUserId, const TArray<int64>& RoleIds)
{
	if (GroupId == 0 || InUserId == 0)
	{
		SetStatus(TEXT("Setting roles needs a selected group and a user id."), true);
		return;
	}

	const bool bChannel = (Kind == ECrowdyGroupKind::Channel);
	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("groupId"), GroupId);
	SetBigIntField(Input, TEXT("userId"), InUserId);

	TArray<TSharedPtr<FJsonValue>> Ids;
	for (int64 RoleId : RoleIds)
	{
		Ids.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), RoleId)));
	}
	Input->SetArrayField(TEXT("roleIds"), Ids);

	const TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetObjectField(TEXT("input"), Input);

	SendGame(bChannel ? ECrowdyCppApiDomain::Channels : ECrowdyCppApiDomain::Teams,
		bChannel ? TEXT("SetChannelMemberRoles") : TEXT("SetTeamMemberRoles"), Vars,
		[this, Kind, GroupId](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(TEXT("Member roles updated."), false);
			SelectGroup(Kind, GroupId);
		});
}

void FCrowdyStudioController::CreateGroupRole(ECrowdyGroupKind Kind, int64 GroupId, const FString& RoleName, const TArray<FString>& Permissions, int32 Rank)
{
	if (GroupId == 0 || RoleName.IsEmpty())
	{
		SetStatus(TEXT("A new role needs a selected group and a role name."), true);
		return;
	}

	const bool bChannel = (Kind == ECrowdyGroupKind::Channel);
	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("groupId"), GroupId);
	Input->SetStringField(TEXT("roleName"), RoleName);
	SetStringArrayField(Input, TEXT("permissions"), Permissions);
	Input->SetNumberField(TEXT("rank"), Rank);

	const TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetObjectField(TEXT("input"), Input);

	SendGame(bChannel ? ECrowdyCppApiDomain::Channels : ECrowdyCppApiDomain::Teams,
		bChannel ? TEXT("CreateChannelRole") : TEXT("CreateTeamRole"), Vars,
		[this, Kind, GroupId](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(TEXT("Role created."), false);
			SelectGroup(Kind, GroupId);
		});
}

void FCrowdyStudioController::UpdateGroupRole(ECrowdyGroupKind Kind, int64 GroupRoleId, const FString& RoleName, const TArray<FString>& Permissions, int32 Rank)
{
	if (GroupRoleId == 0)
	{
		SetStatus(TEXT("Select a role to update."), true);
		return;
	}

	const bool bChannel = (Kind == ECrowdyGroupKind::Channel);
	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("groupRoleId"), GroupRoleId);
	// roleName is ignored server-side for system roles; omit when empty to leave it unchanged.
	if (!RoleName.IsEmpty())
	{
		Input->SetStringField(TEXT("roleName"), RoleName);
	}
	SetStringArrayField(Input, TEXT("permissions"), Permissions);
	Input->SetNumberField(TEXT("rank"), Rank);

	const TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetObjectField(TEXT("input"), Input);

	const int64 GroupId = SelectedGroupId;
	SendGame(bChannel ? ECrowdyCppApiDomain::Channels : ECrowdyCppApiDomain::Teams,
		bChannel ? TEXT("UpdateChannelRole") : TEXT("UpdateTeamRole"), Vars,
		[this, Kind, GroupId](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(TEXT("Role updated."), false);
			SelectGroup(Kind, GroupId);
		});
}

void FCrowdyStudioController::DeleteGroupRole(ECrowdyGroupKind Kind, int64 GroupRoleId)
{
	if (GroupRoleId == 0)
	{
		SetStatus(TEXT("Select a role to delete."), true);
		return;
	}

	const bool bChannel = (Kind == ECrowdyGroupKind::Channel);
	const TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	SetBigIntField(Vars, TEXT("groupRoleId"), GroupRoleId);

	const int64 GroupId = SelectedGroupId;
	SendGame(bChannel ? ECrowdyCppApiDomain::Channels : ECrowdyCppApiDomain::Teams,
		bChannel ? TEXT("DeleteChannelRole") : TEXT("DeleteTeamRole"), Vars,
		[this, Kind, GroupId](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(TEXT("Role deleted."), false);
			SelectGroup(Kind, GroupId);
		});
}

void FCrowdyStudioController::DeleteGroup(ECrowdyGroupKind Kind, int64 GroupId)
{
	if (GroupId == 0)
	{
		SetStatus(TEXT("Select a team or channel to delete."), true);
		return;
	}

	const bool bChannel = (Kind == ECrowdyGroupKind::Channel);
	const TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	SetBigIntField(Vars, TEXT("groupId"), GroupId);

	SendGame(bChannel ? ECrowdyCppApiDomain::Channels : ECrowdyCppApiDomain::Teams,
		bChannel ? TEXT("DeleteChannel") : TEXT("DeleteTeam"), Vars,
		[this, bChannel](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(bChannel ? TEXT("Channel deleted.") : TEXT("Team deleted."), false);

			// The selected group is gone: clear the shared detail panels and refresh the list.
			SelectedGroupId = 0;
			GroupMembers.Reset();
			GroupRoles.Reset();
			OnGroupDetailChanged.Broadcast();
			if (bChannel) { FetchChannels(); } else { FetchTeams(); }
		});
}

void FCrowdyStudioController::UpdateGroup(ECrowdyGroupKind Kind, int64 GroupId, const FString& Name, const FString& Description, const FString& MembershipPolicy)
{
	if (GroupId == 0)
	{
		SetStatus(TEXT("Select a team or channel to edit."), true);
		return;
	}

	const bool bChannel = (Kind == ECrowdyGroupKind::Channel);
	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("groupId"), GroupId);
	// Partial update: only send the fields the user filled in; the server leaves omitted ones as-is.
	if (!Name.IsEmpty())             { Input->SetStringField(TEXT("name"), Name); }
	if (!Description.IsEmpty())      { Input->SetStringField(TEXT("description"), Description); }
	if (!MembershipPolicy.IsEmpty()) { Input->SetStringField(TEXT("membershipPolicy"), MembershipPolicy); }

	const TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetObjectField(TEXT("input"), Input);

	SendGame(bChannel ? ECrowdyCppApiDomain::Channels : ECrowdyCppApiDomain::Teams,
		bChannel ? TEXT("UpdateChannel") : TEXT("UpdateTeam"), Vars,
		[this, bChannel](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(bChannel ? TEXT("Channel updated.") : TEXT("Team updated."), false);
			if (bChannel) { FetchChannels(); } else { FetchTeams(); }
		});
}

void FCrowdyStudioController::FetchNearbyGrids(int64 InUserId, int64 LowX, int64 LowY, int64 LowZ, int64 HighX, int64 HighY, int64 HighZ)
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app to scan for grids."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Low = MakeShared<FJsonObject>();
	SetBigIntField(Low, TEXT("x"), LowX);
	SetBigIntField(Low, TEXT("y"), LowY);
	SetBigIntField(Low, TEXT("z"), LowZ);

	const TSharedPtr<FJsonObject> High = MakeShared<FJsonObject>();
	SetBigIntField(High, TEXT("x"), HighX);
	SetBigIntField(High, TEXT("y"), HighY);
	SetBigIntField(High, TEXT("z"), HighZ);

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("appId"), SelectedAppId);
	SetBigIntField(Input, TEXT("userId"), InUserId);
	Input->SetObjectField(TEXT("lowChunk"), Low);
	Input->SetObjectField(TEXT("highChunk"), High);

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	SendGame(ECrowdyCppApiDomain::GameApps, TEXT("NearbyGridPermissions"), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseNearbyGrids(Envelope, NearbyGrids);
			SetStatus(FString::Printf(TEXT("%d grid(s) in region."), NearbyGrids.Num()), false);
			OnNearbyGridsChanged.Broadcast();
		});
}

namespace
{
	// Turn a createGrid UDP error code into a sentence a developer can act on. The server returns the
	// enum name (e.g. NO_MATCHING_GRID_ASSIGNMENT); unknown codes pass through unchanged.
	FString DescribeGridError(const FString& Code)
	{
		if (Code == TEXT("NO_MATCHING_GRID_ASSIGNMENT"))
		{
			return TEXT("NO_MATCHING_GRID_ASSIGNMENT - those chunk coordinates are not inside any of the app's world bounds (grid assignments). A new grid must fit within an assigned world region. Scan a region to find the app's default world-spanning grid and keep the corners inside it.");
		}
		if (Code == TEXT("GRID_OUTSIDE_ASSIGNMENT"))
		{
			return TEXT("GRID_OUTSIDE_ASSIGNMENT - the chunk range extends past the app's assigned world region. Shrink it so it fits entirely inside one.");
		}
		if (Code == TEXT("GRID_OVERLAPS_EXISTING"))
		{
			return TEXT("GRID_OVERLAPS_EXISTING - the chunk range overlaps an existing grid. Pick a range that does not overlap one.");
		}
		if (Code == TEXT("GRID_ALREADY_EXISTS"))
		{
			return TEXT("GRID_ALREADY_EXISTS - a grid already exists at these coordinates.");
		}
		if (Code == TEXT("INVALID_GRID_COORDINATES"))
		{
			return TEXT("INVALID_GRID_COORDINATES - the chunk coordinates are invalid.");
		}
		if (Code == TEXT("USER_NOT_APP_ADMIN"))
		{
			return TEXT("USER_NOT_APP_ADMIN - creating grids needs the manage_apps permission on the app's organization.");
		}
		return Code;
	}
}

void FCrowdyStudioController::CreateGrid(int64 C1X, int64 C1Y, int64 C1Z, int64 C2X, int64 C2Y, int64 C2Z)
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app before creating a grid."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Corner1 = MakeShared<FJsonObject>();
	SetBigIntField(Corner1, TEXT("x"), C1X);
	SetBigIntField(Corner1, TEXT("y"), C1Y);
	SetBigIntField(Corner1, TEXT("z"), C1Z);

	const TSharedPtr<FJsonObject> Corner2 = MakeShared<FJsonObject>();
	SetBigIntField(Corner2, TEXT("x"), C2X);
	SetBigIntField(Corner2, TEXT("y"), C2Y);
	SetBigIntField(Corner2, TEXT("z"), C2Z);

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("appId"), SelectedAppId);
	Input->SetObjectField(TEXT("corner1"), Corner1);
	Input->SetObjectField(TEXT("corner2"), Corner2);

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	SendGame(ECrowdyCppApiDomain::GameApps, TEXT("CreateGrid"), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			FStudioGrid Created;
			FString GridError;
			if (CrowdyStudioGql::ParseCreateGrid(Envelope, Created, GridError))
			{
				SetStatus(FString::Printf(TEXT("Created grid #%lld."), Created.GridId), false);
			}
			else
			{
				SetStatus(GridError.IsEmpty()
					? TEXT("Grid was not created.")
					: FString::Printf(TEXT("Grid not created: %s"), *DescribeGridError(GridError)), true);
			}
		});
}

void FCrowdyStudioController::FetchGridPermissionLimits(int64 GridId)
{
	if (SelectedAppId == 0 || GridId == 0)
	{
		SetStatus(TEXT("Select an app and a grid to read its whitelist."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), SelectedAppId);
	SetBigIntField(Variables, TEXT("gridId"), GridId);

	SendGame(ECrowdyCppApiDomain::GameApps, TEXT("GridPermissionLimits"), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseGridPermissionKeys(Envelope, TEXT("gridPermissionLimits"), GridWhitelistKeys);
			OnGridWhitelistChanged.Broadcast();
		});
}

void FCrowdyStudioController::SetGridPermissionLimits(int64 GridId, const TArray<FString>& PermissionKeys)
{
	if (SelectedAppId == 0 || GridId == 0)
	{
		SetStatus(TEXT("Select an app and a grid to set its whitelist."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("appId"), SelectedAppId);
	SetBigIntField(Input, TEXT("gridId"), GridId);
	SetStringArrayField(Input, TEXT("permissionKeys"), PermissionKeys);

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	SendGame(ECrowdyCppApiDomain::GameApps, TEXT("SetGridPermissionLimits"), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseGridPermissionKeys(Envelope, TEXT("setGridPermissionLimits"), GridWhitelistKeys);
			SetStatus(TEXT("Grid whitelist updated."), false);
			OnGridWhitelistChanged.Broadcast();
		});
}

void FCrowdyStudioController::FetchGridGroupGrants(int64 GridId, int64 GroupId)
{
	if (SelectedAppId == 0 || GridId == 0 || GroupId == 0)
	{
		SetStatus(TEXT("Select an app, grid, and group to list grants."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), SelectedAppId);
	SetBigIntField(Variables, TEXT("gridId"), GridId);
	SetBigIntField(Variables, TEXT("groupId"), GroupId);

	SendGame(ECrowdyCppApiDomain::GameApps, TEXT("GridGroupGrants"), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseGridGroupGrants(Envelope, TEXT("gridGroupGrants"), GridGroupGrants);
			SetStatus(FString::Printf(TEXT("%d group grant(s)."), GridGroupGrants.Num()), false);
			OnGridDetailChanged.Broadcast();
		});
}

void FCrowdyStudioController::AssignGroupToGrid(int64 GridId, int64 GroupId, bool bHasRole, int64 GroupRoleId, const TArray<FString>& PermissionKeys)
{
	if (SelectedAppId == 0 || GridId == 0 || GroupId == 0 || PermissionKeys.Num() == 0)
	{
		SetStatus(TEXT("A group grant needs an app, grid, group, and at least one key."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("appId"), SelectedAppId);
	SetBigIntField(Input, TEXT("gridId"), GridId);
	SetBigIntField(Input, TEXT("groupId"), GroupId);
	if (bHasRole)
	{
		SetBigIntField(Input, TEXT("groupRoleId"), GroupRoleId);
	}
	SetStringArrayField(Input, TEXT("permissionKeys"), PermissionKeys);

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	SendGame(ECrowdyCppApiDomain::GameApps, TEXT("AssignGroupToGrid"), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseGridGroupGrants(Envelope, TEXT("assignGroupToGrid"), GridGroupGrants);
			SetStatus(TEXT("Granted permissions to group."), false);
			OnGridDetailChanged.Broadcast();
		});
}

void FCrowdyStudioController::RevokeGroupFromGrid(int64 GridId, int64 GroupId, bool bHasRole, int64 GroupRoleId, const TArray<FString>& PermissionKeys)
{
	if (SelectedAppId == 0 || GridId == 0 || GroupId == 0)
	{
		SetStatus(TEXT("Select an app, grid, and group to revoke."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("appId"), SelectedAppId);
	SetBigIntField(Input, TEXT("gridId"), GridId);
	SetBigIntField(Input, TEXT("groupId"), GroupId);
	if (bHasRole)
	{
		SetBigIntField(Input, TEXT("groupRoleId"), GroupRoleId);
	}
	if (PermissionKeys.Num() > 0)
	{
		SetStringArrayField(Input, TEXT("permissionKeys"), PermissionKeys);
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	SendGame(ECrowdyCppApiDomain::GameApps, TEXT("RevokeGroupFromGrid"), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseGridGroupGrants(Envelope, TEXT("revokeGroupFromGrid"), GridGroupGrants);
			SetStatus(TEXT("Revoked group grant."), false);
			OnGridDetailChanged.Broadcast();
		});
}

void FCrowdyStudioController::FetchGridUserPermissions(int64 GridId, int64 InUserId)
{
	if (SelectedAppId == 0 || GridId == 0 || InUserId == 0)
	{
		SetStatus(TEXT("Select an app, grid, and user to read effective permissions."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), SelectedAppId);
	SetBigIntField(Variables, TEXT("gridId"), GridId);
	SetBigIntField(Variables, TEXT("userId"), InUserId);

	SendGame(ECrowdyCppApiDomain::GameApps, TEXT("GridUserPermissions"), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseGridPermissionKeys(Envelope, TEXT("gridUserPermissions"), GridUserEffectiveKeys);
			OnGridDetailChanged.Broadcast();
		});
}

void FCrowdyStudioController::GrantGridPermissions(int64 GridId, int64 InUserId, const TArray<FString>& PermissionKeys)
{
	if (SelectedAppId == 0 || GridId == 0 || InUserId == 0 || PermissionKeys.Num() == 0)
	{
		SetStatus(TEXT("A user grant needs an app, grid, user, and at least one key."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("appId"), SelectedAppId);
	SetBigIntField(Input, TEXT("gridId"), GridId);
	SetBigIntField(Input, TEXT("userId"), InUserId);
	SetStringArrayField(Input, TEXT("permissionKeys"), PermissionKeys);

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	SendGame(ECrowdyCppApiDomain::GameApps, TEXT("GrantGridPermissions"), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseGridPermissionKeys(Envelope, TEXT("grantGridPermissions"), GridUserEffectiveKeys);
			SetStatus(TEXT("Granted permissions to user."), false);
			OnGridDetailChanged.Broadcast();
		});
}

void FCrowdyStudioController::RevokeGridPermissions(int64 GridId, int64 InUserId, const TArray<FString>& PermissionKeys)
{
	if (SelectedAppId == 0 || GridId == 0 || InUserId == 0)
	{
		SetStatus(TEXT("Select an app, grid, and user to revoke."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("appId"), SelectedAppId);
	SetBigIntField(Input, TEXT("gridId"), GridId);
	SetBigIntField(Input, TEXT("userId"), InUserId);
	if (PermissionKeys.Num() > 0)
	{
		SetStringArrayField(Input, TEXT("permissionKeys"), PermissionKeys);
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	SendGame(ECrowdyCppApiDomain::GameApps, TEXT("RevokeGridPermissions"), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseGridPermissionKeys(Envelope, TEXT("revokeGridPermissions"), GridUserEffectiveKeys);
			SetStatus(TEXT("Revoked user grant."), false);
			OnGridDetailChanged.Broadcast();
		});
}

void FCrowdyStudioController::FetchRuntimePermissions()
{
	// The catalog is global and PUBLIC, so there is no app or sign-in guard. It rides the management
	// endpoint with whatever token is set (ignored when absent), independent of the game-plane grid ops.
	SendManagement(ECrowdyCppApiDomain::AppAccess, TEXT("RuntimePermissions"), nullptr,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseRuntimePermissions(Envelope, RuntimePermissions);
			OnRuntimePermissionsChanged.Broadcast();
		});
}

void FCrowdyStudioController::EnsureAccessListsLoaded()
{
	if (!bSignedIn || SelectedAppId == 0 || AccessListsAppId == SelectedAppId)
	{
		return;
	}

	// Marked before the reads go out: the caller runs on paint, so marking on reply would re-send every frame until then.
	AccessListsAppId = SelectedAppId;

	FetchFeatures();
	FetchTierFeatures();
	FetchAppAccessTiers();
}

void FCrowdyStudioController::FetchFeatures()
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app to list features."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), SelectedAppId);

	++AccessReadsInFlight;
	SendAppAccess(TEXT("AppFeatures"), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			ReleaseAccessRead();
			CrowdyStudioGql::ParseFeatures(Envelope, TEXT("gameModelFeatures"), Features);
			SetStatus(FString::Printf(TEXT("%d feature(s)."), Features.Num()), false);
			OnFeaturesChanged.Broadcast();
		},
		[this]() { ReleaseAccessRead(); });
}

void FCrowdyStudioController::DefineFeature(const FString& FeatureKey, const FString& Description)
{
	if (SelectedAppId == 0 || FeatureKey.IsEmpty())
	{
		SetStatus(TEXT("A feature needs an app and a feature key."), true);
		return;
	}
	// The same shape a Server Object function's Required Feature accepts, so every key defined here can be required.
	const bool bKeyShape = FeatureKey.Len() <= 64 && Algo::AllOf(FeatureKey, [](TCHAR Char)
	{
		return (Char < 128 && FChar::IsAlnum(Char)) || Char == TEXT('_') || Char == TEXT('-') || Char == TEXT('.');
	});
	if (!bKeyShape)
	{
		SetStatus(TEXT("A feature key is 1 to 64 letters, digits, underscores, hyphens and dots."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("appId"), SelectedAppId);
	Input->SetStringField(TEXT("featureKey"), FeatureKey);
	SetOptionalStringField(Input, TEXT("description"), Description);

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	++AccessWritesInFlight;
	SendAppAccess(TEXT("DefineAppFeature"), Variables,
		[this](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			ReleaseAccessWrite();
			SetStatus(TEXT("Saved feature."), false);
			FetchFeatures();
		},
		[this]() { ReleaseAccessWrite(); });
}

void FCrowdyStudioController::FetchTierFeatures()
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app to list tier-feature grants."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), SelectedAppId);

	++AccessReadsInFlight;
	SendAppAccess(TEXT("TierFeatures"), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			ReleaseAccessRead();
			CrowdyStudioGql::ParseTierFeatures(Envelope, TEXT("gameModelTierFeatures"), TierFeatures);
			SetStatus(FString::Printf(TEXT("%d tier-feature grant(s)."), TierFeatures.Num()), false);
			OnTierFeaturesChanged.Broadcast();
		},
		[this]() { ReleaseAccessRead(); });
}

void FCrowdyStudioController::FetchAppAccessTiers()
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app to list access tiers."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), SelectedAppId);

	++AccessReadsInFlight;
	SendManagementForApp(ECrowdyCppApiDomain::AppAccess, TEXT("AppAccessTiers"), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			ReleaseAccessRead();
			CrowdyStudioGql::ParseAccessTiers(Envelope, TEXT("appAccessTiers"), AccessTiers);
			OnAccessTiersChanged.Broadcast();
		},
		[this]() { ReleaseAccessRead(); });
}

void FCrowdyStudioController::GrantTierFeature(int64 TierId, const FString& FeatureKey)
{
	if (SelectedAppId == 0 || TierId == 0 || FeatureKey.IsEmpty())
	{
		SetStatus(TEXT("Granting a tier feature needs an app, tier id, and feature key."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("appId"), SelectedAppId);
	SetBigIntField(Input, TEXT("tierId"), TierId);
	Input->SetStringField(TEXT("featureKey"), FeatureKey);

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	++AccessWritesInFlight;
	SendAppAccess(TEXT("GrantTierFeature"), Variables,
		[this](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			ReleaseAccessWrite();
			SetStatus(TEXT("Granted feature to tier."), false);
			FetchTierFeatures();
		},
		[this]() { ReleaseAccessWrite(); });
}

void FCrowdyStudioController::RevokeTierFeature(int64 TierId, const FString& FeatureKey)
{
	if (SelectedAppId == 0 || TierId == 0 || FeatureKey.IsEmpty())
	{
		SetStatus(TEXT("Revoking a tier feature needs an app, tier id, and feature key."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("appId"), SelectedAppId);
	SetBigIntField(Input, TEXT("tierId"), TierId);
	Input->SetStringField(TEXT("featureKey"), FeatureKey);

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	++AccessWritesInFlight;
	SendAppAccess(TEXT("RevokeTierFeature"), Variables,
		[this](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			ReleaseAccessWrite();
			SetStatus(TEXT("Revoked feature from tier."), false);
			FetchTierFeatures();
		},
		[this]() { ReleaseAccessWrite(); });
}

bool FCrowdyStudioController::CanUseGamePlane() const
{
	return !NeedsAppTokenRefresh() || CanMintAppToken();
}

bool FCrowdyStudioController::CanUseAppAccess() const
{
	return AuthScope == ECrowdyStudioAuthScope::OrgToken || CanUseGamePlane();
}

bool FCrowdyStudioController::AreAccessListsLoading() const
{
	if (AccessReadsInFlight > 0)
	{
		return true;
	}
	return bSignedIn && SelectedAppId != 0 && AccessListsAppId != SelectedAppId;
}

void FCrowdyStudioController::ClearAppScopedState()
{
	// Empty every list and selection the OnSelectedAppChanged views render, so switching apps never leaves the
	// previous app's teams, channels, grids or access lists on screen while the new app's token is minted - or if that
	// mint fails (e.g. the app isn't provisioned on the game tier) and the reload is therefore suppressed.
	Teams.Reset();
	Channels.Reset();
	GroupMembers.Reset();
	GroupRoles.Reset();
	SelectedGroupId = 0;
	TeamPolicy = FStudioGroupPolicy();
	ChannelPolicy = FStudioGroupPolicy();

	NearbyGrids.Reset();
	GridWhitelistKeys.Reset();
	GridUserEffectiveKeys.Reset();
	GridGroupGrants.Reset();

	Features.Reset();
	TierFeatures.Reset();
	AccessTiers.Reset();
	// The three lists above now hold nothing, so the new app has not been loaded. Without this the Access page would
	// see its own "already loaded" mark still set to the previous app and never refill.
	AccessListsAppId = 0;
	AccessReadsInFlight = 0;
	AccessWritesInFlight = 0;

	OnTeamsChanged.Broadcast();
	OnChannelsChanged.Broadcast();
	OnGroupDetailChanged.Broadcast();
	OnTeamPolicyChanged.Broadcast();
	OnChannelPolicyChanged.Broadcast();
	OnNearbyGridsChanged.Broadcast();
	OnGridWhitelistChanged.Broadcast();
	OnGridDetailChanged.Broadcast();
	OnFeaturesChanged.Broadcast();
	OnTierFeaturesChanged.Broadcast();
	OnAccessTiersChanged.Broadcast();
}

void FCrowdyStudioController::SelectOrg(int64 OrgId)
{
	SelectedOrgId = OrgId;
	PersistSelection();
}

void FCrowdyStudioController::SelectApp(int64 AppId)
{
	SelectedAppId = AppId;

	// An app carries its own org, so selecting one sets the active org: no separate org pick needed.
	if (const TSharedPtr<FStudioApp> App = GetSelectedApp())
	{
		SelectedOrgId = App->OrgId;
	}
	PersistSelection();

	// A new app invalidates the previous app's game token and endpoint; AnnounceAppContext re-mints.
	ClearAppToken();

	ClearAppScopedState();

	FetchApp(AppId);

	// Mint the app token, then let app-scoped views (teams, channels) reload for the new app.
	AnnounceAppContext();
}

TSharedPtr<FStudioOrg> FCrowdyStudioController::GetSelectedOrg() const
{
	for (const TSharedPtr<FStudioOrg>& Org : Organizations)
	{
		if (Org.IsValid() && Org->OrgId == SelectedOrgId)
		{
			return Org;
		}
	}
	return nullptr;
}

TSharedPtr<FStudioApp> FCrowdyStudioController::GetSelectedApp() const
{
	for (const TSharedPtr<FStudioApp>& App : Apps)
	{
		if (App.IsValid() && App->AppId == SelectedAppId)
		{
			return App;
		}
	}
	return nullptr;
}

bool FCrowdyStudioController::HasOrgPermission(const FString& PermissionKey) const
{
	const TSharedPtr<FStudioOrg> Org = GetSelectedOrg();
	return Org.IsValid() && Org->Permissions.Contains(PermissionKey);
}

bool FCrowdyStudioController::CanManageApps() const
{
	return HasOrgPermission(TEXT("manage_apps"));
}



FString FCrowdyStudioController::ResolveDiscoveryBaseUrl() const
{
	// Single source of truth: the same enum-driven URL the runtime uses, so the editor and
	// the packaged game always talk to the same backend.
	FString Base;
	if (const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>())
	{
		Base = Settings->GetDiscoveryUrl();
	}
	Base.RemoveFromEnd(TEXT("/"));
	return Base;
}

FString FCrowdyStudioController::ResolveDiscoveryUrl() const
{
	return ResolveDiscoveryBaseUrl() + TEXT("/graphql");
}

FString FCrowdyStudioController::ResolveGameUrl() const
{
	// The mint response carries the authoritative per-app game endpoint; prefer it over the
	// settings-derived URL, which can be stale for the selected app (it comes from the myApps record).
	if (!GameApiUrlOverride.IsEmpty())
	{
		return GameApiUrlOverride;
	}

	// The game endpoint already includes the /graphql path (unlike the management base URL).
	if (const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>())
	{
		return Settings->GetGameApiHttpUrl();
	}
	return FString();
}

void FCrowdyStudioController::IssueOperation(ECrowdyCppApiDomain Domain, const TCHAR* OperationName,
	ECrowdyCppTokenPlane Plane, const TSharedPtr<FJsonObject>& Variables,
	TFunction<void(const TSharedPtr<FJsonObject>&)> OnSuccess, TFunction<void()> OnFailure, bool bReportErrors)
{
	const FString OpName(OperationName ? OperationName : TEXT(""));

	const TSharedPtr<FCrowdyCppClient> Client = ResolveApiClient(OperationName);
	if (!Client.IsValid())
	{
		// The client is the only way to the server, so a resolve that fails has to answer the caller right here.
		// Returning without doing so would leave a plan run waiting on a reply that can never arrive.
		if (bReportErrors)
		{
			SetStatus(TEXT("Could not construct the API client; the operation was not sent."), true);
		}
		// Not a cancellation: the client simply couldn't be built.
		bLastFailureWasCanceled = false;
		if (OnFailure)
		{
			OnFailure();
		}
		return;
	}

	BeginRequest();

	TWeakPtr<FCrowdyStudioController> WeakThis = AsShared();
	Client->RunOp(Domain, OpName, Variables,
		[WeakThis, OpName, OnSuccess = MoveTemp(OnSuccess), OnFailure = MoveTemp(OnFailure), bReportErrors]
		(FCrowdyCppJsonResult Result)
		{
			TSharedPtr<FCrowdyStudioController> Self = WeakThis.Pin();
			if (!Self.IsValid())
			{
				return;
			}

			Self->EndRequest();

			if (CrowdyStudioTrace::Enabled())
			{
				UE_LOG(LogCrowdyStudio, Log, TEXT("[studio] '%s' response ok=%d"), *OpName, Result.bTransportOk ? 1 : 0);
			}

			if (!Result.bTransportOk)
			{
				bool bWasCanceled = false;
				const FString Error = DescribeApiFailure(Result.ErrorMessage, bWasCanceled);
				// A cancellation is the console retiring its own client, not the server answering, so it is not
				// shown as something the user did wrong. The caller is still told, because whatever it was
				// counting still has to be unwound.
				if (bReportErrors && !bWasCanceled)
				{
					Self->SetStatus(Error, true);
				}
				Self->bLastFailureWasCanceled = bWasCanceled;
				if (OnFailure)
				{
					OnFailure();
				}
				return;
			}

			if (OnSuccess)
			{
				OnSuccess(CrowdyStudioGql::WrapDataEnvelope(Result.Data));
			}
		},
		Plane);
}

void FCrowdyStudioController::SendManagement(ECrowdyCppApiDomain Domain, const TCHAR* OperationName,
	const TSharedPtr<FJsonObject>& Variables,
	TFunction<void(const TSharedPtr<FJsonObject>&)> OnSuccess, TFunction<void()> OnFailure, bool bReportErrors)
{
	IssueOperation(Domain, OperationName, ECrowdyCppTokenPlane::Management, Variables,
		MoveTemp(OnSuccess), MoveTemp(OnFailure), bReportErrors);
}

void FCrowdyStudioController::SendManagementForApp(ECrowdyCppApiDomain Domain, const TCHAR* OperationName,
	const TSharedPtr<FJsonObject>& Variables,
	TFunction<void(const TSharedPtr<FJsonObject>&)> OnSuccess, TFunction<void()> OnFailure)
{
	const int64 IssuedForAppId = SelectedAppId;
	SendManagement(Domain, OperationName, Variables,
		[this, IssuedForAppId, Inner = MoveTemp(OnSuccess)](const TSharedPtr<FJsonObject>& Envelope)
		{
			if (!Inner || IssuedForAppId != SelectedAppId)
			{
				return;
			}
			Inner(Envelope);
		},
		[this, IssuedForAppId, Inner = MoveTemp(OnFailure)]()
		{
			if (!Inner || IssuedForAppId != SelectedAppId)
			{
				return;
			}
			Inner();
		});
}

void FCrowdyStudioController::SendAppAccess(const TCHAR* OperationName, const TSharedPtr<FJsonObject>& Variables,
	TFunction<void(const TSharedPtr<FJsonObject>&)> OnSuccess, TFunction<void()> OnFailure)
{
	if (AuthScope == ECrowdyStudioAuthScope::OrgToken)
	{
		SendManagementForApp(ECrowdyCppApiDomain::AppAccess, OperationName, Variables, MoveTemp(OnSuccess), MoveTemp(OnFailure));
		return;
	}

	SendGame(ECrowdyCppApiDomain::AppAccess, OperationName, Variables, MoveTemp(OnSuccess), MoveTemp(OnFailure));
}

void FCrowdyStudioController::SendGame(ECrowdyCppApiDomain Domain, const TCHAR* OperationName,
	const TSharedPtr<FJsonObject>& Variables,
	TFunction<void(const TSharedPtr<FJsonObject>&)> OnSuccess, TFunction<void()> OnFailure)
{
	// This is the single definition of "app-scoped": every operation issued here names one app in its variables and
	// reads or writes that app alone. Pin the app selected right now and drop the caller's completions if the
	// selection has moved by the time the reply arrives - the app switch has already emptied the state those
	// completions write to, and letting a late reply run would repopulate the previous app's data under the new
	// app's name. Only the CALLER's completions are dropped: the busy count, the status latch and the cancellation
	// flag are unwound by IssueOperation before either of these is reached, so nothing stays latched.
	//
	// A pinned id of 0 means no app was selected when the op was issued, so there is nothing for the reply to be
	// stale against and it is always delivered.
	const int64 IssuedForAppId = SelectedAppId;
	auto HasAppMovedOn = [this, IssuedForAppId]()
	{
		return IssuedForAppId != 0 && IssuedForAppId != SelectedAppId;
	};

	TFunction<void(const TSharedPtr<FJsonObject>&)> ScopedSuccess =
		[HasAppMovedOn, Inner = MoveTemp(OnSuccess)](const TSharedPtr<FJsonObject>& Envelope)
		{
			if (Inner && !HasAppMovedOn())
			{
				Inner(Envelope);
			}
		};
	TFunction<void()> ScopedFailure =
		[HasAppMovedOn, Inner = MoveTemp(OnFailure)]()
		{
			if (Inner && !HasAppMovedOn())
			{
				Inner();
			}
		};

	// Game-plane ops bear the app-scoped token, not the session token. When it is in hand and fresh,
	// post straight away; otherwise mint (or refresh) it first, then post. An org token can't mint,
	// so its game ops fail closed with a clear message instead of being silently rejected by the server.
	if (!NeedsAppTokenRefresh())
	{
		PostGame(Domain, OperationName, Variables, MoveTemp(ScopedSuccess), MoveTemp(ScopedFailure));
		return;
	}

	if (!CanMintAppToken())
	{
		SetStatus(TEXT("Game-plane actions (teams, channels, grids) need a session sign-in: "
			"sign in with email, dev, or a magic link. An organization token only covers account settings."), true);
		// Not a cancellation: there is no session sign-in to mint from.
		bLastFailureWasCanceled = false;
		ScopedFailure();
		return;
	}

	MintAppToken([this, Domain, OpName = FString(OperationName ? OperationName : TEXT("")), Variables, HasAppMovedOn,
		OnSuccess = MoveTemp(ScopedSuccess), OnFailure = MoveTemp(ScopedFailure)](bool bMinted)
	{
		if (HasAppMovedOn())
		{
			// The selection moved while the token was being minted. The variables still name the app this op was
			// built for, and its reply would be discarded on arrival, so there is nothing to send it for.
			return;
		}
		if (bMinted)
		{
			PostGame(Domain, *OpName, Variables, OnSuccess, OnFailure);
		}
		else
		{
			// MintAppToken already set a descriptive status (e.g. the app isn't provisioned on the game tier);
			// let the caller react to the failed op instead of it stalling with no callback. This is a mint
			// failure, not a cancellation of the op the caller actually asked for.
			bLastFailureWasCanceled = false;
			OnFailure();
		}
	});
}

void FCrowdyStudioController::PostGame(ECrowdyCppApiDomain Domain, const TCHAR* OperationName,
	const TSharedPtr<FJsonObject>& Variables,
	TFunction<void(const TSharedPtr<FJsonObject>&)> OnSuccess, TFunction<void()> OnFailure)
{
	if (ResolveGameUrl().IsEmpty())
	{
		SetStatus(TEXT("No game endpoint configured: sync an app to the project first."), true);
		// Not a cancellation: there is simply no endpoint to call.
		bLastFailureWasCanceled = false;
		if (OnFailure)
		{
			OnFailure();
		}
		return;
	}

	IssueOperation(Domain, OperationName, ECrowdyCppTokenPlane::Game, Variables,
		MoveTemp(OnSuccess), MoveTemp(OnFailure), true);
}

bool FCrowdyStudioController::CanMintAppToken() const
{
	return AuthScope == ECrowdyStudioAuthScope::Session && SelectedAppId != 0 && !AuthToken.IsEmpty();
}

bool FCrowdyStudioController::NeedsAppTokenRefresh() const
{
	if (GameAppToken.IsEmpty())
	{
		return true;
	}
	if (bHaveAppTokenExpiry)
	{
		// Refresh a touch early so an in-flight op doesn't straddle the expiry boundary.
		return FDateTime::UtcNow() >= (GameAppTokenExpiresAt - FTimespan::FromSeconds(60));
	}
	return false;
}

void FCrowdyStudioController::MintAppToken(TFunction<void(bool)> OnDone)
{
	if (!CanMintAppToken())
	{
		if (OnDone)
		{
			OnDone(false);
		}
		return;
	}

	// Queue this request and kick a mint only if one isn't already running. A burst of game ops (e.g.
	// FetchTeams + FetchTeamPolicy from one RefreshAll, both crossing the pre-expiry window) thus folds
	// into a single mintAppToken instead of racing two writes of GameAppToken.
	if (OnDone)
	{
		PendingMintWaiters.Add(MoveTemp(OnDone));
	}
	if (!bMintInFlight)
	{
		StartMint();
	}
}

void FCrowdyStudioController::StartMint(bool bConsentAsked)
{
	bMintInFlight = true;
	MintInFlightAppId = SelectedAppId;
	const int64 MintForAppId = MintInFlightAppId;

	const TSharedPtr<FCrowdyCppClient> Client = ResolveApiClient(TEXT("mintAppToken"));
	if (!Client.IsValid())
	{
		SetStatus(TEXT("Authentication client unavailable."), true);
		FinishMint(MintForAppId, false);
		return;
	}

	BeginRequest();
	TWeakPtr<FCrowdyStudioController> WeakThis = AsShared();
	Client->MintAppToken(MintForAppId,
		[WeakThis, MintForAppId, Client, bConsentAsked](FCrowdyCppAppTokenResult Result)
		{
			TSharedPtr<FCrowdyStudioController> Self = WeakThis.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			Self->EndRequest();
			if (CrowdyStudioConsent::ShouldAskConsent(Result, bConsentAsked))
			{
				Self->AskConsentThenRemint(MintForAppId, Client.ToSharedRef());
				return;
			}
			// An empty token is failed closed here rather than trusted: if the bridge's own response mapping ever
			// stopped rejecting it, an empty GameAppToken would still get installed, releasing every queued waiter
			// into PostGame with no Authorization header and re-minting on every subsequent call.
			if (!Result.bOk)
			{
				bool bWasCanceled = false;
				const FString Message = DescribeApiFailure(Result.ErrorMessage, bWasCanceled);
				if (!bWasCanceled)
				{
					Self->SetStatus(Message, true);
				}
				Self->FinishMint(MintForAppId, false);
				return;
			}
			if (Result.AppToken.IsEmpty())
			{
				Self->SetStatus(TEXT("The server minted an empty app token; treating it as unavailable."), true);
				Self->FinishMint(MintForAppId, false);
				return;
			}
			Self->ApplyMintedAppToken(MintForAppId, Result.AppToken, Result.GameApiUrl, Result.ExpiresAt);
		});
}

void FCrowdyStudioController::AskConsentThenRemint(int64 MintForAppId, const TSharedRef<FCrowdyCppClient>& Client)
{
	TWeakPtr<FCrowdyStudioController> WeakThis = AsShared();
	CrowdyStudioConsent::Ask(Client, [WeakThis, MintForAppId](bool bRecorded)
	{
		TSharedPtr<FCrowdyStudioController> Self = WeakThis.Pin();
		if (!Self.IsValid())
		{
			return;
		}
		if (!bRecorded)
		{
			Self->SetStatus(CrowdyStudioConsent::DeclinedMessage(), true);
			Self->FinishMint(MintForAppId, false);
			return;
		}
		if (!Self->CanMintAppToken())
		{
			Self->FinishMint(MintForAppId, false);
			return;
		}
		Self->StartMint(true);
	});
}

void FCrowdyStudioController::ApplyMintedAppToken(int64 MintForAppId, const FString& Token, const FString& GameUrl,
                                                  const FString& ExpiresAtStr)
{
	// A token for an app the user has since switched away from is stale, so it is discarded rather than installed;
	// FinishMint re-issues for the app that is actually selected now.
	if (MintForAppId != SelectedAppId)
	{
		FinishMint(MintForAppId, false);
		return;
	}

	GameAppToken = Token;
	if (!GameUrl.IsEmpty())
	{
		// The mint returns a bare host; the Game API GraphQL lives at /graphql (else 404).
		GameApiUrlOverride = CrowdyAuthPayloads::EnsureGameApiGraphqlPath(GameUrl);
	}

	FDateTime Parsed;
	bHaveAppTokenExpiry = !ExpiresAtStr.IsEmpty() && FDateTime::ParseIso8601(*ExpiresAtStr, Parsed);
	GameAppTokenExpiresAt = bHaveAppTokenExpiry ? Parsed : FDateTime();

	FinishMint(MintForAppId, true);
}

void FCrowdyStudioController::FinishMint(int64 MintedAppId, bool bReady)
{
	bMintInFlight = false;
	MintInFlightAppId = 0;

	// The selected app changed while this mint was in flight, so its token is for the wrong app. If a
	// waiter still needs one and we can mint, re-issue for the now-current app and carry the waiters
	// over; their continuation then runs against a token that actually matches the selection.
	if (!bReady && MintedAppId != SelectedAppId && PendingMintWaiters.Num() > 0 && CanMintAppToken())
	{
		StartMint();
		return;
	}

	TArray<TFunction<void(bool)>> Waiters = MoveTemp(PendingMintWaiters);
	PendingMintWaiters.Reset();
	for (TFunction<void(bool)>& Waiter : Waiters)
	{
		if (Waiter)
		{
			Waiter(bReady);
		}
	}
}

void FCrowdyStudioController::ClearAppToken()
{
	GameAppToken.Empty();
	GameApiUrlOverride.Empty();
	GameAppTokenExpiresAt = FDateTime();
	bHaveAppTokenExpiry = false;
}

void FCrowdyStudioController::AnnounceAppContext()
{
	// Game-plane views (teams, channels) reload on OnSelectedAppChanged and need the app-scoped token,
	// so mint it first and only announce once it is in hand. A session sign-in can mint; an org token
	// cannot, so there is nothing to announce for it - those game views can't load and keep their
	// on-screen "sign in with a session account" hint (a manual Refresh still surfaces a clear error).
	if (!CanMintAppToken())
	{
		return;
	}

	MintAppToken([this](bool bMinted)
	{
		if (bMinted)
		{
			OnSelectedAppChanged.Broadcast();
		}
		// On mint failure the status already explains why; don't fan the game views out into a storm
		// of fetches that would each fail the same way.
	});
}

void FCrowdyStudioController::SetStatus(const FString& Message, bool bIsError)
{
	StatusMessage = Message;
	bStatusWasError = bIsError;

	UE_CLOG(!bIsError, LogCrowdyStudio, Log, TEXT("%s"), *Message);
	UE_CLOG(bIsError, LogCrowdyStudio, Warning, TEXT("%s"), *Message);

	OnStatusMessage.Broadcast(Message, bIsError);
}

void FCrowdyStudioController::BeginRequest()
{
	++InFlightCount;
}

void FCrowdyStudioController::EndRequest()
{
	if (InFlightCount > 0)
	{
		--InFlightCount;
	}
}

void FCrowdyStudioController::FetchAppsForSignedInUser()
{
	FetchApps();
	FetchPlatformConfig();

	// On sign-in a remembered app is already selected (set in Initialize from saved settings), but it
	// never went through SelectApp, so mint its app token and announce it here too so the team/channel
	// views auto-load.
	if (SelectedAppId != 0)
	{
		AnnounceAppContext();
	}
}

void FCrowdyStudioController::PersistSelection() const
{
	if (UCrowdyStudioUserSettings* User = GetUserSettings())
	{
		User->LastOrgId = SelectedOrgId;
		User->LastAppId = SelectedAppId;
		User->SaveConfig();
	}
}

UCrowdyStudioUserSettings* FCrowdyStudioController::GetUserSettings() const
{
	return GetMutableDefault<UCrowdyStudioUserSettings>();
}
