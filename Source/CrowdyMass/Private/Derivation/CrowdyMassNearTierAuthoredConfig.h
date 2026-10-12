#pragma once

#include "Containers/UnrealString.h"
#include "MassProcessingTypes.h"

class UClass;
class UMassEntityConfigAsset;
class UMassVisualizationTrait;
class UObject;
class UWorld;

/**
 * Wires a class's near tier shell declaration into a Mass entity config somebody AUTHORED.
 *
 * The declaration used to reach only the config this plugin builds from a class, which is the path taken
 * exactly when nothing authored answers for the class. So a class with an authored config declared a shell
 * that nothing read, silently, and the opt-in documented everywhere was half true: whether it did anything
 * depended on a fact about the project that the declaration does not mention.
 *
 * Nothing here ever writes to the authored asset. That asset can be shared by several classes and is a
 * saved file besides, so patching it would give every class the last one's shell and dirty content on
 * disk. What is built instead is a per-class config that names the authored one as its PARENT and carries
 * one trait: a duplicate of the authored visualization trait with the near tier fields filled in. Mass
 * combines traits down the parent chain and lets a child's trait of the same class override the parent's,
 * so the result is the authored config in every respect except the three fields promotion needs.
 *
 * The parent-and-override shape is not a stylistic choice. Duplicating the config asset itself carries the
 * authored asset's ConfigGuid with it, the template registry is keyed on that guid, and the only thing that
 * regenerates it on duplication is editor-only: in a cooked build a duplicated asset would silently hand
 * back the authored template and the patch would do nothing at all.
 */
struct FCrowdyMassNearTierAuthoredConfig
{
	/** The execution flags Mass will build a template with in World, worked out the way Mass works them out. */
	static EProcessorExecutionFlags GetWorldProcessingFlags(const UWorld& World);

	/**
	 * The ONE visualization trait a near tier may be patched into, or null with a reason in OutRefusal.
	 *
	 * Not simply the first trait of that type in the chain, and the difference is the whole of this
	 * function. A patched trait wins only because it is the last thing to write the representation
	 * parameters: `FMassEntityTemplateData::AddConstSharedFragment` keeps the FIRST value offered for a
	 * type and silently discards every later one (the mismatch is an `ensure`, so a Shipping build says
	 * nothing at all). So any OTHER visualization trait that also builds writes those parameters first and
	 * the patch is thrown away, leaving a config that names a high resolution actor its level table never
	 * selects. Nothing promotes and nothing is logged.
	 *
	 * Traits are also filtered by `ValidTargetConfig & WorldProcessingFlags` before they build, so a trait
	 * that is not valid for this world's net mode never runs and a duplicate of it never runs either. That
	 * is refused rather than patched, because the alternative is reporting a promotion that cannot happen.
	 *
	 * A parent and a child holding traits of the SAME class are the ordinary way to author this and are not
	 * ambiguous: Mass keeps one of them, and the patch overrides whichever it kept.
	 */
	static const UMassVisualizationTrait* FindPatchableVisualizationTrait(const UMassEntityConfigAsset& Config,
		EProcessorExecutionFlags WorldProcessingFlags, FString& OutRefusal);

	/**
	 * Whether an authored trait already promotes its entities to an actor on its own.
	 *
	 * BOTH halves are the question. Naming a high resolution template actor while the nearest level is still
	 * drawn as an instance promotes nothing, and every visualization trait in the engine sets that level to
	 * a spawned actor in its own constructor, so either half read alone answers the wrong question: one
	 * says a half-wired config is finished, the other says an untouched trait is a deliberate decision.
	 *
	 * Only a config that answers true here overrides the class's declaration. A config that answers false
	 * has a hole rather than an intent, and the declaration fills it.
	 */
	static bool AlreadyPromotes(const UMassVisualizationTrait& Trait);

	/**
	 * Whether a trait's own promotion is one this plugin's near tier can drive, and what is wrong when not.
	 *
	 * Asked of a config that promotes on ITS OWN, where the class's declaration is shadowed and this code
	 * changes nothing. Both checks the patch path treats as mandatory are checked here too, because a
	 * promotion that spawns an actor nothing drives, or leaves a management class that cannot hide one, is
	 * indistinguishable from a working promotion from the outside. Reporting is all that is done with the
	 * answer: an authored decision stays an authored decision.
	 */
	static bool PromotesUsably(const UMassVisualizationTrait& Trait, FString& OutProblem);

	/**
	 * The first level below the nearest that asks for a spawned actor it has no template for, or INDEX_NONE.
	 *
	 * Both engine visualization trait constructors set the level below the near tier to
	 * `LowResSpawnedActor` and neither ever assigns a low resolution template actor, so a config left at
	 * those defaults logs two errors PER ENTITY PER FRAME once entities reach that band and draws them
	 * through a fallback. It is not this code's to rewrite, since it is somebody's authored level table,
	 * but it is squarely the near tier's business to say so: nothing else looks at that band.
	 */
	static int32 FindUnusableSpawnedActorBand(const UMassVisualizationTrait& Trait);

	/**
	 * A per-class config that is the authored one plus promotion to ShellClass, or null when SourceTrait
	 * could not be duplicated.
	 *
	 * Only the three fields promotion is made of are written: the template actor, the nearest level's
	 * representation, and the management class that makes the switch visually correct. The authored level
	 * distances and actor counts are deliberately left alone, because they are somebody's authored LOD
	 * table and a near tier is a change of what the nearest level DRAWS, not of where it begins.
	 *
	 * OutReplacedManagementClass names the management class that was displaced, when one was, so the caller
	 * can say what it took away. A management class that already derives from this plugin's is kept.
	 */
	static UMassEntityConfigAsset* BuildPatchedConfig(UObject& Outer, const UMassEntityConfigAsset& Authored,
		const UMassVisualizationTrait& SourceTrait, UClass* ShellClass, const UClass*& OutReplacedManagementClass);
};
