// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Data/CrowdyActorPoolBackend.h"
#include "Data/CrowdyActorPoolBackendConfig.h"
#include "Data/CrowdyEntityTypes.h"
#include "Data/CrowdyRenderingBackendTestTypes.h"
#include "Data/FCrowdyPoolConfig.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Replication/State/CrowdyStateTestTarget.h"
#include "Replication/Subsystems/CrowdyActorPoolSubsystem.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Package.h"

// The pool grows past its pre-warm size up to a cap, and the backend secures a pool actor before it destroys the
// actor a spawn event already made for the entity. At the cap that actor is adopted rather than lost.
namespace
{
	constexpr EAutomationTestFlags PoolGrowthTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// An editor world, as in the dormancy cases: a headless Game world trips an assert in MassEntitySubsystem, and
	// the pool and entity subsystems are constructed directly against this one instead.
	struct FCrowdyPoolGrowthTestWorld
	{
		UWorld* World = nullptr;

		FCrowdyPoolGrowthTestWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Editor);
			Context.SetCurrentWorld(World);
		}

		~FCrowdyPoolGrowthTestWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(/*bInformEngineOfWorld=*/false);
		}
	};

	UCrowdyActorPoolSubsystem* MakePoolForGrowthTest(UWorld* World, const int32 PoolSize, const int32 MaxPoolSize)
	{
		UCrowdyActorPoolSubsystem* Pool = NewObject<UCrowdyActorPoolSubsystem>(World);

		FCrowdyPoolConfig Config;
		Config.ActorClass  = ACrowdyStateHostOverrideActor::StaticClass();
		Config.PoolSize    = PoolSize;
		Config.MaxPoolSize = MaxPoolSize;
		Pool->RegisterPool(Config);
		return Pool;
	}

	// A pool backend on an editor world whose pool pre-warms one actor and may never grow past it.
	struct FCrowdyPoolBackendFixture
	{
		UCrowdyActorPoolSubsystem* Pool = nullptr;
		UCrowdyEntitySubsystem* Entities = nullptr;
		UCrowdyActorPoolBackend* Backend = nullptr;

		explicit FCrowdyPoolBackendFixture(UWorld* World)
		{
			Pool = NewObject<UCrowdyActorPoolSubsystem>(World);
			Entities = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());

			UCrowdyActorPoolBackendConfig* Config = NewObject<UCrowdyActorPoolBackendConfig>(GetTransientPackage());
			Config->DefaultPoolSizePerClass = 1;
			Config->MaxPoolSizePerClass = 1;

			Backend = NewObject<UCrowdyActorPoolBackend>(World);
			Backend->InitializeForTest(Pool, Entities, Config);
		}

		void Activate(const int32 SlotId, const FGuid& UUID) const
		{
			Backend->ActivateInstance(SlotId, UUID, ACrowdyStateHostOverrideActor::StaticClass(), FInstancedStruct());
		}

		// What FinishRemoteSpawn leaves behind: an actor of the entity's class registered as its remote proxy.
		AActor* SpawnOrphan(UWorld* World, const FGuid& UUID) const
		{
			AActor* Orphan = World->SpawnActor<ACrowdyStateHostOverrideActor>();

			FCrowdyEntityRecord Record;
			Record.NetID = UUID;
			Record.Role = ECrowdyRole::RemoteProxy;
			Record.Participant = Orphan;
			Entities->RegisterEntity(Record);
			return Orphan;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPoolGrowsToCapThenRefusesTest,
	"CrowdySDK.CrowdyReplication.PoolGrowsToCapThenRefuses", PoolGrowthTestFlags)
