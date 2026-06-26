// Fill out your copyright notice in the Description page of Project Settings.


#include "Data/CrowdyActorPoolPolicy.h"

#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PawnMovementComponent.h"
#include "Replication/Components/CrowdyEntityComponent.h"

namespace
{
	/**
	 * Remote proxies are positioned entirely by interpolated transforms
	 * (UCrowdyRepApplicationPolicy::ApplyToActor teleports them with
	 * SetActorLocationAndRotation each frame), so a live pawn/character movement
	 * component is pure overhead — it keeps running gravity and collision sweeps and
	 * fights the teleported transform. Crucially, SetActorTickEnabled(false) does NOT
	 * stop component ticks (a movement component owns its own tick function), so the
	 * tick has to be disabled explicitly here.
	 *
	 * UPawnMovementComponent is the common base of UCharacterMovementComponent and the
	 * generic pawn movement components, so iterating it covers both.
	 */
	void DisableProxyMovement(AActor* Actor)
	{
		if (!IsValid(Actor))
			return;

		TInlineComponentArray<UPawnMovementComponent*> MovementComponents;
		Actor->GetComponents(MovementComponents);

		for (UPawnMovementComponent* MoveComp : MovementComponents)
		{
			if (!IsValid(MoveComp))
				continue;

			MoveComp->StopMovementImmediately();

			// MOVE_None makes the simulation a no-op even if something later re-enables
			// the tick (e.g. a Blueprint SetMovementMode on the proxy).
			if (UCharacterMovementComponent* CharacterMovement = Cast<UCharacterMovementComponent>(MoveComp))
				CharacterMovement->DisableMovement();

			MoveComp->SetComponentTickEnabled(false);
			MoveComp->Deactivate();
		}
	}
}

void UCrowdyActorPoolPolicy::OnActorPooled_Implementation(AActor* Actor)
{
	if (!IsValid(Actor))
		return;

	Actor->SetActorHiddenInGame(true);
	Actor->SetActorTickEnabled(false);
	Actor->SetActorEnableCollision(false);

	// Pool actors are passive proxies — they must not self-replicate their own
	// pre-pool identity. If left running, each pre-warmed actor's auto-replicator
	// would broadcast a random UUID to the server, causing a cascade where every
	// echo creates another pool actor proxy. A pre-warmed actor also mints a
	// throwaway Owner record in BeginPlay (ResolveIdentity), so clear identity too:
	// the actor stays inert until the backend assigns a real proxy id at checkout,
	// and never pollutes owner queries (IsLocallyOwned/GetEntitiesByOwner) meanwhile.
	if (UCrowdyEntityComponent* Component = Actor->FindComponentByClass<UCrowdyEntityComponent>())
	{
		Component->StopReplication();
		Component->ClearIdentity();
	}

	// Proxies are driven by replicated transforms, not local movement. Strip movement
	// simulation once at pool creation; it stays disabled across acquire/release because
	// the actor-tick toggling in the activate/deactivate hooks never touches component ticks.
	DisableProxyMovement(Actor);
}

void UCrowdyActorPoolPolicy::OnActorActivated_Implementation(AActor* Actor, const FInstancedStruct& InitialState)
{
	if (IsValid(Actor))
	{
		Actor->SetActorHiddenInGame(false);
		Actor->SetActorTickEnabled(true);
		Actor->SetActorEnableCollision(true);
	}
}

void UCrowdyActorPoolPolicy::OnActorDeactivated_Implementation(AActor* Actor)
{
	if (!IsValid(Actor))
		return;

	Actor->SetActorHiddenInGame(true);
	Actor->SetActorTickEnabled(false);
	Actor->SetActorEnableCollision(false);

	if (UCrowdyEntityComponent* Component = Actor->FindComponentByClass<UCrowdyEntityComponent>())
	{
		Component->StopReplication();
		Component->ClearIdentity();
	}
}
