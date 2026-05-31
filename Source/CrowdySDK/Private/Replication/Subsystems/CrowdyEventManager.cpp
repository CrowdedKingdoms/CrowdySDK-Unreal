// Fill out your copyright notice in the Description page of Project Settings.


#include "Replication/Subsystems/CrowdyEventManager.h"
#include "EngineUtils.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Messages/GameObjects/FCrowdyEntitySpawnEvent.h"
#include "Messages/GameObjects/FGameEventNotification.h"
#include "Replication/Interfaces/CrowdyEventReceiver.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystem/CrowdyAutoRegistry.h"
#include "Subsystem/CrowdySDKSubsystem.h"
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "Utils/UEventPayloadRegistry.h"

void UCrowdyEventManager::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    
    SDK = GetWorld()->GetGameInstance()->GetSubsystem<UCrowdySDKSubsystem>();
    
    if (!IsValid(SDK.Get()))
    {
        UE_LOG(LogTemp, Error, TEXT("[CrowdyEventManager]: Invalid CrowdySDK subsystem."));
        return;   
    }
    
    if (!LoadConfig())
    {
        UE_LOG(LogTemp, Error, TEXT("[CrowdyEventManager]: Failed to load config due to incorrect params or disabled by choice."));
        return;  
    }
    
    SDK->RegisterReceptionLayer(this);
    
    if (UWorld* World = GetWorld())
    {
        World->AddOnActorSpawnedHandler(
            FOnActorSpawned::FDelegate::CreateUObject(
                this, &UCrowdyEventManager::OnActorSpawned));
        
        World->GetTimerManager().SetTimer(
          InitialScanTimerHandle,
          this,
          &UCrowdyEventManager::ScanExistingObjects,
          3.0f,   // adjust if needed
          false); // one-shot
    }
    
}

void UCrowdyEventManager::Deinitialize()
{
    HandlerMap.Empty();
    ClassScanCache.Empty();
    PendingRemovals.Empty();
    Super::Deinitialize();
}

void UCrowdyEventManager::RegisterHandler(UObject* Object)
{
	if (!IsValid(Object)) return;
	ensure(IsInGameThread());
	ScanAndBindObject(Object);
}

void UCrowdyEventManager::UnregisterHandler(UObject* Object)
{
	if (!Object) return;
	ensure(IsInGameThread());

	if (DispatchDepth > 0)
	{
		PendingRemovals.AddUnique(Object);
		return;
	}

	RemoveObjectInternal(Object);
}

void UCrowdyEventManager::DispatchEvent(const FInstancedStruct& Payload)
{
	ensure(IsInGameThread());

    const UScriptStruct* StructType = Payload.GetScriptStruct();
    if (!StructType) return;

    FCrowdyTypeID TypeID;
    if (!UEventPayloadRegistry::Get()->GetID(StructType, TypeID)) return;

    TArray<FCrowdyEventHandlerEntry>* Entries = HandlerMap.Find(TypeID);
    if (!Entries) return;

    ++DispatchDepth;

    for (FCrowdyEventHandlerEntry& Entry : *Entries)
    {
        UObject* Object = Entry.Object.Get();
        if (!IsValid(Object)) continue;

        // Build the parameter buffer for ProcessEvent.
        // The function signature is void Func(const FMyStruct& Param).
        // ProcessEvent expects a contiguous buffer matching the
        // UFunction's parameter layout — in this case, one struct instance.
        //
        // We allocate on the stack using the struct's size.
        // InitializeStruct zeroes + constructs, CopyScriptStruct copies
        // the payload data in, then ProcessEvent fires the function,
        // then DestroyStruct cleans up any UObject refs or strings.

        const int32 ParamsSize = Entry.Function->ParmsSize;
        void* ParamBuffer = FMemory_Alloca(ParamsSize);
        FMemory::Memzero(ParamBuffer, ParamsSize);

        // Find the actual parameter property to copy into
        FStructProperty* ParamProp = nullptr;
        for (TFieldIterator<FProperty> PropIt(Entry.Function); PropIt; ++PropIt)
        {
            if (PropIt->HasAnyPropertyFlags(CPF_Parm) &&
                !PropIt->HasAnyPropertyFlags(CPF_ReturnParm))
            {
                ParamProp = CastField<FStructProperty>(*PropIt);
                break;
            }
        }

        if (!ParamProp)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[CrowdyEventManager] Could not find param property on '%s'"),
                *Entry.Function->GetName());
            continue;
        }

        // Initialize the struct in the buffer, copy payload data into it
        ParamProp->Struct->InitializeStruct(ParamBuffer);
        ParamProp->Struct->CopyScriptStruct(ParamBuffer, Payload.GetMemory());

        Object->ProcessEvent(Entry.Function, ParamBuffer);

        // Destroy — releases any FString / TArray / UObject ref members
        ParamProp->Struct->DestroyStruct(ParamBuffer);
    }

    --DispatchDepth;

    if (DispatchDepth == 0)
        FlushPendingRemovals();
}

