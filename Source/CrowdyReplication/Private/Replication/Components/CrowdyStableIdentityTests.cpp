#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/GameModel/CrowdyModelIdentity.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "WorldPartition/ActorInstanceGuids.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyStableIdentityTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

// ComputeStableNetID derives from the engine's per-placement ActorInstanceGuid, and reports whether it had
// to fall back to the path hash. A bare transient actor has no engine instance guid, so it exercises the
// fallback branch (the SDK-owned logic) and must reproduce the path derivation exactly. The primary
// ActorInstanceGuid-valid path is a level-placed/streamed actor and is covered by the cooked live gate,
// not a headless fixture.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStableNetIDFallbackTest,
	"CrowdySDK.GameModel.StableNetIDFallback", CrowdyStableIdentityTestFlags)
bool FCrowdyStableNetIDFallbackTest::RunTest(const FString& Parameters)
{
	AActor* Actor = NewObject<AActor>(GetTransientPackage());
	UCrowdyEntityComponent* Component = NewObject<UCrowdyEntityComponent>(Actor);
	if (!TestNotNull(TEXT("component created"), Component))
	{
		return false;
	}

	// Sanity: a transient actor genuinely has no engine instance guid, so the fallback is the branch under test.
	TestFalse(TEXT("transient actor has no engine instance guid"),
		FActorInstanceGuid::GetActorInstanceGuid(*Actor).IsValid());

	bool bUsedPathFallback = false;
	const FGuid NetID = Component->ComputeStableNetID(bUsedPathFallback);
	TestTrue(TEXT("no engine guid reports the path fallback"), bUsedPathFallback);
	TestEqual(TEXT("fallback reproduces the path derivation exactly"),
		NetID, FCrowdyModelIdentity::StableNetIDFromActorPath(Actor->GetPathName()));
	TestTrue(TEXT("resolved NetID is valid"), NetID.IsValid());

	return true;
}

// The placement guid is a shared id only for an actor loaded with its level. An actor created during play is handed
// a fresh guid by the engine in an editor build and none at all in a packaged one, so honouring it would give the
// same actor two different ids depending on how the game was built. Both inputs are required.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStablePlacementGuidGateTest,
	"CrowdySDK.GameModel.StablePlacementGuidGate", CrowdyStableIdentityTestFlags)
bool FCrowdyStablePlacementGuidGateTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("level-placed with a guid uses the placement id"),
		UCrowdyEntityComponent::ShouldUsePlacementGuid(true, true));
	TestFalse(TEXT("created during play does not, even when the engine offers a guid"),
		UCrowdyEntityComponent::ShouldUsePlacementGuid(false, true));
	TestFalse(TEXT("level-placed with no guid falls back"),
		UCrowdyEntityComponent::ShouldUsePlacementGuid(true, false));
	TestFalse(TEXT("neither falls back"),
		UCrowdyEntityComponent::ShouldUsePlacementGuid(false, false));

	// A transient actor is not loaded from a map, so the component agrees with the rule end to end.
	AActor* Actor = NewObject<AActor>(GetTransientPackage());
	UCrowdyEntityComponent* Component = NewObject<UCrowdyEntityComponent>(Actor);
	if (!TestNotNull(TEXT("component created"), Component))
	{
		return false;
	}
	TestFalse(TEXT("a transient actor is not a level-placed actor"), Actor->IsNetStartupActor());

	bool bUsedPathFallback = false;
	Component->ComputeStableNetID(bUsedPathFallback);
	TestTrue(TEXT("so it takes the path fallback"), bUsedPathFallback);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
