#pragma once

#include "Containers/UnrealString.h"
#include "CrowdyServerObjectComponent.h"
#include "Derivation/CrowdyMassEntityTraitsDerivation.h"

class UClass;
class UCrowdyServerObjectDefinition;

/** The Crowdy Server Object component of an actor class whose definition holds a record per entity; read and throw away. */
struct FCrowdyMassDerivedServerRecords
{
	UCrowdyServerObjectDefinition* Definition = nullptr;

	ECrowdyServerObjectInstanceMode InstanceMode = ECrowdyServerObjectInstanceMode::InstanceId;

	/** The Instance Id every row of the class reads; empty for This Actor's Area. */
	FString InstanceId;

	/** Whether rows follow the areas around the view rather than one instance. */
	bool bArea = false;

	/** False for an instance mode that picks one instance per player or actor, which rows cannot follow. */
	bool bSupported = false;

	ECrowdyMassEntityTraitsRoute Route = ECrowdyMassEntityTraitsRoute::None;

	FString ComponentName;

	/** Empty when the class has a supported component, or when it has none at all. Otherwise what to change. */
	FString Reason;
};

/** Walks a class default object and its Blueprint parents, so ask once per class and keep the answer. */
struct FCrowdyMassServerRecordsDerivation
{
	static FCrowdyMassDerivedServerRecords FromClass(const UClass* ActorClass);
};
