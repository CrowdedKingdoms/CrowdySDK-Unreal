#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Engine/GameInstance.h"
#include "Fragments/CrowdyMassFragments.h"
#include "Mass/CrowdyMassCharacterAvatar.h"
#include "Mass/CrowdyMassEntity.h"
#include "Mass/CrowdyMassEntityTestTypes.h"
#include "MassEntityManager.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Replication/RPC/FCrowdyRpcCall.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Replication/Subsystems/CrowdyEventRouter.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystem/CrowdyAutoRegistry.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"

// Covers task 2.2's avatar layer (resolution against the tracker, the class-mismatch refusal) and the
// two shipped events' wire encoding, plus the one end-to-end claim that spans 2.1 and 2.2 together: a
// real avatar action deferred past its action-sized budget drops rather than surviving toward the
// ordinary spawn-wait.
namespace
{
	constexpr EAutomationTestFlags CrowdyMassAvatarTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// A standalone Mass entity manager with no World, MassEntitySubsystem, spawner, or config asset, so
	// avatar resolution can be exercised against UCrowdyMassEntitySubsystem's own bookkeeping without the
	// machinery SpawnRemoteEntity needs. RegisterTrackedEntityForTest stands in for what a real
	// SpawnRemoteEntity call would have recorded.
	struct FCrowdyMassAvatarTestFixture
	{
		TSharedPtr<FMassEntityManager> EntityManager;
		UCrowdyMassEntitySubsystem* Subsystem = nullptr;

		FCrowdyMassAvatarTestFixture()
		{
			// The owner is not optional here even though the constructor defaults it away. PostInitialize
			// registers the engine's relation types, and registering one constructs relation observers
			// outered to exactly this owner, so a manager built with none tries to create an object with
			// no outer and the engine takes the process down with "Object is not packaged". The transient
			// package is the smallest thing that satisfies it without a world.
			EntityManager = MakeShareable(new FMassEntityManager(GetTransientPackage()));

			// The concurrent storage form rather than the argument-less one, which is deprecated: the
			// single-threaded storage it selects is being removed. The parameter is a variant over both
			// storage kinds, so the concurrent alternative has to be emplaced rather than passed, which
			// is how UMassEntitySubsystem builds its own.
			FMassEntityManagerStorageInitParams InitParams;
			InitParams.Emplace<FMassEntityManager_InitParams_Concurrent>();
			EntityManager->Initialize(InitParams);
			EntityManager->PostInitialize();

			Subsystem = NewObject<UCrowdyMassEntitySubsystem>(GetTransientPackage());
			Subsystem->SetMassEntityManagerForTest(EntityManager.Get());
		}

		~FCrowdyMassAvatarTestFixture()
		{
			EntityManager->Deinitialize();
		}

		// A real, valid Mass entity carrying no fragments (none of the resolution logic under test reads
		// one), registered under UUID exactly as SpawnRemoteEntity's own bookkeeping would.
		FMassEntityHandle MakeTrackedEntity(const FGuid& UUID)
		{
			const FMassArchetypeHandle EmptyArchetype =
				EntityManager->CreateArchetype(MakeArrayView<const UScriptStruct*>(nullptr, 0));
			const FMassEntityHandle Entity = EntityManager->CreateEntity(EmptyArchetype);
			Subsystem->RegisterTrackedEntityForTest(UUID, Entity);
			return Entity;
		}

		// The same, but carrying the identity fragment, for the tests that read who owns an entity.
		FMassEntityHandle MakeIdentifiedEntity(const FGuid& UUID, ECrowdyRole Role, const FGuid& OwnerID)
		{
			const UScriptStruct* Fragments[] = { FCrowdyRemoteIdentityFragment::StaticStruct() };
			const FMassArchetypeHandle Archetype = EntityManager->CreateArchetype(MakeArrayView(Fragments, 1));
			const FMassEntityHandle Entity = EntityManager->CreateEntity(Archetype);
			Subsystem->RegisterTrackedEntityForTest(UUID, Entity);

			if (FCrowdyRemoteIdentityFragment* Identity =
				EntityManager->GetFragmentDataPtr<FCrowdyRemoteIdentityFragment>(Entity))
			{
				Identity->UUID = UUID;
				Identity->Role = Role;
				Identity->OwnerID = OwnerID;
			}

			return Entity;
		}
	};
}

