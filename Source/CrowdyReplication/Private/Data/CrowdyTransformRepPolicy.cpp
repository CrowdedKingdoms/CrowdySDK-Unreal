// Fill out your copyright notice in the Description page of Project Settings.

#include "Data/CrowdyTransformRepPolicy.h"

#include "CrowdyReplicationLog.h"
#include "GameFramework/Actor.h"
#include "Messages/Actor/FCrowdyKitActorState.h"
#include "Replication/Executor/ActorUpdateExecutor.h"

namespace
{
	// A player's actor state, or a Game Kit starter's actor, which a hub moves.
	bool CrowdyTransformRepPolicyRead(const FInstancedStruct& State, FVector& OutLocation, FRotator& OutRotation)
	{
		if (const FCrowdyActorState* Actor = State.GetPtr<FCrowdyActorState>())
		{
			OutLocation = Actor->Location;
			OutRotation = Actor->Rotation;
			return true;
		}
		if (const FCrowdyKitActorState* Kit = State.GetPtr<FCrowdyKitActorState>())
		{
			OutLocation = Kit->Location;
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
	if (!Actor || !CrowdyTransformRepPolicyRead(InitialState, Location, Rotation))
	{
		return;
	}

	Actor->SetActorLocationAndRotation(Location, Rotation);
}

bool UCrowdyTransformRepPolicy::ExtractFields(const FInstancedStruct& State, int64 ServerTimestampMs, int32 SlotId)
{
	FVector Location;
	FRotator Rotation;
	if (!CrowdyTransformRepPolicyRead(State, Location, Rotation) || SlotId < 0)
	{
		// Refusing makes the backend skip the update; a partial transform would be worse than none.
		UE_CLOG(CrowdyReplicationTrace::Pool(), LogCrowdyReplication, Warning,
			TEXT("[CrowdyTransformRepPolicy] Update for slot %d carried %s, not FCrowdyActorState or FCrowdyKitActorState, so it was skipped."),
			SlotId, *GetNameSafe(State.GetScriptStruct()));
		return false;
	}

	EnsureSlot(SlotId);
	Positions[SlotId].Push(Location, ServerTimestampMs);
	Rotations[SlotId].Push(Rotation, ServerTimestampMs);
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

	Actor->SetActorLocationAndRotation(Location, Rotation);
}

void UCrowdyTransformRepPolicy::OnInstanceDeactivated(int32 SlotId)
{
	if (!Positions.IsValidIndex(SlotId))
	{
		return;
	}

	Positions[SlotId] = {};
	Rotations[SlotId] = {};
}

void UCrowdyTransformRepPolicy::EnsureSlot(int32 SlotId)
{
	if (SlotId < Positions.Num())
	{
		return;
	}

	Positions.SetNum(SlotId + 1);
	Rotations.SetNum(SlotId + 1);
}
