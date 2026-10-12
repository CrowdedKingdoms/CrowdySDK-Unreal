#pragma once

#include "CoreMinimal.h"
#include "MassEntityTraitBase.h"
#include "CrowdyMassAnimationTrait.generated.h"

class UCrowdyMassAnimSet;

/** Animates Crowdy crowd rows from an anim set. Its processors run only in worlds whose map profile selects the Mass backend. */
UCLASS(EditInlineNew, CollapseCategories, meta=(DisplayName="Crowdy Mass Animation"))
class CROWDYMASS_API UCrowdyMassAnimationTrait : public UMassEntityTraitBase
{
	GENERATED_BODY()

public:
	virtual void BuildTemplate(FMassEntityTemplateBuildContext& BuildContext, const UWorld& World) const override;

	// The anim set every entity built from this trait shares, mapping this archetype's gaits,
	// actions, and poses to the shared instanced-skinned-mesh animation bank. Mandatory on
	// any archetype that reaches that shared component; add this trait alongside the visualization
	// trait that installs it, not as an opt-in per entity.
	//
	// Resolved once when the entity template is built and cached by object identity for the rest of
	// that session. Re-saving or reimporting this asset during a running session does not refresh
	// entities built before the edit; rebuild the level or restart PIE to see the change.
	UPROPERTY(EditAnywhere, Category="Crowdy Mass|Anim")
	TSoftObjectPtr<UCrowdyMassAnimSet> AnimSet;

	/**
	 * Adds the fragment the renderer joins an entity's animation to its instance through, instead of
	 * requiring another trait on the same config to have added it.
	 *
	 * Leave this off on an authored config. Every visualization trait that draws a crowd as instanced
	 * skinned meshes adds that fragment itself, and REQUIRING it is what makes a config with no such trait
	 * fail template validation with both named, rather than quietly producing entities that never animate.
	 *
	 * Turn it on only for a config whose visualization trait does not add it, which is the case for a
	 * config composed in code around a class that carries nothing but a skeletal mesh.
	 */
	UPROPERTY(EditAnywhere, Category="Crowdy Mass|Anim", AdvancedDisplay)
	bool bAddRepresentationAnimationFragment = false;
};
