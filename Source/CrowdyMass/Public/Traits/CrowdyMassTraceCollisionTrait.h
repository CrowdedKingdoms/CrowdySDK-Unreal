#pragma once

#include "CoreMinimal.h"
#include "Fragments/CrowdyMassFragments.h"
#include "MassEntityTraitBase.h"
#include "CrowdyMassTraceCollisionTrait.generated.h"

UCLASS(EditInlineNew, CollapseCategories, meta=(DisplayName="Crowdy Mass Trace Collision"))
class CROWDYMASS_API UCrowdyMassTraceCollisionTrait : public UMassEntityTraitBase
{
	GENERATED_BODY()

public:
	virtual void BuildTemplate(FMassEntityTemplateBuildContext& BuildContext, const UWorld& World) const override;

	/**
	 * For a config assembled in code rather than authored in the editor.
	 *
	 * Only the object type is settable, and the omission is the point: every other field here is a policy
	 * this plugin picks on an author's behalf, so a config built in code behaves exactly like one someone
	 * added this trait to and left alone. The object type is different in kind. It says what sort of thing
	 * the entity IS rather than what it should collide with, so a trace looking for pawns finds a crowd of
	 * players instead of silently missing them.
	 */
	void SetObjectType(const ECollisionChannel InObjectType) { TraceCollision.ObjectType = InObjectType; }

protected:
	UPROPERTY(EditAnywhere, Category="Crowdy Mass|Trace Collision")
	FCrowdyMassTraceCollisionSettings TraceCollision;
};
