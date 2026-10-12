#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Math/Transform.h"
#include "UObject/NameTypes.h"
#include "UObject/ObjectKey.h"

class UClass;
class UMaterial;
class UMaterialInterface;
class USkinnedAsset;

/**
 * Which of the three ways a mesh reaches an actor supplied a derived mesh.
 *
 * The three do not see each other, so which one answered is the first thing an author needs when the
 * answer is not the one they expected: a mesh found on a parent's construction script and a mesh found
 * on the class in front of them look identical from the outside.
 */
enum class ECrowdyMassMeshRoute : uint8
{
	/** Nothing supplied a mesh, so no route answered. */
	None,

	/** A component declared in C++, which lives on the class default object. */
	NativeComponent,

	/** A component added in the Blueprint editor, which exists only as a construction script template. */
	BlueprintComponent,

	/** An inherited component whose defaults a child Blueprint changed, which is kept in neither of those. */
	InheritedOverride
};

/** A route in words, for a log line or a message shown to whoever authored the class. */
CROWDYMASS_API const TCHAR* LexToString(ECrowdyMassMeshRoute Route);

/**
 * What an actor class would render with, worked out from the class alone.
 *
 * Asset is null when no mesh could be found, and Reason then says in plain words what was looked at and
 * what is missing. Reason is meant to be shown to whoever authored the class, so it names the class and
 * the thing to change.
 *
 * Route, ComponentName and RouteOwnerName describe where the mesh was found rather than what was found,
 * and are only filled in when one was. They exist for diagnostics: nothing about the entity a config is
 * built from depends on which route answered.
 */
struct FCrowdyMassDerivedMesh
{
	USkinnedAsset* Asset = nullptr;
	FString Reason;

	/** Where the class places the mesh under the actor's root, composed up its attach chain: for a character, the drop to its feet and its turn. */
	FTransform Transform = FTransform::Identity;

	ECrowdyMassMeshRoute Route = ECrowdyMassMeshRoute::None;

	/** The mesh component the asset was read off, for a class carrying more than one. */
	FString ComponentName;

	/** What holds that component: the class default object for a native one, otherwise the Blueprint
	 * class whose construction script or override handler carries the template. A parent Blueprint's
	 * name here is the answer to "why is it not using the mesh I set". */
	FString RouteOwnerName;

	bool IsValid() const
	{
		return Asset != nullptr;
	}
};

/**
 * One material slot on a skinned asset that a crowd will not draw with the material assigned to it.
 *
 * A material only draws through the instanced skinned mesh path if it carries the matching usage flag.
 * Without it the engine silently substitutes the default material, so the entity spawns, moves and is
 * hittable, and only looks wrong. Nothing about the entity says so.
 */
struct FCrowdyMassUnflaggedMaterialSlot
{
	int32 SlotIndex = INDEX_NONE;

	/** The slot's own name, which is what an author sees beside it in the mesh editor. */
	FName SlotName;

	/** The material assigned to the slot, by path. */
	FString MaterialPath;

	/**
	 * Where the usage flag has to be ticked for this slot to draw: the material at the root of the
	 * assigned material's parent chain, since an instance normally inherits the flag rather than
	 * carrying one. Equal to MaterialPath when the slot holds a material rather than an instance, or
	 * when the instance has no parent to read a flag from.
	 */
	FString FlagOwnerPath;

	/** The same object, for a caller reporting each material once rather than once per mesh using it. */
	FObjectKey FlagOwner;
};

/** Whether a skinned asset's material slots would draw through the instanced skinned mesh path. */
struct FCrowdyMassInstancedSkinnedMaterials
{
	/** Every material slot the asset declares, whether or not the flag applies to what is in it. */
	int32 SlotCount = 0;

	/**
	 * How many of those carried a material whose usage flag the engine actually gates drawing on. An
	 * empty slot and a material whose domain makes usage flags meaningless are both counted out here, so
	 * this never claims to have checked something it skipped.
	 */
	int32 SlotsChecked = 0;

	/** The checked slots that are missing the flag, in slot order. */
	TArray<FCrowdyMassUnflaggedMaterialSlot> Unflagged;

	bool IsClear() const
	{
		return Unflagged.Num() == 0;
	}

	/** All of it in one sentence, for a report or a log line that has to answer in one go. */
	CROWDYMASS_API FString Describe() const;
};

/**
 * Answers "what skinned asset would an actor of this class render with" without spawning anything and
 * without touching a world. The answer depends only on the class, so a caller is free to work it out
 * once per class and keep it.
 */
struct FCrowdyMassMeshDerivation
{
	/** Resolves the class. Returns the first mesh found, or a null asset and a reason explaining why. */
	static CROWDYMASS_API FCrowdyMassDerivedMesh FromClass(const UClass* ActorClass);

	/**
	 * Reads the instanced skinned usage flag off every material slot of a skinned asset. Reads the flag
	 * itself rather than inferring anything from whether a crowd turned up: an entity drawn with the
	 * default material is indistinguishable from a correct one in every other diagnostic there is.
	 *
	 * A null asset gives an empty verdict with nothing checked and nothing to report.
	 */
	static CROWDYMASS_API FCrowdyMassInstancedSkinnedMaterials CheckInstancedSkinnedMaterials(const USkinnedAsset* Asset);
};

/**
 * Which materials this process has already been told about.
 *
 * The subject is the material the flag is ticked on, not the mesh and not the class, because that is the
 * unit of the fix: twenty character classes parented onto one base material are one thing to change, and
 * twenty lines saying so would bury it. What that suppresses is never lost, because the verdict for any
 * class can be asked for again at any time and is never filtered by this.
 *
 * Held for the process rather than per world on purpose. The flag lives in a saved asset, so the answer
 * cannot change between two worlds of one session, and a fix that does take effect makes the check pass
 * and leaves nothing to say.
 */
struct FCrowdyMassMaterialUsageReports
{
	/**
	 * One message for each material in the verdict that has not been reported before, in slot order, and
	 * nothing at all for the ones that have. Marks each material it returns a message for, so the same
	 * verdict passed twice speaks once. Asset and Context are named in the message as where the material
	 * was first run into, and both may be null.
	 */
	static CROWDYMASS_API void TakeUnreported(const FCrowdyMassInstancedSkinnedMaterials& Verdict,
		const USkinnedAsset* Asset, const UClass* Context, TArray<FString>& OutMessages);

#if WITH_DEV_AUTOMATION_TESTS
	/** Forgets every material reported so far, so one case cannot silence the next. */
	static CROWDYMASS_API void ResetForTest();
#endif
};
