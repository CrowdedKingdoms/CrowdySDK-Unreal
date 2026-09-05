// Fill out your copyright notice in the Description page of Project Settings.


#include "Replication/Executor/ActorUpdateExecutor.h"

#include "GameFramework/Actor.h"

namespace CrowdyActorUpdateExecutor
{
	bool IsNativeStateFunction(const UFunction* StateFunction)
	{
		// A Blueprint override of a BlueprintNativeEvent is a script function on the generated class; the
		// unoverridden case resolves to the native UFunction the C++ declaration produced.
		return StateFunction != nullptr && StateFunction->IsNative();
	}

	bool AnswersStateNatively(const UActorUpdateExecutor* Executor)
	{
		if (!Executor)
		{
			return false;
		}

		// Named through the macro so a rename of the event is a build break rather than a silent fallback
		// to the thunk.
		return IsNativeStateFunction(Executor->GetClass()->FindFunctionByName(
			GET_FUNCTION_NAME_CHECKED(UActorUpdateExecutor, GetActorState)));
	}
}

FInstancedStruct UActorUpdateExecutor::GetActorState_Implementation(const UActorComponent* UpdateComponent) const
{
	return FInstancedStruct();
}

FInstancedStruct UCrowdyDefaultActorUpdateExecutor::GetActorState_Implementation(const UActorComponent* UpdateComponent) const
{
	const AActor* Owner = UpdateComponent ? UpdateComponent->GetOwner() : nullptr;
	if (!IsValid(Owner))
	{
		// An empty snapshot rather than a zeroed one. The replicator compares this against what it last
		// sent, and a default-constructed FCrowdyActorState would read as an actor that had genuinely
		// moved to the origin.
		return FInstancedStruct();
	}

	FCrowdyActorState State;
	State.Location = Owner->GetActorLocation();
	State.Rotation = Owner->GetActorRotation();

	return FInstancedStruct::Make(State);
}
