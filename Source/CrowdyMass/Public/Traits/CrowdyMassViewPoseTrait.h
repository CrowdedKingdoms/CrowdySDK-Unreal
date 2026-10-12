#pragma once

#include "CoreMinimal.h"
#include "MassEntityTraitBase.h"
#include "CrowdyMassViewPoseTrait.generated.h"

/**
 * Gives an entity the view pose fragment, and nothing else.
 *
 * The pose is where a replicated view field lands: both the state plane and the payload appended to a
 * position update write it, and a registered ingestor has nowhere to put its byte without it. The
 * animation trait adds the same fragment, but it adds it alongside an animation set reference and a
 * requirement for a representation fragment that only a visualization trait provides, so an entity that
 * wants replicated pose state without being animated by this plugin could not have one. This is that
 * fragment on its own.
 *
 * Adding both traits to one config is harmless: a fragment named twice is still one fragment on the
 * archetype.
 */
UCLASS(EditInlineNew, CollapseCategories, meta=(DisplayName="Crowdy View Pose"))
class CROWDYMASS_API UCrowdyMassViewPoseTrait : public UMassEntityTraitBase
{
	GENERATED_BODY()

public:
	virtual void BuildTemplate(FMassEntityTemplateBuildContext& BuildContext, const UWorld& World) const override;
};