bool FCrowdyPoolGrowsToCapThenRefusesTest::RunTest(const FString& Parameters)
{
	FCrowdyPoolGrowthTestWorld TestWorld;
	UCrowdyActorPoolSubsystem* Pool = MakePoolForGrowthTest(TestWorld.World, 2, 4);

	// Two refusals below, one warning: a retry per update must not become a warning per update.
	AddExpectedMessagePlain(TEXT("is at its cap of 4 actors"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);

	TSet<AActor*> Acquired;
	for (int32 Index = 0; Index < 4; Index++)
	{
		AActor* Actor = Pool->AcquireActor(ACrowdyStateHostOverrideActor::StaticClass());
		TestNotNull(*FString::Printf(TEXT("acquire %d of 4 is served, past the pre-warm size of 2"), Index + 1), Actor);
		Acquired.Add(Actor);
	}
	TestEqual(TEXT("every acquire got an actor of its own"), Acquired.Num(), 4);

	TestNull(TEXT("the acquire past the cap is refused"), Pool->AcquireActor(ACrowdyStateHostOverrideActor::StaticClass()));
	TestNull(TEXT("and so is the next"), Pool->AcquireActor(ACrowdyStateHostOverrideActor::StaticClass()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPoolCapNeverBelowPrewarmTest,
	"CrowdySDK.CrowdyReplication.PoolCapNeverBelowPrewarm", PoolGrowthTestFlags)
bool FCrowdyPoolCapNeverBelowPrewarmTest::RunTest(const FString& Parameters)
{
	FCrowdyPoolGrowthTestWorld TestWorld;
	UCrowdyActorPoolSubsystem* Pool = MakePoolForGrowthTest(TestWorld.World, 3, 1);

	AddExpectedMessagePlain(TEXT("is at its cap of 3 actors"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);

	TArray<AActor*> Acquired;
	for (int32 Index = 0; Index < 3; Index++)
	{
		AActor* Actor = Pool->AcquireActor(ACrowdyStateHostOverrideActor::StaticClass());
		TestNotNull(*FString::Printf(TEXT("pre-warmed actor %d of 3 is served under a cap of 1"), Index + 1), Actor);
		Acquired.Add(Actor);
	}
	TestNull(TEXT("the pool does not grow past its pre-warm size when the cap is below it"),
		Pool->AcquireActor(ACrowdyStateHostOverrideActor::StaticClass()));

	// Once those actors are gone the pool grows back to its pre-warm size, not to the lower cap.
	for (AActor* Actor : Acquired)
	{
		if (Actor)
			Actor->Destroy();
	}
	for (int32 Index = 0; Index < 3; Index++)
	{
		TestNotNull(*FString::Printf(TEXT("replacement %d of 3 is grown under a cap of 1"), Index + 1),
			Pool->AcquireActor(ACrowdyStateHostOverrideActor::StaticClass()));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPoolDestroyedActorFreesCapacityTest,
	"CrowdySDK.CrowdyReplication.PoolDestroyedActorFreesCapacity", PoolGrowthTestFlags)
bool FCrowdyPoolDestroyedActorFreesCapacityTest::RunTest(const FString& Parameters)
{
	FCrowdyPoolGrowthTestWorld TestWorld;
	UCrowdyActorPoolSubsystem* Pool = MakePoolForGrowthTest(TestWorld.World, 1, 1);

	AActor* First = Pool->AcquireActor(ACrowdyStateHostOverrideActor::StaticClass());
	if (!TestNotNull(TEXT("the only pooled actor is served"), First))
	{
		return false;
	}

	// A remote destroy event destroys the entity's actor wherever it came from, pooled or not.
	First->Destroy();

	AActor* Second = Pool->AcquireActor(ACrowdyStateHostOverrideActor::StaticClass());
	TestNotNull(TEXT("a pooled actor destroyed elsewhere no longer counts against the cap"), Second);
	TestTrue(TEXT("and the actor served is a new one"), Second != First);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPoolSecuresActorBeforeOrphanTest,
	"CrowdySDK.CrowdyReplication.PoolBackendReplacesOrphanOnceSecured", PoolGrowthTestFlags)
bool FCrowdyPoolSecuresActorBeforeOrphanTest::RunTest(const FString& Parameters)
{
	FCrowdyPoolGrowthTestWorld TestWorld;
	const FCrowdyPoolBackendFixture Fixture(TestWorld.World);

	const FGuid Entity = FGuid::NewGuid();
	AActor* Orphan = Fixture.SpawnOrphan(TestWorld.World, Entity);

	Fixture.Activate(0, Entity);

	AActor* SlotActor = Fixture.Backend->GetSlotActorForTest(0);
	TestNotNull(TEXT("the slot holds an actor"), SlotActor);
	TestTrue(TEXT("and it is a pool actor, not the orphan"), SlotActor != Orphan);
	TestFalse(TEXT("the orphan is destroyed once a pool actor is secured"), IsValid(Orphan));
	TestTrue(TEXT("the entity's record now names the pool actor"), Fixture.Entities->FindEntity(Entity) == SlotActor);
	TestTrue(TEXT("the backend reports the slot active"), Fixture.Backend->IsInstanceActive(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPoolAtCapAdoptsOrphanTest,
	"CrowdySDK.CrowdyReplication.PoolBackendAdoptsOrphanAtCap", PoolGrowthTestFlags)
bool FCrowdyPoolAtCapAdoptsOrphanTest::RunTest(const FString& Parameters)
{
	FCrowdyPoolGrowthTestWorld TestWorld;
	const FCrowdyPoolBackendFixture Fixture(TestWorld.World);

	AddExpectedMessagePlain(TEXT("is at its cap of 1 actors"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);

	const FGuid Holder = FGuid::NewGuid();
	Fixture.Activate(0, Holder);
	AActor* PoolActor = Fixture.Backend->GetSlotActorForTest(0);
	if (!TestNotNull(TEXT("the first entity takes the only pool actor"), PoolActor))
	{
		return false;
	}

	const FGuid Adopted = FGuid::NewGuid();
	AActor* Orphan = Fixture.SpawnOrphan(TestWorld.World, Adopted);
	Fixture.Activate(1, Adopted);

	TestTrue(TEXT("a refused acquire leaves the orphan alive"), IsValid(Orphan));
	TestTrue(TEXT("and adopts it as the slot's actor"), Fixture.Backend->GetSlotActorForTest(1) == Orphan);
	TestTrue(TEXT("its own record stays"), Fixture.Entities->FindEntity(Adopted) == Orphan);
	TestTrue(TEXT("the adopted slot is active"), Fixture.Backend->IsInstanceActive(1));

	const FGuid Stranded = FGuid::NewGuid();
	Fixture.Activate(2, Stranded);

	TestNull(TEXT("a refused acquire with no orphan leaves the slot empty"), Fixture.Backend->GetSlotActorForTest(2));
	TestFalse(TEXT("and the backend reports it inactive"), Fixture.Backend->IsInstanceActive(2));
	TestNull(TEXT("and registers nothing for it"), Fixture.Entities->FindEntity(Stranded));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAdoptedOrphanDestroyedOnDeactivateTest,
	"CrowdySDK.CrowdyReplication.PoolBackendDestroysAdoptedOrphanOnDeactivate", PoolGrowthTestFlags)
bool FCrowdyAdoptedOrphanDestroyedOnDeactivateTest::RunTest(const FString& Parameters)
{
	FCrowdyPoolGrowthTestWorld TestWorld;
	const FCrowdyPoolBackendFixture Fixture(TestWorld.World);

	const FGuid Holder = FGuid::NewGuid();
	Fixture.Activate(0, Holder);
	AActor* PoolActor = Fixture.Backend->GetSlotActorForTest(0);

	const FGuid Adopted = FGuid::NewGuid();
	AActor* Orphan = Fixture.SpawnOrphan(TestWorld.World, Adopted);
	Fixture.Activate(1, Adopted);
	if (!TestTrue(TEXT("the orphan was adopted"), Fixture.Backend->GetSlotActorForTest(1) == Orphan))
	{
		return false;
	}

	Fixture.Backend->DeactivateInstance(1, Adopted);

	TestFalse(TEXT("an adopted orphan is destroyed on deactivation"), IsValid(Orphan));
	TestNull(TEXT("and its record is gone"), Fixture.Entities->FindEntity(Adopted));
	TestFalse(TEXT("and the slot is inactive"), Fixture.Backend->IsInstanceActive(1));

	// The control: a pool actor is returned to the pool on deactivation, not destroyed, and serves the next entity.
	Fixture.Backend->DeactivateInstance(0, Holder);
	TestTrue(TEXT("a pool actor survives its deactivation"), IsValid(PoolActor));

	const FGuid Next = FGuid::NewGuid();
	Fixture.Activate(2, Next);
	TestTrue(TEXT("and is handed to the next entity"), Fixture.Backend->GetSlotActorForTest(2) == PoolActor);
	return true;
}

// A slot still holding its previous actor (a missed deactivation) is cleared before the new entity is activated.
// Only a record that names that stale actor is dropped, so the new entity's own spawn-event actor is still found
// and replaced rather than left alive with no slot.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStaleSlotActorKeepsOrphanRecordTest,
	"CrowdySDK.CrowdyReplication.PoolBackendStaleSlotKeepsOrphanRecord", PoolGrowthTestFlags)
bool FCrowdyStaleSlotActorKeepsOrphanRecordTest::RunTest(const FString& Parameters)
{
	FCrowdyPoolGrowthTestWorld TestWorld;
	const FCrowdyPoolBackendFixture Fixture(TestWorld.World);

	const FGuid Previous = FGuid::NewGuid();
	Fixture.Activate(0, Previous);
	AActor* PoolActor = Fixture.Backend->GetSlotActorForTest(0);
	if (!TestNotNull(TEXT("the previous entity holds the only pool actor"), PoolActor))
	{
		return false;
	}

	const FGuid Entity = FGuid::NewGuid();
	AActor* Orphan = Fixture.SpawnOrphan(TestWorld.World, Entity);
	Fixture.Activate(0, Entity);

	TestFalse(TEXT("the new entity's orphan is replaced"), IsValid(Orphan));
	TestTrue(TEXT("by the pool actor the stale slot gave back"), Fixture.Backend->GetSlotActorForTest(0) == PoolActor);
	TestTrue(TEXT("which the new entity's record now names"), Fixture.Entities->FindEntity(Entity) == PoolActor);
	return true;
}

// A spawn-event actor that registered for an entity whose slot never adopted it (a destroy, or a release, before any
// retry) is destroyed with the record, not left alive with none.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyUnadoptedOrphanDestroyedOnDeactivateTest,
	"CrowdySDK.CrowdyReplication.PoolBackendDestroysUnadoptedOrphanOnDeactivate", PoolGrowthTestFlags)
bool FCrowdyUnadoptedOrphanDestroyedOnDeactivateTest::RunTest(const FString& Parameters)
{
	FCrowdyPoolGrowthTestWorld TestWorld;
	const FCrowdyPoolBackendFixture Fixture(TestWorld.World);

	AddExpectedMessagePlain(TEXT("is at its cap of 1 actors"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);

	const FGuid Holder = FGuid::NewGuid();
	Fixture.Activate(0, Holder);
	AActor* PoolActor = Fixture.Backend->GetSlotActorForTest(0);

	const FGuid Stranded = FGuid::NewGuid();
	Fixture.Activate(1, Stranded);
	if (!TestFalse(TEXT("the second entity found no pool actor"), Fixture.Backend->IsInstanceActive(1)))
	{
		return false;
	}
	AActor* Orphan = Fixture.SpawnOrphan(TestWorld.World, Stranded);

	Fixture.Backend->DeactivateInstance(1, Stranded);
	TestFalse(TEXT("the unadopted orphan is destroyed"), IsValid(Orphan));
	TestNull(TEXT("with its record"), Fixture.Entities->FindRecord(Stranded));
	TestTrue(TEXT("and the other slot's pool actor is untouched"), IsValid(PoolActor) && Fixture.Entities->FindEntity(Holder) == PoolActor);

	// The control: an owner entity's own actor is never the backend's to destroy.
	AActor* OwnedActor = TestWorld.World->SpawnActor<ACrowdyStateHostOverrideActor>();
	const FGuid Owned = FGuid::NewGuid();
	FCrowdyEntityRecord OwnerRecord;
	OwnerRecord.NetID = Owned;
	OwnerRecord.Role = ECrowdyRole::Owner;
	OwnerRecord.Participant = OwnedActor;
	Fixture.Entities->RegisterEntity(OwnerRecord);

	Fixture.Backend->DeactivateInstance(2, Owned);
	TestTrue(TEXT("an owner entity's actor survives"), IsValid(OwnedActor));
	TestTrue(TEXT("with its record"), Fixture.Entities->FindEntity(Owned) == OwnedActor);
	return true;
}

// A remote destroy unregisters the entity and destroys its actor, after a delay when the component asks for one. The
// pool actor must not go back to the pool meanwhile, or it is handed to another entity and then destroyed under it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDestroyedEntityActorLeftToDestroyerTest,
	"CrowdySDK.CrowdyReplication.PoolBackendLeavesDestroyedEntityActorToItsDestroyer", PoolGrowthTestFlags)
bool FCrowdyDestroyedEntityActorLeftToDestroyerTest::RunTest(const FString& Parameters)
{
	FCrowdyPoolGrowthTestWorld TestWorld;
	const FCrowdyPoolBackendFixture Fixture(TestWorld.World);

	AddExpectedMessagePlain(TEXT("is at its cap of 1 actors"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);

	const FGuid Destroyed = FGuid::NewGuid();
	Fixture.Activate(0, Destroyed);
	AActor* PoolActor = Fixture.Backend->GetSlotActorForTest(0);
	if (!TestNotNull(TEXT("the entity takes the only pool actor"), PoolActor))
	{
		return false;
	}

	Fixture.Entities->UnregisterEntity(Destroyed);
	TestFalse(TEXT("a slot whose actor lost its record is not active"), Fixture.Backend->IsInstanceActive(0));

	Fixture.Backend->DeactivateInstance(0, Destroyed);
	TestTrue(TEXT("the destroyer's actor is left alive for it"), IsValid(PoolActor));

	const FGuid Next = FGuid::NewGuid();
	Fixture.Activate(1, Next);
	TestTrue(TEXT("and is not handed to the next entity"), Fixture.Backend->GetSlotActorForTest(1) != PoolActor);

	PoolActor->Destroy();
	Fixture.Activate(1, Next);
	AActor* Replacement = Fixture.Backend->GetSlotActorForTest(1);
	TestTrue(TEXT("once the destroyer is done the pool serves a new actor"), IsValid(Replacement) && Replacement != PoolActor);
	return true;
}

// The same for the retry that follows a remote destroy: the slot still holds the destroyer's actor, which it drops.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStaleSlotLeavesDestroyedActorTest,
	"CrowdySDK.CrowdyReplication.PoolBackendStaleSlotLeavesDestroyedActor", PoolGrowthTestFlags)
bool FCrowdyStaleSlotLeavesDestroyedActorTest::RunTest(const FString& Parameters)
{
	FCrowdyPoolGrowthTestWorld TestWorld;
	const FCrowdyPoolBackendFixture Fixture(TestWorld.World);

	AddExpectedMessagePlain(TEXT("is at its cap of 1 actors"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);

	const FGuid Respawned = FGuid::NewGuid();
	Fixture.Activate(0, Respawned);
	AActor* PoolActor = Fixture.Backend->GetSlotActorForTest(0);

	Fixture.Entities->UnregisterEntity(Respawned);
	Fixture.Activate(0, Respawned);
	TestTrue(TEXT("the retry does not take back the actor its destroyer holds"), Fixture.Backend->GetSlotActorForTest(0) != PoolActor);
	TestNull(TEXT("nor register it again"), Fixture.Entities->FindRecord(Respawned));
	return true;
}

// A pool actor drawing a locally owned entity is never registered, so it goes back to the pool even with no record.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyOwnerProxyReleasedWithoutRecordTest,
	"CrowdySDK.CrowdyReplication.PoolBackendReleasesOwnerProxyWithoutRecord", PoolGrowthTestFlags)
bool FCrowdyOwnerProxyReleasedWithoutRecordTest::RunTest(const FString& Parameters)
{
	FCrowdyPoolGrowthTestWorld TestWorld;
	const FCrowdyPoolBackendFixture Fixture(TestWorld.World);

	AActor* OwnedActor = TestWorld.World->SpawnActor<ACrowdyStateHostOverrideActor>();
	const FGuid Owned = FGuid::NewGuid();
	FCrowdyEntityRecord OwnerRecord;
	OwnerRecord.NetID = Owned;
	OwnerRecord.Role = ECrowdyRole::Owner;
	OwnerRecord.Participant = OwnedActor;
	Fixture.Entities->RegisterEntity(OwnerRecord);

	Fixture.Activate(0, Owned);
	AActor* Proxy = Fixture.Backend->GetSlotActorForTest(0);
	if (!TestTrue(TEXT("the owner entity is drawn by a pool actor"), IsValid(Proxy) && Proxy != OwnedActor))
	{
		return false;
	}
	TestTrue(TEXT("which counts as active though no record names it"), Fixture.Backend->IsInstanceActive(0));

	Fixture.Entities->UnregisterEntity(Owned);
	Fixture.Backend->DeactivateInstance(0, Owned);

	const FGuid Next = FGuid::NewGuid();
	Fixture.Activate(1, Next);
	TestTrue(TEXT("the proxy went back to the pool and serves the next entity"), Fixture.Backend->GetSlotActorForTest(1) == Proxy);
	return true;
}

// Release looks in the actor's own class pool, and only there.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPoolReleaseFindsOwnPoolTest,
	"CrowdySDK.CrowdyReplication.PoolReleaseFindsOwnPool", PoolGrowthTestFlags)
bool FCrowdyPoolReleaseFindsOwnPoolTest::RunTest(const FString& Parameters)
{
	FCrowdyPoolGrowthTestWorld TestWorld;
	UCrowdyActorPoolSubsystem* Pool = MakePoolForGrowthTest(TestWorld.World, 1, 1);

	FCrowdyPoolConfig ProbeConfig;
	ProbeConfig.ActorClass = ACrowdyPoolCollisionProbeActor::StaticClass();
	ProbeConfig.PoolSize = 1;
	ProbeConfig.MaxPoolSize = 1;
	Pool->RegisterPool(ProbeConfig);

	AActor* First = Pool->AcquireActor(ACrowdyStateHostOverrideActor::StaticClass());
	AActor* Probe = Pool->AcquireActor(ACrowdyPoolCollisionProbeActor::StaticClass());
	if (!TestNotNull(TEXT("the first pool serves"), First) || !TestNotNull(TEXT("the second pool serves"), Probe))
	{
		return false;
	}

	AddExpectedMessagePlain(TEXT("is at its cap of 1 actors"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);
	Pool->ReleaseActor(TestWorld.World->SpawnActor<ACrowdyPoolCollisionProbeActor>());
	TestNull(TEXT("an actor the pool never made frees nothing"), Pool->AcquireActor(ACrowdyPoolCollisionProbeActor::StaticClass()));

	Pool->ReleaseActor(Probe);
	Pool->ReleaseActor(First);
	TestTrue(TEXT("a released actor is served again by its own pool"), Pool->AcquireActor(ACrowdyPoolCollisionProbeActor::StaticClass()) == Probe);
	TestTrue(TEXT("and so is the other"), Pool->AcquireActor(ACrowdyStateHostOverrideActor::StaticClass()) == First);
	TestNull(TEXT("with nothing else freed"), Pool->AcquireActor(ACrowdyStateHostOverrideActor::StaticClass()));
	return true;
}

// Growth spawns mid-game at the origin, so a pooled actor is made with collision already off.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPoolActorSpawnsWithoutCollisionTest,
	"CrowdySDK.CrowdyReplication.PoolActorSpawnsWithoutCollision", PoolGrowthTestFlags)
bool FCrowdyPoolActorSpawnsWithoutCollisionTest::RunTest(const FString& Parameters)
{
	FCrowdyPoolGrowthTestWorld TestWorld;
	UCrowdyActorPoolSubsystem* Pool = NewObject<UCrowdyActorPoolSubsystem>(TestWorld.World);

	FCrowdyPoolConfig Config;
	Config.ActorClass = ACrowdyPoolCollisionProbeActor::StaticClass();
	Config.PoolSize = 1;
	Config.MaxPoolSize = 2;
	Pool->RegisterPool(Config);

	const ACrowdyPoolCollisionProbeActor* Prewarmed = Cast<ACrowdyPoolCollisionProbeActor>(Pool->AcquireActor(ACrowdyPoolCollisionProbeActor::StaticClass()));
	const ACrowdyPoolCollisionProbeActor* Grown = Cast<ACrowdyPoolCollisionProbeActor>(Pool->AcquireActor(ACrowdyPoolCollisionProbeActor::StaticClass()));
	if (!TestNotNull(TEXT("the pre-warmed actor"), Prewarmed) || !TestNotNull(TEXT("the grown actor"), Grown))
	{
		return false;
	}
	TestFalse(TEXT("the pre-warmed actor had no collision while it was built"), Prewarmed->bCollisionDuringConstruction);
	TestFalse(TEXT("nor did the grown one"), Grown->bCollisionDuringConstruction);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
