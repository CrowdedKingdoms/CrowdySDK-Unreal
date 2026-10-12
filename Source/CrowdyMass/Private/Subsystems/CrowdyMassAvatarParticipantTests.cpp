#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Data/CrowdyEntityTypes.h"
#include "Fragments/CrowdyMassFragments.h"
#include "HAL/PlatformProcess.h"
#include "Mass/CrowdyMassCharacterAvatar.h"
#include "Mass/CrowdyMassEntity.h"
#include "Mass/CrowdyMassEntityTestTypes.h"
#include "MassEntityManager.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"

// Covers what a crowd entity's avatar enrols as in the entity registry: the record it enrols under, the
// two orderings a respawn produces, what an evicted avatar leaves behind, and the reclaim grace window.
namespace
{
	constexpr EAutomationTestFlags CrowdyMassParticipantTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// A standalone Mass entity manager plus the subsystems a real world would supply, all owned here.
	// Nothing in this fixture needs a World, a spawner or a config asset: entities are registered exactly as
	// SpawnRemoteEntity's own bookkeeping would register them, and the registry is injected through the seam
	// that exists for that.
	struct FCrowdyMassParticipantTestFixture
	{
		TSharedPtr<FMassEntityManager> EntityManager;
		UCrowdyMassEntitySubsystem* Subsystem = nullptr;
		UCrowdyEntitySubsystem* Entities = nullptr;

		FCrowdyMassParticipantTestFixture()
		{
			// The owner is not optional even though the constructor defaults it away: PostInitialize
			// registers the engine's relation types, and each one constructs observers outered to this
			// owner, so a manager built with none tries to create an object with no outer.
			EntityManager = MakeShareable(new FMassEntityManager(GetTransientPackage()));

			FMassEntityManagerStorageInitParams InitParams;
			InitParams.Emplace<FMassEntityManager_InitParams_Concurrent>();
			EntityManager->Initialize(InitParams);
			EntityManager->PostInitialize();

			Subsystem = NewObject<UCrowdyMassEntitySubsystem>(GetTransientPackage());
			Entities = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());

			Subsystem->SetMassEntityManagerForTest(EntityManager.Get());
			Subsystem->SetEntitySubsystemForTest(Entities);
		}

		~FCrowdyMassParticipantTestFixture()
		{
			EntityManager->Deinitialize();
		}

		// A live, tracked entity carrying no fragments, registered under UUID exactly as SpawnRemoteEntity
		// would have registered it.
		FMassEntityHandle MakeTrackedEntity(const FGuid& UUID)
		{
			const FMassArchetypeHandle EmptyArchetype =
				EntityManager->CreateArchetype(MakeArrayView<const UScriptStruct*>(nullptr, 0));
			const FMassEntityHandle Entity = EntityManager->CreateEntity(EmptyArchetype);
			Subsystem->RegisterTrackedEntityForTest(UUID, Entity);
			return Entity;
		}

		/**
		 * A live, tracked entity whose identity fragment holds exactly what InitializeCrowdyFragments writes
		 * for an entity the registry has no record for: host-owned, with no owner.
		 *
		 * That value is the trap these tests exist for. It is written for a record-less entity because a
		 * world entity is the safe assumption for the send path, and it is also what a genuinely host-owned
		 * world entity carries, so the fragment answers identically for two different cases.
		 */
		FMassEntityHandle MakeUnsettledEntity(const FGuid& UUID, const FGuid& OwnerID = FGuid())
		{
			const UScriptStruct* Fragments[] = { FCrowdyRemoteIdentityFragment::StaticStruct() };
			const FMassArchetypeHandle Archetype = EntityManager->CreateArchetype(MakeArrayView(Fragments, 1));
			const FMassEntityHandle Entity = EntityManager->CreateEntity(Archetype);
			Subsystem->RegisterTrackedEntityForTest(UUID, Entity);

			if (FCrowdyRemoteIdentityFragment* Identity =
				EntityManager->GetFragmentDataPtr<FCrowdyRemoteIdentityFragment>(Entity))
			{
				Identity->UUID = UUID;
				Identity->OwnerID = OwnerID;
				Identity->Role = ECrowdyRole::HostOwned;
			}

			return Entity;
		}

		// An identity arriving from somewhere other than the avatar layer: what an authority registering the
		// entity would have put in the registry.
		void ArriveRecord(const FGuid& UUID, ECrowdyRole Role, const FGuid& OwnerID, UObject* Participant, uint32 ClassID = 0)
		{
			FCrowdyEntityRecord Record;
			Record.NetID = UUID;
			Record.Role = Role;
			Record.OwnerID = OwnerID;
			Record.ClassID = ClassID;
			Record.Participant = Participant;
			Entities->RegisterEntity(Record);
		}
	};

}

