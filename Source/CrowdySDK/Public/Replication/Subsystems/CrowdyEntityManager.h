#pragma once

#include "CoreMinimal.h"
#include "Core/UDP/Interfaces/ICrowdyReceptionLayer.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystems/WorldSubsystem.h"
#include "Data/CrowdyEntityRegistry.h"
#include "CrowdyEntityManager.generated.h"

struct FCrowdyEntityDestroyEvent;
struct FCrowdyEntityStateEvent;
struct FCrowdyEntitySpawnEvent;

class UCrowdySDKSubsystem;
class UCrowdyGameSession;
class UCrowdyEntityEventHandler;

UCLASS(BlueprintType, meta=(DisplayName="Crowdy Entity Manager"))
class CROWDYSDK_API UCrowdyEntityManager : public UWorldSubsystem, public ICrowdyReceptionLayer
{
    GENERATED_BODY()

public:
   
    // Subsystem Overrides
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    virtual bool ShouldCreateSubsystem(UObject* Outer) const override;

    // ICrowdyReceptionLayer Implementation
    virtual void OnMessageReceived(TSharedRef<ICrowdyMessage> Message) override;
    virtual TArray<ECrowdyMessageType> GetSupportedResponseTypes() const override;
    virtual TArray<UScriptStruct*> GetSupportedEvents() const override;
    
    UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Crowdy Entity Manager")
    void SetHostID(const FGuid& HostID);
    
    AActor* SpawnCrowdyEntity(TSubclassOf<AActor> EntityClass, const FTransform& SpawnTransform, const FInstancedStruct& InitialState);
    void UpdateCrowdyEntityState(AActor* TargetEntity, const FInstancedStruct& NewState);
    void DestroyCrowdyEntity(AActor* TargetEntity);
    void RegisterStaticEntity(AActor* Entity, bool bUseDeterministicID, int64 Seed);
    AActor* FindEntity(const FGuid& EntityID) const;
    FGuid FindEntityID(const AActor* Entity) const;
    bool IsLocallyOwned(const AActor* Entity) const;
    bool IsLocallyOwned(const FGuid& EntityID) const;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Crowdy Entity Manager")
    TArray<FName> SupportedGameEvents;
    
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Crowdy Entity Manager")
    TArray<UScriptStruct*> SupportedEvents;

private:
   
    struct FEntityEntry
    {
        FGuid                  EntityID;
        FGuid                  OwnerID;
        int32                  TypeID = 0;
        TWeakObjectPtr<AActor> Actor;
    };

    TMap<FGuid, FEntityEntry>  IDToEntity;

    UPROPERTY()
    TMap<AActor*, FGuid> ActorToID;

    UPROPERTY()
    TObjectPtr<UCrowdyGameSession> CrowdyGameSession;

    UPROPERTY()
    FGuid CachedLocalPlayerID;

    UPROPERTY()
    TMap<UScriptStruct*, TObjectPtr<UCrowdyEntityEventHandler>> EventHandlers;

    // TypeID → handler for lifecycle routing
    UPROPERTY()
    TMap<int32, TObjectPtr<UCrowdyEntityEventHandler>> TypeIDToHandler;

    UPROPERTY()
    TSet<TObjectPtr<UScriptStruct>> SupportedStructTypes;

    UPROPERTY()
    TObjectPtr<UCrowdyEntityRegistry> EntityRegistry;

    UPROPERTY()
    TObjectPtr<UCrowdySDKSubsystem> CachedSDK;

private:
    
    void RegisterEntityInternal(const FGuid& EntityID, const FGuid& OwnerID, int32 TypeID, AActor* Actor);
    void UnregisterEntityInternal(const FGuid& EntityID);
    void BroadcastToNetwork(const AActor* Actor, FInstancedStruct& Payload);

    void HandleRemoteSpawn(const FCrowdyEntitySpawnEvent& Event);
    void HandleRemoteStateChange(const FCrowdyEntityStateEvent& Event);
    void HandleRemoteDestroy(const FCrowdyEntityDestroyEvent& Event);

    UCrowdyEntityEventHandler* FindHandler(const FInstancedStruct& Payload) const;
    UCrowdyEntityEventHandler* FindHandler(int32 TypeID) const;

    void LoadSupportedStructTypes();
    bool LoadConfig();
};