#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "CrowdyReplicationLog.h"

#include "TimerManager.h"
#include "Core/CrowdySDKBridgeSubsystem.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Engine/AssetManager.h"
#include "Internal/FCrowdyServiceRegistry.h"
#include "Messages/GameObjects/FGameEventNotification.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Subsystem/CrowdyAutoRegistry.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "Utils/HelperFunctions.h"
#include "Utils/SerializationFunctionLibrary.h"
#include "Utils/UCrowdyClassRegistry.h"

void UCrowdyEntitySubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	const UWorld* World = GetWorld();
	checkf(IsValid(World), TEXT("World is invalid"));

	// During UGameEngine::Init the initial Game world is created before a
	// UGameInstance owns it, so GetGameInstance() can be null here.
	UGameInstance* GameInstance = World->GetGameInstance();
	if (!GameInstance)
		return;

	// Level transitions bring in new handler/executor classes; re-scan so
	// their payload structs are registered before any of them sends.
	if (UCrowdyAutoRegistry* AutoRegistry = GameInstance->GetSubsystem<UCrowdyAutoRegistry>())
		AutoRegistry->RegisterLoadedPayloadTypes();

	GameSession = GameInstance->GetSubsystem<UCrowdyGameSession>();
	checkf(IsValid(GameSession), TEXT("GameSession is invalid"));

	// The session may already have a UUID
	// so seed from it and then track future updates.
	LocalPlayerID = GameSession->GetID();
	GameSession->OnOwnerUUIDUpdated.AddDynamic(this, &UCrowdyEntitySubsystem::OnOwnerUUIDUpdated);

	const UCrowdyMapProfile* Profile = UCrowdySDKDeveloperSettings::ResolveProfileForWorld(World);
	if (!Profile || !Profile->bEnableNetworking)
	{
		UE_CLOG(CrowdyReplicationTrace::Entity(), LogCrowdyReplication, Log, TEXT("[CrowdyEntitySubsystem]: Networking disabled for this map — entity events inactive."));
		return;
	}

	Bridge = GameInstance->GetSubsystem<UCrowdySDKBridgeSubsystem>();
	if (!IsValid(Bridge))
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[CrowdyEntitySubsystem]: Invalid Bridge subsystem — entity events disabled."));
		return;
	}

	if (Bridge->ServiceRegistry)
		Bridge->ServiceRegistry->RegisterReceptionLayer(this);
}

void UCrowdyEntitySubsystem::Deinitialize()
{
	if (IsValid(GameSession))
		GameSession->OnOwnerUUIDUpdated.RemoveDynamic(this, &UCrowdyEntitySubsystem::OnOwnerUUIDUpdated);
	GameSession = nullptr;

	for (auto& Pair : PendingRemoteSpawns)
	{
		if (Pair.Value.LoadHandle.IsValid())
			Pair.Value.LoadHandle->CancelHandle();
	}
	PendingRemoteSpawns.Empty();

	Records.Empty();
	ActorToID.Empty();
	PendingEntityEvents.Empty();
	Bridge = nullptr;

	Super::Deinitialize();
}

bool UCrowdyEntitySubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
		return false;

	const UWorld* World = Outer ? Outer->GetWorld() : nullptr;
	if (!World) return false;

	return World->WorldType == EWorldType::PIE
		|| World->WorldType == EWorldType::Game;
}

//Network reception

void UCrowdyEntitySubsystem::OnMessageReceived(TSharedRef<ICrowdyMessage> Message)
{
	if (Message->GetType() != ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION)
		return;

	const auto& EventMessage = static_cast<const FGameEventNotification&>(*Message);
	const UScriptStruct* StructType = EventMessage.State.GetScriptStruct();

	if (StructType != FCrowdyEntitySpawnEvent::StaticStruct()
		&& StructType != FCrowdyEntityDestroyEvent::StaticStruct())
		return;

	PendingEntityEvents.Enqueue(EventMessage.State);
}

void UCrowdyEntitySubsystem::Tick(float DeltaTime)
{
	FInstancedStruct Payload;
	while (PendingEntityEvents.Dequeue(Payload))
	{
		const UScriptStruct* StructType = Payload.GetScriptStruct();

		if (StructType == FCrowdyEntitySpawnEvent::StaticStruct())
			HandleRemoteSpawn(Payload.Get<FCrowdyEntitySpawnEvent>());
		else if (StructType == FCrowdyEntityDestroyEvent::StaticStruct())
			HandleRemoteDestroy(Payload.Get<FCrowdyEntityDestroyEvent>());
	}
}