// Mutating UCrowdyMassEntitySubsystem::GetOrCreateAvatar's `NewObject<UCrowdyMassEntity>(this,
// SpawnClass)` to return nullptr unconditionally (or GetEffectiveAvatarClass to return null) turns this
// red: a known, tracked UUID would then fail to resolve anything.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAvatarKnownUUIDResolvesTest,
	"CrowdySDK.Mass.AvatarKnownUUIDResolves", CrowdyMassAvatarTestFlags)
bool FCrowdyMassAvatarKnownUUIDResolvesTest::RunTest(const FString& Parameters)
{
	FCrowdyMassAvatarTestFixture Fixture;
	const FGuid UUID = FGuid::NewGuid();
	Fixture.MakeTrackedEntity(UUID);

	UFunction* PlayOneShotFn = UCrowdyMassCharacterAvatar::StaticClass()->FindFunctionByName(TEXT("PlayOneShot_Implementation"));
	TestNotNull(TEXT("PlayOneShot resolved"), PlayOneShotFn);
	if (!PlayOneShotFn)
	{
		return false;
	}

	UObject* Receiver = Fixture.Subsystem->ResolveReceiver(UUID, PlayOneShotFn);
	TestNotNull(TEXT("a known, tracked UUID resolves an avatar"), Receiver);
	TestTrue(TEXT("the avatar is (a subclass of) the effective avatar class"),
		Receiver && Receiver->IsA(UCrowdyMassCharacterAvatar::StaticClass()));
	TestTrue(TEXT("the subsystem reports the entity known"), Fixture.Subsystem->IsEntityKnown(UUID));
	TestTrue(TEXT("GetAvatarState reports Present once minted"),
		Fixture.Subsystem->GetAvatarState(UUID) == ECrowdyMassAvatarState::Present);

	return true;
}

// Mutating the `if (!FindEntityByUUID(UUID, Entity)) return nullptr;` guard in ResolveReceiver /
// GetOrCreateAvatar (e.g. deleting it, or having FindEntityByUUID always report success) turns this red:
// an id nothing tracks would then mint and resolve an avatar for it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAvatarUnknownUUIDRefusedTest,
	"CrowdySDK.Mass.AvatarUnknownUUIDRefused", CrowdyMassAvatarTestFlags)
bool FCrowdyMassAvatarUnknownUUIDRefusedTest::RunTest(const FString& Parameters)
{
	FCrowdyMassAvatarTestFixture Fixture; // no entity registered at all

	UFunction* PlayOneShotFn = UCrowdyMassCharacterAvatar::StaticClass()->FindFunctionByName(TEXT("PlayOneShot_Implementation"));
	TestNotNull(TEXT("PlayOneShot resolved"), PlayOneShotFn);
	if (!PlayOneShotFn)
	{
		return false;
	}

	const FGuid UnknownUUID = FGuid::NewGuid();
	TestFalse(TEXT("an unregistered UUID is not known"), Fixture.Subsystem->IsEntityKnown(UnknownUUID));
	TestNull(TEXT("resolving an unknown UUID mints nothing"), Fixture.Subsystem->ResolveReceiver(UnknownUUID, PlayOneShotFn));
	TestTrue(TEXT("GetAvatarState reports NoEntity"),
		Fixture.Subsystem->GetAvatarState(UnknownUUID) == ECrowdyMassAvatarState::NoEntity);
	TestNull(TEXT("GetOrCreateAvatar refuses the same unknown id directly"),
		Fixture.Subsystem->GetOrCreateAvatar(UnknownUUID, nullptr));

	return true;
}

// Mutating away the `if (RequiredClass && !Cached->IsA(RequiredClass)) return nullptr;` check inside the
// cached-avatar branch of UCrowdyMassEntitySubsystem::GetOrCreateAvatar turns this red: the mismatched,
// already-cached avatar would be handed back instead of refused.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAvatarClassMismatchRefusedTest,
	"CrowdySDK.Mass.AvatarClassMismatchRefused", CrowdyMassAvatarTestFlags)
