#include "Utils/HelperFunctions.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Data/CrowdyEntityTypes.h"
#include "Messages/GameObjects/FCrowdyEntitySpawnEvent.h"
#include "Misc/AutomationTest.h"
#include "Replication/State/CrowdyStateTestTarget.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "UObject/Package.h"

// GetDeterministicID mints the NetID that addresses damage, so a collision guard sits at the point that id
// enters the registry (UCrowdyEntitySubsystem::RegisterEntity), since the hash itself cannot be widened without
// orphaning every server-persisted Game Model container keyed off it (see the header comment on the function).
// These tests cover the derivation's one load-bearing property (determinism), the guard refusing a forced
// collision between two live participants, and the legitimate re-registration shapes the guard must not break.
namespace
{
	constexpr EAutomationTestFlags CrowdyHelperFunctionsTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	UCrowdyEntitySubsystem* MakeEntitySubsystem()
	{
		return NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
	}

	// A participant that is not an actor, standing for the crowd avatars that hold a record for an entity with
	// no local body.
	UObject* MakeRegistryTestParticipant()
	{
		return NewObject<UCrowdyStateTestTarget>(GetTransientPackage(), NAME_None, RF_Transient);
	}

	// An actor participant. Native and worldless on purpose: nothing here spawns, it only has to be an AActor so
	// the record resolves through GetActor().
	AActor* MakeRegistryTestActor()
	{
		return NewObject<ACrowdyStateSendTestActor>(GetTransientPackage(), NAME_None, RF_Transient);
	}

	void RegisterRegistryTestRecord(UCrowdyEntitySubsystem* Entities, const FGuid& NetID, const FGuid& OwnerID,
		const ECrowdyRole Role, UObject* Participant, const uint32 ClassID = 0)
	{
		FCrowdyEntityRecord Record;
		Record.NetID = NetID;
		Record.OwnerID = OwnerID;
		Record.Role = Role;
		Record.ClassID = ClassID;
		Record.Participant = Participant;
		Entities->RegisterEntity(Record);
	}

	FCrowdyEntitySpawnEvent MakeRegistryTestSpawnEvent(const FGuid& EntityID, const FGuid& OwnerID, const uint32 ClassID)
	{
		FCrowdyEntitySpawnEvent Event;
		Event.EntityID = EntityID;
		Event.OwnerID = OwnerID;
		Event.ClassID = ClassID;
		return Event;
	}
}

// The whole system (client and server independently deriving the same NetID for the same seed) rests on
// GetDeterministicID being a pure function of its input: the same seed must always produce the same GUID.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeterministicIDStableTest,
	"CrowdySDK.Utils.DeterministicIDStable", CrowdyHelperFunctionsTestFlags)
bool FCrowdyDeterministicIDStableTest::RunTest(const FString& Parameters)
{
	constexpr int64 Seeds[] = { 0, 1, -1, 424242, MIN_int64, MAX_int64 };

	for (const int64 Seed : Seeds)
	{
		const FGuid First = UHelperFunctions::GetDeterministicID(Seed);
		const FGuid Second = UHelperFunctions::GetDeterministicID(Seed);
		TestEqual(TEXT("the same seed derives the same GUID on every call"), First, Second);
	}

	return true;
}

// The collision guard: two DIFFERENT, still-alive participants that arrive under the SAME NetID (the birthday-bound
// hash collision this task exists to defend against, forced here directly rather than searched for) must not both
// bind. The second is refused with a loud error naming both, and the first keeps the id.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEntityRegisterCollisionRefusedTest,
	"CrowdySDK.Entity.RegisterCollisionRefused", CrowdyHelperFunctionsTestFlags)
