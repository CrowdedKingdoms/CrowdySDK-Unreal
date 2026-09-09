// Fill out your copyright notice in the Description page of Project Settings.

#include "Model/FCrowdyStudioController.h"

#include "CrowdyStudioModule.h"
#include "CrowdyStudioSyncService.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Auth/CrowdyAuthPayloadReaders.h"
#include "Auth/FCrowdyLoopbackAuthServer.h"
#include "Auth/FCrowdyTokenVault.h"
#include "ConfigSync/FCrowdyConfigSync.h"
#include "CrowdyCppClient.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Network/CrowdyCpp/CrowdyCppAdminClientHost.h"
#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"
#include "GameModel/CrowdyContainerScan.h"
#include "GameModel/CrowdyEffectPlanCache.h"
#include "GameModel/CrowdySchemaSync.h"
#include "GameModel/CrowdyStudioFunctionMarshalling.h"
#include "Gql/CrowdyStudioQueries.h"
#include "HAL/PlatformProcess.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/GameModel/CrowdyModelIdentity.h"
#include "Replication/GameModel/CrowdyAttributeRegistry.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/DateTime.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/StrongObjectPtr.h"
#include "Replication/GameModel/Effect/CrowdyGameModelFunctionMarshaller.h"
#include "Replication/GameModel/Kit/CrowdyGameKitConfig.h"
#include "Replication/GameModel/Kit/CrowdyGameKitDeploy.h"
#include "Replication/GameModel/Kit/CrowdyGameKitPreview.h"
#include "Replication/GameModel/Kit/CrowdyKitLayerPreset.h"
#include "Settings/CrowdyStudioUserSettings.h"
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "Web/FCrowdyStudioLinks.h"

using namespace CrowdyStudioMarshalling;

namespace
{
	// Loopback wait windows for the interactive (browser) sign-ins. Mirrors the runtime auth constants:
	// magic-link waits for the user to open an emailed link; social waits on the provider consent page.
	static constexpr double MagicLinkTimeoutSeconds = 180.0;
	static constexpr double SocialSignInTimeoutSeconds = 300.0;

	// The per-project editor-ini section that remembers a deployed kit's container-type prefixes per app id, so schema
	// sync keeps protecting kit schema from prune across editor restarts (the authored kit config is session-only).
	const TCHAR* KitPrefixConfigSection = TEXT("CrowdyStudio.DeployedKitPrefixes");
	// The exact-name companions of KitPrefixConfigSection: the full container-type and function names a deploy seeded,
	// so an empty-prefix kit (whose bare names no prefix rule recognizes) is still protected from prune after a restart.
	const TCHAR* KitTypeNamesConfigSection = TEXT("CrowdyStudio.DeployedKitTypeNames");
	const TCHAR* KitFunctionNamesConfigSection = TEXT("CrowdyStudio.DeployedKitFunctionNames");

	// Read a comma-joined per-app string set from a per-project editor-ini section (the shared shape of the kit
	// prefix/type-name/function-name stores). Empty parts are culled and each entry trimmed.
	TSet<FString> LoadKitConfigSet(const TCHAR* Section, int64 AppId)
	{
		TSet<FString> Out;
		if (AppId == 0 || !GConfig)
		{
			return Out;
		}
		FString Joined;
		if (GConfig->GetString(Section, *LexToString(AppId), Joined, GEditorPerProjectIni) && !Joined.IsEmpty())
		{
			TArray<FString> Parts;
			Joined.ParseIntoArray(Parts, TEXT(","), /*CullEmpty*/ true);
			for (const FString& Part : Parts)
			{
				const FString Trimmed = Part.TrimStartAndEnd();
				if (!Trimmed.IsEmpty())
				{
					Out.Add(Trimmed);
				}
			}
		}
		return Out;
	}

	// Union Values into the section's existing per-app set and write it back (sorted, comma-joined), so re-deploying
	// one kit never drops another kit's protection for this app. A no-op for a zero app id or an empty value set.
	void PersistKitConfigSet(const TCHAR* Section, int64 AppId, const TSet<FString>& Values)
	{
		if (AppId == 0 || Values.Num() == 0 || !GConfig)
		{
			return;
		}
		TSet<FString> Merged = LoadKitConfigSet(Section, AppId);
		Merged.Append(Values);
		TArray<FString> Sorted = Merged.Array();
		Sorted.Sort();
		const FString Joined = FString::Join(Sorted, TEXT(","));
		GConfig->SetString(Section, *LexToString(AppId), *Joined, GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
	}

	// The effective container-type prefixes of a set of authored kit layers, applying the same empty->default fallback
	// the emit uses (only Guild has one: empty -> "Guild"). An empty prefix protects nothing (it cannot be told apart
	// from hand-authored schema), so it is dropped.
	void CollectEffectiveKitTypePrefixes(const TArray<TObjectPtr<UCrowdyKitLayerPreset>>& Layers, TSet<FString>& OutPrefixes)
	{
		for (const TObjectPtr<UCrowdyKitLayerPreset>& Layer : Layers)
		{
			const UCrowdyKitLayerPreset* Preset = Layer.Get();
			if (!Preset)
			{
				continue;
			}
			FString Prefix;
			if (const UCrowdyCombatPreset* Combat = Cast<UCrowdyCombatPreset>(Preset))
			{
				Prefix = Combat->TypePrefix;
			}
			else if (const UCrowdyLeaderboardsPreset* Boards = Cast<UCrowdyLeaderboardsPreset>(Preset))
			{
				Prefix = Boards->TypePrefix;
			}
			else if (const UCrowdyGuildPreset* Guild = Cast<UCrowdyGuildPreset>(Preset))
			{
				Prefix = Guild->TypePrefix.IsEmpty() ? FString(TEXT("Guild")) : Guild->TypePrefix;
			}
			else if (const UCrowdyLivingWorldPreset* World = Cast<UCrowdyLivingWorldPreset>(Preset))
			{
				Prefix = World->TypePrefix;
			}
			if (!Prefix.IsEmpty())
			{
				OutPrefixes.Add(Prefix);
			}
		}
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

	bool ParseJsonArray(const FString& JsonText, TArray<TSharedPtr<FJsonValue>>& OutArray)
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		return FJsonSerializer::Deserialize(Reader, OutArray);
	}

	bool ParseJsonObject(const FString& JsonText, TSharedPtr<FJsonObject>& OutObject)
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
	}

}

FCrowdyStudioController::FCrowdyStudioController()
{
}

