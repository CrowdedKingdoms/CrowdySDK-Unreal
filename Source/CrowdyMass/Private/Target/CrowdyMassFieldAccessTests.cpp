#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Fragments/CrowdyMassAnimFragments.h"
#include "Fragments/CrowdyMassFragments.h"
#include "Mass/EntityFragments.h"
#include "MassEntityManager.h"
#include "MassEntityTypes.h"
#include "Target/CrowdyMassEntityTarget.h"
#include "Target/CrowdyMassFieldAccess.h"
#include "Target/CrowdyMassFieldAccessTestTypes.h"
#include "Target/CrowdyMassTargetLibrary.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/TopLevelAssetPath.h"

// Covers the by-name fragment field surface: what a registration will and will not accept, what a read and
// a write do to the entity's own memory, and every refusal either can make.
//
// Everything runs against a standalone Mass entity manager with no World, the same fixture shape the target
// resolve tests use, injected through CrowdyMassFields::SetEntityManagerForTest so the real Blueprint entry
// points are the ones under test. Letting a target resolve its own world instead would put all of it out of
// reach: that path reads the manager off UCrowdyMassEntitySubsystem, whose creation is restricted to PIE and
// Game worlds, and standing one of those up pulls in UMassEntitySubsystem's real initialization, which this
// codebase already documents as leaving Deinitialize asserting.
//
// The Mass-processing refusal is reached for real rather than simulated: FMassEntityManager hands out the
// same FScopedProcessing a running processor holds, so the tests below enter the actual scope that a
// delegate broadcast from inside a chunk loop would be inside.
namespace
{
	constexpr EAutomationTestFlags CrowdyMassFieldTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Built on use rather than held as namespace-scope FNames, so nothing here depends on when this
	// translation unit's statics are initialised relative to the name table's.
	FName FieldAnchor() { return FName(TEXT("CrowdyMassFieldTest.Anchor")); }
	FName FieldCharges() { return FName(TEXT("CrowdyMassFieldTest.Charges")); }
	FName FieldHeat() { return FName(TEXT("CrowdyMassFieldTest.Heat")); }
	FName FieldStance() { return FName(TEXT("CrowdyMassFieldTest.Stance")); }
	FName FieldTypedStance() { return FName(TEXT("CrowdyMassFieldTest.TypedStance")); }
	FName FieldAlerted() { return FName(TEXT("CrowdyMassFieldTest.Alerted")); }

	struct FCrowdyMassFieldFixture
	{
		TSharedPtr<FMassEntityManager> EntityManager;

		// False if any fixture registration was refused, which every test checks before asserting anything
		// else: a refused registration would otherwise present as every access answering UnknownField.
		bool bAllFieldsRegistered = false;

		FCrowdyMassFieldFixture()
		{
			// The manager needs a real owner: PostInitialize constructs relation observers outered to
			// whatever owner it was given, and an owner-less manager takes the process down.
			EntityManager = MakeShareable(new FMassEntityManager(GetTransientPackage()));

			FMassEntityManagerStorageInitParams InitParams;
			InitParams.Emplace<FMassEntityManager_InitParams_Concurrent>();
			EntityManager->Initialize(InitParams);
			EntityManager->PostInitialize();

			// The refusal throttle is process-wide and time-based, so without this a test asserting that a
			// refusal was reported would depend on what ran before it and how recently.
			CrowdyMassFields::ResetRefusalLogThrottleForTest();

			bAllFieldsRegistered =
				CrowdyMassFields::RegisterField<FCrowdyMassFieldTestFragment>(FieldAnchor(), TEXT("Anchor"))
				&& CrowdyMassFields::RegisterField<FCrowdyMassFieldTestFragment>(FieldCharges(), TEXT("Charges"))
				&& CrowdyMassFields::RegisterField<FCrowdyMassFieldTestFragment>(FieldHeat(), TEXT("Heat"))
				&& CrowdyMassFields::RegisterField<FCrowdyMassFieldTestFragment>(FieldStance(), TEXT("Stance"))
				&& CrowdyMassFields::RegisterField<FCrowdyMassFieldTestFragment>(FieldTypedStance(), TEXT("TypedStance"))
				&& CrowdyMassFields::RegisterField<FCrowdyMassFieldTestFragment>(FieldAlerted(), TEXT("bAlerted"));

			CrowdyMassFields::SetEntityManagerForTest(EntityManager.Get());
		}

		~FCrowdyMassFieldFixture()
		{
			CrowdyMassFields::SetEntityManagerForTest(nullptr);

			// One name at a time, and only the fixture's own: the registry is process-wide, so a test run
			// from the editor shares it with whatever the loaded game registered at startup.
			for (const FName FieldName : { FieldAnchor(), FieldCharges(), FieldHeat(), FieldStance(),
				FieldTypedStance(), FieldAlerted() })
			{
				CrowdyMassFields::UnregisterFieldForTest(FieldName);
			}

			EntityManager->Deinitialize();
		}

		FMassEntityHandle MakeEntity(const bool bWithTestFragment, const bool bWithPoseFragment = false,
			const bool bWithOtherFragment = false)
		{
			TArray<const UScriptStruct*> Fragments;
			Fragments.Add(FTransformFragment::StaticStruct());
			if (bWithTestFragment)
			{
				Fragments.Add(FCrowdyMassFieldTestFragment::StaticStruct());
			}
			if (bWithPoseFragment)
			{
				Fragments.Add(FCrowdyMassViewPoseFragment::StaticStruct());
			}
			if (bWithOtherFragment)
			{
				Fragments.Add(FCrowdyMassFieldTestOtherFragment::StaticStruct());
			}

			const FMassArchetypeHandle Archetype = EntityManager->CreateArchetype(Fragments);
			return EntityManager->CreateEntity(Archetype);
		}

