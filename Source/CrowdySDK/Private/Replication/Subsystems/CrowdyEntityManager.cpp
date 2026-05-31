#include "Replication/Subsystems/CrowdyEntityManager.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Data/CrowdyEntityRegistry.h"
#include "Messages/GameObjects/FCrowdyEntitySpawnEvent.h"
#include "Messages/GameObjects/FGameEventNotification.h"
#include "Replication/ObjectHandler/CrowdyEntityEventHandler.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Subsystem/CrowdySDKSubsystem.h"
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "Utils/HelperFunctions.h"
#include "Utils/UEventPayloadRegistry.h"

void UCrowdyEntityManager::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    
    UE_LOG(LogTemp, Log, TEXT("[CrowdyObjectManager]: Initialize — World=%s"), *GetNameSafe(GetWorld()));
    
    const UWorld* World = GetWorld();
    checkf(IsValid(World), TEXT("World is invalid"));
   
    UCrowdySDKSubsystem* SDK = World->GetGameInstance()->GetSubsystem<UCrowdySDKSubsystem>();
    UCrowdyGameSession* GameSession = World->GetGameInstance()->GetSubsystem<UCrowdyGameSession>();

    checkf(IsValid(SDK), TEXT("CrowdySDKSubsystem is invalid"));
    checkf(IsValid(GameSession), TEXT("GameSession is invalid"));

    CachedSDK = SDK;
    
    if (!LoadConfig()) return;
    
    SDK->RegisterReceptionLayer(this);
}

void UCrowdyEntityManager::Deinitialize()
{
    IDToEntity.Empty();
    ActorToID.Empty();
    EventHandlers.Empty();
    TypeIDToHandler.Empty();
    SupportedStructTypes.Empty();
    EntityRegistry = nullptr;
    CachedSDK = nullptr;
    Super::Deinitialize();
}

bool UCrowdyEntityManager::ShouldCreateSubsystem(UObject* Outer) const
{
    if (!Super::ShouldCreateSubsystem(Outer))
        return false;

    const UWorld* World = Outer ? Outer->GetWorld() : nullptr;
    if (!World) return false;

    return World->WorldType == EWorldType::PIE
        || World->WorldType == EWorldType::Game;
}

// ── Public API ───────────────────────────────────────────────────────────────

AActor* UCrowdyEntityManager::SpawnCrowdyEntity(
    const TSubclassOf<AActor> EntityClass,
    const FTransform& SpawnTransform,
    const FInstancedStruct& InitialState)
{
    if (!EntityRegistry)
    {
        UE_LOG(LogTemp, Error, TEXT("[CrowdyEntityManager]: EntityRegistry is null."));
        return nullptr;
    }

    const FCrowdyEntityTypeEntry* Entry = EntityRegistry->FindByClass(EntityClass);
    if (!Entry)
    {
        UE_LOG(LogTemp, Error, TEXT("[CrowdyEntityManager]: Class '%s' not found in registry."), *EntityClass->GetName());
        return nullptr;
    }

    FActorSpawnParameters Params;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    AActor* Actor = GetWorld()->SpawnActor<AActor>(EntityClass, SpawnTransform, Params);
    if (!IsValid(Actor)) return nullptr;

    const FGuid EntityID = FGuid::NewGuid();

    RegisterEntityInternal(EntityID, CachedLocalPlayerID, Entry->TypeID, Actor);

    if (UCrowdyEntityEventHandler* Handler = FindHandler(Entry->TypeID))
        Handler->OnEntitySpawned(Actor, InitialState, true);

    FCrowdyEntitySpawnEvent SpawnEvent;
    SpawnEvent.EntityID     = EntityID;
    SpawnEvent.OwnerID      = CachedLocalPlayerID;
    SpawnEvent.TypeID       = Entry->TypeID;
    SpawnEvent.SpawnTransform = SpawnTransform;
    SpawnEvent.InitialState = InitialState;

    FInstancedStruct Payload;
    Payload.InitializeAs<FCrowdyEntitySpawnEvent>(SpawnEvent);
    BroadcastToNetwork(Actor, Payload);

    return Actor;
}

