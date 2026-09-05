// Fill out your copyright notice in the Description page of Project Settings.


#include "Subsystem/CrowdySDKSubsystem.h"
#include "CrowdySDKLog.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Utils/HelperFunctions.h"
#include "Core/Audio/VoiceChat/VoiceChatSubsystem.h"
#include "Core/Audio/VoiceChat/Service/FVoiceChatService.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Internal/FCrowdyServiceRegistry.h"
#include "Messages/FPingTestMessage.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Messages/Actor/FActorHeartbeatRequestMessage.h"
#include "Messages/Actor/FActorUpdateRequestMessage.h"
#include "Messages/GameObjects/FGameEventRequest.h"
#include "Messages/GameObjects/FSingleActorMessage.h"
#include "Serialization/FCrowdyMessageParser.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Subsystem/CrowdyWorkerThreadsSubsystem.h"
#include "Network/UDP/CrowdyUDPSubsystem.h"
#include "Network/UDP/FCrowdyUDPEndpoint.h"
#include "CrowdyCppClient.h"
#include "Network/CrowdyCpp/CrowdyCppClientSubsystem.h"
#include "Network/CrowdyCpp/CrowdyCppReplicationSubsystem.h"
#include "Subsystem/CrowdyAutoRegistry.h"
#include "Subsystem/CrowdyHostSubsystem.h"
#include "Subsystem/CrowdyPersistenceSubsystem.h"
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "Utils/UActorUpdatePayloadRegistry.h"
#include "Utils/UEventPayloadRegistry.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystem/CrowdyAvatars.h"
#include "Subsystem/CrowdyTeams.h"
#include "Subsystem/CrowdyChannels.h"
#include "Core/CrowdySDKBridgeSubsystem.h"

#include <atomic>

void UCrowdySDKSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Collection.InitializeDependency<UCrowdyAutoRegistry>();
	
	Super::Initialize(Collection);
	
	// Dependencies first
	GameSession            = Collection.InitializeDependency<UCrowdyGameSession>();
	WorkerThreadsSubsystem = Collection.InitializeDependency<UCrowdyWorkerThreadsSubsystem>();
	UdpSubsystem           = Collection.InitializeDependency<UCrowdyUDPSubsystem>();
	PersistenceSubsystem   = Collection.InitializeDependency<UCrowdyPersistenceSubsystem>();
	Collection.InitializeDependency<UCrowdyAvatars>();
	Collection.InitializeDependency<UCrowdyTeams>();
	Collection.InitializeDependency<UCrowdyChannels>();
	UCrowdyAuthentication* CrowdyAuth = Collection.InitializeDependency<UCrowdyAuthentication>();
	UCrowdyCppReplicationSubsystem* ReplicationRouting =
		Collection.InitializeDependency<UCrowdyCppReplicationSubsystem>();
	
	
	
	// Pure allocations (no world required)
	ServiceRegistry = new FCrowdyServiceRegistry();
	Parser          = new FCrowdyMessageParser(ServiceRegistry, UdpSubsystem, GameSession, [this]{ HandleTokenExpired(); });

	if (UCrowdySDKBridgeSubsystem* BridgeSub = GetGameInstance()->GetSubsystem<UCrowdySDKBridgeSubsystem>())
	{
		BridgeSub->ServiceRegistry = ServiceRegistry;
		BridgeSub->DispatchActorUpdateFn = [this](int64 X, int64 Y, int64 Z, ECrowdyDecayRate D, ECrowdyReplicationDistance R, const FString& ID, const FInstancedStruct& State, FCrowdyClassID ClassID, bool bAsync)
		{
			DispatchActorUpdate(X, Y, Z, D, R, ID, State, ClassID, bAsync);
		};
		BridgeSub->DispatchActorHeartbeatFn = [this](int64 X, int64 Y, int64 Z, const FString& ID)
		{
			DispatchActorHeartbeat(X, Y, Z, ID);
		};
		BridgeSub->DispatchGameEventFn = [this](int64 X, int64 Y, int64 Z, ECrowdyDecayRate D, ECrowdyReplicationDistance R, const FGuid& ID, FInstancedStruct Payload, ECrowdyTarget Target, const FGuid& TargetID, bool bAsync)
		{
			DispatchGameEvent_Internal(X, Y, Z, D, R, ID, MoveTemp(Payload), Target, TargetID, bAsync);
		};
		BridgeSub->DispatchGameEventViewFn = [this](int64 X, int64 Y, int64 Z, ECrowdyDecayRate D, ECrowdyReplicationDistance R, const FGuid& ID, const UScriptStruct* PayloadStruct, const void* PayloadMemory, ECrowdyTarget Target, const FGuid& TargetID)
		{
			DispatchGameEventView_Internal(X, Y, Z, D, R, ID, PayloadStruct, PayloadMemory, Target, TargetID);
		};
		BridgeSub->DispatchSingleActorMessageFn = [this](int64 X, int64 Y, int64 Z, const FGuid& TargetActorID, FInstancedStruct Payload, bool bAsync)
		{
			DispatchSingleActorMessage_Internal(X, Y, Z, TargetActorID, MoveTemp(Payload), bAsync);
		};
		BridgeSub->PublishReliableRpcFn = [this](const FString& ChannelName, const TArray<uint8>& Payload)
		{
			if (UCrowdyChannels* Channels = GetGameInstance()->GetSubsystem<UCrowdyChannels>())
				Channels->PublishReliableRpc(ChannelName, Payload);
		};
		BridgeSub->SendMessageFn = [this](const ICrowdyMessage& Msg)
		{
			SendMessage(Msg);
		};
		BridgeSub->BroadcastHUDReadyFn = [this]()
		{
			OnCrowdyHUDReady.Broadcast();
		};
		BridgeSub->ReloadConfigFn = [this]()
		{
			// Live re-apply of the developer settings so an editor Config Sync reaches a running session.
			// Endpoints need no work here: the shared API client re-reads them from these settings on every
			// call and rebuilds itself when they differ. The AppID feeds subsequent ops. The live connection
			// and the current login keep the session they were established with, protocol choice included.
			const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>();
			if (Settings && IsValid(GameSession))
			{
				GameSession->SetAppID(Settings->AppID);
			}
			UE_CLOG(CrowdySDKTrace::Sdk(), LogCrowdySDK, Log, TEXT("Reloaded developer settings into the live session."));
		};
	}

	const UWorld* World = GetWorld();
	if (!World || World->GetGameInstance() != GetGameInstance())
	{
		UE_LOG(LogCrowdySDK, Fatal, TEXT("UWorld is invalid while initializing."));
		return;
	}
		

	// Create anything that may depend on world/online/sockets
	VoiceChatService       = new FVoiceChatService(ServiceRegistry, GameSession, [this](const ICrowdyMessage& Msg) { SendMessage(Msg); });
	PersistenceSubsystem->InitialSetup(GetGameInstance()->GetSubsystem<UCrowdySDKBridgeSubsystem>(), GameSession);
	
	auto LogNull = [](const TCHAR* Name)
	{
		UE_LOG(LogCrowdySDK, Error, TEXT("%s is null or invalid"), Name);
	};

	bool bFailed = false;
	bFailed |= !ServiceRegistry && (LogNull(TEXT("ServiceRegistry")), true);
	bFailed |= !Parser && (LogNull(TEXT("Parser")), true);
	bFailed |= !IsValid(WorkerThreadsSubsystem) && (LogNull(TEXT("WorkerThreadsSubsystem")), true);
	bFailed |= !IsValid(UdpSubsystem) && (LogNull(TEXT("UdpSubsystem")), true);
	bFailed |= !IsValid(GameSession) && (LogNull(TEXT("GameSession")), true);
	bFailed |= !VoiceChatService && (LogNull(TEXT("VoiceChatService")), true);

	if (bFailed) return;

	WorkerThreadsSubsystem->InitializeWorkerThreadPool();

	// Inbound messages are decoded by this parser and handed to this registry. Nothing arrives until a connection
	// is installed.
	if (IsValid(ReplicationRouting))
	{
		ReplicationRouting->SetReceiveTarget(Parser, ServiceRegistry, UdpSubsystem);
	}

	// Subscribed up front rather than when the connection comes up, so the first ping response is measured too.
	EnsurePingResponseSubscription();
	bIsRegistered = true;

	UdpSubsystem->OnUDPConnectionSuccessful.AddDynamic(this, &UCrowdySDKSubsystem::OnUDPConnectionSuccessful);
	UdpSubsystem->OnUDPTimeout.AddDynamic(this, &UCrowdySDKSubsystem::OnUDPTimeout);

	if (!TryLoadConfiguration())
		UE_LOG(LogCrowdySDK, Error, TEXT("Failed to load configuration from Developer Settings."));

	UE_CLOG(CrowdySDKTrace::Sdk(), LogCrowdySDK, Log, TEXT("CrowdySDK Subsystem Initialized (PostWorldInit)"));

	// Avatars, teams and channels issue their own calls on the shared API client and need nothing injected.
	// Inbound channel notifications (type 18) flow through the service registry directly to UCrowdyChannels.
	CrowdyAuth->InjectDependencies(GameSession);

	CrowdyAuth->OnLogin.AddDynamic(this, &UCrowdySDKSubsystem::HandleAuthLogin);
	CrowdyAuth->OnLoginFailed.AddDynamic(this, &UCrowdySDKSubsystem::HandleAuthLoginFailed);
	CrowdyAuth->OnRegister.AddDynamic(this, &UCrowdySDKSubsystem::HandleAuthRegister);
	CrowdyAuth->OnRegisterFailed.AddDynamic(this, &UCrowdySDKSubsystem::HandleAuthRegisterFailed);
	CrowdyAuth->OnSessionRestored.AddDynamic(this, &UCrowdySDKSubsystem::HandleAuthSessionRestored);
	CrowdyAuth->OnAppTokenRefreshed.AddDynamic(this, &UCrowdySDKSubsystem::HandleAppTokenRefreshed);
}