// Defined here (not defaulted in the header) so TUniquePtr<FCrowdyLoopbackAuthServer> can delete a type
// that is only forward-declared in the header - the loopback header is included in this .cpp.
FCrowdyStudioController::~FCrowdyStudioController()
{
	// An outstanding effect stream completes into a lambda that reads this controller, so it must not outlive it.
	// Cancelling releases the delegate without running it.
	if (SchemaEffectStreamHandle.IsValid())
	{
		SchemaEffectStreamHandle->CancelHandle();
		SchemaEffectStreamHandle.Reset();
	}

	// The registry-scan wait completes into a lambda that re-enters this controller too, so unbind it first.
	if (SchemaRegistryFilesLoadedHandle.IsValid())
	{
		if (FAssetRegistryModule* AssetRegistryModule =
			FModuleManager::GetModulePtr<FAssetRegistryModule>(TEXT("AssetRegistry")))
		{
			AssetRegistryModule->Get().OnFilesLoaded().Remove(SchemaRegistryFilesLoadedHandle);
		}
		SchemaRegistryFilesLoadedHandle.Reset();
	}
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

	// Drop any schema-sync plan + the auto-create latch/readiness so a signed-out session never shows the previous
	// user's plan or leaves the latch set for a later plan to auto-apply. The registry wait goes with them: it is
	// the one plan continuation whose lifetime is open-ended, and its app-id guard alone would let a plan latched
	// under this identity fire under the next one to sign in and select the same app id.
	CancelSchemaRegistryWait();
	ClearSchemaSyncState();
	bAutoCreatingSessionChannel = false;
	SessionChannelReadiness = ECrowdyStudioReadiness::Unknown;
	// The retained plan verdicts go with the identity that produced them. They survive an app switch on purpose, but
	// the next person to sign in may have no access to those apps at all, and selecting one by the same id would
	// otherwise show them the previous user's schema.
	ModelSnapshotsByApp.Reset();

	Organizations.Reset();
	Apps.Reset();
	SelectedOrgId = 0;
	// Cleared before the state below, not after: views compare the app they loaded for against the live selection to
	// decide whether their own editor panes are stale, and that comparison has to see the selection already gone.
	SelectedAppId = 0;

	// Signing out ends the app context as surely as switching apps does, so the previous identity's teams, channels,
	// grids and game-model schema must not stay on screen behind the sign-in page.
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
			CrowdyStudioGql::ParseApps(Envelope, Apps);
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
	const FString& Status, const FString& Visibility)
{
	if (Name.IsEmpty() || Slug.IsEmpty())
	{
		SetStatus(TEXT("A new app needs both a name and a slug."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("orgId"), FString::Printf(TEXT("%lld"), OrgId));
	Input->SetStringField(TEXT("name"), Name);
	Input->SetStringField(TEXT("slug"), Slug);
	// status/visibility are the AppStatus/AppVisibility enums - passed as their value names.
	if (!Status.IsEmpty())
	{
		Input->SetStringField(TEXT("status"), Status);
	}
	if (!Visibility.IsEmpty())
	{
		Input->SetStringField(TEXT("visibility"), Visibility);
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	SendManagement(ECrowdyCppApiDomain::Apps, TEXT("CreateApp"), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			const TSharedPtr<FStudioApp> Created = CrowdyStudioGql::ParseApp(Envelope, TEXT("createApp"));
			SetStatus(Created.IsValid()
				? FString::Printf(TEXT("Created app '%s'."), *Created->Name)
				: TEXT("Created app."), false);
			FetchApps();
		});
}

void FCrowdyStudioController::UpdateApp(int64 AppId, const FString& Name, const FString& Status, const FString& Visibility)
{
	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	if (!Name.IsEmpty())
	{
		Input->SetStringField(TEXT("name"), Name);
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

			// Fold the endpoint detail back into the list entry the views already show.
			bool bMerged = false;
			for (TSharedPtr<FStudioApp>& Existing : Apps)
			{
				if (Existing->AppId == Detail->AppId)
				{
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
		});
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

	const FString SessionName = FString::Printf(TEXT("__crowdy_session_%lld"), SelectedAppId);
	SendGame(ECrowdyCppApiDomain::Channels, TEXT("CreateChannel"), Variables,
		[this, bIsSessionChannel = (Name == SessionName)](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			if (bIsSessionChannel)
			{
				// Keep the Game Model setup strip's Session-channel indicator in step when it is created here.
				SessionChannelReadiness = ECrowdyStudioReadiness::Ready;
				OnSchemaSyncReportChanged.Broadcast();
			}
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

ECrowdyModelLoadState FCrowdyStudioController::GetFamilyLoadState(ECrowdyModelFamily Family) const
{
	const int32 Slot = static_cast<int32>(Family);
	if (Slot < 0 || Slot >= static_cast<int32>(ECrowdyModelFamily::Count))
	{
		return ECrowdyModelLoadState::NeverRequested;
	}
	return FamilyLoads[Slot].State;
}

ECrowdyModelLoadState FCrowdyStudioController::GetAttributeLoadState(const FString& TypeName) const
{
	// The order these three are weighed in is the rule, and it is stated once, in the pure layer. Restating it here
	// would give a successful re-read a second chance to lose to the failure recorded before it.
	const bool bFailed = PropertyDefFailedTypes.ContainsByPredicate(
		[&TypeName](const FString& Failed) { return Failed.Equals(TypeName, ESearchCase::CaseSensitive); });

	return CrowdyModelEmptyState::AttributeLoadState(
		PropertyDefsByType.Contains(TypeName), bFailed, PropertyDefFetchesInFlight.Contains(TypeName));
}

uint64 FCrowdyStudioController::BeginFamilyRead(ECrowdyModelFamily Family)
{
	const int32 Slot = static_cast<int32>(Family);
	if (Slot < 0 || Slot >= static_cast<int32>(ECrowdyModelFamily::Count))
	{
		return 0;
	}

	FamilyLoads[Slot].State = ECrowdyModelLoadState::Loading;
	FamilyLoads[Slot].AppId = SelectedAppId;
	FamilyLoads[Slot].Serial = ++NextFamilyReadSerial;
	return FamilyLoads[Slot].Serial;
}

bool FCrowdyStudioController::AcceptFamilyReply(ECrowdyModelFamily Family, int64 ReplyAppId, uint64 ReplySerial,
	ECrowdyModelLoadState Landed)
{
	const int32 Slot = static_cast<int32>(Family);
	if (Slot < 0 || Slot >= static_cast<int32>(ECrowdyModelFamily::Count))
	{
		return false;
	}
	if (!FamilyLoads[Slot].Accepts(ReplyAppId, ReplySerial, SelectedAppId))
	{
		return false;
	}

	FamilyLoads[Slot].State = Landed;
	return true;
}

void FCrowdyStudioController::MarkFamilyLoadedFromPlan(ECrowdyModelFamily Family, int64 AppId)
{
	const int32 Slot = static_cast<int32>(Family);
	if (Slot < 0 || Slot >= static_cast<int32>(ECrowdyModelFamily::Count))
	{
		return;
	}

	FamilyLoads[Slot].State = ECrowdyModelLoadState::Loaded;
	FamilyLoads[Slot].AppId = AppId;
	FamilyLoads[Slot].Serial = ++NextFamilyReadSerial;
}

void FCrowdyStudioController::FetchContainerTypes()
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app to list container types."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), SelectedAppId);

	const int64 RequestAppId = SelectedAppId;
	const uint64 RequestSerial = BeginFamilyRead(ECrowdyModelFamily::Models);

	SendGame(ECrowdyCppApiDomain::GameModel, TEXT("GameModelContainerTypes"), Variables,
		[this, RequestAppId, RequestSerial](const TSharedPtr<FJsonObject>& Envelope)
		{
			TArray<TSharedPtr<FStudioContainerType>> Types;
			CrowdyStudioGql::ParseContainerTypes(Envelope, TEXT("gameModelContainerTypes"), Types);
			IngestContainerTypes(MoveTemp(Types), RequestAppId, RequestSerial);
		},
		[this, RequestAppId, RequestSerial]()
		{
			// A failed read leaves the list exactly as it was and records that it failed, so an empty list is never
			// mistaken for an app with no models. Announced even though nothing changed: a view waiting on this
			// delegate would otherwise sit on a loading state that no refresh ever clears.
			if (AcceptFamilyReply(ECrowdyModelFamily::Models, RequestAppId, RequestSerial,
				ECrowdyModelLoadState::Failed))
			{
				OnContainerTypesChanged.Broadcast();
			}
		});
}

void FCrowdyStudioController::IngestContainerTypes(TArray<TSharedPtr<FStudioContainerType>>&& Types,
	int64 RequestAppId, uint64 RequestSerial)
{
	// The read landed, so an empty list now means this app has none. Recorded here rather than where the read was
	// issued, because a read that never came back must not vouch for the empty list it left behind.
	if (!AcceptFamilyReply(ECrowdyModelFamily::Models, RequestAppId, RequestSerial, ECrowdyModelLoadState::Loaded))
	{
		return;
	}

	ContainerTypes = MoveTemp(Types);
	SetStatus(FString::Printf(TEXT("%d container type(s)."), ContainerTypes.Num()), false);
	OnContainerTypesChanged.Broadcast();
}

void FCrowdyStudioController::UpsertContainerType(const FString& TypeName, const FString& DisplayName, const FString& Description,
	const FString& InstantiableBy, const FString& DefaultPropertyVisibility, int64 ExpectedAppId)
{
	if (SelectedAppId == 0 || TypeName.IsEmpty() || DisplayName.IsEmpty())
	{
		SetStatus(TEXT("A container type needs an app, type name, and display name."), true);
		return;
	}
	// The editor's contents were filled from one app; never write them to a different now-selected app.
	if (SelectedAppId != ExpectedAppId)
	{
		SetStatus(TEXT("This container type was loaded for a different app. Reload the current app's container types before saving."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("appId"), SelectedAppId);
	Input->SetStringField(TEXT("typeName"), TypeName);
	Input->SetStringField(TEXT("displayName"), DisplayName);
	SetOptionalStringField(Input, TEXT("description"), Description);
	SetOptionalStringField(Input, TEXT("instantiableBy"), InstantiableBy);
	SetOptionalStringField(Input, TEXT("defaultPropertyVisibility"), DefaultPropertyVisibility);

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	SendGame(ECrowdyCppApiDomain::GameModel, TEXT("GameModelUpsertContainerType"), Variables,
		[this](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(TEXT("Saved container type."), false);
			FetchContainerTypes();
		});
}

const TArray<TSharedPtr<FStudioPropertyDef>>* FCrowdyStudioController::GetPropertyDefsForType(const FString& TypeName) const
{
	return PropertyDefsByType.Find(TypeName);
}

void FCrowdyStudioController::IngestPropertyDefs(const FString& TypeName, TArray<TSharedPtr<FStudioPropertyDef>>&& Defs)
{
	if (TypeName.IsEmpty())
	{
		return;
	}

	TArray<TSharedPtr<FStudioPropertyDef>>& Cached = PropertyDefsByType.Add(TypeName, MoveTemp(Defs));

	// Every reply reaches the cache, so every reply is announced here. A view that reads the cache by type is waiting
	// on the type it asked for and cannot tell "still loading" from "loaded" any other way.
	OnPropertyDefsCached.Broadcast();

	// The per-type cache takes every reply; the flat mirror takes only the type it is supposed to be showing. With
	// reads coalesced, an older reply can be the last to land, and repainting the mirror from it would leave the page
	// naming one type above another type's attributes - and every editor that builds its property list from the
	// mirror would follow it there.
	if (!PropertyDefsMirrorType.IsEmpty() && TypeName != PropertyDefsMirrorType)
	{
		return;
	}

	PropertyDefs = Cached;
	OnPropertyDefsChanged.Broadcast();
}

bool FCrowdyStudioController::IsLatestPropertyDefRequest(const FString& TypeName, int64 RequestAppId, uint64 RequestSerial) const
{
	const uint64* Latest = PropertyDefLatestRequest.Find(TypeName);
	return SelectedAppId == RequestAppId && Latest != nullptr && *Latest == RequestSerial;
}

void FCrowdyStudioController::FetchPropertyDefs(const FString& TypeName, bool bForceRefresh)
{
	if (SelectedAppId == 0 || TypeName.IsEmpty())
	{
		PropertyDefsMirrorType.Reset();
		PropertyDefs.Reset();
		OnPropertyDefsChanged.Broadcast();
		return;
	}

	// The mirror follows the type most recently asked for by a selection, whether or not this call issues a query. A
	// forced read is a background refresh after a write and must not claim the mirror: the user may have moved to
	// another type since, and taking the mirror here would discard that type's reply and paint the written one instead.
	if (!bForceRefresh)
	{
		PropertyDefsMirrorType = TypeName;
	}

	// A read already running for this type will fill the cache when it lands, so re-selecting the type must not fan
	// out a second identical query. Show whatever is already cached in the meantime. A forced read skips this: it
	// follows a write, and the read in flight was issued before that write, so it answers with the old list.
	if (!bForceRefresh && PropertyDefFetchesInFlight.Contains(TypeName))
	{
		if (const TArray<TSharedPtr<FStudioPropertyDef>>* Cached = PropertyDefsByType.Find(TypeName))
		{
			PropertyDefs = *Cached;
			OnPropertyDefsChanged.Broadcast();
		}
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), SelectedAppId);
	Variables->SetStringField(TEXT("typeName"), TypeName);

	PropertyDefFetchesInFlight.Add(TypeName);
	// A read is going out for this model, so whatever the last one said is no longer the newest answer. Removed here
	// rather than when the reply lands, so the table shows this read running instead of the previous read's error.
	PropertyDefFailedTypes.RemoveAll(
		[&TypeName](const FString& Failed) { return Failed.Equals(TypeName, ESearchCase::CaseSensitive); });
	const int64 RequestAppId = SelectedAppId;
	const uint64 RequestSerial = ++NextPropertyDefRequestSerial;
	PropertyDefLatestRequest.Add(TypeName, RequestSerial);

	// A reply is only acted on when it is still the latest read of its type: the newest read has seen everything the
	// older ones would report, so an older answer landing afterwards can only put a stale list back. That is also why
	// a superseded reply leaves the in-flight key alone - the key belongs to the read that superseded it, which is
	// still outstanding.
	auto IsLatestRequest = [this, TypeName, RequestAppId, RequestSerial]()
	{
		return IsLatestPropertyDefRequest(TypeName, RequestAppId, RequestSerial);
	};

	SendGame(ECrowdyCppApiDomain::GameModel, TEXT("GameModelPropertyDefs"), Variables,
		[this, TypeName, IsLatestRequest](const TSharedPtr<FJsonObject>& Envelope)
		{
			if (!IsLatestRequest())
			{
				return;
			}
			PropertyDefFetchesInFlight.Remove(TypeName);
			TArray<TSharedPtr<FStudioPropertyDef>> Defs;
			CrowdyStudioGql::ParsePropertyDefs(Envelope, TEXT("gameModelPropertyDefs"), Defs);
			IngestPropertyDefs(TypeName, MoveTemp(Defs));
		},
		[this, TypeName, IsLatestRequest]()
		{
			// Release the coalescing key on failure too, otherwise this type could never be loaded again.
			if (IsLatestRequest())
			{
				PropertyDefFetchesInFlight.Remove(TypeName);
				// Nothing reached the cache, so without this the table would go on saying the attributes are being
				// read, forever, for a read that already came back an error. Matched case-sensitively: AddUnique
				// compares with FString::operator==, which folds case, so two models whose names differ only in case
				// would share one record and one of them would never report its failure.
				if (!PropertyDefFailedTypes.ContainsByPredicate(
					[&TypeName](const FString& Failed) { return Failed.Equals(TypeName, ESearchCase::CaseSensitive); }))
				{
					PropertyDefFailedTypes.Add(TypeName);
				}
				// The same announcement a successful reply makes. A view waiting on it is waiting to stop showing a
				// loading state, and a failure it never hears about is the one state it can never leave.
				OnPropertyDefsCached.Broadcast();
			}
		});
}

void FCrowdyStudioController::UpsertPropertyDef(const FString& TypeName, const FString& Key, const FString& ValueType,
	const FString& DefaultValueJson, const FString& Visibility, const FString& Writable, const FString& Description,
	int64 ExpectedAppId)
{
	if (SelectedAppId == 0 || TypeName.IsEmpty() || Key.IsEmpty() || ValueType.IsEmpty())
	{
		SetStatus(TEXT("A property needs an app, type, key, and value type."), true);
		return;
	}
	// The editor's contents were filled from one app; never write them to a different now-selected app.
	if (SelectedAppId != ExpectedAppId)
	{
		SetStatus(TEXT("This property was loaded for a different app. Reload the current app's schema before saving."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("appId"), SelectedAppId);
	Input->SetStringField(TEXT("containerTypeName"), TypeName);
	Input->SetStringField(TEXT("key"), Key);
	Input->SetStringField(TEXT("valueType"), ValueType);
	SetOptionalStringField(Input, TEXT("defaultValueJson"), DefaultValueJson);
	SetOptionalStringField(Input, TEXT("visibility"), Visibility);
	SetOptionalStringField(Input, TEXT("writable"), Writable);
	SetOptionalStringField(Input, TEXT("description"), Description);

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	SendGame(ECrowdyCppApiDomain::GameModel, TEXT("GameModelUpsertPropertyDef"), Variables,
		[this, TypeName](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(TEXT("Saved property."), false);
			// Force the re-read: a read of this type already in flight was issued before this write, so coalescing
			// onto it would show the list as it was before the property was saved, and nothing further would be
			// fetched to correct it.
			FetchPropertyDefs(TypeName, /*bForceRefresh*/ true);
		});
}

void FCrowdyStudioController::FetchFunctions(const FString& ContainerTypeFilter)
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app to list functions."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), SelectedAppId);
	if (ContainerTypeFilter.IsEmpty())
	{
		Variables->SetField(TEXT("containerTypeName"), MakeShared<FJsonValueNull>());
	}
	else
	{
		Variables->SetStringField(TEXT("containerTypeName"), ContainerTypeFilter);
	}

	// Only a whole-app read answers "does this app have any functions at all", so only a whole-app read claims the
	// family. A read narrowed to one container type says nothing about the rest and takes no family serial.
	//
	// It takes a read serial of its own regardless. The mirror below shows whatever was last asked for, and a
	// narrowed read landing after a wider one would otherwise replace the whole app's list with a subset of it, print
	// that subset's count and announce it, which is exactly the out-of-order race the family serial exists to close.
	const bool bUnfiltered = ContainerTypeFilter.IsEmpty();
	const int64 RequestAppId = SelectedAppId;
	const uint64 RequestSerial = bUnfiltered ? BeginFamilyRead(ECrowdyModelFamily::Functions) : 0;
	const uint64 ReadSerial = BeginFunctionRead();

	SendGame(ECrowdyCppApiDomain::GameModel, TEXT("GameModelFunctions"), Variables,
		[this, bUnfiltered, RequestAppId, RequestSerial, ReadSerial](const TSharedPtr<FJsonObject>& Envelope)
		{
			// Parsed into a list of its own, so a reply this controller has already decided against cannot leave a
			// list holding it. Parsed straight into the mirror, a superseded read is applied before anything gets the
			// chance to reject it, and nothing is issued afterwards to put the right list back.
			TArray<TSharedPtr<FStudioFunction>> Parsed;
			CrowdyStudioGql::ParseFunctions(Envelope, TEXT("gameModelFunctions"), Parsed);
			IngestFunctions(MoveTemp(Parsed), bUnfiltered, RequestAppId, RequestSerial, ReadSerial);
		},
		[this, bUnfiltered, RequestAppId, RequestSerial]()
		{
			// The app-wide list stands as it was and is recorded as failed, so an empty one is never read as an app
			// with no functions. Announced for the same reason the models read announces its failure.
			if (bUnfiltered
				&& AcceptFamilyReply(ECrowdyModelFamily::Functions, RequestAppId, RequestSerial,
					ECrowdyModelLoadState::Failed))
			{
				OnFunctionsChanged.Broadcast();
			}
		});
}

void FCrowdyStudioController::IngestFunctions(TArray<TSharedPtr<FStudioFunction>>&& Parsed, bool bUnfiltered,
	int64 RequestAppId, uint64 RequestSerial, uint64 ReadSerial)
{
	bool bTookTheReply = false;

	// Only a read of the whole app may replace the app-wide list. A read narrowed to one container type holds the
	// answer for that type alone, and letting it stand for the app would report every other type as having no
	// functions at all.
	if (bUnfiltered
		&& AcceptFamilyReply(ECrowdyModelFamily::Functions, RequestAppId, RequestSerial, ECrowdyModelLoadState::Loaded))
	{
		UnfilteredFunctions = Parsed;
		bTookTheReply = true;
	}

	// Guarded separately from the list above: a whole-app read can still be the newest thing that list has heard
	// while a narrower read issued after it owns the mirror, and the reverse.
	if (IsLatestFunctionRead(ReadSerial))
	{
		Functions = MoveTemp(Parsed);
		SetStatus(FString::Printf(TEXT("%d function(s)."), Functions.Num()), false);
		bTookTheReply = true;
	}

	// Announced only where something was actually taken, and wherever it was taken: a view reading the app-wide list
	// has to hear about a reply the mirror declined, or it waits on a list that is already in hand.
	if (bTookTheReply)
	{
		OnFunctionsChanged.Broadcast();
	}
}

void FCrowdyStudioController::UpsertFunction(const FString& Name, const FString& ContainerTypeName, const FString& Description,
	const FString& ReturnType, const FString& InvokeScope, const FString& ReturnExpression,
	const FString& ParametersJson, const FString& MutationsJson, const FString& InvokePolicyJson,
	int64 ExpectedAppId)
{
	if (SelectedAppId == 0 || Name.IsEmpty())
	{
		SetStatus(TEXT("A function needs an app and a name."), true);
		return;
	}
	// The editor's contents were filled from one app; never write them to a different now-selected app.
	if (SelectedAppId != ExpectedAppId)
	{
		SetStatus(TEXT("This function was loaded for a different app. Reload the current app's functions before saving."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("appId"), SelectedAppId);
	Input->SetStringField(TEXT("name"), Name);
	SetOptionalStringField(Input, TEXT("containerTypeName"), ContainerTypeName);
	SetOptionalStringField(Input, TEXT("description"), Description);
	SetOptionalStringField(Input, TEXT("invokeScope"), InvokeScope);

	// Same reasoning as the policy below: the editor saves the whole function, so clearing the return boxes means
	// "this function answers with nothing". Omitting them would leave the server's previous values in place and the
	// panel would read them straight back, making a return impossible to remove once added.
	for (const TPair<const TCHAR*, const FString*> Field :
		{ TPair<const TCHAR*, const FString*>{ TEXT("returnType"), &ReturnType },
		  TPair<const TCHAR*, const FString*>{ TEXT("returnExpression"), &ReturnExpression } })
	{
		if (Field.Value->IsEmpty())
		{
			Input->SetField(Field.Key, MakeShared<FJsonValueNull>());
		}
		else
		{
			Input->SetStringField(Field.Key, *Field.Value);
		}
	}

	// The editor saves the whole function each time, so an empty policy means "no requirements". Send
	// an explicit JSON null (not an omitted field) so the server clears any existing policy and the
	// runtime falls back to "anyone entitled may invoke", instead of keeping the previous policy.
	if (InvokePolicyJson.IsEmpty())
	{
		Input->SetField(TEXT("invokePolicyJson"), MakeShared<FJsonValueNull>());
	}
	else
	{
		Input->SetStringField(TEXT("invokePolicyJson"), InvokePolicyJson);
	}

	if (!ParametersJson.IsEmpty())
	{
		TArray<TSharedPtr<FJsonValue>> Params;
		if (!ParseJsonArray(ParametersJson, Params))
		{
			SetStatus(TEXT("Parameters must be a JSON array."), true);
			return;
		}
		Input->SetArrayField(TEXT("parameters"), Params);
	}

	if (!MutationsJson.IsEmpty())
	{
		TArray<TSharedPtr<FJsonValue>> Muts;
		if (!ParseJsonArray(MutationsJson, Muts))
		{
			SetStatus(TEXT("Mutations must be a JSON array."), true);
			return;
		}
		Input->SetArrayField(TEXT("mutations"), Muts);
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	SendGame(ECrowdyCppApiDomain::GameModel, TEXT("GameModelUpsertFunction"), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			FStudioFunction Saved;
			if (CrowdyStudioGql::ParseFunction(Envelope, TEXT("gameModelUpsertFunction"), Saved) && Saved.Warnings.Num() > 0)
			{
				// The warning detail shows in the function editor's warnings panel; keep the status terse.
				SetStatus(FString::Printf(TEXT("Saved function (%d static-analysis warning(s))."), Saved.Warnings.Num()), false);
			}
			else
			{
				SetStatus(TEXT("Saved function."), false);
			}
			FetchFunctions(FString());
		});
}

void FCrowdyStudioController::DeleteFunction(const FString& OwningTypeName, const FString& Name, int64 ExpectedAppId)
{
	if (SelectedAppId == 0 || Name.IsEmpty())
	{
		SetStatus(TEXT("Select an app and a function to delete."), true);
		return;
	}
	// The function list the caller acted on belongs to one app; never delete from a different now-selected app.
	if (SelectedAppId != ExpectedAppId)
	{
		SetStatus(TEXT("This function was listed for a different app. Reload the current app's functions before deleting."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), SelectedAppId);
	// The mutation takes the bare name. OwningTypeName is NOT sent: whether the server resolves that name within one
	// container type is not settled, so passing it would be inventing an argument the operation does not declare. It
	// is used below only to say which model the caller was looking at.
	Variables->SetStringField(TEXT("name"), Name);

	// Worked out BEFORE the write, from the list as it stood when the caller acted, because the refresh below
	// replaces that list with one the deleted function is already missing from.
	const bool bNameOnSeveralModels =
		CrowdyGameModelDelete::ScopesCarryingFunctionName(UnfilteredFunctions, Name).Num() >= 2;

	SendGame(ECrowdyCppApiDomain::GameModel, TEXT("GameModelDeleteFunction"), Variables,
		[this, OwningTypeName, Name, bNameOnSeveralModels](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			// The mutation takes the bare name. Naming a model here is only true where this app carries that name on
			// one model; where it carries it on several, saying "on Knight" would be a promise the operation may not
			// keep, so the sentence says what is certain instead.
			if (bNameOnSeveralModels)
			{
				SetStatus(FString::Printf(
					TEXT("Deleted the function %s. This app had that name on more than one model and the delete ")
					TEXT("takes the name alone, so more than one may have gone."), *Name), false);
			}
			else
			{
				SetStatus(OwningTypeName.IsEmpty()
					? FString::Printf(TEXT("Deleted the function %s."), *Name)
					: FString::Printf(TEXT("Deleted the function %s on %s."), *Name, *OwningTypeName), false);
			}
			FetchFunctions(FString());
		});
}

void FCrowdyStudioController::DeleteContainerType(const FString& TypeName, int64 ExpectedAppId)
{
	if (SelectedAppId == 0 || TypeName.IsEmpty())
	{
		SetStatus(TEXT("Select an app and a model to delete."), true);
		return;
	}
	// The model list the caller acted on belongs to one app; never delete from a different now-selected app.
	if (SelectedAppId != ExpectedAppId)
	{
		SetStatus(TEXT("This model was listed for a different app. Reload the current app's models before deleting."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), SelectedAppId);
	Variables->SetStringField(TEXT("typeName"), TypeName);

	SendGame(ECrowdyCppApiDomain::GameModel, TEXT("GameModelDeleteContainerType"), Variables,
		[this, TypeName](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(FString::Printf(TEXT("Deleted the model %s and its attributes."), *TypeName), false);
			// The attributes went with the model, so the cached list for it is a list of things that no longer
			// exist. Drop the entry rather than leave it: a view reading the cache would still paint them. Its
			// in-flight key and serial go too, so a read issued BEFORE this delete cannot land afterwards and put
			// the whole list back.
			PropertyDefsByType.Remove(TypeName);
			PropertyDefFetchesInFlight.Remove(TypeName);
			PropertyDefLatestRequest.Remove(TypeName);
			if (PropertyDefsMirrorType.Equals(TypeName, ESearchCase::CaseSensitive))
			{
				PropertyDefsMirrorType.Reset();
				PropertyDefs.Reset();
				OnPropertyDefsChanged.Broadcast();
			}
			OnPropertyDefsCached.Broadcast();
			FetchContainerTypes();
		});
}

void FCrowdyStudioController::DeletePropertyDef(const FString& TypeName, const FString& Key, int64 ExpectedAppId)
{
	if (SelectedAppId == 0 || TypeName.IsEmpty() || Key.IsEmpty())
	{
		SetStatus(TEXT("Select an app, a model and an attribute to delete."), true);
		return;
	}
	// The attribute list the caller acted on belongs to one app; never delete from a different now-selected app.
	if (SelectedAppId != ExpectedAppId)
	{
		SetStatus(TEXT("This attribute was listed for a different app. Reload the current app's schema before deleting."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), SelectedAppId);
	Variables->SetStringField(TEXT("containerTypeName"), TypeName);
	Variables->SetStringField(TEXT("key"), Key);

	SendGame(ECrowdyCppApiDomain::GameModel, TEXT("GameModelDeletePropertyDef"), Variables,
		[this, TypeName, Key](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(FString::Printf(TEXT("Deleted the attribute %s from %s."), *Key, *TypeName), false);
			// Force the re-read: a read of this model already in flight was issued before this delete, so coalescing
			// onto it would show the attribute still there and nothing further would be fetched to correct it.
			FetchPropertyDefs(TypeName, /*bForceRefresh*/ true);
		});
}

void FCrowdyStudioController::DeleteAutomation(const FString& Name, int64 ExpectedAppId)
{
	if (SelectedAppId == 0 || Name.IsEmpty())
	{
		SetStatus(TEXT("Select an app and an automation to delete."), true);
		return;
	}
	// The automation list the caller acted on belongs to one app; never delete from a different now-selected app.
	if (SelectedAppId != ExpectedAppId)
	{
		SetStatus(TEXT("This automation was listed for a different app. Reload the current app's automations before deleting."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), SelectedAppId);
	Variables->SetStringField(TEXT("name"), Name);

	SendGame(ECrowdyCppApiDomain::GameModel, TEXT("GameModelDeleteAutomation"), Variables,
		[this, Name](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(FString::Printf(TEXT("Deleted the automation %s and its event triggers."), *Name), false);
			// The triggers went with it, and the browsed list holds both, so it is re-read as one.
			FetchAutomations();
		});
}

void FCrowdyStudioController::DeleteContainer(const FString& ContainerId, int64 ExpectedAppId)
{
	if (SelectedAppId == 0 || ContainerId.IsEmpty())
	{
		SetStatus(TEXT("Select an app and a container to delete."), true);
		return;
	}
	// The container list the caller acted on belongs to one app; never delete from a different now-selected app.
	if (SelectedAppId != ExpectedAppId)
	{
		SetStatus(TEXT("This container was listed for a different app. Reload the current app's containers before deleting."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), SelectedAppId);
	Variables->SetStringField(TEXT("containerId"), ContainerId);

	SendGame(ECrowdyCppApiDomain::GameModel, TEXT("GameModelDeleteContainer"), Variables,
		[this, ContainerId](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			// A false server reply means the instance did not exist already, an idempotent no-op, not a failure.
			SetStatus(TEXT("Deleted container."), false);
			if (SelectedContainerId == ContainerId)
			{
				SelectedContainerId.Reset();
				ContainerState = FStudioContainerState();
				OnContainerStateChanged.Broadcast();
			}
			// Re-read exactly what the user has on screen. When the list was paged, that is every page loaded so
			// far, read back as one window from the start: re-reading only the last page would drop the earlier
			// ones from the list, and dropping the page arguments would widen it to every container in the app.
			// This window comes back one container short of what it asks for, because one was just deleted. That is
			// not the end of the list, so it must not be allowed to answer whether another page exists.
			const int32 RelistLimit = LastContainerLimit > 0 ? LastContainerOffset + LastContainerLimit : 0;
			ReadContainers(LastContainerTypeFilter, LastContainerSessionFilter, RelistLimit, 0, /*bAppend*/ false,
				EContainerPageEvidence::Keep);
		});
}

void FCrowdyStudioController::FetchAutomations()
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app to list automations."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), SelectedAppId);

	// Two families, issued at the same moment: the automations and, chained behind them, their event triggers. They
	// are numbered separately because a trigger read that failed while the automations arrived is its own state, and
	// folding the two makes it indistinguishable from an app whose automations simply have no triggers.
	const int64 RequestAppId = SelectedAppId;
	const uint64 AutomationSerial = BeginFamilyRead(ECrowdyModelFamily::Automations);
	const uint64 TriggerSerial = BeginFamilyRead(ECrowdyModelFamily::AutomationTriggers);

	SendGame(ECrowdyCppApiDomain::GameModel, TEXT("GameModelAutomations"), Variables,
		[this, RequestAppId, AutomationSerial, TriggerSerial](const TSharedPtr<FJsonObject>& Envelope)
		{
			TArray<FStudioAutomation> ParsedAutomations;
			CrowdyStudioGql::ParseAutomations(Envelope, TEXT("gameModelAutomations"), ParsedAutomations);

			// Nothing on show is replaced yet. The automations and their triggers are put in place together, once the
			// chained read below settles: a window in which the new automations stand beside no triggers at all is a
			// window any other read landing in it can repaint from, and every event automation would render with its
			// trigger phrase missing and then correct itself, which reads as information lost and put back.
			const TSharedPtr<FJsonObject> TriggerVariables = MakeShared<FJsonObject>();
			SetBigIntField(TriggerVariables, TEXT("appId"), SelectedAppId);
			SendGame(ECrowdyCppApiDomain::GameModel, TEXT("GameModelAutomationTriggers"), TriggerVariables,
				[this, RequestAppId, AutomationSerial, TriggerSerial, AutomationsForNames = ParsedAutomations]
				(const TSharedPtr<FJsonObject>& TriggerEnvelope)
				{
					if (!AcceptFamilyReply(ECrowdyModelFamily::Automations, RequestAppId, AutomationSerial,
						ECrowdyModelLoadState::Loaded))
					{
						return;
					}
					AcceptFamilyReply(ECrowdyModelFamily::AutomationTriggers, RequestAppId, TriggerSerial,
						ECrowdyModelLoadState::Loaded);

					TArray<FStudioAutomationTrigger> ParsedTriggers;
					CrowdyStudioGql::ParseAutomationTriggers(TriggerEnvelope, TEXT("gameModelAutomationTriggers"),
						AutomationsForNames, ParsedTriggers);

					Automations.Reset(AutomationsForNames.Num());
					for (const FStudioAutomation& Automation : AutomationsForNames)
					{
						Automations.Add(MakeShared<FStudioAutomation>(Automation));
					}

					AutomationTriggers.Reset(ParsedTriggers.Num());
					for (const FStudioAutomationTrigger& Trigger : ParsedTriggers)
					{
						AutomationTriggers.Add(MakeShared<FStudioAutomationTrigger>(Trigger));
					}

					SetStatus(FString::Printf(TEXT("%d automation(s)."), Automations.Num()), false);
					// The single announcement for both reads.
					OnAutomationsChanged.Broadcast();
				},
				// Both handlers carry their own copy of the parsed automations. The order in which the two capture
				// lists are evaluated is not fixed by the language, so moving into either one could hand the other a
				// list that has already been moved out of.
				[this, RequestAppId, AutomationSerial, TriggerSerial, AutomationsForNames = ParsedAutomations]()
				{
					// The automations themselves came back, so that list is this app's and an empty one means it has
					// none. The triggers did not, and this is the one place that difference is recorded: folding it
					// into the automations' own state is what used to make a failed trigger read look like an app
					// whose automations have no triggers.
					if (!AcceptFamilyReply(ECrowdyModelFamily::Automations, RequestAppId, AutomationSerial,
						ECrowdyModelLoadState::Loaded))
					{
						return;
					}
					AcceptFamilyReply(ECrowdyModelFamily::AutomationTriggers, RequestAppId, TriggerSerial,
						ECrowdyModelLoadState::Failed);

					// Take the automations and leave the triggers empty, then announce anyway: a view waiting on this
					// delegate would otherwise sit on a loading state that no refresh ever clears.
					Automations.Reset(AutomationsForNames.Num());
					for (const FStudioAutomation& Automation : AutomationsForNames)
					{
						Automations.Add(MakeShared<FStudioAutomation>(Automation));
					}
					AutomationTriggers.Reset();
					OnAutomationsChanged.Broadcast();
				});
		},
		[this, RequestAppId, AutomationSerial, TriggerSerial]()
		{
			// The automations read failed, so the trigger read behind it was never issued. Both are recorded as
			// failed: a family left saying it is still loading for a read that will never arrive is a page that says
			// "wait" forever.
			const bool bAccepted = AcceptFamilyReply(ECrowdyModelFamily::Automations, RequestAppId, AutomationSerial,
				ECrowdyModelLoadState::Failed);
			AcceptFamilyReply(ECrowdyModelFamily::AutomationTriggers, RequestAppId, TriggerSerial,
				ECrowdyModelLoadState::Failed);

			// Nothing was read, so the lists stand as they were, but the views still have to be told the attempt
			// is over.
			if (bAccepted)
			{
				OnAutomationsChanged.Broadcast();
			}
		});
}

bool FCrowdyStudioController::ShouldLoadGameModelLists(int64 SelectedAppId, int64 LoadedAppId)
{
	return SelectedAppId != 0 && LoadedAppId != SelectedAppId;
}

void FCrowdyStudioController::EnsureGameModelListsLoaded()
{
	if (!ShouldLoadGameModelLists(SelectedAppId, GameModelListsAppId))
	{
		return;
	}

	// Marked before the reads are issued, not after they land. The caller runs from the paint path, so leaving it
	// unmarked until the replies arrive would issue three more reads on every frame in between.
	GameModelListsAppId = SelectedAppId;

	FetchContainerTypes();
	// No container-type filter: the browser shows every model at once, so it needs the app-wide function list.
	FetchFunctions(FString());
	FetchAutomations();
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

	SendGame(ECrowdyCppApiDomain::GameModel, TEXT("GameModelFeatures"), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseFeatures(Envelope, TEXT("gameModelFeatures"), Features);
			SetStatus(FString::Printf(TEXT("%d feature(s)."), Features.Num()), false);
			OnFeaturesChanged.Broadcast();
		});
}

void FCrowdyStudioController::DefineFeature(const FString& FeatureKey, const FString& Description)
{
	if (SelectedAppId == 0 || FeatureKey.IsEmpty())
	{
		SetStatus(TEXT("A feature needs an app and a feature key."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("appId"), SelectedAppId);
	Input->SetStringField(TEXT("featureKey"), FeatureKey);
	SetOptionalStringField(Input, TEXT("description"), Description);

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	SendGame(ECrowdyCppApiDomain::GameModel, TEXT("GameModelDefineFeature"), Variables,
		[this](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(TEXT("Saved feature."), false);
			FetchFeatures();
		});
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

	SendGame(ECrowdyCppApiDomain::GameModel, TEXT("GameModelTierFeatures"), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseTierFeatures(Envelope, TEXT("gameModelTierFeatures"), TierFeatures);
			SetStatus(FString::Printf(TEXT("%d tier-feature grant(s)."), TierFeatures.Num()), false);
			OnTierFeaturesChanged.Broadcast();
		});
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

	// The only app-scoped read that goes out on the management plane, so it carries its own pinned-app check rather
	// than inheriting the one every game-plane op gets from SendGame. Without it a reply issued for the previous app
	// refills the tier list after an app switch has already emptied it.
	const int64 RequestAppId = SelectedAppId;
	SendManagement(ECrowdyCppApiDomain::AppAccess, TEXT("AppAccessTiers"), Variables,
		[this, RequestAppId](const TSharedPtr<FJsonObject>& Envelope)
		{
			if (SelectedAppId != RequestAppId)
			{
				return;
			}
			CrowdyStudioGql::ParseAccessTiers(Envelope, TEXT("appAccessTiers"), AccessTiers);
			OnAccessTiersChanged.Broadcast();
		});
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

	SendGame(ECrowdyCppApiDomain::GameModel, TEXT("GameModelGrantTierFeature"), Variables,
		[this](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(TEXT("Granted feature to tier."), false);
			FetchTierFeatures();
		});
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

	SendGame(ECrowdyCppApiDomain::GameModel, TEXT("GameModelRevokeTierFeature"), Variables,
		[this](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			SetStatus(TEXT("Revoked feature from tier."), false);
			FetchTierFeatures();
		});
}

void FCrowdyStudioController::RunGameModelLint(bool bQuiet)
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app to lint its game model."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), SelectedAppId);

	SendGame(ECrowdyCppApiDomain::GameModel, TEXT("CrowdyModelLint"), Variables,
		[this, bQuiet](const TSharedPtr<FJsonObject>& Envelope)
		{
			if (!CrowdyStudioGql::ParseModelLint(Envelope, TEXT("gameModelLint"), GameModelLint))
			{
				return;
			}

			// Logged one line per finding rather than summarised, because the summary counts say how much is wrong
			// and nothing about what: the remedy is the only part a developer can act on without a second call.
			for (const FStudioLintFinding& Finding : GameModelLint.Findings)
			{
				const FString Where = Finding.SubjectKind.IsEmpty()
					? Finding.Subject : Finding.SubjectKind + TEXT("/") + Finding.Subject;
				const FString Remedy = Finding.Remedy.IsEmpty() ? FString() : TEXT("  ") + Finding.Remedy;
				if (Finding.IsError())
				{
					UE_LOG(LogCrowdyStudio, Warning, TEXT("[GameModelLint] ERROR %s on %s: %s%s"),
						*Finding.Code, *Where, *Finding.Message, *Remedy);
				}
				else
				{
					UE_LOG(LogCrowdyStudio, Log, TEXT("[GameModelLint] %s %s on %s: %s%s"),
						*Finding.Severity, *Finding.Code, *Where, *Finding.Message, *Remedy);
				}
			}

			OnGameModelLintChanged.Broadcast();

			// An error is worth interrupting for whatever asked, because an enforced one quarantines the object and
			// it will refuse to run. A clean answer only speaks when the developer asked for it.
			if (!GameModelLint.bClean)
			{
				SetStatus(FString::Printf(
					TEXT("Game model lint: %d error(s), %d warning(s). See the log for each finding."),
					GameModelLint.ErrorCount, GameModelLint.WarningCount), true);
				return;
			}
			if (!bQuiet)
			{
				SetStatus(GameModelLint.WarningCount > 0
					? FString::Printf(TEXT("Game model lint: clean, with %d warning(s)."), GameModelLint.WarningCount)
					: TEXT("Game model lint: clean."), false);
			}
		});
}

void FCrowdyStudioController::FetchGameModelPolicy()
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app to read its game-model policy."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), SelectedAppId);

	SendGame(ECrowdyCppApiDomain::GameModel, TEXT("GameModelPolicy"), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseGameModelPolicy(Envelope, TEXT("gameModelPolicy"), GameModelPolicy);
			OnGameModelPolicyChanged.Broadcast();
		});
}

void FCrowdyStudioController::SetGameModelPolicy(const FString& SessionCreationPolicy, const FString& DefaultParticipantRole)
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app before setting game-model policy."), true);
		return;
	}

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	SetBigIntField(Input, TEXT("appId"), SelectedAppId);
	SetOptionalStringField(Input, TEXT("sessionCreationPolicy"), SessionCreationPolicy);
	SetOptionalStringField(Input, TEXT("defaultParticipantRole"), DefaultParticipantRole);

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	SendGame(ECrowdyCppApiDomain::GameModel, TEXT("GameModelSetPolicy"), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseGameModelPolicy(Envelope, TEXT("gameModelSetPolicy"), GameModelPolicy);
			SetStatus(TEXT("Game-model policy updated."), false);
			OnGameModelPolicyChanged.Broadcast();
		});
}

void FCrowdyStudioController::SeedGameModel(const FString& SeedJson)
{
	if (SelectedAppId == 0 || SeedJson.IsEmpty())
	{
		SetStatus(TEXT("Seeding needs a selected app and a JSON body."), true);
		return;
	}

	TSharedPtr<FJsonObject> Input;
	if (!ParseJsonObject(SeedJson, Input))
	{
		SetStatus(TEXT("Seed body must be a JSON object."), true);
		return;
	}

	SetBigIntField(Input, TEXT("appId"), SelectedAppId);

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	SendGame(ECrowdyCppApiDomain::GameModel, TEXT("GameModelSeed"), Variables,
		[this](const TSharedPtr<FJsonObject>& Envelope)
		{
			FString Summary;
			CrowdyStudioGql::ParseSeedResult(Envelope, Summary);
			SetStatus(Summary.IsEmpty() ? TEXT("Seed complete.") : Summary, false);
			FetchContainerTypes();
			// A seed is the write most likely to leave the model incoherent: it writes many objects at once, and a
			// function calling one seeded later in the same batch is only visible once the whole batch has landed.
			RunGameModelLint(true);
		});
}

// One in-flight schema-sync plan: the reflected desired schema + the server schema being read back. Held by
// TSharedRef across the async read fan-out so a plan survives its callbacks (and is released if abandoned).
struct FCrowdySchemaSyncRun
{
	int64 AppId = 0;                                            // the app this plan is pinned to (captured at plan start)
	TArray<FCrowdyDesiredContainerType> Desired;
	TArray<FCrowdyGameModelFunctionInput> DesiredFunctions;    // compiled from every UCrowdyEffect asset
	TArray<FCrowdyGameModelAutomationInput> DesiredAutomations; // the automations effects that run themselves lower to
	TArray<FCrowdyGameModelAutomationTriggerInput> DesiredTriggers; // their event triggers (when the event mode is chosen)
	TSet<FString> RecognizedFunctionNames;                      // every function name any effect asset claims (incl. skipped ones): never prune-delete these
	TSet<FString> RecognizedAutomationNames;                    // every automation name any effect asset claims (incl. skipped ones): never prune-delete these
	TSet<FString> RecognizedKitTypePrefixes;                    // deployed Game Kit container-type prefixes: keep kit types/props/functions off the prune list
	TSet<FString> RecognizedKitTypeNames;                       // exact deployed Game Kit type names: protects an empty-prefix kit the prefixes cannot
	TSet<FString> RecognizedKitFunctionNames;                   // exact deployed Game Kit function names: protects an empty-prefix kit's bare function names
	// Which effect asset declared each function / automation the plan looked at, including the ones it passed over.
	// Filled by the two gathers; nothing in the diff reads them, so they are carried purely for the view.
	TArray<FCrowdySchemaAuthorship> FunctionAuthorship;
	TArray<FCrowdySchemaAuthorship> AutomationAuthorship;
	// The plan's shared effect sweep: the reflected vocabulary the effects compile against, the cross-plan compile
	// cache, and the probe that decided which assets had to be streamed. Shared by both gathers so the project is
	// swept once and an unchanged effect is never loaded.
	FCrowdyEffectGatherContext EffectGather;
	TArray<FString> Warnings;                                   // pre-diff (e.g. a duplicate CrowdyContainer tag)
	TArray<FStudioContainerType> CurrentTypes;
	TMap<FString, TArray<FStudioPropertyDef>> CurrentPropsByType;
	TArray<FStudioFunction> CurrentFunctions;                  // the server's functions for the pinned app
	TArray<FStudioAutomation> CurrentAutomations;              // the server's automations for the pinned app
	TArray<FStudioAutomationTrigger> CurrentTriggers;          // the server's automation event triggers for the pinned app
	int64 SessionChannelId = 0;                                 // __crowdy_session_<appId>, read only to tell existence (0 = absent)
	int32 PendingPropReads = 0;
	bool bTypesRead = false;                                    // the type/prop read fan has joined
	// Whether the server's container types were actually READ, as opposed to the read being skipped because the
	// project declares none. CurrentTypes is empty either way, and the two cases mean opposite things: "the server
	// has no types" versus "nobody asked". Only the first may be published to the page.
	bool bServerTypesRead = false;
	bool bFunctionsRead = false;                                // the function read has returned
	bool bAutomationsRead = false;                             // the automation + trigger read has returned
	bool bChannelsRead = false;                                 // the session-channel read has returned
	bool bFailed = false;                                       // a read failed; FailSchemaPlan reported it once
	// Set the first time the plan resumes after its effect stream. A stream whose assets are already resident can
	// complete inside the request call itself, so the caller's own fallback path must not resume the same plan twice.
	bool bResumedAfterEffectStream = false;
};

void FCrowdyStudioController::ClearSchemaSyncState()
{
	PendingSyncTypeUpserts.Reset();
	PendingSyncPropUpserts.Reset();
	PendingSyncFunctionUpserts.Reset();
	PendingSyncAutomationUpserts.Reset();
	PendingSyncTriggerUpserts.Reset();
	PendingPruneTypes.Reset();
	PendingPruneProps.Reset();
	PendingPruneFunctions.Reset();
	PendingPruneAutomations.Reset();
	PlannedSyncAppId = 0;
	bPlannedSyncNeedsSessionChannel = false;
	PlannedSessionChannelFunctionKeys.Reset();
	// The five arrays above are gone, so any apply walk still in flight was built against a plan that no longer
	// exists. Moving this on is what makes its completion leave them alone: its entries are plan indices, and the
	// next plan refills the same positions with different entities.
	++SchemaPlanGeneration;
	// bAutoCreatingSessionChannel is deliberately NOT reset here: a plan that lands between the channel create and
	// the apply it resumes must not clear the single-shot latch, or that apply could create the channel a second time.
	//
	// SchemaApplySelection is NOT reset here for the same reason. A plan rebuilds all five pending arrays, and an
	// apply that resumes afterwards is meant to send what the user picked; clearing it here would silently widen that
	// resumed apply to everything. It is held by identity key rather than by index precisely so it can be
	// re-resolved against the rebuilt arrays. An app switch clears it, which is the case that matters.
	//
	// The retained per-app model snapshot is deliberately NOT dropped here either. This runs at the START of every
	// fresh plan, and a plan takes as long as its server reads: clearing the snapshot would blank the Source and
	// Status column of every row in the browser for that whole time, which reads as a page whose data failed to
	// load rather than as a page waiting for a newer answer. The snapshot is the LAST thing that was actually known,
	// so it stands until a finished plan replaces it in one step. A plan that fails leaves it standing too, which is
	// correct for the same reason; the report's own banner is what says the plan did not finish.
	SchemaSyncReport = FCrowdySchemaSyncReport();
	OnSchemaSyncReportChanged.Broadcast();
}

TSharedPtr<const FCrowdyModelSnapshot> FCrowdyStudioController::GetModelSnapshot() const
{
	if (SelectedAppId == 0)
	{
		return nullptr;
	}
	const TSharedPtr<const FCrowdyModelSnapshot>* Found = ModelSnapshotsByApp.Find(SelectedAppId);
	return Found ? *Found : nullptr;
}

ECrowdyStudioReadiness FCrowdyStudioController::GetSchemaReadiness() const
{
	// Unknown until a plan (or apply) has produced a valid report; NotReady when a plan/apply failed (StatusNote set)
	// or upserts are outstanding; Ready otherwise. A just-applied report counts as in-sync (its pending upserts were
	// written) even though its counts still show what was applied, so the strip does not flash amber right after a
	// successful apply.
	//
	// Server-only entities are deliberately NOT drift. A sync never deletes them, so folding them in here left the
	// indicator amber on an empty plan with no press on this strip able to clear it, beside a report saying there
	// was nothing to sync. They get the Advisory state instead, which the strip words as a review item.
	if (!SchemaSyncReport.bValid || !SchemaSyncReport.StatusNote.IsEmpty())
	{
		return SchemaSyncReport.StatusNote.IsEmpty() ? ECrowdyStudioReadiness::Unknown : ECrowdyStudioReadiness::NotReady;
	}
	if (SchemaSyncReport.HasPendingUpserts())
	{
		return ECrowdyStudioReadiness::NotReady;
	}
	// A plan that never issued a read has zero of everything because nothing was compared. Reading that as Ready
	// says the schema matches an app nobody asked, which can be carrying a kit deploy or a console-seeded schema.
	if (SchemaSyncReport.bServerNotCompared)
	{
		return ECrowdyStudioReadiness::Advisory;
	}
	return SchemaSyncReport.HasServerOnlyReview() ? ECrowdyStudioReadiness::Advisory : ECrowdyStudioReadiness::Ready;
}

void FCrowdyStudioController::SetSchemaPlanPhase(int64 AppId, const FString& Phase)
{
	// An empty phase ends the plan, whatever app it was for. Every path that abandons a plan has to reach here, or the
	// page keeps reporting work that stopped, which is worse than never having reported it: it says "wait" forever.
	SchemaPlanBusyAppId = Phase.IsEmpty() ? 0 : AppId;
	SchemaPlanPhase = Phase;
	OnSchemaPlanProgress.Broadcast();
}

void FCrowdyStudioController::CancelSchemaRegistryWait()
{
	if (!bSchemaPlanWaitingOnRegistry && !SchemaRegistryFilesLoadedHandle.IsValid())
	{
		return;
	}
	if (SchemaRegistryFilesLoadedHandle.IsValid())
	{
		if (FAssetRegistryModule* AssetRegistryModule =
			FModuleManager::GetModulePtr<FAssetRegistryModule>(TEXT("AssetRegistry")))
		{
			AssetRegistryModule->Get().OnFilesLoaded().Remove(SchemaRegistryFilesLoadedHandle);
		}
		SchemaRegistryFilesLoadedHandle.Reset();
	}
	bSchemaPlanWaitingOnRegistry = false;
	if (SchemaRegistryWaitAppId != 0)
	{
		SetSchemaPlanPhase(SchemaRegistryWaitAppId, FString());
		SchemaRegistryWaitAppId = 0;
	}
}

void FCrowdyStudioController::PlanSchemaSync()
{
	PlanSchemaSyncInternal();
}

void FCrowdyStudioController::PlanSchemaSyncInternal()
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app to sync the Game Model schema."), true);
		return;
	}

	// The asset registry's initial scan blocks everything a plan reads from it: the effect probe and the
	// duplicate-name index both call WaitForCompletion, which on a freshly launched editor stalls the game thread
	// for however much of the scan is left. Wait for it asynchronously here instead, so every blocking wait
	// further down the plan is a no-op by the time it runs.
	{
		IAssetRegistry& AssetRegistry =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		if (AssetRegistry.IsLoadingAssets())
		{
			if (bSchemaPlanWaitingOnRegistry)
			{
				if (SchemaRegistryWaitAppId == SelectedAppId)
				{
					SetStatus(TEXT("Still waiting for the asset registry to finish its initial scan; the plan will start shortly."), false);
					return;
				}
				// The app moved while waiting, so the pending wait now belongs to a plan nobody wants. Re-pin the
				// one subscription to the new app rather than stacking a second, and close the superseded app's
				// phase exactly as a superseded stream closes it.
				SetSchemaPlanPhase(SchemaRegistryWaitAppId, FString());
				SchemaRegistryWaitAppId = SelectedAppId;
				SetSchemaPlanPhase(SelectedAppId, TEXT("Waiting for the asset registry's initial scan"));
				return;
			}

			bSchemaPlanWaitingOnRegistry = true;
			SchemaRegistryWaitAppId = SelectedAppId;
			SetStatus(TEXT("Waiting for the asset registry to finish its initial scan..."), false);
			SetSchemaPlanPhase(SelectedAppId, TEXT("Waiting for the asset registry's initial scan"));

			// OnFilesLoaded broadcasts on the game thread, the same thread this runs on, so the scan cannot slip
			// to completed between the IsLoadingAssets check above and this subscription.
			SchemaRegistryFilesLoadedHandle = AssetRegistry.OnFilesLoaded().AddLambda([this]()
			{
				// The Remove below frees THIS LAMBDA'S OWN closure: the delegate instance owns the heap block the
				// [this] capture lives in, and multicast Remove unbinds (and frees) immediately even mid-broadcast.
				// Pin the controller to the stack first, and touch members only through the pin after the Remove.
				FCrowdyStudioController* const Self = this;
				FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get()
					.OnFilesLoaded().Remove(Self->SchemaRegistryFilesLoadedHandle);
				Self->SchemaRegistryFilesLoadedHandle.Reset();
				Self->bSchemaPlanWaitingOnRegistry = false;

				const int64 WaitAppId = Self->SchemaRegistryWaitAppId;
				Self->SchemaRegistryWaitAppId = 0;

				// The selection moved (or was cleared) while the registry was scanning: drop this plan as any
				// superseded continuation is dropped.
				if (WaitAppId != Self->SelectedAppId)
				{
					Self->SetSchemaPlanPhase(WaitAppId, FString());
					return;
				}
				Self->PlanSchemaSyncInternal();
			});
			return;
		}
	}

	// The container-asset stream that opens a plan, guarded the same way and for the same reason as the effect stream
	// below. There is nothing to cancel for a foreign app: that stream's completion checks the app itself and returns
	// without starting anything, so this plan may simply proceed while it drains.
	if (bSchemaContainerLoadInFlight && SchemaContainerLoadAppId == SelectedAppId)
	{
		SetStatus(TEXT("Still reading this project's Game Model containers; the plan will finish shortly."), false);
		return;
	}

	// One effect stream at a time per app. Without this a second click while the first is still reading would start a
	// second stream over the same assets and leave two plans racing to repaint the same report.
	//
	// A stream belonging to another app is a different matter: the app was switched while it was reading, so its
	// completion is dropped on arrival and no plan will ever come of it. Refusing here on its account would tell the
	// user a plan is on the way for the app they just selected while nothing at all had been started for it. Cancel it
	// and let this plan proceed.
	if (bSchemaEffectStreamInFlight)
	{
		if (SchemaEffectStreamAppId == SelectedAppId)
		{
			SetStatus(TEXT("Still reading this project's effect assets; the plan will finish shortly."), false);
			return;
		}

		if (SchemaEffectStreamHandle.IsValid())
		{
			SchemaEffectStreamHandle->CancelHandle();
			SchemaEffectStreamHandle.Reset();
		}
		bSchemaEffectStreamInFlight = false;
		SchemaEffectStreamAppId = 0;
	}

	// A fresh plan supersedes any prior plan (and unblocks Apply after a stalled apply left pending state).
	ClearSchemaSyncState();

	// A Blueprint marked as a Game Model container that has not been opened this session is not loaded, so
	// GatherContainerClasses (which iterates loaded classes) would miss it. Bring the project's container assets in
	// first. The scan asks the asset registry which Blueprints are containers before opening anything, so only the
	// containers and the assets the registry cannot answer for are read; a project that has not been re-saved since
	// the tags arrived still loads everything, at exactly the speed it used to. STREAMED, not force-loaded: this was
	// a synchronous load behind a modal dialog and it held the game thread for seconds on a real project, which is
	// most of what made the first press of this button feel broken. Idempotent, so a later plan in the same session
	// finds everything resident and continues almost immediately.
	const FCrowdyContainerScanPlan ContainerScanPlan = FCrowdyContainerScan::BuildContainerScanPlan();
	SetStatus(ContainerScanPlan.DescribeStatus(), false);
	SetSchemaPlanPhase(SelectedAppId, TEXT("Reading this project's Game Model containers"));
	bSchemaContainerLoadInFlight = true;
	SchemaContainerLoadAppId = SelectedAppId;

	const int64 ContainerLoadAppId = SelectedAppId;
	FCrowdySchemaSync::StreamContainerScanPlan(ContainerScanPlan,
		[this, ContainerLoadAppId]()
		{
			bSchemaContainerLoadInFlight = false;
			SchemaContainerLoadAppId = 0;

			// The selection moved while the assets were streaming, so this plan is for an app the user has left.
			// Dropped exactly as FinishSchemaPlan drops a superseded run.
			if (ContainerLoadAppId != SelectedAppId)
			{
				SetSchemaPlanPhase(ContainerLoadAppId, FString());
				return;
			}

			ContinueSchemaPlanAfterContainerLoad();
		});
}

void FCrowdyStudioController::ContinueSchemaPlanAfterContainerLoad()
{
	const TArray<UClass*> Classes = FCrowdySchemaSync::GatherContainerClasses();
	const TSharedRef<FCrowdySchemaSyncRun> Run = MakeShared<FCrowdySchemaSyncRun>();
	Run->AppId = SelectedAppId; // pin the whole plan (read + apply) to the app selected right now
	Run->Desired = FCrowdySchemaSync::BuildDesiredSchema(Classes, Run->Warnings);

	// Set up the effect sweep before anything is loaded. The gathers compile each effect against the vocabulary
	// reflected just above, so that vocabulary is half of what decides whether a previous plan's compiled result
	// still stands; the other half is the asset's own saved content. BeginPlan drops the whole store when a project
	// setting the compile reads has moved, since no per-asset key can see that.
	FCrowdyEffectPlanCache& PlanCache = FCrowdyEffectPlanCache::Get();
	PlanCache.BeginPlan(FCrowdyEffectPlanCache::ComputeGlobalSalt());
	Run->EffectGather.DesiredTypes = Run->Desired;
	Run->EffectGather.Cache = &PlanCache;
	FCrowdyEffectPlanCache::ProbeEffectAssets(Run->EffectGather);

	TArray<FSoftObjectPath> EffectsToStream;
	EffectsToStream.Reserve(Run->EffectGather.Probes.Num());
	for (const FCrowdyEffectAssetProbe& Probe : Run->EffectGather.Probes)
	{
		if (Probe.NeedsLoad())
		{
			EffectsToStream.Add(Probe.ObjectPath);
		}
	}

	if (EffectsToStream.Num() == 0)
	{
		// Nothing changed since the last plan (or the project has no effects at all), so there is nothing to wait for.
		ContinueSchemaPlanAfterEffectStream(Run);
		return;
	}

	bSchemaEffectStreamInFlight = true;
	SchemaEffectStreamAppId = Run->AppId;
	SetStatus(FString::Printf(TEXT("Reading %d effect asset(s)..."), EffectsToStream.Num()), false);
	// The count is worth saying: this is the phase that takes the longest on a cold editor, and a number moving from
	// one plan to the next is also how a reader sees the compile cache doing its job.
	SetSchemaPlanPhase(Run->AppId, EffectsToStream.Num() == 1
		? FString(TEXT("Reading 1 effect asset"))
		: FString::Printf(TEXT("Reading %d effect assets"), EffectsToStream.Num()));

	// Streamed, not force-loaded: loading every effect synchronously froze the editor for seconds on a project with
	// a real number of them, and this is the most-used button on the page. When the completion runs, every asset the
	// plan needs is resident and the gathers become in-memory lookups.
	SchemaEffectStreamHandle = UAssetManager::GetStreamableManager().RequestAsyncLoad(
		MoveTemp(EffectsToStream),
		FStreamableDelegate::CreateLambda([this, Run]()
		{
			bSchemaEffectStreamInFlight = false;
			SchemaEffectStreamAppId = 0;
			SchemaEffectStreamHandle.Reset();
			ContinueSchemaPlanAfterEffectStream(Run);
		}),
		FStreamableManager::AsyncLoadHighPriority);

	if (!SchemaEffectStreamHandle.IsValid())
	{
		// The request was refused outright, so no completion is ever coming. Carry on rather than leaving the plan
		// half-started behind a guard that nothing will ever clear; the gather falls back to loading what it needs.
		bSchemaEffectStreamInFlight = false;
		SchemaEffectStreamAppId = 0;
		ContinueSchemaPlanAfterEffectStream(Run);
	}
}

void FCrowdyStudioController::ContinueSchemaPlanAfterEffectStream(const TSharedRef<FCrowdySchemaSyncRun>& Run)
{
	if (Run->bResumedAfterEffectStream)
	{
		return;
	}
	Run->bResumedAfterEffectStream = true;

	// The stream is pinned to the app the plan started under, exactly like the server reads. A completion that lands
	// after the user switched apps is dropped rather than reading app A's assets into app B's plan.
	if (Run->AppId != SelectedAppId)
	{
		SetSchemaPlanPhase(Run->AppId, FString());
		return;
	}

	Run->DesiredFunctions = FCrowdySchemaSync::GatherDesiredFunctions(
		Run->Warnings, Run->RecognizedFunctionNames, Run->FunctionAuthorship, &Run->EffectGather);

	// Augment the desired schema with the SDK-owned Model Collection plumbing (a reserved crowdy_rev property + a
	// per-type touch function) before the reads, so the collection functions ride the same channel-id resolution
	// (InjectSessionChannelTarget) and diff (DiffFunctions, prune-protected via RecognizedFunctionNames) as effects.
	// Deliberately after the gather: this adds schema no designer authored, which no effect compiles against.
	FCrowdySchemaSync::AppendReservedCollectionSchema(
		Run->Desired, Run->DesiredFunctions, Run->RecognizedFunctionNames, Run->Warnings);

	// The automations effects that opt into running themselves lower to (plus their event triggers), read from the same
	// compiled effect results as the functions. Gather warnings fold into the plan warnings exactly as the functions do.
	Run->DesiredAutomations = FCrowdySchemaSync::GatherDesiredAutomations(
		Run->Warnings, Run->RecognizedAutomationNames, Run->DesiredTriggers, Run->AutomationAuthorship,
		&Run->EffectGather);

	// Kit-owned schema (types/props/functions emitted from a deployed Game Kit) is not reflected from a code class, so
	// the diff would flag it as server-only drift. Collect the recognized kit type prefixes now so DiffSchema and
	// DiffFunctions keep that schema off the prune list.
	Run->RecognizedKitTypePrefixes = GatherRecognizedKitTypePrefixes();
	// The exact-name layer of the same protection, loaded per app from the persisted deploy record: covers a kit whose
	// type prefix is empty, whose bare type/function names no prefix rule could recognize.
	Run->RecognizedKitTypeNames = LoadPersistedKitTypeNames(Run->AppId);
	Run->RecognizedKitFunctionNames = LoadPersistedKitFunctionNames(Run->AppId);

	if (Run->Desired.Num() == 0 && Run->DesiredFunctions.Num() == 0)
	{
		// Nothing reflectable: a valid, empty plan (still surfaces any duplicate-tag / effect-compile warnings). Its
		// zero counts mean nothing was compared, not that the two sides agree: no read is issued below this branch,
		// so the app may hold a kit deploy or a console-seeded schema this plan has never seen.
		SchemaSyncReport = FCrowdySchemaSync::BuildReport(FCrowdySchemaDelta(), Run->Warnings, /*bApplied*/ false);
		SchemaSyncReport.bServerNotCompared = true;
		SetStatus(TEXT("No CrowdyContainer classes with Server Owned attributes and no effect assets were found."), false);
		SetSchemaPlanPhase(Run->AppId, FString());
		OnSchemaSyncReportChanged.Broadcast();
		return;
	}

	SetStatus(TEXT("Reading server schema..."), false);
	SetSchemaPlanPhase(Run->AppId, TEXT("Reading the server's schema"));

	// Two independent read fans join in TryFinishSchemaPlan: the container-type/property reads and the function
	// read. A plan with no desired types skips the type read (nothing to diff there) but still reads functions.
	if (Run->Desired.Num() > 0)
	{
		const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
		SetBigIntField(Variables, TEXT("appId"), Run->AppId);
		SendGame(ECrowdyCppApiDomain::GameModel, TEXT("GameModelContainerTypes"), Variables,
			[this, Run](const TSharedPtr<FJsonObject>& Envelope)
			{
				TArray<TSharedPtr<FStudioContainerType>> Types;
				CrowdyStudioGql::ParseContainerTypes(Envelope, TEXT("gameModelContainerTypes"), Types);
				for (const TSharedPtr<FStudioContainerType>& T : Types)
				{
					if (T.IsValid())
					{
						Run->CurrentTypes.Add(*T);
					}
				}
				Run->bServerTypesRead = true;
				ReadDesiredTypeProps(Run);
			},
			[this, Run]() { FailSchemaPlan(Run, TEXT("reading server container types")); });
	}
	else
	{
		Run->bTypesRead = true; // no desired types -> no type/prop read; the function read drives the join
	}

	ReadCurrentFunctions(Run);
	ReadCurrentAutomations(Run);
	ReadSessionChannel(Run);
}

void FCrowdyStudioController::ReadSessionChannel(const TSharedRef<FCrowdySchemaSyncRun>& Run)
{
	// Read whether the app's default session channel exists. The name is deterministic (__crowdy_session_<appId>; the
	// runtime UCrowdyChannels, the notification's own $session_channel_name and the Channels view all converge on
	// it), so the notification does not need the id; only the existence question does, because naming a channel the
	// app does not have reaches nobody. Not found -> id stays 0 and Apply auto-creates the channel. A failed read
	// reports "plan failed" (FailSchemaPlan).
	const FString SessionName = FString::Printf(TEXT("__crowdy_session_%lld"), Run->AppId);
	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), Run->AppId);

	SendGame(ECrowdyCppApiDomain::Channels, TEXT("Channels"), Variables,
		[this, Run, SessionName](const TSharedPtr<FJsonObject>& Envelope)
		{
			TArray<TSharedPtr<FStudioGroup>> Channels;
			CrowdyStudioGql::ParseGroups(Envelope, TEXT("channels"), Channels);
			for (const TSharedPtr<FStudioGroup>& Channel : Channels)
			{
				if (Channel.IsValid() && Channel->Name == SessionName)
				{
					Run->SessionChannelId = Channel->GroupId;
					break;
				}
			}
			Run->bChannelsRead = true;
			TryFinishSchemaPlan(Run);
		},
		[this, Run]() { FailSchemaPlan(Run, TEXT("reading the app's session channel")); });
}

void FCrowdyStudioController::ReadDesiredTypeProps(const TSharedRef<FCrowdySchemaSyncRun>& Run)
{
	Run->PendingPropReads = Run->Desired.Num();
	for (const FCrowdyDesiredContainerType& Desired : Run->Desired)
	{
		const FString TypeName = Desired.TypeName;
		const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
		SetBigIntField(Variables, TEXT("appId"), Run->AppId); // pinned app id, not the (possibly-changed) selection
		Variables->SetStringField(TEXT("typeName"), TypeName);

		SendGame(ECrowdyCppApiDomain::GameModel, TEXT("GameModelPropertyDefs"), Variables,
			[this, Run, TypeName](const TSharedPtr<FJsonObject>& Envelope)
			{
				TArray<TSharedPtr<FStudioPropertyDef>> Defs;
				CrowdyStudioGql::ParsePropertyDefs(Envelope, TEXT("gameModelPropertyDefs"), Defs);
				TArray<FStudioPropertyDef>& Out = Run->CurrentPropsByType.FindOrAdd(TypeName);
				for (const TSharedPtr<FStudioPropertyDef>& P : Defs)
				{
					if (P.IsValid())
					{
						Out.Add(*P);
					}
				}
				// A failed read now reports "plan failed" (FailSchemaPlan) instead of stalling; a partial read that
				// still joins only ever yields safe, redundant upserts.
				if (--Run->PendingPropReads <= 0)
				{
					Run->bTypesRead = true;
					TryFinishSchemaPlan(Run);
				}
			},
			[this, Run, TypeName]() { FailSchemaPlan(Run, FString::Printf(TEXT("reading property definitions for '%s'"), *TypeName)); });
	}
}

void FCrowdyStudioController::ReadCurrentFunctions(const TSharedRef<FCrowdySchemaSyncRun>& Run)
{
	// One read of every function for the pinned app (no containerTypeName filter). Server-only functions are
	// surfaced app-wide as prune candidates, exactly like server-only types/props.
	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), Run->AppId);

	SendGame(ECrowdyCppApiDomain::GameModel, TEXT("GameModelFunctions"), Variables,
		[this, Run](const TSharedPtr<FJsonObject>& Envelope)
		{
			TArray<TSharedPtr<FStudioFunction>> Fns;
			CrowdyStudioGql::ParseFunctions(Envelope, TEXT("gameModelFunctions"), Fns);
			for (const TSharedPtr<FStudioFunction>& F : Fns)
			{
				if (F.IsValid())
				{
					Run->CurrentFunctions.Add(*F);
				}
			}
			Run->bFunctionsRead = true;
			TryFinishSchemaPlan(Run);
		},
		[this, Run]() { FailSchemaPlan(Run, TEXT("reading server functions")); });
}

void FCrowdyStudioController::ReadCurrentAutomations(const TSharedRef<FCrowdySchemaSyncRun>& Run)
{
	// Read every automation for the pinned app, then (chained) every event trigger. Triggers reference their automation
	// by id, so the automations must be in hand to resolve each trigger's automation name for the name-keyed diff.
	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), Run->AppId);

	SendGame(ECrowdyCppApiDomain::GameModel, TEXT("GameModelAutomations"), Variables,
		[this, Run](const TSharedPtr<FJsonObject>& Envelope)
		{
			CrowdyStudioGql::ParseAutomations(Envelope, TEXT("gameModelAutomations"), Run->CurrentAutomations);

			const TSharedPtr<FJsonObject> TriggerVariables = MakeShared<FJsonObject>();
			SetBigIntField(TriggerVariables, TEXT("appId"), Run->AppId);
			SendGame(ECrowdyCppApiDomain::GameModel, TEXT("GameModelAutomationTriggers"), TriggerVariables,
				[this, Run](const TSharedPtr<FJsonObject>& TriggerEnvelope)
				{
					CrowdyStudioGql::ParseAutomationTriggers(TriggerEnvelope, TEXT("gameModelAutomationTriggers"),
						Run->CurrentAutomations, Run->CurrentTriggers);
					Run->bAutomationsRead = true;
					TryFinishSchemaPlan(Run);
				},
				[this, Run]() { FailSchemaPlan(Run, TEXT("reading server automation triggers")); });
		},
		[this, Run]() { FailSchemaPlan(Run, TEXT("reading server automations")); });
}

