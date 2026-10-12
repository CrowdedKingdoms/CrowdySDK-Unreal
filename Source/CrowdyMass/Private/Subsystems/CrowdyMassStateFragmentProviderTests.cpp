#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Assets/CrowdyMassAnimSet.h"
#include "CrowdyMassStateFragmentTestTypes.h"
#include "CrowdyMassStateLogCapture.h"
#include "Fragments/CrowdyMassAnimFragments.h"
#include "MassEntityConfigAsset.h"
#include "MassEntityManager.h"
#include "Replication/State/CrowdyStateCodec.h"
#include "Replication/State/FCrowdyRepLayout.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"
#include "Traits/CrowdyMassAnimationTrait.h"
#include "UObject/EnumProperty.h"
#include "UObject/StrongObjectPtr.h"

// Covers CrowdyMass's own half of ICrowdyEntitySubscriber's state plane: binding a class's replicated
// state property to the pose fragment of an entity that has no participant and no component of its own,
// turning the arriving state VALUE into the row id the fragment holds, and answering the interface's two
// separate questions separately. The router's half (the shared decode scratch buffer, the class-id guard,
// the defer-queue interaction, the world-travel supersede latch) is covered in CrowdySDK.StateFragment.* in
// CrowdyReplication's own test tree, using a subscriber double rather than a real Mass entity.
//
// The property is found by the TYPE it is declared as: the class's animation set names the enum its pose
// rows are values of, and the replicated property of that enum is the one that drives the pose. So a class
// with no set binds nothing, and what a game calls the property is its own business. Several cases below
// exist only to keep that from quietly degrading back into a name match or into "any byte will do".
namespace
{
	constexpr EAutomationTestFlags CrowdyMassStateFragmentTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// A pose id no case below ever produces, written onto an entity before an update so a decode that
	// silently does nothing leaves an assertion failure behind rather than matching the 0 a freshly
	// constructed fragment already holds.
	constexpr uint8 PoisonedPoseIdForStateFragmentTests = 77;

	int32 IndexOfPropertyNamed(const FCrowdyRepLayout& Layout, const TCHAR* Name)
	{
		const FName Wanted(Name);
		for (int32 Index = 0; Index < Layout.Properties.Num(); ++Index)
		{
			const FProperty* Prop = Layout.Properties[Index].Property;
			if (Prop && Prop->GetFName() == Wanted)
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	// A standalone Mass entity manager with no World, mirroring the fixture in
	// View/CrowdyMassViewStateIngestionTests.cpp: PostInitialize constructs relation observers outered to
	// whatever owner the manager was given, and an owner-less manager takes the process down.
	struct FCrowdyMassStateFragmentTestFixture
	{
		TSharedPtr<FMassEntityManager> EntityManager;
		UCrowdyMassEntitySubsystem* Subsystem = nullptr;

		// The animation sets and configs handed to classes below. Held strongly because the trait names its
		// set through a SOFT pointer, which keeps nothing alive: a collection between authoring one of these
		// and resolving it would take the set away and turn every case here into a class with no set.
		TArray<TStrongObjectPtr<UObject>> KeepAlive;

		FCrowdyMassStateFragmentTestFixture()
		{
			EntityManager = MakeShareable(new FMassEntityManager(GetTransientPackage()));

			FMassEntityManagerStorageInitParams InitParams;
			InitParams.Emplace<FMassEntityManager_InitParams_Concurrent>();
			EntityManager->Initialize(InitParams);
			EntityManager->PostInitialize();

			Subsystem = NewObject<UCrowdyMassEntitySubsystem>(GetTransientPackage());
			Subsystem->SetMassEntityManagerForTest(EntityManager.Get());

			// Installs the config suppression with no binding, so a class no case here configures resolves
			// to nothing rather than to whatever animation set the running project happens to ship.
			Subsystem->SetAuthoredEntityConfigForTest(nullptr, nullptr);

			// Initialize() is never called on a subsystem built this way (there is no World subsystem
			// collection to construct it through), so without this the field registrations
			// ApplyDecodedState resolves against are empty and every decoded byte would be silently
			// dropped, which would make every assertion below pass whether or not ingestion actually ran.
			Subsystem->RegisterStandardStateFragmentFieldsForTest();
		}

		~FCrowdyMassStateFragmentTestFixture()
		{
			EntityManager->Deinitialize();
		}

		/**
		 * Gives a class an animation set naming StateEnumName, with one pose row per entry in PoseNames.
		 *
		 * The ORDER of PoseNames is what the resulting ids are: row N answers id N+1. Cases below pass an
		 * order deliberately unlike the enum's own value order, which is what lets them tell a real
		 * value-to-row mapping from an arriving byte copied straight through.
		 */
		void GiveAnimSet(const UClass* EntityClass, const TCHAR* StateEnumName, const TArray<FName>& PoseNames)
		{
			UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);

			// Named rather than assigned: these are enums declared in C++, so they are not assets and the
			// object property beside this one could never hold one.
			AnimSet->PoseStateEnumName = StateEnumName;

			for (const FName& PoseName : PoseNames)
			{
				FCrowdyMassPoseEntry Pose;
				Pose.PoseName = PoseName;
				AnimSet->Poses.Add(Pose);
			}

			UMassEntityConfigAsset* Config = NewObject<UMassEntityConfigAsset>(GetTransientPackage(), NAME_None, RF_Transient);
			UCrowdyMassAnimationTrait* Trait = NewObject<UCrowdyMassAnimationTrait>(Config, NAME_None, RF_Transient);
			Trait->AnimSet = TSoftObjectPtr<UCrowdyMassAnimSet>(AnimSet);
			Config->GetMutableConfig().AddTrait(*Trait);

			KeepAlive.Add(TStrongObjectPtr<UObject>(AnimSet));
			KeepAlive.Add(TStrongObjectPtr<UObject>(Config));

			Subsystem->SetAuthoredEntityConfigForTest(EntityClass, Config);
		}

		// A real entity carrying the pose fragment, tracked under UUID and stamped with StateClass exactly
		// as SpawnRemoteEntity's own bookkeeping would, so GetEntityClass/ApplyDecodedState resolve it.
		// StateClass may be null, which is a real entity with nothing declared for it rather than no entity.
		FMassEntityHandle MakePoseEntity(const FGuid& UUID, const UClass* StateClass)
		{
			const UScriptStruct* Fragments[] = {FCrowdyMassViewPoseFragment::StaticStruct()};
			const FMassArchetypeHandle Archetype = EntityManager->CreateArchetype(MakeArrayView(Fragments, 1));
			const FMassEntityHandle Entity = EntityManager->CreateEntity(Archetype);
			Subsystem->RegisterTrackedEntityForTest(UUID, Entity);
			Subsystem->SetStateEntityClassForTest(UUID, StateClass);
			return Entity;
		}

		FCrowdyMassViewPoseFragment* GetPose(FMassEntityHandle Entity)
		{
			return EntityManager->GetFragmentDataPtr<FCrowdyMassViewPoseFragment>(Entity);
		}
	};
}

// A stance goes on the wire and a ROW ID comes out on the pose fragment of an entity with no participant
// and no component anywhere, driven entirely through GetEntityClass + ApplyDecodedState.
//
// The set authors Mounted first and Dead second, so Mounted (enum value 2) means id 1 and Dead (enum value
// 1) means id 2. Both are asserted, because either alone could be satisfied by a byte copied through: it
// is the pair that can only hold if a mapping ran.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassStateFragmentProviderReceivesPoseTest,
	"CrowdySDK.Mass.StateFragmentProviderReceivesPose", CrowdyMassStateFragmentTestFlags)