// The record an avatar enrols under is keyed by the ENTITY'S id, which is the whole point: every avatar in
// a crowd is one class, so an id derived from the participant's class would be one id for the entire crowd
// and every attack would land on whichever entity happened to hold it. Mutating
// RegisterAvatarParticipant's `Record.NetID = UUID` to anything class-derived (or routing this through
// UCrowdyEntitySubsystem::RegisterParticipant) turns this red on the second entity.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAvatarParticipantCarriesEntityUUIDTest,
	"CrowdySDK.Mass.AvatarParticipantCarriesEntityUUID", CrowdyMassParticipantTestFlags)
bool FCrowdyMassAvatarParticipantCarriesEntityUUIDTest::RunTest(const FString& Parameters)
{
	FCrowdyMassParticipantTestFixture Fixture;

	const FGuid FirstUUID(0xC0, 1, 2, 3);
	const FGuid SecondUUID(0xC1, 1, 2, 3);
	const FGuid TheirOwner(0x0BADC0DE, 1, 2, 3);

	Fixture.MakeUnsettledEntity(FirstUUID, TheirOwner);
	Fixture.MakeUnsettledEntity(SecondUUID, TheirOwner);

	UCrowdyMassEntity* FirstAvatar = Fixture.Subsystem->GetOrCreateAvatar(FirstUUID, nullptr);
	UCrowdyMassEntity* SecondAvatar = Fixture.Subsystem->GetOrCreateAvatar(SecondUUID, nullptr);
	if (!TestNotNull(TEXT("the first entity minted an avatar"), FirstAvatar)
		|| !TestNotNull(TEXT("the second entity minted an avatar"), SecondAvatar))
	{
		return false;
	}

	TestEqual(TEXT("both avatars are the same class, which is what would collapse a class-derived id"),
		FirstAvatar->GetClass(), SecondAvatar->GetClass());

	const FCrowdyEntityRecord* FirstRecord = Fixture.Entities->FindRecord(FirstUUID);
	if (!TestNotNull(TEXT("minting an avatar enrolled the entity"), FirstRecord))
	{
		return false;
	}

	TestEqual(TEXT("the record is keyed by the entity's own id"), FirstRecord->NetID, FirstUUID);
	TestEqual(TEXT("the avatar is the record's participant"),
		FirstRecord->GetParticipant(), static_cast<UObject*>(FirstAvatar));
	TestEqual(TEXT("a crowd entity enrols read-only, so it is never authoritative for the entity"),
		static_cast<uint8>(FirstRecord->Role), static_cast<uint8>(ECrowdyRole::RemoteProxy));
	TestEqual(TEXT("the owner is the one the entity itself records"), FirstRecord->OwnerID, TheirOwner);

	// The claim that a class-derived id would break: two entities, one avatar class, two distinct ids, each
	// resolving back to its own entity.
	TestEqual(TEXT("the first avatar resolves to the first entity's id"),
		Fixture.Entities->FindEntityID(FirstAvatar), FirstUUID);
	TestEqual(TEXT("the second avatar resolves to the second entity's id"),
		Fixture.Entities->FindEntityID(SecondAvatar), SecondUUID);

	return true;
}

// A player who dies and respawns comes back under the same id, and the tracker hands the id to a new
// entity while the previous avatar is still cached. The registry refuses a second live participant on one
// id, so the stale avatar has to let go before the replacement enrols. Mutating away the
// UnregisterAvatarParticipant call in GetOrCreateAvatar's id-reuse branch turns this red: the record keeps
// naming the avatar of the entity that left, and the respawned player can never be hit again.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAvatarParticipantRespawnRebindsTest,
	"CrowdySDK.Mass.AvatarParticipantRespawnRebinds", CrowdyMassParticipantTestFlags)
bool FCrowdyMassAvatarParticipantRespawnRebindsTest::RunTest(const FString& Parameters)
{
	FCrowdyMassParticipantTestFixture Fixture;

	const FGuid UUID(0xC2, 1, 2, 3);
	const FMassEntityHandle FirstLife = Fixture.MakeUnsettledEntity(UUID);

	UCrowdyMassEntity* FirstAvatar = Fixture.Subsystem->GetOrCreateAvatar(UUID, nullptr);
	if (!TestNotNull(TEXT("the first life minted an avatar"), FirstAvatar))
	{
		return false;
	}
	TestEqual(TEXT("the first life is enrolled"), Fixture.Entities->FindEntityID(FirstAvatar), UUID);

	// The entity goes, and the tracker hands the same id to a fresh one. The avatar cache still holds the
	// object built for the entity that left, pointing at a handle that no longer resolves.
	Fixture.EntityManager->DestroyEntity(FirstLife);
	Fixture.MakeUnsettledEntity(UUID);

	UCrowdyMassEntity* SecondAvatar = Fixture.Subsystem->GetOrCreateAvatar(UUID, nullptr);
	if (!TestNotNull(TEXT("the second life minted an avatar"), SecondAvatar))
	{
		return false;
	}
	TestNotEqual(TEXT("the second life got a fresh avatar, not the one the first life left"),
		static_cast<UObject*>(SecondAvatar), static_cast<UObject*>(FirstAvatar));

	const FCrowdyEntityRecord* Record = Fixture.Entities->FindRecord(UUID);
	if (!TestNotNull(TEXT("the respawned entity is enrolled"), Record))
	{
		return false;
	}
	TestEqual(TEXT("the record names the avatar of the life that is alive now"),
		Record->GetParticipant(), static_cast<UObject*>(SecondAvatar));
	TestFalse(TEXT("the avatar of the life that ended answers for nothing"),
		Fixture.Entities->FindEntityID(FirstAvatar).IsValid());

	return true;
}

