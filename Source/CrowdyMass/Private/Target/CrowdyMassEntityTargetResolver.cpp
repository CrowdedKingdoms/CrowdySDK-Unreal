#include "Target/CrowdyMassEntityTargetResolver.h"

#include "Components/ActorComponent.h"
#include "Engine/World.h"
#include "Fragments/CrowdyMassFragments.h"
#include "Mass/CrowdyMassEntity.h"
#include "Mass/EntityFragments.h"
#include "MassEntityManager.h"
#include "Replication/RPC/FCrowdyRpcTarget.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"
#include "Target/CrowdyMassEntityTarget.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace CrowdyMassTarget
{

EResolveResult ResolveRpcTarget(const FCrowdyMassEntityTarget& Target, FCrowdyRpcTarget& OutRpcTarget)
{
	OutRpcTarget = FCrowdyRpcTarget();

	UWorld* World = Target.World.Get();
	if (!World)
	{
		return EResolveResult::NoWorld;
	}

	const UCrowdyMassEntitySubsystem* MassSubsystem = World->GetSubsystem<UCrowdyMassEntitySubsystem>();
	FMassEntityManager* EntityManager = MassSubsystem ? MassSubsystem->GetMassEntityManager() : nullptr;
	if (!EntityManager)
	{
		return EResolveResult::NoWorld;
	}

	return ResolveRpcTargetIn(*EntityManager, Target, OutRpcTarget);
}

EResolveResult ResolveRpcTargetIn(FMassEntityManager& InEntityManager, const FCrowdyMassEntityTarget& Target,
	FCrowdyRpcTarget& OutRpcTarget)
{
	OutRpcTarget = FCrowdyRpcTarget();

	FMassEntityManager* EntityManager = &InEntityManager;

	// Built, not merely valid. A reserved entity that has not been built yet passes IsEntityValid (its slot
	// is in range and its serial matches) but has no archetype, and reading a fragment off one is a null
	// dereference rather than a miss. Not-built takes the same "not addressable right now" answer a stale
	// handle does, because that is what it is.
	if (!EntityManager->IsEntityValid(Target.Entity) || !EntityManager->IsEntityBuilt(Target.Entity))
	{
		return EResolveResult::InvalidEntity;
	}

	const FCrowdyRemoteIdentityFragment* Identity = EntityManager->GetFragmentDataPtr<FCrowdyRemoteIdentityFragment>(Target.Entity);
	if (!Identity)
	{
		return EResolveResult::MissingIdentity;
	}

	if (!Identity->UUID.IsValid())
	{
		return EResolveResult::Unroutable;
	}

	OutRpcTarget.NetID = Identity->UUID;
	OutRpcTarget.OwnerID = Identity->OwnerID;
	OutRpcTarget.Role = Identity->Role;

	if (const FTransformFragment* Transform = EntityManager->GetFragmentDataPtr<FTransformFragment>(Target.Entity))
	{
		OutRpcTarget.Location = Transform->GetTransform().GetLocation();
	}

	return EResolveResult::Resolved;
}

EClassCheckResult CheckEventClass(const UFunction* Fn, const UClass* RecordedClass)
{
	if (!RecordedClass)
	{
		return EClassCheckResult::ClassUnknown;
	}

	const UClass* DeclaringClass = Fn ? Fn->GetOwnerClass() : nullptr;

	// UCrowdyEventRouter::ResolveStateContainer runs the body on the participant or on the first component
	// of the declaring class it carries. A component is not part of the entity's class and can be added at
	// run time, so the class cannot confirm one; admit the two kinds that resolution can reach rather than
	// refuse events that route correctly.
	if (DeclaringClass && !RecordedClass->IsChildOf(DeclaringClass)
		&& (DeclaringClass->IsChildOf(UActorComponent::StaticClass())
			|| DeclaringClass->IsChildOf(UCrowdyMassEntity::StaticClass())))
	{
		return EClassCheckResult::Ok;
	}

	if (!DeclaringClass || !RecordedClass->IsChildOf(DeclaringClass))
	{
		return EClassCheckResult::ClassMismatch;
	}

	return EClassCheckResult::Ok;
}

}