TArray<ECrowdyMessageType> UCrowdyEntitySubsystem::GetSupportedResponseTypes() const
{
	return { ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION };
}

TArray<UScriptStruct*> UCrowdyEntitySubsystem::GetSupportedEvents() const
{
	return
	{
		FCrowdyEntitySpawnEvent::StaticStruct(),
		FCrowdyEntityDestroyEvent::StaticStruct()
	};
}

//Registry 
void UCrowdyEntitySubsystem::RegisterEntity(const FCrowdyEntityRecord& Record)
{
	check(IsInGameThread());

	if (!Record.NetID.IsValid())
	{
		UE_LOG(LogCrowdyReplication, Warning, TEXT("[CrowdyEntitySubsystem]: Rejected registration with invalid NetID (Actor=%s)."),
			*GetNameSafe(Record.Actor.Get()));
		return;
	}

	// Re-registering an existing NetID replaces the record; drop the old actor mapping first.
	if (const FCrowdyEntityRecord* Existing = Records.Find(Record.NetID))
	{
		if (AActor* OldActor = Existing->Actor.Get())
			ActorToID.Remove(OldActor);
	}

	Records.Add(Record.NetID, Record);

	if (AActor* Actor = Record.Actor.Get())
		ActorToID.Add(Actor, Record.NetID);

	OnEntityRegistered.Broadcast(Record.NetID);
}

void UCrowdyEntitySubsystem::UnregisterEntity(const FGuid& NetID)
{
	check(IsInGameThread());

	const FCrowdyEntityRecord* Record = Records.Find(NetID);
	if (!Record) return;

	if (AActor* Actor = Record->Actor.Get())
		ActorToID.Remove(Actor);

	Records.Remove(NetID);

	OnEntityUnregistered.Broadcast(NetID);
}

AActor* UCrowdyEntitySubsystem::FindEntity(const FGuid& NetID) const
{
	const FCrowdyEntityRecord* Record = Records.Find(NetID);
	return Record ? Record->Actor.Get() : nullptr;
}

FGuid UCrowdyEntitySubsystem::FindEntityID(const AActor* Actor) const
{
	if (!Actor) return FGuid{};

	const FGuid* NetID = ActorToID.Find(const_cast<AActor*>(Actor));
	return NetID ? *NetID : FGuid{};
}

const FCrowdyEntityRecord* UCrowdyEntitySubsystem::FindRecord(const FGuid& NetID) const
{
	return Records.Find(NetID);
}

bool UCrowdyEntitySubsystem::IsLocallyOwned(const FGuid& NetID) const
{
	const FCrowdyEntityRecord* Record = Records.Find(NetID);
	return Record && Record->OwnerID.IsValid() && Record->OwnerID == LocalPlayerID;
}

FGuid UCrowdyEntitySubsystem::GetLocalPlayerID() const
{
	return LocalPlayerID;
}

FGuid UCrowdyEntitySubsystem::GetHostID() const
{
	return IsValid(GameSession) ? GameSession->GetHostID() : FGuid{};
}

void UCrowdyEntitySubsystem::SetLocalPlayerID(const FGuid& InLocalPlayerID)
{
	LocalPlayerID = InLocalPlayerID;
}

void UCrowdyEntitySubsystem::OnOwnerUUIDUpdated(FString NewUUID)
{
	LocalPlayerID = USerializationFunctionLibrary::ToGuid(NewUUID);
}

//Networked entity lifecycle

AActor* UCrowdyEntitySubsystem::SpawnEntity(const TSubclassOf<AActor> EntityClass, const FTransform& SpawnTransform, const FInstancedStruct& InitialState)
{
	if (!EntityClass)
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[CrowdyEntitySubsystem]: SpawnEntity called with a null class."));
		return nullptr;
	}

	const uint32 ClassID = UCrowdyClassRegistry::Get()->GetID(EntityClass);

	AActor* Actor = GetWorld()->SpawnActorDeferred<AActor>(EntityClass, SpawnTransform, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!IsValid(Actor)) return nullptr;

	const FGuid EntityID = FGuid::NewGuid();

	// Inject identity before BeginPlay; the component registers the record itself
	UCrowdyEntityComponent* Component = Actor->FindComponentByClass<UCrowdyEntityComponent>();
	if (Component)
		Component->InitIdentity(EntityID, LocalPlayerID, ECrowdyRole::Owner, ClassID);

	Actor->FinishSpawning(SpawnTransform);

	if (!Component)
		Component = AddStaticEntityComponent(Actor, EntityID, LocalPlayerID, ECrowdyRole::Owner, ClassID);

	if (Component)
		Component->OnCrowdySpawned.Broadcast(InitialState, true);

	FCrowdyEntitySpawnEvent SpawnEvent;
	SpawnEvent.EntityID       = EntityID;
	SpawnEvent.OwnerID        = LocalPlayerID;
	SpawnEvent.ClassID        = ClassID;
	SpawnEvent.ClassPath      = EntityClass->GetPathName();
	SpawnEvent.SpawnTransform = SpawnTransform;
	SpawnEvent.InitialState   = InitialState;

	FInstancedStruct Payload;
	Payload.InitializeAs<FCrowdyEntitySpawnEvent>(SpawnEvent);
	DispatchGameEvent(Actor, MoveTemp(Payload));

	return Actor;
}

