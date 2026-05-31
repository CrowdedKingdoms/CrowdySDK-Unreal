// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "CrowdyUtilities.generated.h"

struct FInstancedStruct;
/**
 * 
 */
UCLASS(BlueprintType, meta=(DisplayName="Crowdy Utility Functions"))
class CROWDYSDK_API UCrowdyUtilities : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
	
public:
	
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Utilities|Entities", meta=(WorldContext="WorldContextObject",AutoCreateRefTerm="InitialState",Keywords="Spawn Create Crowdy Entity"))
	static AActor* SpawnCrowdyEntity(
		UObject* WorldContextObject,
		TSubclassOf<AActor> EntityActorClass, 
		const FTransform& SpawnTransform, 
		const FInstancedStruct& InitialState);

	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Utilities|Entities", meta=(WorldContext="WorldContextObject", DefaultToSelf="TargetEntity", AutoCreateRefTerm="NewState", Keywords="Update State Crowdy Entity"))
	static void UpdateCrowdyEntityState(
		UObject* WorldContextObject,
		AActor* TargetEntity,
		const FInstancedStruct& NewState);
	
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Utilities|Entities", meta=(WorldContext="WorldContextObject", DefaultToSelf="TargetEntity", Keywords="Destroy Crowdy Entity"))
	static void DestroyCrowdyEntity(
		UObject* WorldContextObject,
		AActor* TargetEntity);
	
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Utilities|Entities", meta=(WorldContext="WorldContextObject", DefaultToSelf="Entity", AdvancedDisplay="bUseDeterministicID,Seed", Keywords="Register Crowdy Entity Static"))
	static void RegisterStaticEntity(UObject* WorldContextObject, AActor* Entity, bool bUseDeterministicID, int64 Seed);
	
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Utilities|Entities", meta=(WorldContext="WorldContextObject", Keywords="Find Crowdy Entity"))
	static AActor* FindCrowdyEntity(UObject* WorldContextObject, bool& bIsValid, const FGuid& EntityID);
	
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Utilities|Entities", meta=(WorldContext="WorldContextObject", DefaultToSelf="Entity", Keywords="Find Crowdy Entity ID"))
	static FGuid FindCrowdyEntityID(UObject* WorldContextObject, bool& bIsValid, const AActor* Entity);
	
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy SDK|Utilities|Entities", meta=(WorldContext="WorldContextObject", DefaultToSelf="Entity", Keywords="Is Crowdy Entity Locally Owned"))
	static bool IsCrowdyEntityLocallyOwned(UObject* WorldContextObject, const AActor* Entity);
	
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Utilities|Actors", meta=(WorldContext="WorldContextObject", DefaultToSelf="Actor", Keywords="Find Get Crowdy Actor Entity"))
	static FGuid FindCrowdyActorID(UObject* WorldContextObject, const AActor* Actor, bool& bIsValid);
	
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Utilities|Actors", meta=(WorldContext="WorldContextObject", Keywords="Find Get Crowdy Actor Entity"))
	static AActor* FindCrowdyActor(UObject* WorldContextObject, const FGuid& ID, bool& bIsValid);
	
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Utilities|Actors", meta=(WorldContext="WorldContextObject", ExpandBoolAsExecs="bHasAuthority", Keywords="Crowdy Has Authority"))
	static void CrowdyHasAuthority(UObject* WorldContextObject, bool& bHasAuthority);
	
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Utilities|Actors", meta=(WorldContext="WorldContextObject", DefaultToSelf = "TargetActor" ,ExpandBoolAsExecs="bIsPlayerControlled", Keywords="Crowdy Is Player Controlled"))
	static void IsCrowdyActorPlayerControlled(UObject* WorldContextObject, AActor* TargetActor, bool& bIsPlayerControlled);
	
	// C++ Overload
	static bool CrowdyHasAuthority(const UObject* WorldContextObject);
	
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Utilities|Actors", meta=(WorldContext="WorldContextObject", DefaultToSelf="Object", Keywords="Crowdy Event Events Register"))
	static void RegisterForEventReception(UObject* WorldContextObject, UObject* Object);
	
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Utilities|Actors", meta=(WorldContext="WorldContextObject", DefaultToSelf="Object", Keywords="Crowdy Event Events Unregister"))
	static void UnregisterForEventReception(UObject* WorldContextObject, UObject* Object);
	
	UFUNCTION(BlueprintCallable, meta=(WorldContext="WorldContextObject", DisplayName="Dispatch Game Event", Keywords="Crowdy Event Events"))
	static void DispatchGameEvent(
		const UObject* WorldContextObject,
		int64 ChunkX,
		int64 ChunkY,
		int64 ChunkZ,
		ECrowdyDecayRate DecayRate,
		ECrowdyReplicationDistance ReplicationDistance,
		const FGuid& InstigatorID,
		FInstancedStruct EventPayload,
		bool bAsync = false);

	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Utilities|Connection", meta=(WorldContext="WorldContextObject", Keywords="Crowdy Connection Connected Server"))
	static bool CrowdyIsConnectedToServer(UObject* WorldContextObject);

	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Utilities|Session", meta=(WorldContext="WorldContextObject", Keywords="Crowdy Host Get ID"))
	static FGuid CrowdyGetHostID(UObject* WorldContextObject, bool& bIsValid);
};
