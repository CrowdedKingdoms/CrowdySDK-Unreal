#pragma once

#include "Containers/UnrealString.h"
#include "Derivation/CrowdyMassMeshDerivation.h"
#include "UObject/NameTypes.h"

class UClass;
class UWorld;

/**
 * What an actor class would be spawned as, answered from the class alone with nothing spawned and no
 * crowd running.
 *
 * This is the same set of questions UCrowdyMassEntitySubsystem asks on a spawn, asked ahead of one, so
 * that "will this class appear" can be answered against a real authored class rather than inferred from
 * a crowd that did or did not turn up. It reads a class default object and every Blueprint in the parent
 * chain, so it belongs to a console command and never to an update path.
 *
 * bAuthoredConfigWins is the part that stops the rest of this being read as a promise. Everything else
 * describes what WOULD be derived; a class something already configures never reaches deriving at all,
 * and this report would otherwise describe an answer the game never uses.
 */
struct FCrowdyMassClassDerivationReport
{
	/** The class asked about, by path, so a report copied out of a log still names its subject. */
	FString ClassPath;

	bool bMeshResolved = false;

	/** The skinned asset a spawn would render this class with. Empty when none was found. */
	FString MeshAssetPath;

	ECrowdyMassMeshRoute MeshRoute = ECrowdyMassMeshRoute::None;

	/** The mesh component the asset was read off, and what holds that component. Both empty on a miss. */
	FString MeshComponentName;
	FString MeshRouteOwnerName;

	/** Where a derived config would place the mesh on the entity's transform. */
	FTransform MeshTransform = FTransform::Identity;

	/**
	 * False when nothing on the class describes a collision shape, in which case ObjectTypeName is the
	 * trait's own default rather than something read from the class. A derived entity is still hittable
	 * either way: the shape it is hit with comes from the physics asset on the mesh, not from here.
	 */
	bool bObjectTypeResolved = false;

	FName ObjectTypeName;

	/** Where the object type came from, in words, including the case where it came from nowhere. */
	FString ObjectTypeSourceDescription;

	/** True when something authored answers for this class, so nothing would be derived for it at all. */
	bool bAuthoredConfigWins = false;

	/** Which authored source that was, or why the question could not be answered here. */
	FString ConfigSourceDescription;

	/**
	 * Whether this class would animate, and where its animation set comes from.
	 *
	 * Reported separately from the mesh because the two fail independently and look the same from outside:
	 * a class with a mesh and no animation set spawns, renders and moves, and only stands in its bind
	 * pose. There is no other diagnostic that names that.
	 *
	 * Empty when there was no Crowdy Mass entity subsystem to ask, exactly as the config question is: the
	 * authored half of the answer lives there and cannot be worked out from the class alone.
	 *
	 * Asking this WORKS THE ANSWER OUT and caches it, which is right for a report whose whole job is to say
	 * what a spawn would do, and worth knowing before reading a later "nothing has asked about this class"
	 * as evidence: this report is a thing that asks.
	 */
	FString AnimSetPath;

	/** Which source supplied it, or which of the ways it could be absent this is, in words. */
	FString AnimSetSourceDescription;

	/**
	 * The Bank that animation set names, which a config derived from the class gives its mesh. An authored
	 * config's mesh carries its own bank and ignores this. Empty when no set was found.
	 */
	FString BankPath;

	/** Whether that Bank is unset, did not load, or animates a different mesh, in words. */
	FString BankDescription;

	/**
	 * Whether the mesh's materials would draw through the instanced skinned mesh path a crowd uses.
	 *
	 * Answered here because it is the one thing about a crowd that is invisible to every other question in
	 * this report: an entity whose material is missing the usage flag spawns, moves and is hittable, and
	 * only renders as the default material. "It appeared" says nothing about it, so the flag itself is
	 * read rather than inferred from anything downstream.
	 *
	 * Left empty when no mesh resolved, because there is then nothing to read materials off.
	 */
	FCrowdyMassInstancedSkinnedMaterials Materials;

	/** Empty when a mesh resolved. Otherwise what is missing and what to change, in that order. */
	FString Reason;

	/**
	 * Reads ActorClass. World is only used to ask which authored config would win, and may be null: the
	 * report then says the config question went unanswered rather than answering it wrongly.
	 */
	static CROWDYMASS_API FCrowdyMassClassDerivationReport ForClass(const UClass* ActorClass, const UWorld* World);

	/** Everything above on one line, for a console command that has to answer in one go. */
	CROWDYMASS_API FString ToLine() const;

	/**
	 * The class a person meant by what they typed: a short name (BP_MyCharacter), a package path
	 * (/Game/Characters/BP_MyCharacter) or a full object path all resolve, and so does a native class
	 * with or without its type prefix. Null with a filled-in OutWhy otherwise, including when the name is
	 * ambiguous, which is reported rather than resolved to an arbitrary one of the matches.
	 */
	static CROWDYMASS_API UClass* ResolveActorClass(const FString& ClassNameOrPath, FString& OutWhy);
};