void UCrowdyEntityManager::UpdateCrowdyEntityState(AActor* TargetEntity, const FInstancedStruct& NewState)
{
    const FGuid EntityID = FindEntityID(TargetEntity);
    if (!EntityID.IsValid()) return;

    const FEntityEntry* Entry = IDToEntity.Find(EntityID);
    if (!Entry) return;

    if (UCrowdyEntityEventHandler* Handler = FindHandler(Entry->TypeID))
        Handler->OnEntityStateChanged(TargetEntity, NewState);

    FCrowdyEntityStateEvent StateEvent;
    StateEvent.EntityID = EntityID;
    StateEvent.NewState = NewState;

    FInstancedStruct Payload;
    Payload.InitializeAs<FCrowdyEntityStateEvent>(StateEvent);
    BroadcastToNetwork(TargetEntity, Payload);
}

void UCrowdyEntityManager::DestroyCrowdyEntity(AActor* TargetEntity)
{
    const FGuid EntityID = FindEntityID(TargetEntity);
    
    if (!EntityID.IsValid())
    {
        UE_LOG(LogTemp, Error, TEXT("[CrowdyEntityManager]: Entity not found in registry."));
        return;
    }
    
    const FEntityEntry* Entry = IDToEntity.Find(EntityID);
    if (!Entry)
    {
        UE_LOG(LogTemp, Error, TEXT("[CrowdyEntityManager]: Entity not found in registry."));
        return;
    };

    const int32 TypeID = Entry->TypeID;

    FCrowdyEntityDestroyEvent DestroyEvent;
    DestroyEvent.EntityID = EntityID;

    FInstancedStruct Payload;
    Payload.InitializeAs<FCrowdyEntityDestroyEvent>(DestroyEvent);
    BroadcastToNetwork(TargetEntity, Payload);

    float Delay = 0.f;
    if (UCrowdyEntityEventHandler* Handler = FindHandler(TypeID))
        Delay = Handler->OnEntityDestroyed(TargetEntity, true);

    UnregisterEntityInternal(EntityID);

    if (Delay > 0.f)
    {
        FTimerHandle Handle;
        GetWorld()->GetTimerManager().SetTimer(Handle, [TargetEntity]()
        {
            if (IsValid(TargetEntity)) TargetEntity->Destroy();
        }, Delay, false);
    }
    else if (IsValid(TargetEntity))
    {
        TargetEntity->Destroy();
    }
}

void UCrowdyEntityManager::RegisterStaticEntity(AActor* Entity, const bool bUseDeterministicID, const int64 Seed)
{
    if (!EntityRegistry)
    {
        return;
    }
    
    if (!IsValid(Entity))
        return;
    
    const FCrowdyEntityTypeEntry* Entry = EntityRegistry->FindByClass(Entity->GetClass());
    
    if (!Entry)
    {
        UE_LOG(LogTemp, Error, TEXT("[CrowdyEntityManager]: Class '%s' not found in registry."), *Entity->GetClass()->GetName());
    }
    
    const FGuid EntityID = bUseDeterministicID ? UHelperFunctions::GetDeterministicID(Seed) : FGuid::NewGuid();
    
    RegisterEntityInternal(EntityID, CachedLocalPlayerID, Entry->TypeID, Entity);
    
    if (UCrowdyEntityEventHandler* Handler = FindHandler(Entry->TypeID))
        Handler->OnEntitySpawned(Entity, FInstancedStruct(), true);
}

// Lookups

AActor* UCrowdyEntityManager::FindEntity(const FGuid& EntityID) const
{
    const FEntityEntry* Entry = IDToEntity.Find(EntityID);
    return Entry && Entry->Actor.IsValid() ? Entry->Actor.Get() : nullptr;
}

FGuid UCrowdyEntityManager::FindEntityID(const AActor* Entity) const
{
    const FGuid* ID = ActorToID.Find(Entity);
    return ID ? *ID : FGuid{};
}