// The other ordering of the same respawn: the replacement claims the id first and the previous life's
// teardown arrives afterwards. Teardown compares identity rather than removing by id, so a late one is a
// no-op. Mutating UnregisterAvatarParticipant's participant comparison away (removing by id alone) turns
// this red: the teardown of a life that already ended unbinds the one that replaced it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAvatarParticipantLateTeardownKeepsNewBindingTest,
	"CrowdySDK.Mass.AvatarParticipantLateTeardownKeepsNewBinding", CrowdyMassParticipantTestFlags)
bool FCrowdyMassAvatarParticipantLateTeardownKeepsNewBindingTest::RunTest(const FString& Parameters)
{
	FCrowdyMassParticipantTestFixture Fixture;

	const FGuid UUID(0xC3, 1, 2, 3);
	const FGuid TheirOwner(0x0BADC0DE, 1, 2, 3);
	Fixture.MakeUnsettledEntity(UUID, TheirOwner);

	UCrowdyMassEntity* Avatar = Fixture.Subsystem->GetOrCreateAvatar(UUID, nullptr);
	if (!TestNotNull(TEXT("the entity minted an avatar"), Avatar))
	{
		return false;
	}

	// The replacement claims the id: the returning pawn tears the avatar's record down and registers its
	// own under the same id, all before the avatar's own teardown gets a chance to run.
	UObject* ReturningPawn = NewObject<UCrowdyMassOtherParticipantTestTarget>(GetTransientPackage());
	Fixture.Entities->UnregisterEntity(UUID);
	Fixture.ArriveRecord(UUID, ECrowdyRole::Owner, TheirOwner, ReturningPawn);

	// And now the late teardown for the entity the avatar stood for.
	Fixture.Subsystem->DestroyRemoteEntity(UUID);

	const FCrowdyEntityRecord* Record = Fixture.Entities->FindRecord(UUID);
	if (!TestNotNull(TEXT("the replacement's record survived the late teardown"), Record))
	{
		return false;
	}
	TestEqual(TEXT("and still names the replacement, not the avatar that has gone"),
		Record->GetParticipant(), ReturningPawn);

	return true;
}

// The registry holds a participant weakly, so a record left behind by a reclaim names an object nothing
// can resolve: the entity looks enrolled while nothing can reach it. Mutating away the
// UnregisterAvatarParticipant call in EvictOldestAvatar turns this red.
//
// The capacity is forced down here. In a live world it follows the tracked-entity count and the cache
// cannot reach it (see the test below), but the reclaim path still runs for every path that drops an
// avatar without the entity going with it, and it has to leave the registry clean when it does.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAvatarParticipantEvictionClearsRecordTest,
	"CrowdySDK.Mass.AvatarParticipantEvictionClearsRecord", CrowdyMassParticipantTestFlags)
