#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "UObject/StrongObjectPtr.h"

class UClass;
class UWorld;

// A development instrument, not a shipped surface. Nothing in a packaged game builds a proxy to look at
// it, and the console command that drives this is guarded the same way.
#if !UE_BUILD_SHIPPING

/**
 * Written into the first integer parameter of whatever function the probe dispatches, so a body that reads
 * its own parameter can be told apart from one handed a zeroed frame. The design's claim that there is no
 * second signature to bridge is only worth anything if the frame actually arrives.
 */
inline constexpr int32 CrowdyMassProxyProbeParameterSentinel = 0x3C3C3C;

/**
 * What one component looked like on an actor that was constructed but never spawned.
 */
struct FCrowdyMassProxyComponentReading
{
	FName Name;
	FName ClassName;

	// Registration is what gives a component a scene proxy, a physics state and, for a skeletal mesh, an
	// animation instance. Nothing registers the components of an actor that was never spawned.
	bool bRegistered = false;

	// Only ever true for a skeletal mesh component, and only when that component is registered: the
	// animation instance is created by the component's registration, so a body reaching for one before
	// then finds nothing and every montage call it makes returns without doing anything.
	bool bHasAnimInstance = false;
};

/**
 * One reading of what a per-class dispatch proxy can and cannot do, taken against a real class in a real
 * world. The design for author-once RPC delivery rests on running the author's own event body on a stand-in
 * of their class that was never spawned, and this is what measures whether that stand-in is real enough.
 */
struct FCrowdyMassDispatchProxyProbeReading
{
	// Non-empty means the probe refused before measuring anything, and every field below is meaningless.
	FString Refusal;

	FName ClassName;

	// Handed back, and held strongly, so a caller can go on to read whatever the body it ran wrote. Nothing
	// else references a proxy, so without this it would be collected out from under that reading.
	TStrongObjectPtr<AActor> Proxy;

	bool bProxyConstructed = false;

	// An actor resolves its world by walking its outer chain to a level, so a proxy outered anywhere else
	// answers null and every world-context node in the author's body fails.
	bool bWorldResolved = false;

	// AActor::ProcessEvent runs a body only when the actor's world reports its actors initialized, or when
	// editor script execution is force-enabled. The condition is a property of the WORLD, not of the actor,
	// which is the reason an unspawned actor can run a body at all.
	bool bWorldActorsInitialized = false;
	bool bScriptExecutionAllowedHere = false;

	bool bRootComponentPresent = false;

	// The whole claim of the design is that this thing is not in the world. Spawning is what puts an actor
	// in its level's list, so a proxy that appears there was spawned after all and everything that follows
	// from never being spawned is void.
	bool bProxyPresentInLevelActorList = false;

	TArray<FCrowdyMassProxyComponentReading> ProxyComponents;

	// Components a genuinely spawned actor of the same class carries and the proxy does not. Everything a
	// Blueprint adds through its construction script lands here, because that script runs during spawning
	// and nowhere else.
	TArray<FName> ComponentsOnlyOnASpawnedActor;
	bool bComparisonSpawned = false;
	int32 SpawnedComponentCount = 0;

	// The entity's position has to be projected onto the borrowed proxy before the body runs, or every
	// location the body reads is the origin.
	bool bTransformProjected = false;
	FVector ProjectedLocation = FVector::ZeroVector;
	FVector ReadBackLocation = FVector::ZeroVector;

	bool bFunctionResolved = false;
	bool bFunctionDispatched = false;

	/**
	 * The design resets the proxy's property block from the class default object between entities so
	 * nothing carries over. A wholesale copy also overwrites the properties that point at the proxy's OWN
	 * components with the class default object's components, which would leave every body operating on the
	 * default object's mesh. These two record whether that happens and whether skipping subobject
	 * references avoids it.
	 */
	bool bNaiveCdoResetStealsComponents = false;
	bool bGuardedCdoResetKeepsComponents = false;
	bool bGuardedCdoResetClearsPlainProperties = false;

	// One block of text, so a run in a play session can be copied out in a single selection.
	FString ToReport() const;
};

/**
 * Builds a dispatch proxy for ActorClass exactly as the author-once design describes, runs FunctionName on
 * it if the class declares one, and reports what was real.
 *
 * bSpawnComparisonActor spawns and immediately destroys one genuine actor of the same class, which is the
 * only way to see which components the class only ever gets during spawning. It runs that actor's
 * construction script and its BeginPlay, so pass false against a class whose spawn has side effects.
 */
FCrowdyMassDispatchProxyProbeReading CrowdyMassProbeDispatchProxy(UWorld* World, UClass* ActorClass,
	FName FunctionName, bool bSpawnComparisonActor);

#endif
