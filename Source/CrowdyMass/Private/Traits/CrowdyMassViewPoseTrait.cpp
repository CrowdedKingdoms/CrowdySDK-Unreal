#include "Traits/CrowdyMassViewPoseTrait.h"

#include "Fragments/CrowdyMassAnimFragments.h"
#include "MassEntityTemplateRegistry.h"

void UCrowdyMassViewPoseTrait::BuildTemplate(FMassEntityTemplateBuildContext& BuildContext, const UWorld& World) const
{
	BuildContext.AddFragment<FCrowdyMassViewPoseFragment>();
}