bool FCrowdyMassStateFragmentProviderReceivesPoseTest::RunTest(const FString& Parameters)
{
	FCrowdyMassStateFragmentTestFixture Fixture;
	Fixture.GiveAnimSet(UCrowdyMassStateFragmentPoseTarget::StaticClass(),
		TEXT("ECrowdyMassStateFragmentTestStance"), {TEXT("Mounted"), TEXT("Dead")});

	const FGuid UUID = FGuid::NewGuid();
	const FMassEntityHandle Entity = Fixture.MakePoseEntity(UUID, UCrowdyMassStateFragmentPoseTarget::StaticClass());

	FCrowdyMassViewPoseFragment* Pose = Fixture.GetPose(Entity);
	if (!TestNotNull(TEXT("pose fragment present"), Pose))
	{
		return false;
	}
	Pose->PoseId = PoisonedPoseIdForStateFragmentTests;

	// GetEntityClass is the other half of the contract: it must answer the locally-resolved class for this
	// live, participant-less, component-less entity, and the entity must be known independently of it.
	TestTrue(TEXT("the subsystem holds the entity"), Fixture.Subsystem->IsEntityKnown(UUID));
	TestTrue(TEXT("and answers the class it was given at spawn"),
		Fixture.Subsystem->GetEntityClass(UUID) == UCrowdyMassStateFragmentPoseTarget::StaticClass());

	FCrowdyRepLayout Layout;
	if (!TestTrue(TEXT("layout built"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyMassStateFragmentPoseTarget::StaticClass(), Layout)))
	{
		return false;
	}

	UCrowdyMassStateFragmentPoseTarget* Source = NewObject<UCrowdyMassStateFragmentPoseTarget>();
	Source->Stance = ECrowdyMassStateFragmentTestStance::Mounted;
	TArray<uint8> Blob;
	FCrowdyStateCodec::Encode(Layout, Source, TBitArray<>(true, Layout.Properties.Num()), /*bKeyframe=*/true, Blob);

	UCrowdyMassStateFragmentPoseTarget* Decoded = NewObject<UCrowdyMassStateFragmentPoseTarget>();
	TArray<int32> ChangedIndices;
	TArray<int32> PresentIndices;
	if (!TestTrue(TEXT("decode succeeded"),
		FCrowdyStateCodec::Decode(Layout, Layout.LayoutHash, Blob, Decoded, ChangedIndices, &PresentIndices)))
	{
		return false;
	}

	// PRESENT, not changed, which is what UCrowdyEventRouter hands a subscriber and what every case in this
	// file uses. The difference decides whether a pose can ever be CLEARED: a decode target starts at its
	// class defaults, so a state equal to the enum's default reads as unchanged and would never be
	// delivered, and a case driven off the changed list could never show a corpse standing back up.
	Fixture.Subsystem->ApplyDecodedState(UUID, Layout, Decoded, PresentIndices);

	TestEqual(TEXT("Mounted lands as the id of the row that names it, not as its enum value"),
		Pose->PoseId, static_cast<uint8>(1));

	// The second half of the pair, applied directly rather than re-encoded: what is being asserted is the
	// mapping, and the codec already had its say above.
	const int32 StanceIndex = IndexOfPropertyNamed(Layout, TEXT("Stance"));
	if (!TestNotEqual(TEXT("the stance is in the layout"), StanceIndex, static_cast<int32>(INDEX_NONE)))
	{
		return false;
	}

	Decoded->Stance = ECrowdyMassStateFragmentTestStance::Dead;
	Fixture.Subsystem->ApplyDecodedState(UUID, Layout, Decoded, TArray<int32>({StanceIndex}));

	TestEqual(TEXT("and Dead lands as the second row's id, which its enum value of 1 could not have produced"),
		Pose->PoseId, static_cast<uint8>(2));

	return true;
}

// Binding resolves by declared TYPE, not by the layout position the stance happens to occupy on the
// simplest fixture. Inserting a CrowdyState property above it shifts it to a different layout index; a
// positional implementation would read the property above it (an int32) through the pose ingestor's byte
// pointer instead.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassStateFragmentBindingByTypeNotIndexTest,
	"CrowdySDK.Mass.StateFragmentBindingByTypeNotIndex", CrowdyMassStateFragmentTestFlags)
bool FCrowdyMassStateFragmentBindingByTypeNotIndexTest::RunTest(const FString& Parameters)
{
	FCrowdyMassStateFragmentTestFixture Fixture;
	Fixture.GiveAnimSet(UCrowdyMassStateFragmentReorderedPoseTarget::StaticClass(),
		TEXT("ECrowdyMassStateFragmentTestStance"), {TEXT("Mounted"), TEXT("Dead")});

	const FGuid UUID = FGuid::NewGuid();
	const FMassEntityHandle Entity =
		Fixture.MakePoseEntity(UUID, UCrowdyMassStateFragmentReorderedPoseTarget::StaticClass());

	FCrowdyMassViewPoseFragment* Pose = Fixture.GetPose(Entity);
	if (!TestNotNull(TEXT("pose fragment present"), Pose))
	{
		return false;
	}
	Pose->PoseId = PoisonedPoseIdForStateFragmentTests;

	FCrowdyRepLayout Layout;
	if (!TestTrue(TEXT("layout built"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyMassStateFragmentReorderedPoseTarget::StaticClass(), Layout)))
	{
		return false;
	}

	const int32 StanceIndex = IndexOfPropertyNamed(Layout, TEXT("Stance"));
	if (!TestNotEqual(TEXT("the stance is in the layout"), StanceIndex, static_cast<int32>(INDEX_NONE)))
	{
		return false;
	}
	// The whole point of this fixture: the stance does NOT sit at index 0, because SomeOtherState was
	// declared above it. A red proof needs this to hold, or the case would pass under a positional
	// implementation too.
	TestNotEqual(TEXT("the stance is not at layout index 0 in this fixture"), StanceIndex, 0);

	UCrowdyMassStateFragmentReorderedPoseTarget* Source = NewObject<UCrowdyMassStateFragmentReorderedPoseTarget>();
	// A value that would land on the pose fragment (truncated to its low byte) if binding were positional
	// rather than by type.
	Source->SomeOtherState = 999;
	Source->Stance = ECrowdyMassStateFragmentTestStance::Dead;
	TArray<uint8> Blob;
	FCrowdyStateCodec::Encode(Layout, Source, TBitArray<>(true, Layout.Properties.Num()), /*bKeyframe=*/true, Blob);

	UCrowdyMassStateFragmentReorderedPoseTarget* Decoded = NewObject<UCrowdyMassStateFragmentReorderedPoseTarget>();
	TArray<int32> ChangedIndices;
	TArray<int32> PresentIndices;
	if (!TestTrue(TEXT("decode succeeded"),
		FCrowdyStateCodec::Decode(Layout, Layout.LayoutHash, Blob, Decoded, ChangedIndices, &PresentIndices)))
	{
		return false;
	}

	Fixture.Subsystem->ApplyDecodedState(UUID, Layout, Decoded, PresentIndices);

	TestEqual(TEXT("the pose fragment landed the stance's own row id, not the property declared above it"),
		Pose->PoseId, static_cast<uint8>(2));

	return true;
}

// A class that declares its replicated stance OwnerOnly logs the diagnostic once per (class, layout hash)
// resolution, not once per delta: the resolution is cached, so a second entity of the same class must
// reuse it rather than re-trigger the warning.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassStateFragmentOwnerOnlyWarnsOnceTest,
	"CrowdySDK.Mass.StateFragmentOwnerOnlyWarnsOnce", CrowdyMassStateFragmentTestFlags)