void UCrowdySDKSubsystem::Deinitialize()
{
	// Stop all repeating timers before anything else is torn down. The GameHost poll rides the GameInstance timer
	// manager (UWorld::GetTimerManager forwards to it for a Game/PIE world), so it survives level travel without a
	// per-world re-arm; clearing it here on GameInstance teardown is enough.
	StopHostPolling();

	if (UCrowdySDKBridgeSubsystem* B = GetGameInstance()->GetSubsystem<UCrowdySDKBridgeSubsystem>())
	{
		B->ServiceRegistry       = nullptr;
		B->DispatchActorUpdateFn  = nullptr;
		B->DispatchActorHeartbeatFn = nullptr;
		B->DispatchGameEventFn    = nullptr;
		B->DispatchGameEventViewFn = nullptr;
		B->PublishReliableRpcFn   = nullptr;
		B->SendMessageFn          = nullptr;
		B->BroadcastHUDReadyFn    = nullptr;
		B->ReloadConfigFn         = nullptr;
	}

	// Detached before the parser and the registry go, because the routed receive path holds both of them.
	if (const UGameInstance* Instance = GetGameInstance())
	{
		if (UCrowdyCppReplicationSubsystem* ReplicationRouting =
			Instance->GetSubsystem<UCrowdyCppReplicationSubsystem>())
		{
			ReplicationRouting->SetReceiveTarget(nullptr, nullptr, nullptr);
		}
	}

	ServiceRegistry = nullptr;
	Parser = nullptr;
	UEventPayloadRegistry::Get()->Reset();
	UEventPayloadRegistry::Get()->Shutdown();
	UActorUpdatePayloadRegistry::Get()->Reset();
	UActorUpdatePayloadRegistry::Get()->Shutdown();
	Super::Deinitialize();
}

void UCrowdySDKSubsystem::ReloadEndpointsFromSettings()
{
	// Nothing to push. The shared API client re-reads both endpoints from the developer settings every time it is
	// resolved and rebuilds itself when they differ, so a settings change already reaches the next call on its own.
}

void UCrowdySDKSubsystem::Login(const FString Email, const FString Password) const
{
	if (UCrowdyAuthentication* Auth = GetGameInstance()->GetSubsystem<UCrowdyAuthentication>())
		Auth->Login(Email, Password, FOnAuthSuccess(), FOnAuthError());
}

void UCrowdySDKSubsystem::Register(const FString Email, const FString Password) const
{
	if (UCrowdyAuthentication* Auth = GetGameInstance()->GetSubsystem<UCrowdyAuthentication>())
		Auth->Register(Email, Password, FOnAuthSuccess(), FOnAuthError());
}

void UCrowdySDKSubsystem::CompleteLoginLink(const FString Token) const
{
	if (UCrowdyAuthentication* Auth = GetGameInstance()->GetSubsystem<UCrowdyAuthentication>())
		Auth->CompleteLoginLink(Token, FOnAuthSuccess(), FOnAuthError());
}

void UCrowdySDKSubsystem::BeginMagicLinkSignIn(const FString Email) const
{
	if (UCrowdyAuthentication* Auth = GetGameInstance()->GetSubsystem<UCrowdyAuthentication>())
		Auth->BeginMagicLinkSignIn(Email, FOnAuthSuccess(), FOnAuthError());
}

void UCrowdySDKSubsystem::BeginSocialSignIn(const FString Provider) const
{
	if (UCrowdyAuthentication* Auth = GetGameInstance()->GetSubsystem<UCrowdyAuthentication>())
		Auth->BeginSocialSignIn(Provider, FOnAuthSuccess(), FOnAuthError());
}

void UCrowdySDKSubsystem::RefreshAppToken() const
{
	if (UCrowdyAuthentication* Auth = GetGameInstance()->GetSubsystem<UCrowdyAuthentication>())
		Auth->RefreshAppToken();
}

void UCrowdySDKSubsystem::Logout() const
{
	// Forget the durable credential too: cancel the refresh timer and delete the
	// persisted SESSION token so the next launch does not silently restore the user.
	if (UCrowdyAuthentication* Auth = GetGameInstance()->GetSubsystem<UCrowdyAuthentication>())
		Auth->ClearSavedSession();

	GameSession->ClearCurrentSessionData();

	// Scrub the bearers the shared API client still holds. It reinstalls them from here on every resolve, so a call
	// issued after this point would otherwise still go out as the account that just signed out.
	if (UCrowdyCppClientSubsystem* ClientHost = GetGameInstance()->GetSubsystem<UCrowdyCppClientSubsystem>())
	{
		ClientHost->SetManagementToken(FString());
		ClientHost->SetGameToken(FString());
	}

	// Close the routed connection for the same reason. It signs with the token it was given when it opened rather
	// than reading the session, so clearing the session above does not reach it and it would go on sending validly
	// signed traffic as the account that just left.
	if (UCrowdyCppReplicationSubsystem* Replication = GetGameInstance()->GetSubsystem<UCrowdyCppReplicationSubsystem>())
	{
		Replication->CloseConnection();
	}

	// The host subsystem only exists in a Game or PIE world, so signing out from anywhere else finds nothing to
	// clear rather than crashing.
	UWorld* World = GetWorld();
	if (UCrowdyHostSubsystem* HostSubsystem = World ? World->GetSubsystem<UCrowdyHostSubsystem>() : nullptr)
	{
		HostSubsystem->SetHostUserID(0);
	}
	
	// Stop host polling user is no longer authenticated.
	// Cast away const: Logout() is declared const but timer management is
	// inherently mutable state. The alternative is making Logout() non-const
	// which would break existing Blueprint call sites.
	const_cast<UCrowdySDKSubsystem*>(this)->StopHostPolling();

	OnLogout.Broadcast(true);
}

void UCrowdySDKSubsystem::SetGameSessionInfo(const FGameSessionInfo GameSessionInfo)
{
	GameSession->SetUserID(GameSessionInfo.UserID);

	// Management plane: the identity SESSION token.
	GameSession->SetSessionToken(GameSessionInfo.SessionToken);
	GameSession->SetSessionGameTokenID(GameSessionInfo.SessionGameTokenID);

	// Gameplay plane: the app-scoped token (Game API / UDP). Kept distinct from the
	// session token so a management credential never authorizes gameplay.
	GameSession->SetGameToken(GameSessionInfo.GameToken);
	GameSession->SetGameTokenID(GameSessionInfo.GameTokenID);

	// Hand both bearers to the shared API client, which reinstalls them from here on every resolve.
	if (UCrowdyCppClientSubsystem* ClientHost = GetGameInstance()->GetSubsystem<UCrowdyCppClientSubsystem>())
	{
		ClientHost->SetManagementToken(GameSessionInfo.SessionToken);
		ClientHost->SetGameToken(GameSessionInfo.GameToken);
	}
}

void UCrowdySDKSubsystem::RequestUDPAccess() const
{
	UdpSubsystem->SetConnectionState(EUDPConnectionState::Connecting);

	// The connection is Connecting from here on, so every exit below has to answer through FailUDPAccess. A path
	// that simply returns leaves the state stuck at Connecting, which reads as a live connection while nothing is
	// listening, and the reconnect monitor keeps retrying into it.
	if (!IsValid(GameSession))
	{
		FailUDPAccess(TEXT("there is no game session"));
		return;
	}

	// The connection assigns its own server from inside itself, on its own thread, so this hands the work over
	// rather than querying an endpoint here.
	const_cast<UCrowdySDKSubsystem*>(this)->OpenRoutedConnection();
}