void UCrowdyEntitySubsystem::DestroyEntity(AActor* TargetEntity)
{
	const FGuid EntityID = FindEntityID(TargetEntity);
	if (!EntityID.IsValid())
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[CrowdyEntitySubsystem]: DestroyEntity — '%s' is not a registered entity."),
			*GetNameSafe(TargetEntity));
		return;
	}

	FCrowdyEntityDestroyEvent DestroyEvent;
	DestroyEvent.EntityID = EntityID;

	FInstancedStruct Payload;
	Payload.InitializeAs<FCrowdyEntityDestroyEvent>(DestroyEvent);
	DispatchGameEvent(TargetEntity, MoveTemp(Payload));

	float Delay = 0.f;
	if (UCrowdyEntityComponent* Component = TargetEntity->FindComponentByClass<UCrowdyEntityComponent>())
	{
		Component->OnCrowdyDestroyed.Broadcast(true);
		Delay = Component->DestroyDelay;
	}

	UnregisterEntity(EntityID);
	DestroyAfterDelay(TargetEntity, Delay);
}

void UCrowdyEntitySubsystem::RegisterStaticEntity(AActor* Entity, const bool bUseDeterministicID, const int64 Seed)
{
	if (!IsValid(Entity)) return;

	if (FindEntityID(Entity).IsValid())
	{
		UE_LOG(LogCrowdyReplication, Warning, TEXT("[CrowdyEntitySubsystem]: RegisterStaticEntity — '%s' is already registered (it likely has a UCrowdyEntityComponent)."),
			*GetNameSafe(Entity));
		return;
	}

	const uint32 ClassID = UCrowdyClassRegistry::Get()->GetID(Entity->GetClass());
	const FGuid EntityID = bUseDeterministicID ? UHelperFunctions::GetDeterministicID(Seed) : FGuid::NewGuid();

	UCrowdyEntityComponent* Component = Entity->FindComponentByClass<UCrowdyEntityComponent>();
	if (Component)
	{
		// Component exists but never resolved an ID (it would be registered otherwise)
		Component->InitIdentity(EntityID, LocalPlayerID, ECrowdyRole::Owner, ClassID);

		FCrowdyEntityRecord Record;
		Record.NetID   = EntityID;
		Record.OwnerID = LocalPlayerID;
		Record.Role    = ECrowdyRole::Owner;
		Record.ClassID = ClassID;
		Record.Actor   = Entity;
		RegisterEntity(Record);
	}
	else
	{
		// RegisterComponent runs the component's BeginPlay, which registers the record
		Component = AddStaticEntityComponent(Entity, EntityID, LocalPlayerID, ECrowdyRole::Owner, ClassID);
	}

	if (Component)
		Component->OnCrowdySpawned.Broadcast(FInstancedStruct(), true);
}