bool FCrowdyMassStateFragmentOwnerOnlyWarnsOnceTest::RunTest(const FString& Parameters)
{
	FCrowdyMassStateFragmentTestFixture Fixture;
	Fixture.GiveAnimSet(UCrowdyMassStateFragmentOwnerOnlyPoseTarget::StaticClass(),
		TEXT("ECrowdyMassStateFragmentTestStance"), {TEXT("Dead")});

	FCrowdyRepLayout Layout;
	if (!TestTrue(TEXT("layout built"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyMassStateFragmentOwnerOnlyPoseTarget::StaticClass(), Layout)))
	{
		return false;
	}

	const int32 StanceIndex = IndexOfPropertyNamed(Layout, TEXT("Stance"));
	if (!TestNotEqual(TEXT("the stance is in the layout"), StanceIndex, static_cast<int32>(INDEX_NONE)))
	{
		return false;
	}

	const FGuid FirstUUID = FGuid::NewGuid();
	const FGuid SecondUUID = FGuid::NewGuid();
	const FMassEntityHandle FirstEntity =
		Fixture.MakePoseEntity(FirstUUID, UCrowdyMassStateFragmentOwnerOnlyPoseTarget::StaticClass());
	Fixture.MakePoseEntity(SecondUUID, UCrowdyMassStateFragmentOwnerOnlyPoseTarget::StaticClass());

	FCrowdyMassViewPoseFragment* FirstPose = Fixture.GetPose(FirstEntity);
	if (!TestNotNull(TEXT("pose fragment present"), FirstPose))
	{
		return false;
	}
	FirstPose->PoseId = PoisonedPoseIdForStateFragmentTests;

	// Exact-count expectation (not "at least once"): if the resolution cache ever stopped being reused
	// across entities of the same class, this fires twice and the test goes red.
	AddExpectedError(TEXT("declares its CrowdyState property 'Stance' owner-only"),
		EAutomationExpectedErrorFlags::Contains, 1);

	UCrowdyMassStateFragmentOwnerOnlyPoseTarget* Decoded = NewObject<UCrowdyMassStateFragmentOwnerOnlyPoseTarget>();
	Decoded->Stance = ECrowdyMassStateFragmentTestStance::Dead;
	TArray<int32> ChangedIndices;
	TArray<int32> PresentIndices;
	ChangedIndices.Add(StanceIndex);

	Fixture.Subsystem->ApplyDecodedState(FirstUUID, Layout, Decoded, ChangedIndices);
	Fixture.Subsystem->ApplyDecodedState(SecondUUID, Layout, Decoded, ChangedIndices);

	// The warning says the ingestor is a no-op in practice, NOT that the field was refused, and the
	// production comment makes that an explicit promise: binding still happens in case the owner-skip is
	// ever overridden. Asserted, because without this the whole case would stay green under a build that
	// turned the diagnostic into a refusal, which is the obvious next edit somebody makes to it.
	TestEqual(TEXT("the field is still BOUND: an owner-only property that does arrive is applied"),
		FirstPose->PoseId, static_cast<uint8>(1));

	return true;
}

/**
 * A sender cannot name a pose the receiving client has not authored, which is what moving the wire from a
 * row id to a state value bought.
 *
 * Under the old contract the sender chose the id outright, so a forged one could only be caught downstream
 * where the authored range is known. Now every id on the fragment comes out of this client's own authored
 * table: a value no row names answers 0 and clears the pose. There is no arriving byte that produces an id
 * this client did not author, so the whole forged-id question stops existing at this point rather than
 * being handled here.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassStateFragmentForgedStateCannotNameAnUnauthoredPoseTest,
	"CrowdySDK.Mass.StateFragmentForgedStateCannotNameAnUnauthoredPose", CrowdyMassStateFragmentTestFlags)
bool FCrowdyMassStateFragmentForgedStateCannotNameAnUnauthoredPoseTest::RunTest(const FString& Parameters)
{
	FCrowdyMassStateFragmentTestFixture Fixture;
	Fixture.GiveAnimSet(UCrowdyMassStateFragmentPoseTarget::StaticClass(),
		TEXT("ECrowdyMassStateFragmentTestStance"), {TEXT("Dead")});

	const FGuid UUID = FGuid::NewGuid();
	const FMassEntityHandle Entity = Fixture.MakePoseEntity(UUID, UCrowdyMassStateFragmentPoseTarget::StaticClass());

	FCrowdyMassViewPoseFragment* Pose = Fixture.GetPose(Entity);
	if (!TestNotNull(TEXT("pose fragment present"), Pose))
	{
		return false;
	}

	FCrowdyRepLayout Layout;
	if (!TestTrue(TEXT("layout built"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyMassStateFragmentPoseTarget::StaticClass(), Layout)))
	{
		return false;
	}

	const int32 StanceIndex = IndexOfPropertyNamed(Layout, TEXT("Stance"));
	if (!TestNotEqual(TEXT("the stance is in the layout"), StanceIndex, static_cast<int32>(INDEX_NONE)))
	{
		return false;
	}

	UCrowdyMassStateFragmentPoseTarget* Decoded = NewObject<UCrowdyMassStateFragmentPoseTarget>();
	const TArray<int32> PresentIndices({StanceIndex});

	// The control first, so the refusal below cannot be this path simply never running.
	Decoded->Stance = ECrowdyMassStateFragmentTestStance::Dead;
	Fixture.Subsystem->ApplyDecodedState(UUID, Layout, Decoded, PresentIndices);
	TestEqual(TEXT("an authored stance lands its row"), Pose->PoseId, static_cast<uint8>(1));

	// Far outside anything this enum declares, which is exactly what a forged byte looks like: the wire
	// carries a byte and nothing constrains it to an entry the sender's own build knows about.
	Decoded->Stance = static_cast<ECrowdyMassStateFragmentTestStance>(250);
	Fixture.Subsystem->ApplyDecodedState(UUID, Layout, Decoded, PresentIndices);

	TestEqual(TEXT("a state no row authors clears the pose rather than naming one"),
		Pose->PoseId, static_cast<uint8>(0));

	return true;
}

/**
 * TEnumAsByte works too, which the SDK promises in three places and nothing proved until this case.
 *
 * It reaches everything down a different branch from every other fixture here: it reflects as a plain byte
 * property CARRYING the enum rather than as an enum property WRAPPING a byte, so the type match reads the
 * enum off a different field, and the wire encoder cannot defer to an underlying property because there
 * isn't one. That second half is not hypothetical: deferring to the byte's own SerializeItem walks straight
 * back into the by-name encoding, and this case is what says so.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassStateFragmentEnumAsByteTest,
	"CrowdySDK.Mass.StateFragmentReadsATEnumAsByteProperty", CrowdyMassStateFragmentTestFlags)
bool FCrowdyMassStateFragmentEnumAsByteTest::RunTest(const FString& Parameters)
{
	FCrowdyMassStateFragmentTestFixture Fixture;

	// Harnessed is value 2 and row 0, so id 1; Mounted is value 1 and row 1, so id 2. Neither id equals its
	// value, which is what keeps this from passing on a byte written straight through.
	Fixture.GiveAnimSet(UCrowdyMassStateFragmentEnumAsByteTarget::StaticClass(),
		TEXT("ECrowdyMassStateFragmentTestMount"), {TEXT("Harnessed"), TEXT("Mounted")});

	const FGuid UUID = FGuid::NewGuid();
	const FMassEntityHandle Entity =
		Fixture.MakePoseEntity(UUID, UCrowdyMassStateFragmentEnumAsByteTarget::StaticClass());

	FCrowdyMassViewPoseFragment* Pose = Fixture.GetPose(Entity);
	if (!TestNotNull(TEXT("pose fragment present"), Pose))
	{
		return false;
	}
	Pose->PoseId = PoisonedPoseIdForStateFragmentTests;

	FCrowdyRepLayout Layout;
	if (!TestTrue(TEXT("layout built"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyMassStateFragmentEnumAsByteTarget::StaticClass(), Layout)))
	{
		return false;
	}

	const int32 MountIndex = IndexOfPropertyNamed(Layout, TEXT("Mount"));
	if (!TestNotEqual(TEXT("the mount is in the layout"), MountIndex, static_cast<int32>(INDEX_NONE)))
	{
		return false;
	}

	// The fixture really is the other declaration form, or this case would be re-testing its sibling.
	TestNull(TEXT("and it is a plain byte property, not an enum property"),
		CastField<FEnumProperty>(Layout.Properties[MountIndex].Property));

	UCrowdyMassStateFragmentEnumAsByteTarget* Source = NewObject<UCrowdyMassStateFragmentEnumAsByteTarget>();
	Source->Mount = ECrowdyMassStateFragmentTestMount::Mounted;
	TArray<uint8> Blob;
	FCrowdyStateCodec::Encode(Layout, Source, TBitArray<>(true, Layout.Properties.Num()), /*bKeyframe=*/true, Blob);

	UCrowdyMassStateFragmentEnumAsByteTarget* Decoded = NewObject<UCrowdyMassStateFragmentEnumAsByteTarget>();
	TArray<int32> ChangedIndices;
	TArray<int32> PresentIndices;
	if (!TestTrue(TEXT("decode succeeded"),
		FCrowdyStateCodec::Decode(Layout, Layout.LayoutHash, Blob, Decoded, ChangedIndices, &PresentIndices)))
	{
		return false;
	}

	// One CrowdyState property, so this is the whole field cost. A TEnumAsByte encoded by name would be
	// dozens of bytes here, exactly as the enum-class form was before the codec carried values.
	TestEqual(TEXT("a TEnumAsByte costs the same on the wire as the byte it is"), Blob.Num(), 4);

	Fixture.Subsystem->ApplyDecodedState(UUID, Layout, Decoded, PresentIndices);

	TestEqual(TEXT("and Mounted selects the row that names it rather than its own value"),
		Pose->PoseId, static_cast<uint8>(2));

	return true;
}