void UCrowdySDKSubsystem::OpenRoutedConnection()
{
	UCrowdyCppReplicationSubsystem* Replication = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UCrowdyCppReplicationSubsystem>()
		: nullptr;
	if (!Replication)
	{
		FailUDPAccess(TEXT("there is no replication routing subsystem on this game instance"));
		return;
	}

	const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>();
	if (!Settings)
	{
		FailUDPAccess(TEXT("the developer settings could not be read"));
		return;
	}

	FCrowdyCppConnectionRequest Request;
	Request.AppId = GameSession->GetAppID();
	Request.Token = GameSession->GetGameToken();
	Request.GameTokenId = GameSession->GetGameTokenID();
	Request.TokenExpiresAtIso8601 = GameSession->GetAppTokenExpiresAt();
	Request.GameApiUrl = Settings->GetGameApiHttpUrl();
	Request.DiscoveryUrl = Settings->GetDiscoveryUrl();
	Request.SilenceTimeoutSeconds = Settings->UDPTimeoutSeconds > 0.f ? Settings->UDPTimeoutSeconds : 15.f;

	// Only when IPv6 was chosen deliberately. The connection commits to one address family and does not fall back
	// to the other, so treating Auto as a preference would turn a reachability problem into a connection that never
	// comes up.
	Request.bPreferIpv6 = Settings->UDPProtocol == ECrowdyUDPProtocol::IPv6;

	TWeakObjectPtr<UCrowdySDKSubsystem> WeakThis(this);
	Request.RequestTokenRefresh = [WeakThis]()
	{
		if (const UCrowdySDKSubsystem* Self = WeakThis.Get())
		{
			if (UCrowdyAuthentication* Auth = Self->GetGameInstance()->GetSubsystem<UCrowdyAuthentication>())
			{
				Auth->RefreshAppToken();
			}
		}
	};

	// Routed through the ordinary endpoint answer rather than a second notification path, so a player is told the
	// app is full by exactly the code that tells them everything else about the connection request.
	Request.ReportAppFull = [WeakThis]()
	{
		if (UCrowdySDKSubsystem* Self = WeakThis.Get())
		{
			FCrowdyUDPEndpoint Full;
			Full.bGateKeep = true;
			Self->HandleUDPAddressNotify(Full);
		}
	};

	// The state is already Connecting, set by the request this came from. What follows is asynchronous, so the
	// outcome arrives as a connection state change rather than from here.
	if (!Replication->OpenConnection(Request))
	{
		FailUDPAccess(TEXT("the replication connection could not be opened with the current app token"));
	}
}

void UCrowdySDKSubsystem::RequestVersionInfo() const
{
	UCrowdyCppClientSubsystem* ClientHost = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UCrowdyCppClientSubsystem>()
		: nullptr;
	if (!ClientHost)
	{
		FailVersionInfo(TEXT("there is no API client host on this game instance"));
		return;
	}

	// The version query is public and has to answer before sign-in, which is exactly when a client discovers it is
	// too old to sign in at all. Installing whatever the live session currently holds covers both cases: before
	// sign-in that is empty, which sends no Authorization header, and afterwards it is never a stale bearer from an
	// account that has already left.
	ClientHost->SetGameToken(IsValid(GameSession) ? GameSession->GetGameToken() : FString());

	FCrowdyCppClient* Client = ClientHost->GetClient(ResolveClientConfig());
	if (!Client)
	{
		FailVersionInfo(TEXT("the API client could not be constructed"));
		return;
	}

	TWeakObjectPtr<const UCrowdySDKSubsystem> WeakThis(this);

	// The version query answers about a different deployment under each bearer, so the game plane is named rather
	// than left to be guessed.
	Client->RunOp(ECrowdyCppApiDomain::ServerStatus, TEXT("VersionInfo"), MakeShared<FJsonObject>(),
		[WeakThis](FCrowdyCppJsonResult Result)
		{
			const UCrowdySDKSubsystem* Self = WeakThis.Get();
			if (!IsValid(Self))
			{
				return;
			}

			const TSharedPtr<FJsonObject>* VersionInfo = nullptr;
			if (!Result.bTransportOk || !Result.Data.IsValid()
				|| !Result.Data->TryGetObjectField(TEXT("versionInfo"), VersionInfo)
				|| !VersionInfo->IsValid())
			{
				Self->FailVersionInfo(Result.bTransportOk
					? FString(TEXT("the answer carried no version information"))
					: Result.ErrorMessage);
				return;
			}

			// A version component the server omits stays zero, which is what an absent version has always meant here.
			auto ReadVersion = [&VersionInfo](const TCHAR* FieldName, FGameVersion& OutVersion)
			{
				const TSharedPtr<FJsonObject>* VersionObject = nullptr;
				if (!(*VersionInfo)->TryGetObjectField(FieldName, VersionObject) || !VersionObject->IsValid())
				{
					return;
				}
				(*VersionObject)->TryGetNumberField(TEXT("major"), OutVersion.MajorVersion);
				(*VersionObject)->TryGetNumberField(TEXT("minor"), OutVersion.MinorVersion);
				(*VersionObject)->TryGetNumberField(TEXT("patch"), OutVersion.PatchVersion);
				(*VersionObject)->TryGetNumberField(TEXT("build"), OutVersion.BuildNumber);
			};

			FGameVersion ServerVersion;
			FGameVersion MinimumClientVersion;
			ReadVersion(TEXT("serverVersion"), ServerVersion);
			ReadVersion(TEXT("minimumClientVersion"), MinimumClientVersion);

			UE::Tasks::Launch(UE_SOURCE_LOCATION, [WeakThis, ServerVersion, MinimumClientVersion]()
			{
				if (const UCrowdySDKSubsystem* Target = WeakThis.Get())
				{
					Target->OnVersionInfo.Broadcast(ServerVersion, MinimumClientVersion);
				}
			}, LowLevelTasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
		}, ECrowdyCppTokenPlane::Game);
}

FCrowdyCppClientConfig UCrowdySDKSubsystem::ResolveClientConfig()
{
	FCrowdyCppClientConfig Config;
	if (const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>())
	{
		Config.DiscoveryUrl = Settings->GetDiscoveryUrl();
		Config.ApiUrl = Settings->GetGameApiHttpUrl();
		if (Config.ApiUrl.IsEmpty())
		{
			Config.ApiUrl = Config.DiscoveryUrl;
		}
	}
	return Config;
}

void UCrowdySDKSubsystem::SetQueryEndpoint(const FString InEndpoint) const
{
	// Deprecated: use SetGameApiUrl or SetDiscoveryUrl instead. Routes to the game endpoint to preserve
	// old call-site behaviour.
	SetGameApiUrl(InEndpoint);
}

void UCrowdySDKSubsystem::SetDiscoveryUrl(const FString InUrl) const
{
	// Writing the setting is the mechanism: the shared API client re-reads both URLs every time it is resolved and
	// rebuilds itself when they differ, so the next call picks this up with nothing else to do. The value is not
	// written back to config, so it lasts for this session only.
	UCrowdySDKDeveloperSettings* Settings = GetMutableDefault<UCrowdySDKDeveloperSettings>();
	if (!Settings)
	{
		return;
	}

	Settings->DiscoveryUrl = InUrl;

	// Only the Custom backend reads this field; Dev and Production resolve to their own built-in hosts. Saying so
	// is the point: otherwise the call looks like it worked and the origin silently stays where it was.
	if (Settings->Environment != ECrowdyEnvironment::Custom)
	{
		UE_LOG(LogCrowdySDK, Warning,
			TEXT("SetDiscoveryUrl: the shared origin is built in for this mode, so the value was stored but will not be used. Switch the backend to Custom for it to take effect."));
	}
}

void UCrowdySDKSubsystem::SetGameApiUrl(const FString InHttpUrl) const
{
	// Writing the setting is the mechanism: the shared API client re-reads both endpoints every time it is
	// resolved and rebuilds itself when they differ, so the next call picks this up with nothing else to do.
	// The value is not written back to config, so it lasts for this session only.
	if (UCrowdySDKDeveloperSettings* Settings = GetMutableDefault<UCrowdySDKDeveloperSettings>())
	{
		Settings->GameApiHttpUrl = InHttpUrl;
	}
}

void UCrowdySDKSubsystem::StartUDPTimeoutMonitoring(const float ThresholdSeconds)
{
	UE_LOG(LogCrowdySDK, Warning,
		TEXT("StartUDPTimeoutMonitoring is deprecated — the connection watches its own liveness and the threshold "
		     "comes from 'UDP Timeout (seconds)' in Project Settings > Plugins > Crowdy SDK. The value passed here "
		     "is ignored; only the round-trip ping is started."));

	if (!bIsRegistered)
	{
		EnsurePingResponseSubscription();
		bIsRegistered = true;
	}

	if (!PingMessageTimerHandle.IsValid())
	{
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().SetTimer(
				PingMessageTimerHandle,
				this,
				&UCrowdySDKSubsystem::SendPingTestMessage,
				5.0f, true, 5.0f);
		}
	}
}

