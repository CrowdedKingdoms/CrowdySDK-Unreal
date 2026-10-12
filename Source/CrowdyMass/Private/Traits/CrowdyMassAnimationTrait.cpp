#include "Traits/CrowdyMassAnimationTrait.h"

#include "Assets/CrowdyMassAnimSet.h"
#include "CrowdyMassLog.h"
#include "Fragments/CrowdyMassAnimFragments.h"
#include "MassEntityTemplateRegistry.h"
#include "MassEntityUtils.h"
#include "MassMovementFragments.h"
#include "MassRepresentationFragments.h"

void UCrowdyMassAnimationTrait::BuildTemplate(FMassEntityTemplateBuildContext& BuildContext, const UWorld& World) const
{
	BuildContext.AddFragment<FCrowdyMassAnimStateFragment>();
	BuildContext.AddFragment<FCrowdyMassActionFragment>();

	// Added here rather than by whatever sets poses, so that an entity can be put into a pose without
	// its archetype changing. Adding a fragment moves the entity to a different archetype, and those
	// moves are already the dominant per-entity cost in this system; a crowd where every death changed
	// archetype would pay for it in exactly the moment a lot of entities die at once.
	BuildContext.AddFragment<FCrowdyMassViewPoseFragment>();

	// The shared instanced-skinned-mesh component joins this entity's animation to its instance by
	// position in FMassRepresentationAnimationFragment, so that fragment has to exist on this
	// archetype or the join is silently wrong instead of missing. Require it rather than add it: a
	// visualization trait that installs the shared mesh component is expected to add it, and a
	// missing requirement fails template validation with the trait and type named, while adding a
	// fragment another trait already added only logs at Info by default and does not fail the build.
	//
	// The exception is a config whose visualization trait supplies no such fragment, which nothing but
	// this trait can then add; see bAddRepresentationAnimationFragment for when that is the case.
	if (bAddRepresentationAnimationFragment)
	{
		BuildContext.AddFragment<FMassRepresentationAnimationFragment>();
	}
	else
	{
		BuildContext.RequireFragment<FMassRepresentationAnimationFragment>();
	}

	// Gait is derived from how fast the entity is moving, so an archetype with no velocity has no
	// locomotion to animate. Required rather than added for the same reason as above: whichever trait
	// moves these entities owns the fragment, and a config that has no such trait should fail
	// template validation with both named instead of quietly producing entities that never move out
	// of their idle pose.
	BuildContext.RequireFragment<FMassVelocityFragment>();

	if (AnimSet.IsNull())
	{
		UE_LOG(LogCrowdyMass, Error, TEXT("UCrowdyMassAnimationTrait: no AnimSet assigned. Entities built from this trait will have no animation bank to select from."));
	}

	FCrowdyMassAnimSetSharedFragment AnimSetSharedFragment;
	AnimSetSharedFragment.AnimSet = AnimSet.LoadSynchronous();

	if (AnimSet.IsNull() == false && AnimSetSharedFragment.AnimSet == nullptr)
	{
		UE_LOG(LogCrowdyMass, Error, TEXT("UCrowdyMassAnimationTrait: AnimSet '%s' failed to load. Entities built from this trait will have no animation bank to select from."), *AnimSet.ToString());
	}

	FMassEntityManager& EntityManager = UE::Mass::Utils::GetEntityManagerChecked(World);
	const FConstSharedStruct SharedFragment = EntityManager.GetOrCreateConstSharedFragment(AnimSetSharedFragment);
	BuildContext.AddConstSharedFragment(SharedFragment);
}