/**
 * A byte-wide enum that is not the one the set names is not a pose.
 *
 * Matching on the declared type has to mean the SPECIFIC enum, not "an enum that fits in a byte". A game
 * replicates plenty of small states; taking an unrelated one for a pose would drive the crowd's appearance
 * off whatever value happened to be a byte, and it would look like a working feature showing wrong clips.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassStateFragmentIgnoresAnotherEnumTest,
	"CrowdySDK.Mass.StateFragmentIgnoresAPropertyOfAnotherEnum", CrowdyMassStateFragmentTestFlags)
bool FCrowdyMassStateFragmentIgnoresAnotherEnumTest::RunTest(const FString& Parameters)
{
	FCrowdyMassStateFragmentTestFixture Fixture;

	// The set names the stance enum; the class replicates a mood. Both are byte-wide enums.
	Fixture.GiveAnimSet(UCrowdyMassStateFragmentOtherEnumTarget::StaticClass(),
		TEXT("ECrowdyMassStateFragmentTestStance"), {TEXT("Dead")});

	const FGuid UUID = FGuid::NewGuid();
	const FMassEntityHandle Entity =
		Fixture.MakePoseEntity(UUID, UCrowdyMassStateFragmentOtherEnumTarget::StaticClass());

	FCrowdyMassViewPoseFragment* Pose = Fixture.GetPose(Entity);
	if (!TestNotNull(TEXT("pose fragment present"), Pose))
	{
		return false;
	}
	Pose->PoseId = PoisonedPoseIdForStateFragmentTests;

	FCrowdyRepLayout Layout;
	if (!TestTrue(TEXT("layout built"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyMassStateFragmentOtherEnumTarget::StaticClass(), Layout)))
	{
		return false;
	}

	const int32 MoodIndex = IndexOfPropertyNamed(Layout, TEXT("Mood"));
	if (!TestNotEqual(TEXT("the mood is in the layout"), MoodIndex, static_cast<int32>(INDEX_NONE)))
	{
		return false;
	}

	// The class carries nothing this build can pose from, and that is worth saying out loud rather than
	// leaving as a crowd that never moves.
	AddExpectedError(TEXT("carries no CrowdyState property declared as"),
		EAutomationExpectedErrorFlags::Contains, 1);

	UCrowdyMassStateFragmentOtherEnumTarget* Decoded = NewObject<UCrowdyMassStateFragmentOtherEnumTarget>();
	Decoded->Mood = ECrowdyMassStateFragmentTestMood::Angry;
	Fixture.Subsystem->ApplyDecodedState(UUID, Layout, Decoded, TArray<int32>({MoodIndex}));

	TestEqual(TEXT("an unrelated enum is not taken for a pose"),
		Pose->PoseId, PoisonedPoseIdForStateFragmentTests);

	return true;
}

/**
 * The old contract is gone, and its removal is asserted rather than assumed.
 *
 * A class used to declare a raw uint8 called exactly PoseId, carrying a row id the SENDER resolved. Both
 * halves of that are now wrong: the name means nothing, and a sender-chosen row id is exactly what this
 * phase stopped trusting. Such a property must bind nothing at all, or a game half-migrated would have two
 * roads writing the same byte with values that mean different things.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassStateFragmentIgnoresALegacyPoseIdByteTest,
	"CrowdySDK.Mass.StateFragmentIgnoresALegacyPoseIdByte", CrowdyMassStateFragmentTestFlags)
bool FCrowdyMassStateFragmentIgnoresALegacyPoseIdByteTest::RunTest(const FString& Parameters)
{
	FCrowdyMassStateFragmentTestFixture Fixture;
	Fixture.GiveAnimSet(UCrowdyMassStateFragmentLegacyByteTarget::StaticClass(),
		TEXT("ECrowdyMassStateFragmentTestStance"), {TEXT("Dead")});

	const FGuid UUID = FGuid::NewGuid();
	const FMassEntityHandle Entity =
		Fixture.MakePoseEntity(UUID, UCrowdyMassStateFragmentLegacyByteTarget::StaticClass());

	FCrowdyMassViewPoseFragment* Pose = Fixture.GetPose(Entity);
	if (!TestNotNull(TEXT("pose fragment present"), Pose))
	{
		return false;
	}
	Pose->PoseId = PoisonedPoseIdForStateFragmentTests;

	FCrowdyRepLayout Layout;
	if (!TestTrue(TEXT("layout built"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyMassStateFragmentLegacyByteTarget::StaticClass(), Layout)))
	{
		return false;
	}

	const int32 PoseIdIndex = IndexOfPropertyNamed(Layout, TEXT("PoseId"));
	if (!TestNotEqual(TEXT("the legacy byte is in the layout"), PoseIdIndex, static_cast<int32>(INDEX_NONE)))
	{
		return false;
	}

	AddExpectedError(TEXT("carries no CrowdyState property declared as"),
		EAutomationExpectedErrorFlags::Contains, 1);

	UCrowdyMassStateFragmentLegacyByteTarget* Decoded = NewObject<UCrowdyMassStateFragmentLegacyByteTarget>();
	Decoded->PoseId = 42;
	Fixture.Subsystem->ApplyDecodedState(UUID, Layout, Decoded, TArray<int32>({PoseIdIndex}));

	TestEqual(TEXT("a byte named PoseId no longer drives the pose"),
		Pose->PoseId, PoisonedPoseIdForStateFragmentTests);

	return true;
}

/**
 * A game's own field taking the pose property is reported as that, not as a class with no pose property.
 *
 * Name-matched fields are offered every property before the pose field is, so a game that registers a field
 * for the very property it poses from takes that slot and its crowd silently stops posing. The property is
 * still declared, still marked, and still of the enum the set names, so the "carries no CrowdyState property
 * declared as X" line would be false, and following it means declaring a second property of the same enum,
 * which then draws a second false line calling the game's own property one that is never read.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassStateFragmentPoseTakenByANameFieldTest,
	"CrowdySDK.Mass.StateFragmentPoseTakenByANameFieldSaysSo", CrowdyMassStateFragmentTestFlags)
bool FCrowdyMassStateFragmentPoseTakenByANameFieldTest::RunTest(const FString& Parameters)
{
	FCrowdyMassStateFragmentTestFixture Fixture;
	Fixture.GiveAnimSet(UCrowdyMassStateFragmentPoseTarget::StaticClass(),
		TEXT("ECrowdyMassStateFragmentTestStance"), {TEXT("Normal"), TEXT("Crouched")});

	// The game registers a field under the pose property's own name, which is the ordinary way it would
	// mirror that byte into a fragment of its own.
	Fixture.Subsystem->RegisterStateFragmentField(TEXT("Stance"), ECrowdyMassStateFieldType::UInt8,
		[](const void*, const FCrowdyMassViewStateContext&) {});

	const FGuid UUID = FGuid::NewGuid();
	const FMassEntityHandle Entity =
		Fixture.MakePoseEntity(UUID, UCrowdyMassStateFragmentPoseTarget::StaticClass());

	FCrowdyMassViewPoseFragment* Pose = Fixture.GetPose(Entity);
	if (!TestNotNull(TEXT("pose fragment present"), Pose))
	{
		return false;
	}
	Pose->PoseId = PoisonedPoseIdForStateFragmentTests;

	FCrowdyRepLayout Layout;
	if (!TestTrue(TEXT("layout built"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyMassStateFragmentPoseTarget::StaticClass(), Layout)))
	{
		return false;
	}

	const int32 StanceIndex = IndexOfPropertyNamed(Layout, TEXT("Stance"));
	if (!TestNotEqual(TEXT("the stance property is in the layout"), StanceIndex, static_cast<int32>(INDEX_NONE)))
	{
		return false;
	}

	// The capture takes the warning rather than AddExpectedError, which would consume it before this test
	// could read it and leave the assertions below passing against an empty list.
	FCrowdyMassStateLogCapture Capture;
	UCrowdyMassStateFragmentPoseTarget* Decoded = NewObject<UCrowdyMassStateFragmentPoseTarget>();
	Decoded->Stance = ECrowdyMassStateFragmentTestStance::Normal;
	Fixture.Subsystem->ApplyDecodedState(UUID, Layout, Decoded, TArray<int32>({StanceIndex}));

	const TArray<FString> Warnings = Capture.TakeWarnings();

	TestEqual(TEXT("the class is never told it carries no property of the enum, which it plainly does"),
		CrowdyMassCountLinesContaining(Warnings, TEXT("carries no CrowdyState property declared as")), 0);
	TestEqual(TEXT("and it is told which name took the slot, so the remedy is on the registration"),
		CrowdyMassCountLinesContaining(Warnings, TEXT("'Stance'")), 1);

	TestEqual(TEXT("nothing posed the entity, which is the symptom the line has to explain"),
		Pose->PoseId, PoisonedPoseIdForStateFragmentTests);

	return true;
}

/**
 * An enum wider than a byte is still refused, and refused loudly.
 *
 * The point of accepting the enum form is that it is one byte at the same address, so the acceptance has
 * to be about the width and not about the declaration being an enum. This class's set names the wide enum,
 * so the property IS the one meant to drive the pose and is refused anyway: reading its low byte would
 * answer a real pose id for a value that never fits, selecting whichever row that byte happened to name.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassStateFragmentWideEnumRefusedTest,
	"CrowdySDK.Mass.StateFragmentRefusesAWiderThanByteEnumProperty", CrowdyMassStateFragmentTestFlags)
bool FCrowdyMassStateFragmentWideEnumRefusedTest::RunTest(const FString& Parameters)
{
	FCrowdyMassStateFragmentTestFixture Fixture;
	Fixture.GiveAnimSet(UCrowdyMassStateFragmentWideEnumPoseTarget::StaticClass(),
		TEXT("ECrowdyMassStateFragmentTestWideStance"), {TEXT("Distant")});

	const FGuid UUID = FGuid::NewGuid();
	const FMassEntityHandle Entity =
		Fixture.MakePoseEntity(UUID, UCrowdyMassStateFragmentWideEnumPoseTarget::StaticClass());

	FCrowdyMassViewPoseFragment* Pose = Fixture.GetPose(Entity);
	if (!TestNotNull(TEXT("pose fragment present"), Pose))
	{
		return false;
	}
	Pose->PoseId = PoisonedPoseIdForStateFragmentTests;

	FCrowdyRepLayout Layout;
	if (!TestTrue(TEXT("layout built"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyMassStateFragmentWideEnumPoseTarget::StaticClass(), Layout)))
	{
		return false;
	}

	const int32 StanceIndex = IndexOfPropertyNamed(Layout, TEXT("Stance"));
	if (!TestNotEqual(TEXT("the wide enum property is in the layout"), StanceIndex, static_cast<int32>(INDEX_NONE)))
	{
		return false;
	}

	AddExpectedError(TEXT("underlying type is wider than a byte"), EAutomationExpectedErrorFlags::Contains, 1);

	// And the class is reported as carrying no usable pose property, because the refusal above leaves the
	// pose field unclaimed rather than consumed. Both lines are wanted: one says why this property was
	// rejected, the other says the class ended up with nothing.
	AddExpectedError(TEXT("carries no CrowdyState property declared as"),
		EAutomationExpectedErrorFlags::Contains, 1);

	UCrowdyMassStateFragmentWideEnumPoseTarget* Decoded = NewObject<UCrowdyMassStateFragmentWideEnumPoseTarget>();
	Decoded->Stance = ECrowdyMassStateFragmentTestWideStance::Distant;
	Fixture.Subsystem->ApplyDecodedState(UUID, Layout, Decoded, TArray<int32>({StanceIndex}));

	TestEqual(TEXT("nothing is written for a state a byte cannot carry"),
		Pose->PoseId, PoisonedPoseIdForStateFragmentTests);

	return true;
}

/**
 * A property refused for its width does not consume the pose field on its way out.
 *
 * The field is claimed by TYPE, so several properties can match one enum, and a claim taken before the
 * width check would be spent by a property that was then rejected. Everything after it of the same enum
 * becomes unreachable for the life of the world, decided by nothing more than declaration order, and the
 * ambiguity report then names the REJECTED property as the one driving the pose: two statements that are
 * both false, pointing an author at the wrong property to fix.
 *
 * Asserted through the reports rather than through the fragment, because with both properties refused the
 * fragment is untouched either way. What separates the two builds is what gets SAID.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassStateFragmentRefusedWidthDoesNotConsumeTheFieldTest,
	"CrowdySDK.Mass.StateFragmentRefusedWidthDoesNotConsumeTheField", CrowdyMassStateFragmentTestFlags)
bool FCrowdyMassStateFragmentRefusedWidthDoesNotConsumeTheFieldTest::RunTest(const FString& Parameters)
{
	FCrowdyMassStateFragmentTestFixture Fixture;
	Fixture.GiveAnimSet(UCrowdyMassStateFragmentTwoWideEnumTarget::StaticClass(),
		TEXT("ECrowdyMassStateFragmentTestWideStance"), {TEXT("Distant")});

	const FGuid UUID = FGuid::NewGuid();
	const FMassEntityHandle Entity =
		Fixture.MakePoseEntity(UUID, UCrowdyMassStateFragmentTwoWideEnumTarget::StaticClass());

	FCrowdyMassViewPoseFragment* Pose = Fixture.GetPose(Entity);
	if (!TestNotNull(TEXT("pose fragment present"), Pose))
	{
		return false;
	}
	Pose->PoseId = PoisonedPoseIdForStateFragmentTests;

	FCrowdyRepLayout Layout;
	if (!TestTrue(TEXT("layout built"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyMassStateFragmentTwoWideEnumTarget::StaticClass(), Layout)))
	{
		return false;
	}

	const int32 StanceIndex = IndexOfPropertyNamed(Layout, TEXT("Stance"));
	const int32 SecondStanceIndex = IndexOfPropertyNamed(Layout, TEXT("SecondStance"));
	if (!TestNotEqual(TEXT("the first wide property is in the layout"), StanceIndex, static_cast<int32>(INDEX_NONE))
		|| !TestNotEqual(TEXT("and so is the second"), SecondStanceIndex, static_cast<int32>(INDEX_NONE)))
	{
		return false;
	}

	// BOTH are refused for their width, exactly two lines. A build that spent the claim on the first one
	// reports only one, because the second never matches an unclaimed field to be refused by.
	AddExpectedError(TEXT("underlying type is wider than a byte"), EAutomationExpectedErrorFlags::Contains, 2);

	// And the class honestly reports having nothing to pose from. A build that spent the claim reports the
	// field as satisfied and stays silent here.
	AddExpectedError(TEXT("carries no CrowdyState property declared as"),
		EAutomationExpectedErrorFlags::Contains, 1);

	UCrowdyMassStateFragmentTwoWideEnumTarget* Decoded = NewObject<UCrowdyMassStateFragmentTwoWideEnumTarget>();
	Decoded->Stance = ECrowdyMassStateFragmentTestWideStance::Distant;
	Decoded->SecondStance = ECrowdyMassStateFragmentTestWideStance::Distant;
	Fixture.Subsystem->ApplyDecodedState(UUID, Layout, Decoded, TArray<int32>({StanceIndex, SecondStanceIndex}));

	TestEqual(TEXT("and nothing is written, since neither property can be read as a byte"),
		Pose->PoseId, PoisonedPoseIdForStateFragmentTests);

	return true;
}

/**
 * Two replicated properties of the same enum are reported, not silently resolved by declaration order.
 *
 * One set names one enum, so both properties are asking for the same rows and only the earlier one can
 * win. Which one that is has nothing to do with what the author meant, so leaving it unsaid would make a
 * crowd driven off the wrong half of a class look like a broken pose rather than an ambiguous declaration.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassStateFragmentTwoStancesReportedTest,
	"CrowdySDK.Mass.StateFragmentTwoStancePropertiesReported", CrowdyMassStateFragmentTestFlags)
bool FCrowdyMassStateFragmentTwoStancesReportedTest::RunTest(const FString& Parameters)
{
	FCrowdyMassStateFragmentTestFixture Fixture;
	Fixture.GiveAnimSet(UCrowdyMassStateFragmentTwoStanceTarget::StaticClass(),
		TEXT("ECrowdyMassStateFragmentTestStance"), {TEXT("Mounted"), TEXT("Dead")});

	const FGuid UUID = FGuid::NewGuid();
	const FMassEntityHandle Entity =
		Fixture.MakePoseEntity(UUID, UCrowdyMassStateFragmentTwoStanceTarget::StaticClass());

	FCrowdyMassViewPoseFragment* Pose = Fixture.GetPose(Entity);
	if (!TestNotNull(TEXT("pose fragment present"), Pose))
	{
		return false;
	}
	Pose->PoseId = PoisonedPoseIdForStateFragmentTests;

	FCrowdyRepLayout Layout;
	if (!TestTrue(TEXT("layout built"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyMassStateFragmentTwoStanceTarget::StaticClass(), Layout)))
	{
		return false;
	}

	const int32 StanceIndex = IndexOfPropertyNamed(Layout, TEXT("Stance"));
	const int32 SecondStanceIndex = IndexOfPropertyNamed(Layout, TEXT("SecondStance"));
	if (!TestNotEqual(TEXT("the first stance is in the layout"), StanceIndex, static_cast<int32>(INDEX_NONE))
		|| !TestNotEqual(TEXT("and so is the second"), SecondStanceIndex, static_cast<int32>(INDEX_NONE)))
	{
		return false;
	}

	AddExpectedError(TEXT("declares both 'Stance' and 'SecondStance'"),
		EAutomationExpectedErrorFlags::Contains, 1);

	UCrowdyMassStateFragmentTwoStanceTarget* Decoded = NewObject<UCrowdyMassStateFragmentTwoStanceTarget>();
	Decoded->Stance = ECrowdyMassStateFragmentTestStance::Mounted;
	Decoded->SecondStance = ECrowdyMassStateFragmentTestStance::Dead;
	Fixture.Subsystem->ApplyDecodedState(UUID, Layout, Decoded, TArray<int32>({StanceIndex, SecondStanceIndex}));

	// The one that was reported as winning is the one that actually wrote: Mounted is row 0, id 1, and the
	// loser would have written Dead's id of 2.
	TestEqual(TEXT("the property named in the warning is the one that drove the pose"),
		Pose->PoseId, static_cast<uint8>(1));

	return true;
}

/**
 * A class that names no animation set binds nothing, and says which of the several reasons applies.
 *
 * This is the ordinary first-run failure: the property is declared and marked correctly and the pose still
 * never arrives, because nothing on the receiving side knows which enum to look for. It has to be
 * distinguishable from a set that failed to load and from a set naming no enum, since all three end with a
 * crowd that never poses and each is fixed somewhere else.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassStateFragmentNoAnimSetReportedTest,
	"CrowdySDK.Mass.StateFragmentNoAnimSetReported", CrowdyMassStateFragmentTestFlags)
bool FCrowdyMassStateFragmentNoAnimSetReportedTest::RunTest(const FString& Parameters)
{
	FCrowdyMassStateFragmentTestFixture Fixture;

	// Deliberately no GiveAnimSet call: the class declares its stance and nothing names a set for it.
	const FGuid UUID = FGuid::NewGuid();
	const FMassEntityHandle Entity = Fixture.MakePoseEntity(UUID, UCrowdyMassStateFragmentPoseTarget::StaticClass());

	FCrowdyMassViewPoseFragment* Pose = Fixture.GetPose(Entity);
	if (!TestNotNull(TEXT("pose fragment present"), Pose))
	{
		return false;
	}
	Pose->PoseId = PoisonedPoseIdForStateFragmentTests;

	FCrowdyRepLayout Layout;
	if (!TestTrue(TEXT("layout built"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyMassStateFragmentPoseTarget::StaticClass(), Layout)))
	{
		return false;
	}

	const int32 StanceIndex = IndexOfPropertyNamed(Layout, TEXT("Stance"));
	if (!TestNotEqual(TEXT("the stance is in the layout"), StanceIndex, static_cast<int32>(INDEX_NONE)))
	{
		return false;
	}

	// Named as its own fault, with its own remedy, rather than as "no pose for this class".
	AddExpectedError(TEXT("names no animation set, so there is no enum to recognise its replicated pose state by"),
		EAutomationExpectedErrorFlags::Contains, 1);

	UCrowdyMassStateFragmentPoseTarget* Decoded = NewObject<UCrowdyMassStateFragmentPoseTarget>();
	Decoded->Stance = ECrowdyMassStateFragmentTestStance::Dead;
	Fixture.Subsystem->ApplyDecodedState(UUID, Layout, Decoded, TArray<int32>({StanceIndex}));

	TestEqual(TEXT("and nothing is written, since no value can be given a meaning"),
		Pose->PoseId, PoisonedPoseIdForStateFragmentTests);

	return true;
}

/**
 * The class-to-property binding is dropped when the animation set behind it changes.
 *
 * Which property carries a class's pose is decided by the enum that class's SET names, so the resolution
 * cached here is made of the set as much as of the class. Nothing else would ever drop it: the entry is
 * keyed by class and layout hash, and neither moves when an asset is authored, replaced or reloaded. So
 * without that drop, adding the set a warning has just asked for changes nothing for the life of the
 * world, which is the exact shape the S1 review named and the one a warning-then-fix loop walks straight
 * into.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassStateFragmentBindingDroppedWhenAnimSetChangesTest,
	"CrowdySDK.Mass.StateFragmentBindingDroppedWhenAnimSetChanges", CrowdyMassStateFragmentTestFlags)
bool FCrowdyMassStateFragmentBindingDroppedWhenAnimSetChangesTest::RunTest(const FString& Parameters)
{
	FCrowdyMassStateFragmentTestFixture Fixture;

	const FGuid UUID = FGuid::NewGuid();
	const FMassEntityHandle Entity = Fixture.MakePoseEntity(UUID, UCrowdyMassStateFragmentPoseTarget::StaticClass());

	FCrowdyMassViewPoseFragment* Pose = Fixture.GetPose(Entity);
	if (!TestNotNull(TEXT("pose fragment present"), Pose))
	{
		return false;
	}
	Pose->PoseId = PoisonedPoseIdForStateFragmentTests;

	FCrowdyRepLayout Layout;
	if (!TestTrue(TEXT("layout built"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyMassStateFragmentPoseTarget::StaticClass(), Layout)))
	{
		return false;
	}

	const int32 StanceIndex = IndexOfPropertyNamed(Layout, TEXT("Stance"));
	if (!TestNotEqual(TEXT("the stance is in the layout"), StanceIndex, static_cast<int32>(INDEX_NONE)))
	{
		return false;
	}

	UCrowdyMassStateFragmentPoseTarget* Decoded = NewObject<UCrowdyMassStateFragmentPoseTarget>();
	Decoded->Stance = ECrowdyMassStateFragmentTestStance::Dead;
	const TArray<int32> PresentIndices({StanceIndex});

	// First, with no set at all, which both resolves and CACHES the "nothing binds" answer.
	AddExpectedError(TEXT("names no animation set, so there is no enum to recognise its replicated pose state by"),
		EAutomationExpectedErrorFlags::Contains, 1);
	Fixture.Subsystem->ApplyDecodedState(UUID, Layout, Decoded, PresentIndices);
	TestEqual(TEXT("nothing binds while the class names no set"),
		Pose->PoseId, PoisonedPoseIdForStateFragmentTests);

	// Then the remedy that warning asks for, with nothing else changed: same class, same layout, same
	// delta.
	Fixture.GiveAnimSet(UCrowdyMassStateFragmentPoseTarget::StaticClass(),
		TEXT("ECrowdyMassStateFragmentTestStance"), {TEXT("Mounted"), TEXT("Dead")});

	Fixture.Subsystem->ApplyDecodedState(UUID, Layout, Decoded, PresentIndices);

	TestEqual(TEXT("and the same delta now lands, so the cached miss was dropped rather than kept"),
		Pose->PoseId, static_cast<uint8>(2));

	return true;
}

/**
 * A decoded delta is refused when its layout describes a different class from the one this entity was
 * activated as, even though the router would never normally send such a pair.
 *
 * The guard exists because the router and this subsystem agree about the class across two files and nothing
 * in the arguments proves it. Every property offset below is applied to memory shaped like the layout's
 * class, so a layout for another class reads this entity's bytes at the wrong offsets and writes the results
 * into its fragments: silent value corruption rather than a miss, and the worse of the two by far, because a
 * miss shows up as nothing happening while corruption shows up as a plausible wrong value.
 *
 * The two fixture classes here both carry a stance at DIFFERENT layout positions, so a mismatched apply
 * would land something rather than harmlessly finding nothing: that is what makes the refusal worth
 * asserting rather than assuming.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassStateFragmentClassMismatchRefusedTest,
	"CrowdySDK.Mass.StateFragmentClassMismatchRefused", CrowdyMassStateFragmentTestFlags)
bool FCrowdyMassStateFragmentClassMismatchRefusedTest::RunTest(const FString& Parameters)
{
	FCrowdyMassStateFragmentTestFixture Fixture;
	Fixture.GiveAnimSet(UCrowdyMassStateFragmentPoseTarget::StaticClass(),
		TEXT("ECrowdyMassStateFragmentTestStance"), {TEXT("Mounted"), TEXT("Dead")});
	Fixture.GiveAnimSet(UCrowdyMassStateFragmentReorderedPoseTarget::StaticClass(),
		TEXT("ECrowdyMassStateFragmentTestStance"), {TEXT("Mounted"), TEXT("Dead")});

	const FGuid UUID = FGuid::NewGuid();

	// Activated as one class...
	const FMassEntityHandle Entity = Fixture.MakePoseEntity(UUID, UCrowdyMassStateFragmentPoseTarget::StaticClass());

	FCrowdyMassViewPoseFragment* Pose = Fixture.GetPose(Entity);
	if (!TestNotNull(TEXT("pose fragment present"), Pose))
	{
		return false;
	}
	Pose->PoseId = PoisonedPoseIdForStateFragmentTests;

	// ...and handed a delta decoded against another. The reordered target is deliberately the other class:
	// its stance sits at a different layout index, so an unrefused apply would write a real value here.
	FCrowdyRepLayout ForeignLayout;
	if (!TestTrue(TEXT("the other class builds a layout"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyMassStateFragmentReorderedPoseTarget::StaticClass(), ForeignLayout)))
	{
		return false;
	}

	UCrowdyMassStateFragmentReorderedPoseTarget* Source = NewObject<UCrowdyMassStateFragmentReorderedPoseTarget>();
	Source->Stance = ECrowdyMassStateFragmentTestStance::Dead;
	TArray<uint8> Blob;
	FCrowdyStateCodec::Encode(ForeignLayout, Source, TBitArray<>(true, ForeignLayout.Properties.Num()),
		/*bKeyframe=*/true, Blob);

	UCrowdyMassStateFragmentReorderedPoseTarget* Decoded = NewObject<UCrowdyMassStateFragmentReorderedPoseTarget>();
	TArray<int32> DeliveredIndices;
	TArray<int32> PresentIndices;
	if (!TestTrue(TEXT("decode succeeded"),
		FCrowdyStateCodec::Decode(ForeignLayout, ForeignLayout.LayoutHash, Blob, Decoded, DeliveredIndices, &PresentIndices)))
	{
		return false;
	}

	AddExpectedError(TEXT("Refusing a decoded state delta for entity"),
		EAutomationExpectedErrorFlags::Contains, 1);

	// The delta is well formed and carries a real value; only the class disagrees. Asserted as "the target
	// is untouched" rather than "a function returned false", because the refusal is only worth anything if
	// nothing was written.
	Fixture.Subsystem->ApplyDecodedState(UUID, ForeignLayout, Decoded, PresentIndices);

	TestEqual(TEXT("nothing was written: the entity keeps the value it was poisoned with"),
		Pose->PoseId, PoisonedPoseIdForStateFragmentTests);

	// The control, so this can never pass by the delta being empty or the value coinciding with the poison:
	// the same bytes DO land once the entity is the class the layout describes.
	const FGuid MatchingUUID = FGuid::NewGuid();
	const FMassEntityHandle MatchingEntity = Fixture.MakePoseEntity(
		MatchingUUID, UCrowdyMassStateFragmentReorderedPoseTarget::StaticClass());

	FCrowdyMassViewPoseFragment* MatchingPose = Fixture.GetPose(MatchingEntity);
	if (!TestNotNull(TEXT("second pose fragment present"), MatchingPose))
	{
		return false;
	}
	MatchingPose->PoseId = PoisonedPoseIdForStateFragmentTests;

	Fixture.Subsystem->ApplyDecodedState(MatchingUUID, ForeignLayout, Decoded, PresentIndices);

	TestEqual(TEXT("the same delta lands when the class matches, so the refusal above was the class and nothing else"),
		MatchingPose->PoseId, static_cast<uint8>(2));

	return true;
}