		// A target naming Entity. World is deliberately left unset: nothing reached through the test seam
		// reads it, which is what puts these outcomes in reach at all.
		FCrowdyMassEntityTarget MakeTarget(const FMassEntityHandle Entity) const
		{
			FCrowdyMassEntityTarget Target;
			Target.Entity = Entity;
			return Target;
		}

		FCrowdyMassFieldTestFragment* Fragment(const FMassEntityHandle Entity) const
		{
			return EntityManager->GetFragmentDataPtr<FCrowdyMassFieldTestFragment>(Entity);
		}
	};
}

// Every accessor writes the entity's own fragment memory and reads back what it wrote, asserted against the
// fragment directly rather than against the reader. A reader and a writer agreeing with each other proves
// nothing on its own: both landing on the wrong property would agree perfectly.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassFieldRoundTripsThroughFragmentTest,
	"CrowdySDK.MassFields.RegisteredFieldRoundTripsThroughTheFragment", CrowdyMassFieldTestFlags)
bool FCrowdyMassFieldRoundTripsThroughFragmentTest::RunTest(const FString& Parameters)
{
	FCrowdyMassFieldFixture Fixture;
	if (!TestTrue(TEXT("every fixture field registered"), Fixture.bAllFieldsRegistered))
	{
		return false;
	}

	const FMassEntityHandle Entity = Fixture.MakeEntity(/*bWithTestFragment=*/true);
	const FCrowdyMassEntityTarget Target = Fixture.MakeTarget(Entity);
	if (!TestNotNull(TEXT("the fixture entity carries the test fragment"), Fixture.Fragment(Entity)))
	{
		return false;
	}

	TestTrue(TEXT("a registered name reports itself registered"),
		UCrowdyMassTargetLibrary::IsFieldRegistered(FieldStance()));
	TestFalse(TEXT("and a name nobody registered does not"),
		UCrowdyMassTargetLibrary::IsFieldRegistered(FName(TEXT("CrowdyMassFieldTest.NeverRegistered"))));

	TestTrue(TEXT("a byte write reports success"),
		UCrowdyMassTargetLibrary::SetFieldByte(Target, FieldStance(), 2));
	TestEqual(TEXT("the byte reached the fragment itself"),
		static_cast<int32>(Fixture.Fragment(Entity)->Stance), 2);

	uint8 ReadStance = 0;
	TestTrue(TEXT("the byte reads back"), UCrowdyMassTargetLibrary::GetFieldByte(Target, FieldStance(), ReadStance));
	TestEqual(TEXT("the byte read back is the one written"), static_cast<int32>(ReadStance), 2);

	TestTrue(TEXT("an integer write reports success"),
		UCrowdyMassTargetLibrary::SetFieldInt(Target, FieldCharges(), -17));
	TestEqual(TEXT("the integer reached the fragment itself"), Fixture.Fragment(Entity)->Charges, -17);

	int32 ReadCharges = 0;
	TestTrue(TEXT("the integer reads back"), UCrowdyMassTargetLibrary::GetFieldInt(Target, FieldCharges(), ReadCharges));
	TestEqual(TEXT("the integer read back is the one written"), ReadCharges, -17);

	TestTrue(TEXT("a real write reports success"),
		UCrowdyMassTargetLibrary::SetFieldFloat(Target, FieldHeat(), 0.5));
	TestEqual(TEXT("the real reached the fragment itself, narrowed to the float it is declared as"),
		Fixture.Fragment(Entity)->Heat, 0.5f);

	double ReadHeat = 0.0;
	TestTrue(TEXT("the real reads back"), UCrowdyMassTargetLibrary::GetFieldFloat(Target, FieldHeat(), ReadHeat));
	TestEqual(TEXT("the real read back is the one written"), ReadHeat, 0.5);

	TestTrue(TEXT("a boolean write reports success"),
		UCrowdyMassTargetLibrary::SetFieldBool(Target, FieldAlerted(), true));
	TestTrue(TEXT("the boolean reached the fragment itself"), Fixture.Fragment(Entity)->bAlerted);

	bool bReadAlerted = false;
	TestTrue(TEXT("the boolean reads back"), UCrowdyMassTargetLibrary::GetFieldBool(Target, FieldAlerted(), bReadAlerted));
	TestTrue(TEXT("the boolean read back is the one written"), bReadAlerted);

	const FVector Anchor(12.0, -3.5, 0.25);
	TestTrue(TEXT("a vector write reports success"),
		UCrowdyMassTargetLibrary::SetFieldVector(Target, FieldAnchor(), Anchor));
	TestEqual(TEXT("the vector reached the fragment itself"), Fixture.Fragment(Entity)->Anchor, Anchor);

	FVector ReadAnchor = FVector::ZeroVector;
	TestTrue(TEXT("the vector reads back"), UCrowdyMassTargetLibrary::GetFieldVector(Target, FieldAnchor(), ReadAnchor));
	TestEqual(TEXT("the vector read back is the one written"), ReadAnchor, Anchor);

	// Nothing else moved. A defect resolving the wrong property would leave one of these carrying the value
	// aimed at another, which is invisible to any single field's own round trip.
	TestEqual(TEXT("the integer was not disturbed by the later writes"), Fixture.Fragment(Entity)->Charges, -17);
	TestEqual(TEXT("the byte was not disturbed by the later writes"),
		static_cast<int32>(Fixture.Fragment(Entity)->Stance), 2);

	return true;
}

