#include "Utils/CrowdyUtilities.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Data/CrowdyEntityTypes.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Replication/Components/CrowdyEntityComponent.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyUtilitiesTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

// The Blueprint "Is Crowdy Entity Locally Controlled" helper is the gate game code puts in front of input, AI and
// anything else that must run on exactly one client, so its false answers matter as much as its true one: an actor
// that is not a Crowdy entity, or one whose entity never received an identity, must never read as locally
// controlled. It answers purely from the actor's entity component and needs no world context.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyUtilitiesLocallyControlledTest,
	"CrowdySDK.CrowdyServices.EntityLocallyControlled", CrowdyUtilitiesTestFlags)
bool FCrowdyUtilitiesLocallyControlledTest::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("a null actor is not locally controlled"),
		UCrowdyUtilities::IsCrowdyEntityLocallyControlled(nullptr));

	AActor* PlainActor = NewObject<AActor>(GetTransientPackage());
	if (!TestNotNull(TEXT("plain actor created"), PlainActor))
	{
		return false;
	}
	TestFalse(TEXT("an actor that is not a Crowdy entity is not locally controlled"),
		UCrowdyUtilities::IsCrowdyEntityLocallyControlled(PlainActor));

	AActor* EntityActor = NewObject<AActor>(GetTransientPackage());
	UCrowdyEntityComponent* Component = NewObject<UCrowdyEntityComponent>(EntityActor);
	if (!TestNotNull(TEXT("entity component created"), Component))
	{
		return false;
	}
	if (!TestNotNull(TEXT("the component is reachable from its actor"),
		UCrowdyUtilities::GetCrowdyEntityComponent(EntityActor)))
	{
		return false;
	}

	TestFalse(TEXT("an entity with no identity yet is not locally controlled"),
		UCrowdyUtilities::IsCrowdyEntityLocallyControlled(EntityActor));

	Component->AssignPooledIdentity(FGuid::NewGuid(), FGuid::NewGuid(), ECrowdyRole::Owner, 0);
	TestTrue(TEXT("an entity this client owns is locally controlled"),
		UCrowdyUtilities::IsCrowdyEntityLocallyControlled(EntityActor));

	Component->AssignPooledIdentity(FGuid::NewGuid(), FGuid::NewGuid(), ECrowdyRole::RemoteProxy, 0);
	TestFalse(TEXT("a remote proxy is not locally controlled"),
		UCrowdyUtilities::IsCrowdyEntityLocallyControlled(EntityActor));

	// Host-owned with no elected host fails closed, so no client claims a world entity before a host is known.
	Component->AssignPooledIdentity(FGuid::NewGuid(), FGuid(), ECrowdyRole::HostOwned, 0);
	TestFalse(TEXT("a host-owned entity with no known host fails closed"),
		UCrowdyUtilities::IsCrowdyEntityLocallyControlled(EntityActor));

	// Back to an owned identity, then pooled: clearing the identity takes local control away again.
	Component->AssignPooledIdentity(FGuid::NewGuid(), FGuid::NewGuid(), ECrowdyRole::Owner, 0);
	Component->ClearIdentity();
	TestFalse(TEXT("an entity returned to the pool is not locally controlled"),
		UCrowdyUtilities::IsCrowdyEntityLocallyControlled(EntityActor));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