void FCrowdyStudioController::TryFinishSchemaPlan(const TSharedRef<FCrowdySchemaSyncRun>& Run)
{
	// All read fans (type/prop, function, automation/trigger, session channel) must have landed before the diff sees a
	// complete snapshot. A run already declared failed (one read errored) is not finished by a sibling read that later succeeds.
	if (!Run->bFailed && Run->bTypesRead && Run->bFunctionsRead && Run->bAutomationsRead && Run->bChannelsRead)
	{
		FinishSchemaPlan(Run);
	}
}

void FCrowdyStudioController::FinishSchemaPlan(const TSharedRef<FCrowdySchemaSyncRun>& Run)
{
	// Drop a stale plan: if the app selection changed after this run started (SelectApp -> ClearSchemaSyncState),
	// repainting Pending*/report here would surface app A's diff under the now-selected app B. Apply/prune already
	// refuse on the pin; this also stops a superseded plan from ever repainting the view. Also cancel any pending
	// auto-create continuation so it cannot apply against the wrong app.
	if (Run->AppId != SelectedAppId)
	{
		bAutoCreatingSessionChannel = false;
		SetSchemaPlanPhase(Run->AppId, FString());
		return;
	}

	// Record whether this plan needs the app's session channel (an effect declares a channel notification) and
	// whether it exists, so the setup strip and the apply auto-create can consult it. The strip only flags the
	// channel "Missing" (NotReady) when the plan actually needs it; a project with no channel-notifying effect
	// leaves it Unknown (neutral) rather than falsely amber.
	bPlannedSyncNeedsSessionChannel = (Run->SessionChannelId == 0)
		&& FCrowdySchemaSync::AnyFunctionNeedsSessionChannel(Run->DesiredFunctions);

	// Which functions those are, by scoped name, taken from the desired schema while it still says so. A function's
	// upserts carry the notification but nothing that says which channel it names, so nothing downstream could tell
	// from them which functions depend on the session channel existing. A selective apply has to answer exactly that
	// question, and this is the last point at which the answer exists.
	PlannedSessionChannelFunctionKeys.Reset();
	for (const FCrowdyGameModelFunctionInput& Desired : Run->DesiredFunctions)
	{
		// Put to the same rule the whole-plan question above puts, one function at a time, so the two can never
		// disagree about what counts as needing the channel.
		if (FCrowdySchemaSync::AnyFunctionNeedsSessionChannel({ Desired }))
		{
			PlannedSessionChannelFunctionKeys.Add(
				FCrowdySchemaSync::ScopedNameKey(Desired.ContainerTypeName, Desired.Name));
		}
	}

	if (Run->SessionChannelId != 0)
	{
		SessionChannelReadiness = ECrowdyStudioReadiness::Ready;
	}
	else
	{
		SessionChannelReadiness = bPlannedSyncNeedsSessionChannel
			? ECrowdyStudioReadiness::NotReady : ECrowdyStudioReadiness::Unknown;
	}

	// Address the effect channel notifications at the app's session channel BY NAME before diffing, so the desired
	// notification is complete whether or not that channel exists yet, and a function copied into another app still
	// notifies the app it is running in.
	FCrowdySchemaSync::InjectSessionChannelTarget(Run->DesiredFunctions);

	FCrowdySchemaDelta Delta =
		FCrowdySchemaSync::DiffSchema(Run->Desired, Run->CurrentTypes, Run->CurrentPropsByType,
			Run->RecognizedKitTypePrefixes, Run->RecognizedKitTypeNames);
	FCrowdySchemaSync::DiffFunctions(
		Run->DesiredFunctions, Run->CurrentFunctions, Delta, Run->RecognizedFunctionNames,
		Run->RecognizedKitTypePrefixes, Run->RecognizedKitFunctionNames);

	// Automations diff after functions so the dangling-function and not-autonomous checks can see the same desired
	// function set. Kit automations are protected by the type-prefix set for now; exact-name persistence for kit
	// automations is a named follow-up, so no kit-automation-name set is threaded yet (empty).
	FCrowdySchemaSync::DiffAutomations(
		Run->DesiredAutomations, Run->DesiredTriggers,
		Run->CurrentAutomations, Run->CurrentTriggers,
		Run->DesiredFunctions, Delta,
		Run->RecognizedAutomationNames,
		Run->RecognizedKitTypePrefixes,
		TSet<FString>());

	// A new plan, so anything still walking the previous one is addressing indices that no longer name what it sent.
	++SchemaPlanGeneration;

	PendingSyncTypeUpserts = Delta.TypeUpserts;
	PendingSyncPropUpserts = Delta.PropUpserts;
	PendingSyncFunctionUpserts = Delta.FunctionUpserts;
	PendingSyncAutomationUpserts = Delta.AutomationUpserts;
	PendingSyncTriggerUpserts = Delta.TriggerUpserts;
	PendingPruneTypes = Delta.ServerOnlyTypes;   // the opt-in prune deletes these
	PendingPruneProps = Delta.ServerOnlyProps;
	PendingPruneFunctions = Delta.ServerOnlyFunctions;
	PendingPruneAutomations = Delta.ServerOnlyAutomations;
	PlannedSyncAppId = Run->AppId; // apply/prune refuse if the selection has changed away from this
	SchemaSyncReport = FCrowdySchemaSync::BuildReport(Delta, Run->Warnings, /*bApplied*/ false);

	// Retain what this plan learned, so the Models browser can say where each entity came from without re-planning.
	// Reached only past the superseded-run check above, so a plan whose app was switched away never publishes one.
	{
		FCrowdyModelSnapshotPlan Captured;
		Captured.AppId = Run->AppId;
		Captured.DesiredTypes = &Run->Desired;
		Captured.DesiredFunctions = &Run->DesiredFunctions;
		Captured.DesiredAutomations = &Run->DesiredAutomations;
		Captured.DesiredTriggers = &Run->DesiredTriggers;
		Captured.FunctionAuthorship = &Run->FunctionAuthorship;
		Captured.AutomationAuthorship = &Run->AutomationAuthorship;
		Captured.Delta = &Delta;
		Captured.KitTypePrefixes = &Run->RecognizedKitTypePrefixes;
		Captured.KitTypeNames = &Run->RecognizedKitTypeNames;
		Captured.KitFunctionNames = &Run->RecognizedKitFunctionNames;
		// No kit automation names are threaded into DiffAutomations yet, so none are claimed here either: what this
		// page calls kit-owned stays exactly what the prune declines to offer.
		ModelSnapshotsByApp.Add(Run->AppId, MakeShared<FCrowdyModelSnapshot>(CaptureModelSnapshot(Captured)));
	}

	// Publish the server side the plan just read, so the rows and the verdict about them arrive together. Ordered
	// before the announcements below for that reason: a view that rebuilt on the snapshot alone would classify rows
	// it has not been given yet.
	IngestPlanServerReads(Run);

	// The plan is done. Cleared before the announcements so a view that rebuilds on any of them sees an idle page
	// rather than one still reporting a check that has already produced its answer.
	SetSchemaPlanPhase(Run->AppId, FString());

	if (Delta.IsEmpty())
	{
		SetStatus(TEXT("Schema is in sync; no changes to apply."), false);
	}
	else
	{
		SetStatus(FString::Printf(TEXT("Plan: %d change(s) to apply, %d warning(s)."),
			SchemaSyncReport.UpsertCount(), SchemaSyncReport.Warnings.Num()), false);
	}
	OnSchemaSyncReportChanged.Broadcast();
	OnModelSnapshotChanged.Broadcast();
}