// THE LOAD-BEARING CASE. A write reached from Blueprint while Mass is running processors is refused,
// reported, and leaves the value alone; the identical write lands on both sides of the scope, so what the
// refusal is about is the scope and not the write.
//
// This is the hazard the surface exists to close: a Blueprint bound to a delegate broadcast from inside a
// chunk loop would otherwise write a fragment during the schedule, racing every processor declared to read
// it, which no amount of care at the call site can prevent.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassFieldWriteRefusedInsideProcessingTest,
	"CrowdySDK.MassFields.WriteRefusedInsideMassProcessingScope", CrowdyMassFieldTestFlags)
bool FCrowdyMassFieldWriteRefusedInsideProcessingTest::RunTest(const FString& Parameters)
{
	FCrowdyMassFieldFixture Fixture;
	if (!TestTrue(TEXT("every fixture field registered"), Fixture.bAllFieldsRegistered))
	{
		return false;
	}

	const FMassEntityHandle Entity = Fixture.MakeEntity(/*bWithTestFragment=*/true);
	const FCrowdyMassEntityTarget Target = Fixture.MakeTarget(Entity);

	TestTrue(TEXT("the write lands with nothing processing"),
		UCrowdyMassTargetLibrary::SetFieldInt(Target, FieldCharges(), 3));
	TestEqual(TEXT("and reaches the fragment"), Fixture.Fragment(Entity)->Charges, 3);

	AddExpectedErrorPlain(TEXT("was written while Mass processors are running"),
		EAutomationExpectedErrorFlags::Contains, 1);

	{
		// The same scope a running processor holds, taken from the entity manager itself rather than
		// simulated, so this is the real condition and not a stand-in for it.
		FMassEntityManager::FScopedProcessing ProcessingScope = Fixture.EntityManager->NewProcessingScope();

		TestFalse(TEXT("the same write is refused while processors are running"),
			UCrowdyMassTargetLibrary::SetFieldInt(Target, FieldCharges(), 9));
		TestEqual(TEXT("and the fragment still holds what it held before"), Fixture.Fragment(Entity)->Charges, 3);
	}

	TestTrue(TEXT("the write lands again once the scope is over"),
		UCrowdyMassTargetLibrary::SetFieldInt(Target, FieldCharges(), 9));
	TestEqual(TEXT("and reaches the fragment"), Fixture.Fragment(Entity)->Charges, 9);

	return true;
}

// A read is refused inside the same scope, for the weaker reason: the value is being written by the
// schedule right now, so nothing here could answer with it. Reported at Warning rather than Error, because
// a stale read is not the corruption a write would be.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassFieldReadRefusedInsideProcessingTest,
	"CrowdySDK.MassFields.ReadRefusedInsideMassProcessingScope", CrowdyMassFieldTestFlags)
bool FCrowdyMassFieldReadRefusedInsideProcessingTest::RunTest(const FString& Parameters)
{
	FCrowdyMassFieldFixture Fixture;
	if (!TestTrue(TEXT("every fixture field registered"), Fixture.bAllFieldsRegistered))
	{
		return false;
	}

	const FMassEntityHandle Entity = Fixture.MakeEntity(/*bWithTestFragment=*/true);
	const FCrowdyMassEntityTarget Target = Fixture.MakeTarget(Entity);
	Fixture.Fragment(Entity)->Charges = 42;

	AddExpectedErrorPlain(TEXT("was read while Mass processors are running"),
		EAutomationExpectedErrorFlags::Contains, 1);

	int32 ReadCharges = -1;
	{
		FMassEntityManager::FScopedProcessing ProcessingScope = Fixture.EntityManager->NewProcessingScope();

		TestFalse(TEXT("a read is refused while processors are running"),
			UCrowdyMassTargetLibrary::GetFieldInt(Target, FieldCharges(), ReadCharges));
		TestEqual(TEXT("and answers with a default rather than a half-written value"), ReadCharges, 0);
	}

	TestTrue(TEXT("the read answers once the scope is over"),
		UCrowdyMassTargetLibrary::GetFieldInt(Target, FieldCharges(), ReadCharges));
	TestEqual(TEXT("with the value the fragment actually holds"), ReadCharges, 42);

	return true;
}

// The pose write runs under the same gate, which is the point of it having one at all: setting a pose from
// a death delegate is the most obvious use of these two features together, and that delegate is broadcast
// from inside a chunk loop.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassPoseWriteRefusedInsideProcessingTest,
	"CrowdySDK.MassFields.PoseWriteRefusedInsideMassProcessingScope", CrowdyMassFieldTestFlags)
