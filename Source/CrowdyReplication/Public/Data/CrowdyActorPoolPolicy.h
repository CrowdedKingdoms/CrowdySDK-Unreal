// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Object.h"
#include "CrowdyActorPoolPolicy.generated.h"

class AActor;
class APawn;
class UCharacterMovementComponent;

namespace CrowdyActorPool
{
	/** Stops every pawn movement component on a proxy, whose transform comes only from replicated updates. */
	CROWDYREPLICATION_API void DisableProxyMovement(AActor* Actor);

	/** Gives pawn movement back to an actor that was a proxy, once this client writes its entity. */
	CROWDYREPLICATION_API void RestoreProxyMovement(AActor* Actor);

	/** False for a position or rotation no actor can be placed at: not finite, or past the addressable world. */
	CROWDYREPLICATION_API bool IsPlaceable(const FVector& Location, const FRotator& Rotation);

	/** One proxy slot's motion between frames. */
	struct FProxyMotion
	{
		TWeakObjectPtr<UCharacterMovementComponent> Movement;
		TWeakObjectPtr<APawn> Pawn;
		// A different actor in the slot, or this one destroyed, rebinds the movement component.
		TWeakObjectPtr<const AActor> BoundActor;
		FVector LastLocation = FVector::ZeroVector;
		int64 LastRenderMs = 0;
		double TeleportSpeed = 0.0;
	};

	/**
	 * Gives a proxy character's stopped movement component the velocity it is drawn moving at, with the matching
	 * acceleration, and a proxy pawn its aim pitch, all of which animation blueprints read. Call after placing the
	 * actor each frame.
	 */
	CROWDYREPLICATION_API void ApplyProxyMotion(FProxyMotion& Motion, AActor* Actor, const FVector& Location, float AimPitch, int64 RenderTimeMs);

	/** Stops a proxy's drawn motion; call when its slot is released. */
	CROWDYREPLICATION_API void ResetProxyMotion(FProxyMotion& Motion);
}

/**
 * Default actor-pool policy: hides pooled actors, shows them on activation, and strips
 * proxy movement. It is concrete and fully functional, so UCrowdyActorPoolBackendConfig
 * may leave PoolPolicyClass empty: the pool subsystem falls back to this class. Subclass
 * it only to add per-actor activate/deactivate behavior.
 */
UCLASS(Blueprintable, BlueprintType)
class CROWDYREPLICATION_API UCrowdyActorPoolPolicy : public UObject
{
	GENERATED_BODY()
public:
	
	/** Called once per actor right after it's spawned into the pool. */
	UFUNCTION(BlueprintNativeEvent, Category="Crowdy SDK|Actor Pool")
	void OnActorPooled(AActor* Actor);
	
	/** Called when an actor is checked out and assigned a UUID. */
	UFUNCTION(BlueprintNativeEvent, Category="Crowdy SDK|Actor Pool")
	void OnActorActivated(AActor* Actor, const FInstancedStruct& InitialState);
	
	/** Called when an actor is returned to the pool. */
	UFUNCTION(BlueprintNativeEvent, Category="Crowdy SDK|Actor Pool")
	void OnActorDeactivated(AActor* Actor);
};
