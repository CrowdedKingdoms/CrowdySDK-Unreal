// Fill out your copyright notice in the Description page of Project Settings.


#include "Subsystem/CrowdySDKSubsystem.h"
#include "CrowdySDKLog.h"
#include "Core/Audio/VoiceChat/VoiceChatSubsystem.h"
#include "Core/Audio/VoiceChat/Service/FVoiceChatService.h"
#include "Core/GraphQL/Enums/EQueryResponseType.h"
#include "Core/GraphQL/Interfaces/ICrowdyQueryRequest.h"
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"
#include "Core/GraphQL/Interfaces/ICrowdyQueryTransmissionLayer.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Internal/FCrowdyServiceRegistry.h"
#include "Internal/FCrowdyDataRegistry.h"
#include "Messages/FPingTestMessage.h"
#include "Messages/Actor/FActorUpdateRequestMessage.h"
#include "Messages/GameObjects/FGameEventRequest.h"
#include "Messages/GameObjects/FSingleActorMessage.h"
#include "Network/GraphQL/CrowdyQuerySubsystem.h"
#include "Network/UDP/FCrowdyTransmissionLayerUDP.h"
#include "Serialization/FCrowdyMessageParser.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Subsystem/CrowdyWorkerThreadsSubsystem.h"
#include "Utils/FMessageBufferPool.h"
#include "Network/GraphQL/FCrowdyQueryTransmissionLayerGQL.h"
#include "Network/UDP/CrowdyUDPSubsystem.h"
#include "Queries/Data/Version/FVersionInfoRequest.h"
#include "Queries/Data/Version/FVersionInfoResponse.h"
#include "Queries/Permissions/FTeleportRequest.h"
#include "Queries/Permissions/FTeleportResponse.h"
#include "Queries/UDP/FUDPAddressNotify.h"
#include "Queries/UDP/FUDPAddressRequest.h"
#include "Queries/Data/GameHost/FGameHostRequest.h"
#include "Queries/Data/GameHost/FGameHostResponse.h"
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

void UCrowdySDKSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Collection.InitializeDependency<UCrowdyAutoRegistry>();
	
	Super::Initialize(Collection);
	
	// Dependencies first
	GameSession            = Collection.InitializeDependency<UCrowdyGameSession>();
	WorkerThreadsSubsystem = Collection.InitializeDependency<UCrowdyWorkerThreadsSubsystem>();
	QuerySubsystem         = Collection.InitializeDependency<UCrowdyQuerySubsystem>();
	UdpSubsystem           = Collection.InitializeDependency<UCrowdyUDPSubsystem>();
	PersistenceSubsystem   = Collection.InitializeDependency<UCrowdyPersistenceSubsystem>();
	UCrowdyAvatars* CrowdyAvatars = Collection.InitializeDependency<UCrowdyAvatars>();
	UCrowdyTeams* CrowdyTeams = Collection.InitializeDependency<UCrowdyTeams>();
	UCrowdyChannels* CrowdyChannels = Collection.InitializeDependency<UCrowdyChannels>();
	UCrowdyAuthentication* CrowdyAuth = Collection.InitializeDependency<UCrowdyAuthentication>();
	
	
	
	// Pure allocations (no world required)
	ServiceRegistry = new FCrowdyServiceRegistry();
	DataRegistry    = new FCrowdyDataRegistry();
	Parser          = new FCrowdyMessageParser(ServiceRegistry, UdpSubsystem, [this]{ TriggerUdpHeartbeat(); }, GameSession, [this]{ HandleTokenExpired(); });
	BufferPool      = new FMessageBufferPool();

	if (UCrowdySDKBridgeSubsystem* BridgeSub = GetGameInstance()->GetSubsystem<UCrowdySDKBridgeSubsystem>())
	{
		BridgeSub->ServiceRegistry = ServiceRegistry;
		BridgeSub->DataRegistry    = DataRegistry;
		BridgeSub->DispatchActorUpdateFn = [this](int64 X, int64 Y, int64 Z, ECrowdyDecayRate D, ECrowdyReplicationDistance R, const FString& ID, const FInstancedStruct& State, bool bAsync)
		{
			DispatchActorUpdate(X, Y, Z, D, R, ID, State, bAsync);
		};
		BridgeSub->DispatchGameEventFn = [this](int64 X, int64 Y, int64 Z, ECrowdyDecayRate D, ECrowdyReplicationDistance R, const FGuid& ID, FInstancedStruct Payload, ECrowdyTarget Target, const FGuid& TargetID, bool bAsync)
		{
			DispatchGameEvent_Internal(X, Y, Z, D, R, ID, MoveTemp(Payload), Target, TargetID, bAsync);
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
		BridgeSub->ExecuteQueryFn = [this](ICrowdyQueryRequest& Req)
		{
			ExecuteQuery(Req);
		};
		BridgeSub->BroadcastHUDReadyFn = [this]()
		{
			OnCrowdyHUDReady.Broadcast();
		};
		BridgeSub->ReloadConfigFn = [this]()
		{
			// Live re-apply of the developer settings so an editor Config Sync reaches a running
			// session. Endpoints feed subsequent GraphQL; AppID/UDP protocol feed subsequent ops.
			// The current UDP socket / login keep the session they were established with.
			ApplyEndpointsFromSettings();
			const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>();
			if (Settings && IsValid(GameSession))
			{
				GameSession->SetAppID(Settings->AppID);
			}
			if (Settings && IsValid(UdpSubsystem))
			{
				UdpSubsystem->SetPreferredProtocol(Settings->UDPProtocol);
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
	TransmissionLayer      = new FCrowdyTransmissionLayerUDP(GameSession);
	QueryTransmissionLayer = new FCrowdyQueryTransmissionLayerGQL(QuerySubsystem);
	VoiceChatService       = new FVoiceChatService(ServiceRegistry, GameSession, [this](const ICrowdyMessage& Msg) { SendMessage(Msg); });
	PersistenceSubsystem->InitialSetup(GetGameInstance()->GetSubsystem<UCrowdySDKBridgeSubsystem>(), GameSession);
	
	auto LogNull = [](const TCHAR* Name)
	{
		UE_LOG(LogCrowdySDK, Error, TEXT("%s is null or invalid"), Name);
	};

	bool bFailed = false;
	bFailed |= !ServiceRegistry && (LogNull(TEXT("ServiceRegistry")), true);
	bFailed |= !DataRegistry && (LogNull(TEXT("DataRegistry")), true);
	bFailed |= !Parser && (LogNull(TEXT("Parser")), true);
	bFailed |= !TransmissionLayer && (LogNull(TEXT("TransmissionLayer")), true);
	bFailed |= !QueryTransmissionLayer && (LogNull(TEXT("QueryTransmissionLayer")), true);
	bFailed |= !BufferPool && (LogNull(TEXT("BufferPool")), true);
	bFailed |= !IsValid(WorkerThreadsSubsystem) && (LogNull(TEXT("WorkerThreadsSubsystem")), true);
	bFailed |= !IsValid(QuerySubsystem) && (LogNull(TEXT("QuerySubsystem")), true);
	bFailed |= !IsValid(UdpSubsystem) && (LogNull(TEXT("UdpSubsystem")), true);
	bFailed |= !IsValid(GameSession) && (LogNull(TEXT("GameSession")), true);
	bFailed |= !VoiceChatService && (LogNull(TEXT("VoiceChatService")), true);

	if (bFailed) return;

	WorkerThreadsSubsystem->InitializeWorkerThreadPool(BufferPool, Parser, ServiceRegistry, UdpSubsystem, GameSession);
	QuerySubsystem->InitializeQuerySubsystem(DataRegistry);

	ApplyEndpointsFromSettings();

	RegisterQueryReceptionLayer(this);

	// Always register for UDP messages so the SDK can handle ping responses
	// immediately once the socket opens — no longer lazily deferred.
	RegisterReceptionLayer(this);
	bIsRegistered = true;

	UdpSubsystem->OnUDPConnectionSuccessful.AddDynamic(this, &UCrowdySDKSubsystem::OnUDPConnectionSuccessful);
	UdpSubsystem->OnUDPTimeout.AddDynamic(this, &UCrowdySDKSubsystem::OnUDPTimeout);

	if (!TryLoadConfiguration())
		UE_LOG(LogCrowdySDK, Error, TEXT("Failed to load configuration from Developer Settings."));
	
	UE_CLOG(CrowdySDKTrace::Sdk(), LogCrowdySDK, Log, TEXT("CrowdySDK Subsystem Initialized (PostWorldInit)"));
	
	CrowdyAvatars->InjectDependencies(DataRegistry, QuerySubsystem);
	CrowdyTeams->InjectDependencies(DataRegistry, QuerySubsystem);
	CrowdyChannels->InjectDependencies(DataRegistry, QuerySubsystem);
	// Inbound channel notifications (type 18) flow through the service registry to this layer.
	RegisterReceptionLayer(CrowdyChannels);
	CrowdyAuth->InjectDependencies(DataRegistry, QuerySubsystem, GameSession);

	CrowdyAuth->OnLogin.AddDynamic(this, &UCrowdySDKSubsystem::HandleAuthLogin);
	CrowdyAuth->OnLoginFailed.AddDynamic(this, &UCrowdySDKSubsystem::HandleAuthLoginFailed);
	CrowdyAuth->OnRegister.AddDynamic(this, &UCrowdySDKSubsystem::HandleAuthRegister);
	CrowdyAuth->OnRegisterFailed.AddDynamic(this, &UCrowdySDKSubsystem::HandleAuthRegisterFailed);
	CrowdyAuth->OnSessionRestored.AddDynamic(this, &UCrowdySDKSubsystem::HandleAuthSessionRestored);
	CrowdyAuth->OnAppTokenRefreshed.AddDynamic(this, &UCrowdySDKSubsystem::HandleAppTokenRefreshed);
}

void UCrowdySDKSubsystem::Deinitialize()
{
	// Stop all repeating timers before anything else is torn down.
	StopHostPolling();

	FWorldDelegates::OnPostWorldInitialization.RemoveAll(this);

	if (UCrowdySDKBridgeSubsystem* B = GetGameInstance()->GetSubsystem<UCrowdySDKBridgeSubsystem>())
	{
		B->ServiceRegistry       = nullptr;
		B->DataRegistry          = nullptr;
		B->DispatchActorUpdateFn  = nullptr;
		B->DispatchGameEventFn    = nullptr;
		B->PublishReliableRpcFn   = nullptr;
		B->SendMessageFn          = nullptr;
		B->ExecuteQueryFn         = nullptr;
		B->BroadcastHUDReadyFn    = nullptr;
		B->ReloadConfigFn         = nullptr;
	}

	ServiceRegistry = nullptr;
	Parser = nullptr;
	TransmissionLayer = nullptr;
	UEventPayloadRegistry::Get()->Reset();
	UEventPayloadRegistry::Get()->Shutdown();
	UActorUpdatePayloadRegistry::Get()->Reset();
	UActorUpdatePayloadRegistry::Get()->Shutdown();
	Super::Deinitialize();
}

void UCrowdySDKSubsystem::ApplyEndpointsFromSettings()
{
	const UCrowdySDKDeveloperSettings* DeveloperSettings = GetDefault<UCrowdySDKDeveloperSettings>();
	if (DeveloperSettings && IsValid(QuerySubsystem))
	{
		QuerySubsystem->SetManagementEndpoint(DeveloperSettings->GetManagementApiUrl());
		QuerySubsystem->SetGameEndpoint(DeveloperSettings->GetGameApiHttpUrl());
	}
}

void UCrowdySDKSubsystem::ReloadEndpointsFromSettings()
{
	ApplyEndpointsFromSettings();
	UE_CLOG(CrowdySDKTrace::Sdk(), LogCrowdySDK, Log, TEXT("Reloaded GraphQL endpoints from developer settings."));
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

void UCrowdySDKSubsystem::DevLogin(const FString Email) const
{
	if (UCrowdyAuthentication* Auth = GetGameInstance()->GetSubsystem<UCrowdyAuthentication>())
		Auth->DevLogin(Email, FOnAuthSuccess(), FOnAuthError());
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

	QuerySubsystem->ClearAuthToken();
	GameSession->ClearCurrentSessionData();

	UCrowdyHostSubsystem* HostSS = GetWorld()->GetSubsystem<UCrowdyHostSubsystem>();
	HostSS->SetHostUserID(0);
	
	// Stop host polling — user is no longer authenticated.
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
	QuerySubsystem->SetSessionToken(GameSessionInfo.SessionToken);

	// Gameplay plane: the app-scoped token (Game API / UDP). Kept distinct from the
	// session token so a management credential never authorizes gameplay.
	GameSession->SetGameToken(GameSessionInfo.GameToken);
	GameSession->SetGameTokenID(GameSessionInfo.GameTokenID);
	QuerySubsystem->SetAppToken(GameSessionInfo.GameToken);
}

void UCrowdySDKSubsystem::RequestUDPAccess() const
{
	UdpSubsystem->SetConnectionState(EUDPConnectionState::Connecting);
	FUDPAddressRequest UDPAddressRequest;
	UDPAddressRequest.PrepareQuery();
	ExecuteQuery(UDPAddressRequest);
}

void UCrowdySDKSubsystem::RequestVersionInfo() const
{
	FVersionInfoRequest VersionInfoRequest;
	VersionInfoRequest.PrepareQuery();
	ExecuteQuery(VersionInfoRequest);
}

void UCrowdySDKSubsystem::SetQueryEndpoint(const FString InEndpoint) const
{
	// Deprecated — use SetManagementApiUrl / SetGameApiUrl instead.
	// Routes to the Game endpoint to preserve old call-site behaviour.
	QuerySubsystem->SetGameEndpoint(InEndpoint);
}

void UCrowdySDKSubsystem::SetManagementApiUrl(const FString InUrl) const
{
	QuerySubsystem->SetManagementEndpoint(InUrl);
}

void UCrowdySDKSubsystem::SetGameApiUrl(const FString InHttpUrl) const
{
	QuerySubsystem->SetGameEndpoint(InHttpUrl);
}

void UCrowdySDKSubsystem::StartUDPTimeoutMonitoring(const float ThresholdSeconds)
{
	UE_LOG(LogCrowdySDK, Warning,
		TEXT("StartUDPTimeoutMonitoring is deprecated — timeout monitoring "
		     "now starts automatically after the UDP socket connects. "
		     "Set 'UDP Timeout (seconds)' in Project Settings > Plugins > Crowdy SDK."));

	// Still functional as a runtime override (e.g. dynamic threshold changes).
	//UdpSubsystem->StartTimeoutMonitoring(ThresholdSeconds);

	if (!bIsRegistered)
	{
		RegisterReceptionLayer(this);
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
	//UdpSubsystem->StopTimeoutMonitoring();
	
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
	UdpSubsystem->OnMessageReceived();
}

void UCrowdySDKSubsystem::StartVoiceChat()
{
	if (ValidateVoiceChatSubsystem())
		VoiceChatSubsystem->StartVoiceChat();
	
	if (!IsLayerRegistered(VoiceChatService))
		RegisterReceptionLayer(VoiceChatService);
}

void UCrowdySDKSubsystem::StopVoiceChat()
{
	if (ValidateVoiceChatSubsystem())
		VoiceChatSubsystem->StopVoiceChat();
	
	if (!IsLayerRegistered(VoiceChatService))
		RegisterReceptionLayer(VoiceChatService);
}

void UCrowdySDKSubsystem::PlayVoiceChat()
{
	if (ValidateVoiceChatSubsystem())
		VoiceChatSubsystem->PlayVoiceChat();
	
	if (!IsLayerRegistered(VoiceChatService))
		RegisterReceptionLayer(VoiceChatService);
}

void UCrowdySDKSubsystem::MuteVoiceChat()
{
	if (ValidateVoiceChatSubsystem())
		VoiceChatSubsystem->MuteVoiceChat();
	
	if (!IsLayerRegistered(VoiceChatService))
		RegisterReceptionLayer(VoiceChatService);
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
	FTeleportRequest TeleportRequest;
	TeleportRequest.MapID = GameSession->GetAppID();
	TeleportRequest.UUID = GameSession->GetUUID();
	TeleportRequest.ChunkX = ChunkX;
	TeleportRequest.ChunkY = ChunkY;
	TeleportRequest.ChunkZ = ChunkZ;
	TeleportRequest.VoxelX = VoxelX;
	TeleportRequest.VoxelY = VoxelY;
	TeleportRequest.VoxelZ = VoxelZ;
	TeleportRequest.PrepareQuery();
	ExecuteQuery(TeleportRequest);
}

void UCrowdySDKSubsystem::DeregisterAllReceptionLayers()
{
	ServiceRegistry->DeregisterAllReceptionLayers();
	bIsRegistered = false;
}

void UCrowdySDKSubsystem::SetExpectedActorUpdateStateSize(const int32 InSize) const
{
	UE_CLOG(CrowdySDKTrace::Sdk(), LogCrowdySDK, Log, TEXT("Expected Actor Update State Size: %d"), InSize);
	Parser->SetExpectedActorStateSize(InSize);
}

void UCrowdySDKSubsystem::DispatchActorUpdate(const int64 ChunkX, const int64 ChunkY, const int64 ChunkZ,
                                              const ECrowdyDecayRate DecayRate, const ECrowdyReplicationDistance ReplicationDistance,
                                              const FString& InstigatorID, UPARAM(ref) const FInstancedStruct& ActorStatePayload, const bool bAsync)
{
	auto BuildAndDispatch = [this, ChunkX, ChunkY, ChunkZ, DecayRate, ReplicationDistance, InstigatorID, 
		
	ActorStatePayload = ActorStatePayload]() mutable
	{
		FActorUpdateRequestMessage ActorUpdateRequest;
		ActorUpdateRequest.AppID = GameSession->GetAppID();
		ActorUpdateRequest.ChunkX = ChunkX;
		ActorUpdateRequest.ChunkY = ChunkY;
		ActorUpdateRequest.ChunkZ = ChunkZ;
		ActorUpdateRequest.DecayRate = DecayRate;
		ActorUpdateRequest.ReplicationDistance = ReplicationDistance;
		ActorUpdateRequest.UUID = InstigatorID;
		
		FCrowdyTypeID StateID;
		
		if (!UActorUpdatePayloadRegistry::Get()->GetID(ActorStatePayload.GetScriptStruct(), StateID))
		{
			UE_LOG(LogCrowdySDK, Error,
		TEXT("Actor Update Payload not registered. Struct=%s Path=%s"),
		ActorStatePayload.GetScriptStruct()
			? *ActorStatePayload.GetScriptStruct()->GetName() : TEXT("null"),
		ActorStatePayload.GetScriptStruct()
			? *ActorStatePayload.GetScriptStruct()->GetPathName() : TEXT("null"));
			return;
		}
		
		if (!USerializationFunctionLibrary::SerializeActorState(ActorStatePayload, ActorUpdateRequest.StateBytes))
		{
			UE_LOG(LogCrowdySDK, Error, TEXT("Failed to serialize actor state payload."));
			return;
		}
		
		ActorUpdateRequest.StateSize = ActorUpdateRequest.StateBytes.Num();
		
		SendMessage(ActorUpdateRequest);
	};
	
	if (bAsync)
	{
		UE::Tasks::Launch(
			UE_SOURCE_LOCATION, 
			MoveTemp(BuildAndDispatch), 
			LowLevelTasks::ETaskPriority::BackgroundNormal
			);
	}
	else
	{
		BuildAndDispatch();
	}
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
		FGameEventRequest EventRequest;

		EventRequest.AppID = GameSession->GetAppID();
		EventRequest.ChunkX = ChunkX;
		EventRequest.ChunkY = ChunkY;
		EventRequest.ChunkZ = ChunkZ;
		EventRequest.DecayRate = DecayRate;
		EventRequest.ReplicationDistance = ReplicationDistance;
		EventRequest.UUID = InstigatorID.ToString(EGuidFormats::Digits);
		EventRequest.Target = Target;
		EventRequest.TargetID = TargetID;

		FCrowdyTypeID EventID;

		if (!UEventPayloadRegistry::Get()->GetID(Payload.GetScriptStruct(), EventID))
		{
			UE_LOG(LogCrowdySDK, Error, TEXT("Event Payload not registered. %s"), *Payload.GetScriptStruct()->GetName());
			return;
		}

		EventRequest.EventType = static_cast<uint16>(EventID);

		if (!USerializationFunctionLibrary::SerializeEventState(Payload, EventRequest.StateBytes))
		{
			UE_LOG(LogCrowdySDK, Error, TEXT("Failed to serialize event payload."));
			return;
		}

		EventRequest.StateSize = EventRequest.StateBytes.Num();

		SendMessage(EventRequest);
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
		Request.UUID = TargetActorID.ToString(EGuidFormats::Digits);

		FCrowdyTypeID EventID;
		if (!UEventPayloadRegistry::Get()->GetID(Payload.GetScriptStruct(), EventID))
		{
			UE_LOG(LogCrowdySDK, Error, TEXT("Single-actor payload not registered. %s"),
				*GetNameSafe(Payload.GetScriptStruct()));
			return;
		}

		Request.EventType = static_cast<uint16>(EventID);

		if (!USerializationFunctionLibrary::SerializeEventState(Payload, Request.StateBytes))
		{
			UE_LOG(LogCrowdySDK, Error, TEXT("Failed to serialize single-actor payload."));
			return;
		}

		Request.StateSize = Request.StateBytes.Num();

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

	// Wildcard struct — step manually so Blueprint can wire any struct type
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
	RequestUDPAccess();
}

void UCrowdySDKSubsystem::HandleTokenExpired()
{
	// Called from the UDP receive worker thread; marshal to the game thread before
	// touching the auth subsystem (timers, delegates, query dispatch).
	TWeakObjectPtr<UCrowdySDKSubsystem> WeakThis(this);
	AsyncTask(ENamedThreads::GameThread, [WeakThis]()
	{
		UCrowdySDKSubsystem* Self = WeakThis.Get();
		if (!IsValid(Self))
			return;
		if (UCrowdyAuthentication* Auth = Self->GetGameInstance()->GetSubsystem<UCrowdyAuthentication>())
			Auth->RecoverExpiredAppToken();
	});
}

void UCrowdySDKSubsystem::HandleUDPAddressNotify(const FUDPAddressNotify& UDPAddressNotify)
{
	if (UDPAddressNotify.bGateKeep)
	{
		UdpSubsystem->SetConnectionState(EUDPConnectionState::GateKeep);

		UE::Tasks::Launch(UE_SOURCE_LOCATION, [this]()
		{
			OnUDPAddressNotify.Broadcast(true, true);
		}, LowLevelTasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);

		return;
	}

	const bool bSuccess = UdpSubsystem->InitializeUDP(UDPAddressNotify);

	if (!bSuccess)
	{
		// Socket init failed — go back to Disconnected so the UI doesn't
		// show "Connecting" forever. The caller can retry via RequestUDPAccess.
		UdpSubsystem->SetConnectionState(EUDPConnectionState::Disconnected);
	}

	if (bSuccess)
	{
		// Auto-start timeout monitoring
		// Read the threshold fresh from settings each time so editor tweaks
		// take effect without restarting the game.
		const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>();
		const float TimeoutSecs = (Settings && Settings->UDPTimeoutSeconds > 0.f)
			? Settings->UDPTimeoutSeconds
			: 15.f;

		UdpSubsystem->StartTimeoutMonitoring(TimeoutSecs);

		// Auto-start keep-alive ping 
		// SetTimer is game-thread-only; this entire function may be called
		// from a UE::Tasks worker, so dispatch to the game thread explicitly.
		TWeakObjectPtr<UCrowdySDKSubsystem> WeakThis(this);
		AsyncTask(ENamedThreads::GameThread, [WeakThis]()
		{
			UCrowdySDKSubsystem* Self = WeakThis.Get();
			if (!IsValid(Self))
				return;

			// Only starts once; persists across reconnects so no double-timer.
			if (!Self->PingMessageTimerHandle.IsValid())
			{
				if (UWorld* World = Self->GetWorld())
				{
					World->GetTimerManager().SetTimer(
						Self->PingMessageTimerHandle,
						Self,
						&UCrowdySDKSubsystem::SendPingTestMessage,
						5.0f, /*bLoop=*/true, /*FirstDelay=*/5.0f);
				}
			}
		});

		// Begin host polling now that UDP is live but only if we're already
		// logged in (UserID > 0). If this is a reconnection before login, polling
		// will be skipped here and will never start (login no longer starts it).
		if (GameSession->GetUserID() != 0)
			StartHostPolling();
	}

	UE::Tasks::Launch(UE_SOURCE_LOCATION, [this, bSuccess]()
	{
		OnUDPAddressNotify.Broadcast(bSuccess, false);
	}, LowLevelTasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
}

void UCrowdySDKSubsystem::HandleVersionInfoResponse(const FVersionInfoResponse& VersionInfoResponse) const
{
	UE::Tasks::Launch(UE_SOURCE_LOCATION, [this, VersionInfoResponse]()
	{
		OnVersionInfo.Broadcast(VersionInfoResponse.ServerVersion, VersionInfoResponse.MinimumClientVersion);
	}, LowLevelTasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
}

void UCrowdySDKSubsystem::HandleTeleportPermissionResponse(const FTeleportResponse& TeleportResponse) const
{
	UE::Tasks::Launch(UE_SOURCE_LOCATION, [this, TeleportResponse]()
	{
		OnTeleportPermission.Broadcast(TeleportResponse.bTeleportAllowed);
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

	// Apply UDP configuration — must happen before the first InitializeUDP call.
	UdpSubsystem->SetPreferredProtocol(Settings->UDPProtocol);
	// UDPTimeoutSeconds is read directly in HandleUDPAddressNotify so that
	// live-settings changes in the editor take effect without restarting.

	return true;
}

void UCrowdySDKSubsystem::OnUDPTimeout()
{
	UE_LOG(LogCrowdySDK, Warning, TEXT("UDP timed out — auto-reconnecting."));

	// Stop polling while the connection is down; it restarts automatically
	// once HandleUDPAddressNotify reports success again.
	StopHostPolling();

	// Transition state before broadcasting so UI can react instantly.
	UdpSubsystem->SetConnectionState(EUDPConnectionState::Reconnecting);

	OnUDPTimedOut.Broadcast();

	// Re-request a UDP endpoint; HandleUDPAddressNotify will rebuild the
	// socket and restart timeout monitoring automatically.
	RequestUDPAccess();
}

void UCrowdySDKSubsystem::OnUDPConnectionSuccessful()
{
	OnUDPConnectionSuccess.Broadcast();

	// The socket is up and the player is authenticated, so join the reliable-RPC channels now; any
	// reliable sends queued before this flush once the joins complete. Idempotent across reconnects.
	if (UCrowdyChannels* Channels = GetGameInstance()->GetSubsystem<UCrowdyChannels>())
		Channels->BootstrapReliableRpcChannels();
}

void UCrowdySDKSubsystem::SendPingTestMessage()
{
	const FInt64Vector ChunkCoordinate = GameSession->GetPlayerCurrentChunkCoordinates();
	
	FPingTestMessage PingTestMessage;
	PingTestMessage.UUID = GameSession->GetUUID();
	PingTestMessage.AppID = GameSession->GetAppID();
	PingTestMessage.ChunkX = ChunkCoordinate.X;
	PingTestMessage.ChunkY = ChunkCoordinate.Y;
	PingTestMessage.ChunkZ = ChunkCoordinate.Z;
	PingTestMessage.DecayRate = ECrowdyDecayRate::Exponential_Decay;
	PingTestMessage.ReplicationDistance = ECrowdyReplicationDistance::One_Chunk;
	SendMessage(PingTestMessage);
}

void UCrowdySDKSubsystem::RegisterReceptionLayer(ICrowdyReceptionLayer* Layer) const
{
	if (!Layer)
	{
		UE_LOG(LogCrowdySDK, Error, TEXT("Candidate Reception Layer is null."));
		return;
	}

	if (!ServiceRegistry)
	{
		UE_LOG(LogCrowdySDK, Error, TEXT("Service Registry is null."));
		return;
	}

	ServiceRegistry->RegisterReceptionLayer(Layer);
}

void UCrowdySDKSubsystem::RegisterQueryReceptionLayer(ICrowdyQueryReceptionLayer* LayerToRegister) const
{
	if (!LayerToRegister)
	{
		UE_LOG(LogCrowdySDK, Error, TEXT("Candidate Query Reception Layer is null."));
		return;
	}

	if (!DataRegistry)
	{
		UE_LOG(LogCrowdySDK, Error, TEXT("Data Registry is null."));
		return;
	}

	DataRegistry->RegisterLayer(LayerToRegister);
}

bool UCrowdySDKSubsystem::IsLayerRegistered(const ICrowdyReceptionLayer* Layer) const
{
	return ServiceRegistry->IsLayerRegistered(Layer);
}

void UCrowdySDKSubsystem::SendMessage(const ICrowdyMessage& Message) const
{
	TArray<uint8> Bytes = Message.Serialize();

	if (Bytes.IsEmpty())
		return;

	if (TransmissionLayer)
	{
		TransmissionLayer->SendBytes(MoveTemp(Bytes), Message.bContainsAuth, Message.SequenceNumber);
	}
}

void UCrowdySDKSubsystem::ExecuteQuery(ICrowdyQueryRequest& Query) const
{
	UE_CLOG(CrowdySDKTrace::Sdk(), LogCrowdySDK, Log, TEXT("Execute Query:%s"), *Query.GetOperationName().ToString());
	QueryTransmissionLayer->ExecuteQuery(Query);
}

void UCrowdySDKSubsystem::SetTransmissionLayer(ICrowdyTransmissionLayer* Layer)
{
	TransmissionLayer = Layer;
}

void UCrowdySDKSubsystem::SetQueryTransmissionLayer(ICrowdyQueryTransmissionLayer* Layer)
{
	QueryTransmissionLayer = Layer;
}

void UCrowdySDKSubsystem::OnResponseReceived(TSharedPtr<ICrowdyQueryResponse> Response)
{
	if (!Response->IsValid())
	{
		UE_LOG(LogCrowdySDK, Error, TEXT("%s"), *Response->GetError())
		UE::Tasks::Launch(UE_SOURCE_LOCATION, [this, Response]()
		{
			switch (Response->GetResponseType())
			{
			case EQueryResponseType::UDP_Info:
				{
					OnUDPAddressNotify.Broadcast(false, false);
					break;
				}
			case EQueryResponseType::TeleportRequest:
				{
					OnTeleportPermission.Broadcast(false);
					break;
				}
			case EQueryResponseType::VersionInfo:
				{
					OnVersionInfo.Broadcast(FGameVersion(), FGameVersion());
					break;
				}
			default:
				break;
			}
		}, LowLevelTasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);

		return;
	}

	switch (Response->GetResponseType())
	{
	case EQueryResponseType::UDP_Info:
		{
			const FUDPAddressNotify UDPAddressNotify = static_cast<FUDPAddressNotify&>(*Response);
			HandleUDPAddressNotify(UDPAddressNotify);
			break;
		}
	case EQueryResponseType::VersionInfo:
		{
			const FVersionInfoResponse VersionInfoResponse = static_cast<FVersionInfoResponse&>(*Response);
			HandleVersionInfoResponse(VersionInfoResponse);
			break;
		}
	case EQueryResponseType::TeleportRequest:
		{
			const FTeleportResponse TeleportResponse = static_cast<FTeleportResponse&>(*Response);
			HandleTeleportPermissionResponse(TeleportResponse);
			break;
		}
	case EQueryResponseType::GameHost:
		{
			const FGameHostResponse& HostResponse = static_cast<FGameHostResponse&>(*Response);
			
			AsyncTask(ENamedThreads::GameThread, [this, HostResponse]()
			{
				const TObjectPtr<UCrowdyHostSubsystem> HostSubsystem = GetWorld()->GetSubsystem<UCrowdyHostSubsystem>();
				
				if (!IsValid(HostSubsystem.Get()))
					return;
				
				if (!HostSubsystem->IsReady())
					return;
				
				HostSubsystem->SetHostUserID(HostResponse.HostUserId);
				const bool bAmHost = HostSubsystem->IsHost();
			
				UE_CLOG(CrowdySDKTrace::Sdk(), LogCrowdySDK, Log,
				TEXT("GameHost | HostUserID=%lld | LocalUserID=%lld | IsHost=%s | ActorCount=%d | EarliestJoined=%s"),
				HostResponse.HostUserId,
				GameSession->GetUserID(),
				bAmHost ? TEXT("YES") : TEXT("no"),
				HostResponse.ActorCount,
				*HostResponse.EarliestActorJoinedAt);
			});
			
			break;
		}

	default:
		UE_LOG(LogCrowdySDK, Warning, TEXT("Unknown response received:%s"),
		       *Response->GetOperationName().ToString())
	}
}

TArray<EQueryResponseType> UCrowdySDKSubsystem::GetSupportedResponseType() const
{
	return
	{
		EQueryResponseType::UDP_Info,
		EQueryResponseType::VersionInfo,
		EQueryResponseType::TeleportRequest,
		EQueryResponseType::GameHost,
	};
}

void UCrowdySDKSubsystem::OnMessageReceived(TSharedRef<ICrowdyMessage> Message)
{
	switch (Message->GetType())
	{
	case ECrowdyMessageType::GENERIC_SPATIAL_1:
		{
			const auto& PingTestMessage = static_cast<const FPingTestMessage&>(*Message);
			
			if (PingTestMessage.UUID != GameSession->GetUUID())
			{
				return;
			}
			
			const int64 PingTimeMs = PingTestMessage.ReceiveTime - PingTestMessage.SendTime;
			UdpSubsystem->UpdatePingTime(PingTimeMs);
			break;
		}
	default:
		break;
	}
}

TArray<ECrowdyMessageType> UCrowdySDKSubsystem::GetSupportedResponseTypes() const
{
	return {ECrowdyMessageType::GENERIC_SPATIAL_1};
}

// ─── Game Host polling ────────────────────────────────────────────────────────

void UCrowdySDKSubsystem::StartHostPolling() const
{
	// SetTimer must run on the game thread; this function may be called from a
	// background response thread, so dispatch explicitly.
	TWeakObjectPtr<UCrowdySDKSubsystem> WeakThis(const_cast<UCrowdySDKSubsystem*>(this));
	AsyncTask(ENamedThreads::GameThread, [WeakThis]()
	{
		UCrowdySDKSubsystem* Self = WeakThis.Get();
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
	});
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

	FGameHostRequest Request;
	Request.AppId = GameSession->GetAppID();
	Request.PrepareQuery();
	ExecuteQuery(Request);
}