EUDPConnectionState UCrowdySDKSubsystem::GetUDPConnectionState() const
{
	if (!IsValid(UdpSubsystem))
		return EUDPConnectionState::Disconnected;
	return UdpSubsystem->GetConnectionState();
}

void UCrowdySDKSubsystem::StopUDPTimeoutMonitoring()
{
	if (PingMessageTimerHandle.IsValid())
		GetWorld()->GetTimerManager().ClearTimer(PingMessageTimerHandle);
}

void UCrowdySDKSubsystem::StopNetworkOperations() const
{
	UdpSubsystem->StopUDPOperations();
}

void UCrowdySDKSubsystem::ToggleNetworkMessageProcessing() const
{
	UdpSubsystem->ToggleUDPMessageProcessing();
}

void UCrowdySDKSubsystem::TriggerUdpHeartbeat() const
{
	// Does nothing. The connection tracks its own liveness from the traffic it carries, so there is no clock here
	// left for a caller to refresh.
}

void UCrowdySDKSubsystem::StartVoiceChat()
{
	if (ValidateVoiceChatSubsystem())
		VoiceChatSubsystem->StartVoiceChat();
}

void UCrowdySDKSubsystem::StopVoiceChat()
{
	if (ValidateVoiceChatSubsystem())
		VoiceChatSubsystem->StopVoiceChat();
}

void UCrowdySDKSubsystem::PlayVoiceChat()
{
	if (ValidateVoiceChatSubsystem())
		VoiceChatSubsystem->PlayVoiceChat();
}

void UCrowdySDKSubsystem::MuteVoiceChat()
{
	if (ValidateVoiceChatSubsystem())
		VoiceChatSubsystem->MuteVoiceChat();
}

void UCrowdySDKSubsystem::SetVoiceChatStreamTimeoutThreshold(const float InSeconds)
{
	if (ValidateVoiceChatSubsystem())
		VoiceChatSubsystem->SetStreamTimeoutThreshold(InSeconds);
}

void UCrowdySDKSubsystem::ToggleOwnerEcho(const bool bEnable) const
{
	VoiceChatService->ToggleOwnerEcho(bEnable);
}

void UCrowdySDKSubsystem::RequestTeleportPermission(const int64 ChunkX, const int64 ChunkY, const int64 ChunkZ,
                                                    const int32 VoxelX, const int32 VoxelY, const int32 VoxelZ) const
{
	if (!IsValid(GameSession))
	{
		FailTeleportPermission(TEXT("there is no game session"));
		return;
	}

	const int64 AppID = GameSession->GetAppID();
	const FString UUID = GameSession->GetUUID();

	// The server identifies the actor by the 32-character id it carries on the UDP wire, not by a hyphenated
	// RFC-4122 string, and it cannot answer for an app it has no id for. Both are refused here rather than sent.
	if (AppID <= 0 || UUID.Len() != 32)
	{
		FailTeleportPermission(TEXT("the session has no app id or no 32-character actor id"));
		return;
	}

	UCrowdyCppClientSubsystem* ClientHost = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UCrowdyCppClientSubsystem>()
		: nullptr;
	if (!ClientHost)
	{
		FailTeleportPermission(TEXT("there is no API client host on this game instance"));
		return;
	}

	// The bearer is refreshed from the live session on every request rather than trusting whatever the shared client
	// still holds, so a request issued after a sign-out fails instead of answering as the account that just left.
	ClientHost->SetGameToken(GameSession->GetGameToken());

	FCrowdyCppClient* Client = ClientHost->GetClient(ResolveClientConfig());
	if (!Client)
	{
		FailTeleportPermission(TEXT("the API client could not be constructed"));
		return;
	}

	// The app id and the chunk address are 64-bit, so they ride as decimal strings; the voxel address is a plain
	// 16-bit integer triple and rides as numbers.
	const TSharedPtr<FJsonObject> ChunkAddress = MakeShared<FJsonObject>();
	ChunkAddress->SetStringField(TEXT("x"), FString::Printf(TEXT("%lld"), ChunkX));
	ChunkAddress->SetStringField(TEXT("y"), FString::Printf(TEXT("%lld"), ChunkY));
	ChunkAddress->SetStringField(TEXT("z"), FString::Printf(TEXT("%lld"), ChunkZ));

	const TSharedPtr<FJsonObject> VoxelAddress = MakeShared<FJsonObject>();
	VoxelAddress->SetNumberField(TEXT("x"), static_cast<double>(VoxelX));
	VoxelAddress->SetNumberField(TEXT("y"), static_cast<double>(VoxelY));
	VoxelAddress->SetNumberField(TEXT("z"), static_cast<double>(VoxelZ));

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), AppID));
	Input->SetObjectField(TEXT("chunkAddress"), ChunkAddress);
	Input->SetObjectField(TEXT("voxelAddress"), VoxelAddress);
	Input->SetStringField(TEXT("uuid"), UUID);

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);

	TWeakObjectPtr<const UCrowdySDKSubsystem> WeakThis(this);
	Client->RunOp(ECrowdyCppApiDomain::Teleport, TEXT("TeleportRequest"), Variables,
		[WeakThis](FCrowdyCppJsonResult Result)
		{
			const UCrowdySDKSubsystem* Self = WeakThis.Get();
			if (!IsValid(Self))
			{
				return;
			}

			const TSharedPtr<FJsonObject>* TeleportObject = nullptr;
			if (!Result.bTransportOk || !Result.Data.IsValid()
				|| !Result.Data->TryGetObjectField(TEXT("teleportRequest"), TeleportObject)
				|| !TeleportObject->IsValid())
			{
				Self->FailTeleportPermission(Result.bTransportOk
					? FString(TEXT("the answer carried no teleport decision"))
					: Result.ErrorMessage);
				return;
			}

			// An answer without the flag is not a permit: the caller only ever moves the actor on an explicit yes.
			bool bAllowed = false;
			(*TeleportObject)->TryGetBoolField(TEXT("success"), bAllowed);

			UE::Tasks::Launch(UE_SOURCE_LOCATION, [WeakThis, bAllowed]()
			{
				if (const UCrowdySDKSubsystem* Target = WeakThis.Get())
				{
					Target->OnTeleportPermission.Broadcast(bAllowed);
				}
			}, LowLevelTasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
		});
}

void UCrowdySDKSubsystem::DeregisterAllReceptionLayers()
{
	// No-op: delivery now stops automatically when a subscriber's handle is destroyed, so there is
	// nothing left to release in bulk.
}

void UCrowdySDKSubsystem::SetExpectedActorUpdateStateSize(const int32 InSize) const
{
	UE_CLOG(CrowdySDKTrace::Sdk(), LogCrowdySDK, Log, TEXT("Expected Actor Update State Size: %d"), InSize);
	Parser->SetExpectedActorStateSize(InSize);
}

void UCrowdySDKSubsystem::DispatchActorUpdate(const int64 ChunkX, const int64 ChunkY, const int64 ChunkZ,
                                              const ECrowdyDecayRate DecayRate, const ECrowdyReplicationDistance ReplicationDistance,
                                              const FString& InstigatorID, const FInstancedStruct& ActorStatePayload,
                                              const FCrowdyClassID ClassID, const bool bAsync)
{
	// The lambda below exists only to own what a deferred send would otherwise read after its caller has
	// moved on, so the inline caller must not go through it: constructing it copies the id string and deep
	// copies the payload whichever branch then runs.
	if (!bAsync)
	{
		BuildAndSendActorUpdate(ChunkX, ChunkY, ChunkZ, DecayRate, ReplicationDistance,
			InstigatorID, ActorStatePayload, ClassID);
		return;
	}

	UE::Tasks::Launch(
		UE_SOURCE_LOCATION,
		[this, ChunkX, ChunkY, ChunkZ, DecayRate, ReplicationDistance, InstigatorID, ClassID,
			State = ActorStatePayload]()
		{
			BuildAndSendActorUpdate(ChunkX, ChunkY, ChunkZ, DecayRate, ReplicationDistance,
				InstigatorID, State, ClassID);
		},
		LowLevelTasks::ETaskPriority::BackgroundNormal
		);
}

