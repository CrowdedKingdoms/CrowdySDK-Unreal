// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "CrowdyModel.generated.h"

/**
 * Blueprint access to the Game Model (Server Owned) plane, mirroring UCrowdyUtilities: a function library
 * that takes the object you already have, never a new component. Entity is any registered participant - an
 * actor carrying a UCrowdyEntityComponent, or a UObject enrolled as a participant (a Host-owned subsystem) -
 * and its container is resolved automatically off the entity NetID (auto-bound when the entity registers), so
 * callers pass the object + a property key, never a raw container id or GraphQL. Reads come from the
 * subsystem's authoritative cache (a pull refreshes it); they are the last value the server confirmed, never a
 * local prediction.
 *
 * This library is bound to entities (actors and subsystems); free/data containers accessed by containerId
 * (inventories, quests) have their own function library (UCrowdyGameModel).
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyModel : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// Re-pull Entity's Server Owned container now, or right after a read of it already in flight lands. Each changed
	// attribute's OnRep fires. Use after a change you could not observe via a notification (rare the pipeline pulls for you).
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Model|Attributes", DisplayName = "Refresh Game Model")
	static void PullNow(UObject* Entity);

	// Typed reads of a Server Owned attribute by its key (the lowercased property name). Return Default when
	// the entity is not a bound container, the key is not cached yet, or the cached value is a different type.
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Attributes", DisplayName = "Get Model Attribute (Integer)")
	static int32 GetInt(const UObject* Entity, FName Key, int32 Default = 0);

	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Attributes", DisplayName = "Get Model Attribute (Float)")
	static float GetFloat(const UObject* Entity, FName Key, float Default = 0.0f);

	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Attributes", DisplayName = "Get Model Attribute (Boolean)")
	static bool GetBool(const UObject* Entity, FName Key, bool bDefault = false);

	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Attributes", DisplayName = "Get Model Attribute (String)")
	static FString GetString(const UObject* Entity, FName Key, const FString& Default = TEXT(""));

	// True when Entity has a resolved Server Owned container bound (auto-bind succeeded, or one was bound
	// explicitly). Lets a UI wait for the first authoritative pull before reading.
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Attributes", DisplayName = "Is Game Model Ready")
	static bool IsContainerBound(const UObject* Entity);

	// Returns Actor's CrowdyContainer component of ContainerClass (the attributes-component pattern), so an
	// actor-centric graph resolves the right sub-participant target without threading the component through by hand.
	// Null when Actor is null, ContainerClass is unset, or no such component exists. Feed the result as the
	// Entity/Target of the model getters and the Apply Crowdy Effect node so the write lands on the component's
	// container, not the actor's. Returns the FIRST component of that class; an actor carrying two of the same
	// container component (each a distinct sub-participant) must reference the specific component directly instead.
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Attributes", DisplayName = "Get Model Component")
	static UActorComponent* GetModelComponent(AActor* Actor, TSubclassOf<UActorComponent> ContainerClass);

	// Enroll a CrowdyContainer component ADDED AT RUNTIME so it binds its Game Model container. The automatic sweep
	// only runs when the actor first registers, so a component you attach later needs this call. Give a runtime-added
	// component a cross-client-stable identity by implementing ICrowdyBindingKeyProvider (Get Crowdy Binding Key);
	// without a key its container id is not stable across clients. No-op when Component is null, its owner is not a
	// registered Crowdy entity yet, or it is not a bindable container.
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Model|Attributes", DisplayName = "Enroll Model Component")
	static void EnrollModelComponent(UActorComponent* Component);

	// The counterpart to Enroll Model Component: call when removing a runtime-added container component (while its
	// actor lives on) so its Game Model binding and record are dropped instead of leaking. No-op if it was never
	// enrolled, or if its actor is tearing down anyway (the actor teardown handles it).
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Model|Attributes", DisplayName = "Unenroll Model Component")
	static void UnenrollModelComponent(UActorComponent* Component);

	// Invoke a Server Owned function against Entity's container (fire-and-forget). The server evaluates the
	// rules transactionally; on success the confirmed result echoes into the cache + OnRep here and peers are
	// notified to re-pull. A latent node with Success/Failed pins + params is planned; this is the minimal
	// no-parameter entry so Blueprints can drive a function today.
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Model|Advanced", DisplayName = "Call Model Function (Fire and Forget)")
	static void Invoke(UObject* Entity, FName FunctionName);
};