bool FCrowdyMassAvatarParticipantEvictionClearsRecordTest::RunTest(const FString& Parameters)
{
	FCrowdyMassParticipantTestFixture Fixture;

	constexpr int32 ForcedCapacity = 2;
	Fixture.Subsystem->SetAvatarCacheCapacityForTest(ForcedCapacity);

	// One more entity than the cache holds, minted oldest first, so the first is the least recently touched
	// when the last one forces a reclaim.
	TArray<FGuid> UUIDs;
	UUIDs.Reserve(ForcedCapacity + 1);
	for (int32 Index = 0; Index <= ForcedCapacity; ++Index)
	{
		const FGuid UUID(0xD0, 1, 2, static_cast<uint32>(Index) + 1);
		UUIDs.Add(UUID);
		Fixture.MakeTrackedEntity(UUID);
	}

	for (int32 Index = 0; Index < ForcedCapacity; ++Index)
	{
		if (!Fixture.Subsystem->GetOrCreateAvatar(UUIDs[Index], nullptr))
		{
			AddError(FString::Printf(TEXT("entity %d failed to mint an avatar"), Index));
			return false;
		}
	}

	TestNotNull(TEXT("the oldest entity is enrolled before the cache overflows"),
		Fixture.Entities->FindRecord(UUIDs[0]));

	// One past the capacity: minting reclaims the least recently touched avatar.
	UCrowdyMassEntity* Overflowing = Fixture.Subsystem->GetOrCreateAvatar(UUIDs[ForcedCapacity], nullptr);
	if (!TestNotNull(TEXT("the entity past the capacity still minted an avatar"), Overflowing))
	{
		return false;
	}

	TestEqual(TEXT("the oldest avatar was reclaimed"),
		static_cast<uint8>(Fixture.Subsystem->GetAvatarState(UUIDs[0])),
		static_cast<uint8>(ECrowdyMassAvatarState::Evicted));
	TestNull(TEXT("and its record went with it rather than naming a dropped object"),
		Fixture.Entities->FindRecord(UUIDs[0]));
	TestNotNull(TEXT("the entity that caused the reclaim is enrolled"),
		Fixture.Entities->FindRecord(UUIDs[ForcedCapacity]));

	// The reclaimed entity is still very much alive, so asking again enrols it again.
	TestNotNull(TEXT("asking again re-mints and re-enrols"),
		Fixture.Subsystem->GetOrCreateAvatar(UUIDs[0], nullptr));
	TestNotNull(TEXT("the re-minted avatar carries a record again"),
		Fixture.Entities->FindRecord(UUIDs[0]));

	return true;
}

