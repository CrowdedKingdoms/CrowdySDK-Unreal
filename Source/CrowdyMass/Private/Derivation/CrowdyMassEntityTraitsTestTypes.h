#pragma once

#include "CoreMinimal.h"
#include "MassEntityTraitBase.h"
#include "MassVisualizationTrait.h"
#include "CrowdyMassEntityTraitsTestTypes.generated.h"

/**
 * A game's own Mass trait, of a kind nothing in this plugin adds by itself.
 *
 * Marker is what separates "a trait of the right class arrived" from "the trait that arrived carries what
 * was authored on it". A copy that lost the authored values would pass the first and fail the second, and
 * only the pair of them says the feature works.
 *
 * Deliberately NOT wrapped in WITH_DEV_AUTOMATION_TESTS: UnrealHeaderTool refuses a reflected type inside a
 * preprocessor block, so this ships in game targets like its sibling fixture types do. Hidden so it is not
 * offered as a choice in the trait picker on every Mass entity config in the project.
 */
UCLASS(Hidden)
class UCrowdyMassEntityTraitsTestTrait : public UMassEntityTraitBase
{
	GENERATED_BODY()

public:
	virtual void BuildTemplate(FMassEntityTemplateBuildContext& BuildContext, const UWorld& World) const override {}

	UPROPERTY(EditAnywhere, Category="Test")
	int32 Marker = 0;
};

/** A second trait class, so "the right one of two" can be told from "whichever was first". */
UCLASS(Hidden)
class UCrowdyMassEntityTraitsTestSecondTrait : public UMassEntityTraitBase
{
	GENERATED_BODY()

public:
	virtual void BuildTemplate(FMassEntityTemplateBuildContext& BuildContext, const UWorld& World) const override {}
};

/**
 * A visualization trait of a class no derived config adds, which is the shape that must be refused.
 *
 * It has to be a different class from the one a derived config already carries: a trait of the SAME class
 * replaces that one and is allowed, so asserting the refusal with this plugin's own visualization trait
 * would test the opposite rule.
 */
UCLASS(Hidden)
class UCrowdyMassEntityTraitsTestVisualizationTrait : public UMassVisualizationTrait
{
	GENERATED_BODY()
};