bool FCrowdyMassPoseWriteRefusedInsideProcessingTest::RunTest(const FString& Parameters)
{
	FCrowdyMassFieldFixture Fixture;

	const FMassEntityHandle Entity = Fixture.MakeEntity(/*bWithTestFragment=*/false, /*bWithPoseFragment=*/true);
	const FCrowdyMassEntityTarget Target = Fixture.MakeTarget(Entity);

	// Re-read every time rather than held across the calls below, for the same reason the accessors
	// themselves never hold one: a fragment pointer belongs to the call that fetched it.
	const auto PoseIdOf = [&Fixture, Entity]() -> int32
	{
		const FCrowdyMassViewPoseFragment* Fragment =
			Fixture.EntityManager->GetFragmentDataPtr<FCrowdyMassViewPoseFragment>(Entity);
		return Fragment ? static_cast<int32>(Fragment->PoseId) : INDEX_NONE;
	};

	if (!TestEqual(TEXT("the fixture entity carries a pose fragment holding no pose"), PoseIdOf(), 0))
	{
		return false;
	}

	const CrowdyMassFields::EAccessResult Wrote = CrowdyMassFields::SetPoseIdIn(*Fixture.EntityManager, Target, 7);
	TestTrue(FString::Printf(TEXT("the pose write lands with nothing processing, instead of: %s"),
		CrowdyMassFields::DescribeAccessResult(Wrote)), Wrote == CrowdyMassFields::EAccessResult::Ok);
	TestEqual(TEXT("and reaches the pose fragment"), PoseIdOf(), 7);

	AddExpectedErrorPlain(TEXT("was written while Mass processors are running"),
		EAutomationExpectedErrorFlags::Contains, 1);

	{
		FMassEntityManager::FScopedProcessing ProcessingScope = Fixture.EntityManager->NewProcessingScope();

		TestTrue(TEXT("the same pose write is refused while processors are running"),
			CrowdyMassFields::SetPoseIdIn(*Fixture.EntityManager, Target, 3)
				== CrowdyMassFields::EAccessResult::DuringMassProcessing);
		TestEqual(TEXT("and the pose fragment still holds what it held before"), PoseIdOf(), 7);
	}

	// An entity with no pose fragment is an ordinary miss and not the same answer as a refusal.
	const FMassEntityHandle Poseless = Fixture.MakeEntity(/*bWithTestFragment=*/true);
	TestTrue(TEXT("an entity carrying no pose fragment answers NoFragment, not a refusal"),
		CrowdyMassFields::SetPoseIdIn(*Fixture.EntityManager, Fixture.MakeTarget(Poseless), 1)
			== CrowdyMassFields::EAccessResult::NoFragment);

	return true;
}

// A member with no UPROPERTY has no reflection entry at all, so it can never be found by name. Registering
// one is refused at registration, where the cause is visible, rather than accepted and then presenting as a
// field that reads nothing forever.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassFieldUnreflectedMemberRefusedTest,
	"CrowdySDK.MassFields.UnreflectedMemberCannotBeRegistered", CrowdyMassFieldTestFlags)
bool FCrowdyMassFieldUnreflectedMemberRefusedTest::RunTest(const FString& Parameters)
{
	FCrowdyMassFieldFixture Fixture;

	const FName UnmarkedField(TEXT("CrowdyMassFieldTest.Unmarked"));

	AddExpectedErrorPlain(TEXT("has no UPROPERTY named 'Unmarked'"), EAutomationExpectedErrorFlags::Contains, 1);

	TestFalse(TEXT("registering an unreflected member is refused"),
		CrowdyMassFields::RegisterField<FCrowdyMassFieldTestFragment>(UnmarkedField, TEXT("Unmarked")));
	TestFalse(TEXT("and nothing is left registered under that name"),
		CrowdyMassFields::IsFieldRegistered(UnmarkedField));

	return true;
}

// A field name is claimed process-wide, so a second binding under one name is refused and the first is
// kept. The assertion is on where a write LANDS, not merely on the return value: the failure this guards
// against is one registration silently answering for another's field.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassFieldSecondBindingRefusedTest,
	"CrowdySDK.MassFields.SecondBindingUnderOneNameIsRefused", CrowdyMassFieldTestFlags)
bool FCrowdyMassFieldSecondBindingRefusedTest::RunTest(const FString& Parameters)
{
	FCrowdyMassFieldFixture Fixture;
	if (!TestTrue(TEXT("every fixture field registered"), Fixture.bAllFieldsRegistered))
	{
		return false;
	}

	// Registering the identical binding again is what a per-world startup path does on the second world, so
	// it succeeds rather than reporting a conflict with itself.
	TestTrue(TEXT("re-registering the identical binding succeeds"),
		CrowdyMassFields::RegisterField<FCrowdyMassFieldTestFragment>(FieldCharges(), TEXT("Charges")));

	AddExpectedErrorPlain(TEXT("is already registered on"), EAutomationExpectedErrorFlags::Contains, 1);

	TestFalse(TEXT("binding the same name to a different fragment is refused"),
		CrowdyMassFields::RegisterField<FCrowdyMassFieldTestOtherFragment>(FieldCharges(), TEXT("Charges")));

	const CrowdyMassFields::FFieldBinding* Binding = CrowdyMassFields::FindField(FieldCharges());
	if (TestNotNull(TEXT("the name is still bound to something"), Binding))
	{
		TestTrue(TEXT("and it is still the first binding's fragment"),
			Binding->FragmentType == FCrowdyMassFieldTestFragment::StaticStruct());
	}

	const FMassEntityHandle Entity = Fixture.MakeEntity(/*bWithTestFragment=*/true, /*bWithPoseFragment=*/false,
		/*bWithOtherFragment=*/true);
	const FCrowdyMassEntityTarget Target = Fixture.MakeTarget(Entity);

	TestTrue(TEXT("a write through that name still succeeds"),
		UCrowdyMassTargetLibrary::SetFieldInt(Target, FieldCharges(), 5));
	TestEqual(TEXT("and lands on the first binding's fragment"), Fixture.Fragment(Entity)->Charges, 5);

	const FCrowdyMassFieldTestOtherFragment* Other =
		Fixture.EntityManager->GetFragmentDataPtr<FCrowdyMassFieldTestOtherFragment>(Entity);
	if (TestNotNull(TEXT("the entity also carries the refused binding's fragment"), Other))
	{
		TestEqual(TEXT("which the write did not touch"), Other->Charges, 0);
	}

	return true;
}

