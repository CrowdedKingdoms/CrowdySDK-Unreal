// Also gated on !UE_BUILD_SHIPPING, because these cases measure the dispatch proxy probe, whose whole
// header and implementation live under that condition. The two conditions coincide unless a build forces
// automation tests into a Shipping target, which is the configuration this guard exists for.
#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING

#include "Misc/AutomationTest.h"

#include "Components/SkeletalMeshComponent.h"
#include "Dev/CrowdyMassDispatchProxyProbe.h"
#include "Dispatch/CrowdyMassDispatchProxy.h"
#include "Dev/CrowdyMassDispatchProxyTestTypes.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "UObject/Script.h"

/**
 * The prototype behind author-once RPC delivery to a crowd entity.
 *
 * The design runs the author's OWN event body on a per-class stand-in that was never spawned, so a
 * developer writes one body instead of two. Everything else in that tier rests on one question these cases
 * answer with a real actor class in a real world: how much of an actor is real before it is spawned.
 *
 * Read the readings, not just the verdicts. Two of these cases pass while recording a limit that decides
 * what a body may and may not contain.
 */
namespace
{
	constexpr EAutomationTestFlags CrowdyMassDispatchProxyTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	/**
	 * An Editor world, because a Game world spins up this project's play-gated world subsystems and at
	 * least one of them asserts on teardown against an entity manager a bare test world never initialized.
	 *
	 * The script execution guard stands in for the one thing an Editor world cannot supply: AActor's
	 * ProcessEvent runs a body only where the world reports its actors initialized, which a played level
	 * does and a bare test world never does. That condition is a property of the WORLD rather than of the
	 * actor, which is exactly why an unspawned actor can run a body at all in a real session.
	 */
	struct FCrowdyMassDispatchProxyFixture
	{
		UWorld* World = nullptr;
		FEditorScriptExecutionGuard ScriptGuard;

		FCrowdyMassDispatchProxyFixture()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Editor);
			Context.SetCurrentWorld(World);
		}

		~FCrowdyMassDispatchProxyFixture()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(/*bInformEngineOfWorld=*/false);
		}
	};
}

/**
 * A stand-in that was never spawned is real enough to run the author's own body, and the body sees the
 * entity's position rather than the origin.
 *
 * This is the load-bearing half of the design. If it were false there would be no author-once tier at all,
 * only the fragment write the receive node already does.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassDispatchProxyRunsTheAuthorsBodyTest,
	"CrowdySDK.Mass.DispatchProxyRunsTheAuthorsBody", CrowdyMassDispatchProxyTestFlags)
bool FCrowdyMassDispatchProxyRunsTheAuthorsBodyTest::RunTest(const FString& Parameters)
{
	FCrowdyMassDispatchProxyFixture Fixture;

	const FCrowdyMassDispatchProxyProbeReading Reading = CrowdyMassProbeDispatchProxy(Fixture.World,
		ACrowdyMassDispatchProxyTestActor::StaticClass(), TEXT("ProxyBody"), /*bSpawnComparisonActor=*/true);

	if (!TestTrue(FString::Printf(TEXT("the probe measured something: %s"), *Reading.Refusal),
		Reading.Refusal.IsEmpty()))
	{
		return false;
	}

	TestTrue(TEXT("a proxy was constructed"), Reading.bProxyConstructed);
	TestTrue(TEXT("the proxy resolves a world, so world-context nodes in a body have one"), Reading.bWorldResolved);
	TestFalse(TEXT("the proxy is NOT in the level's actor list, so it really was never spawned"),
		Reading.bProxyPresentInLevelActorList);
	TestTrue(TEXT("the proxy has a root component"), Reading.bRootComponentPresent);
	TestTrue(TEXT("the entity's position projects onto the proxy"), Reading.bTransformProjected);
	TestEqual(TEXT("and reads back as the position asked for"), Reading.ReadBackLocation, Reading.ProjectedLocation);
	TestTrue(TEXT("the body's function resolved"), Reading.bFunctionResolved);

	const ACrowdyMassDispatchProxyTestActor* Proxy =
		Cast<ACrowdyMassDispatchProxyTestActor>(Reading.Proxy.Get());
	if (!TestNotNull(TEXT("the probe handed the proxy back"), Proxy))
	{
		return false;
	}

	// The whole point of the case. ProcessEvent silently does nothing on an actor whose world will not have
	// it, so a test that only checked the call was made would pass against a body that never ran.
	TestEqual(TEXT("the author's body ran exactly once"), Proxy->BodyRunCount, 1);
	TestTrue(TEXT("and it resolved a world from inside itself"), Proxy->bBodySawWorld);
	TestEqual(TEXT("and it read the projected position, not the origin"),
		Proxy->BodySawLocation, Reading.ProjectedLocation);
	TestEqual(TEXT("and the frame's parameter reached it"),
		Proxy->LastValue, CrowdyMassProxyProbeParameterSentinel);

	// Recorded rather than asserted: a body running here at all rests on the world's script gate, which an
	// Editor world only opens because the fixture forces it. The reading says which of the two it was.
	AddInfo(Reading.ToReport());

	return true;
}