void FCrowdyStudioController::IngestPlanServerReads(const TSharedRef<FCrowdySchemaSyncRun>& Run)
{
	// A plan reads this app's container types, functions and automations from the server and used them only for the
	// diff. They are the same three reads the page's Refresh issues, so publishing them here fills the browser for no
	// extra round trip, and leaves the schema on screen the one the plan's verdict was actually computed against
	// rather than whatever an earlier Refresh happened to leave behind.

	// Only when the read genuinely happened. A plan whose project declares no container classes skips it entirely and
	// leaves CurrentTypes empty, and publishing that would report an app with a populated server schema as having no
	// models at all.
	if (Run->bServerTypesRead)
	{
		ContainerTypes.Reset(Run->CurrentTypes.Num());
		for (const FStudioContainerType& Type : Run->CurrentTypes)
		{
			ContainerTypes.Add(MakeShared<FStudioContainerType>(Type));
		}
		MarkFamilyLoadedFromPlan(ECrowdyModelFamily::Models, Run->AppId);
	}

	// The app-wide store only. Functions is the mirror of whatever container type a view last asked for, and this read
	// named no type at all, so writing the whole app into it would answer a question that view never asked.
	UnfilteredFunctions.Reset(Run->CurrentFunctions.Num());
	for (const FStudioFunction& Function : Run->CurrentFunctions)
	{
		UnfilteredFunctions.Add(MakeShared<FStudioFunction>(Function));
	}

	// Both assigned together, exactly as FetchAutomations does. An automation standing beside no trigger renders with
	// its trigger phrase missing, and any refresh landing in that window paints it that way.
	Automations.Reset(Run->CurrentAutomations.Num());
	for (const FStudioAutomation& Automation : Run->CurrentAutomations)
	{
		Automations.Add(MakeShared<FStudioAutomation>(Automation));
	}
	AutomationTriggers.Reset(Run->CurrentTriggers.Num());
	for (const FStudioAutomationTrigger& Trigger : Run->CurrentTriggers)
	{
		AutomationTriggers.Add(MakeShared<FStudioAutomationTrigger>(Trigger));
	}

	// A plan reads the functions, the automations and their triggers unconditionally, so all three lists are now this
	// app's and an empty one means it has none.
	MarkFamilyLoadedFromPlan(ECrowdyModelFamily::Functions, Run->AppId);
	MarkFamilyLoadedFromPlan(ECrowdyModelFamily::Automations, Run->AppId);
	MarkFamilyLoadedFromPlan(ECrowdyModelFamily::AutomationTriggers, Run->AppId);

	// The lists now hold this app's schema, so the page has nothing left to load for it. Set even when the type read
	// was skipped: the functions and automations were still published, and a second automatic load would re-read all
	// three to learn nothing new.
	GameModelListsAppId = Run->AppId;

	OnContainerTypesChanged.Broadcast();
	OnFunctionsChanged.Broadcast();
	OnAutomationsChanged.Broadcast();
}