bool FCrowdyEntityRegisterCollisionRefusedTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem();
	if (!TestNotNull(TEXT("entities"), Entities))
	{
		return false;
	}

	const FGuid CollidingNetID = FGuid::NewGuid();
	UObject* ParticipantA = NewObject<UCrowdyStateTestTarget>(GetTransientPackage(), NAME_None, RF_Transient);
	UObject* ParticipantB = NewObject<UCrowdyStateTestTarget>(GetTransientPackage(), NAME_None, RF_Transient);

	FCrowdyEntityRecord RecordA;
	RecordA.NetID = CollidingNetID;
	RecordA.OwnerID = FGuid::NewGuid();
	RecordA.Role = ECrowdyRole::Owner;
	RecordA.Participant = ParticipantA;
	Entities->RegisterEntity(RecordA);
	TestTrue(TEXT("the first registration binds the id"), Entities->FindParticipant(CollidingNetID) == ParticipantA);

	FCrowdyEntityRecord RecordB;
	RecordB.NetID = CollidingNetID;
	RecordB.OwnerID = FGuid::NewGuid();
	RecordB.Role = ECrowdyRole::Owner;
	RecordB.Participant = ParticipantB;

	// The collision is a loud error; whitelist it so it does not fail the test run.
	AddExpectedError(TEXT("NetID collision"), EAutomationExpectedErrorFlags::Contains, 1);
	Entities->RegisterEntity(RecordB);

	TestTrue(TEXT("the id still resolves to the first participant"),
		Entities->FindParticipant(CollidingNetID) == ParticipantA);
	TestFalse(TEXT("the second participant was never bound"), Entities->FindEntityID(ParticipantB).IsValid());

	return true;
}

// The legitimate re-registration shapes the guard must NOT refuse: (1) the same object re-announcing itself with
// updated fields (a plain update, not a new entrant), and (2) a NEW participant taking over a NetID whose previous
// record was already torn down first - the shape every real caller uses for a respawn (EndPlay unregisters before
// the replacement actor's BeginPlay registers) and for the actor pool reclaiming a slot (it unregisters the old
// occupant before handing the id to the newly acquired actor). Both must succeed silently, with no error logged.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEntityLegitimateReRegistrationTest,
	"CrowdySDK.Entity.LegitimateReRegistrationSucceeds", CrowdyHelperFunctionsTestFlags)
bool FCrowdyEntityLegitimateReRegistrationTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem();
	if (!TestNotNull(TEXT("entities"), Entities))
	{
		return false;
	}

	const FGuid NetID = FGuid::NewGuid();
	const FGuid OriginalOwner = FGuid::NewGuid();
	UObject* Participant = NewObject<UCrowdyStateTestTarget>(GetTransientPackage(), NAME_None, RF_Transient);

	FCrowdyEntityRecord Record;
	Record.NetID = NetID;
	Record.OwnerID = OriginalOwner;
	Record.Role = ECrowdyRole::Owner;
	Record.Participant = Participant;
	Entities->RegisterEntity(Record);

	// Case 1: the same object re-registers (e.g. ownership metadata changed in place). Not a collision.
	const FGuid ReassignedOwner = FGuid::NewGuid();
	Record.OwnerID = ReassignedOwner;
	Entities->RegisterEntity(Record);
	TestTrue(TEXT("re-registering the same object still resolves to it"),
		Entities->FindParticipant(NetID) == Participant);
	if (const FCrowdyEntityRecord* Updated = Entities->FindRecord(NetID))
	{
		TestEqual(TEXT("the in-place re-registration applied the new owner"), Updated->OwnerID, ReassignedOwner);
	}

	// Case 2: respawn / pooled reclaim shape - the old record is torn down first (mirroring EndPlay's
	// UnregisterEntity and the actor pool's ReleaseActor-then-reassign path), THEN a different object registers
	// under the identical NetID. This must succeed exactly like a fresh registration.
	Entities->UnregisterEntity(NetID);
	TestFalse(TEXT("the id is free once unregistered"), Entities->FindEntity(NetID) != nullptr);

	UObject* ReplacementParticipant = NewObject<UCrowdyStateTestTarget>(GetTransientPackage(), NAME_None, RF_Transient);
	FCrowdyEntityRecord ReplacementRecord;
	ReplacementRecord.NetID = NetID;
	ReplacementRecord.OwnerID = OriginalOwner;
	ReplacementRecord.Role = ECrowdyRole::Owner;
	ReplacementRecord.Participant = ReplacementParticipant;
	Entities->RegisterEntity(ReplacementRecord);

	TestTrue(TEXT("the reclaimed id resolves to the new participant"),
		Entities->FindParticipant(NetID) == ReplacementParticipant);
	TestFalse(TEXT("the old participant no longer maps to the id"), Entities->FindEntityID(Participant).IsValid());

	return true;
}

