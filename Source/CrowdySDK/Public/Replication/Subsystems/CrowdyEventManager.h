// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Core/FCrowdyTypeID.h"
#include "Core/UDP/Interfaces/ICrowdyReceptionLayer.h"
#include "Messages/GameObjects/FCrowdyEntitySpawnEvent.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/UObjectArray.h"
#include "CrowdyEventManager.generated.h"


class UCrowdySDKSubsystem;
struct FInstancedStruct;

struct FCrowdyEventHandlerEntry
{
	TWeakObjectPtr<UObject> Object;
	UFunction* Function = nullptr;
	UScriptStruct* ParamStruct = nullptr;
};

/**
 * 
 */
UCLASS()
class CROWDYSDK_API UCrowdyEventManager : public UTickableWorldSubsystem, public ICrowdyReceptionLayer
{
	GENERATED_BODY()
	
public:
	
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	
	// Register any UObject — actor, component, subsystem, anything
	void RegisterHandler(UObject* Object);
	void UnregisterHandler(UObject* Object);

	// Called by your existing event reception layer with the deserialized payload
	void DispatchEvent(const FInstancedStruct& Payload);

	// UTickableWorldSubsystem
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(UCrowdyEventDispatcher, STATGROUP_Tickables);
	}
	
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	
	// Reception Layer API
	virtual void OnMessageReceived(TSharedRef<ICrowdyMessage> Message) override;
	virtual TArray<ECrowdyMessageType> GetSupportedResponseTypes() const override;
	virtual TArray<UScriptStruct*> GetSupportedEvents() const override;
	
	

private:
	
	UPROPERTY()
	TObjectPtr<UCrowdySDKSubsystem> SDK;
	
	// Queue for events
	TQueue<FInstancedStruct, EQueueMode::Mpsc> EventQueue;
	
	// TypeID -> array of handlers for that struct type
	TMap<FCrowdyTypeID, TArray<FCrowdyEventHandlerEntry>> HandlerMap;

	// Reentrance guard — deferred removal during dispatch
	int32 DispatchDepth = 0;
	TArray<TWeakObjectPtr<UObject>> PendingRemovals;

	UPROPERTY()
	TMap<UClass*, bool> ClassScanCache;

	FTimerHandle InitialScanTimerHandle;
	
	void ScanAndBindObject(UObject* Object);
	void FlushPendingRemovals();
	void RemoveObjectInternal(UObject* Object);
	
	bool HasAnyCrowdyEventFunctions(UClass* Class);
	void ScanExistingObjects();
	void OnActorSpawned(AActor* Actor);
	
	// Resolves the struct type from a UFunction's first parameter.
	// Returns null if the function isn't a valid CrowdyEvent handler.
	static UScriptStruct* ResolveHandlerStruct(UFunction* Function);
	bool LoadConfig() const;
};