void UCrowdySDKSubsystem::BuildAndSendActorUpdate(const int64 ChunkX, const int64 ChunkY, const int64 ChunkZ,
                                                  const ECrowdyDecayRate DecayRate, const ECrowdyReplicationDistance ReplicationDistance,
                                                  const FString& InstigatorID, const FInstancedStruct& ActorStatePayload,
                                                  const FCrowdyClassID ClassID)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Crowdy_DispatchActorUpdate);

	FActorUpdateRequestMessage ActorUpdateRequest;
	ActorUpdateRequest.AppID = GameSession->GetAppID();
	ActorUpdateRequest.ChunkX = ChunkX;
	ActorUpdateRequest.ChunkY = ChunkY;
	ActorUpdateRequest.ChunkZ = ChunkZ;
	ActorUpdateRequest.DecayRate = DecayRate;
	ActorUpdateRequest.ReplicationDistance = ReplicationDistance;
	ActorUpdateRequest.UUID = FCrowdyActorId::FromStringOrUnset(InstigatorID);

	// The registry lookup that used to stand here only validated into an id nothing read, and
	// SerializeActorState performs the same lookup itself. Each one builds the struct's path into an
	// FString and hashes it into an FName, so the pair cost two heap allocations per outbound update
	// to answer one question. The serializer's own failure reports it.
	if (!USerializationFunctionLibrary::SerializeActorState(ActorStatePayload, ClassID, ActorUpdateRequest.StateBytes))
	{
		UE_LOG(LogCrowdySDK, Error, TEXT("Failed to serialize actor state payload. Struct=%s Path=%s"),
			ActorStatePayload.GetScriptStruct()
				? *ActorStatePayload.GetScriptStruct()->GetName() : TEXT("null"),
			ActorStatePayload.GetScriptStruct()
				? *ActorStatePayload.GetScriptStruct()->GetPathName() : TEXT("null"));
		return;
	}

	ActorUpdateRequest.StateSize = ActorUpdateRequest.StateBytes.Num();

	SendMessage(ActorUpdateRequest);
}

void UCrowdySDKSubsystem::DispatchActorHeartbeat(const int64 ChunkX, const int64 ChunkY, const int64 ChunkZ,
                                                 const FString& InstigatorID)
{
	if (!GameSession)
	{
		return;
	}

	FActorHeartbeatRequestMessage Heartbeat;
	Heartbeat.AppID = GameSession->GetAppID();
	Heartbeat.ChunkX = ChunkX;
	Heartbeat.ChunkY = ChunkY;
	Heartbeat.ChunkZ = ChunkZ;
	Heartbeat.UUID = FCrowdyActorId::FromStringOrUnset(InstigatorID);

	SendMessage(Heartbeat);
}

void UCrowdySDKSubsystem::DispatchGameEvent_Internal(const int64 ChunkX, const int64 ChunkY, const int64 ChunkZ,
                                                     const ECrowdyDecayRate DecayRate, const ECrowdyReplicationDistance ReplicationDistance,
                                                     const FGuid& InstigatorID, FInstancedStruct EventPayload,
                                                     const ECrowdyTarget Target, const FGuid& TargetID, const bool bAsync)
{
	auto BuildAndSend = [this,
	ChunkX, ChunkY, ChunkZ,
	DecayRate, ReplicationDistance,
	InstigatorID, Target, TargetID,
	Payload = MoveTemp(EventPayload)]() mutable
	{
		// The lambda's job is ownership, and deferral when it was asked for. The send itself is the view
		// overload, so there is one description of the frame rather than two that can drift apart. Payload
		// belongs to this lambda, so the view it hands over is valid for the whole call either way.
		DispatchGameEventView_Internal(ChunkX, ChunkY, ChunkZ, DecayRate, ReplicationDistance, InstigatorID,
			Payload.GetScriptStruct(), Payload.GetMemory(), Target, TargetID);
	};

	if (bAsync)
	{
		UE::Tasks::Launch(UE_SOURCE_LOCATION, MoveTemp(BuildAndSend), LowLevelTasks::ETaskPriority::BackgroundNormal);
	}
	else
	{
		BuildAndSend();
	}
}

void UCrowdySDKSubsystem::DispatchGameEventView_Internal(const int64 ChunkX, const int64 ChunkY, const int64 ChunkZ,
	const ECrowdyDecayRate DecayRate, const ECrowdyReplicationDistance ReplicationDistance,
	const FGuid& InstigatorID, const UScriptStruct* PayloadStruct, const void* PayloadMemory,
	const ECrowdyTarget Target, const FGuid& TargetID)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Crowdy_DispatchGameEvent);

	FCrowdyTypeID EventID;
	if (!UEventPayloadRegistry::Get()->GetID(PayloadStruct, EventID))
	{
		UE_LOG(LogCrowdySDK, Error, TEXT("Event Payload not registered. %s"), *GetNameSafe(PayloadStruct));
		return;
	}

	FGameEventRequest EventRequest;

	EventRequest.AppID = GameSession->GetAppID();
	EventRequest.ChunkX = ChunkX;
	EventRequest.ChunkY = ChunkY;
	EventRequest.ChunkZ = ChunkZ;
	EventRequest.DecayRate = DecayRate;
	EventRequest.ReplicationDistance = ReplicationDistance;
	EventRequest.UUID = FCrowdyActorId::FromGuid(InstigatorID);
	EventRequest.Target = Target;
	EventRequest.TargetID = TargetID;
	EventRequest.EventType = static_cast<uint16>(EventID);

	// The request serializes the payload straight into the frame from this view, so the encoded bytes are
	// written once rather than into a buffer that is then copied in. A payload that cannot be encoded makes
	// the frame empty, which the send path refuses.
	EventRequest.PayloadStruct = PayloadStruct;
	EventRequest.PayloadMemory = PayloadMemory;
	EventRequest.PayloadTypeID = EventID;

	SendMessage(EventRequest);
}

void UCrowdySDKSubsystem::DispatchSingleActorMessage_Internal(const int64 ChunkX, const int64 ChunkY, const int64 ChunkZ,
                                                              const FGuid& TargetActorID, FInstancedStruct EventPayload, const bool bAsync)
{
	auto BuildAndSend = [this,
	ChunkX, ChunkY, ChunkZ,
	TargetActorID,
	Payload = MoveTemp(EventPayload)]() mutable
	{
		FSingleActorRequest Request;

		Request.AppID = GameSession->GetAppID();
		Request.ChunkX = ChunkX;
		Request.ChunkY = ChunkY;
		Request.ChunkZ = ChunkZ;

		// The server ignores distance/decay for a single-actor message and routes purely by the
		// destination UUID, so the chunk only locates the target and these stay at their zero values.
		Request.DecayRate = ECrowdyDecayRate::No_Decay;
		Request.ReplicationDistance = ECrowdyReplicationDistance::None;

		// UUID is the destination actor; the server delivers only to the client that owns it.
		Request.UUID = FCrowdyActorId::FromGuid(TargetActorID);

		FCrowdyTypeID EventID;
		if (!UEventPayloadRegistry::Get()->GetID(Payload.GetScriptStruct(), EventID))
		{
			UE_LOG(LogCrowdySDK, Error, TEXT("Single-actor payload not registered. %s"),
				*GetNameSafe(Payload.GetScriptStruct()));
			return;
		}

		Request.EventType = static_cast<uint16>(EventID);

		// Same view hand-off as the spatial path above, and valid for the same reason: Payload is this
		// lambda's own, so it outlives the send whether the body runs inline or on a task.
		Request.PayloadStruct = Payload.GetScriptStruct();
		Request.PayloadMemory = Payload.GetMemory();
		Request.PayloadTypeID = EventID;

		SendMessage(Request);
	};

	if (bAsync)
	{
		UE::Tasks::Launch(UE_SOURCE_LOCATION, MoveTemp(BuildAndSend), LowLevelTasks::ETaskPriority::BackgroundNormal);
	}
	else
	{
		BuildAndSend();
	}
}



PRAGMA_DISABLE_DEPRECATION_WARNINGS
PRAGMA_ENABLE_DEPRECATION_WARNINGS


