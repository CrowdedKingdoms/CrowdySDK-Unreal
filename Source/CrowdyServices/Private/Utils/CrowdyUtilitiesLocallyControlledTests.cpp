#include "Utils/CrowdyUtilities.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Data/CrowdyEntityTypes.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "UObject/Package.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyUtilitiesTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyUtilitiesLeaderEntityNotPlayerTest,
	"CrowdySDK.CrowdyServices.ALeaderEntityIsNotPlayerControlled", CrowdyUtilitiesTestFlags)
bool FCrowdyUtilitiesLeaderEntityNotPlayerTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Editor);
	Context.SetCurrentWorld(World);

	AActor* Actor = World->SpawnActor<AActor>();
	UCrowdyEntityComponent* Component = NewObject<UCrowdyEntityComponent>(Actor);
	Component->RegisterComponent();
	Component->Mode = ECrowdyEntityMode::Dynamic;
	TestTrue(TEXT("a moving entity reads as a player's"), UCrowdyUtilities::IsCrowdyEntityPlayerControlled(Actor, Actor));

	Component->OwnedBy = ECrowdyOwnedBy::Leader;
	TestFalse(TEXT("one that follows the Leader does not"), UCrowdyUtilities::IsCrowdyEntityPlayerControlled(Actor, Actor));

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(/*bInformEngineOfWorld=*/false);
	return true;
}

// The Blueprint "Is Crowdy Entity Locally Owned" helper is the gate game code puts in front of input, AI and anything
// else that must run on exactly one client, so its false answers matter as much as its true one: an actor that is not
// a Crowdy entity, or one whose entity never received an identity, must never read as locally owned. For an actor it
// answers purely from the entity component and needs no world context.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyUtilitiesLocallyOwnedTest,
	"CrowdySDK.CrowdyServices.EntityLocallyOwned", CrowdyUtilitiesTestFlags)
bool FCrowdyUtilitiesLocallyOwnedTest::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("nothing is not locally owned"), UCrowdyUtilities::IsCrowdyEntityLocallyOwned(nullptr));

	AActor* PlainActor = NewObject<AActor>(GetTransientPackage());
	if (!TestNotNull(TEXT("plain actor created"), PlainActor))
	{
		return false;
	}
	TestFalse(TEXT("an actor that is not a Crowdy entity is not locally owned"),
		UCrowdyUtilities::IsCrowdyEntityLocallyOwned(PlainActor));

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

	TestFalse(TEXT("an entity with no identity yet is not locally owned"),
		UCrowdyUtilities::IsCrowdyEntityLocallyOwned(EntityActor));

	Component->AssignPooledIdentity(FGuid(), FGuid::NewGuid(), ECrowdyRole::Owner, 0);
	TestFalse(TEXT("an owner role without an entity id is not locally owned"),
		UCrowdyUtilities::IsCrowdyEntityLocallyOwned(EntityActor));

	Component->AssignPooledIdentity(FGuid::NewGuid(), FGuid::NewGuid(), ECrowdyRole::Owner, 0);
	TestTrue(TEXT("an entity this client owns is locally owned"), UCrowdyUtilities::IsCrowdyEntityLocallyOwned(EntityActor));

	bool bSwitched = false;
	UCrowdyUtilities::SwitchIsCrowdyEntityLocallyOwned(EntityActor, bSwitched);
	TestTrue(TEXT("the switch takes the same branch"), bSwitched);
	TestTrue(TEXT("an object inside the actor, such as its component as Self, asks about the actor"),
		UCrowdyUtilities::IsCrowdyEntityLocallyOwned(Component));

	Component->AssignPooledIdentity(FGuid::NewGuid(), FGuid::NewGuid(), ECrowdyRole::RemoteProxy, 0);
	TestFalse(TEXT("a remote proxy is not locally owned"), UCrowdyUtilities::IsCrowdyEntityLocallyOwned(EntityActor));
	UCrowdyUtilities::SwitchIsCrowdyEntityLocallyOwned(EntityActor, bSwitched);
	TestFalse(TEXT("and the switch takes the other branch"), bSwitched);

	// A world entity with no elected host fails closed, so no client claims it before a host is known.
	Component->AssignPooledIdentity(FGuid::NewGuid(), FGuid(), ECrowdyRole::HostOwned, 0);
	TestFalse(TEXT("a world entity with no known host fails closed"),
		UCrowdyUtilities::IsCrowdyEntityLocallyOwned(EntityActor));

	// Back to an owned identity, then pooled: clearing the identity takes ownership away again.
	Component->AssignPooledIdentity(FGuid::NewGuid(), FGuid::NewGuid(), ECrowdyRole::Owner, 0);
	Component->ClearIdentity();
	TestFalse(TEXT("an entity returned to the pool is not locally owned"),
		UCrowdyUtilities::IsCrowdyEntityLocallyOwned(EntityActor));
	return true;
}

// A replicated subsystem has no component, so the answer comes from its registry record: the elected host writes a
// world-owned record, and a client writes a record it owns.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyUtilitiesRecordLocallyOwnedTest,
	"CrowdySDK.CrowdyServices.RecordLocallyOwned", CrowdyUtilitiesTestFlags)
bool FCrowdyUtilitiesRecordLocallyOwnedTest::RunTest(const FString& Parameters)
{
	const FGuid Local = FGuid::NewGuid();
	const FGuid Other = FGuid::NewGuid();

	FCrowdyEntityRecord World;
	World.NetID = FGuid::NewGuid();
	World.Role = ECrowdyRole::HostOwned;
	TestTrue(TEXT("the elected host writes a world-owned record"), UCrowdyUtilities::IsRecordLocallyOwned(World, Local, Local));
	TestFalse(TEXT("another client does not"), UCrowdyUtilities::IsRecordLocallyOwned(World, Local, Other));
	TestFalse(TEXT("nobody does before a host is known"), UCrowdyUtilities::IsRecordLocallyOwned(World, Local, FGuid()));
	TestFalse(TEXT("not even a client that has no player id yet"), UCrowdyUtilities::IsRecordLocallyOwned(World, FGuid(), FGuid()));

	FCrowdyEntityRecord Owned = World;
	Owned.Role = ECrowdyRole::Owner;
	Owned.OwnerID = Local;
	TestTrue(TEXT("a client writes a record it owns"), UCrowdyUtilities::IsRecordLocallyOwned(Owned, Local, Other));
	Owned.OwnerID = Other;
	TestFalse(TEXT("but not one another client owns"), UCrowdyUtilities::IsRecordLocallyOwned(Owned, Local, Local));

	FCrowdyEntityRecord Mirrored = World;
	Mirrored.Role = ECrowdyRole::RemoteProxy;
	Mirrored.OwnerID = Local;
	TestFalse(TEXT("a record that only mirrors the entity here is not written here, whatever owner id it carries"),
		UCrowdyUtilities::IsRecordLocallyOwned(Mirrored, Local, Local));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