/**
 * The limit that decides what such a body may contain: the components exist, and none of them is
 * registered.
 *
 * An unregistered skeletal mesh has no animation instance, so every montage call a body makes through it
 * returns having done nothing, silently. That is not a defect in the proxy; it is what "never spawned"
 * means, and it is why the animation half of a presentation body cannot travel this road.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassDispatchProxyComponentsAreUnregisteredTest,
	"CrowdySDK.Mass.DispatchProxyComponentsAreUnregistered", CrowdyMassDispatchProxyTestFlags)
bool FCrowdyMassDispatchProxyComponentsAreUnregisteredTest::RunTest(const FString& Parameters)
{
	FCrowdyMassDispatchProxyFixture Fixture;

	const FCrowdyMassDispatchProxyProbeReading Reading = CrowdyMassProbeDispatchProxy(Fixture.World,
		ACrowdyMassDispatchProxyTestActor::StaticClass(), TEXT("ProxyBody"), /*bSpawnComparisonActor=*/false);

	if (!TestTrue(FString::Printf(TEXT("the probe measured something: %s"), *Reading.Refusal),
		Reading.Refusal.IsEmpty()))
	{
		return false;
	}

	// Asserted before the registration reading, so a probe that found no components at all cannot pass this
	// case by having nothing to report about them.
	if (!TestTrue(TEXT("the proxy carries the class's native components"), Reading.ProxyComponents.Num() >= 2))
	{
		return false;
	}

	for (const FCrowdyMassProxyComponentReading& Component : Reading.ProxyComponents)
	{
		TestFalse(FString::Printf(TEXT("'%s' is not registered"), *Component.Name.ToString()),
			Component.bRegistered);
	}

	const ACrowdyMassDispatchProxyTestActor* Proxy =
		Cast<ACrowdyMassDispatchProxyTestActor>(Reading.Proxy.Get());
	if (!TestNotNull(TEXT("the probe handed the proxy back"), Proxy))
	{
		return false;
	}

	// What the body itself saw, rather than what the probe saw from outside. A body reaching for its mesh
	// gets a real pointer, which is why this failure mode is silent: nothing is null, nothing warns, and
	// the montage simply never plays.
	TestTrue(TEXT("the body found its mesh"), Proxy->bBodySawMesh);
	TestFalse(TEXT("but that mesh is unregistered"), Proxy->bBodySawRegisteredMesh);

	// Deliberately NOT asserted: that the mesh has no animation instance. It has none, but this fixture's
	// component carries no skeletal mesh asset either, and that alone would keep one from being created.
	// Registering the components makes the assertions above go red and leaves this one green, so asserting
	// it here would name registration and measure something else. What creates an animation instance is
	// USkeletalMeshComponent's registration, which is a fact of the engine rather than of this fixture.

	return true;
}

