#include "Derivation/CrowdyMassServerRecordsDerivation.h"

#include "CrowdyServerObjectDefinition.h"
#include "GameFramework/Actor.h"
#include "Templates/SubclassOf.h"
#include "UObject/Class.h"

FCrowdyMassDerivedServerRecords FCrowdyMassServerRecordsDerivation::FromClass(const UClass* ActorClass)
{
	FCrowdyMassDerivedServerRecords Result;
	if (!ActorClass || !ActorClass->IsChildOf(AActor::StaticClass()))
	{
		return Result;
	}

	// Never builds a default object: a class still loading or compiling must not run its constructor here.
	const UObject* DefaultObject = ActorClass->GetDefaultObject(/*bCreateIfNeeded=*/false);
	if (!DefaultObject)
	{
		return Result;
	}

	const UCrowdyServerObjectComponent* Found = nullptr;
	const TSubclassOf<AActor> AsActorClass{const_cast<UClass*>(ActorClass)};
	AActor::ForEachComponentOfActorClassDefault(AsActorClass, UCrowdyServerObjectComponent::StaticClass(),
		[&Found](const UActorComponent* Component)
		{
			const UCrowdyServerObjectComponent* Candidate = Cast<UCrowdyServerObjectComponent>(Component);
			const bool bRecords = Candidate && Candidate->Definition && Candidate->Definition->Holds == ECrowdyServerObjectHolds::RecordPerEntity;
			Found = bRecords ? Candidate : nullptr;
			return !bRecords;
		});
	if (!Found)
	{
		return Result;
	}

	UCrowdyServerObjectDefinition* Definition = Found->Definition;
	Result.Definition = Definition;
	Result.InstanceMode = Found->InstanceMode;
	Result.Route = RouteOfClassDefaultComponent(*Found, DefaultObject);
	Result.ComponentName = Found->GetName();

	if (Definition->bOnlyOneInstance)
	{
		Result.InstanceId = UCrowdyServerObjectDefinition::OnlyInstanceId;
		Result.bSupported = true;
		return Result;
	}
	if (Found->InstanceMode == ECrowdyServerObjectInstanceMode::ThisActorsArea)
	{
		Result.bArea = true;
		Result.bSupported = true;
		return Result;
	}
	if (Found->InstanceMode == ECrowdyServerObjectInstanceMode::InstanceId && !Found->InstanceId.IsEmpty())
	{
		Result.InstanceId = Found->InstanceId;
		Result.bSupported = true;
		return Result;
	}

	const UEnum* Modes = StaticEnum<ECrowdyServerObjectInstanceMode>();
	Result.Reason = Found->InstanceMode == ECrowdyServerObjectInstanceMode::InstanceId
		? FString::Printf(TEXT("'%s' reads its records with no Instance Id, so its crowd rows get no server values. Set one on %s."),
			*ActorClass->GetPathName(), *Result.ComponentName)
		: FString::Printf(TEXT("'%s' reads its records with Instance Mode %s, which crowd rows cannot follow, so they get no server values. Use Only One Instance, Instance Id or This Actor's Area on %s."),
			*ActorClass->GetPathName(), *Modes->GetNameStringByValue(static_cast<int64>(Found->InstanceMode)), *Result.ComponentName);
	return Result;
}
