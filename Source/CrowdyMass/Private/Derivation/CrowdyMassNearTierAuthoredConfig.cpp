#include "Derivation/CrowdyMassNearTierAuthoredConfig.h"

#include "Components/CrowdyMassNearTierComponent.h"
#include "Derivation/CrowdyMassNearTierDerivation.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "MassEntityConfigAsset.h"
#include "MassEntityTraitBase.h"
#include "MassEntityUtils.h"
#include "MassRepresentationFragments.h"
#include "MassRepresentationTypes.h"
#include "MassVisualizationTrait.h"
#include "Representation/CrowdyMassRepresentationActorManagement.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

namespace CrowdyMassNearTierAuthored
{
	// Walks a config and every config above it, in no particular order, with the same cycle guard Mass uses
	// on the same chain. Deliberately does NOT reproduce which trait overrides which: the caller only ever
	// asks whether a trait OTHER than the one it chose would also build, and an override cannot make an
	// extra one appear.
	template <typename FuncType>
	void ForEachTraitInChain(const UMassEntityConfigAsset& Config, FuncType&& Visitor)
	{
		TArray<const UMassEntityConfigAsset*, TInlineAllocator<8>> Visited;

		for (const UMassEntityConfigAsset* Current = &Config; Current != nullptr; )
		{
			for (const UMassEntityTraitBase* Trait : Current->GetConfig().GetTraits())
			{
				if (Trait)
				{
					Visitor(Trait);
				}
			}

			const UMassEntityConfigAsset* Parent = Current->GetConfig().GetParent();
			if (!Parent || Visited.Contains(Parent))
			{
				break;
			}

			Visited.Add(Parent);
			Current = Parent;
		}
	}
}

EProcessorExecutionFlags FCrowdyMassNearTierAuthoredConfig::GetWorldProcessingFlags(const UWorld& World)
{
	// The same expression FMassEntityTemplateBuildContext::BuildFromTraits uses to decide which traits run.
	// Worked out the same way rather than approximated from the net mode, because a patch that builds under
	// a different rule than the template does is a patch that reports success and changes nothing.
	return World.IsGameWorld()
		? UE::Mass::Utils::DetermineProcessorExecutionFlags(&World)
		: EProcessorExecutionFlags::All;
}

const UMassVisualizationTrait* FCrowdyMassNearTierAuthoredConfig::FindPatchableVisualizationTrait(
	const UMassEntityConfigAsset& Config, const EProcessorExecutionFlags WorldProcessingFlags, FString& OutRefusal)
{
	OutRefusal.Reset();

	// Walks the parent chain, which is where a project that shares one base config between several classes
	// keeps its visualization. Looking only at the config's own traits would report a config with a parent
	// as carrying no visualization at all, and the advice that produces sends an author to add a trait they
	// already have.
	const UMassVisualizationTrait* Chosen =
		Cast<UMassVisualizationTrait>(Config.FindTrait(UMassVisualizationTrait::StaticClass()));

	if (!Chosen)
	{
		OutRefusal = TEXT("carries no visualization trait, so there is nothing for a near tier to change");
		return nullptr;
	}

	// A trait Mass will not run is a trait a duplicate of will not run either. Refused rather than patched,
	// because the patch would be built, cached and reported as a promotion that then never happens.
	if (!static_cast<bool>(Chosen->ValidTargetConfig & WorldProcessingFlags))
	{
		OutRefusal = FString::Printf(
			TEXT("carries visualization trait '%s', which is not valid for this world's net mode, so Mass does not build from it here"),
			*Chosen->GetClass()->GetName());
		return nullptr;
	}

	// The patched trait wins only by being the LAST thing to write the representation parameters, and only
	// one trait can write them at all: the template data keeps the first value offered for a const shared
	// fragment type and discards the rest, with nothing but an ensure to say so. So a second visualization
	// trait that also builds silently beats the patch, and the result is a config naming a high resolution
	// actor whose level table never selects one.
	const UMassVisualizationTrait* Rival = nullptr;

	CrowdyMassNearTierAuthored::ForEachTraitInChain(Config,
		[&Rival, Chosen, WorldProcessingFlags](const UMassEntityTraitBase* Trait)
		{
			const UMassVisualizationTrait* AsVisualization = Cast<UMassVisualizationTrait>(Trait);
			if (!AsVisualization || AsVisualization == Chosen || Rival)
			{
				return;
			}

			if (!static_cast<bool>(AsVisualization->ValidTargetConfig & WorldProcessingFlags))
			{
				return;
			}

			// A parent and a child holding traits of the same class for the same net modes is the ordinary
			// way to author this: Mass keeps one of them and the patch overrides whichever it kept, so this
			// is not a second writer.
			if (AsVisualization->GetClass() == Chosen->GetClass()
				&& static_cast<bool>(AsVisualization->ValidTargetConfig & Chosen->ValidTargetConfig))
			{
				return;
			}

			Rival = AsVisualization;
		});

	if (Rival)
	{
		OutRefusal = FString::Printf(
			TEXT("carries visualization traits '%s' and '%s', which both build here, so which one decides how an entity is drawn is not something this can answer; the near tier would be written to one and thrown away by the other"),
			*Chosen->GetClass()->GetName(), *Rival->GetClass()->GetName());
		return nullptr;
	}

	return Chosen;
}

