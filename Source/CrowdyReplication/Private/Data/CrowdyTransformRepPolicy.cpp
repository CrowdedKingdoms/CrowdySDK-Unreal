// Fill out your copyright notice in the Description page of Project Settings.

#include "Data/CrowdyTransformRepPolicy.h"

#include "CrowdyReplicationLog.h"
#include "GameFramework/Actor.h"
#include "Replication/Executor/ActorUpdateExecutor.h"

void UCrowdyTransformRepPolicy::OnInstanceActivated_Implementation(AActor* Actor, const FInstancedStruct& InitialState)
{
	const FCrowdyActorState* State = InitialState.GetPtr<FCrowdyActorState>();
	if (!Actor || !State)
	{
		return;
	}

	Actor->SetActorLocationAndRotation(State->Location, State->Rotation);
}

bool UCrowdyTransformRepPolicy::ExtractFields(const FInstancedStruct& State, int64 ServerTimestampMs, int32 SlotId)
{
	const FCrowdyActorState* Incoming = State.GetPtr<FCrowdyActorState>();
	if (!Incoming || SlotId < 0)
	{
		// Refusing makes the backend skip the update; a partial transform would be worse than none.
		UE_CLOG(CrowdyReplicationTrace::Pool(), LogCrowdyReplication, Warning,
			TEXT("[CrowdyTransformRepPolicy] Update for slot %d carried %s, not FCrowdyActorState, so it was skipped."),
			SlotId, *GetNameSafe(State.GetScriptStruct()));
		return false;
	}

	EnsureSlot(SlotId);
	Positions[SlotId].Push(Incoming->Location, ServerTimestampMs);
	Rotations[SlotId].Push(Incoming->Rotation, ServerTimestampMs);
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
