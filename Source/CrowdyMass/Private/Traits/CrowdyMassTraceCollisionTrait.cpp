#include "Traits/CrowdyMassTraceCollisionTrait.h"

#include "MassEntityTemplateRegistry.h"

void UCrowdyMassTraceCollisionTrait::BuildTemplate(FMassEntityTemplateBuildContext& BuildContext, const UWorld& World) const
{
	FCrowdyMassTraceCollisionFragment& TraceCollisionFragment = BuildContext.AddFragment_GetRef<FCrowdyMassTraceCollisionFragment>();
	TraceCollisionFragment.bEnableTraceCollision = TraceCollision.bEnableTraceCollision;
	TraceCollisionFragment.ObjectType = TraceCollision.ObjectType;
	TraceCollisionFragment.ResponseToChannels = FCollisionResponseContainer(TraceCollision.DefaultResponse.GetValue());

	for (const FCrowdyMassCollisionResponseOverride& ResponseOverride : TraceCollision.ResponseOverrides)
	{
		TraceCollisionFragment.ResponseToChannels.SetResponse(ResponseOverride.Channel.GetValue(), ResponseOverride.Response.GetValue());
	}
}