bool UCrowdyEntityManager::IsLocallyOwned(const FGuid& EntityID) const
{
    const FEntityEntry* Entry = IDToEntity.Find(EntityID);
    return Entry && Entry->OwnerID == CachedLocalPlayerID;
}

bool UCrowdyEntityManager::IsLocallyOwned(const AActor* Entity) const
{
    return IsLocallyOwned(FindEntityID(Entity));
}

// Network Reception 

void UCrowdyEntityManager::OnMessageReceived(TSharedRef<ICrowdyMessage> Message)
{
    if (Message->GetType() != ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION)
        return;
    
    FGameEventNotification& GameEvent =
        static_cast<FGameEventNotification&>(*Message);
    
    AsyncTask(ENamedThreads::GameThread, [this, GameEvent = MoveTemp(GameEvent)]()
    {
        const UScriptStruct* StructType = GameEvent.State.GetScriptStruct();
        if (!StructType) return;

        // Lifecycle events
        if (StructType == FCrowdyEntitySpawnEvent::StaticStruct())
        {
            const FCrowdyEntitySpawnEvent* Event = GameEvent.State.GetPtr<FCrowdyEntitySpawnEvent>();
            if (Event) HandleRemoteSpawn(*Event);
            return;
        }
        if (StructType == FCrowdyEntityStateEvent::StaticStruct())
        {
            const FCrowdyEntityStateEvent* Event = GameEvent.State.GetPtr<FCrowdyEntityStateEvent>();
            if (Event) HandleRemoteStateChange(*Event);
            return;
        }
        if (StructType == FCrowdyEntityDestroyEvent::StaticStruct())
        {
            const FCrowdyEntityDestroyEvent* Event = GameEvent.State.GetPtr<FCrowdyEntityDestroyEvent>();
            if (Event) HandleRemoteDestroy(*Event);
            return;
        }

        // Custom events — whitelist check
        if (!SupportedStructTypes.Contains(StructType)) return;

        if (UCrowdyEntityEventHandler* Handler = FindHandler(GameEvent.State))
            Handler->OnObjectEvent(USerializationFunctionLibrary::ToGuid(GameEvent.UUID), GameEvent.State);
    });
}

TArray<ECrowdyMessageType> UCrowdyEntityManager::GetSupportedResponseTypes() const
{
    return { ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION };
}

TArray<UScriptStruct*> UCrowdyEntityManager::GetSupportedEvents() const
{
   return 
    {
       FCrowdyEntitySpawnEvent::StaticStruct(), 
       FCrowdyEntityStateEvent::StaticStruct(), 
       FCrowdyEntityDestroyEvent::StaticStruct()
   };
}

void UCrowdyEntityManager::SetHostID(const FGuid& HostID)
{
    CachedLocalPlayerID = HostID;
}

// ── Remote Handlers ──────────────────────────────────────────────────────────

void UCrowdyEntityManager::HandleRemoteSpawn(const FCrowdyEntitySpawnEvent& Event)
{
    
    if (IDToEntity.Contains(Event.EntityID))
    {
        UE_LOG(LogTemp, Error, TEXT("[CrowdyEntityManager]: Entity already spawned on remote spawn."));
        return;
    }
    
    if (!EntityRegistry.Get())
    {
        UE_LOG(LogTemp, Error, TEXT("[CrowdyEntityManager]: EntityRegistry is null on remote spawn."));
        return;
    }

    const FCrowdyEntityTypeEntry* Entry = EntityRegistry->FindByTypeID(Event.TypeID);
    if (!Entry)
    {
        UE_LOG(LogTemp, Error, TEXT("[CrowdyEntityManager]: Unknown TypeID %d in remote spawn."), Event.TypeID);
        return;
    }

    TSubclassOf<AActor> ActorClass = Entry->EntityActorClass.LoadSynchronous();
    if (!ActorClass) return;

    FActorSpawnParameters Params;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    AActor* Actor = GetWorld()->SpawnActor<AActor>(ActorClass, Event.SpawnTransform, Params);
    if (!IsValid(Actor)) return;

    RegisterEntityInternal(Event.EntityID, Event.OwnerID, Event.TypeID, Actor);

    if (UCrowdyEntityEventHandler* Handler = FindHandler(Event.TypeID))
        Handler->OnEntitySpawned(Actor, Event.InitialState, false);
}