// Replacing a record whose participant has already been collected must not leave that participant's mapping
// behind. The key cannot be looked up (nothing can reconstruct it from a dead weak pointer) and it cannot
// misroute (a TObjectKey carries the object's serial number, so a new object at the same address never matches
// it), so the only thing it does is accumulate: one entry per replaced-while-dead participant, for the life of
// the world. Removing the by-value fallback from RegisterEntity's replace path turns this red at two mappings.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEntityReplaceDropsDeadParticipantMappingTest,
	"CrowdySDK.Entity.ReplaceDropsDeadParticipantMapping", CrowdyHelperFunctionsTestFlags)
bool FCrowdyEntityReplaceDropsDeadParticipantMappingTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem();
	if (!TestNotNull(TEXT("entities"), Entities))
	{
		return false;
	}

	const FGuid NetID = FGuid::NewGuid();
	UObject* Doomed = MakeRegistryTestParticipant();
	RegisterRegistryTestRecord(Entities, NetID, FGuid::NewGuid(), ECrowdyRole::RemoteProxy, Doomed);
	TestEqual(TEXT("the first participant holds one mapping"), Entities->GetParticipantMappingCountForTest(), 1);

	// Garbage, not merely unreferenced: a weak pointer to a garbage object already reads as null, which is exactly
	// the state the record is in when the participant dies before its record is replaced.
	Doomed->MarkAsGarbage();
	TestNull(TEXT("the record's participant reads as dead"), Entities->FindParticipant(NetID));

	UObject* Replacement = MakeRegistryTestParticipant();
	RegisterRegistryTestRecord(Entities, NetID, FGuid::NewGuid(), ECrowdyRole::RemoteProxy, Replacement);

	TestTrue(TEXT("the id resolves to the replacement"), Entities->FindParticipant(NetID) == Replacement);
	TestEqual(TEXT("the dead participant's mapping went with it"),
		Entities->GetParticipantMappingCountForTest(), 1);

	return true;
}

// A refused registration leaves the caller's participant answering to no id at all, and until now the only trace
// of that was a log line on whichever machine hit it. TryRegisterEntity reports it, so a caller that binds state
// to an id can give up its claim instead of running on as though it holds one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEntityRegisterReportsRefusalTest,
	"CrowdySDK.Entity.RegisterReportsRefusal", CrowdyHelperFunctionsTestFlags)
bool FCrowdyEntityRegisterReportsRefusalTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem();
	if (!TestNotNull(TEXT("entities"), Entities))
	{
		return false;
	}

	FCrowdyEntityRecord NoID;
	NoID.Role = ECrowdyRole::Owner;
	NoID.Participant = MakeRegistryTestParticipant();
	TestTrue(TEXT("a record with no id is reported as refused"),
		Entities->TryRegisterEntity(NoID) == ECrowdyEntityRegistration::RefusedInvalidNetID);

	const FGuid NetID = FGuid::NewGuid();
	FCrowdyEntityRecord First;
	First.NetID = NetID;
	First.OwnerID = FGuid::NewGuid();
	First.Role = ECrowdyRole::RemoteProxy;
	First.Participant = MakeRegistryTestParticipant();
	TestTrue(TEXT("the first registration is reported as registered"),
		Entities->TryRegisterEntity(First) == ECrowdyEntityRegistration::Registered);

	// Re-announcing the same object is an update, not a second claimant.
	First.OwnerID = FGuid::NewGuid();
	TestTrue(TEXT("the same participant re-registering is reported as registered"),
		Entities->TryRegisterEntity(First) == ECrowdyEntityRegistration::Registered);

	FCrowdyEntityRecord Colliding;
	Colliding.NetID = NetID;
	Colliding.OwnerID = FGuid::NewGuid();
	Colliding.Role = ECrowdyRole::RemoteProxy;
	Colliding.Participant = MakeRegistryTestParticipant();

	AddExpectedErrorPlain(TEXT("NetID collision"), EAutomationExpectedErrorFlags::Contains, 1);
	const ECrowdyEntityRegistration Result = Entities->TryRegisterEntity(Colliding);

	TestTrue(TEXT("a second live participant on one id is reported as refused, not as registered"),
		Result == ECrowdyEntityRegistration::RefusedIdHeldByLiveParticipant);
	TestTrue(TEXT("the incumbent still holds the id"),
		Entities->FindParticipant(NetID) == First.GetParticipant());

	return true;
}

