#include "Dev/CrowdyMassCrowdSimStateEntity.h"

#include "Components/SceneComponent.h"
#include "Replication/Components/CrowdyEntityComponent.h"

ACrowdyMassCrowdSimStateEntity::ACrowdyMassCrowdSimStateEntity()
{
	PrimaryActorTick.bCanEverTick = false;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	EntityComp = CreateDefaultSubobject<UCrowdyEntityComponent>(TEXT("CrowdyEntity"));
}
