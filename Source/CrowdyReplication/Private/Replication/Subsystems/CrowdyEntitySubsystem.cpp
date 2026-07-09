#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "CrowdyReplicationLog.h"

#include "TimerManager.h"
#include "Core/CrowdyCategory/FCrowdyTypeIDGenerator.h"
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
	ParticipantToID.Empty();
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
		UE_LOG(LogCrowdyReplication, Warning, TEXT("[CrowdyEntitySubsystem]: Rejected registration with invalid NetID (Participant=%s)."),
			*GetNameSafe(Record.GetParticipant()));
		return;
	}

	// Re-registering an existing NetID replaces the record; drop the old participant mapping first.
	if (const FCrowdyEntityRecord* Existing = Records.Find(Record.NetID))
	{
		if (UObject* OldParticipant = Existing->GetParticipant())
			ParticipantToID.Remove(OldParticipant);
	}

	Records.Add(Record.NetID, Record);

	if (UObject* Participant = Record.GetParticipant())
		ParticipantToID.Add(Participant, Record.NetID);

	OnEntityRegistered.Broadcast(Record.NetID);
}

void UCrowdyEntitySubsystem::UnregisterEntity(const FGuid& NetID)
{
	check(IsInGameThread());

	const FCrowdyEntityRecord* Record = Records.Find(NetID);
	if (!Record) return;

	if (UObject* Participant = Record->GetParticipant())
		ParticipantToID.Remove(Participant);

	Records.Remove(NetID);

	OnEntityUnregistered.Broadcast(NetID);
}

AActor* UCrowdyEntitySubsystem::FindEntity(const FGuid& NetID) const
{
	const FCrowdyEntityRecord* Record = Records.Find(NetID);
	return Record ? Record->GetActor() : nullptr;
}

UObject* UCrowdyEntitySubsystem::FindParticipant(const FGuid& NetID) const
{
	const FCrowdyEntityRecord* Record = Records.Find(NetID);
	return Record ? Record->GetParticipant() : nullptr;
}

FGuid UCrowdyEntitySubsystem::FindEntityID(const UObject* Participant) const
{
	if (!Participant) return FGuid{};

	// TObjectKey<UObject> constructs from a const UObject*, so no const_cast is needed to look up the key.
	const FGuid* NetID = ParticipantToID.Find(Participant);
	return NetID ? *NetID : FGuid{};
}

FGuid UCrowdyEntitySubsystem::FindEntityID(const AActor* Actor) const
{
	return FindEntityID(static_cast<const UObject*>(Actor));
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

	// A LocalClient participant enrolled before the local player id arrived was minted with an empty owner
	// salt; re-derive its owner-salted identity now. Actor entities carry their own identity (set at spawn,
	// GetActor() non-null) and are never re-stamped here, guaranteeing zero behavior change for actors. Inert
	// in Phase 0 (no participant is enrolled, so this set is always empty). Collect-then-mutate so the
	// re-registration below does not modify Records mid-iteration.
	TArray<FGuid> StaleParticipantIDs;
	for (const TPair<FGuid, FCrowdyEntityRecord>& Pair : Records)
	{
		const FCrowdyEntityRecord& Record = Pair.Value;
		if (Record.Role == ECrowdyRole::Owner && !Record.OwnerID.IsValid()
			&& Record.GetActor() == nullptr && Record.GetParticipant() != nullptr)
		{
			StaleParticipantIDs.Add(Pair.Key);
		}
	}

	for (const FGuid& OldNetID : StaleParticipantIDs)
		RestampParticipantIdentity(OldNetID);
}

FGuid UCrowdyEntitySubsystem::RegisterParticipant(UObject* Participant, const ECrowdyOwnership Ownership)
{
	check(IsInGameThread());

	if (!IsValid(Participant))
	{
		UE_LOG(LogCrowdyReplication, Warning, TEXT("[CrowdyEntitySubsystem]: RegisterParticipant called with an invalid participant."));
		return FGuid{};
	}

	ECrowdyRole Role;
	FGuid OwnerID;
	UCrowdyEntityComponent::DeriveAuthority(Ownership, GetLocalPlayerID(), Role, OwnerID);

	// Host: world singleton keyed by class path — every client computes the same id, no salt.
	// LocalClient: owner-salted so two clients' same-class participants get distinct ids.
	const FString PathName = Participant->GetClass()->GetPathName();
	const FString Seed = (Ownership == ECrowdyOwnership::Host)
		? PathName
		: PathName + TEXT(":") + OwnerID.ToString();

	FCrowdyEntityRecord Record;
	Record.NetID       = UHelperFunctions::GetDeterministicID(FCrowdyTypeIDGenerator::GenerateFromString(Seed));
	Record.OwnerID     = OwnerID;
	Record.Role        = Role;
	Record.ClassID     = UCrowdyClassRegistry::Get()->GetID(Participant->GetClass());
	Record.Participant = Participant;
	RegisterEntity(Record);

	return Record.NetID;
}

