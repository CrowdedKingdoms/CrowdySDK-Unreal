// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Data/CrowdyEntityTypes.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "StructUtils/InstancedStruct.h"
#include "CrowdyUtilities.generated.h"

// ECrowdyEventRecipient now lives in CrowdyNet alongside ECrowdyDecayRate and
// ECrowdyReplicationDistance (see Core/UDP/Enums/ECrowdyMessageType.h, included
// above) so lower modules can reference it in RPC routing metadata.

/**
 * Blueprint-facing surface of the Crowdy SDK.
 * Covers entity spawning/destroying, actor↔NetID resolution, event sending,
 * and session helpers.
 */
UCLASS(BlueprintType, meta=(DisplayName="Crowdy Utility Functions"))
class CROWDYSERVICES_API UCrowdyUtilities : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:

	// Entity Lifecycle

	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Entities",
		meta=(WorldContext="WorldContextObject", AutoCreateRefTerm="InitialState",
		      Keywords="Spawn Create Crowdy Entity"))
	static AActor* SpawnCrowdyEntity(UObject* WorldContextObject,
		TSubclassOf<AActor> EntityActorClass,
		const FTransform& SpawnTransform,
		const FInstancedStruct& InitialState);

	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Entities",
		meta=(WorldContext="WorldContextObject", DefaultToSelf="TargetEntity",
		      Keywords="Destroy Remove Crowdy Entity"))
	static void DestroyCrowdyEntity(UObject* WorldContextObject, AActor* TargetEntity);

	//Lookups

	/** Returns the Crowdy NetID for the given actor. */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Entities",
		meta=(WorldContext="WorldContextObject", DefaultToSelf="Actor",
		      Keywords="Get Find Crowdy Entity ID UUID"))
	static FGuid GetCrowdyEntityID(UObject* WorldContextObject, const AActor* Actor, bool& bIsValid);

	/** Returns the actor registered under the given Crowdy NetID. */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Entities",
		meta=(WorldContext="WorldContextObject",
		      Keywords="Get Find Crowdy Entity Actor"))
	static AActor* GetCrowdyEntity(UObject* WorldContextObject, const FGuid& ID, bool& bIsValid);

	/** Returns the local player's own entity UUID. */
	UFUNCTION(BlueprintPure, Category="Crowdy SDK|Entities",
		meta=(WorldContext="WorldContextObject",
		      Keywords="Get Local Player Entity ID UUID"))
	static FGuid GetLocalPlayerEntityID(UObject* WorldContextObject);

	/** Returns the UUID of the player that owns this entity. Invalid for world-static entities. */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Entities",
		meta=(WorldContext="WorldContextObject", DefaultToSelf="Entity",
		      Keywords="Get Crowdy Entity Owner ID UUID"))
	static FGuid GetCrowdyEntityOwnerID(UObject* WorldContextObject, const AActor* Entity, bool& bIsValid);

	/** Returns all entity actors whose owner UUID matches the given player ID. */
	UFUNCTION(BlueprintPure, Category="Crowdy SDK|Entities",
		meta=(WorldContext="WorldContextObject",
		      Keywords="Get All Entities By Owner Crowdy"))
	static TArray<AActor*> GetAllCrowdyEntitiesByOwner(UObject* WorldContextObject, const FGuid& OwnerID);

	/** True when the given UUID is currently registered in the entity registry. */
	UFUNCTION(BlueprintPure, Category="Crowdy SDK|Entities",
		meta=(WorldContext="WorldContextObject",
		      Keywords="Is Crowdy Entity Registered Valid"))
	static bool CrowdyIsEntityRegistered(UObject* WorldContextObject, const FGuid& EntityID);

	/** Returns the replication role of this entity (Owner, RemoteProxy, HostOwned, or None). */
	UFUNCTION(BlueprintPure, Category="Crowdy SDK|Entities",
		meta=(WorldContext="WorldContextObject", DefaultToSelf="Entity",
		      Keywords="Get Crowdy Entity Role Owner Proxy"))
	static ECrowdyRole GetCrowdyEntityRole(UObject* WorldContextObject, const AActor* Entity);

	
	//Entity State Checks

	/** Exec-flow version: routes to IsPlayerControlled or IsNotPlayerControlled. */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Entities",
		meta=(WorldContext="WorldContextObject", DefaultToSelf="TargetActor",
		      ExpandBoolAsExecs="bIsPlayerControlled",
		      Keywords="Switch Is Player Controlled Crowdy Entity"))
	static void SwitchIsCrowdyEntityPlayerControlled(UObject* WorldContextObject,
		AActor* TargetActor, bool& bIsPlayerControlled);

	UFUNCTION(BlueprintPure, Category="Crowdy SDK|Entities",
		meta=(WorldContext="WorldContextObject", DefaultToSelf="TargetActor",
		      Keywords="Is Player Controlled Crowdy Entity"))
	static bool IsCrowdyEntityPlayerControlled(UObject* WorldContextObject, AActor* TargetActor);

	/** Exec-flow version: routes to IsRemoteProxy or IsNotRemoteProxy. */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Entities",
		meta=(WorldContext="WorldContextObject", DefaultToSelf="Entity",
		      ExpandBoolAsExecs="bIsRemoteProxy",
		      Keywords="Switch Is Remote Proxy Crowdy Entity"))
	static void SwitchCrowdyIsRemoteProxy(UObject* WorldContextObject,
		AActor* Entity, bool& bIsRemoteProxy);

	UFUNCTION(BlueprintPure, Category="Crowdy SDK|Entities",
		meta=(WorldContext="WorldContextObject", DefaultToSelf="Entity",
		      Keywords="Is Remote Proxy Crowdy Entity"))
	static bool CrowdyIsRemoteProxy(UObject* WorldContextObject, const AActor* Entity);

	// Events

	/**
	 * Sends a game event addressed at TargetEntity. The sender is always the local
	 * player (derived from the game session). DecayRate and ReplicationDistance are
	 * server-side spatial filters applied before the event reaches remote clients.
	 * The EventPayload pin accepts any USTRUCT directly.
	 */
	UFUNCTION(BlueprintCallable, CustomThunk, Category="Crowdy SDK|Events",
		meta=(WorldContext="WorldContextObject", DefaultToSelf="TargetEntity",
		      DisplayName="Send Crowdy Event", CustomStructureParam="EventPayload",
		      Keywords="Send Crowdy Event Entity"))
	static void K2_SendCrowdyEvent(
		UObject* WorldContextObject,
		AActor* TargetEntity,
		const int32& EventPayload,
		ECrowdyEventRecipient Recipient = ECrowdyEventRecipient::SpatialMulticast,
		ECrowdyDecayRate DecayRate = ECrowdyDecayRate::No_Decay,
		ECrowdyReplicationDistance ReplicationDistance = ECrowdyReplicationDistance::Eight_Chunks);
	DECLARE_FUNCTION(execK2_SendCrowdyEvent);

	/** C++ overload — pass any USTRUCT directly. */
	template<typename T>
	static void SendCrowdyEvent(
		UObject* WorldContextObject,
		AActor* TargetEntity,
		const T& EventPayload,
		ECrowdyEventRecipient Recipient = ECrowdyEventRecipient::SpatialMulticast,
		ECrowdyDecayRate DecayRate = ECrowdyDecayRate::No_Decay,
		ECrowdyReplicationDistance ReplicationDistance = ECrowdyReplicationDistance::Eight_Chunks)
	{
		SendCrowdyEvent_Internal(WorldContextObject, TargetEntity,
			FInstancedStruct::Make<T>(EventPayload), Recipient, DecayRate, ReplicationDistance);
	}

	// Session

	/** Exec-flow version: routes to HasAuthority or DoesNotHaveAuthority. */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Session",
		meta=(WorldContext="WorldContextObject", ExpandBoolAsExecs="bHasAuthority",
		      Keywords="Crowdy Has Authority Switch"))
	static void CrowdyHasAuthority(UObject* WorldContextObject, bool& bHasAuthority);

	/** C++ overload — returns true when this client is the host. */
	static bool CrowdyHasAuthority(const UObject* WorldContextObject);

	UFUNCTION(BlueprintPure, Category="Crowdy SDK|Session",
		meta=(WorldContext="WorldContextObject",
		      Keywords="Crowdy Is Connected Server"))
	static bool CrowdyIsConnectedToServer(UObject* WorldContextObject);

	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Session",
		meta=(WorldContext="WorldContextObject",
		      Keywords="Crowdy Get Host ID"))
	static FGuid CrowdyGetHostID(UObject* WorldContextObject, bool& bIsValid);

private:

	static void SendCrowdyEvent_Internal(
		UObject* WorldContextObject,
		AActor* TargetEntity,
		FInstancedStruct EventPayload,
		ECrowdyEventRecipient Recipient,
		ECrowdyDecayRate DecayRate,
		ECrowdyReplicationDistance ReplicationDistance);
};
