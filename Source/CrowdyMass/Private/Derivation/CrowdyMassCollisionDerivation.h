#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "Fragments/CrowdyMassFragments.h"

class UCapsuleComponent;
class UPhysicsAsset;
class UPrimitiveComponent;
class USkinnedMeshComponent;

/** Which shape on an actor class a trace collision answer was read from. */
enum class ECrowdyMassCollisionDerivationSource : uint8
{
	/** Nothing on the class describes a collision shape, so the answer carries a reason instead of settings. */
	None,

	/** A capsule component, whose radius and half height are the entity's size directly. */
	CapsuleComponent,

	/** The physics asset on a skinned mesh, whose bodies were measured in the mesh's reference pose. */
	PhysicsAsset
};

/**
 * What trace collision an actor class would present, worked out from the class alone.
 *
 * Settings is the same shape the Crowdy Mass Trace Collision trait authors, so an entity built from a
 * class and one built from an authored config describe collision in identical terms.
 *
 * Radius and HalfHeight are the entity's size in centimetres and are UNSCALED: a class knows the size
 * its component was authored at, not the scale an individual actor happens to be spawned with.
 *
 * When nothing on the class describes a shape, Source is None and Reason says what is missing and what
 * to add. Reason is empty for every other Source.
 */
struct FCrowdyMassDerivedTraceCollision
{
	FCrowdyMassTraceCollisionSettings Settings;

	float Radius = 0.0f;

	float HalfHeight = 0.0f;

	ECrowdyMassCollisionDerivationSource Source = ECrowdyMassCollisionDerivationSource::None;

	FString Reason;

	bool IsValid() const
	{
		return Source != ECrowdyMassCollisionDerivationSource::None;
	}
};

/**
 * Reads the trace collision an actor class presents straight off the class, without spawning an actor
 * and without a world. Everything it looks at (a component on the class default object, a component a
 * Blueprint added, the physics asset behind a skinned mesh) is fixed for the whole class, so the answer
 * is the same for every entity of that class.
 *
 * That also makes it the wrong thing to call often: it walks the class hierarchy's components and reads
 * assets. Ask once per class, keep the answer, and never call it while updating entities.
 */
struct FCrowdyMassCollisionDerivation
{
	/**
	 * Trace collision settings and size for ActorClass, or a filled-in Reason when the class describes
	 * no shape. A null class, or a class that is not an actor, is a reason rather than a crash.
	 */
	static FCrowdyMassDerivedTraceCollision FromActorClass(const UClass* ActorClass);

	/**
	 * The collision channel and per-channel responses Component is set up with, in the authored form:
	 * the response most of its channels share becomes DefaultResponse, and every channel that differs
	 * becomes one override. Expanding the result reproduces the component's own responses exactly.
	 */
	static FCrowdyMassTraceCollisionSettings SettingsFromComponent(const UPrimitiveComponent& Component);

	/** Settings expanded back into the per-channel responses a collision query actually uses. */
	static FCollisionResponseContainer ExpandResponses(const FCrowdyMassTraceCollisionSettings& Settings);

	/**
	 * The capsule that decides what traces hit this class, preferring one that answers queries over one
	 * whose collision is switched off entirely. Null when the class has no capsule at all.
	 */
	static const UCapsuleComponent* FindCapsuleComponent(const UClass* ActorClass);

	/**
	 * The first skinned mesh component on the class that has a physics asset, with the asset itself in
	 * OutPhysicsAsset. Null when no skinned mesh on the class carries one.
	 */
	static const USkinnedMeshComponent* FindPhysicsAssetComponent(const UClass* ActorClass, const UPhysicsAsset*& OutPhysicsAsset);

	/**
	 * Measures a physics asset's bodies in the skinned mesh's reference pose and reports the capsule
	 * that contains them. False when no body names a bone the mesh actually has, which leaves nothing
	 * to measure.
	 */
	static bool ComputePhysicsAssetExtents(const USkinnedMeshComponent& Component, const UPhysicsAsset& PhysicsAsset, float& OutRadius, float& OutHalfHeight);

	/**
	 * How many collision channels a response container holds. The container is one response byte per
	 * channel and nothing else, so its size is that count. ECC_MAX is larger and cannot be used here:
	 * it counts a trailing deprecated entry that has no slot in the container.
	 */
	static constexpr int32 ChannelCount = static_cast<int32>(sizeof(FCollisionResponseContainer));
};