void FCrowdyStudioController::FailSchemaPlan(const TSharedRef<FCrowdySchemaSyncRun>& Run, const FString& Context)
{
	if (Run->bFailed)
	{
		return; // report a plan failure once, even if several reads error
	}
	Run->bFailed = true;

	// However this run ends, it has stopped, so the page must stop reporting it as running. A failed plan that left
	// the indicator up would be the worst of both: the error in the status line and a spinner saying to keep waiting.
	SetSchemaPlanPhase(Run->AppId, FString());

	// A superseded run (its app was switched away) is dropped silently, exactly like FinishSchemaPlan.
	if (Run->AppId != SelectedAppId)
	{
		bAutoCreatingSessionChannel = false;
		return;
	}

	// Drop the half-built plan so Apply cannot run against a partial read. The ephemeral status line already carries
	// the specific server error (SendGame set it); the report panel gets a durable "re-plan" banner naming the read.
	ClearSchemaSyncState();
	bAutoCreatingSessionChannel = false;
	SessionChannelReadiness = ECrowdyStudioReadiness::Unknown;

	SchemaSyncReport.bValid = true;
	SchemaSyncReport.StatusNote = FString::Printf(
		TEXT("Plan failed while %s. The server returned an error or did not respond (see the status message). Re-plan when it is reachable."),
		*Context);
	OnSchemaSyncReportChanged.Broadcast();
}

FCrowdyApplyPlanInput FCrowdyStudioController::MakeSchemaApplyPlanInput() const
{
	FCrowdyApplyPlanInput Input;
	// The app the plan was computed for, never the live selection: every op the apply issues names this one, and a
	// selection that has moved since is refused rather than silently retargeted.
	Input.AppId = PlannedSyncAppId;
	Input.Types = &PendingSyncTypeUpserts;
	Input.Props = &PendingSyncPropUpserts;
	Input.Functions = &PendingSyncFunctionUpserts;
	Input.Automations = &PendingSyncAutomationUpserts;
	Input.Triggers = &PendingSyncTriggerUpserts;
	return Input;
}

void FCrowdyStudioController::SetSchemaApplySelection(const TArray<FString>& UnitKeys)
{
	SchemaApplySelection = UnitKeys;
	bSchemaApplySelectionIsAll = false;
}

void FCrowdyStudioController::ClearSchemaApplySelection()
{
	SchemaApplySelection.Reset();
	bSchemaApplySelectionIsAll = true;
}

bool FCrowdyStudioController::ApplySchemaSelection(const FCrowdyApplyPlan& Plan, int64 ExpectedAppId)
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app to apply the schema."), true);
		return false;
	}
	// Two comparisons against two sources: the plan carries the app its units were built from, ExpectedAppId is the
	// app the caller believes it is showing, and both have to agree with the live selection and with the plan that
	// is actually pending. A guard comparing one of these to itself would agree however stale the sheet had become.
	if (Plan.AppId != SelectedAppId || ExpectedAppId != SelectedAppId || Plan.AppId != PlannedSyncAppId)
	{
		SetStatus(TEXT("This selection was made for a different app. Re-plan for the current app before applying."), true);
		return false;
	}
	if (!Plan.bSendable)
	{
		SetStatus(Plan.Sheet.BlockedReason.IsEmpty()
			? FString(TEXT("Nothing to send. Press Preview changes first."))
			: Plan.Sheet.BlockedReason, true);
		return false;
	}

	SetSchemaApplySelection(Plan.SelectedKeys);
	ApplySchemaSync();
	return true;
}

void FCrowdyStudioController::ApplySchemaSync()
{
	if (SelectedAppId == 0)
	{
		bAutoCreatingSessionChannel = false;
		SetStatus(TEXT("Select an app to apply the schema."), true);
		return;
	}
	if (PendingSyncTypeUpserts.Num() + PendingSyncPropUpserts.Num() + PendingSyncFunctionUpserts.Num()
		+ PendingSyncAutomationUpserts.Num() + PendingSyncTriggerUpserts.Num() == 0)
	{
		bAutoCreatingSessionChannel = false;
		SetStatus(TEXT("Nothing to apply. Run a plan first, or the schema already matches code."), false);
		return;
	}
	// The plan is pinned to the app it was computed against; never apply app A's diff to a now-selected app B.
	if (SelectedAppId != PlannedSyncAppId)
	{
		bAutoCreatingSessionChannel = false;
		SetStatus(TEXT("This plan was computed for a different app. Re-plan for the current app before applying."), true);
		return;
	}

	// What this press actually sends. Everything is the default, and the whole of the plan is what "everything"
	// means: the selection surface hides the SDK's own wiring, so a model whose only pending changes ARE that wiring
	// has no tickable row at all, and taking the selectable rows here would leave it permanently unsent.
	//
	// Everything that CAN be picked, though. A unit whose owning model nothing could name is refused as a pick, and
	// picking it anyway makes the whole plan unsendable, so one effect with an unresolved target model would block
	// every unrelated change in the app while unticking any single row released them all. It still takes part in the
	// closure as a candidate, so a selection that genuinely depends on it is still refused, which is the case that
	// refusal is for.
	const FCrowdyApplyPlanInput PlanInput = MakeSchemaApplyPlanInput();
	TArray<FString> SelectionKeys;
	if (bSchemaApplySelectionIsAll)
	{
		const TArray<FCrowdyApplyUnit> AllUnits = CrowdyApplySelection::BuildUnits(PlanInput);
		SelectionKeys.Reserve(AllUnits.Num());
		for (const FCrowdyApplyUnit& Unit : AllUnits)
		{
			FString UnusedReason;
			if (CrowdyApplySelection::CanSelect(Unit, UnusedReason))
			{
				SelectionKeys.Add(Unit.IdentityKey());
			}
		}
	}
	else
	{
		// Re-resolved against the arrays as they stand now, which is what lets a selection survive a plan landing
		// between the pick and the apply: a plan destroys and rebuilds all five, so an index would name a different
		// entity afterwards while an identity key either still names the same one or is dropped.
		SelectionKeys = SchemaApplySelection;
	}

	const FCrowdyApplyPlan Selection = CrowdyApplySelection::BuildPlan(PlanInput, SelectionKeys);
	if (!Selection.bSendable)
	{
		// Enforced here as well as in the panel that renders it, so a refusal cannot be lost by a widget that
		// forgot to ask. Every refusal carries its own sentence; there is no generic failure.
		bAutoCreatingSessionChannel = false;
		SetStatus(Selection.Sheet.BlockedReason.IsEmpty()
			? FString(TEXT("Nothing to apply. Run a plan first, or the schema already matches code."))
			: Selection.Sheet.BlockedReason, true);
		return;
	}

	// If this plan needs the app's session channel for an effect's model-changed notification but it does not exist,
	// create it once and then apply, so the designer's single Apply click also provisions the channel. The plan
	// already carries the notification (it names the channel rather than resolving an id), so what is applied is the
	// plan the designer previewed rather than a re-diff they never saw. Single-shot: bAutoCreatingSessionChannel
	// latches so the re-entry below cannot create a second time.
	//
	// Subset-aware: the whole plan needing the channel is not the same question as THIS selection needing it, and a
	// selection with no channel-notifying function in it has no use for a channel the create would provision.
	if (bPlannedSyncNeedsSessionChannel && !bAutoCreatingSessionChannel
		&& ClosedSetNeedsSessionChannel(Selection.ClosedSet))
	{
		bAutoCreatingSessionChannel = true;
		SetStatus(TEXT("Creating the app's session channel so effect notifications can target it, then applying..."), false);

		// Must match the runtime's deterministic name (UCrowdyChannels::GetSessionChannelName): __crowdy_session_<appId>.
		const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
		SetBigIntField(Input, TEXT("appId"), PlannedSyncAppId);
		Input->SetStringField(TEXT("name"), FString::Printf(TEXT("__crowdy_session_%lld"), PlannedSyncAppId));
		Input->SetStringField(TEXT("description"), TEXT("Reliable-RPC session channel (auto)"));
		Input->SetBoolField(TEXT("membersCanSend"), true);
		Input->SetStringField(TEXT("membershipPolicy"), TEXT("open"));

		const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
		Variables->SetObjectField(TEXT("input"), Input);

		const int64 CreatedForApp = PlannedSyncAppId;
		SendGame(ECrowdyCppApiDomain::Channels, TEXT("CreateChannel"), Variables,
			[this, CreatedForApp](const TSharedPtr<FJsonObject>&)
			{
				// If the app was switched while the create was in flight, do not stamp readiness or re-plan for the
				// wrong app; SelectApp already dropped the latch.
				if (SelectedAppId != CreatedForApp)
				{
					bAutoCreatingSessionChannel = false;
					return;
				}
				SessionChannelReadiness = ECrowdyStudioReadiness::Ready;
				bPlannedSyncNeedsSessionChannel = false;
				// Straight back into the apply the create interrupted. The latch is still set, so this pass skips the
				// block above rather than creating again.
				ApplySchemaSync();
			},
			[this]()
			{
				// Creation failed (SendGame surfaced the error); drop the latch so the user can retry. The pending
				// plan is intact and still carries the notification; it just has no channel to reach anyone on yet.
				bAutoCreatingSessionChannel = false;
			});
		return;
	}

	// Build a per-walk op list ((generated operation name, variables)) from the CLOSED SET rather than from the five
	// pending arrays. The closed set is already in apply order (models first, because a new attribute needs its
	// model; then attributes, because a function names the keys it writes; then functions, automations and their
	// triggers), and each unit addresses exactly one pending upsert by kind and plan index, so the ops a whole
	// selection builds are element-for-element the ops the five arrays walked whole would have built. Authored fields
	// are sent EXPLICITLY (not omitted) so the server read-back matches and a follow-up plan is a genuine no-op.
	// Owning the list per walk means a stalled walk leaves the pending upserts intact for a clean re-click, with no
	// shared "already applying" latch. Every op in this walk is a Game Model mutation, so RunSchemaUpserts issues
	// them all against ECrowdyCppApiDomain::GameModel.
	const TSharedRef<TArray<TPair<FString, TSharedPtr<FJsonObject>>>> Ops =
		MakeShared<TArray<TPair<FString, TSharedPtr<FJsonObject>>>>();
	const TSharedRef<TArray<FCrowdyApplyUnit>> Sent = MakeShared<TArray<FCrowdyApplyUnit>>();

	for (const FCrowdyApplyUnit& Unit : Selection.ClosedSet)
	{
		switch (Unit.Kind)
		{
		case ECrowdyApplyKind::Type:
		{
			if (!PendingSyncTypeUpserts.IsValidIndex(Unit.PlanIndex))
			{
				continue;
			}
			const FCrowdySchemaTypeUpsert& TypeUpsert = PendingSyncTypeUpserts[Unit.PlanIndex];
			const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
			SetBigIntField(Input, TEXT("appId"), PlannedSyncAppId);
			Input->SetStringField(TEXT("typeName"), TypeUpsert.Type.TypeName);
			Input->SetStringField(TEXT("displayName"), TypeUpsert.Type.DisplayName);
			Input->SetStringField(TEXT("instantiableBy"), TypeUpsert.Type.InstantiableBy);
			Input->SetStringField(TEXT("defaultPropertyVisibility"), TypeUpsert.Type.DefaultVisibility);

			const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
			Variables->SetObjectField(TEXT("input"), Input);
			Ops->Add(TPair<FString, TSharedPtr<FJsonObject>>(TEXT("GameModelUpsertContainerType"), Variables));
			break;
		}

		case ECrowdyApplyKind::Attribute:
		{
			if (!PendingSyncPropUpserts.IsValidIndex(Unit.PlanIndex))
			{
				continue;
			}
			const FCrowdySchemaPropUpsert& PropUpsert = PendingSyncPropUpserts[Unit.PlanIndex];
			const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
			SetBigIntField(Input, TEXT("appId"), PlannedSyncAppId);
			Input->SetStringField(TEXT("containerTypeName"), PropUpsert.ContainerTypeName);
			Input->SetStringField(TEXT("key"), PropUpsert.Prop.Key);
			Input->SetStringField(TEXT("valueType"), PropUpsert.Prop.ValueType);
			SetOptionalStringField(Input, TEXT("defaultValueJson"), PropUpsert.Prop.DefaultValueJson);
			Input->SetStringField(TEXT("visibility"), PropUpsert.Prop.Visibility);
			Input->SetStringField(TEXT("writable"), PropUpsert.Prop.Writable);

			const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
			Variables->SetObjectField(TEXT("input"), Input);
			Ops->Add(TPair<FString, TSharedPtr<FJsonObject>>(TEXT("GameModelUpsertPropertyDef"), Variables));
			break;
		}

		case ECrowdyApplyKind::Function:
		{
			if (!PendingSyncFunctionUpserts.IsValidIndex(Unit.PlanIndex))
			{
				continue;
			}
			const TSharedPtr<FJsonObject> Input = CrowdyGameModelMarshalling::BuildFunctionUpsertInput(
				PendingSyncFunctionUpserts[Unit.PlanIndex].Function, PlannedSyncAppId);
			const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
			Variables->SetObjectField(TEXT("input"), Input);
			Ops->Add(TPair<FString, TSharedPtr<FJsonObject>>(TEXT("GameModelUpsertFunction"), Variables));
			break;
		}

		case ECrowdyApplyKind::Automation:
		{
			if (!PendingSyncAutomationUpserts.IsValidIndex(Unit.PlanIndex))
			{
				continue;
			}
			const TSharedPtr<FJsonObject> Variables = CrowdyStudioGql::BuildAutomationUpsertVariables(
				PendingSyncAutomationUpserts[Unit.PlanIndex].Automation, PlannedSyncAppId);
			Ops->Add(TPair<FString, TSharedPtr<FJsonObject>>(TEXT("GameModelUpsertAutomation"), Variables));
			break;
		}

		case ECrowdyApplyKind::Trigger:
		{
			if (!PendingSyncTriggerUpserts.IsValidIndex(Unit.PlanIndex))
			{
				continue;
			}
			const TSharedPtr<FJsonObject> Variables = CrowdyStudioGql::BuildAutomationTriggerUpsertVariables(
				PendingSyncTriggerUpserts[Unit.PlanIndex].Trigger, PlannedSyncAppId);
			Ops->Add(TPair<FString, TSharedPtr<FJsonObject>>(TEXT("GameModelUpsertAutomationTrigger"), Variables));
			break;
		}

		default:
			continue;
		}

		// One entry per op, in the same order, so the completion can take exactly these out of the pending arrays.
		Sent->Add(Unit);
	}

	// Keys the user picked that this plan no longer holds are dropped rather than mapped to a neighbour, and the
	// count is said out loud: silently sending fewer changes than were ticked is the same defect as silently
	// sending more.
	const FString DroppedNote = Selection.DroppedKeys.Num() > 0
		? FString::Printf(TEXT(" %d of what you picked is no longer in this plan and was dropped."),
			Selection.DroppedKeys.Num())
		: FString();
	SetStatus(FString::Printf(TEXT("Applying %d schema change(s)...%s"), Ops->Num(), *DroppedNote), false);
	RunSchemaUpserts(Ops, Sent, 0, SchemaPlanGeneration);
}

bool FCrowdyStudioController::ClosedSetNeedsSessionChannel(const TArray<FCrowdyApplyUnit>& ClosedSet) const
{
	if (PlannedSessionChannelFunctionKeys.Num() == 0)
	{
		return false;
	}

	for (const FCrowdyApplyUnit& Unit : ClosedSet)
	{
		if (Unit.Kind != ECrowdyApplyKind::Function)
		{
			continue;
		}
		// The unit's scoped name is the upsert's, and the upsert's is the desired function's: both sides are
		// (container type, function name) spelled by FCrowdySchemaSync::ScopedNameKey, so nothing has to translate
		// between two conventions here.
		if (CrowdyModelSnapshotKeys::Contains(PlannedSessionChannelFunctionKeys,
			FCrowdySchemaSync::ScopedNameKey(Unit.OwningType, Unit.Name)))
		{
			return true;
		}
	}
	return false;
}

void FCrowdyStudioController::RemoveAppliedUpserts(const TArray<FCrowdyApplyUnit>& Sent)
{
	TArray<int32> TypeIndices;
	TArray<int32> PropIndices;
	TArray<int32> FunctionIndices;
	TArray<int32> AutomationIndices;
	TArray<int32> TriggerIndices;

	for (const FCrowdyApplyUnit& Unit : Sent)
	{
		switch (Unit.Kind)
		{
		case ECrowdyApplyKind::Type:       TypeIndices.Add(Unit.PlanIndex); break;
		case ECrowdyApplyKind::Attribute:  PropIndices.Add(Unit.PlanIndex); break;
		case ECrowdyApplyKind::Function:   FunctionIndices.Add(Unit.PlanIndex); break;
		case ECrowdyApplyKind::Automation: AutomationIndices.Add(Unit.PlanIndex); break;
		case ECrowdyApplyKind::Trigger:    TriggerIndices.Add(Unit.PlanIndex); break;
		default: break;
		}
	}

	// Highest index first. Removing a lower entry shifts every entry after it down by one, so an ascending walk
	// would delete the wrong upserts for every index after the first and leave applied ones behind.
	auto RemoveDescending = [](auto& Array, TArray<int32>& Indices)
	{
		Indices.Sort([](int32 Left, int32 Right) { return Left > Right; });
		for (const int32 Index : Indices)
		{
			if (Array.IsValidIndex(Index))
			{
				Array.RemoveAt(Index);
			}
		}
	};

	RemoveDescending(PendingSyncTypeUpserts, TypeIndices);
	RemoveDescending(PendingSyncPropUpserts, PropIndices);
	RemoveDescending(PendingSyncFunctionUpserts, FunctionIndices);
	RemoveDescending(PendingSyncAutomationUpserts, AutomationIndices);
	RemoveDescending(PendingSyncTriggerUpserts, TriggerIndices);
}

void FCrowdyStudioController::RestateSchemaReportFromPending()
{
	// Counted by the same function that counted them when the plan was built, over the arrays as they stand now, so
	// the panel cannot end up printing one set of numbers from a plan and another from a send. Hand-adjusting the
	// ten counters here would be a second statement of what a create is and what an update is.
	FCrowdySchemaDelta Remaining;
	Remaining.TypeUpserts = PendingSyncTypeUpserts;
	Remaining.PropUpserts = PendingSyncPropUpserts;
	Remaining.FunctionUpserts = PendingSyncFunctionUpserts;
	Remaining.AutomationUpserts = PendingSyncAutomationUpserts;
	Remaining.TriggerUpserts = PendingSyncTriggerUpserts;
	// The prune candidates are untouched by an apply, so they carry over exactly; recounting them from the pending
	// prune lists keeps the whole report one statement about one moment.
	Remaining.ServerOnlyTypes = PendingPruneTypes;
	Remaining.ServerOnlyProps = PendingPruneProps;
	Remaining.ServerOnlyFunctions = PendingPruneFunctions;
	Remaining.ServerOnlyAutomations = PendingPruneAutomations;

	// The plan's warnings are about the code and the server, not about what this walk sent, so they are carried
	// through rather than recomputed. bApplied and the banner are decided by the caller, which knows whether
	// anything is left.
	const bool bWasApplied = SchemaSyncReport.bApplied;
	const FString StatusNote = SchemaSyncReport.StatusNote;
	// Copied out first: the report is about to be replaced, and handing its own field to the call that replaces it
	// would leave the reader working out whether the argument outlives the assignment.
	const TArray<FString> Warnings = SchemaSyncReport.Warnings;

	SchemaSyncReport = FCrowdySchemaSync::BuildReport(Remaining, Warnings, bWasApplied);
	SchemaSyncReport.StatusNote = StatusNote;
}