/**
 * The reason the capacity is not a fixed number below the crowd size.
 *
 * Reclaiming an avatar is not a cache miss that costs a rebuild: it drops the entity's registry record, so
 * the entity is not enrolled until something asks for the avatar again. A cap of a few hundred against a
 * crowd of thousands spends that on exactly the entities being touched most, which are the ones under fire.
 *
 * Mutating GetAvatarCacheCapacity to ignore the tracked-entity count (returning CrowdyMassMinCachedAvatars
 * flat) turns this red on the last entity: the first one is reclaimed and loses its record.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAvatarCacheCapacityFollowsTrackerTest,
	"CrowdySDK.Mass.AvatarCacheCapacityFollowsTracker", CrowdyMassParticipantTestFlags)
bool FCrowdyMassAvatarCacheCapacityFollowsTrackerTest::RunTest(const FString& Parameters)
{
	FCrowdyMassParticipantTestFixture Fixture;

	// One more entity than the floor, so the tracked-entity count is what decides the capacity.
	constexpr int32 EntityCount = CrowdyMassMinCachedAvatars + 1;

	TArray<FGuid> UUIDs;
	UUIDs.Reserve(EntityCount);
	for (int32 Index = 0; Index < EntityCount; ++Index)
	{
		const FGuid UUID(0xD1, 1, 2, static_cast<uint32>(Index) + 1);
		UUIDs.Add(UUID);
		Fixture.MakeTrackedEntity(UUID);
	}

	TestEqual(TEXT("the capacity is the tracked-entity count once the crowd is past the floor"),
		Fixture.Subsystem->GetAvatarCacheCapacity(), EntityCount);

	for (int32 Index = 0; Index < EntityCount; ++Index)
	{
		if (!Fixture.Subsystem->GetOrCreateAvatar(UUIDs[Index], nullptr))
		{
			AddError(FString::Printf(TEXT("entity %d failed to mint an avatar"), Index));
			return false;
		}
	}

	// The entity minted first is the one a fixed cap would have thrown away.
	TestEqual(TEXT("the first entity's avatar is still present after the whole crowd has one"),
		static_cast<uint8>(Fixture.Subsystem->GetAvatarState(UUIDs[0])),
		static_cast<uint8>(ECrowdyMassAvatarState::Present));
	TestNotNull(TEXT("so it kept its registry record"),
		Fixture.Entities->FindRecord(UUIDs[0]));
	TestNotNull(TEXT("and so did the entity that would have caused the eviction"),
		Fixture.Entities->FindRecord(UUIDs[EntityCount - 1]));

	return true;
}

/**
 * An avatar adopts who an existing record says the entity is, but never its ROLE.
 *
 * Role decides whether the participant is authoritative for the entity. Adopting HostOwned would undo the
 * RemoteProxy the avatar is registered with on purpose; on a host client it would also make the state
 * replicator treat the avatar as a CrowdyState sender under the crowd entity's own id.
 *
 * Two independent mutations turn this red: restoring `Record.Role = Existing->Role` (the role reads
 * HostOwned) and dropping either of the OwnerID/ClassID adoptions (the identity is lost).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAvatarKeepsRemoteProxyRoleTest,
	"CrowdySDK.Mass.AvatarKeepsRemoteProxyRole", CrowdyMassParticipantTestFlags)
bool FCrowdyMassAvatarKeepsRemoteProxyRoleTest::RunTest(const FString& Parameters)
{
	FCrowdyMassParticipantTestFixture Fixture;

	const FGuid UUID(0xD4, 1, 2, 3);
	const FGuid WorldOwner(0x0BADC0DE, 1, 2, 3);
	constexpr uint32 ArrivedClassID = 0xABCDEF01;

	Fixture.MakeUnsettledEntity(UUID, WorldOwner);

	// A host-owned record whose participant has gone: the slot is free, so the avatar takes it and adopts
	// what the record knows.
	Fixture.ArriveRecord(UUID, ECrowdyRole::HostOwned, WorldOwner, nullptr, ArrivedClassID);

	UCrowdyMassEntity* Avatar = Fixture.Subsystem->GetOrCreateAvatar(UUID, nullptr);
	if (!TestNotNull(TEXT("the entity minted an avatar"), Avatar))
	{
		return false;
	}

	const FCrowdyEntityRecord* Record = Fixture.Entities->FindRecord(UUID);
	if (!TestNotNull(TEXT("the avatar took the free participant slot"), Record))
	{
		return false;
	}

	TestEqual(TEXT("the avatar holds the slot"), Record->GetParticipant(), static_cast<UObject*>(Avatar));
	TestEqual(TEXT("the role stays read-only, so the avatar is never authoritative for the entity"),
		static_cast<uint8>(Record->Role), static_cast<uint8>(ECrowdyRole::RemoteProxy));
	TestEqual(TEXT("but who the entity is is adopted: the owner the record already named"),
		Record->OwnerID, WorldOwner);
	TestEqual(TEXT("and the class the record already named"),
		static_cast<int64>(Record->ClassID), static_cast<int64>(ArrivedClassID));

	return true;
}

/**
 * A crowd whose config omits the Crowdy Mass Trace Collision trait is silently untraceable: the fragment
 * has one producer, nothing adds it when a remote entity spawns, and every refusal used to be a bare
 * false that reads exactly like a mis-aimed trace.
 *
 * The warning is said once because a whole crowd built from one config is missing the trait together, and
 * every refusal is still counted so the size of the problem is visible. Mutating away the `bWarned` guard
 * makes the message occur three times instead of once; mutating away the counter's increment makes the
 * count read zero.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassMissingTraceCollisionFragmentReportedOnceTest,
	"CrowdySDK.Mass.MissingTraceCollisionFragmentReportedOnce", CrowdyMassParticipantTestFlags)
bool FCrowdyMassMissingTraceCollisionFragmentReportedOnceTest::RunTest(const FString& Parameters)
{
	FCrowdyMassParticipantTestFixture Fixture;

	AddExpectedMessagePlain(TEXT("Crowdy Mass Trace Collision trait"),
		ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);

	TestEqual(TEXT("nothing has been refused for a missing fragment yet"),
		Fixture.Subsystem->GetMissingTraceCollisionFragmentCount(), 0);

	Fixture.Subsystem->ReportMissingTraceCollisionFragment();
	Fixture.Subsystem->ReportMissingTraceCollisionFragment();
	Fixture.Subsystem->ReportMissingTraceCollisionFragment();

	TestEqual(TEXT("every refusal is counted even though the warning is said once"),
		Fixture.Subsystem->GetMissingTraceCollisionFragmentCount(), 3);

	return true;
}

// The registry refuses a second live participant on one id, and it is right to: the id addresses a damage
// victim. So the avatar must not try. Mutating away either guard in RegisterAvatarParticipant (the live
// participant check, or the locally-owned check) turns one of these red, and in the second case the
// returning pawn's own re-registration would be the thing that collides.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAvatarParticipantYieldsToOwnerTest,
	"CrowdySDK.Mass.AvatarParticipantYieldsToOwner", CrowdyMassParticipantTestFlags)
bool FCrowdyMassAvatarParticipantYieldsToOwnerTest::RunTest(const FString& Parameters)
{
	FCrowdyMassParticipantTestFixture Fixture;

	const FGuid TakenUUID(0xF0, 1, 2, 3);
	const FGuid BetweenLivesUUID(0xF1, 1, 2, 3);
	const FGuid LocalPlayer(0x10CA1, 1, 2, 3);
	const FGuid TheirOwner(0x0BADC0DE, 1, 2, 3);

	Fixture.MakeUnsettledEntity(TakenUUID);
	Fixture.MakeUnsettledEntity(BetweenLivesUUID);

	// Something live already answers for this id.
	UObject* Incumbent = NewObject<UCrowdyMassOtherParticipantTestTarget>(GetTransientPackage());
	Fixture.ArriveRecord(TakenUUID, ECrowdyRole::RemoteProxy, TheirOwner, Incumbent);

	UCrowdyMassEntity* TakenAvatar = Fixture.Subsystem->GetOrCreateAvatar(TakenUUID, nullptr);
	TestNotNull(TEXT("the avatar is still minted; only its enrolment is refused"), TakenAvatar);
	if (const FCrowdyEntityRecord* Record = Fixture.Entities->FindRecord(TakenUUID))
	{
		TestEqual(TEXT("the incumbent participant keeps the id"), Record->GetParticipant(), Incumbent);
	}
	TestFalse(TEXT("and the avatar answers for nothing"),
		TakenAvatar && Fixture.Entities->FindEntityID(TakenAvatar).IsValid());

	// A locally-owned record whose participant has died is a pawn between lives, not a free id: its own
	// registration is coming back, and it would collide with an avatar sitting in the slot.
	Fixture.ArriveRecord(BetweenLivesUUID, ECrowdyRole::Owner, LocalPlayer, nullptr);

	UCrowdyMassEntity* BetweenLivesAvatar = Fixture.Subsystem->GetOrCreateAvatar(BetweenLivesUUID, nullptr);
	TestNotNull(TEXT("that avatar is minted too"), BetweenLivesAvatar);
	if (const FCrowdyEntityRecord* Record = Fixture.Entities->FindRecord(BetweenLivesUUID))
	{
		TestEqual(TEXT("the locally-owned record keeps its role rather than becoming a remote proxy"),
			static_cast<uint8>(Record->Role), static_cast<uint8>(ECrowdyRole::Owner));
		TestNull(TEXT("and its participant slot is left free for the pawn that returns"),
			Record->GetParticipant());
	}

	return true;
}

// A crowd entity stands for another client. One that names this client as its owner would enrol as a
// record this client is authoritative for, and the avatar would claim what the local player's own
// participant is supposed to own. Mutating away the local-player check in
// RegisterAvatarParticipant turns this red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAvatarParticipantRefusesLocalOwnerTest,
	"CrowdySDK.Mass.AvatarParticipantRefusesLocalOwner", CrowdyMassParticipantTestFlags)
bool FCrowdyMassAvatarParticipantRefusesLocalOwnerTest::RunTest(const FString& Parameters)
{
	FCrowdyMassParticipantTestFixture Fixture;

	const FGuid LocalPlayer(0x10CA1, 1, 2, 3);
	const FGuid MineUUID(0xF2, 1, 2, 3);
	const FGuid TheirsUUID(0xF3, 1, 2, 3);
	const FGuid TheirOwner(0x0BADC0DE, 1, 2, 3);

	Fixture.Entities->SetLocalPlayerID(LocalPlayer);
	Fixture.MakeUnsettledEntity(MineUUID, LocalPlayer);
	Fixture.MakeUnsettledEntity(TheirsUUID, TheirOwner);

	TestNotNull(TEXT("an entity claiming the local player still mints an avatar"),
		Fixture.Subsystem->GetOrCreateAvatar(MineUUID, nullptr));
	TestNull(TEXT("but it is not enrolled, so it can never claim what the local player owns"),
		Fixture.Entities->FindRecord(MineUUID));

	// The control: an entity owned by anybody else enrols normally, so the refusal above is about who the
	// owner is and not about the enrolment path being broken.
	TestNotNull(TEXT("an entity owned by another client mints an avatar"),
		Fixture.Subsystem->GetOrCreateAvatar(TheirsUUID, nullptr));
	TestNotNull(TEXT("and enrols"), Fixture.Entities->FindRecord(TheirsUUID));

	return true;
}

/**
 * A row drawn again inside the reclaim grace window gets the avatar it had, and the registry record with it.
 *
 * A crowd row is destroyed as soon as its view state goes quiet, and a peer crossing a chunk boundary produces
 * that constantly. Without the window each redraw drops the registry record and mints a new avatar.
 *
 * Mutating ReleaseAvatarState to unregister the participant immediately (the behaviour the window replaced)
 * turns the "record survived" assertion red; the control shows the same sequence with the window shut does
 * drop the record, so the assertion is about the window and not about the teardown never running.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassRedrawInsideGraceKeepsAvatarTest,
	"CrowdySDK.Mass.RedrawInsideGraceKeepsAvatar", CrowdyMassParticipantTestFlags)
bool FCrowdyMassRedrawInsideGraceKeepsAvatarTest::RunTest(const FString& Parameters)
{
	FCrowdyMassParticipantTestFixture Fixture;

	const FGuid TheirOwner(0x0BADC0DE, 1, 2, 3);
	const FGuid UUID(0xB2, 1, 2, 3);
	Fixture.MakeUnsettledEntity(UUID, TheirOwner);

	UCrowdyMassEntity* First = Fixture.Subsystem->GetOrCreateAvatar(UUID, nullptr);
	if (!TestNotNull(TEXT("the row minted an avatar"), First))
	{
		return false;
	}
	TestNotNull(TEXT("and enrolled under its id"), Fixture.Entities->FindRecord(UUID));

	// The row goes quiet and is destroyed, then is drawn again a moment later.
	Fixture.Subsystem->DestroyRemoteEntity(UUID);
	TestEqual(TEXT("the avatar is held rather than released"),
		Fixture.Subsystem->GetAvatarReclaimGraceCountForTest(), 1);
	TestNotNull(TEXT("and its record stays while it is held"), Fixture.Entities->FindRecord(UUID));

	Fixture.MakeUnsettledEntity(UUID, TheirOwner);
	UCrowdyMassEntity* Second = Fixture.Subsystem->GetOrCreateAvatar(UUID, nullptr);

	TestEqual(TEXT("the redraw got the avatar it had, not a fresh one"),
		static_cast<UObject*>(Second), static_cast<UObject*>(First));
	TestEqual(TEXT("and nothing is left waiting in the window"),
		Fixture.Subsystem->GetAvatarReclaimGraceCountForTest(), 0);

	const FCrowdyEntityRecord* Record = Fixture.Entities->FindRecord(UUID);
	if (!TestNotNull(TEXT("the registry record survived the redraw"), Record))
	{
		return false;
	}
	TestEqual(TEXT("and still names the same avatar"), Record->GetParticipant(), static_cast<UObject*>(First));

	// The control: the same sequence with the window shut drops the record, so the assertions above are about
	// the window rather than about a teardown that never runs in this fixture.
	Fixture.Subsystem->SetAvatarReclaimGraceSecondsForTest(0.0f);
	Fixture.Subsystem->DestroyRemoteEntity(UUID);
	TestNull(TEXT("with no window the record goes with the row"), Fixture.Entities->FindRecord(UUID));
	TestEqual(TEXT("and nothing is held"), Fixture.Subsystem->GetAvatarReclaimGraceCountForTest(), 0);

	return true;
}

/**
 * The reclaim window closes on its own, and the next sweep is due when the soonest entry is.
 *
 * Every caller-driven route into the sweep needs some OTHER row to leave the crowd, so the last row to go would
 * keep its record for the rest of the session. The armed timer's entry point is the forced sweep below; the
 * arming itself needs a world and is exercised by the editor and game builds rather than here.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassGraceWindowClosesOnItsOwnTest,
	"CrowdySDK.Mass.GraceWindowClosesOnItsOwn", CrowdyMassParticipantTestFlags)
bool FCrowdyMassGraceWindowClosesOnItsOwnTest::RunTest(const FString& Parameters)
{
	FCrowdyMassParticipantTestFixture Fixture;

	const FGuid TheirOwner(0x0BADC0DE, 1, 2, 3);
	const FGuid Brief(0xB9, 1, 2, 1);
	const FGuid Long(0xB9, 1, 2, 2);
	const FGuid Later(0xB9, 1, 2, 3);
	const FGuid Last(0xB9, 1, 2, 4);

	for (const FGuid& UUID : { Brief, Long, Later, Last })
	{
		Fixture.MakeUnsettledEntity(UUID, TheirOwner);
		Fixture.Subsystem->GetOrCreateAvatar(UUID, nullptr);
	}

	Fixture.Subsystem->SetAvatarReclaimGraceSecondsForTest(0.005f);
	Fixture.Subsystem->DestroyRemoteEntity(Brief);

	Fixture.Subsystem->SetAvatarReclaimGraceSecondsForTest(60.0f);
	Fixture.Subsystem->DestroyRemoteEntity(Long);

	// Back to a brief window, so a next-due computed from the window rather than from a deadline would land
	// milliseconds away instead of a minute away.
	Fixture.Subsystem->SetAvatarReclaimGraceSecondsForTest(0.005f);
	FPlatformProcess::Sleep(0.02f);

	Fixture.Subsystem->SweepExpiredAvatarGraceForTest();
	TestEqual(TEXT("the expired entry is gone and the live one is not"),
		Fixture.Subsystem->GetAvatarReclaimGraceCountForTest(), 1);

	const double NextSweep = Fixture.Subsystem->GetNextAvatarGraceSweepSecondsForTest();
	TestTrue(TEXT("and the next sweep is due at the surviving entry's own deadline, not a window from now"),
		NextSweep > FPlatformTime::Seconds() + 30.0);

	// The scenario itself: a row leaves and nothing else does. The caller-driven sweep is refused because the
	// next one is not due for a minute, so nothing would ever expire this entry.
	Fixture.Subsystem->DestroyRemoteEntity(Later);
	FPlatformProcess::Sleep(0.02f);
	Fixture.Subsystem->DestroyRemoteEntity(Last);
	TestEqual(TEXT("the caller-driven sweep expired nothing, which is the state the timer exists for"),
		Fixture.Subsystem->GetAvatarReclaimGraceCountForTest(), 3);

	// What the armed timer does, once both brief windows have actually run out.
	FPlatformProcess::Sleep(0.02f);
	Fixture.Subsystem->SweepExpiredAvatarGraceForTest();
	TestEqual(TEXT("the timer's sweep closes every window that has run out"),
		Fixture.Subsystem->GetAvatarReclaimGraceCountForTest(), 1);
	TestNotNull(TEXT("while the entry that has not run out keeps its record"),
		Fixture.Entities->FindRecord(Long));
	TestNull(TEXT("and the ones that did lose theirs"),
		Fixture.Entities->FindRecord(Later));

	return true;
}

/**
 * Changing the avatar class releases every avatar already minted, held ones included, and an entity that
 * still exists is minted again on the new class.
 *
 * Mutating away the `EvictedAvatarUUIDs.Add` loop in ReleaseAllAvatars turns the Evicted assertion red;
 * mutating away its UnregisterAllAvatarParticipants call leaves the minted entity's record behind; mutating
 * away the grace-window loop at its top leaves the held entity's record behind.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAvatarClassChangeReleasesAndRemintsTest,
	"CrowdySDK.Mass.AvatarClassChangeReleasesAndRemints", CrowdyMassParticipantTestFlags)
bool FCrowdyMassAvatarClassChangeReleasesAndRemintsTest::RunTest(const FString& Parameters)
{
	FCrowdyMassParticipantTestFixture Fixture;

	const FGuid TheirOwner(0x0BADC0DE, 1, 2, 3);
	const FGuid Minted(0xB3, 1, 2, 1);
	const FGuid Held(0xB3, 1, 2, 2);
	Fixture.MakeUnsettledEntity(Minted, TheirOwner);
	Fixture.MakeUnsettledEntity(Held, TheirOwner);

	UCrowdyMassEntity* Before = Fixture.Subsystem->GetOrCreateAvatar(Minted, nullptr);
	if (!TestNotNull(TEXT("the entity minted an avatar"), Before))
	{
		return false;
	}
	TestNotNull(TEXT("and enrolled under its id"), Fixture.Entities->FindRecord(Minted));

	// A second entity whose row has gone, so its avatar is waiting in the reclaim window with its record.
	if (!TestNotNull(TEXT("the second entity minted an avatar"), Fixture.Subsystem->GetOrCreateAvatar(Held, nullptr)))
	{
		return false;
	}
	Fixture.Subsystem->DestroyRemoteEntity(Held);
	TestEqual(TEXT("the second entity's avatar is held in the window"),
		Fixture.Subsystem->GetAvatarReclaimGraceCountForTest(), 1);
	TestNotNull(TEXT("with its record"), Fixture.Entities->FindRecord(Held));

	Fixture.Subsystem->SetAvatarClass(UCrowdyMassOtherEntityTestReceiver::StaticClass());

	TestEqual(TEXT("the entity that is still alive reports its avatar released"),
		static_cast<uint8>(Fixture.Subsystem->GetAvatarState(Minted)),
		static_cast<uint8>(ECrowdyMassAvatarState::Evicted));
	TestNull(TEXT("and its record went with the avatar"), Fixture.Entities->FindRecord(Minted));
	TestEqual(TEXT("nothing is left waiting in the window"),
		Fixture.Subsystem->GetAvatarReclaimGraceCountForTest(), 0);
	TestNull(TEXT("and the held avatar's record went too"), Fixture.Entities->FindRecord(Held));

	UCrowdyMassEntity* After = Fixture.Subsystem->GetOrCreateAvatar(Minted, nullptr);
	if (!TestNotNull(TEXT("asking again mints an avatar"), After))
	{
		return false;
	}
	TestNotEqual(TEXT("a new one, not the avatar of the old class"),
		static_cast<UObject*>(After), static_cast<UObject*>(Before));
	TestEqual(TEXT("of the class that was just set"),
		After->GetClass(), UCrowdyMassOtherEntityTestReceiver::StaticClass());
	TestEqual(TEXT("and the entity reports it present again"),
		static_cast<uint8>(Fixture.Subsystem->GetAvatarState(Minted)),
		static_cast<uint8>(ECrowdyMassAvatarState::Present));

	const FCrowdyEntityRecord* Record = Fixture.Entities->FindRecord(Minted);
	if (!TestNotNull(TEXT("and its record is back"), Record))
	{
		return false;
	}
	TestEqual(TEXT("naming the new avatar"), Record->GetParticipant(), static_cast<UObject*>(After));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