/**
 * Declaring a pose as the game's own enum costs exactly what the raw byte cost, measured rather than
 * assumed.
 *
 * This is the phase's whole ask of a game, so its price has to be pinned by a number. Left alone it is not
 * free: a CrowdyState leaf is encoded through FProperty::SerializeItem, and FEnumProperty::SerializeItem
 * writes the enum ENTRY NAME, which a memory archive writes as a length-prefixed string. Measured before
 * the codec carried enums by value, one such field cost 51 bytes against 4 for the plain byte beside it.
 * The field it lands on carries CrowdyHeartbeat, so that would be paid per entity per keyframe interval
 * whether or not anything moved, on the plane whose receive drain bounds the population.
 *
 * The two fixtures differ in exactly one thing: one declares its pose property as a byte-wide enum, the
 * other as a raw uint8. Both carry one CrowdyState property and nothing else, so any difference between
 * the two blobs is the field encoding and nothing else.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassStateFragmentEnumWireCostTest,
	"CrowdySDK.Mass.StateFragmentEnumWireCost", CrowdyMassStateFragmentTestFlags)
bool FCrowdyMassStateFragmentEnumWireCostTest::RunTest(const FString& Parameters)
{
	FCrowdyRepLayout EnumLayout;
	FCrowdyRepLayout ByteLayout;
	if (!TestTrue(TEXT("the enum-declared class builds a layout"),
			FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyMassStateFragmentPoseTarget::StaticClass(), EnumLayout))
		|| !TestTrue(TEXT("the byte-declared class builds a layout"),
			FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyMassStateFragmentLegacyByteTarget::StaticClass(), ByteLayout)))
	{
		return false;
	}

	// One property each, or the comparison below would be measuring the class and not the field.
	if (!TestEqual(TEXT("the enum-declared class carries exactly one CrowdyState property"), EnumLayout.Properties.Num(), 1)
		|| !TestEqual(TEXT("and so does the byte-declared one"), ByteLayout.Properties.Num(), 1))
	{
		return false;
	}

	UCrowdyMassStateFragmentPoseTarget* EnumSource = NewObject<UCrowdyMassStateFragmentPoseTarget>();
	EnumSource->Stance = ECrowdyMassStateFragmentTestStance::Mounted;
	TArray<uint8> EnumBlob;
	FCrowdyStateCodec::Encode(EnumLayout, EnumSource, TBitArray<>(true, 1), /*bKeyframe=*/true, EnumBlob);

	UCrowdyMassStateFragmentLegacyByteTarget* ByteSource = NewObject<UCrowdyMassStateFragmentLegacyByteTarget>();
	ByteSource->PoseId = 2;
	TArray<uint8> ByteBlob;
	FCrowdyStateCodec::Encode(ByteLayout, ByteSource, TBitArray<>(true, 1), /*bKeyframe=*/true, ByteBlob);

	AddInfo(FString::Printf(
		TEXT("one CrowdyState property, keyframed: declared as a byte-wide enum it costs %d bytes, declared as a uint8 it costs %d."),
		EnumBlob.Num(), ByteBlob.Num()));

	// Equality rather than a bound, and deliberately not a magic number: what is being held is that the two
	// declarations cost the SAME, so a game is never charged for choosing the typed one. If this ever fails
	// with the enum larger, the codec has gone back to writing the entry name and every heartbeat on the
	// plane got roughly thirty times more expensive with nothing else to show for it.
	TestEqual(TEXT("a byte-wide enum costs exactly what the uint8 it replaced cost"),
		EnumBlob.Num(), ByteBlob.Num());

	// And the value really did travel: a size match with a broken decode would be a cheap wrong answer.
	UCrowdyMassStateFragmentPoseTarget* Decoded = NewObject<UCrowdyMassStateFragmentPoseTarget>();
	TArray<int32> ChangedIndices;
	TArray<int32> PresentIndices;
	if (!TestTrue(TEXT("decode succeeded"),
		FCrowdyStateCodec::Decode(EnumLayout, EnumLayout.LayoutHash, EnumBlob, Decoded, ChangedIndices, &PresentIndices)))
	{
		return false;
	}

	TestTrue(TEXT("and the stance round-trips through the compact encoding"),
		Decoded->Stance == ECrowdyMassStateFragmentTestStance::Mounted);

	return true;
}