void UCrowdyEntitySubsystem::UnregisterParticipant(UObject* Participant)
{
	check(IsInGameThread());

	if (!Participant) return;

	const FGuid NetID = FindEntityID(Participant);
	if (NetID.IsValid())
		UnregisterEntity(NetID);
}

void UCrowdyEntitySubsystem::RestampParticipantIdentity(const FGuid& OldNetID)
{
	const FCrowdyEntityRecord* Existing = Records.Find(OldNetID);
	if (!Existing) return;

	FCrowdyEntityRecord Record = *Existing;
	UObject* Participant = Record.GetParticipant();
	if (!IsValid(Participant))
	{
		UnregisterEntity(OldNetID);
		return;
	}

	Record.OwnerID = LocalPlayerID;
	const FString Seed = Participant->GetClass()->GetPathName() + TEXT(":") + Record.OwnerID.ToString();
	const FGuid NewNetID = UHelperFunctions::GetDeterministicID(FCrowdyTypeIDGenerator::GenerateFromString(Seed));

	if (NewNetID == OldNetID)
	{
		// Owner resolved to the same salt (already correct) — update the record in place.
		Records.Add(OldNetID, Record);
		return;
	}

	UnregisterEntity(OldNetID);
	Record.NetID = NewNetID;
	RegisterEntity(Record);
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
		Record.Participant = Entity;
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
			if (AActor* Actor = Pair.Value.GetActor())
				Result.Add(Actor);
		}
	}
	return Result;
}

void UCrowdyEntitySubsystem::ReassignOwnership(const FGuid& NetID, const FGuid& NewOwnerID,
	const FGuid& ExpectedPreviousOwnerID)
{
	check(IsInGameThread());

	FCrowdyEntityRecord* Record = Records.Find(NetID);
	if (!Record)
	{
		// Not present locally (e.g. a proxy that has not spawned yet). The router defers a grant for a not-yet-
		// present entity, so this is a benign miss rather than a lost transfer.
		return;
	}

	const FGuid PreviousOwnerID = Record->OwnerID;

	// Compare-and-swap: a stale or duplicate grant whose expected previous owner no longer matches is dropped.
	// Skipped when the caller passes an invalid expectation (a host-owned source has no per-client owner id).
	if (ExpectedPreviousOwnerID.IsValid() && PreviousOwnerID != ExpectedPreviousOwnerID)
	{
		UE_CLOG(CrowdyReplicationTrace::Entity(), LogCrowdyReplication, Log,
			TEXT("[CrowdyEntitySubsystem]: ReassignOwnership %s dropped — expected previous owner %s but current is %s."),
			*NetID.ToString(), *ExpectedPreviousOwnerID.ToString(), *PreviousOwnerID.ToString());
		return;
	}

	// Idempotent: re-applying the same owner (a duplicate grant) is a no-op.
	if (PreviousOwnerID == NewOwnerID)
		return;

	// Re-derive the role from the new owner: a valid player id means a client owns it (Owner where we are that
	// client, RemoteProxy elsewhere); an invalid id means it is host-owned (a world entity, no per-client owner).
	ECrowdyRole NewRole;
	if (NewOwnerID.IsValid())
		NewRole = (NewOwnerID == LocalPlayerID) ? ECrowdyRole::Owner : ECrowdyRole::RemoteProxy;
	else
		NewRole = ECrowdyRole::HostOwned;

	Record->OwnerID = NewOwnerID;
	Record->Role    = NewRole;

	// Capture the actor before broadcasting so a re-entrant handler cannot leave us reading a freed record.
	AActor* Actor = Record->GetActor();

	// Keep the entity component's cached identity in sync and let it move any Dynamic-mode continuous channel.
	if (IsValid(Actor))
	{
		if (UCrowdyEntityComponent* Component = Actor->FindComponentByClass<UCrowdyEntityComponent>())
			Component->ApplyOwnershipReassignment(NewOwnerID, NewRole);
	}

	UE_CLOG(CrowdyReplicationTrace::Entity(), LogCrowdyReplication, Log,
		TEXT("[CrowdyEntitySubsystem]: ReassignOwnership %s: %s -> %s (role %d)."),
		*NetID.ToString(), *PreviousOwnerID.ToString(), *NewOwnerID.ToString(), static_cast<int32>(NewRole));

	OnEntityOwnershipChanged.Broadcast(Actor, NetID, NewOwnerID, PreviousOwnerID);
}

void UCrowdyEntitySubsystem::NotifyOwnershipRequested(AActor* TargetEntity, const FGuid& RequesterID)
{
	// A player avatar's NetID equals its player id, so FindEntity resolves the requester's avatar when it exists on
	// this client; otherwise the actor is null and game code falls back to the always-valid RequesterID.
	AActor* RequesterActor = FindEntity(RequesterID);
	OnOwnershipRequested.Broadcast(TargetEntity, RequesterActor, RequesterID);
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

	AActor* Actor = Record->GetActor();

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