// A spawn event is authored by an ordinary peer, so what it may do to a record this client already holds is
// bounded. It fills in what is not known yet: the class of an entity registered from position updates before the
// event naming that class arrived, and an owner for a record that has none. That is the branch's real use.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEntityRemoteSpawnBackfillsUnsetMetadataTest,
	"CrowdySDK.Entity.RemoteSpawnBackfillsUnsetMetadata", CrowdyHelperFunctionsTestFlags)
bool FCrowdyEntityRemoteSpawnBackfillsUnsetMetadataTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem();
	if (!TestNotNull(TEXT("entities"), Entities))
	{
		return false;
	}
	Entities->SetLocalPlayerID(FGuid::NewGuid());

	const FGuid NetID = FGuid::NewGuid();
	const FGuid TheirOwner = FGuid::NewGuid();

	// The shape a pooled proxy is in when its spawn event lands: an actor is already active for the id, with
	// neither an owner nor a class recorded for it yet.
	RegisterRegistryTestRecord(Entities, NetID, FGuid(), ECrowdyRole::RemoteProxy, MakeRegistryTestActor());

	Entities->HandleRemoteSpawnForTest(MakeRegistryTestSpawnEvent(NetID, TheirOwner, 4242));

	const FCrowdyEntityRecord* Record = Entities->FindRecord(NetID);
	if (!TestNotNull(TEXT("the entity is still registered"), Record))
	{
		return false;
	}

	TestEqual(TEXT("an owner the record did not have is filled in"), Record->OwnerID, TheirOwner);
	TestEqual(TEXT("a class the record did not have is filled in"),
		static_cast<int64>(Record->ClassID), static_cast<int64>(4242));
	TestEqual(TEXT("the role is not something a spawn event sets"),
		static_cast<uint8>(Record->Role), static_cast<uint8>(ECrowdyRole::RemoteProxy));

	return true;
}

// The same branch must not re-authorize. A peer can put any owner id in a spawn event naming any entity, so an
// owner already settled here is never re-pointed, and the local player's id is never accepted from the wire at
// all: that answer decides whether this client creates and pins the server-side row bound to the entity's id, so
// taking it from a peer hands the victim's row to the wrong user and leaves the victim unable to bind its own.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEntityRemoteSpawnCannotRewriteOwnershipTest,
	"CrowdySDK.Entity.RemoteSpawnCannotRewriteOwnership", CrowdyHelperFunctionsTestFlags)
bool FCrowdyEntityRemoteSpawnCannotRewriteOwnershipTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem();
	if (!TestNotNull(TEXT("entities"), Entities))
	{
		return false;
	}

	const FGuid LocalPlayer = FGuid::NewGuid();
	const FGuid Victim = FGuid::NewGuid();
	const FGuid Attacker = FGuid::NewGuid();
	Entities->SetLocalPlayerID(LocalPlayer);

	// Another player's entity, as this client knows it.
	const FGuid TheirEntity = FGuid::NewGuid();
	RegisterRegistryTestRecord(Entities, TheirEntity, Victim, ECrowdyRole::RemoteProxy, MakeRegistryTestActor());

	// The forged event: it names another player's entity and claims this client owns it.
	Entities->HandleRemoteSpawnForTest(MakeRegistryTestSpawnEvent(TheirEntity, LocalPlayer, 1));

	const FCrowdyEntityRecord* Proxy = Entities->FindRecord(TheirEntity);
	if (!TestNotNull(TEXT("the entity is still registered"), Proxy))
	{
		return false;
	}
	TestEqual(TEXT("the owner this client had recorded is unchanged"), Proxy->OwnerID, Victim);
	TestFalse(TEXT("a peer's event cannot make another player's entity read as locally owned"),
		Entities->IsLocallyOwned(TheirEntity));

	// The same refusal for a third party's id: an owner already settled is not the event's to move.
	Entities->HandleRemoteSpawnForTest(MakeRegistryTestSpawnEvent(TheirEntity, Attacker, 1));
	const FCrowdyEntityRecord* AfterThirdParty = Entities->FindRecord(TheirEntity);
	if (!TestNotNull(TEXT("the entity survives a second forged event"), AfterThirdParty))
	{
		return false;
	}
	TestEqual(TEXT("the owner is not re-pointed at a third party either"), AfterThirdParty->OwnerID, Victim);

	// And in the other direction: an entity this client owns is not given away by a peer's event.
	const FGuid OwnEntity = FGuid::NewGuid();
	RegisterRegistryTestRecord(Entities, OwnEntity, LocalPlayer, ECrowdyRole::Owner, MakeRegistryTestActor());
	Entities->HandleRemoteSpawnForTest(MakeRegistryTestSpawnEvent(OwnEntity, Attacker, 1));

	const FCrowdyEntityRecord* Own = Entities->FindRecord(OwnEntity);
	if (!TestNotNull(TEXT("the locally owned entity is still registered"), Own))
	{
		return false;
	}
	TestEqual(TEXT("a peer's event cannot take an entity this client owns"), Own->OwnerID, LocalPlayer);
	TestTrue(TEXT("it is still locally owned"), Entities->IsLocallyOwned(OwnEntity));

	return true;
}