bool FCrowdyMassAvatarClassMismatchRefusedTest::RunTest(const FString& Parameters)
{
	FCrowdyMassAvatarTestFixture Fixture;
	const FGuid UUID = FGuid::NewGuid();
	Fixture.MakeTrackedEntity(UUID);

	// Mint the default avatar class first, exactly as a first inbound event with no class requirement
	// would (ResolveReceiver passes Function->GetOwnerClass(), but a direct GetOrCreateAvatar(UUID,
	// nullptr) call exercises the same first-mint path with no requirement at all).
	UCrowdyMassEntity* Minted = Fixture.Subsystem->GetOrCreateAvatar(UUID, nullptr);
	TestNotNull(TEXT("first mint succeeds"), Minted);
	TestTrue(TEXT("minted from the default avatar class"),
		Minted && Minted->IsA(UCrowdyMassCharacterAvatar::StaticClass()));

	// A receiver function declared on a class the cached avatar does not derive from must be refused,
	// not handed the mismatched cached object back.
	UCrowdyMassEntity* Mismatched = Fixture.Subsystem->GetOrCreateAvatar(UUID, UCrowdyMassOtherEntityTestReceiver::StaticClass());
	TestNull(TEXT("a class mismatch against the cached avatar is refused, not returned mismatched"), Mismatched);

	// The cache itself is untouched by the refusal: asking again for the class actually cached still
	// resolves the same object, not a freshly re-minted one.
	UCrowdyMassEntity* StillCached = Fixture.Subsystem->GetOrCreateAvatar(UUID, UCrowdyMassCharacterAvatar::StaticClass());
	TestTrue(TEXT("the correctly-classed request still resolves the same cached avatar"), StillCached == Minted);

	return true;
}

// Marshals both shipped avatar events and compares the wire bytes against an independently authored
// literal, not against a second call to the same serializer: a round-trip test comparing two
// serializations is blind to a field the writer silently drops, because both sides would lose it
// identically. Mutating FCrowdyRPC::SerializeParams's declaration-order iteration (e.g. skipping a
// parameter, or writing it in the wrong order) turns this red without needing a second encode to compare
// against.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAvatarEventsMarshalAuthoredBytesTest,
	"CrowdySDK.Mass.AvatarEventsMarshalAuthoredBytes", CrowdyMassAvatarTestFlags)
bool FCrowdyMassAvatarEventsMarshalAuthoredBytesTest::RunTest(const FString& Parameters)
{
	auto AppendLittleEndian = [](TArray<uint8>& Out, const void* Value, int32 Size)
	{
		const uint8* Bytes = static_cast<const uint8*>(Value);
		for (int32 ByteIndex = 0; ByteIndex < Size; ++ByteIndex)
		{
			Out.Add(Bytes[ByteIndex]);
		}
	};

	{
		UFunction* Fn = UCrowdyMassCharacterAvatar::StaticClass()->FindFunctionByName(TEXT("PlayOneShot_Implementation"));
		TestNotNull(TEXT("PlayOneShot resolved"), Fn);
		if (!Fn)
		{
			return false;
		}
		const FCrowdyFnInfo Info = FCrowdyRPC::BuildFnInfo(Fn);
		TestTrue(TEXT("three primitive params are POD"), Info.bParamsPOD);

		const int32 ActionId = 7;
		const int32 Variant = 3;
		const float PlayRate = 1.5f;
		const FCrowdyRpcCall Call = FCrowdyRPC::MarshalCall(Fn, Info,
			&UCrowdyMassCharacterAvatar::PlayOneShot_Implementation, ActionId, Variant, PlayRate);

		TArray<uint8> Expected;
		Expected.Add(CrowdyRpcParamBlobVersion);
		AppendLittleEndian(Expected, &ActionId, sizeof(ActionId));
		AppendLittleEndian(Expected, &Variant, sizeof(Variant));
		AppendLittleEndian(Expected, &PlayRate, sizeof(PlayRate));

		TestEqual(TEXT("PlayOneShot authored blob length"), Call.ParamBlob.Num(), Expected.Num());
		TestTrue(TEXT("PlayOneShot authored blob bytes match exactly, field for field"), Call.ParamBlob == Expected);
	}

	{
		UFunction* Fn = UCrowdyMassCharacterAvatar::StaticClass()->FindFunctionByName(TEXT("HitReact_Implementation"));
		TestNotNull(TEXT("HitReact resolved"), Fn);
		if (!Fn)
		{
			return false;
		}
		const FCrowdyFnInfo Info = FCrowdyRPC::BuildFnInfo(Fn);
		TestTrue(TEXT("two primitive params are POD"), Info.bParamsPOD);

		const int32 ActionId = 4;
		const int32 Variant = 11;
		const FCrowdyRpcCall Call = FCrowdyRPC::MarshalCall(Fn, Info,
			&UCrowdyMassCharacterAvatar::HitReact_Implementation, ActionId, Variant);

		TArray<uint8> Expected;
		Expected.Add(CrowdyRpcParamBlobVersion);
		AppendLittleEndian(Expected, &ActionId, sizeof(ActionId));
		AppendLittleEndian(Expected, &Variant, sizeof(Variant));

		TestEqual(TEXT("HitReact authored blob length"), Call.ParamBlob.Num(), Expected.Num());
		TestTrue(TEXT("HitReact authored blob bytes match exactly, field for field"), Call.ParamBlob == Expected);
	}

	return true;
}