void FCrowdyStudioController::RunSchemaUpserts(const TSharedRef<TArray<TPair<FString, TSharedPtr<FJsonObject>>>>& Ops,
	const TSharedRef<const TArray<FCrowdyApplyUnit>>& Sent, int32 Index, uint64 PlanGeneration)
{
	if (Index >= Ops->Num())
	{
		const int32 Applied = Ops->Num();
		bAutoCreatingSessionChannel = false; // the (possibly channel-auto-created) apply is complete

		// This walk wrote the schema every per-effect status was judged against, so those verdicts are now stale. They
		// have no other reader to correct them, and a stale "out of sync" is what the pre-play prompt fires on.
		if (Applied > 0)
		{
			CrowdyStudioSyncService::InvalidateAllCachedStatuses();
		}

		// The plan this walk was built from is gone: something re-planned (or the app was switched) while the ops
		// were sending, and the five pending arrays now hold a different diff. Every decision below addresses those
		// arrays by plan index, so taken against the new plan they would strike out entries this walk never sent,
		// and if the count happened to reach zero the pill would read Ready over a model that was never created.
		// The writes themselves stand, so the lists are re-read and the user is told what to do next.
		//
		// What was picked is left alone rather than reset to everything: the new plan holds its own entities, the
		// selection is by identity key so it re-resolves against them, and widening it here would send changes on the
		// strength of a walk that finished against something else.
		if (PlanGeneration != SchemaPlanGeneration)
		{
			SetStatus(FString::Printf(
				TEXT("Sent %d schema change(s), but the plan was recomputed while they were sending. Press Preview changes to see what is left."),
				Applied), false);
			FetchContainerTypes();
			FetchFunctions(FString());
			FetchAutomations();
			return;
		}

		// Exactly what this walk wrote, and nothing else. Clearing all five would erase the changes a partial send
		// deliberately left behind, and there is nothing left anywhere to say what they were.
		RemoveAppliedUpserts(*Sent);
		// The next press starts from everything again, so a narrowed send is never silently repeated.
		ClearSchemaApplySelection();
		// The counts and lines the panel prints describe what is still planned, so they follow the arrays down.
		RestateSchemaReportFromPending();

		const int32 Remaining = PendingSyncTypeUpserts.Num() + PendingSyncPropUpserts.Num()
			+ PendingSyncFunctionUpserts.Num() + PendingSyncAutomationUpserts.Num() + PendingSyncTriggerUpserts.Num();

		if (Remaining == 0)
		{
			SchemaSyncReport.bApplied = true;
			SchemaSyncReport.StatusNote.Empty(); // clear any prior partial-apply banner
			SetStatus(FString::Printf(TEXT("Applied %d schema change(s). Re-plan to confirm zero drift."), Applied), false);
		}
		else
		{
			// bApplied stays false. It is what the readiness pill reads as "no drift whatever the counts say", so
			// setting it here would paint an app with real, still-pending drift as in sync.
			SchemaSyncReport.StatusNote = FString::Printf(
				TEXT("Sent %d change(s). %d more are still planned and were not sent. Sync to Server again to pick from what is left."),
				Applied, Remaining);
			SetStatus(FString::Printf(TEXT("Sent %d schema change(s). %d more are still planned."), Applied, Remaining), false);
		}

		// The retained plan describes the schema as it stood BEFORE this walk wrote to it, so every entity it just
		// created is still listed there as one the server does not have and every entity it just updated as one the
		// server has not got. Left in place, the browser would go on saying "Not on server yet" and "Changed in code"
		// about the very things this apply wrote, which is a false statement the tool itself just made false. Drop it
		// UNCONDITIONALLY, including after a partial send: a verdict that is stale for what was written and correct
		// for what was not is one nobody can trust part of. With no plan the page says nothing at all about where a
		// row came from, which is the honest answer until the next one is run, and the freshness line asks for that
		// plan by name.
		if (PlannedSyncAppId != 0 && ModelSnapshotsByApp.Remove(PlannedSyncAppId) > 0)
		{
			OnModelSnapshotChanged.Broadcast();
		}

		FetchContainerTypes(); // refresh the studio game-model lists to the new schema
		FetchFunctions(FString());
		// The walk creates and updates automations and their triggers too, and the browsed automation list is read
		// independently of the plan, so it learns about them only from a read of its own.
		FetchAutomations();
		// Zero drift is not the same claim as a coherent model: the plan compares what is authored here against what
		// the server holds, and says nothing about a function calling one that was never written on either side.
		// Quiet, so a clean answer does not overwrite the apply's own count.
		RunGameModelLint(true);
		OnSchemaSyncReportChanged.Broadcast();
		return;
	}

	// A mid-sequence failure stops the walk here (already-applied upserts are committed and idempotent, and the
	// pending plan stays intact so re-clicking Apply retries cleanly). The OnFailure records how far the walk got
	// so the report panel shows "applied N of M" rather than the walk silently stalling on "Applying M...".
	const int32 Total = Ops->Num();
	const TPair<FString, TSharedPtr<FJsonObject>>& Op = (*Ops)[Index];
	SendGame(ECrowdyCppApiDomain::GameModel, *Op.Key, Op.Value,
		[this, Ops, Sent, Index, PlanGeneration](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			RunSchemaUpserts(Ops, Sent, Index + 1, PlanGeneration);
		},
		[this, Index, Total]()
		{
			bAutoCreatingSessionChannel = false; // this apply attempt is over (a fresh Apply starts a new walk)
			// A cancellation is this editor rebuilding its own client (e.g. an endpoint change), not the server
			// refusing anything, so it gets its own honest wording rather than being dressed up as a server error.
			SchemaSyncReport.StatusNote = bLastFailureWasCanceled
				? FString::Printf(
					TEXT("Applied %d of %d change(s), then the operation was cancelled. The applied changes are committed and idempotent; click Apply again to finish."),
					Index, Total)
				: FString::Printf(
					TEXT("Applied %d of %d change(s), then stopped on a server error (see the status message). The applied changes are committed and idempotent; fix the error and click Apply again to finish."),
					Index, Total);

			// A walk that stopped partway still WROTE everything before the stop. Those entities exist on the server
			// now, so the retained plan and the three browsed lists are judgements about a server that has moved, and
			// every row would keep rendering a verdict computed before the write. Same drop and same re-reads the
			// completion runs, for the same reason; a walk that wrote nothing has invalidated nothing.
			if (Index > 0)
			{
				CrowdyStudioSyncService::InvalidateAllCachedStatuses();
				if (PlannedSyncAppId != 0 && ModelSnapshotsByApp.Remove(PlannedSyncAppId) > 0)
				{
					OnModelSnapshotChanged.Broadcast();
				}
				FetchContainerTypes();
				FetchFunctions(FString());
				FetchAutomations();
			}

			OnSchemaSyncReportChanged.Broadcast();
		});
}

void FCrowdyStudioController::PruneServerOnlySchema()
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app to prune the schema."), true);
		return;
	}
	if (PendingPruneTypes.Num() + PendingPruneProps.Num() + PendingPruneFunctions.Num() + PendingPruneAutomations.Num() == 0)
	{
		SetStatus(TEXT("Nothing to prune. Run a plan first; only server-only types/props/functions/automations are prunable."), false);
		return;
	}
	// The prune candidates came from a plan pinned to one app; never delete from a different now-selected app.
	if (SelectedAppId != PlannedSyncAppId)
	{
		SetStatus(TEXT("This plan was computed for a different app. Re-plan for the current app before pruning."), true);
		return;
	}

	// Delete server-only PROPERTY DEFS first (on code-owned types), then server-only TYPES (each cascades its own
	// prop defs). A type still holding live containers / bound functions is REFUSED server-side and arrives as a
	// GraphQL error (surfaced by SendGame), not a silent success.
	const TSharedRef<TArray<TPair<FString, TSharedPtr<FJsonObject>>>> Ops =
		MakeShared<TArray<TPair<FString, TSharedPtr<FJsonObject>>>>();

	// Automations first: an automation references a function (deleting the function first would be refused while the
	// automation still points at it), and gameModelDeleteAutomation also removes the automation's event triggers.
	for (const FString& AutomationName : PendingPruneAutomations)
	{
		const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
		SetBigIntField(Variables, TEXT("appId"), PlannedSyncAppId);
		Variables->SetStringField(TEXT("name"), AutomationName);
		Ops->Add(TPair<FString, TSharedPtr<FJsonObject>>(TEXT("GameModelDeleteAutomation"), Variables));
	}

	for (const FCrowdySchemaPropRef& Prop : PendingPruneProps)
	{
		const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
		SetBigIntField(Variables, TEXT("appId"), PlannedSyncAppId);
		Variables->SetStringField(TEXT("containerTypeName"), Prop.ContainerTypeName);
		Variables->SetStringField(TEXT("key"), Prop.Key);
		Ops->Add(TPair<FString, TSharedPtr<FJsonObject>>(TEXT("GameModelDeletePropertyDef"), Variables));
	}
	// Functions before types: a container type that still has a bound function is refused server-side, so a
	// server-only function bound to a server-only type must be deleted first.
	//
	// The candidate carries the container type the diff identified it by. Only the name goes on the wire, because
	// that is all gameModelDeleteFunction declares; the type is carried so nothing between the plan and here has to
	// guess which model a bare name came from, and a candidate whose type the read never gave stays visibly
	// undetermined instead of reading as one that no model owns.
	for (const FCrowdySchemaFunctionRef& Function : PendingPruneFunctions)
	{
		const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
		SetBigIntField(Variables, TEXT("appId"), PlannedSyncAppId);
		Variables->SetStringField(TEXT("name"), Function.Name);
		Ops->Add(TPair<FString, TSharedPtr<FJsonObject>>(TEXT("GameModelDeleteFunction"), Variables));
	}
	for (const FString& TypeName : PendingPruneTypes)
	{
		const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
		SetBigIntField(Variables, TEXT("appId"), PlannedSyncAppId);
		Variables->SetStringField(TEXT("typeName"), TypeName);
		Ops->Add(TPair<FString, TSharedPtr<FJsonObject>>(TEXT("GameModelDeleteContainerType"), Variables));
	}

	SetStatus(FString::Printf(TEXT("Pruning %d server-only entity(s)..."), Ops->Num()), false);
	RunSchemaPrune(Ops, 0);
}

void FCrowdyStudioController::RunSchemaPrune(const TSharedRef<TArray<TPair<FString, TSharedPtr<FJsonObject>>>>& Ops, int32 Index)
{
	if (Index >= Ops->Num())
	{
		const int32 Pruned = Ops->Num();
		if (Pruned > 0)
		{
			// A delete moves the server schema the per-effect statuses were judged against, so they are stale too.
			CrowdyStudioSyncService::InvalidateAllCachedStatuses();
		}
		PendingPruneTypes.Reset();
		PendingPruneProps.Reset();
		PendingPruneFunctions.Reset();
		PendingPruneAutomations.Reset();
		SetStatus(FString::Printf(TEXT("Pruned %d server-only entity(s). Re-plan to confirm."), Pruned), false);
		FetchContainerTypes(); // refresh the studio game-model lists to the pruned schema
		FetchFunctions(FString());
		// A prune destroys automations as well, and without this the browsed list keeps listing entities that no
		// longer exist right after the one irreversible operation on this page.
		FetchAutomations();
		OnSchemaSyncReportChanged.Broadcast();
		return;
	}

	// A refused/failed delete (e.g. a type that still has live containers) stops this walk there; the entities
	// deleted so far are gone (idempotent), and the pending prune list stays for a clean re-click after the blocker
	// is cleared. SendGame surfaces the refusal error; the OnFailure adds how far the walk got so it is not silent.
	const int32 Total = Ops->Num();
	const TPair<FString, TSharedPtr<FJsonObject>>& Op = (*Ops)[Index];
	SendGame(ECrowdyCppApiDomain::GameModel, *Op.Key, Op.Value,
		[this, Ops, Index](const TSharedPtr<FJsonObject>& /*Envelope*/)
		{
			RunSchemaPrune(Ops, Index + 1);
		},
		[this, Index, Total]()
		{
			// A cancellation is this editor rebuilding its own client (e.g. an endpoint change), not the server
			// refusing the delete, so it gets its own honest wording instead of sending the user hunting for a
			// blocker that was never there.
			const FString Note = bLastFailureWasCanceled
				? FString::Printf(
					TEXT("Pruned %d of %d entity(s), then the operation was cancelled. Re-click Remove Server-Only to finish."),
					Index, Total)
				: FString::Printf(
					TEXT("Pruned %d of %d entity(s), then stopped (a type with live containers or bound functions is refused server-side). Clear the blocker and re-click Remove Server-Only to finish."),
					Index, Total);
			SetStatus(Note, !bLastFailureWasCanceled);
			// A walk that stopped partway still deleted everything before the stop, so the per-effect verdicts computed
			// against the pre-delete schema are stale; a walk that deleted nothing has invalidated nothing.
			if (Index > 0)
			{
				CrowdyStudioSyncService::InvalidateAllCachedStatuses();
			}
			// Mirror the partial-apply path: leave a durable banner in the report panel, not just the ephemeral toast.
			SchemaSyncReport.StatusNote = Note;
			OnSchemaSyncReportChanged.Broadcast();
		});
}

void FCrowdyStudioController::CountLiveModelsScoped(const TArray<FString>& TypeNames, int32 PerTypeLimit,
	TFunction<void(int64, TArray<FCrowdyDeleteLiveCount>&&)> OnDone)
{
	if (!OnDone)
	{
		// No completion means nothing is waiting on the answer, and the reads exist only to produce one.
		return;
	}

	// One probe at a time. A second set of reads issued over the first would share the one pending list, and the
	// first caller's completion would then fire on the second caller's counts. Answering the newcomer immediately
	// with nothing is honest: every model it asked about comes back Unknown, which is a blocker rather than a
	// green light, and it clears by asking again once the outstanding probe has settled.
	if (LiveCountCompletion)
	{
		TArray<FCrowdyDeleteLiveCount> Unread;
		for (const FString& TypeName : TypeNames)
		{
			Unread.Add(FCrowdyDeleteLiveCount{ TypeName, ECrowdyLiveCountState::Unknown, 0, {} });
		}
		OnDone(SelectedAppId, MoveTemp(Unread));
		return;
	}

	LiveCountAppId = SelectedAppId;
	++LiveCountSerial;
	LiveCountResults.Reset();
	LiveCountPending.Reset();
	LiveCountCompletion = MoveTemp(OnDone);

	if (LiveCountAppId != 0)
	{
		for (const FString& TypeName : TypeNames)
		{
			// Server keys, so the duplicate check is case-sensitive: two model names differing only in case are two
			// models, and folding them would leave one of them never probed and silently Unknown.
			const bool bAlreadyQueued = LiveCountPending.ContainsByPredicate(
				[&TypeName](const FString& Queued) { return Queued.Equals(TypeName, ESearchCase::CaseSensitive); });
			if (!TypeName.IsEmpty() && !bAlreadyQueued)
			{
				LiveCountPending.Add(TypeName);
			}
		}
	}

	if (LiveCountPending.Num() == 0)
	{
		// No app selected, or nothing worth reading. The completion still runs, and every model it asked about is
		// reported Unknown.
		FinishLiveModelCount();
		return;
	}

	// Iterated over a copy: a read can fail inside the call that issues it (no game endpoint, an org-token sign-in
	// that cannot mint), which finishes the probe and empties the pending list under the loop.
	const TArray<FString> Queued = LiveCountPending;
	for (const FString& TypeName : Queued)
	{
		if (!LiveCountCompletion)
		{
			break;
		}
		ReadLiveModelCountFor(TypeName, PerTypeLimit);
	}
}

void FCrowdyStudioController::ReadLiveModelCountFor(const FString& TypeName, int32 PerTypeLimit)
{
	const int64 ReadForAppId = LiveCountAppId;
	const uint64 ReadForSerial = LiveCountSerial;

	// Nothing here writes the shared container list, the filters or the paging state. The list is filtered by
	// whatever was last typed into the Live tab's boxes, so a count taken from it would report zero for a model
	// with hundreds of instances, and a delete refusal nobody was warned about would read as a green light.
	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), ReadForAppId);
	Variables->SetStringField(TEXT("typeName"), TypeName);
	Variables->SetField(TEXT("sessionId"), MakeShared<FJsonValueNull>());
	if (PerTypeLimit > 0)
	{
		Variables->SetNumberField(TEXT("limit"), PerTypeLimit);
	}

	SendGame(ECrowdyCppApiDomain::GameModel, TEXT("GameModelContainers"), Variables,
		[this, TypeName, PerTypeLimit, ReadForAppId, ReadForSerial](const TSharedPtr<FJsonObject>& Envelope)
		{
			if (!LiveCountCompletion || LiveCountAppId != ReadForAppId || LiveCountSerial != ReadForSerial)
			{
				return; // this probe has already been answered, or a later one replaced it
			}

			TArray<TSharedPtr<FStudioContainer>> Page;
			CrowdyStudioGql::ParseContainers(Envelope, TEXT("gameModelContainers"), Page);

			FCrowdyDeleteLiveCount Count;
			Count.TypeName = TypeName;
			Count.Count = Page.Num();
			// A read that came back full stopped at its limit, so the count is a floor rather than a total and has
			// to say so; anything short of the limit is the whole set.
			Count.State = (PerTypeLimit > 0 && Page.Num() >= PerTypeLimit)
				? ECrowdyLiveCountState::AtLeast
				: ECrowdyLiveCountState::Exact;
			for (const TSharedPtr<FStudioContainer>& Container : Page)
			{
				if (Count.SampleIds.Num() >= CrowdyDeleteMaxDisclosureEntries)
				{
					break;
				}
				if (Container.IsValid())
				{
					Count.SampleIds.Add(Container->ContainerId);
				}
			}
			LiveCountResults.Add(MoveTemp(Count));

			LiveCountPending.RemoveAll([&TypeName](const FString& Pending)
				{ return Pending.Equals(TypeName, ESearchCase::CaseSensitive); });
			if (LiveCountPending.Num() == 0)
			{
				FinishLiveModelCount();
			}
		},
		[this, TypeName, ReadForAppId, ReadForSerial]()
		{
			if (!LiveCountCompletion || LiveCountAppId != ReadForAppId || LiveCountSerial != ReadForSerial)
			{
				return;
			}
			// No result recorded, so this model is reported Unknown. A failed read says nothing about how many live
			// models there are, and reporting none would be the one answer that lets a refused delete look safe.
			LiveCountPending.RemoveAll([&TypeName](const FString& Pending)
				{ return Pending.Equals(TypeName, ESearchCase::CaseSensitive); });
			if (LiveCountPending.Num() == 0)
			{
				FinishLiveModelCount();
			}
		});
}

void FCrowdyStudioController::FinishLiveModelCount()
{
	if (!LiveCountCompletion)
	{
		return; // already answered; every path that can end a probe reaches here, and only the first one may
	}

	// Whatever never landed is reported Unknown by name rather than left out, so a caller that walks the results
	// sees the models it asked about and cannot read an absent entry as a count of zero.
	for (const FString& TypeName : LiveCountPending)
	{
		LiveCountResults.Add(FCrowdyDeleteLiveCount{ TypeName, ECrowdyLiveCountState::Unknown, 0, {} });
	}

	const int64 AnsweredForAppId = LiveCountAppId;
	TArray<FCrowdyDeleteLiveCount> Counts = MoveTemp(LiveCountResults);
	TFunction<void(int64, TArray<FCrowdyDeleteLiveCount>&&)> Completion = MoveTemp(LiveCountCompletion);

	// Cleared before the completion runs: it may start another probe, and that one must not be torn down by the
	// state teardown of the probe it was started from.
	LiveCountResults.Reset();
	LiveCountPending.Reset();
	LiveCountCompletion = nullptr;
	LiveCountAppId = 0;

	Completion(AnsweredForAppId, MoveTemp(Counts));
}

bool FCrowdyStudioController::IsDeleteCommitInFlight() const
{
	return DeleteCommitAppId != 0 && DeleteCommitAppId == SelectedAppId;
}

bool FCrowdyStudioController::IsContainerPurgeInFlight() const
{
	return ContainerPurgeAppId != 0 && ContainerPurgeAppId == SelectedAppId;
}

void FCrowdyStudioController::CancelContainerPurge()
{
	// Latched rather than acted on: a delete is in flight and stopping it here would leave the walk to run on past
	// a purge that had already announced itself finished. Only ever set, never cleared, or a second press with
	// nothing pinned would withdraw the first one's request.
	if (ContainerPurgeAppId != 0)
	{
		bContainerPurgeCancelRequested = true;
	}
}

void FCrowdyStudioController::PurgeContainers(const FString& TypeName, int64 ExpectedAppId)
{
	auto Refuse = [this](const FString& Reason)
	{
		// Stopped, not a clean sweep of nothing: the default outcome renders as "Deleted 0 live models. Nothing is
		// left to read.", which is the one sentence a refusal must never produce.
		LastContainerPurgeOutcome = FCrowdyDeleteOutcome();
		LastContainerPurgeOutcome.bStopped = true;
		SetStatus(Reason, true);
		OnContainerPurgeFinished.Broadcast();
	};

	if (ContainerPurgeAppId != 0)
	{
		// A purge already running owns this state, and announcing a refusal would tell the tab THAT purge had ended.
		SetStatus(TEXT("A live-model delete is already running."), true);
		return;
	}
	if (SelectedAppId == 0)
	{
		Refuse(TEXT("Select an app first."));
		return;
	}
	if (SelectedAppId != ExpectedAppId)
	{
		Refuse(TEXT("These live models were listed for a different app. Reload this app's live models first."));
		return;
	}

	ContainerPurgeAppId = SelectedAppId;
	++ContainerPurgeSerial;
	ContainerPurgeTypeName = TypeName;
	ContainerPurgePageIds.Reset();
	ContainerPurgeCompleted = 0;
	ContainerPurgeAlreadyGone = 0;
	ContainerPurgePageRemoved = 0;
	ContainerPurgePasses = 0;
	bContainerPurgeCancelRequested = false;
	LastContainerPurgeOutcome = FCrowdyDeleteOutcome();

	SetStatus(TypeName.IsEmpty()
		? FString(TEXT("Deleting every live model in this app..."))
		: FString::Printf(TEXT("Deleting every live model of %s..."), *TypeName), false);

	OnContainerPurgeProgress.Broadcast();
	ReadContainerPurgePage();
}