/**
 * The reset between entities must skip the proxy's own subobject references.
 *
 * The design resets the proxy's property block from the class default object so nothing carries from one
 * entity to the next. Written as a wholesale copy that also overwrites the properties pointing at the
 * proxy's OWN components with the default object's, which would leave every body from then on operating on
 * the class default object's mesh: shared across every entity, and belonging to an object that must never
 * be written to at all.
 *
 * Both halves are asserted together because either alone is satisfiable by a reset that does nothing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassDispatchProxyResetMustSkipSubobjectsTest,
	"CrowdySDK.Mass.DispatchProxyResetMustSkipSubobjects", CrowdyMassDispatchProxyTestFlags)
bool FCrowdyMassDispatchProxyResetMustSkipSubobjectsTest::RunTest(const FString& Parameters)
{
	FCrowdyMassDispatchProxyFixture Fixture;

	const FCrowdyMassDispatchProxyProbeReading Reading = CrowdyMassProbeDispatchProxy(Fixture.World,
		ACrowdyMassDispatchProxyTestActor::StaticClass(), NAME_None, /*bSpawnComparisonActor=*/false);

	if (!TestTrue(FString::Printf(TEXT("the probe measured something: %s"), *Reading.Refusal),
		Reading.Refusal.IsEmpty()))
	{
		return false;
	}

	TestTrue(TEXT("a wholesale reset from the class default object does steal the proxy's components"),
		Reading.bNaiveCdoResetStealsComponents);
	TestTrue(TEXT("a reset that skips subobject references leaves the proxy holding its own"),
		Reading.bGuardedCdoResetKeepsComponents);
	TestTrue(TEXT("and that guarded reset still restores a plain property, so it is not a reset that skipped everything"),
		Reading.bGuardedCdoResetClearsPlainProperties);

	return true;
}

/**
 * What a class only ever gets by being spawned.
 *
 * A native class carries the same components either way, which is what this pins. Everything a Blueprint
 * adds through its construction script is created by AActor::ExecuteConstruction, which spawning calls and
 * nothing else does, so for a Blueprint class this difference is not empty. A native fixture cannot show
 * that; it can only establish the baseline the Blueprint case is measured against, which is why the
 * Blueprint reading is taken in a play session through crowdy.mass.proxyprobe instead.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassDispatchProxyMatchesASpawnedNativeActorTest,
	"CrowdySDK.Mass.DispatchProxyMatchesASpawnedNativeActor", CrowdyMassDispatchProxyTestFlags)
bool FCrowdyMassDispatchProxyMatchesASpawnedNativeActorTest::RunTest(const FString& Parameters)
{
	FCrowdyMassDispatchProxyFixture Fixture;

	const FCrowdyMassDispatchProxyProbeReading Reading = CrowdyMassProbeDispatchProxy(Fixture.World,
		ACrowdyMassDispatchProxyTestActor::StaticClass(), NAME_None, /*bSpawnComparisonActor=*/true);

	if (!TestTrue(FString::Printf(TEXT("the probe measured something: %s"), *Reading.Refusal),
		Reading.Refusal.IsEmpty()))
	{
		return false;
	}

	if (!TestTrue(TEXT("a comparison actor was actually spawned"), Reading.bComparisonSpawned))
	{
		return false;
	}

	TestEqual(TEXT("a spawned actor of a native class carries the same number of components as the proxy"),
		Reading.SpawnedComponentCount, Reading.ProxyComponents.Num());
	TestEqual(TEXT("so nothing is missing from the proxy for a native class"),
		Reading.ComponentsOnlyOnASpawnedActor.Num(), 0);

	return true;
}

// Deciding which properties a reset copies means walking the whole property chain and testing every object
// property for whether it points at one of the proxy's own subobjects. That is per-event work for an answer
// that cannot change, so it is worked out once per class. This is the case that says the cached answer is
// the same answer, including after a body has written to the proxy.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassDispatchProxyResetListIsClassStableTest,
	"CrowdySDK.Mass.DispatchProxyResetListIsClassStable", CrowdyMassDispatchProxyTestFlags)