void UCrowdyEventManager::Tick(float DeltaTime)
{
    if (EventQueue.IsEmpty() || HandlerMap.IsEmpty())
        return;
    
    FInstancedStruct Event;
    while (EventQueue.Dequeue(Event))
    {
        DispatchEvent(Event);
    }
}

bool UCrowdyEventManager::ShouldCreateSubsystem(UObject* Outer) const
{
    if (!Super::ShouldCreateSubsystem(Outer))
        return false;

    const UWorld* World = Outer ? Outer->GetWorld() : nullptr;
    
    if (!World)
        return false;

    return (World->WorldType == EWorldType::PIE || World->WorldType == EWorldType::Game);
}

void UCrowdyEventManager::OnMessageReceived(TSharedRef<ICrowdyMessage> Message)
{
    if (Message->GetType() != ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION)
        return;
    
    const auto& EventMessage = static_cast<const FGameEventNotification&>(*Message);
    
    if (EventMessage.State.GetScriptStruct() == nullptr)
        return;
    
    // We don't want to process entity events here at all
    if (EventMessage.State.GetScriptStruct() == FCrowdyEntitySpawnEvent::StaticStruct() 
        || EventMessage.State.GetScriptStruct() == FCrowdyEntityDestroyEvent::StaticStruct() 
        || EventMessage.State.GetScriptStruct() == FCrowdyEntityStateEvent::StaticStruct())
    {
        return;
    }
    
    // Enqueue any other event here
    EventQueue.Enqueue(EventMessage.State);
}

TArray<ECrowdyMessageType> UCrowdyEventManager::GetSupportedResponseTypes() const
{
  return {ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION};
}

TArray<UScriptStruct*> UCrowdyEventManager::GetSupportedEvents() const
{
    TArray<UScriptStruct*> Events = GetWorld()->GetGameInstance()->GetSubsystem<UCrowdyAutoRegistry>()->GetAllRegisteredGameEvents();
    
    if (!Events.IsEmpty())
        return Events;
    
    UE_LOG(LogTemp, Error, TEXT("[CrowdyEventManager] No events registered in UCrowdyAutoRegistry"));
    return {};
}



void UCrowdyEventManager::ScanAndBindObject(UObject* Object)
{
	const UClass* Class = Object->GetClass();

    // Walk the full class hierarchy so inherited UFUNCTIONs are included
    for (TFieldIterator<UFunction> It(Class); It; ++It)
    {
        UFunction* Function = *It;
        if (!Function->HasMetaData(FName("CrowdyEvent"))) continue;

        UScriptStruct* ParamStruct = ResolveHandlerStruct(Function);
        if (!ParamStruct)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[CrowdyEventManager] '%s::%s' is tagged CrowdyEvent but has "
                     "no valid struct parameter — skipped. Expected signature: "
                     "void FuncName(const FMyStruct& Param)"),
                *Class->GetName(), *Function->GetName());
            continue;
        }

        FCrowdyTypeID TypeID;
        if (!UEventPayloadRegistry::Get()->GetID(ParamStruct, TypeID))
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[CrowdyEventManager] '%s::%s' — parameter struct '%s' is not "
                     "registered in UEventPayloadRegistry. "
                     "Add meta=(CrowdyCategory=\"Event\") to the struct."),
                *Class->GetName(), *Function->GetName(), *ParamStruct->GetName());
            continue;
        }

        FCrowdyEventHandlerEntry Entry;
        Entry.Object      = Object;
        Entry.Function    = Function;
        Entry.ParamStruct = ParamStruct;

        TArray<FCrowdyEventHandlerEntry>& Entries = HandlerMap.FindOrAdd(TypeID);

        // Deduplicate — same object + same function = same binding
        const bool bAlreadyBound = Entries.ContainsByPredicate(
            [&](const FCrowdyEventHandlerEntry& E)
            {
                return E.Object == Object && E.Function == Function;
            });

        if (!bAlreadyBound)
        {
            Entries.Add(MoveTemp(Entry));
            UE_LOG(LogTemp, Verbose,
                TEXT("[CrowdyEventManager] Bound '%s::%s' -> struct '%s' (TypeID=%d)"),
                *Class->GetName(), *Function->GetName(),
                *ParamStruct->GetName(), TypeID);
        }
    }
}

void UCrowdyEventManager::FlushPendingRemovals()
{
    for (const TWeakObjectPtr<UObject>& Weak : PendingRemovals)
        if (UObject* Obj = Weak.Get())
            RemoveObjectInternal(Obj);

    PendingRemovals.Reset();
}

void UCrowdyEventManager::RemoveObjectInternal(UObject* Object)
{
    for (auto& Pair : HandlerMap)
    {
        Pair.Value.RemoveAll([Object](const FCrowdyEventHandlerEntry& E)
        {
            return !E.Object.IsValid() || E.Object.Get() == Object;
        });
    }
}