void FCrowdyStudioController::ReadContainerPurgePage()
{
	const int64 ReadForAppId = ContainerPurgeAppId;
	const uint64 ReadForSerial = ContainerPurgeSerial;

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), ReadForAppId);
	if (ContainerPurgeTypeName.IsEmpty())
	{
		Variables->SetField(TEXT("typeName"), MakeShared<FJsonValueNull>());
	}
	else
	{
		Variables->SetStringField(TEXT("typeName"), ContainerPurgeTypeName);
	}
	// No session filter: a purge that skipped session-owned live models would report the app cleared while they
	// stood, and they are exactly what a model delete is then refused over.
	Variables->SetField(TEXT("sessionId"), MakeShared<FJsonValueNull>());
	Variables->SetNumberField(TEXT("limit"), ContainerPurgePageSize);
	// Always offset zero. Each delete shifts the rest of the ordering down, so a window that advanced would step
	// over exactly as many live models as it had just deleted.
	Variables->SetNumberField(TEXT("offset"), 0);

	SendGame(ECrowdyCppApiDomain::GameModel, TEXT("GameModelContainers"), Variables,
		[this, ReadForAppId, ReadForSerial](const TSharedPtr<FJsonObject>& Envelope)
		{
			if (ContainerPurgeAppId != ReadForAppId || ContainerPurgeSerial != ReadForSerial)
			{
				return;
			}

			// Checked here as well as in the walk. A Stop pressed while this read was outstanding would otherwise
			// be discarded by an empty page, which reports the run as a clean sweep the reader never let finish.
			if (bContainerPurgeCancelRequested)
			{
				FinishContainerPurge(true, /*bByCancel*/ true, FString());
				return;
			}

			TArray<TSharedPtr<FStudioContainer>> Page;
			CrowdyStudioGql::ParseContainers(Envelope, TEXT("gameModelContainers"), Page);

			TArray<FString> Kept;
			for (const TSharedPtr<FStudioContainer>& Container : Page)
			{
				if (Kept.Num() >= ContainerPurgePageSize)
				{
					break; // the limit was asked for, never trusted: a longer reply is not licence to delete more
				}
				if (!Container.IsValid() || Container->ContainerId.IsEmpty())
				{
					continue;
				}
				// The type is a request argument the server may not have honoured, and the confirm named one model.
				// Re-checking each row is what the runtime's own container read already does with bindingKey.
				if (!ContainerPurgeTypeName.IsEmpty()
					&& !Container->TypeName.Equals(ContainerPurgeTypeName, ESearchCase::CaseSensitive))
				{
					continue;
				}
				Kept.Add(Container->ContainerId);
			}

			// A page the server filled but this could take nothing from is not an empty app. Reporting it as one is
			// the single answer that lets a half-drained app read as cleared.
			if (Kept.Num() == 0 && Page.Num() > 0)
			{
				FinishContainerPurge(true, false, TEXT("live models this could not identify or match"));
				return;
			}

			// Nothing came back at all, which is the only evidence a drain has that it is over.
			if (Kept.Num() == 0)
			{
				FinishContainerPurge(/*bStopped*/ false, /*bByCancel*/ false, FString());
				return;
			}

			// A page identical to the one just drained means the deletes are not sticking, whatever they answered.
			// Container ids are opaque, and both FString::operator== and TArray equality fold case, so this is
			// spelled out: two ids differing only in case are two live models.
			const bool bSamePageAgain = ContainerPurgePageIds.Num() == Kept.Num()
				&& !ContainerPurgePageIds.IsEmpty()
				&& [&]()
				{
					for (int32 Index = 0; Index < Kept.Num(); ++Index)
					{
						if (!ContainerPurgePageIds[Index].Equals(Kept[Index], ESearchCase::CaseSensitive))
						{
							return false;
						}
					}
					return true;
				}();
			if (bSamePageAgain)
			{
				FinishContainerPurge(true, false, TEXT("live models that came straight back"));
				return;
			}

			// The drain is unbounded by design, so it needs one bound that is not the server's cooperation. A live
			// app recreates binding-key containers under NEW ids as fast as this deletes them, which no comparison
			// of one page against the last can see, and the pass count is the only thing that rises in that case.
			if (++ContainerPurgePasses > ContainerPurgeMaxPasses)
			{
				FinishContainerPurge(true, false, TEXT("live models that kept arriving faster than this could clear them"));
				return;
			}

			ContainerPurgePageIds = MoveTemp(Kept);
			ContainerPurgePageRemoved = 0;
			RunContainerPurgeWalk(0);
		},
		[this, ReadForAppId, ReadForSerial]()
		{
			if (ContainerPurgeAppId != ReadForAppId || ContainerPurgeSerial != ReadForSerial)
			{
				return;
			}
			// A failed read says nothing about what is left, and reporting none would be the one answer that lets a
			// half-drained app read as cleared. bLastFailureWasCanceled is only meaningful inside this call: a
			// client torn down here refused nothing, and blaming the server sends the reader hunting for a cause.
			FinishContainerPurge(true, bLastFailureWasCanceled, TEXT("a read of what is left"));
		});
}

void FCrowdyStudioController::RunContainerPurgeWalk(int32 Index)
{
	if (bContainerPurgeCancelRequested)
	{
		FinishContainerPurge(true, /*bByCancel*/ true, FString());
		return;
	}

	if (!ContainerPurgePageIds.IsValidIndex(Index))
	{
		if (ContainerPurgePageRemoved == 0)
		{
			FinishContainerPurge(true, false, TEXT("live models the server kept listing but did not remove"));
			return;
		}
		ReadContainerPurgePage();
		return;
	}

	const int64 RunForAppId = ContainerPurgeAppId;
	const uint64 RunForSerial = ContainerPurgeSerial;
	const FString ContainerId = ContainerPurgePageIds[Index];

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), RunForAppId);
	Variables->SetStringField(TEXT("containerId"), ContainerId);

	SendGame(ECrowdyCppApiDomain::GameModel, TEXT("GameModelDeleteContainer"), Variables,
		[this, Index, ContainerId, RunForAppId, RunForSerial](const TSharedPtr<FJsonObject>& Envelope)
		{
			if (ContainerPurgeAppId != RunForAppId || ContainerPurgeSerial != RunForSerial)
			{
				return;
			}
			HandleContainerPurgeReply(Index, ContainerId, Envelope);
		},
		[this, ContainerId, RunForAppId, RunForSerial]()
		{
			if (ContainerPurgeAppId != RunForAppId || ContainerPurgeSerial != RunForSerial)
			{
				return;
			}
			// A cancellation names no live model, because none of them refused anything.
			FinishContainerPurge(true, bLastFailureWasCanceled,
				bLastFailureWasCanceled ? FString() : FString::Printf(TEXT("the live model %s"), *ContainerId));
		});
}

// ContainerId by value: the walk this ends can clear the array a caller may have taken it from.
void FCrowdyStudioController::HandleContainerPurgeReply(int32 Index, FString ContainerId,
	const TSharedPtr<FJsonObject>& Envelope)
{
	// A false reply means the live model was not there, which is an idempotent no-op and counts as success. Only a
	// reply whose shape carries no answer at all stops the drain.
	const ECrowdyDeleteReply Reply =
		CrowdyGameModelDelete::ReadDeleteReply(Envelope, TEXT("gameModelDeleteContainer"));
	if (!CrowdyGameModelDelete::IsDeleteReplySuccess(Reply))
	{
		FinishContainerPurge(true, false, FString::Printf(TEXT("the live model %s"), *ContainerId));
		return;
	}

	++ContainerPurgeCompleted;
	if (Reply == ECrowdyDeleteReply::AlreadyGone)
	{
		++ContainerPurgeAlreadyGone;
	}
	else
	{
		++ContainerPurgePageRemoved;
	}

	OnContainerPurgeProgress.Broadcast();
	RunContainerPurgeWalk(Index + 1);
}

void FCrowdyStudioController::FinishContainerPurge(bool bStopped, bool bByCancel, const FString& StoppedOn)
{
	if (ContainerPurgeAppId == 0)
	{
		return; // already ended; every path that can end a purge reaches here and only the first one may
	}

	FCrowdyDeleteOutcome Outcome;
	Outcome.AppId = ContainerPurgeAppId;
	Outcome.Completed = ContainerPurgeCompleted;
	Outcome.AlreadyGone = ContainerPurgeAlreadyGone;
	// Left at zero deliberately. Nothing here ever knew how many live models the app held, and Total is the field
	// StopText and CompletionText render as "N of M"; a purge must never reach a formatter that can say that.
	Outcome.bStopped = bStopped;
	Outcome.bStoppedByCancel = bByCancel;
	Outcome.StoppedOnDescription = StoppedOn;

	const int64 PurgedAppId = ContainerPurgeAppId;

	ContainerPurgeAppId = 0;
	ContainerPurgeTypeName.Reset();
	ContainerPurgePageIds.Reset();
	ContainerPurgeCompleted = 0;
	ContainerPurgeAlreadyGone = 0;
	ContainerPurgePageRemoved = 0;
	ContainerPurgePasses = 0;
	bContainerPurgeCancelRequested = false;
	LastContainerPurgeOutcome = Outcome;

	SetStatus(CrowdyGameModelDelete::PurgeText(Outcome), bStopped && !bByCancel);

	// Only when a paged read has actually happened. With no limit recorded, ReadContainers omits it entirely and
	// re-reads every live model in the app unpaged, which is the cost this tab's paging exists to avoid.
	if (Outcome.Completed > 0 && SelectedAppId == PurgedAppId && LastContainerLimit > 0)
	{
		// Re-read exactly the window the Live tab has on screen. Keep, because a shorter answer here is the purge
		// working rather than evidence that the list is over.
		ReadContainers(LastContainerTypeFilter, LastContainerSessionFilter, LastContainerOffset + LastContainerLimit,
			0, /*bAppend*/ false, EContainerPageEvidence::Keep);
	}

	if (Outcome.Completed > 0)
	{
		CrowdyStudioSyncService::InvalidateAllCachedStatuses();
	}

	OnContainerPurgeProgress.Broadcast();
	OnContainerPurgeFinished.Broadcast();
}

void FCrowdyStudioController::CommitDeletePlan(const FCrowdyDeletePlan& Plan, int64 ExpectedAppId)
{
	// A walk already running owns the delete state. Announcing a refusal here would tell the review that THAT walk
	// had ended, so this one path says its piece in the status line and stays silent on the delegate; the running
	// walk's own finish is the signal the review is waiting for.
	if (DeleteCommitAppId != 0)
	{
		SetStatus(TEXT("A delete is already running. Wait for it to finish before starting another."), true);
		return;
	}

	// Every other refusal announces. A caller continues from the finished signal, so one that never arrives leaves
	// the page reporting a delete that is under way forever.
	auto Refuse = [this](const FString& Reason)
	{
		LastDeleteOutcome = FCrowdyDeleteOutcome();
		DeleteRemainder.Reset();
		SetStatus(Reason, true);
		OnDeleteCommitFinished.Broadcast();
	};

	if (SelectedAppId == 0)
	{
		Refuse(TEXT("Select an app before deleting anything."));
		return;
	}
	// Two comparisons against two sources. The plan carries the app its evidence was gathered for and ExpectedAppId
	// is the app the widget that drew the sheet believes it is showing; comparing one of those to itself would agree
	// no matter how stale the sheet had become.
	if (Plan.AppId != SelectedAppId || ExpectedAppId != SelectedAppId)
	{
		Refuse(TEXT("This delete was prepared for a different app. Reopen the review for the current app before deleting."));
		return;
	}
	// Checked here as well as in the sheet, so the rule that a blocker cannot be committed holds away from the
	// widget that happens to render it.
	if (!Plan.bCommittable)
	{
		Refuse(TEXT("This delete still has something blocking it. Clear the blocker or change what is marked, then try again."));
		return;
	}
	if (Plan.Ops.Num() == 0)
	{
		Refuse(TEXT("Nothing is marked for deletion."));
		return;
	}

	DeleteCommitAppId = SelectedAppId;
	DeleteCommitCompleted = 0;
	DeleteCommitAlreadyGone = 0;
	DeleteCommitTotal = Plan.Ops.Num();
	DeleteRemainder.Reset();
	LastDeleteOutcome = FCrowdyDeleteOutcome();

	// The walk owns its own copy of the ops, so nothing the review does to its plan afterwards can change what is
	// running, how far it got, or what the remainder turns out to be.
	DeleteWalkOps = MakeShared<TArray<FCrowdyDeleteOp>>(Plan.Ops);

	SetStatus(FString::Printf(TEXT("Deleting %d entry(s)..."), DeleteCommitTotal), false);
	OnDeleteCommitProgress.Broadcast();
	RunDeleteWalk(DeleteWalkOps.ToSharedRef(), 0);
}

void FCrowdyStudioController::RunDeleteWalk(const TSharedRef<TArray<FCrowdyDeleteOp>>& Ops, int32 Index)
{
	if (DeleteWalkOps.Get() != &Ops.Get())
	{
		return; // this walk has already ended, or another has replaced it
	}
	if (Index >= Ops->Num())
	{
		FinishDeleteWalk(INDEX_NONE, /*bWasCanceled*/ false);
		return;
	}

	const FCrowdyDeleteOp& Op = (*Ops)[Index];

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	// Every server id is a BigInt and goes out as a JSON string, which is the one argument an op cannot carry among
	// its plain string arguments and therefore the one that cannot be set the wrong way here.
	SetBigIntField(Variables, TEXT("appId"), DeleteCommitAppId);
	for (const TPair<FString, FString>& Arg : Op.StringArgs)
	{
		Variables->SetStringField(Arg.Key, Arg.Value);
	}

	SendGame(ECrowdyCppApiDomain::GameModel, *Op.OperationName, Variables,
		[this, Ops, Index](const TSharedPtr<FJsonObject>& Envelope)
		{
			HandleDeleteOpReply(Ops, Index, Envelope);
		},
		[this, Ops, Index]()
		{
			// A cancellation is this editor rebuilding its own client, not the server refusing the delete, so the two
			// are told apart here and worded differently rather than sending a reader hunting for a blocker that was
			// never there. bLastFailureWasCanceled is only meaningful inside this call.
			HandleDeleteOpFailure(Ops, Index, bLastFailureWasCanceled);
		});
}

void FCrowdyStudioController::HandleDeleteOpReply(const TSharedRef<TArray<FCrowdyDeleteOp>>& Ops, int32 Index,
	const TSharedPtr<FJsonObject>& Envelope)
{
	if (DeleteWalkOps.Get() != &Ops.Get() || !Ops->IsValidIndex(Index))
	{
		return; // this walk has already ended, or another has replaced it
	}

	const ECrowdyDeleteReply Reply = CrowdyGameModelDelete::ReadDeleteReply(Envelope, (*Ops)[Index].ResultField);
	if (!CrowdyGameModelDelete::IsDeleteReplySuccess(Reply))
	{
		// A clean envelope carrying no answer under the field the operation declares says nothing about whether the
		// entity survived. Refusals arrive on the failure path, so nothing legitimate lands here, and continuing
		// would report a delete that may not have happened. Ending the walk names the op it stopped on, so there is
		// nothing further to say here.
		FinishDeleteWalk(Index, /*bWasCanceled*/ false);
		return;
	}

	++DeleteCommitCompleted;
	if (Reply == ECrowdyDeleteReply::AlreadyGone)
	{
		// The entity was not there. An idempotent no-op, and a success: this is exactly what a second press after a
		// partial commit sees on everything the first attempt already finished.
		++DeleteCommitAlreadyGone;
	}
	OnDeleteCommitProgress.Broadcast();

	// The next op runs only once this one has settled. A later op can depend on this one (a model delete is refused
	// while a function is still bound to it), so the walk is sequential by requirement, not by convenience.
	RunDeleteWalk(Ops, Index + 1);
}

void FCrowdyStudioController::HandleDeleteOpFailure(
	const TSharedRef<TArray<FCrowdyDeleteOp>>& Ops, int32 Index, bool bWasCanceled)
{
	if (DeleteWalkOps.Get() != &Ops.Get())
	{
		return;
	}
	FinishDeleteWalk(Index, bWasCanceled);
}

void FCrowdyStudioController::FinishDeleteWalk(int32 StoppedAtIndex, bool bWasCanceled)
{
	if (!DeleteWalkOps.IsValid())
	{
		return; // already ended; every path that can end a walk reaches here, and only the first one may
	}

	const TArray<FCrowdyDeleteOp> Ops = *DeleteWalkOps;
	const int64 WalkAppId = DeleteCommitAppId;

	FCrowdyDeleteOutcome Outcome;
	Outcome.AppId = WalkAppId;
	Outcome.Total = DeleteCommitTotal;
	Outcome.Completed = DeleteCommitCompleted;
	Outcome.AlreadyGone = DeleteCommitAlreadyGone;
	Outcome.bStopped = StoppedAtIndex != INDEX_NONE;
	Outcome.StoppedAtIndex = StoppedAtIndex;
	Outcome.bStoppedByCancel = Outcome.bStopped && bWasCanceled;
	if (Ops.IsValidIndex(StoppedAtIndex))
	{
		Outcome.StoppedOnDescription = Ops[StoppedAtIndex].Describe;
	}

	LastDeleteOutcome = Outcome;
	// The op at the stop index did not complete, so the remainder starts with it and a second press finishes exactly
	// what is left rather than starting over.
	DeleteRemainder = CrowdyGameModelDelete::Remainder(Ops, StoppedAtIndex);

	// Cleared before anything is announced: a listener may start the remaining deletes straight from the finished
	// signal, and that attempt must not be refused by the walk it is being told has just ended.
	DeleteWalkOps.Reset();
	DeleteCommitAppId = 0;
	DeleteCommitCompleted = 0;
	DeleteCommitTotal = 0;
	DeleteCommitAlreadyGone = 0;

	SetStatus(Outcome.bStopped ? CrowdyGameModelDelete::StopText(Outcome) : CrowdyGameModelDelete::CompletionText(Outcome),
		Outcome.bStopped && !Outcome.bStoppedByCancel);

	// A commit that deleted anything moved the server schema every per-effect status was judged against, so those
	// verdicts are stale. Nothing else re-reads them, so a stale "out of sync" would keep re-raising the pre-play prompt.
	if (Outcome.Completed > 0)
	{
		CrowdyStudioSyncService::InvalidateAllCachedStatuses();
	}

	// A walk that stopped still wrote to the server, so the lists are refreshed either way. They are not refreshed
	// when the app has moved on: the switch emptied them already, and re-reading here would fill the new app's page
	// from a walk that belonged to the previous one.
	if (WalkAppId != 0 && WalkAppId == SelectedAppId)
	{
		RefreshAfterDeleteCommit();
	}

	OnDeleteCommitProgress.Broadcast();
	OnDeleteCommitFinished.Broadcast();
}

void FCrowdyStudioController::RefreshAfterDeleteCommit()
{
	// A commit can remove models, attributes, functions and automations, and the browser renders all four from these
	// lists, so without this it goes on listing entities that no longer exist right after the one operation on this
	// page that cannot be undone.
	FetchContainerTypes();
	FetchFunctions(FString());
	FetchAutomations();

	// The per-model attribute cache is dropped whole rather than edited: a commit can have emptied any part of it,
	// and a cached list is indistinguishable from a fresh one once it is in the map. The in-flight keys and serials
	// go with it, so a read issued BEFORE the deletes cannot land afterwards and put a deleted attribute back.
	const FString OpenModel = PropertyDefsMirrorType;
	PropertyDefsByType.Reset();
	PropertyDefFetchesInFlight.Reset();
	PropertyDefLatestRequest.Reset();
	PropertyDefs.Reset();
	PropertyDefsMirrorType.Reset();
	OnPropertyDefsCached.Broadcast();
	OnPropertyDefsChanged.Broadcast();

	// Exactly one model is re-read: the one whose attributes are on screen. A view asks for a model's attributes
	// once and caches them, so dropping the cache alone would leave the open model showing nothing until it was
	// selected again; re-reading every cached model instead would be one round trip per model the session ever
	// opened, for a delete that may have touched none of them.
	if (!OpenModel.IsEmpty())
	{
		FetchPropertyDefs(OpenModel);
	}

	// Live models are re-listed only when some are on screen. With nothing read there is nothing to correct, and an
	// unpaged read of every live model in the app is not something a delete that touched none of them should cost.
	if (Containers.Num() > 0)
	{
		// Re-read exactly the window on screen, as the single-instance delete does: it comes back short because
		// entities were deleted, which says nothing about whether the list ends there.
		const int32 RelistLimit = LastContainerLimit > 0 ? LastContainerOffset + LastContainerLimit : 0;
		ReadContainers(LastContainerTypeFilter, LastContainerSessionFilter, RelistLimit, 0, /*bAppend*/ false,
			EContainerPageEvidence::Keep);
	}

	// The retained plan describes the schema as it stood BEFORE these deletes, so every entity just removed is still
	// listed there with a verdict about where it came from. A verdict computed before a write is stale the moment
	// the write lands, and with no plan the page says nothing at all about provenance, which is the honest answer
	// until the next one is run.
	if (SelectedAppId != 0 && ModelSnapshotsByApp.Remove(SelectedAppId) > 0)
	{
		OnModelSnapshotChanged.Broadcast();
	}
}

void FCrowdyStudioController::FetchContainers(const FString& TypeNameFilter, const FString& SessionIdFilter,
	int32 Limit, int32 Offset, bool bAppend)
{
	ReadContainers(TypeNameFilter, SessionIdFilter, Limit, Offset, bAppend, EContainerPageEvidence::Update);
}

