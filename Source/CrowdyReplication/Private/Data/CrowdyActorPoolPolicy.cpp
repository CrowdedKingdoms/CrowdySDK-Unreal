// Fill out your copyright notice in the Description page of Project Settings.


#include "Data/CrowdyActorPoolPolicy.h"

#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PawnMovementComponent.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "UObject/UnrealType.h"

namespace
{
	// A gap this long between drawn frames is a stall or a hitch, not motion.
	constexpr double MaxProxyMotionGapSeconds = 0.25;

	// A proxy's movement component never ticks, so nothing else writes its acceleration; the field is protected.
	FVector* FindProxyAcceleration(UCharacterMovementComponent* Movement)
	{
		static const FStructProperty* Property =
			FindFProperty<FStructProperty>(UCharacterMovementComponent::StaticClass(), TEXT("Acceleration"));
		return Property ? Property->ContainerPtrToValuePtr<FVector>(Movement) : nullptr;
	}

	// Only a stopped proxy's component is written: one that runs again belongs to a character simulated here.
	void SetProxyMotion(UCharacterMovementComponent* Movement, const FVector& Velocity)
	{
		if (Movement->MovementMode != MOVE_None) return;

		Movement->Velocity = Velocity;
		if (FVector* Acceleration = FindProxyAcceleration(Movement))
			*Acceleration = Velocity.GetSafeNormal();
	}
}

/**
 * Remote proxies are positioned entirely by interpolated transforms
 * (UCrowdyRepApplicationPolicy::ApplyToActor teleports them with
 * SetActorLocationAndRotation each frame), so a live pawn/character movement
 * component is pure overhead: it keeps running gravity and collision sweeps and
 * fights the teleported transform. Crucially, SetActorTickEnabled(false) does NOT
 * stop component ticks (a movement component owns its own tick function), so the
 * tick has to be disabled explicitly here.
 *
 * UPawnMovementComponent is the common base of UCharacterMovementComponent and the
 * generic pawn movement components, so iterating it covers both.
 */
void CrowdyActorPool::DisableProxyMovement(AActor* Actor)
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

void CrowdyActorPool::RestoreProxyMovement(AActor* Actor)
{
	if (!IsValid(Actor))
		return;

	TInlineComponentArray<UPawnMovementComponent*> MovementComponents;
	Actor->GetComponents(MovementComponents);

	for (UPawnMovementComponent* MoveComp : MovementComponents)
	{
		// One still active was never a proxy's, and its movement mode is the game's.
		if (!IsValid(MoveComp) || MoveComp->IsActive())
			continue;

		MoveComp->Activate(true);
		MoveComp->SetComponentTickEnabled(true);

		if (UCharacterMovementComponent* CharacterMovement = Cast<UCharacterMovementComponent>(MoveComp))
			CharacterMovement->SetDefaultMovementMode();
	}
}

bool CrowdyActorPool::IsPlaceable(const FVector& Location, const FRotator& Rotation)
{
	return !Location.ContainsNaN() && !Rotation.ContainsNaN() && Location.GetAbsMax() <= CrowdyMaxAddressableWorldCoordinate;
}

void CrowdyActorPool::ApplyProxyMotion(FProxyMotion& Motion, AActor* Actor, const FVector& Location, const float AimPitch, const int64 RenderTimeMs)
{
	if (Motion.BoundActor.Get() != Actor)
	{
		// The previous actor may already be another slot's, so it is let go without being written to.
		Motion = FProxyMotion();
		Motion.BoundActor = Actor;
		Motion.Pawn = Cast<APawn>(Actor);
		UCharacterMovementComponent* Movement = Actor ? Actor->FindComponentByClass<UCharacterMovementComponent>() : nullptr;
		Motion.Movement = Movement;
		Motion.TeleportSpeed = Movement ? FMath::Max(4.0 * Movement->MaxWalkSpeed, 3000.0) : 0.0;
	}

	if (APawn* Pawn = Motion.Pawn.Get())
		Pawn->SetRemoteViewPitch(AimPitch);

	UCharacterMovementComponent* Movement = Motion.Movement.Get();
	if (!Movement) return;

	if (Motion.LastRenderMs != 0 && RenderTimeMs == Motion.LastRenderMs) return;

	// A render time that went backwards (the wall clock was set back) starts over, like a first frame.
	const double Seconds = static_cast<double>(RenderTimeMs - Motion.LastRenderMs) / 1000.0;
	FVector Velocity = FVector::ZeroVector;
	if (Motion.LastRenderMs != 0 && Seconds > 0.0 && Seconds <= MaxProxyMotionGapSeconds)
	{
		Velocity = (Location - Motion.LastLocation) / Seconds;
		if (Velocity.SizeSquared() > FMath::Square(Motion.TeleportSpeed))
			Velocity = FVector::ZeroVector;
	}

	Motion.LastLocation = Location;
	Motion.LastRenderMs = RenderTimeMs;
	SetProxyMotion(Movement, Velocity);
}

void CrowdyActorPool::ResetProxyMotion(FProxyMotion& Motion)
{
	if (UCharacterMovementComponent* Movement = Motion.Movement.Get())
		SetProxyMotion(Movement, FVector::ZeroVector);
	Motion = FProxyMotion();
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
	CrowdyActorPool::DisableProxyMovement(Actor);
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