bool UCrowdyEventManager::HasAnyCrowdyEventFunctions(UClass* Class)
{
    if (!Class) return false;

    if (const bool* Cached = ClassScanCache.Find(Class))
        return *Cached;

    bool bFound = false;
    for (TFieldIterator<UFunction> It(Class); It; ++It)
    {
        if ((*It)->HasMetaData(FName("CrowdyEvent")))
        {
            bFound = true;
            break;
        }
    }

    ClassScanCache.Add(Class, bFound);
    UE_LOG(LogTemp, Log, TEXT("Added %s to ClassScanCache"), *Class->GetName());
    return bFound;
}

void UCrowdyEventManager::ScanExistingObjects()
{
    UWorld* World = GetWorld();
    if (!World) return;

    int32 TotalFound = 0;

    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Actor = *It;
        if (!IsValid(Actor)) continue;

        if (Actor->Implements<UCrowdyEventReceiver>())
        {
            UE_LOG(LogTemp, Log,
                TEXT("[CrowdyEventManager] ScanExistingObjects: found actor '%s'"),
                *Actor->GetName());
            RegisterHandler(Actor);
            ++TotalFound;
        }

        for (UActorComponent* Component : Actor->GetComponents())
        {
            if (IsValid(Component) && Component->Implements<UCrowdyEventReceiver>())
            {
                UE_LOG(LogTemp, Log,
                    TEXT("[CrowdyEventManager] ScanExistingObjects: found component '%s' on '%s'"),
                    *Component->GetName(), *Actor->GetName());
                RegisterHandler(Component);
                ++TotalFound;
            }
        }
    }

    if (UGameInstance* GI = World->GetGameInstance())
    {
        for (UGameInstanceSubsystem* Subsystem :
            GI->GetSubsystemArrayCopy<UGameInstanceSubsystem>())
        {
            if (IsValid(Subsystem) && Subsystem->Implements<UCrowdyEventReceiver>())
            {
                UE_LOG(LogTemp, Log,
                    TEXT("[CrowdyEventManager] ScanExistingObjects: found GI subsystem '%s'"),
                    *Subsystem->GetClass()->GetName());
                RegisterHandler(Subsystem);
                ++TotalFound;
            }
        }
    }

    for (UWorldSubsystem* Subsystem :
        World->GetSubsystemArrayCopy<UWorldSubsystem>())
    {
        if (IsValid(Subsystem) && Subsystem->Implements<UCrowdyEventReceiver>())
        {
            UE_LOG(LogTemp, Log,
                TEXT("[CrowdyEventManager] ScanExistingObjects: found world subsystem '%s'"),
                *Subsystem->GetClass()->GetName());
            RegisterHandler(Subsystem);
            ++TotalFound;
        }
    }

    UE_LOG(LogTemp, Log,
        TEXT("[CrowdyEventManager] ScanExistingObjects complete — %d receivers found."),
        TotalFound);
}

void UCrowdyEventManager::OnActorSpawned(AActor* Actor)
{
    if (!IsValid(Actor)) return;

    if (Actor->Implements<UCrowdyEventReceiver>())
        RegisterHandler(Actor);

    for (UActorComponent* Component : Actor->GetComponents())
    {
        if (IsValid(Component) &&
            Component->Implements<UCrowdyEventReceiver>())
            RegisterHandler(Component);
    }
}

UScriptStruct* UCrowdyEventManager::ResolveHandlerStruct(UFunction* Function)
{
    // Inspect the first non-return parameter.
    // Valid signature: void Func(const FMyStruct& Param)
    // We accept by value or by const ref both appear as FStructProperty.
    for (TFieldIterator<FProperty> It(Function); It; ++It)
    {
        if (!It->HasAnyPropertyFlags(CPF_Parm))      continue;
        if (It->HasAnyPropertyFlags(CPF_ReturnParm)) continue;

        const FStructProperty* StructProp = CastField<FStructProperty>(*It);
        if (!StructProp)
        {
            // First param exists but isn't a struct — not a valid handler
            return nullptr;
        }

        return StructProp->Struct;
    }

    return nullptr; // no parameters
}

bool UCrowdyEventManager::LoadConfig() const
{
    const UCrowdySDKDeveloperSettings* DevSettings = GetDefault<UCrowdySDKDeveloperSettings>();
    
    if (!IsValid(DevSettings))
    {
        UE_LOG(LogTemp, Error, TEXT("[CrowdyEventManager]: DeveloperSettings is null."));
        return false;
    }

    const UWorld* World = GetWorld();
    if (!IsValid(World))
        return false;

    FString CurrentMap = FPackageName::GetShortName(World->GetOutermost()->GetName());

#if WITH_EDITOR
    if (World->WorldType == EWorldType::Editor || World->WorldType == EWorldType::PIE)
        CurrentMap = World->RemovePIEPrefix(CurrentMap);
#endif

    for (const auto& [WorldPtr, bAllowed] : DevSettings->EventManagerAllowedMaps)
    {
        if (WorldPtr.IsNull())
            continue;

        if (FPackageName::GetShortName(WorldPtr.GetAssetName()) != CurrentMap)
            continue;

        return bAllowed;
    }

    return false;
}
