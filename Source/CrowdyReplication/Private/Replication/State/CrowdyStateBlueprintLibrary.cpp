#include "Replication/State/CrowdyStateBlueprintLibrary.h"

#include "CrowdyReplicationLog.h"
#include "GameFramework/Actor.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "UObject/ObjectKey.h"

namespace
{
	void WarnNoEntityComponentOnce(const AActor* Target, const TCHAR* NodeName)
	{
		// Keyed by world too, so each new PIE session warns again.
		static TSet<TTuple<FObjectKey, FObjectKey, UPTRINT>> Warned;
		bool bAlreadyWarned = false;
		Warned.Add(MakeTuple(FObjectKey(Target->GetWorld()), FObjectKey(Target->GetClass()), reinterpret_cast<UPTRINT>(NodeName)), &bAlreadyWarned);
		if (bAlreadyWarned)
		{
			return;
		}

		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("%s was called on '%s' (class '%s'), which has no Crowdy Entity Component, so nothing is marked dirty. Add a Crowdy Entity Component."),
			NodeName, *Target->GetName(), *Target->GetClass()->GetName());
	}
}

void UCrowdyStateBlueprintLibrary::MarkCrowdyStateDirty(AActor* Target, FName PropertyName)
{
	if (!Target)
	{
		return;
	}
	UCrowdyEntityComponent* Component = Target->FindComponentByClass<UCrowdyEntityComponent>();
	if (!Component)
	{
		WarnNoEntityComponentOnce(Target, TEXT("Mark Crowdy State Dirty"));
		return;
	}
	Component->MarkStateDirty(PropertyName);
}

void UCrowdyStateBlueprintLibrary::MarkAllCrowdyStatesDirty(AActor* Target)
{
	if (!Target)
	{
		return;
	}
	UCrowdyEntityComponent* Component = Target->FindComponentByClass<UCrowdyEntityComponent>();
	if (!Component)
	{
		WarnNoEntityComponentOnce(Target, TEXT("Mark All Crowdy States Dirty"));
		return;
	}
	Component->MarkAllStateDirty();
}