void UCrowdyEntitySubsystem::DispatchGameEvent(const AActor* Context, FInstancedStruct&& Payload,
	const ECrowdyTarget Target, const AActor* TargetEntity,
	const ECrowdyDecayRate DecayRate, const ECrowdyReplicationDistance ReplicationDistance)
{
	if (!Bridge || !IsValid(Context))
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[CrowdyEntitySubsystem]: DispatchGameEvent — Bridge or context actor is null."));
		return;
	}

	FGuid TargetID;
	if (Target == ECrowdyTarget::Entity || Target == ECrowdyTarget::Owner)
	{
		const FGuid NetID = FindEntityID(TargetEntity);
		const FCrowdyEntityRecord* Record = FindRecord(NetID);
		if (!Record)
		{
			UE_LOG(LogCrowdyReplication, Warning, TEXT("[CrowdyEntitySubsystem]: DispatchGameEvent — target '%s' is not a registered entity, event dropped."),
				*GetNameSafe(TargetEntity));
			return;
		}

		// Owner routing matches against player IDs, so address the entity's owning client
		TargetID = Target == ECrowdyTarget::Owner ? Record->OwnerID : NetID;
	}

	int64 ChunkX, ChunkY, ChunkZ;
	UHelperFunctions::GetChunkCoordinateAtLocation(this, Context->GetActorLocation(), ChunkX, ChunkY, ChunkZ);

	if (Bridge->DispatchGameEventFn)
		Bridge->DispatchGameEventFn(ChunkX, ChunkY, ChunkZ,
			DecayRate, ReplicationDistance,
			LocalPlayerID, MoveTemp(Payload), Target, TargetID, false);
}

void UCrowdyEntitySubsystem::DispatchSingleActorMessage(const AActor* TargetActor, FInstancedStruct Payload)
{
	if (!Bridge || !IsValid(TargetActor))
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[CrowdyEntitySubsystem]: DispatchSingleActorMessage — Bridge or target actor is null."));
		return;
	}

	// The destination actor must be a registered entity — that NetID is the UUID the server routes by.
	const FGuid TargetID = FindEntityID(TargetActor);
	if (!TargetID.IsValid())
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[CrowdyEntitySubsystem]: DispatchSingleActorMessage — '%s' is not a registered entity; dropping."),
			*GetNameSafe(TargetActor));
		return;
	}

	int64 ChunkX, ChunkY, ChunkZ;
	UHelperFunctions::GetChunkCoordinateAtLocation(this, TargetActor->GetActorLocation(), ChunkX, ChunkY, ChunkZ);

	if (Bridge->DispatchSingleActorMessageFn)
		Bridge->DispatchSingleActorMessageFn(ChunkX, ChunkY, ChunkZ, TargetID, MoveTemp(Payload), false);
}

void UCrowdyEntitySubsystem::PublishReliableRpc(const FString& ChannelName, const TArray<uint8>& ChannelPayload)
{
	if (!Bridge)
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[CrowdyEntitySubsystem]: PublishReliableRpc — Bridge is null; dropping."));
		return;
	}

	if (Bridge->PublishReliableRpcFn)
	{
		Bridge->PublishReliableRpcFn(ChannelName, ChannelPayload);
	}
	else
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[CrowdyEntitySubsystem]: PublishReliableRpc — channel transport not wired; a reliable RPC was dropped."));
	}
}

TArray<AActor*> UCrowdyEntitySubsystem::GetEntitiesByOwner(const FGuid& OwnerID) const
{
	TArray<AActor*> Result;
	for (const auto& Pair : Records)
	{
		if (Pair.Value.OwnerID == OwnerID)
		{
			if (AActor* Actor = Pair.Value.Actor.Get())
				Result.Add(Actor);
		}
	}
	return Result;
}

// Remote handlers

void UCrowdyEntitySubsystem::HandleRemoteSpawn(const FCrowdyEntitySpawnEvent& Event)
{
	// Pool backend may have already registered this entity from position updates
	// that arrived before the spawn event. Update metadata and return — the pool
	// actor is already active and correctly registered.
	if (FCrowdyEntityRecord* Existing = Records.Find(Event.EntityID))
	{
		Existing->OwnerID = Event.OwnerID;
		Existing->ClassID = Event.ClassID;
		return;
	}

	// Duplicate spawn event arrived while the class is still loading — the first one is already pending.
	if (PendingRemoteSpawns.Contains(Event.EntityID))
		return;

	FSoftClassPath ClassPath = UCrowdyClassRegistry::Get()->Resolve(Event.ClassID);
	if (!ClassPath.IsValid())
		ClassPath = FSoftClassPath(Event.ClassPath);

	if (!ClassPath.IsValid())
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[CrowdyEntitySubsystem]: Unknown ClassID %u and empty class path on remote spawn %s."),
			Event.ClassID, *Event.EntityID.ToString());
		return;
	}

	if (UClass* LoadedClass = ClassPath.ResolveClass())
	{
		FinishRemoteSpawn(Event, LoadedClass);
		return;
	}

	// Class is not in memory; stream it in and hold the spawn until it lands.
	// A destroy event arriving during the load cancels the spawn.
	FPendingRemoteSpawn& Pending = PendingRemoteSpawns.Add(Event.EntityID);
	Pending.SpawnEvent = Event;
	Pending.LoadHandle = UAssetManager::GetStreamableManager().RequestAsyncLoad(
		ClassPath,
		FStreamableDelegate::CreateUObject(this, &UCrowdyEntitySubsystem::OnRemoteSpawnClassLoaded,
			Event.EntityID, ClassPath));
}