// The identity fragment carries the id, owner and role that routing and authority are decided from, so it
// is the one fragment a graph must not be able to rewrite. Refused at registration, which is the only place
// it can be refused once and for all rather than per call.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassFieldIdentityFragmentRefusedTest,
	"CrowdySDK.MassFields.IdentityFragmentCannotBeRegistered", CrowdyMassFieldTestFlags)
bool FCrowdyMassFieldIdentityFragmentRefusedTest::RunTest(const FString& Parameters)
{
	FCrowdyMassFieldFixture Fixture;

	const FName IdentityField(TEXT("CrowdyMassFieldTest.Identity"));

	AddExpectedErrorPlain(TEXT("the remote identity fragment carries the entity's id"),
		EAutomationExpectedErrorFlags::Contains, 1);

	TestFalse(TEXT("registering a field on the identity fragment is refused"),
		CrowdyMassFields::RegisterField<FCrowdyRemoteIdentityFragment>(IdentityField, TEXT("UUID")));
	TestFalse(TEXT("and nothing is left registered under that name"),
		CrowdyMassFields::IsFieldRegistered(IdentityField));

	return true;
}

// Reading a field as the wrong type is refused rather than reinterpreted, and a refused write leaves the
// value alone. Reading a float's bytes as an integer would answer with a number, and it would be a
// plausible one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassFieldTypeMismatchRefusedTest,
	"CrowdySDK.MassFields.TypeMismatchRefusedAndLeavesTheValueAlone", CrowdyMassFieldTestFlags)
bool FCrowdyMassFieldTypeMismatchRefusedTest::RunTest(const FString& Parameters)
{
	FCrowdyMassFieldFixture Fixture;
	if (!TestTrue(TEXT("every fixture field registered"), Fixture.bAllFieldsRegistered))
	{
		return false;
	}

	const FMassEntityHandle Entity = Fixture.MakeEntity(/*bWithTestFragment=*/true);
	const FCrowdyMassEntityTarget Target = Fixture.MakeTarget(Entity);
	Fixture.Fragment(Entity)->Heat = 2.5f;

	AddExpectedErrorPlain(TEXT("cannot be written as the type asked for here"),
		EAutomationExpectedErrorFlags::Contains, 1);

	TestFalse(TEXT("writing a real field as an integer is refused"),
		UCrowdyMassTargetLibrary::SetFieldInt(Target, FieldHeat(), 9));
	TestEqual(TEXT("and the field still holds its own value"), Fixture.Fragment(Entity)->Heat, 2.5f);

	// Same field, same throttle entry: the read's own line is suppressed within the interval, so only the
	// answer is asserted here and not a second log line.
	int32 ReadHeat = -1;
	TestFalse(TEXT("reading a real field as an integer is refused"),
		UCrowdyMassTargetLibrary::GetFieldInt(Target, FieldHeat(), ReadHeat));
	TestEqual(TEXT("and answers with a default rather than the float's bytes"), ReadHeat, 0);

	return true;
}

// A byte field also reaches a single-byte enum member, so an authored state enum does not have to be
// declared as a raw byte to be reachable from a graph.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassFieldByteReachesEnumMemberTest,
	"CrowdySDK.MassFields.ByteAccessorReachesASingleByteEnumMember", CrowdyMassFieldTestFlags)
bool FCrowdyMassFieldByteReachesEnumMemberTest::RunTest(const FString& Parameters)
{
	FCrowdyMassFieldFixture Fixture;
	if (!TestTrue(TEXT("every fixture field registered"), Fixture.bAllFieldsRegistered))
	{
		return false;
	}

	const FMassEntityHandle Entity = Fixture.MakeEntity(/*bWithTestFragment=*/true);
	const FCrowdyMassEntityTarget Target = Fixture.MakeTarget(Entity);

	TestTrue(TEXT("a byte write onto an enum member succeeds"),
		UCrowdyMassTargetLibrary::SetFieldByte(Target, FieldTypedStance(),
			static_cast<uint8>(ECrowdyMassFieldTestStance::Prone)));
	TestTrue(TEXT("and the member holds the enumerator it names"),
		Fixture.Fragment(Entity)->TypedStance == ECrowdyMassFieldTestStance::Prone);

	uint8 ReadStance = 0;
	TestTrue(TEXT("the enum member reads back as a byte"),
		UCrowdyMassTargetLibrary::GetFieldByte(Target, FieldTypedStance(), ReadStance));
	TestEqual(TEXT("with the enumerator's own value"), static_cast<int32>(ReadStance),
		static_cast<int32>(ECrowdyMassFieldTestStance::Prone));

	return true;
}

// A destroyed entity's handle is refused. This is the whole reason a target carries a handle and no id:
// Mass bumps a slot's serial when it is reused, so a target held across a despawn cannot start naming
// whatever respawned into the same slot, and a queued hit cannot follow a player onto their fresh body.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassFieldStaleHandleRefusedTest,
	"CrowdySDK.MassFields.StaleHandleRefusedAfterTheEntityIsDestroyed", CrowdyMassFieldTestFlags)
