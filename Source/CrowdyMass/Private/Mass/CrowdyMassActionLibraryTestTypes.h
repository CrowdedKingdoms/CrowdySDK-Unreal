#pragma once

#include "CoreMinimal.h"
#include "Components/CrowdyMassAnimSetComponent.h"
#include "GameFramework/Actor.h"
#include "CrowdyMassActionLibraryTestTypes.generated.h"

/**
 * An actor class carrying a Crowdy Mass Anim Set component as a native default subobject, which is the route a
 * class default object answers for without any Blueprint compilation.
 *
 * Deliberately NOT wrapped in WITH_DEV_AUTOMATION_TESTS: UnrealHeaderTool refuses a reflected type inside a
 * preprocessor block, so this ships in game targets like its sibling fixture types do. Hidden so it is not
 * offered in any class picker.
 */
UCLASS(Hidden)
class ACrowdyMassActionLibraryTestActor : public AActor
{
	GENERATED_BODY()

public:
	ACrowdyMassActionLibraryTestActor()
	{
		AnimSetComponent = CreateDefaultSubobject<UCrowdyMassAnimSetComponent>(TEXT("AnimSetComponent"));
	}

	UPROPERTY()
	TObjectPtr<UCrowdyMassAnimSetComponent> AnimSetComponent;
};
