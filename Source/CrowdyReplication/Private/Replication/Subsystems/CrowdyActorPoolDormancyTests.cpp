// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Data/FCrowdyPoolConfig.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/State/CrowdyStateTestTarget.h"
#include "Replication/Subsystems/CrowdyActorPoolSubsystem.h"

namespace
{
	constexpr EAutomationTestFlags PoolDormancyTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// An editor world, deliberately. A real Game world is what the pool normally lives in, but standing one up
	// headlessly runs the engine's own play initialisation and trips an assert in MassEntitySubsystem, so the
	// subsystem is constructed directly against an editor world instead. That is enough for what is asserted
	// here, because the question is what the pool does to its actors before their spawn finishes, not what a
	// world does afterwards.
	struct FCrowdyPoolTestWorld
	{
		UWorld* World = nullptr;

		FCrowdyPoolTestWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Editor);
			Context.SetCurrentWorld(World);
		}

		~FCrowdyPoolTestWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(/*bInformEngineOfWorld=*/false);
		}
	};
}

// A pre-warmed pool actor stands for no entity, so the pool must mark it dormant BEFORE its spawn finishes.
// Unregistering it afterwards is not the same thing, and that ordering was the whole defect: registration is
// delivered synchronously, and the Game Model answers it by ensuring a server container row that no later
// cleanup can recall. A pool of 8 pre-warmed actors carrying two containers each sent 16 such ensures for
// actors that stood for nothing, and because every client derives the same ids for its own pool, two clients
// raced to create the same rows and each was refused on the ones the other won.
//
// This asserts the marking rather than the absence of a record, because an editor world never begins play, so
// "nothing registered" would be true here whatever the pool did. The unpooled actor below is the control that
// keeps the assertion honest: the flag has to be false somewhere, or it proves nothing when it is true.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPoolPrewarmMarksDormantTest,
	"CrowdySDK.CrowdyReplication.PoolPrewarmMarksDormant", PoolDormancyTestFlags)
bool FCrowdyPoolPrewarmMarksDormantTest::RunTest(const FString& Parameters)
{
	FCrowdyPoolTestWorld TestWorld;
	if (!TestNotNull(TEXT("the test world exists"), TestWorld.World))
	{
		return false;
	}

	// Control: an actor spawned outside the pool is not dormant, so a true answer below means something.
	AActor* Unpooled = TestWorld.World->SpawnActor<ACrowdyStateHostOverrideActor>();
	if (!TestNotNull(TEXT("the control actor spawned"), Unpooled))
	{
		return false;
	}
	UCrowdyEntityComponent* UnpooledComponent = Unpooled->FindComponentByClass<UCrowdyEntityComponent>();
	if (!TestNotNull(TEXT("the control actor carries an entity component"), UnpooledComponent))
	{
		return false;
	}
	TestFalse(TEXT("CONTROL: an actor spawned outside the pool is not dormant"),
		UnpooledComponent->IsPooledDormant());

	UCrowdyActorPoolSubsystem* Pool = NewObject<UCrowdyActorPoolSubsystem>(TestWorld.World);
	if (!TestNotNull(TEXT("the pool subsystem was created"), Pool))
	{
		return false;
	}

	FCrowdyPoolConfig Config;
	Config.ActorClass = ACrowdyStateHostOverrideActor::StaticClass();
	Config.PoolSize   = 4;
	Pool->RegisterPool(Config);

	int32 Pooled = 0;
	int32 Dormant = 0;
	for (TObjectIterator<UCrowdyEntityComponent> It; It; ++It)
	{
		AActor* Owner = It->GetOwner();
		if (!IsValid(Owner) || Owner == Unpooled || Owner->GetWorld() != TestWorld.World)
		{
			continue;
		}
		++Pooled;
		Dormant += It->IsPooledDormant() ? 1 : 0;
	}

	TestEqual(TEXT("the pool pre-warmed the configured number of actors"), Pooled, 4);
	TestEqual(TEXT("every pre-warmed pool actor is dormant"), Dormant, Pooled);

	return true;
}

// Dormancy has to end when the pool hands the actor a real proxy identity, or a reused pool actor would stay
// invisible to every listener for the rest of its life. This is the other half of the contract, and without it
// the fix above could be "mark everything dormant forever" and still pass.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPooledIdentityEndsDormancyTest,
	"CrowdySDK.CrowdyReplication.PooledIdentityEndsDormancy", PoolDormancyTestFlags)
bool FCrowdyPooledIdentityEndsDormancyTest::RunTest(const FString& Parameters)
{
	FCrowdyPoolTestWorld TestWorld;
	AActor* Actor = TestWorld.World ? TestWorld.World->SpawnActor<ACrowdyStateHostOverrideActor>() : nullptr;
	UCrowdyEntityComponent* Component = Actor ? Actor->FindComponentByClass<UCrowdyEntityComponent>() : nullptr;
	if (!TestNotNull(TEXT("the actor carries an entity component"), Component))
	{
		return false;
	}

	Component->MarkPooledDormant();
	TestTrue(TEXT("marking makes it dormant"), Component->IsPooledDormant());

	const FGuid ProxyID = FGuid::NewGuid();
	Component->AssignPooledIdentity(ProxyID, FGuid::NewGuid(), ECrowdyRole::RemoteProxy, 0);

	TestFalse(TEXT("being handed a real proxy identity ends dormancy"), Component->IsPooledDormant());
	TestEqual(TEXT("and the component carries that identity"), Component->GetNetID(), ProxyID);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
