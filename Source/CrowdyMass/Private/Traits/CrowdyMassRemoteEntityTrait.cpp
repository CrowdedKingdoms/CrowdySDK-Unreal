#include "Traits/CrowdyMassRemoteEntityTrait.h"

#include "Fragments/CrowdyMassFragments.h"
#include "Mass/EntityFragments.h"
#include "MassActorSubsystem.h"
#include "MassCrowdFragments.h"
#include "MassEntityTemplateRegistry.h"
#include "MassLODFragments.h"
#include "MassMovementFragments.h"

void UCrowdyMassRemoteEntityTrait::BuildTemplate(FMassEntityTemplateBuildContext& BuildContext, const UWorld& World) const
{
	BuildContext.AddFragment<FTransformFragment>();
	BuildContext.AddFragment<FMassActorFragment>();
	BuildContext.AddFragment<FMassVelocityFragment>();
	BuildContext.AddFragment<FMassViewerInfoFragment>();
	BuildContext.AddFragment<FCrowdyRemoteIdentityFragment>();
	BuildContext.AddFragment<FCrowdyRemoteInterpolationFragment>();
	BuildContext.AddTag<FMassCodeDrivenMovementTag>();
	BuildContext.AddTag<FMassCrowdTag>();
	BuildContext.AddTag<FMassCollectDistanceLODViewerInfoTag>();
}