DEFINE_FUNCTION(UCrowdySDKSubsystem::execK2_DispatchGameEvent)
{
	P_GET_PROPERTY(FInt64Property, Z_Param_ChunkX);
	P_GET_PROPERTY(FInt64Property, Z_Param_ChunkY);
	P_GET_PROPERTY(FInt64Property, Z_Param_ChunkZ);
	P_GET_ENUM(ECrowdyDecayRate, Z_Param_DecayRate);
	P_GET_ENUM(ECrowdyReplicationDistance, Z_Param_ReplicationDistance);
	P_GET_STRUCT_REF(FGuid, Z_Param_Out_InstigatorID);

	// Wildcard struct step manually so Blueprint can wire any struct type
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.MostRecentProperty        = nullptr;
	Stack.StepCompiledIn<FStructProperty>(nullptr);
	const void*      StructPtr  = Stack.MostRecentPropertyAddress;
	FStructProperty* StructProp = CastField<FStructProperty>(Stack.MostRecentProperty);

	P_GET_ENUM(ECrowdyTarget, Z_Param_Target);
	P_GET_STRUCT_REF(FGuid, Z_Param_Out_TargetID);
	P_GET_UBOOL(Z_Param_bAsync);
	P_FINISH;

	P_NATIVE_BEGIN;
	if (ensureMsgf(StructProp && StructPtr, TEXT("K2_DispatchGameEvent: EventPayload is not a valid struct.")))
	{
		FInstancedStruct EventPayload;
		EventPayload.InitializeAs(StructProp->Struct, static_cast<const uint8*>(StructPtr));
		P_THIS->DispatchGameEvent_Internal(
			Z_Param_ChunkX, Z_Param_ChunkY, Z_Param_ChunkZ,
			static_cast<ECrowdyDecayRate>(Z_Param_DecayRate),
			static_cast<ECrowdyReplicationDistance>(Z_Param_ReplicationDistance),
			Z_Param_Out_InstigatorID,
			MoveTemp(EventPayload),
			static_cast<ECrowdyTarget>(Z_Param_Target),
			Z_Param_Out_TargetID,
			(bool)Z_Param_bAsync);
	}
	P_NATIVE_END;
}

void UCrowdySDKSubsystem::HandleAuthLogin(FCrowdyAuthResult Result)
{
	// Never log the token itself (it is a bearer credential). Auth has already minted
	// the app token by the time this fires, so RequestUDPAccess authorizes with it.
	UE_CLOG(CrowdySDKTrace::Sdk(), LogCrowdySDK, Log, TEXT("Sign-in successful. UserID=%lld"), Result.UserID);

	RequestUDPAccess();

	OnLogin.Broadcast(true, Result.GameToken);
}

void UCrowdySDKSubsystem::HandleAuthLoginFailed(FString Message)
{
	OnLogin.Broadcast(false, FString());
}

void UCrowdySDKSubsystem::HandleAuthRegister(FCrowdyAuthResult Result)
{
	// Registration signs the player in and mints an app token, so it proceeds to
	// UDP access exactly like a login.
	RequestUDPAccess();

	OnRegister.Broadcast(true, Result.GameToken);
}

void UCrowdySDKSubsystem::HandleAuthRegisterFailed(FString Message)
{
	OnRegister.Broadcast(false, FString());
}

void UCrowdySDKSubsystem::HandleAuthSessionRestored(FCrowdyAuthResult Result)
{
	UE_CLOG(CrowdySDKTrace::Sdk(), LogCrowdySDK, Log, TEXT("Session restored. UserID=%lld"), Result.UserID);

	RequestUDPAccess();

	// A restored session is a signed-in player; surface it on the same delegate.
	OnLogin.Broadcast(true, Result.GameToken);
}

void UCrowdySDKSubsystem::HandleAppTokenRefreshed()
{
	// The app token rotated (proactive timer or expiry recovery). Re-assign the UDP
	// session so the server installs the new token/gameTokenId.
	UE_CLOG(CrowdySDKTrace::Sdk(), LogCrowdySDK, Log, TEXT("App token refreshed - re-assigning UDP session."));

	// The connection signs with material it holds, so it is handed the new token rather than rebuilt: a rebuild
	// would throw away the queued sends and the session it is already established under. A rotation that lands
	// before there is a connection still has to open one, which is what the failed install means here.
	UCrowdyCppReplicationSubsystem* Replication = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UCrowdyCppReplicationSubsystem>()
		: nullptr;

	if (Replication && IsValid(GameSession)
		&& Replication->InstallToken(GameSession->GetGameToken(), GameSession->GetGameTokenID(),
			GameSession->GetAppTokenExpiresAt()))
	{
		return;
	}

	RequestUDPAccess();
}

void UCrowdySDKSubsystem::HandleTokenExpired()
{
	if (const UGameInstance* GameInstance = GetGameInstance())
	{
		if (UCrowdyAuthentication* Auth = GameInstance->GetSubsystem<UCrowdyAuthentication>())
			Auth->RecoverExpiredAppToken();
	}
}

void UCrowdySDKSubsystem::HandleUDPAddressNotify(const FCrowdyUDPEndpoint& Endpoint)
{
	// A full app is the one answer this reports. It is an answer rather than a failure, so the player is told the
	// server was reached and has no room, instead of watching a connection retry into it forever. Everything else
	// about a connection attempt arrives as a connection state change.
	if (!Endpoint.bGateKeep)
	{
		return;
	}

	UdpSubsystem->SetConnectionState(EUDPConnectionState::GateKeep);

	// Announced on this stack, like every other outcome of a connection attempt. The answer arrives on the game
	// thread already, and deferring one of these while the others are immediate would let the game hear them in an
	// order the connection never reported them in.
	OnUDPAddressNotify.Broadcast(true, true);
}

void UCrowdySDKSubsystem::FailUDPAccess(const FString& Reason) const
{
	UE_LOG(LogCrowdySDK, Warning, TEXT("UDP access request failed: %s"), *Reason);

	// Out of Connecting before anything is told about the failure: a listener that retries straight away would
	// otherwise have its own Connecting overwritten by this failure landing afterwards, and the connection would
	// read as live while nothing was listening.
	if (IsValid(UdpSubsystem))
	{
		UdpSubsystem->SetConnectionState(EUDPConnectionState::Disconnected);
	}

	// Announced on this stack, like every other outcome of a connection attempt. Every path that reaches here runs
	// on the game thread, and deferring one of these while the others are immediate would let the game hear them in
	// an order the connection never reported them in.
	OnUDPAddressNotify.Broadcast(false, false);
}

void UCrowdySDKSubsystem::FailVersionInfo(const FString& Reason) const
{
	UE_LOG(LogCrowdySDK, Warning, TEXT("Version query failed: %s"), *Reason);

	TWeakObjectPtr<const UCrowdySDKSubsystem> WeakThis(this);
	UE::Tasks::Launch(UE_SOURCE_LOCATION, [WeakThis]()
	{
		if (const UCrowdySDKSubsystem* Self = WeakThis.Get())
		{
			Self->OnVersionInfo.Broadcast(FGameVersion(), FGameVersion());
		}
	}, LowLevelTasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
}

void UCrowdySDKSubsystem::FailTeleportPermission(const FString& Reason) const
{
	UE_LOG(LogCrowdySDK, Warning, TEXT("Teleport permission check failed: %s"), *Reason);

	TWeakObjectPtr<const UCrowdySDKSubsystem> WeakThis(this);
	UE::Tasks::Launch(UE_SOURCE_LOCATION, [WeakThis]()
	{
		if (const UCrowdySDKSubsystem* Self = WeakThis.Get())
		{
			Self->OnTeleportPermission.Broadcast(false);
		}
	}, LowLevelTasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
}

bool UCrowdySDKSubsystem::ValidateVoiceChatSubsystem()
{
	if (IsValid(VoiceChatSubsystem))
	{
		return true;
	}

	if (const UWorld* World = GetWorld())
	{
		VoiceChatSubsystem = World->GetSubsystem<UVoiceChatSubsystem>();
	}

	VoiceChatSubsystem->InitializeVoiceChatSubsystem(VoiceChatService);
	VoiceChatService->SetVoiceChatManagerReference(VoiceChatSubsystem);
	return IsValid(VoiceChatSubsystem);
}

bool UCrowdySDKSubsystem::TryLoadConfiguration()
{
	const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>();

	if (!IsValid(Settings))
	{
		UE_LOG(LogCrowdySDK, Error, TEXT("Developer Settings are invalid — check Project Settings -> Plugins -> Crowdy SDK."));
		return false;
	}

	// Apply AppID from settings to the game session (was never being applied before)
	GameSession->SetAppID(Settings->AppID);

	// The UDP protocol choice and the timeout are read fresh when a connection is opened, so a settings change in
	// the editor takes effect on the next connect without restarting.

	return true;
}

void UCrowdySDKSubsystem::OnUDPTimeout()
{
	UE_LOG(LogCrowdySDK, Warning, TEXT("The UDP connection failed — reconnecting."));

	// Stop polling while the connection is down; it restarts on its own once the connection reports itself up.
	StopHostPolling();

	// Transition state before broadcasting so UI can react instantly.
	UdpSubsystem->SetConnectionState(EUDPConnectionState::Reconnecting);

	OnUDPTimedOut.Broadcast();

	// Open the connection again. Success arrives as a connection state change, which restarts the session timers
	// and re-announces the connection.
	RequestUDPAccess();
}

void UCrowdySDKSubsystem::OnUDPConnectionSuccessful()
{
	OnUDPConnectionSuccess.Broadcast();

	// The connection is up and the player is authenticated, so join the reliable-RPC channels now; any
	// reliable sends queued before this flush once the joins complete. Idempotent across reconnects.
	if (UCrowdyChannels* Channels = GetGameInstance()->GetSubsystem<UCrowdyChannels>())
		Channels->BootstrapReliableRpcChannels();

	// The first point at which the connection is known to be usable, so this is where the timers a live session
	// needs are started and where anything waiting to hear that the connection came up is told.
	StartConnectedSessionWork();

	// Announced on this stack, like every other outcome of a connection attempt. This runs on the game thread inside
	// the drain that read the connection's state, and a failure reported later in that same drain announces itself
	// immediately, so deferring this one would tell the game the connection came up after telling it that it died.
	OnUDPAddressNotify.Broadcast(true, false);
}

void UCrowdySDKSubsystem::StartConnectedSessionWork()
{
	// SetTimer is game-thread-only. The connection coming up is reported on the game thread, so the
	// registration normally happens right here; the dispatch stays as the answer for any other caller.
	// The timer is guarded against a second start, so a reconnect does not stack it.
	auto RegisterPingTimer = [](UCrowdySDKSubsystem* Self)
	{
		if (!IsValid(Self) || Self->PingMessageTimerHandle.IsValid())
			return;

		if (UWorld* World = Self->GetWorld())
		{
			World->GetTimerManager().SetTimer(
				Self->PingMessageTimerHandle,
				Self,
				&UCrowdySDKSubsystem::SendPingTestMessage,
				5.0f, /*bLoop=*/true, /*FirstDelay=*/5.0f);
		}
	};

	if (IsInGameThread())
	{
		RegisterPingTimer(this);
	}
	else
	{
		TWeakObjectPtr<UCrowdySDKSubsystem> WeakThis(this);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, RegisterPingTimer]()
		{
			RegisterPingTimer(WeakThis.Get());
		});
	}

	// Only once the player is signed in. A connection that comes up before sign-in leaves polling to the sign-in
	// that follows, since login does not start it.
	if (IsValid(GameSession) && GameSession->GetUserID() != 0)
		StartHostPolling();
}

