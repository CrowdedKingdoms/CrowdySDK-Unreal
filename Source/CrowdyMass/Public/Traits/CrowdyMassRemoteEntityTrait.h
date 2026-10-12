#pragma once

#include "CoreMinimal.h"
#include "MassEntityTraitBase.h"
#include "CrowdyMassRemoteEntityTrait.generated.h"

UCLASS(EditInlineNew, CollapseCategories, meta=(DisplayName="Crowdy Remote Entity"))
class CROWDYMASS_API UCrowdyMassRemoteEntityTrait : public UMassEntityTraitBase
{
	GENERATED_BODY()

public:
	virtual void BuildTemplate(FMassEntityTemplateBuildContext& BuildContext, const UWorld& World) const override;
};