/**
 * Whether this subsystem HOLDS an id and what class it holds it AS are two separate answers, and neither is
 * derived from the other.
 *
 * A crowd entity can legitimately be spawned with no class: a crowd that is only ever a position declares
 * nothing. Such an entity is held exactly like any other, so the router refuses its payloads rather than
 * queueing them on a spawn that already happened. Answering IsEntityKnown from the recorded class would
 * turn every payload for that crowd into a wait that can only time out, and a crowd of those fills the
 * router's bounded spawn-wait queue and evicts the entities genuinely still arriving.
 *
 * Asserted on the real subsystem rather than on a double, because this is the implementation the router
 * actually asks, and the two answers are read from two different containers here (the tracker and the class
 * map) which is precisely what a later change could quietly collapse.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassStateFragmentKnownIsNotAnsweredFromClassTest,
	"CrowdySDK.Mass.StateFragmentKnownIsNotAnsweredFromClass", CrowdyMassStateFragmentTestFlags)
bool FCrowdyMassStateFragmentKnownIsNotAnsweredFromClassTest::RunTest(const FString& Parameters)
{
	FCrowdyMassStateFragmentTestFixture Fixture;

	// A position-only entity: tracked, with nothing declared for it.
	const FGuid ClasslessUUID = FGuid::NewGuid();
	Fixture.MakePoseEntity(ClasslessUUID, nullptr);

	TestTrue(TEXT("an entity spawned with no class is still held"),
		Fixture.Subsystem->IsEntityKnown(ClasslessUUID));
	TestNull(TEXT("and honestly reports having no class"),
		Fixture.Subsystem->GetEntityClass(ClasslessUUID));

	// The control that keeps the pair above from passing on a subsystem that simply answers true to
	// everything: an id nothing ever spawned is not held, and has no class either.
	const FGuid NeverSpawnedUUID = FGuid::NewGuid();
	TestFalse(TEXT("an id nothing spawned is not held"),
		Fixture.Subsystem->IsEntityKnown(NeverSpawnedUUID));
	TestNull(TEXT("and has no class"), Fixture.Subsystem->GetEntityClass(NeverSpawnedUUID));

	// And the ordinary entity, so "held" is not being confused with "has a class" in the other direction.
	const FGuid ClassedUUID = FGuid::NewGuid();
	Fixture.MakePoseEntity(ClassedUUID, UCrowdyMassStateFragmentPoseTarget::StaticClass());

	TestTrue(TEXT("an entity spawned with a class is held"), Fixture.Subsystem->IsEntityKnown(ClassedUUID));
	TestTrue(TEXT("and reports that class"),
		Fixture.Subsystem->GetEntityClass(ClassedUUID) == UCrowdyMassStateFragmentPoseTarget::StaticClass());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
