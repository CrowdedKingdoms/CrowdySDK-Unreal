#include "Replication/State/CrowdyStateBlueprintLibrary.h"

#include "GameFramework/Actor.h"
#include "Replication/Components/CrowdyEntityComponent.h"

void UCrowdyStateBlueprintLibrary::MarkCrowdyStateDirty(AActor* Target, FName PropertyName)
{
	if (!Target)
	{
		return;
	}
	if (UCrowdyEntityComponent* Component = Target->FindComponentByClass<UCrowdyEntityComponent>())
	{
		Component->MarkStateDirty(PropertyName);
	}
}

void UCrowdyStateBlueprintLibrary::MarkAllCrowdyStatesDirty(AActor* Target)
{
	if (!Target)
	{
		return;
	}
	if (UCrowdyEntityComponent* Component = Target->FindComponentByClass<UCrowdyEntityComponent>())
	{
		Component->MarkAllStateDirty();
	}
}