bool FCrowdyMassDispatchProxyResetListIsClassStableTest::RunTest(const FString& Parameters)
{
	FCrowdyMassDispatchProxyFixture Fixture;

	AActor* Proxy = CrowdyMassDispatchProxy::Construct(
		Fixture.World, ACrowdyMassDispatchProxyTestActor::StaticClass());
	if (!TestNotNull(TEXT("a proxy was built"), Proxy))
	{
		return false;
	}

	TArray<FProperty*> ResetProperties;
	TArray<FProperty*> SubobjectProperties;
	TArray<TObjectPtr<UObject>> SubobjectValues;
	CrowdyMassDispatchProxy::BuildResetPlan(Proxy, ResetProperties, SubobjectProperties, SubobjectValues);

	TestTrue(TEXT("the reset list has entries, so a cached empty list is not what makes the reset look correct"),
		ResetProperties.Num() > 0);
	TestTrue(TEXT("and the proxy's own components were recognised, so the plan has two halves"),
		SubobjectProperties.Num() > 0);
	TestEqual(TEXT("with a value recorded for each"), SubobjectValues.Num(), SubobjectProperties.Num());

	int32 ChainLength = 0;
	for (FProperty* Property = Proxy->GetClass()->PropertyLink; Property; Property = Property->PropertyLinkNext)
	{
		++ChainLength;
	}
	TestTrue(TEXT("and the reset list is shorter than the whole chain, so the subobject split did run"),
		ResetProperties.Num() < ChainLength);

	ACrowdyMassDispatchProxyTestActor* Typed = Cast<ACrowdyMassDispatchProxyTestActor>(Proxy);
	if (!TestNotNull(TEXT("the proxy is of the fixture class"), Typed))
	{
		return false;
	}

	USceneComponent* OwnRoot = Typed->ProxyRoot;
	if (!TestNotNull(TEXT("the proxy has a component of its own to protect"), OwnRoot))
	{
		return false;
	}

	// A second proxy only as a source of a component this one does not own, which is what a body reassigning
	// a component pointer looks like from here.
	AActor* Other = CrowdyMassDispatchProxy::Construct(
		Fixture.World, ACrowdyMassDispatchProxyTestActor::StaticClass());
	ACrowdyMassDispatchProxyTestActor* OtherTyped = Cast<ACrowdyMassDispatchProxyTestActor>(Other);
	if (!TestNotNull(TEXT("a second proxy was built"), OtherTyped))
	{
		return false;
	}

	Typed->LastValue = 4242;
	Typed->CarriedValue = 99;
	Typed->ProxyRoot = OtherTyped->ProxyRoot;

	CrowdyMassDispatchProxy::ResetToClassDefaults(Proxy, ResetProperties, SubobjectProperties, SubobjectValues);

	TestEqual(TEXT("resetting through the cached plan still clears what a body wrote"), Typed->LastValue, 0);
	TestEqual(TEXT("and the second written member too"), Typed->CarriedValue, 0);

	// The case caching gets wrong if the subobject half is SKIPPED rather than restored: the reassignment
	// would survive into every entity dispatched afterwards, because a cached plan never re-asks.
	TestTrue(TEXT("and a component pointer a body reassigned is put back to the proxy's own component"),
		Typed->ProxyRoot == OwnRoot);
	TestFalse(TEXT("rather than being left pointing at another object's component"),
		Typed->ProxyRoot == OtherTyped->ProxyRoot);

	// The other direction, which is the reason the subobject half is not simply copied from the class
	// default object: doing that would hand every entity the default object's component.
	const UObject* Cdo = Proxy->GetClass()->GetDefaultObject();
	const ACrowdyMassDispatchProxyTestActor* CdoTyped = Cast<const ACrowdyMassDispatchProxyTestActor>(Cdo);
	if (CdoTyped)
	{
		TestFalse(TEXT("and never the default object's component"),
			Typed->ProxyRoot == CdoTyped->ProxyRoot);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
