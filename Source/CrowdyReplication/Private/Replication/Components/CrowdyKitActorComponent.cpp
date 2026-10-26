#include "Replication/Components/CrowdyKitActorComponent.h"

UCrowdyKitActorComponent::UCrowdyKitActorComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

bool UCrowdyKitActorComponent::ApplyKitActor(const FGuid& Key, const FCrowdyKitActorState& State, const bool bStartsDrawing)
{
	const bool bChanged = bStartsDrawing || Key != KitKey || !State.HasSameFacts(KitActor);
	KitKey = Key;
	KitActor = State;
	if (bChanged)
	{
		OnKitActorChanged.Broadcast(this, KitActor);
	}
	return bChanged;
}