bool FCrowdyMassFieldStaleHandleRefusedTest::RunTest(const FString& Parameters)
{
	FCrowdyMassFieldFixture Fixture;
	if (!TestTrue(TEXT("every fixture field registered"), Fixture.bAllFieldsRegistered))
	{
		return false;
	}

	const FMassEntityHandle Entity = Fixture.MakeEntity(/*bWithTestFragment=*/true);
	const FCrowdyMassEntityTarget Target = Fixture.MakeTarget(Entity);

	TestTrue(TEXT("the target reports itself live while the entity is alive"),
		UCrowdyMassTargetLibrary::IsTargetLive(Target));
	TestTrue(TEXT("the write lands while the entity is alive"),
		UCrowdyMassTargetLibrary::SetFieldInt(Target, FieldCharges(), 4));

	Fixture.EntityManager->DestroyEntity(Entity);

	TestFalse(TEXT("the target stops reporting itself live once the entity is destroyed"),
		UCrowdyMassTargetLibrary::IsTargetLive(Target));
	TestFalse(TEXT("the same target is refused once the entity is destroyed"),
		UCrowdyMassTargetLibrary::SetFieldInt(Target, FieldCharges(), 5));

	int32 ReadCharges = -1;
	TestFalse(TEXT("and so is a read through it"),
		UCrowdyMassTargetLibrary::GetFieldInt(Target, FieldCharges(), ReadCharges));

	return true;
}

// An entity that carries none of the fragment is an ordinary miss, not a fault: an entity built from a
// different config legitimately lacks fragments another one has.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassFieldMissingFragmentRefusedTest,
	"CrowdySDK.MassFields.EntityWithoutTheFragmentIsRefused", CrowdyMassFieldTestFlags)
bool FCrowdyMassFieldMissingFragmentRefusedTest::RunTest(const FString& Parameters)
{
	FCrowdyMassFieldFixture Fixture;
	if (!TestTrue(TEXT("every fixture field registered"), Fixture.bAllFieldsRegistered))
	{
		return false;
	}

	const FMassEntityHandle Bare = Fixture.MakeEntity(/*bWithTestFragment=*/false);
	const FCrowdyMassEntityTarget Target = Fixture.MakeTarget(Bare);

	int32 ReadCharges = -1;
	TestFalse(TEXT("a read on an entity with no such fragment is refused"),
		UCrowdyMassTargetLibrary::GetFieldInt(Target, FieldCharges(), ReadCharges));
	TestFalse(TEXT("and so is a write"),
		UCrowdyMassTargetLibrary::SetFieldInt(Target, FieldCharges(), 1));

	// An unregistered name is a different answer from a missing fragment, and both are different from a
	// refusal: only one of the three will ever start working on its own.
	const FMassEntityHandle WithFragment = Fixture.MakeEntity(/*bWithTestFragment=*/true);
	CrowdyMassFields::ResetRefusalLogThrottleForTest();
	AddExpectedErrorPlain(TEXT("Nothing is registered as"), EAutomationExpectedErrorFlags::Contains, 1);

	TestFalse(TEXT("a name nobody registered is refused"),
		UCrowdyMassTargetLibrary::SetFieldInt(Fixture.MakeTarget(WithFragment),
			FName(TEXT("CrowdyMassFieldTest.NeverRegistered")), 1));

	return true;
}

// An archetype change moves the entity's data, and the same target keeps working across it. Nothing here
// may hold a fragment pointer between calls: one that outlived the call that fetched it would be reading
// the archetype the entity has left.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassFieldSurvivesArchetypeChangeTest,
	"CrowdySDK.MassFields.TargetStillWorksAfterAnArchetypeChange", CrowdyMassFieldTestFlags)
bool FCrowdyMassFieldSurvivesArchetypeChangeTest::RunTest(const FString& Parameters)
{
	FCrowdyMassFieldFixture Fixture;
	if (!TestTrue(TEXT("every fixture field registered"), Fixture.bAllFieldsRegistered))
	{
		return false;
	}

	const FMassEntityHandle Entity = Fixture.MakeEntity(/*bWithTestFragment=*/true);
	const FCrowdyMassEntityTarget Target = Fixture.MakeTarget(Entity);

	TestTrue(TEXT("the write lands before the archetype changes"),
		UCrowdyMassTargetLibrary::SetFieldInt(Target, FieldCharges(), 11));

	// Adding a fragment moves the entity to a different archetype, which is exactly what invalidates any
	// pointer taken before it.
	Fixture.EntityManager->AddFragmentToEntity(Entity, FCrowdyMassViewPoseFragment::StaticStruct());

	int32 ReadCharges = -1;
	TestTrue(TEXT("the same target still reads after the archetype changes"),
		UCrowdyMassTargetLibrary::GetFieldInt(Target, FieldCharges(), ReadCharges));
	TestEqual(TEXT("and the value moved with the entity"), ReadCharges, 11);

	TestTrue(TEXT("and it still writes"), UCrowdyMassTargetLibrary::SetFieldInt(Target, FieldCharges(), 12));
	TestEqual(TEXT("into the entity's new home"), Fixture.Fragment(Entity)->Charges, 12);

	return true;
}

// A code reload replaces a fragment's UScriptStruct and destroys the properties the old one owned, so every
// binding registered against it holds a pointer that addresses nothing. The rebind re-resolves each binding
// from the names it was registered under, which is the one part of it a reload cannot invalidate.
//
// Reloading native code from inside a running test is not something a test can do, so the registry is put
// into the state a reload leaves it in instead: resolved pointers that address a different fragment while
// the stored identity still names the right one. What is asserted is where a write LANDS on either side of
// the rebind, not what the binding reports about itself: a binding that reports correctly and writes
// somewhere else is exactly the failure being guarded against.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassFieldRebindRestoresBindingTest,
	"CrowdySDK.MassFields.RebindRestoresABindingThatMoved", CrowdyMassFieldTestFlags)