// The end-to-end claim spanning 2.1 and 2.2: a real avatar PlayOneShot addressed to an entity that never
// registers anywhere is judged against its own 0.3s action budget, not the ordinary 600-tick spawn wait.
// Mutating CrowdyEventRouter.cpp's IsActionScopedCall to `return false` turns this red on its own (the
// call falls back to the ordinary unbounded wait and the first assertion after RetryDeferredForTest
// fails); mutating the deadline check in RetryDeferredEvents (`Now > Deferred.ExpiresAtSeconds`) turns it
// red the same way.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAvatarActionDeferralExpiresOnScheduleTest,
	"CrowdySDK.Mass.AvatarActionDeferralExpiresOnSchedule", CrowdyMassAvatarTestFlags)
bool FCrowdyMassAvatarActionDeferralExpiresOnScheduleTest::RunTest(const FString& Parameters)
{
	UGameInstance* GameInstance = NewObject<UGameInstance>(GetTransientPackage());
	UCrowdyAutoRegistry* Registry = NewObject<UCrowdyAutoRegistry>(GameInstance);
	Registry->UpdateClassRpcFunctions(UCrowdyMassCharacterAvatar::StaticClass());

	UCrowdyEntitySubsystem* Entities = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
	Entities->SetLocalPlayerID(FGuid::NewGuid());

	UCrowdyEventRouter* Router = NewObject<UCrowdyEventRouter>(GetTransientPackage());
	Router->SetAutoRegistryForTest(Registry);
	Router->SetEntitySubsystemForTest(Entities);
	// No ICrowdyEntitySubscriber is registered: this entity can never be resolved, so the deferral is
	// judged purely on its own budget rather than eventually finding a receiver.

	UFunction* Fn = UCrowdyMassCharacterAvatar::StaticClass()->FindFunctionByName(TEXT("PlayOneShot_Implementation"));
	TestNotNull(TEXT("PlayOneShot resolved"), Fn);
	if (!Fn)
	{
		return false;
	}
	const FCrowdyFnInfo Info = FCrowdyRPC::BuildFnInfo(Fn);

	FCrowdyRpcCall Call = FCrowdyRPC::MarshalCall(Fn, Info, &UCrowdyMassCharacterAvatar::PlayOneShot_Implementation, 1, 0, 1.0f);
	const FGuid NeverRegisteredEntityID = FGuid::NewGuid();
	Call.EntityID = NeverRegisteredEntityID;
	Call.SenderID = FGuid::NewGuid(); // foreign, so the broadcast echo-drop never fires

	// Scoped so the payload dies before the retry below, which is the production lifetime: the wire message
	// that carried it is gone the moment its delivery returns, and only the queue's own copy survives.
	{
		const FInstancedStruct Payload = FInstancedStruct::Make(Call);

		FCrowdyInboundEvent StaleEvent;
		StaleEvent.Payload = &Payload;
		StaleEvent.bTargetedDelivery = false;
		StaleEvent.Target = ECrowdyTarget::Everyone;
		// Stamped as though it arrived a full second ago: well past the 0.3s action budget before the very
		// first retry tick runs.
		StaleEvent.ReceivedAtSeconds = FPlatformTime::Seconds() - 1.0;

		Router->DispatchEvent(StaleEvent);
	}

	TestEqual(TEXT("a real avatar PlayOneShot for an id that never registers is queued once"),
		Router->NumDeferredForTest(), 1);

	Router->RetryDeferredForTest();
	TestEqual(TEXT("it is gone within its 0.3s action budget rather than surviving toward the 600-tick spawn wait"),
		Router->NumDeferredForTest(), 0);
	TestEqual(TEXT("the drop was counted as an eviction, not a silent loss"),
		Router->NumDeferredEvictionsForTest(), 1);

	// Control: the identical call, stamped as freshly arrived, must still be waiting after one retry
	// tick, since its budget has not elapsed yet. This is what distinguishes "judged against its own age"
	// from "always evicted on the first retry regardless of when it arrived".
	// Scoped for the same reason as the stale event above.
	{
		const FInstancedStruct Payload = FInstancedStruct::Make(Call);

		FCrowdyInboundEvent FreshEvent;
		FreshEvent.Payload = &Payload;
		FreshEvent.bTargetedDelivery = false;
		FreshEvent.Target = ECrowdyTarget::Everyone;
		FreshEvent.ReceivedAtSeconds = FPlatformTime::Seconds();

		Router->DispatchEvent(FreshEvent);
	}

	TestEqual(TEXT("a freshly-arrived deferral is queued"), Router->NumDeferredForTest(), 1);
	Router->RetryDeferredForTest();
	TestEqual(TEXT("and is still waiting after one retry, since its budget has not elapsed"),
		Router->NumDeferredForTest(), 1);

	return true;
}

