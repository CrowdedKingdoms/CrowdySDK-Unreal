// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Data/CrowdyActorPoolPolicy.h"
#include "Data/FCrowdyPoolConfig.h"
#include "Subsystems/WorldSubsystem.h"
#include "CrowdyActorPoolSubsystem.generated.h"

USTRUCT()
struct FSlot
{
	GENERATED_BODY()
	
	TWeakObjectPtr<AActor> Actor;
	FGuid UUID;
	bool bActive = false;
};


USTRUCT()
struct FPool
{
	GENERATED_BODY()
	
	TSubclassOf<AActor> ActorClass;
	
	UPROPERTY()
	TObjectPtr<UCrowdyActorPoolPolicy> Policy;
	
	TArray<FSlot> Slots;
};

/**
 * 
 */
UCLASS(BlueprintType, meta=(DisplayName="Crowdy Actor Pool Subsystem"))
class CROWDYSDK_API UCrowdyActorPoolSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()
public:
	
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	
	/**
	 * Register a pool. Call this once at startup (GameMode::BeginPlay, etc.)
	 * Multiple pools can exist, one per actor class.
	 */
	UFUNCTION(BlueprintCallable, Category="Actor Pool")
	void RegisterPool(const FCrowdyPoolConfig& Config);

	/** Check out an actor, bind it to a UUID, call OnActorActivated. */
	UFUNCTION(BlueprintCallable, Category="Actor Pool")
	AActor* AcquireActor(TSubclassOf<AActor> ActorClass, const FGuid& ID, const FInstancedStruct& InitialState);

	/** Return an actor to the pool by UUID, call OnActorDeactivated. */
	UFUNCTION(BlueprintCallable, Category="Actor Pool")
	void ReleaseActor(const FGuid& ID);

	/** Direct UUID→Actor lookup. Returns null if UUID not active. */
	UFUNCTION(BlueprintCallable, Category="Actor Pool")
	AActor* FindActor(const FGuid& ID, bool& bIsValid);
	
	//C++ overload
	AActor* FindActor(const FGuid& ID);
	
	UFUNCTION(BlueprintCallable, Category="Actor Pool")
	FGuid FindActorID(bool& bIsValid ,const AActor* Actor);
	
	//C++ Overload
	FGuid FindActorID(const AActor* Actor);
	
	/*Convenience, find and cast. Returns null if not found or wrong type.*/
	template<typename T>
	T* FindActorAs(const FGuid& ID, bool& bIsValid)
	{
		return Cast<T>(FindActor(ID, bIsValid));
	}
	
private:
	
	UPROPERTY()
	TMap<TSubclassOf<AActor>, FPool> Pools;
	
	UPROPERTY()
	TMap<FGuid, TWeakObjectPtr<AActor>> IDToActor;
	
	UPROPERTY()
	TMap<TObjectPtr<AActor>, FGuid> ActorToID;
	
	FPool* FindPool(const UClass* ActorClass);
};