void UCrowdyEntityManager::HandleRemoteStateChange(const FCrowdyEntityStateEvent& Event)
{
    const FEntityEntry* Entry = IDToEntity.Find(Event.EntityID);
    if (!Entry || !Entry->Actor.IsValid()) return;

    if (UCrowdyEntityEventHandler* Handler = FindHandler(Entry->TypeID))
        Handler->OnEntityStateChanged(Entry->Actor.Get(), Event.NewState);
}

void UCrowdyEntityManager::HandleRemoteDestroy(const FCrowdyEntityDestroyEvent& Event)
{
    const FEntityEntry* Entry = IDToEntity.Find(Event.EntityID);
    if (!Entry)
    {
        UE_LOG(LogTemp, Error, TEXT("[CrowdyEntityManager]: Entity already destroyed on remote destroy."));
        return;
    }
    
    AActor* Actor    = Entry->Actor.Get();
    const int32 TypeID = Entry->TypeID;

    float Delay = 0.f;
    if (UCrowdyEntityEventHandler* Handler = FindHandler(TypeID))
        Delay = Handler->OnEntityDestroyed(Actor, false);

    UnregisterEntityInternal(Event.EntityID);

    if (Delay > 0.f)
    {
        FTimerHandle Handle;
        GetWorld()->GetTimerManager().SetTimer(Handle, [Actor]()
        {
            if (IsValid(Actor)) Actor->Destroy();
        }, Delay, false);
    }
    else if (IsValid(Actor))
    {
        Actor->Destroy();
    }
}

//Internal

void UCrowdyEntityManager::RegisterEntityInternal(
    const FGuid& EntityID, const FGuid& OwnerID, const int32 TypeID, AActor* Actor)
{
    UE_LOG(LogTemp, Log, TEXT("[CrowdyEntityManager]: Registering Actor=%s, ID=%s"),
       *GetNameSafe(Actor), *EntityID.ToString());
    
    FEntityEntry Entry;
    Entry.EntityID = EntityID;
    Entry.OwnerID  = OwnerID;
    Entry.TypeID   = TypeID;
    Entry.Actor    = Actor;

    IDToEntity.Add(EntityID, Entry);
    ActorToID.Add(Actor, EntityID);
}

void UCrowdyEntityManager::UnregisterEntityInternal(const FGuid& EntityID)
{
    const FEntityEntry* Entry = IDToEntity.Find(EntityID);
    if (!Entry) return;

    if (Entry->Actor.IsValid())
        ActorToID.Remove(Entry->Actor.Get());

    IDToEntity.Remove(EntityID);
}

void UCrowdyEntityManager::BroadcastToNetwork(const AActor* Actor, FInstancedStruct& Payload)
{
    if (!IsValid(CachedSDK.Get()) || !IsValid(Actor))
    {
        UE_LOG(LogTemp, Error, TEXT("[CrowdyEntityManager]: CachedSDK or Actor is null."));
        return;
    }

    int64 ChunkX, ChunkY, ChunkZ;
    UHelperFunctions::GetChunkCoordinateAtLocation(this, Actor->GetActorLocation(), ChunkX, ChunkY, ChunkZ);

    CachedSDK->DispatchGameEvent(
        ChunkX, ChunkY, ChunkZ,
        ECrowdyDecayRate::No_Decay,
        ECrowdyReplicationDistance::Eight_Chunks,
        CachedLocalPlayerID,
        Payload,
        true);
}

UCrowdyEntityEventHandler* UCrowdyEntityManager::FindHandler(const FInstancedStruct& Payload) const
{
    const UScriptStruct* StructType = Payload.GetScriptStruct();
    if (!StructType) return nullptr;

    const TObjectPtr<UCrowdyEntityEventHandler>* Handler = EventHandlers.Find(StructType);
    return Handler ? Handler->Get() : nullptr;
}