bool FCrowdyMassNearTierAuthoredConfig::AlreadyPromotes(const UMassVisualizationTrait& Trait)
{
	return Trait.HighResTemplateActor != nullptr
		&& Trait.Params.LODRepresentation[EMassLOD::High] == EMassRepresentationType::HighResSpawnedActor;
}

bool FCrowdyMassNearTierAuthoredConfig::PromotesUsably(const UMassVisualizationTrait& Trait, FString& OutProblem)
{
	OutProblem.Reset();

	UClass* PromotedClass = Trait.HighResTemplateActor;
	if (!PromotedClass)
	{
		OutProblem = TEXT("names no high resolution actor at all");
		return false;
	}

	// The same capability test a declared shell has to pass. Everything a promoted entity does is done by
	// its near tier component, so an actor without one is placed once and then stands still while its
	// entity walks away, drawn by neither the crowd nor itself.
	if (!FCrowdyMassNearTierDerivation::CanHostNearTierView(PromotedClass))
	{
		OutProblem = FString::Printf(
			TEXT("promotes to '%s', which carries no Crowdy Mass Near Tier component, so nothing would drive it from the entity"),
			*PromotedClass->GetPathName());
		return false;
	}

	const UClass* ManagementClass = Trait.Params.RepresentationActorManagementClass;
	if (!ManagementClass || !ManagementClass->IsChildOf(UCrowdyMassRepresentationActorManagement::StaticClass()))
	{
		OutProblem = FString::Printf(
			TEXT("is managed by '%s', which does not hide a demoted actor, so an entity in that band is drawn twice, once as the actor and once as the crowd row"),
			*GetNameSafe(ManagementClass));
		return false;
	}

	return true;
}

int32 FCrowdyMassNearTierAuthoredConfig::FindUnusableSpawnedActorBand(const UMassVisualizationTrait& Trait)
{
	if (Trait.LowResTemplateActor != nullptr)
	{
		return INDEX_NONE;
	}

	// Only the levels BELOW the nearest, which the near tier does not touch and which nothing else checks.
	// The nearest level is the one being patched and has a template actor by construction.
	for (int32 LODIndex = EMassLOD::High + 1; LODIndex < EMassLOD::Max; ++LODIndex)
	{
		if (Trait.Params.LODRepresentation[LODIndex] == EMassRepresentationType::LowResSpawnedActor)
		{
			return LODIndex;
		}
	}

	return INDEX_NONE;
}

UMassEntityConfigAsset* FCrowdyMassNearTierAuthoredConfig::BuildPatchedConfig(UObject& Outer,
	const UMassEntityConfigAsset& Authored, const UMassVisualizationTrait& SourceTrait, UClass* ShellClass,
	const UClass*& OutReplacedManagementClass)
{
	OutReplacedManagementClass = nullptr;

	UMassEntityConfigAsset* PatchedConfig = NewObject<UMassEntityConfigAsset>(&Outer);
	PatchedConfig->GetMutableConfig().SetParentAsset(Authored);

	// Duplicated rather than built from scratch, so everything the author set that promotion has no opinion
	// about (the meshes, the level distances, whatever a subclassed trait adds of its own) arrives intact.
	// The duplicate keeps the source's concrete class, which is what makes Mass treat it as an override of
	// the trait it came from rather than as a second visualization trait beside it.
	UMassVisualizationTrait* PatchedTrait = DuplicateObject<UMassVisualizationTrait>(&SourceTrait, PatchedConfig);
	if (!PatchedTrait)
	{
		return nullptr;
	}

	PatchedTrait->HighResTemplateActor = ShellClass;
	PatchedTrait->Params.LODRepresentation[EMassLOD::High] = EMassRepresentationType::HighResSpawnedActor;

	// A promoted entity is hidden, shown, made ready and placed by its management class, and the stock one
	// does none of those things the way a crowd row handover needs. A game that subclassed ours keeps its
	// own; anything else is replaced, because leaving a management class that cannot hide an actor produces
	// a promotion that renders the entity twice and reads as working.
	//
	// Reported through OutReplacedManagementClass rather than done quietly. What is displaced can be doing
	// real work of its own: MetaHuman's crowd management is a sibling of this plugin's rather than a child,
	// and it carries a pooled-actor teleport fixup that this one has no equivalent of.
	const UClass* AuthoredManagement = PatchedTrait->Params.RepresentationActorManagementClass;
	if (!AuthoredManagement || !AuthoredManagement->IsChildOf(UCrowdyMassRepresentationActorManagement::StaticClass()))
	{
		OutReplacedManagementClass = AuthoredManagement;
		PatchedTrait->Params.RepresentationActorManagementClass = UCrowdyMassRepresentationActorManagement::StaticClass();
	}

	PatchedConfig->GetMutableConfig().AddTrait(*PatchedTrait);

	return PatchedConfig;
}