// IsLocallyOwned authorizes creating server-side state bound to the id, so it takes both halves of the answer.
// A record that names the local player while its role says this client only mirrors the entity contradicts
// itself, and the two readings are not equally safe: the owner id can be written by a decoded message, the role
// only by whatever registered the entity here. Dropping the role check turns this red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEntityProxyRecordIsNotLocallyOwnedTest,
	"CrowdySDK.Entity.ProxyRecordIsNotLocallyOwned", CrowdyHelperFunctionsTestFlags)
bool FCrowdyEntityProxyRecordIsNotLocallyOwnedTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem();
	if (!TestNotNull(TEXT("entities"), Entities))
	{
		return false;
	}

	const FGuid LocalPlayer = FGuid::NewGuid();
	Entities->SetLocalPlayerID(LocalPlayer);

	const FGuid ProxyID = FGuid::NewGuid();
	RegisterRegistryTestRecord(Entities, ProxyID, LocalPlayer, ECrowdyRole::RemoteProxy, MakeRegistryTestActor());
	TestFalse(TEXT("a proxy record naming the local player is not locally owned"),
		Entities->IsLocallyOwned(ProxyID));

	const FGuid OwnedID = FGuid::NewGuid();
	RegisterRegistryTestRecord(Entities, OwnedID, LocalPlayer, ECrowdyRole::Owner, MakeRegistryTestActor());
	TestTrue(TEXT("a record this client actually owns still reads as locally owned"),
		Entities->IsLocallyOwned(OwnedID));

	return true;
}

// A record existing for an id is not by itself a reason to drop the spawn. An actor already standing for the id
// is (spawning again would give the entity two bodies), but a non-actor stand-in - a crowd avatar enrolled from
// position updates - is not: the entity's body is exactly what the event is announcing. Both events here name a
// class this client cannot resolve, so the spawn stops at class resolution with a loud error; that error firing
// exactly once is the discriminator. Restoring the unconditional early return silences it (none), and dropping
// the actor check doubles it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEntityRemoteSpawnNotSwallowedByStandInTest,
	"CrowdySDK.Entity.RemoteSpawnNotSwallowedByStandIn", CrowdyHelperFunctionsTestFlags)
bool FCrowdyEntityRemoteSpawnNotSwallowedByStandInTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem();
	if (!TestNotNull(TEXT("entities"), Entities))
	{
		return false;
	}
	Entities->SetLocalPlayerID(FGuid::NewGuid());

	const FGuid TheirOwner = FGuid::NewGuid();
	const FGuid ActorHeld = FGuid::NewGuid();
	const FGuid StandInHeld = FGuid::NewGuid();

	RegisterRegistryTestRecord(Entities, ActorHeld, TheirOwner, ECrowdyRole::RemoteProxy, MakeRegistryTestActor());
	RegisterRegistryTestRecord(Entities, StandInHeld, TheirOwner, ECrowdyRole::RemoteProxy,
		MakeRegistryTestParticipant());

	// Unresolvable on purpose: an unknown class id with no class path to fall back to.
	AddExpectedErrorPlain(TEXT("Unknown ClassID"), EAutomationExpectedErrorFlags::Contains, 1);

	Entities->HandleRemoteSpawnForTest(MakeRegistryTestSpawnEvent(ActorHeld, TheirOwner, 0));
	Entities->HandleRemoteSpawnForTest(MakeRegistryTestSpawnEvent(StandInHeld, TheirOwner, 0));

	TestNotNull(TEXT("the entity whose actor is already active keeps its record"), Entities->FindRecord(ActorHeld));
	TestNotNull(TEXT("the stand-in keeps the id until an actor actually exists for it"),
		Entities->FindRecord(StandInHeld));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