bool FCrowdyMassFieldRebindRestoresBindingTest::RunTest(const FString& Parameters)
{
	FCrowdyMassFieldFixture Fixture;
	if (!TestTrue(TEXT("every fixture field registered"), Fixture.bAllFieldsRegistered))
	{
		return false;
	}

#if WITH_EDITOR
	TestTrue(TEXT("registering a field installs the reload hook that re-resolves bindings"),
		CrowdyMassFields::IsReloadRebindInstalledForTest());
#endif

	const UScriptStruct* Registered = FCrowdyMassFieldTestFragment::StaticStruct();
	const UScriptStruct* Elsewhere = FCrowdyMassFieldTestOtherFragment::StaticStruct();

	CrowdyMassFields::FFieldBinding Moved;
	Moved.FragmentType = Elsewhere;
	Moved.Property = Elsewhere->FindPropertyByName(TEXT("Charges"));
	// The identity is left naming what the field was actually registered on. That is what a reload leaves
	// behind: the names survive it, the pointers do not.
	Moved.FragmentTypePath = Registered->GetStructPathName();
	Moved.PropertyName = TEXT("Charges");

	if (!TestNotNull(TEXT("the second fixture fragment declares a member for the moved binding to point at"),
		Moved.Property))
	{
		return false;
	}
	if (!TestTrue(TEXT("the registered binding was replaced with the moved one"),
		CrowdyMassFields::OverwriteFieldBindingForTest(FieldCharges(), Moved)))
	{
		return false;
	}

	const FMassEntityHandle Entity = Fixture.MakeEntity(/*bWithTestFragment=*/true, /*bWithPoseFragment=*/false,
		/*bWithOtherFragment=*/true);
	const FCrowdyMassEntityTarget Target = Fixture.MakeTarget(Entity);

	// Re-read every time, for the same reason the accessors never hold one: a fragment pointer belongs to the
	// call that fetched it.
	const auto ElsewhereCharges = [&Fixture, Entity]() -> int32
	{
		const FCrowdyMassFieldTestOtherFragment* Fragment =
			Fixture.EntityManager->GetFragmentDataPtr<FCrowdyMassFieldTestOtherFragment>(Entity);
		return Fragment ? Fragment->Charges : INDEX_NONE;
	};

	// Establishes that the moved binding is the one being followed, so that what the rebind changes is not
	// being read off a registry nothing was consulting.
	TestTrue(TEXT("a write through the moved binding reports success"),
		UCrowdyMassTargetLibrary::SetFieldInt(Target, FieldCharges(), 41));
	TestEqual(TEXT("and lands on the fragment the moved binding points at"), ElsewhereCharges(), 41);
	TestEqual(TEXT("leaving the fragment the name was registered against untouched"),
		Fixture.Fragment(Entity)->Charges, 0);

	TestEqual(TEXT("the rebind dropped nothing: every binding still resolves"),
		CrowdyMassFields::RebindRegisteredFields(), 0);

	const CrowdyMassFields::FFieldBinding* Rebound = CrowdyMassFields::FindField(FieldCharges());
	if (!TestNotNull(TEXT("the field is still registered after the rebind"), Rebound))
	{
		return false;
	}
	TestTrue(TEXT("the binding points at the fragment it was registered against again"),
		Rebound->FragmentType == Registered);
	TestTrue(TEXT("and at that fragment's own property, not merely at some property"),
		Rebound->Property == Registered->FindPropertyByName(TEXT("Charges")));

	TestTrue(TEXT("a write after the rebind reports success"),
		UCrowdyMassTargetLibrary::SetFieldInt(Target, FieldCharges(), 17));
	TestEqual(TEXT("and lands on the fragment the name was registered against"),
		Fixture.Fragment(Entity)->Charges, 17);
	TestEqual(TEXT("while the fragment the moved binding pointed at keeps what it already held"),
		ElsewhereCharges(), 41);

	return true;
}

// A rebind that emptied the registry would be a worse defect than the one it fixes and a silent one:
// registrations run from startup or from a world's setup, so they would never run again and every access for
// the rest of the session would answer as though the game had never registered anything. Nothing that still
// resolves may be disturbed, and both halves of that are asserted: the registrations survive, and each one
// still names the same property afterwards.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassFieldRebindKeepsRegistrationsTest,
	"CrowdySDK.MassFields.RebindKeepsEveryRegistrationThatStillResolves", CrowdyMassFieldTestFlags)