// The send path asks this one question to decide whether this client may originate events for an
// entity, so every answer it can give is worth pinning. Mutating any branch of
// UCrowdyMassEntity::IsEventLocallyOwned turns one of these red on its own.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAvatarLocalOwnershipTest,
	"CrowdySDK.Mass.AvatarLocalOwnership", CrowdyMassAvatarTestFlags)
bool FCrowdyMassAvatarLocalOwnershipTest::RunTest(const FString& Parameters)
{
	FCrowdyMassAvatarTestFixture Fixture;

	const FGuid RemoteUUID(0xA1, 1, 2, 3);
	const FGuid HostUUID(0xA2, 1, 2, 3);
	const FGuid UnownedUUID(0xA3, 1, 2, 3);
	const FGuid ForeignOwnedUUID(0xA4, 1, 2, 3);
	const FGuid SomebodyElse(0x0BADC0DE, 1, 2, 3);

	Fixture.MakeIdentifiedEntity(RemoteUUID, ECrowdyRole::RemoteProxy, SomebodyElse);
	Fixture.MakeIdentifiedEntity(HostUUID, ECrowdyRole::HostOwned, FGuid());
	Fixture.MakeIdentifiedEntity(UnownedUUID, ECrowdyRole::Owner, FGuid());
	Fixture.MakeIdentifiedEntity(ForeignOwnedUUID, ECrowdyRole::Owner, SomebodyElse);

	const UClass* AvatarClass = UCrowdyMassEntity::StaticClass();

	// The case every crowd member is: another client simulates it and sends for it. This is the refusal
	// that stops one client originating events for another's entity.
	const UCrowdyMassEntity* Remote = Fixture.Subsystem->GetOrCreateAvatar(RemoteUUID, AvatarClass);
	if (TestNotNull(TEXT("the remote-proxy entity resolved an avatar"), Remote))
	{
		TestFalse(TEXT("a RemoteProxy entity is not locally owned, so this client may not send for it"),
			Remote->IsEventLocallyOwned());
	}

	// Host-owned is nobody's to claim through this question. The send path recognises it separately by
	// reading the role, and answering yes here would let a client that is not the host speak for one.
	const UCrowdyMassEntity* HostOwned = Fixture.Subsystem->GetOrCreateAvatar(HostUUID, AvatarClass);
	if (TestNotNull(TEXT("the host-owned entity resolved an avatar"), HostOwned))
	{
		TestFalse(TEXT("a HostOwned entity is not locally owned"), HostOwned->IsEventLocallyOwned());
	}

	// Owner with no owner id recorded: the send path substitutes the local player as the sender in
	// exactly this case, so the role's claim stands and the answer is yes.
	const UCrowdyMassEntity* Unowned = Fixture.Subsystem->GetOrCreateAvatar(UnownedUUID, AvatarClass);
	if (TestNotNull(TEXT("the owner-role entity with no owner id resolved an avatar"), Unowned))
	{
		TestTrue(TEXT("Owner with no owner id recorded is locally owned"), Unowned->IsEventLocallyOwned());
	}

	// This fixture has no world, so an entity claiming an owner id cannot have that id compared against
	// anything. The refusal is still the right answer, and it is a DIFFERENT rule from the comparison
	// itself: asserting the comparison through this path would pass without the comparison existing.
	const UCrowdyMassEntity* ForeignOwned = Fixture.Subsystem->GetOrCreateAvatar(ForeignOwnedUUID, AvatarClass);
	if (TestNotNull(TEXT("the foreign-owned entity resolved an avatar"), ForeignOwned))
	{
		TestFalse(TEXT("an owner id that cannot be compared at all is refused rather than assumed"),
			ForeignOwned->IsEventLocallyOwned());
	}

	return true;
}