void UCrowdyEntitySubsystem::OnRemoteSpawnClassLoaded(const FGuid EntityID, const FSoftClassPath ClassPath)
{
	FPendingRemoteSpawn Pending;
	if (!PendingRemoteSpawns.RemoveAndCopyValue(EntityID, Pending))
		return; // destroyed (or torn down) while the class was loading

	UClass* LoadedClass = ClassPath.ResolveClass();
	if (!LoadedClass)
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[CrowdyEntitySubsystem]: Failed to load class '%s' for remote spawn %s."),
			*ClassPath.ToString(), *EntityID.ToString());
		return;
	}

	FinishRemoteSpawn(Pending.SpawnEvent, LoadedClass);
}

void UCrowdyEntitySubsystem::FinishRemoteSpawn(const FCrowdyEntitySpawnEvent& Event, UClass* EntityClass)
{
	// Same class as the owning client identity is injected before BeginPlay,
	// so the component registers as RemoteProxy instead of minting an ID.
	AActor* Actor = GetWorld()->SpawnActorDeferred<AActor>(EntityClass, Event.SpawnTransform, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!IsValid(Actor)) return;

	UCrowdyEntityComponent* Component = Actor->FindComponentByClass<UCrowdyEntityComponent>();
	if (Component)
		Component->InitIdentity(Event.EntityID, Event.OwnerID, ECrowdyRole::RemoteProxy, Event.ClassID);

	Actor->FinishSpawning(Event.SpawnTransform);

	if (!Component)
		Component = AddStaticEntityComponent(Actor, Event.EntityID, Event.OwnerID, ECrowdyRole::RemoteProxy, Event.ClassID);

	if (Component)
		Component->OnCrowdySpawned.Broadcast(Event.InitialState, false);
}

void UCrowdyEntitySubsystem::HandleRemoteDestroy(const FCrowdyEntityDestroyEvent& Event)
{
	// Destroyed before its class finished loading: the actor never existed
	// here, so cancel the load and drop the pending spawn.
	if (FPendingRemoteSpawn* Pending = PendingRemoteSpawns.Find(Event.EntityID))
	{
		if (Pending->LoadHandle.IsValid())
			Pending->LoadHandle->CancelHandle();
		PendingRemoteSpawns.Remove(Event.EntityID);
		return;
	}

	const FCrowdyEntityRecord* Record = Records.Find(Event.EntityID);
	if (!Record)
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[CrowdyEntitySubsystem]: Entity %s already destroyed on remote destroy."),
			*Event.EntityID.ToString());
		return;
	}

	AActor* Actor = Record->Actor.Get();

	float Delay = 0.f;
	if (IsValid(Actor))
	{
		if (UCrowdyEntityComponent* Component = Actor->FindComponentByClass<UCrowdyEntityComponent>())
		{
			Component->OnCrowdyDestroyed.Broadcast(false);
			Delay = Component->DestroyDelay;
		}
	}

	UnregisterEntity(Event.EntityID);
	DestroyAfterDelay(Actor, Delay);
}

//Internal

UCrowdyEntityComponent* UCrowdyEntitySubsystem::AddStaticEntityComponent(AActor* Actor, const FGuid& EntityID,
	const FGuid& EntityOwnerID, const ECrowdyRole Role, const uint32 ClassID) const
{
	UCrowdyEntityComponent* Component = NewObject<UCrowdyEntityComponent>(Actor, TEXT("CrowdyEntityComponent"));
	if (!IsValid(Component)) return nullptr;

	Component->Mode = ECrowdyEntityMode::Static;
	Component->InitIdentity(EntityID, EntityOwnerID, Role, ClassID);
	Component->RegisterComponent();

	return Component;
}

void UCrowdyEntitySubsystem::DestroyAfterDelay(AActor* Actor, const float Delay) const
{
	if (!IsValid(Actor)) return;

	if (Delay > 0.f)
	{
		FTimerHandle Handle;
		GetWorld()->GetTimerManager().SetTimer(Handle, [Actor]()
		{
			if (IsValid(Actor)) Actor->Destroy();
		}, Delay, false);
	}
	else
	{
		Actor->Destroy();
	}
}