bool FCrowdyMassFieldRebindKeepsRegistrationsTest::RunTest(const FString& Parameters)
{
	FCrowdyMassFieldFixture Fixture;
	if (!TestTrue(TEXT("every fixture field registered"), Fixture.bAllFieldsRegistered))
	{
		return false;
	}

	const TArray<FName> FixtureFields = { FieldAnchor(), FieldCharges(), FieldHeat(), FieldStance(),
		FieldTypedStance(), FieldAlerted() };

	// Copied out rather than held as pointers into the registry, so the comparison afterwards is against
	// values recorded before the rebind and not against whatever the registry holds by then.
	TMap<FName, CrowdyMassFields::FFieldBinding> Before;
	for (const FName FieldName : FixtureFields)
	{
		const CrowdyMassFields::FFieldBinding* Binding = CrowdyMassFields::FindField(FieldName);
		if (!TestNotNull(*FString::Printf(TEXT("'%s' is registered before the rebind"), *FieldName.ToString()),
			Binding))
		{
			return false;
		}
		Before.Add(FieldName, *Binding);
	}

	TestEqual(TEXT("a rebind of a registry that still resolves drops nothing"),
		CrowdyMassFields::RebindRegisteredFields(), 0);

	for (const FName FieldName : FixtureFields)
	{
		const CrowdyMassFields::FFieldBinding* After = CrowdyMassFields::FindField(FieldName);
		if (!TestNotNull(*FString::Printf(TEXT("'%s' is still registered after the rebind"), *FieldName.ToString()),
			After))
		{
			continue;
		}

		const CrowdyMassFields::FFieldBinding& Was = Before[FieldName];
		TestTrue(*FString::Printf(TEXT("'%s' still names the same fragment"), *FieldName.ToString()),
			After->FragmentType == Was.FragmentType);
		TestTrue(*FString::Printf(TEXT("'%s' still names the same property"), *FieldName.ToString()),
			After->Property == Was.Property);
	}

	// And the surface itself still works, which is the thing a caller would notice.
	const FMassEntityHandle Entity = Fixture.MakeEntity(/*bWithTestFragment=*/true);
	const FCrowdyMassEntityTarget Target = Fixture.MakeTarget(Entity);

	TestTrue(TEXT("a write still lands after a rebind"),
		UCrowdyMassTargetLibrary::SetFieldInt(Target, FieldCharges(), 23));
	TestEqual(TEXT("on the property it always did"), Fixture.Fragment(Entity)->Charges, 23);

	return true;
}

// A member a developer has deleted, and a fragment type they have removed, both leave a binding that can
// never resolve again. Each is reported once and unregistered, and neither takes anything else with it: a
// name whose fragment is gone is freed rather than left registered and permanently answering nothing, so
// registering it again against whatever carries it now succeeds.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassFieldRebindDropsUnresolvableTest,
	"CrowdySDK.MassFields.RebindUnregistersOnlyWhatStoppedResolving", CrowdyMassFieldTestFlags)
bool FCrowdyMassFieldRebindDropsUnresolvableTest::RunTest(const FString& Parameters)
{
	FCrowdyMassFieldFixture Fixture;
	if (!TestTrue(TEXT("every fixture field registered"), Fixture.bAllFieldsRegistered))
	{
		return false;
	}

	const UScriptStruct* Registered = FCrowdyMassFieldTestFragment::StaticStruct();

	// The fragment still exists; the member it was registered on does not.
	CrowdyMassFields::FFieldBinding MemberGone;
	MemberGone.FragmentType = Registered;
	MemberGone.Property = Registered->FindPropertyByName(TEXT("Charges"));
	MemberGone.FragmentTypePath = Registered->GetStructPathName();
	MemberGone.PropertyName = TEXT("CrowdyMassFieldTestDeletedMember");

	// The fragment type itself is gone, so nothing can be resolved off it at all.
	CrowdyMassFields::FFieldBinding FragmentGone;
	FragmentGone.FragmentType = Registered;
	FragmentGone.Property = Registered->FindPropertyByName(TEXT("Heat"));
	FragmentGone.FragmentTypePath = FTopLevelAssetPath(TEXT("/Script/CrowdyMass"),
		TEXT("CrowdyMassFieldTestFragmentThatWasDeleted"));
	FragmentGone.PropertyName = TEXT("Heat");

	if (!TestTrue(TEXT("both bindings were replaced"),
		CrowdyMassFields::OverwriteFieldBindingForTest(FieldCharges(), MemberGone)
			&& CrowdyMassFields::OverwriteFieldBindingForTest(FieldHeat(), FragmentGone)))
	{
		return false;
	}

	AddExpectedErrorPlain(TEXT("stopped resolving when native code was reloaded"),
		EAutomationExpectedErrorFlags::Contains, 2);

	TestEqual(TEXT("both unresolvable bindings were dropped, and only those two"),
		CrowdyMassFields::RebindRegisteredFields(), 2);

	TestFalse(TEXT("the field whose member is gone is no longer registered"),
		CrowdyMassFields::IsFieldRegistered(FieldCharges()));
	TestFalse(TEXT("nor is the field whose fragment is gone"),
		CrowdyMassFields::IsFieldRegistered(FieldHeat()));

	// The load-bearing half. A rebind that cleared the registry would satisfy every assertion above and this
	// is what separates the two.
	for (const FName Survivor : { FieldAnchor(), FieldStance(), FieldTypedStance(), FieldAlerted() })
	{
		TestTrue(*FString::Printf(TEXT("'%s' still resolves and was left registered"), *Survivor.ToString()),
			CrowdyMassFields::IsFieldRegistered(Survivor));
	}

	// A dropped name is free rather than poisoned, so a game can register it again against whatever carries
	// the field now without being refused as a conflict with a binding that no longer exists.
	TestTrue(TEXT("a dropped name can be registered again"),
		CrowdyMassFields::RegisterField<FCrowdyMassFieldTestFragment>(FieldCharges(), TEXT("Charges")));

	const FMassEntityHandle Entity = Fixture.MakeEntity(/*bWithTestFragment=*/true);
	const FCrowdyMassEntityTarget Target = Fixture.MakeTarget(Entity);

	TestTrue(TEXT("and a write through the re-registered name reports success"),
		UCrowdyMassTargetLibrary::SetFieldInt(Target, FieldCharges(), 8));
	TestEqual(TEXT("and reaches the fragment"), Fixture.Fragment(Entity)->Charges, 8);

	return true;
}

#endif