UCrowdyEntityEventHandler* UCrowdyEntityManager::FindHandler(int32 TypeID) const
{
    const TObjectPtr<UCrowdyEntityEventHandler>* Handler = TypeIDToHandler.Find(TypeID);
    return Handler ? Handler->Get() : nullptr;
}

void UCrowdyEntityManager::LoadSupportedStructTypes()
{
    const UCrowdySDKDeveloperSettings* DevSettings = GetDefault<UCrowdySDKDeveloperSettings>();
    if (!DevSettings) return;

    const UEventPayloadType* Registry = DevSettings->EventPayloadDataAsset.LoadSynchronous();
    if (!Registry) return;

    for (const FEventPayloadTypeEntry& Entry : Registry->Entries)
    {
        if (!SupportedGameEvents.Contains(Entry.EventName)) continue;
        if (!IsValid(Entry.EventType)) continue;
        SupportedStructTypes.Add(Entry.EventType);
    }
}

bool UCrowdyEntityManager::LoadConfig()
{
    const UCrowdySDKDeveloperSettings* DevSettings = GetDefault<UCrowdySDKDeveloperSettings>();
    if (!IsValid(DevSettings))
    {
        UE_LOG(LogTemp, Error, TEXT("[CrowdyEntityManager]: DeveloperSettings is null."));
        return false;
    }

    // Resolve registry for current map
    const UWorld* World = GetWorld();
    if (!IsValid(World))
    {
        UE_LOG(LogTemp, Error, TEXT("[CrowdyEntityManager]: World is null in LoadConfig."));
        return false;
    }

    // Strip the UEDPIE_X_ prefix so PIE worlds match their editor asset path
    const FString WorldPath = UWorld::RemovePIEPrefix(World->GetPathName());

    const TSoftObjectPtr<UCrowdyEntityRegistry>* RegistryPtr = nullptr;
    for (const auto& Pair : DevSettings->EntityRegistryConfigs)
    {
        if (Pair.Key.ToSoftObjectPath().ToString() == WorldPath)
        {
            RegistryPtr = &Pair.Value;
            break;
        }
    }

    if (!RegistryPtr || RegistryPtr->IsNull())
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[CrowdyEntityManager]: No EntityRegistry mapped for world '%s', skipping."),
            *WorldPath);
        return false;
    }

    EntityRegistry = RegistryPtr->LoadSynchronous();
    if (!IsValid(EntityRegistry.Get()))
    {
        UE_LOG(LogTemp, Error,
            TEXT("[CrowdyEntityManager]: EntityRegistry failed to load for world '%s'. Asset path: %s"),
            *WorldPath,
            *RegistryPtr->ToString());
        return false;
    }

  
    // Build TypeID -> Handler map
   TypeIDToHandler.Empty();

    for (const FCrowdyEntityTypeEntry& Entry : EntityRegistry->EntityTypes)
    {
        if (Entry.TypeID < 0)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[CrowdyEntityManager]: Invalid TypeID on '%s'."),
                *Entry.DebugTypeName.ToString());
            continue;
        }

        TSubclassOf<UCrowdyEntityEventHandler> HandlerClass =
            Entry.HandlerClass.LoadSynchronous();

        if (!HandlerClass)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[CrowdyEntityManager]: Missing HandlerClass for '%s'."),
                *Entry.DebugTypeName.ToString());
            continue;
        }

        if (TypeIDToHandler.Contains(Entry.TypeID))
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[CrowdyEntityManager]: Duplicate TypeID '%d' on '%s', skipping."),
                Entry.TypeID,
                *Entry.DebugTypeName.ToString());
            continue;
        }

        UCrowdyEntityEventHandler* Handler =
            NewObject<UCrowdyEntityEventHandler>(this, HandlerClass);

        if (!IsValid(Handler))
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[CrowdyEntityManager]: Failed to create handler for TypeID '%d'."),
                Entry.TypeID);
            continue;
        }

        TypeIDToHandler.Add(Entry.TypeID, Handler);
    }
    
    return true;
}