void FCrowdyStudioController::ReadContainers(const FString& TypeNameFilter, const FString& SessionIdFilter,
	int32 Limit, int32 Offset, bool bAppend, EContainerPageEvidence Evidence)
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app to list live containers."), true);
		return;
	}

	// Remembered so DeleteContainer's re-list reproduces what is on screen, at the same filters and over the same
	// window, instead of silently widening to every container for the app.
	LastContainerTypeFilter = TypeNameFilter;
	LastContainerSessionFilter = SessionIdFilter;
	LastContainerLimit = Limit;
	LastContainerOffset = Offset;

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), SelectedAppId);
	// Optional filters: an empty box means "all", sent as JSON null.
	if (TypeNameFilter.IsEmpty()) { Variables->SetField(TEXT("typeName"), MakeShared<FJsonValueNull>()); }
	else { Variables->SetStringField(TEXT("typeName"), TypeNameFilter); }
	if (SessionIdFilter.IsEmpty()) { Variables->SetField(TEXT("sessionId"), MakeShared<FJsonValueNull>()); }
	else { Variables->SetStringField(TEXT("sessionId"), SessionIdFilter); }
	// Paging is opt-in and both arguments are plain Ints, not BigInt. Omitted entirely when unset, so a caller
	// that does not page sends exactly the request it always has.
	if (Limit > 0) { Variables->SetNumberField(TEXT("limit"), Limit); }
	if (Offset > 0) { Variables->SetNumberField(TEXT("offset"), Offset); }

	const uint64 ReadSerial = BeginContainerRead();
	const int64 RequestAppId = SelectedAppId;
	const uint64 FamilySerial = BeginFamilyRead(ECrowdyModelFamily::LiveModels);

	SendGame(ECrowdyCppApiDomain::GameModel, TEXT("GameModelContainers"), Variables,
		[this, Limit, bAppend, Evidence, ReadSerial, RequestAppId, FamilySerial](const TSharedPtr<FJsonObject>& Envelope)
		{
			if (!IsLatestContainerRead(ReadSerial))
			{
				return;
			}

			TArray<TSharedPtr<FStudioContainer>> Page;
			CrowdyStudioGql::ParseContainers(Envelope, TEXT("gameModelContainers"), Page);
			IngestContainerPage(MoveTemp(Page), Limit, bAppend, Evidence);
			AcceptFamilyReply(ECrowdyModelFamily::LiveModels, RequestAppId, FamilySerial,
				ECrowdyModelLoadState::Loaded);

			// Never a total: the query has no count, so a paged read reports only what has been read so far.
			const FString Note = Limit > 0
				? FString::Printf(TEXT("Showing %d live container(s) read so far."), Containers.Num())
				: FString::Printf(TEXT("%d live container(s)."), Containers.Num());
			SetStatus(Note, false);
			OnContainersChanged.Broadcast();
		},
		[this, RequestAppId, FamilySerial]()
		{
			// The announcement matters more here than anywhere else: the Live tab sets a latch when it issues a read
			// and consumes it in the handler for this delegate. A failure that never broadcast left that latch
			// standing, so the next change from any other source consumed it and marked models as read against a
			// read that never filled anything.
			if (AcceptFamilyReply(ECrowdyModelFamily::LiveModels, RequestAppId, FamilySerial,
				ECrowdyModelLoadState::Failed))
			{
				OnContainersChanged.Broadcast();
			}
		});
}

void FCrowdyStudioController::IngestContainerPage(TArray<TSharedPtr<FStudioContainer>>&& Page, int32 Limit,
	bool bAppend, EContainerPageEvidence Evidence)
{
	// The page as the server sent it, before any de-duplication, is what says whether another page exists.
	LastContainerPageSize = Page.Num();
	if (Evidence == EContainerPageEvidence::Update)
	{
		bContainersMayHaveMore = PageMayHaveMore(Limit, LastContainerPageSize);
	}

	if (bAppend)
	{
		AppendContainers(Page);
	}
	else
	{
		Containers = MoveTemp(Page);
	}
}

void FCrowdyStudioController::AppendContainers(const TArray<TSharedPtr<FStudioContainer>>& Page)
{
	for (const TSharedPtr<FStudioContainer>& Incoming : Page)
	{
		if (!Incoming.IsValid())
		{
			continue;
		}

		// Two reads at different offsets can overlap when the window shifts under them, so the same container
		// arrives twice and would be listed twice. Container ids are opaque, and both FString::operator== and a
		// TSet<FString> compare case-insensitively, so the match has to be spelled out as case-sensitive.
		const bool bAlreadyHeld = Containers.ContainsByPredicate(
			[&Incoming](const TSharedPtr<FStudioContainer>& Held)
			{
				return Held.IsValid() && Held->ContainerId.Equals(Incoming->ContainerId, ESearchCase::CaseSensitive);
			});

		if (!bAlreadyHeld)
		{
			Containers.Add(Incoming);
		}
	}
}

void FCrowdyStudioController::FetchContainerState(const FString& ContainerId)
{
	if (SelectedAppId == 0 || ContainerId.IsEmpty())
	{
		SetStatus(TEXT("Select an app and a container to read its state."), true);
		return;
	}

	SelectedContainerId = ContainerId;
	ContainerState = FStudioContainerState();

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	SetBigIntField(Variables, TEXT("appId"), SelectedAppId);
	Variables->SetStringField(TEXT("containerId"), ContainerId);

	SendGame(ECrowdyCppApiDomain::GameModel, TEXT("GameModelContainerState"), Variables,
		[this, ContainerId](const TSharedPtr<FJsonObject>& Envelope)
		{
			if (SelectedContainerId != ContainerId)
			{
				return; // selection moved on before the reply arrived
			}
			CrowdyStudioGql::ParseContainerState(Envelope, TEXT("gameModelContainerState"), ContainerState);
			OnContainerStateChanged.Broadcast();
		});
}

void FCrowdyStudioController::ClearAppScopedState()
{
	// End anything the previous app had in flight that a caller is WAITING on, before the state it reads is emptied.
	// Neither of these can end itself here: a reply issued for one app is dropped on arrival once the selection has
	// moved, so nothing would decrement the outstanding work and nothing would ever fire the completion. A pre-flight
	// left hanging strands a review saying a check is under way with no way to start another, and a commit left
	// hanging leaves the page reporting a delete that is still running.
	FinishLiveModelCount();
	FinishDeleteWalk(DeleteCommitCompleted, /*bWasCanceled*/ true);
	FinishContainerPurge(/*bStopped*/ true, /*bByCancel*/ true, FString());

	// Empty every list, policy and selection the OnSelectedAppChanged views render, so switching apps never leaves
	// the previous app's teams, channels, grids or game-model schema on screen while the new app's token is minted -
	// or if that mint fails (e.g. the app isn't provisioned on the game tier) and the reload is therefore suppressed.
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

	ContainerTypes.Reset();
	PropertyDefs.Reset();
	PropertyDefsByType.Reset();
	PropertyDefsMirrorType.Reset();
	// Any attribute read still in flight belongs to the previous app; its reply is discarded, so drop the keys or the
	// new app's types of the same name could never be loaded. The per-type serials go with them: a type of the same
	// name under the new app starts its numbering fresh, so a reply from the previous app can never match.
	PropertyDefFetchesInFlight.Reset();
	PropertyDefLatestRequest.Reset();
	// A model of the same name under the new app has not been read at all, let alone failed.
	PropertyDefFailedTypes.Reset();
	Functions.Reset();
	UnfilteredFunctions.Reset();
	// Any function read still in flight belongs to the previous app, and switching back to that app restores the id
	// its reply is checked against. Moving the serial on is what drops it, so a read issued before the switch can
	// never refill the mirror the switch just emptied.
	++NextFunctionReadSerial;
	// An automation names the model it targets, so leaving one on screen under another app puts a type name in front of
	// the user that the new app may not even have.
	Automations.Reset();
	AutomationTriggers.Reset();
	// The three lists above now hold nothing, so the new app has not been loaded. Without this the page would see its
	// own "already loaded" mark still set to the previous app and never refill.
	GameModelListsAppId = 0;
	// And nothing has been READ for the new app either. Left set, an empty list would read as "this app has none",
	// which is what clears a delete pre-flight's blockers for every model in the app at once. The serial moves on
	// first, so a read still in flight across the switch can never land on a slot it would match again: switching
	// away and back selects the SAME app, which is the one case an app id alone cannot tell apart.
	++NextFamilyReadSerial;
	for (FCrowdyFamilyLoad& Load : FamilyLoads)
	{
		Load = FCrowdyFamilyLoad();
	}
	// The last commit's result and its leftovers are deliberately NOT cleared here. The teardown above ends a walk
	// this switch just cancelled, and that outcome is the only record of it; a reader who switched apps mid-delete
	// still has to be able to see what happened. Both carry the app they belong to, and a review shows them only
	// for its own app and only while they still describe what its button would run.
	Features.Reset();
	TierFeatures.Reset();
	AccessTiers.Reset();
	GameModelPolicy = FStudioGameModelPolicy();
	// A lint report is about ONE app, so the new app must not inherit the previous one's findings.
	GameModelLint = FStudioLintReport();
	Containers.Reset();
	ContainerState = FStudioContainerState();
	SelectedContainerId.Reset();
	LastContainerTypeFilter.Reset();
	LastContainerSessionFilter.Reset();
	// The page the previous app was read at means nothing under the new one, and a stale full-page reading would
	// otherwise offer "load more" against a list that has not been read at all.
	LastContainerLimit = 0;
	LastContainerOffset = 0;
	LastContainerPageSize = 0;
	bContainersMayHaveMore = false;
	// Any container read still in flight belongs to the previous app, and switching back to that app restores the
	// id its reply is checked against. Moving the serial on is what drops it, so a page issued before the switch
	// can never land on the list the switch just emptied and leave it holding page two and nothing else.
	++NextContainerReadSerial;

	OnTeamsChanged.Broadcast();
	OnChannelsChanged.Broadcast();
	OnGroupDetailChanged.Broadcast();
	OnTeamPolicyChanged.Broadcast();
	OnChannelPolicyChanged.Broadcast();
	OnNearbyGridsChanged.Broadcast();
	OnGridWhitelistChanged.Broadcast();
	OnGridDetailChanged.Broadcast();
	OnContainerTypesChanged.Broadcast();
	OnPropertyDefsChanged.Broadcast();
	OnPropertyDefsCached.Broadcast();
	OnFunctionsChanged.Broadcast();
	OnAutomationsChanged.Broadcast();
	OnFeaturesChanged.Broadcast();
	OnTierFeaturesChanged.Broadcast();
	OnAccessTiersChanged.Broadcast();
	OnGameModelPolicyChanged.Broadcast();
	// Announced here rather than left to OnSelectedAppChanged, which is broadcast only after a successful mint: a
	// failed mint or a sign-out would otherwise leave the Issues tab rendering the previous app's findings behind a
	// tab the strip has already greyed out, because the strip reads the report every frame and the panel does not.
	OnGameModelLintChanged.Broadcast();
	OnContainersChanged.Broadcast();
	OnContainerStateChanged.Broadcast();
	// The retained snapshots are keyed by app id and read through the live selection, so the previous app's verdict
	// is unreachable under the new app's name without anything being erased. What the views still need is the news
	// that the answer has changed: the new app has its own snapshot, or none at all, and a browser left showing the
	// previous app's Source and Status columns would be attributing one app's schema to another.
	OnModelSnapshotChanged.Broadcast();
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

	// A schema-sync plan is computed against one app's server schema; drop it on an app switch so app A's
	// diff can never be applied to app B (ApplySchemaSync also refuses on a PlannedSyncAppId mismatch).
	ClearSchemaSyncState();
	// The selection names entities of the app being left. ClearSchemaSyncState deliberately does NOT clear it,
	// because a re-plan runs that and the selection has to survive one; an app switch is the opposite case.
	ClearSchemaApplySelection();
	// The channel auto-create latch + session-channel readiness are per-app; reset them so a mid-continuation app
	// switch cannot leave the latch set, and the setup strip does not carry the old app's channel state.
	bAutoCreatingSessionChannel = false;
	SessionChannelReadiness = ECrowdyStudioReadiness::Unknown;

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
		SetStatus(TEXT("Game-plane actions (teams, channels, grids, game models) need a session sign-in: "
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

UCrowdyGameKitConfig* FCrowdyStudioController::GetKitDeployConfig()
{
	if (!KitDeployConfig.IsValid())
	{
		// A transient object, not a saved asset: the Deploy Kit card edits it inline so a designer never has to
		// create a content asset. It lives for the editor session (rooted by the strong ptr).
		KitDeployConfig.Reset(NewObject<UCrowdyGameKitConfig>(GetTransientPackage(), NAME_None, RF_Transient));
	}
	return KitDeployConfig.Get();
}

TSet<FString> FCrowdyStudioController::GatherRecognizedKitTypePrefixes() const
{
	TSet<FString> Prefixes;
	// Read the config the Deploy Kit card edited this session directly (do NOT create one via GetKitDeployConfig: a
	// plan must not materialize a kit config as a side effect), plus any prefixes persisted from a prior deploy.
	if (KitDeployConfig.IsValid())
	{
		CollectEffectiveKitTypePrefixes(KitDeployConfig->Layers, Prefixes);
	}
	Prefixes.Append(LoadPersistedKitPrefixes(SelectedAppId));
	return Prefixes;
}

TSet<FString> FCrowdyStudioController::LoadPersistedKitPrefixes(int64 AppId) const
{
	TSet<FString> Out;
	if (AppId == 0 || !GConfig)
	{
		return Out;
	}
	FString Joined;
	if (GConfig->GetString(KitPrefixConfigSection, *LexToString(AppId), Joined, GEditorPerProjectIni) && !Joined.IsEmpty())
	{
		TArray<FString> Parts;
		Joined.ParseIntoArray(Parts, TEXT(","), /*CullEmpty*/ true);
		for (const FString& Part : Parts)
		{
			const FString Trimmed = Part.TrimStartAndEnd();
			if (!Trimmed.IsEmpty())
			{
				Out.Add(Trimmed);
			}
		}
	}
	return Out;
}

void FCrowdyStudioController::PersistDeployedKitPrefixes(int64 AppId, const TSet<FString>& Prefixes)
{
	if (AppId == 0 || Prefixes.Num() == 0 || !GConfig)
	{
		return;
	}
	// Union with the already-persisted set so re-deploying one kit never drops another kit's protection for this app.
	TSet<FString> Merged = LoadPersistedKitPrefixes(AppId);
	Merged.Append(Prefixes);
	TArray<FString> Sorted = Merged.Array();
	Sorted.Sort();
	const FString JoinedPrefixes = FString::Join(Sorted, TEXT(","));
	GConfig->SetString(KitPrefixConfigSection, *LexToString(AppId), *JoinedPrefixes, GEditorPerProjectIni);
	GConfig->Flush(false, GEditorPerProjectIni);
}

TSet<FString> FCrowdyStudioController::LoadPersistedKitTypeNames(int64 AppId) const
{
	return LoadKitConfigSet(KitTypeNamesConfigSection, AppId);
}

TSet<FString> FCrowdyStudioController::LoadPersistedKitFunctionNames(int64 AppId) const
{
	return LoadKitConfigSet(KitFunctionNamesConfigSection, AppId);
}

void FCrowdyStudioController::PersistDeployedKitNames(int64 AppId, const TSet<FString>& TypeNames, const TSet<FString>& FunctionNames)
{
	PersistKitConfigSet(KitTypeNamesConfigSection, AppId, TypeNames);
	PersistKitConfigSet(KitFunctionNamesConfigSection, AppId, FunctionNames);
}

void FCrowdyStudioController::DeployGameKit(const UCrowdyGameKitConfig* Config,
	TFunction<void(bool, const FString&)> OnDone)
{
	auto Fail = [this, &OnDone](const FString& Message)
	{
		SetStatus(Message, true);
		if (OnDone)
		{
			OnDone(false, Message);
		}
	};

	if (!Config)
	{
		Fail(TEXT("Select a Game Kit config asset to deploy."));
		return;
	}
	if (SelectedAppId == 0)
	{
		Fail(TEXT("Select an app before deploying a Game Kit."));
		return;
	}
	// Seeding schema + automations is manage_apps-gated, and that authority rides the app-scoped token minted from
	// a session sign-in; an org-token sign-in cannot mint, so fail closed with a clear message rather than let the
	// server reject the whole deploy.
	if (!CanMintAppToken())
	{
		Fail(TEXT("Deploying a Game Kit needs a session sign-in that can mint an app token: sign in with email, dev, "
			"or a magic link. An organization token only covers account settings."));
		return;
	}

	const int64 AppId = SelectedAppId;
	const FString SessionId = Config->SessionId;
	// Copy the layers so the deploy is independent of later edits to the config asset, and hold a strong ref to the
	// config so its instanced preset subobjects (referenced by the copied TObjectPtrs) survive a GC during the mint
	// window; a bare TObjectPtr in a lambda capture is not a GC root.
	TArray<TObjectPtr<UCrowdyKitLayerPreset>> Layers = Config->Layers;
	// The effective kit type prefixes, captured before the layers move, so a successful deploy can persist them and the
	// schema-sync prune-protection survives an editor restart (the authored config is session-only).
	TSet<FString> DeployedPrefixes;
	CollectEffectiveKitTypePrefixes(Config->Layers, DeployedPrefixes);
	// The EXACT container-type + function names this deploy seeds, read from the same emit the deploy runs, captured
	// before the layers move. Persisted once the seed lands so a later schema sync protects the kit's schema by exact
	// name - the only recognition that covers an empty-prefix kit whose bare names carry no prefix.
	TArray<FString> DeployedTypeNamesArr;
	TArray<FString> DeployedFunctionNamesArr;
	if (!CrowdyKitDeployedNames(Config->Layers, AppId, SessionId, DeployedTypeNamesArr, DeployedFunctionNamesArr))
	{
		UE_LOG(LogCrowdyStudio, Warning,
			TEXT("Could not read the kit's deployed schema names; a later schema sync will not exact-name protect this kit's empty-prefix types or functions from prune."));
	}
	TSet<FString> DeployedTypeNames;
	DeployedTypeNames.Append(DeployedTypeNamesArr);
	TSet<FString> DeployedFunctionNames;
	DeployedFunctionNames.Append(DeployedFunctionNamesArr);
	const TStrongObjectPtr<UCrowdyGameKitConfig> ConfigGuard(const_cast<UCrowdyGameKitConfig*>(Config));

	// Runs once the app token is confirmed fresh. Threads the caller's OnDone through so the mint-failure path can
	// still report it.
	auto Proceed = [this, AppId, SessionId, Layers = MoveTemp(Layers), DeployedPrefixes = MoveTemp(DeployedPrefixes),
		DeployedTypeNames = MoveTemp(DeployedTypeNames), DeployedFunctionNames = MoveTemp(DeployedFunctionNames), ConfigGuard]
		(TFunction<void(bool, const FString&)> Done)
	{
		// A mint may have resolved after the user switched the selected app; the layers/appId here are the ones the
		// user confirmed, so refuse rather than deploy the old app's schema with the new app's token.
		if (SelectedAppId != AppId)
		{
			const FString Message = TEXT("The selected app changed during the deploy; re-run it for the current app.");
			SetStatus(Message, true);
			if (Done)
			{
				Done(false, Message);
			}
			return;
		}

		const FString GameUrl = ResolveGameUrl();
		// GameAppToken is the app-scoped, manage_apps-bearing token the schema-sync mutations also use.
		CrowdyKitDeployLayers(Layers, AppId, SessionId, GameUrl, GameAppToken,
			[WeakThis = TWeakPtr<FCrowdyStudioController>(AsShared()), Done = MoveTemp(Done), AppId, DeployedPrefixes,
				DeployedTypeNames, DeployedFunctionNames]
			(FCrowdyKitDeployOutcome Outcome)
			{
				const FString Message = Outcome.bOk
					? FString::Printf(TEXT("Game Kit deployed: %d of %d step(s) applied."),
						Outcome.StepsCompleted, Outcome.StepsTotal)
					: (Outcome.Error.IsEmpty() ? FString(TEXT("Game Kit deploy failed.")) : Outcome.Error);

				if (const TSharedPtr<FCrowdyStudioController> Self = WeakThis.Pin())
				{
					Self->SetStatus(Outcome.bOk ? Message : FString(TEXT("Game Kit deploy failed: ")) + Message,
						!Outcome.bOk);
					// Remember this deploy's type prefixes AND the exact type/function names so a later schema sync keeps
					// the kit's types/props/functions off the prune list even in a fresh editor session with no kit
					// re-authored. The exact names cover an empty-prefix kit the prefixes cannot. Persist as soon as the
					// seed step lands (the seed creates every type and function), even if a later automation or trigger
					// step fails: the schema exists on the server from that point, so it must be protected or a partial
					// deploy would leave the created schema deletable by a later prune.
					if (Outcome.StepsCompleted >= 1)
					{
						Self->PersistDeployedKitPrefixes(AppId, DeployedPrefixes);
						Self->PersistDeployedKitNames(AppId, DeployedTypeNames, DeployedFunctionNames);
					}
				}
				if (Done)
				{
					Done(Outcome.bOk, Message);
				}
			});
	};

	if (!NeedsAppTokenRefresh())
	{
		Proceed(MoveTemp(OnDone));
		return;
	}

	MintAppToken([Proceed = MoveTemp(Proceed), OnDone = MoveTemp(OnDone)](bool bMinted) mutable
	{
		if (bMinted)
		{
			Proceed(MoveTemp(OnDone));
		}
		else if (OnDone)
		{
			// MintAppToken already set a descriptive status; report the failed deploy so the view stops waiting.
			OnDone(false, TEXT("Could not mint an app token for the Game Kit deploy."));
		}
	});
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

void FCrowdyStudioController::StartMint()
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
		[WeakThis, MintForAppId](FCrowdyCppAppTokenResult Result)
		{
			TSharedPtr<FCrowdyStudioController> Self = WeakThis.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			Self->EndRequest();
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
	OnBusyChanged.Broadcast();
}

void FCrowdyStudioController::EndRequest()
{
	if (InFlightCount > 0)
	{
		--InFlightCount;
	}
	OnBusyChanged.Broadcast();
}

void FCrowdyStudioController::FetchAppsForSignedInUser()
{
	FetchApps();

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

const FString& FCrowdyStudioController::GetLastGameModelTab() const
{
	static const FString Empty;
	if (const UCrowdyStudioUserSettings* User = GetUserSettings())
	{
		return User->LastGameModelTab;
	}
	return Empty;
}

void FCrowdyStudioController::SetLastGameModelTab(const FString& TabKey)
{
	if (UCrowdyStudioUserSettings* User = GetUserSettings())
	{
		User->LastGameModelTab = TabKey;
		User->SaveConfig();
	}
}