// The rule itself, asserted directly rather than through an avatar, because reaching it through one
// needs a resolved world and a fixture without one refuses before the comparison runs. Mutating
// `return OwnerID == LocalPlayerID` to `return true` turns the last case red and nothing else.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassIdentityOwnershipRuleTest,
	"CrowdySDK.Mass.IdentityOwnershipRule", CrowdyMassAvatarTestFlags)
bool FCrowdyMassIdentityOwnershipRuleTest::RunTest(const FString& Parameters)
{
	const FGuid LocalPlayer(0x10CA1, 1, 2, 3);
	const FGuid SomebodyElse(0x0BADC0DE, 1, 2, 3);

	TestFalse(TEXT("RemoteProxy is never locally owned, whoever owns it"),
		IsCrowdyMassIdentityLocallyOwned(ECrowdyRole::RemoteProxy, SomebodyElse, LocalPlayer));
	TestFalse(TEXT("RemoteProxy is not locally owned even when the owner id is this client"),
		IsCrowdyMassIdentityLocallyOwned(ECrowdyRole::RemoteProxy, LocalPlayer, LocalPlayer));
	TestFalse(TEXT("HostOwned is nobody's to claim through this question"),
		IsCrowdyMassIdentityLocallyOwned(ECrowdyRole::HostOwned, FGuid(), LocalPlayer));
	TestFalse(TEXT("None is not locally owned"),
		IsCrowdyMassIdentityLocallyOwned(ECrowdyRole::None, FGuid(), LocalPlayer));

	TestTrue(TEXT("Owner with no owner id recorded is locally owned, since the sender defaults to this client"),
		IsCrowdyMassIdentityLocallyOwned(ECrowdyRole::Owner, FGuid(), LocalPlayer));
	TestTrue(TEXT("Owner whose owner id IS this client is locally owned"),
		IsCrowdyMassIdentityLocallyOwned(ECrowdyRole::Owner, LocalPlayer, LocalPlayer));

	// The hardening, and the only case in the matrix that the role alone would answer wrongly. An
	// identity whose two fields disagree must refuse: the role gates the send and the owner id stamps
	// the sender, so letting it through would emit an event under another player's name.
	TestFalse(TEXT("Owner claiming ANOTHER player's owner id is refused"),
		IsCrowdyMassIdentityLocallyOwned(ECrowdyRole::Owner, SomebodyElse, LocalPlayer));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