void UCrowdySDKSubsystem::SendPingTestMessage()
{
	// The ping is a spatial self-echo: the server only relays it back to us when it is
	// addressed to the chunk the server currently has us in. Derive that chunk live from the
	// local player's pawn location (the same transform-derived chunk the actor-update path
	// sends) instead of GameSession's cached CurrentPlayerChunkCoordinates, which is only
	// refreshed from Blueprint and goes stale across a level transition, leaving the ping
	// aimed at the previous level's chunk so no echo returns and the measured ping freezes.
	FInt64Vector ChunkCoordinate = GameSession->GetPlayerCurrentChunkCoordinates();

	const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	APawn* LocalPawn = PC ? PC->GetPawn() : nullptr;
	if (LocalPawn)
	{
		UHelperFunctions::GetChunkCoordinateAtLocation(
			LocalPawn, LocalPawn->GetActorLocation(),
			ChunkCoordinate.X, ChunkCoordinate.Y, ChunkCoordinate.Z);
	}

	UE_CLOG(CrowdySDKTrace::Sdk(), LogCrowdySDK, Log,
		TEXT("SendPingTestMessage: pawn=%s chunk=(%lld, %lld, %lld)"),
		LocalPawn ? *LocalPawn->GetName() : TEXT("<none - using stale cache>"),
		ChunkCoordinate.X, ChunkCoordinate.Y, ChunkCoordinate.Z);

	FPingTestMessage PingTestMessage;
	PingTestMessage.UUID = FCrowdyActorId::FromStringOrUnset(GameSession->GetUUID());
	PingTestMessage.AppID = GameSession->GetAppID();
	PingTestMessage.ChunkX = ChunkCoordinate.X;
	PingTestMessage.ChunkY = ChunkCoordinate.Y;
	PingTestMessage.ChunkZ = ChunkCoordinate.Z;
	PingTestMessage.DecayRate = ECrowdyDecayRate::Exponential_Decay;
	PingTestMessage.ReplicationDistance = ECrowdyReplicationDistance::One_Chunk;
	SendMessage(PingTestMessage);
}

void UCrowdySDKSubsystem::EnsurePingResponseSubscription()
{
	if (PingResponseSubscription.IsValid() || !ServiceRegistry)
	{
		return;
	}

	PingResponseSubscription = ServiceRegistry->SubscribeToOpcode<FPingTestMessage>(
		ECrowdyMessageType::GENERIC_SPATIAL_1,
		{ ECrowdySubscriptionRole::Observe, false, TEXT("CrowdySDKSubsystem") },
		[this](const FPingTestMessage& PingTestMessage, const FCrowdyDelivery&)
		{
			// An id nobody set must not match anybody. Text of the wrong length and no text at all both read
			// as the same unset id, so a ping that carries no id would otherwise be measured as our own echo
			// for as long as the local session has no id of its own.
			const FCrowdyActorId LocalSessionId = FCrowdyActorId::FromStringOrUnset(GameSession->GetUUID());
			if (!LocalSessionId.IsSet() || PingTestMessage.UUID != LocalSessionId)
			{
				return;
			}

			const int64 PingTimeMs = PingTestMessage.ReceiveTime - PingTestMessage.SendTime;
			UdpSubsystem->UpdatePingTime(PingTimeMs);
		});
}

void UCrowdySDKSubsystem::WarnNoRegistry(const FName SubscriberName)
{
	UE_LOG(LogCrowdySDK, Error,
		TEXT("'%s' tried to subscribe before the message router existed, so it will receive nothing. ")
		TEXT("Subscribe once this subsystem has initialised."),
		*SubscriberName.ToString());
}

FCrowdySubscription UCrowdySDKSubsystem::Subscribe(const TConstArrayView<FCrowdySubscriptionKey> Keys,
	const FCrowdySubscriptionOptions& Options, FCrowdyDeliveryHandler Handler) const
{
	if (!ServiceRegistry)
	{
		WarnNoRegistry(Options.SubscriberName);
		return FCrowdySubscription();
	}

	return ServiceRegistry->Subscribe(Keys, Options, MoveTemp(Handler));
}

FCrowdySubscription UCrowdySDKSubsystem::SubscribeToEventPayload(const UScriptStruct* PayloadStruct,
	const FCrowdySubscriptionOptions& Options, FCrowdyDeliveryHandler Handler) const
{
	if (!ServiceRegistry)
	{
		WarnNoRegistry(Options.SubscriberName);
		return FCrowdySubscription();
	}

	return ServiceRegistry->SubscribeToEventPayload(PayloadStruct, Options, MoveTemp(Handler));
}

FCrowdySubscription UCrowdySDKSubsystem::SubscribeToEventPayload(const FCrowdyTypeID TypeID,
	const FCrowdySubscriptionOptions& Options, FCrowdyDeliveryHandler Handler) const
{
	if (!ServiceRegistry)
	{
		WarnNoRegistry(Options.SubscriberName);
		return FCrowdySubscription();
	}

	return ServiceRegistry->SubscribeToEventPayload(TypeID, Options, MoveTemp(Handler));
}

FCrowdySubscription UCrowdySDKSubsystem::SubscribeToActorUpdatePayload(const UScriptStruct* PayloadStruct,
	const FCrowdySubscriptionOptions& Options, FCrowdyDeliveryHandler Handler) const
{
	if (!ServiceRegistry)
	{
		WarnNoRegistry(Options.SubscriberName);
		return FCrowdySubscription();
	}

	return ServiceRegistry->SubscribeToActorUpdatePayload(PayloadStruct, Options, MoveTemp(Handler));
}

FCrowdySubscription UCrowdySDKSubsystem::SubscribeToActorUpdatePayload(const FCrowdyTypeID TypeID,
	const FCrowdySubscriptionOptions& Options, FCrowdyDeliveryHandler Handler) const
{
	if (!ServiceRegistry)
	{
		WarnNoRegistry(Options.SubscriberName);
		return FCrowdySubscription();
	}

	return ServiceRegistry->SubscribeToActorUpdatePayload(TypeID, Options, MoveTemp(Handler));
}

