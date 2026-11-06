// Fill out your copyright notice in the Description page of Project Settings.

#include "Data/CrowdyTransformRepPolicy.h"

#include "CrowdyReplicationLog.h"
#include "Data/CrowdyActorPoolPolicy.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Messages/Actor/FCrowdyKitActorState.h"
#include "Replication/Executor/ActorUpdateExecutor.h"
#include "Replication/Subsystems/CrowdyKitActorSubsystem.h"

namespace
{
	// A player's actor state, or a Game Kit starter's actor, which a hub moves.
	bool CrowdyTransformRepPolicyRead(const FInstancedStruct& State, const UCrowdyKitActorSubsystem* KitActors, FVector& OutLocation, FRotator& OutRotation, float& OutAimPitch)
	{
		OutAimPitch = 0.f;
		if (const FCrowdyActorState* Actor = State.GetPtr<FCrowdyActorState>())
		{
			OutLocation = Actor->Location;
			OutRotation = Actor->Rotation;
			OutAimPitch = FMath::IsFinite(Actor->AimPitch) ? Actor->AimPitch : 0.f;
			return true;
		}
		if (const FCrowdyKitActorState* Kit = State.GetPtr<FCrowdyKitActorState>())
		{
			OutLocation = KitActors ? KitActors->PlaceInWorld(*Kit) : Kit->Location;
			OutRotation = Kit->Rotation;
			return true;
		}
		return false;
	}
}

void UCrowdyTransformRepPolicy::OnInstanceActivated_Implementation(AActor* Actor, const FInstancedStruct& InitialState)
{
	FVector Location;
	FRotator Rotation;
	float AimPitch;
	if (!Actor)
	{
		return;
	}
	if (InitialState.GetScriptStruct() == FCrowdyKitActorState::StaticStruct())
	{
		const UWorld* World = Actor->GetWorld();
		KitActors = World ? World->GetSubsystem<UCrowdyKitActorSubsystem>() : nullptr;
	}
	if (!CrowdyTransformRepPolicyRead(InitialState, KitActors.Get(), Location, Rotation, AimPitch))
	{
		return;
	}

	Actor->SetActorLocationAndRotation(Location, Rotation);
}

bool UCrowdyTransformRepPolicy::ExtractFields(const FInstancedStruct& State, int64 ServerTimestampMs, int32 SlotId)
{
	FVector Location;
	FRotator Rotation;
	float AimPitch;
	if (!CrowdyTransformRepPolicyRead(State, KitActors.Get(), Location, Rotation, AimPitch) || SlotId < 0)
	{
		// Refusing makes the backend skip the update; a partial transform would be worse than none.
		UE_CLOG(CrowdyReplicationTrace::Pool(), LogCrowdyReplication, Warning,
			TEXT("[CrowdyTransformRepPolicy] Update for slot %d carried %s, not FCrowdyActorState or FCrowdyKitActorState, so it was skipped."),
			SlotId, *GetNameSafe(State.GetScriptStruct()));
		return false;
	}

	if (!CrowdyActorPool::IsPlaceable(Location, Rotation))
	{
		UE_CLOG(CrowdyReplicationTrace::Pool(), LogCrowdyReplication, Warning,
			TEXT("[CrowdyTransformRepPolicy] Update for slot %d placed the actor at %s %s, which no actor can stand at, so it was skipped."),
			SlotId, *Location.ToCompactString(), *Rotation.ToCompactString());
		return false;
	}

	EnsureSlot(SlotId);
	Positions[SlotId].Push(Location, ServerTimestampMs);
	Rotations[SlotId].Push(Rotation, ServerTimestampMs);
	AimPitches[SlotId].Push(AimPitch, ServerTimestampMs);
	return true;
}

void UCrowdyTransformRepPolicy::ApplyToActor(AActor* Actor, int32 SlotId, int64 RenderTimeMs)
{
	if (!Actor || !Positions.IsValidIndex(SlotId))
	{
		return;
	}

	const FVector Location = Positions[SlotId].Sample(RenderTimeMs,
		[](const FVector& A, const FVector& B, float T) { return FMath::Lerp(A, B, T); });

	// Slerp through quaternions so extrapolation (T past 1) turns on the short arc instead of wrapping Euler angles.
	const FRotator Rotation = Rotations[SlotId].Sample(RenderTimeMs,
		[](const FRotator& A, const FRotator& B, float T)
		{
			return FQuat::Slerp(A.Quaternion(), B.Quaternion(), FMath::Clamp(T, 0.f, 1.2f)).GetNormalized().Rotator();
		});

	const float AimPitch = AimPitches[SlotId].Sample(RenderTimeMs,
		[](const float A, const float B, float T) { return FMath::Lerp(A, B, FMath::Clamp(T, 0.f, 1.f)); });

	Actor->SetActorLocationAndRotation(Location, Rotation);
	CrowdyActorPool::ApplyProxyMotion(Motions[SlotId], Actor, Location, AimPitch, RenderTimeMs);
}

void UCrowdyTransformRepPolicy::OnInstanceDeactivated(int32 SlotId)
{
	if (!Positions.IsValidIndex(SlotId))
	{
		return;
	}

	Positions[SlotId] = {};
	Rotations[SlotId] = {};
	AimPitches[SlotId] = {};
	CrowdyActorPool::ResetProxyMotion(Motions[SlotId]);
}

void UCrowdyTransformRepPolicy::EnsureSlot(int32 SlotId)
{
	if (SlotId < Positions.Num())
	{
		return;
	}

	Positions.SetNum(SlotId + 1);
	Rotations.SetNum(SlotId + 1);
	AimPitches.SetNum(SlotId + 1);
	Motions.SetNum(SlotId + 1);
}