FCrowdySubscription UCrowdySDKSubsystem::SubscribeToAllPayloads(const ECrowdyPayloadCategory Category,
	const FCrowdySubscriptionOptions& Options, FCrowdyDeliveryHandler Handler) const
{
	if (!ServiceRegistry)
	{
		WarnNoRegistry(Options.SubscriberName);
		return FCrowdySubscription();
	}

	return ServiceRegistry->SubscribeToAllPayloads(Category, Options, MoveTemp(Handler));
}

FCrowdySubscription UCrowdySDKSubsystem::SubscribeToOpcode(const ECrowdyMessageType MessageType,
	const FCrowdySubscriptionOptions& Options, FCrowdyDeliveryHandler Handler) const
{
	if (!ServiceRegistry)
	{
		WarnNoRegistry(Options.SubscriberName);
		return FCrowdySubscription();
	}

	return ServiceRegistry->SubscribeToOpcode(MessageType, Options, MoveTemp(Handler));
}

void UCrowdySDKSubsystem::SendMessage(const ICrowdyMessage& Message) const
{
	if (!Message.UUID.IsSet())
	{
		// An actor id of all zeroes addresses no actor, so this message can reach nobody. It is reported
		// rather than refused because a send before the local session has an id is expected during connect,
		// and it is reported on a timer because this funnel carries every outbound message, several times a
		// second on a replication path.
		constexpr double UnsetActorIdReportIntervalSeconds = 5.0;

		static std::atomic<int64> UnsetActorIdSends{0};
		static std::atomic<double> NextUnsetActorIdReportSeconds{0.0};

		const int64 TotalSoFar = UnsetActorIdSends.fetch_add(1, std::memory_order_relaxed) + 1;
		const double Now = FPlatformTime::Seconds();

		double Deadline = NextUnsetActorIdReportSeconds.load(std::memory_order_relaxed);
		if (Now >= Deadline
			&& NextUnsetActorIdReportSeconds.compare_exchange_strong(Deadline, Now + UnsetActorIdReportIntervalSeconds,
				std::memory_order_relaxed))
		{
			UE_LOG(LogCrowdySDK, Warning,
				TEXT("Sending '%s' with an unset actor id - it addresses no actor. %lld such sends so far; ")
				TEXT("further reports are suppressed for %.0f seconds."),
				*Message.GetTypeName().ToString(), TotalSoFar, UnsetActorIdReportIntervalSeconds);
		}
	}

	// The single funnel for every outbound message. There is one connection and nothing else to hand a message to,
	// so an outcome other than Sent means it was dropped, with a reason logged at the point it was dropped.
	if (const UGameInstance* Instance = GetGameInstance())
	{
		if (UCrowdyCppReplicationSubsystem* Replication = Instance->GetSubsystem<UCrowdyCppReplicationSubsystem>())
		{
			Replication->TrySendMessage(Message);
			return;
		}
	}

	// Said once rather than once per message: this funnel runs several times a second, so a per-message report would
	// bury the first one that mattered.
	static std::atomic<bool> bReportedMissingReplication{false};
	if (!bReportedMissingReplication.exchange(true))
	{
		UE_LOG(LogCrowdySDK, Warning, TEXT("Dropped a '%s': there is no replication subsystem to send it through."),
			*Message.GetTypeName().ToString());
	}
}

void UCrowdySDKSubsystem::StartHostPolling() const
{
	// SetTimer must run on the game thread. Every caller in the SDK is already on it, so the registration
	// usually happens right here; the dispatch stays because this is callable from anywhere.
	auto RegisterPollTimer = [](UCrowdySDKSubsystem* Self)
	{
		if (!IsValid(Self))
			return;

		// Don't double-start if already running.
		if (Self->HostPollTimerHandle.IsValid())
			return;

		const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>();
		const float Interval = (Settings && Settings->HostPollIntervalSeconds > 0.f)
			? Settings->HostPollIntervalSeconds
			: 5.f;

		if (UWorld* World = Self->GetWorld())
		{
			World->GetTimerManager().SetTimer(
				Self->HostPollTimerHandle,
				Self,
				&UCrowdySDKSubsystem::PollGameHost,
				Interval, /*bLoop=*/true, /*FirstDelay=*/0.f); // fire immediately, then loop
		}

		UE_CLOG(CrowdySDKTrace::Sdk(), LogCrowdySDK, Log, TEXT("GameHost polling started (interval=%.1fs)"), Interval);
	};

	if (IsInGameThread())
	{
		RegisterPollTimer(const_cast<UCrowdySDKSubsystem*>(this));
	}
	else
	{
		TWeakObjectPtr<UCrowdySDKSubsystem> WeakThis(const_cast<UCrowdySDKSubsystem*>(this));
		AsyncTask(ENamedThreads::GameThread, [WeakThis, RegisterPollTimer]()
		{
			RegisterPollTimer(WeakThis.Get());
		});
	}
}

void UCrowdySDKSubsystem::StopHostPolling()
{
	if (HostPollTimerHandle.IsValid())
	{
		if (UWorld* World = GetWorld())
			World->GetTimerManager().ClearTimer(HostPollTimerHandle);

		UE_CLOG(CrowdySDKTrace::Sdk(), LogCrowdySDK, Log, TEXT("GameHost polling stopped."));
	}
}

void UCrowdySDKSubsystem::PollGameHost() const
{
	if (!IsValid(GameSession))
		return;

	// The bearer is refreshed from the live session on every poll rather than trusting whatever the shared client
	// still holds, so a poll issued after a sign-out fails instead of answering as the account that just left.
	UCrowdyCppClientSubsystem* ClientHost = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UCrowdyCppClientSubsystem>()
		: nullptr;
	if (!ClientHost)
	{
		UE_LOG(LogCrowdySDK, Warning, TEXT("No API client host on this game instance; the host poll cannot run."));
		return;
	}

	ClientHost->SetGameToken(GameSession->GetGameToken());

	FCrowdyCppClient* Client = ClientHost->GetClient(ResolveClientConfig());
	if (!Client)
	{
		UE_LOG(LogCrowdySDK, Warning, TEXT("Could not construct the API client; the host poll cannot run."));
		return;
	}

	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), GameSession->GetAppID()));

	TWeakObjectPtr<const UCrowdySDKSubsystem> WeakThis(this);
	Client->RunOp(ECrowdyCppApiDomain::Host, TEXT("GameHost"), Variables,
		[WeakThis](FCrowdyCppJsonResult Result)
		{
			const TSharedPtr<FJsonObject>* GameHost = nullptr;
			if (!Result.bTransportOk || !Result.Data.IsValid()
				|| !Result.Data->TryGetObjectField(TEXT("gameHost"), GameHost))
			{
				return;
			}

			// hostUserId is a BigInt, so it arrives as a decimal string. An answer that does not carry one has not
			// named a host, and applying the zero it would otherwise parse to elects a user nobody is: the id
			// still hashes to a valid-looking host GUID, so every client would see a host set and none would
			// claim authority.
			FString HostUserIdText;
			if (!(*GameHost)->TryGetStringField(TEXT("hostUserId"), HostUserIdText))
			{
				UE_LOG(LogCrowdySDK, Warning, TEXT("GameHost answered without a hostUserId; leaving the host as it was."));
				return;
			}
			const int64 HostUserId = FCString::Atoi64(*HostUserIdText);

			// Saturating rather than cast: a non-finite or out-of-range number converts to garbage, not to a bound.
			double ActorCountValue = 0.0;
			(*GameHost)->TryGetNumberField(TEXT("actorCount"), ActorCountValue);
			const int32 ActorCount = FMath::IsFinite(ActorCountValue)
				? static_cast<int32>(FMath::Clamp(ActorCountValue,
					static_cast<double>(MIN_int32), static_cast<double>(MAX_int32)))
				: 0;

			FString EarliestActorJoinedAt;
			(*GameHost)->TryGetStringField(TEXT("earliestActorJoinedAt"), EarliestActorJoinedAt);

			const UCrowdySDKSubsystem* Self = WeakThis.Get();
			UWorld* World = Self ? Self->GetWorld() : nullptr;
			UCrowdyHostSubsystem* HostSubsystem = World ? World->GetSubsystem<UCrowdyHostSubsystem>() : nullptr;
			if (!IsValid(HostSubsystem) || !HostSubsystem->IsReady())
				return;

			HostSubsystem->SetHostUserID(HostUserId);

			UE_CLOG(CrowdySDKTrace::Sdk(), LogCrowdySDK, Log,
				TEXT("GameHost | HostUserID=%lld | LocalUserID=%lld | IsHost=%s | ActorCount=%d | EarliestJoined=%s"),
				HostUserId,
				IsValid(Self->GameSession) ? Self->GameSession->GetUserID() : 0,
				HostSubsystem->IsHost() ? TEXT("YES") : TEXT("no"),
				ActorCount,
				*EarliestActorJoinedAt);
		});
}
