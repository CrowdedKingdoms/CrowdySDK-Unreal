#include "CrowdyMassStateFragmentTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_METADATA

#include "CrowdyMassStateLogCapture.h"
#include "Fragments/CrowdyMassAnimFragments.h"
#include "MassEntityManager.h"
#include "Misc/AutomationTest.h"
#include "Replication/State/FCrowdyRepLayout.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"

// Covers what a CrowdyState fragment field registered BY NAME will and will not read.
//
// A registration hands its ingestor a bare pointer to one property's value, so the cast that ingestor makes
// is the only thing deciding how those bytes are read. The registration therefore names the type it reads
// and the resolution matches on the name AND on that type: a property carrying the name and declared as
// something else is refused rather than misread, and a float handed to an ingestor casting to int32 would
// be a wrong number nothing downstream could tell from a right one.
//
// Nothing here gives a class an animation set, so CrowdyMass's own pose field never matches and every case
// below is about the name-matched path alone. The pose field's separate byte rule is covered in
// CrowdyMassStateFragmentProviderTests.cpp and must stay covered there: the two rules are different on
// purpose.
namespace
{
	constexpr EAutomationTestFlags CrowdyMassTypedFieldTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// What an ingestor read, and whether it ran at all. Both halves are asserted: a refusal that let the
	// ingestor run on a misread value and a refusal that stopped it look identical from the value alone,
	// because a fragment left untouched holds whatever it already held.
	struct FCrowdyMassTypedFieldRead
	{
		bool bRan = false;
		double Value = 0.0;
	};

	using FCrowdyMassTypedFieldReadRef = TSharedRef<FCrowdyMassTypedFieldRead, ESPMode::ThreadSafe>;

	int32 TypedFieldLayoutIndexOf(const FCrowdyRepLayout& Layout, const TCHAR* Name)
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

	// A subsystem with a real entity manager and no standard fields at all: no pose registration, so nothing
	// can claim a property by type, and no project entity config, so a class resolves to whatever this
	// fixture declares rather than to whatever the running project happens to ship.
	struct FCrowdyMassTypedFieldFixture
	{
		TSharedPtr<FMassEntityManager> EntityManager;
		UCrowdyMassEntitySubsystem* Subsystem = nullptr;

		FCrowdyMassTypedFieldFixture()
		{
			// PostInitialize constructs relation observers outered to whatever owner the manager was given,
			// and an owner-less manager takes the process down.
			EntityManager = MakeShareable(new FMassEntityManager(GetTransientPackage()));

			FMassEntityManagerStorageInitParams InitParams;
			InitParams.Emplace<FMassEntityManager_InitParams_Concurrent>();
			EntityManager->Initialize(InitParams);
			EntityManager->PostInitialize();

			Subsystem = NewObject<UCrowdyMassEntitySubsystem>(GetTransientPackage());
			Subsystem->SetMassEntityManagerForTest(EntityManager.Get());
			Subsystem->SetAuthoredEntityConfigForTest(nullptr, nullptr);
		}

		~FCrowdyMassTypedFieldFixture()
		{
			EntityManager->Deinitialize();
		}

		// Registers one field reading FieldType, recording what it reads rather than writing a fragment: the
		// value is the subject, and a fragment write would only add a way for the case to pass or fail for a
		// reason that is not this one.
		FCrowdyMassTypedFieldReadRef RegisterRecordingField(const TCHAR* PropertyName, const ECrowdyMassStateFieldType FieldType)
		{
			const FCrowdyMassTypedFieldReadRef Record = MakeShared<FCrowdyMassTypedFieldRead, ESPMode::ThreadSafe>();

			Subsystem->RegisterStateFragmentField(FName(PropertyName), FieldType,
				[Record, FieldType](const void* PropertyValuePtr, const FCrowdyMassViewStateContext&)
				{
					Record->bRan = true;

					switch (FieldType)
					{
					case ECrowdyMassStateFieldType::UInt8:
						Record->Value = *static_cast<const uint8*>(PropertyValuePtr);
						return;
					case ECrowdyMassStateFieldType::Int32:
						Record->Value = *static_cast<const int32*>(PropertyValuePtr);
						return;
					case ECrowdyMassStateFieldType::Int64:
						Record->Value = static_cast<double>(*static_cast<const int64*>(PropertyValuePtr));
						return;
					case ECrowdyMassStateFieldType::Float:
						Record->Value = *static_cast<const float*>(PropertyValuePtr);
						return;
					case ECrowdyMassStateFieldType::Double:
						Record->Value = *static_cast<const double*>(PropertyValuePtr);
						return;
					case ECrowdyMassStateFieldType::Bool:
						Record->Value = *static_cast<const bool*>(PropertyValuePtr) ? 1.0 : 0.0;
						return;
					}
				});

			return Record;
		}

		// A real entity tracked under UUID and stamped with StateClass exactly as SpawnRemoteEntity's own
		// bookkeeping would, so ApplyDecodedState resolves it.
		FGuid MakeTrackedEntity(const UClass* StateClass)
		{
			const UScriptStruct* Fragments[] = {FCrowdyMassViewPoseFragment::StaticStruct()};
			const FMassArchetypeHandle Archetype = EntityManager->CreateArchetype(MakeArrayView(Fragments, 1));
			const FMassEntityHandle Entity = EntityManager->CreateEntity(Archetype);

			const FGuid UUID = FGuid::NewGuid();
			Subsystem->RegisterTrackedEntityForTest(UUID, Entity);
			Subsystem->SetStateEntityClassForTest(UUID, StateClass);
			return UUID;
		}
	};

	// Builds Class's layout and finds one property in it, failing the case rather than letting every
	// assertion after it read as vacuously true.
	bool BuildLayoutAndIndex(FAutomationTestBase& Test, const UClass* Class, const TCHAR* PropertyName,
		FCrowdyRepLayout& OutLayout, int32& OutIndex)
	{
		if (!Test.TestTrue(TEXT("the fixture class has a CrowdyState layout"),
			FCrowdyStateLayoutBuilder::BuildLayout(Class, OutLayout)))
		{
			return false;
		}

		OutIndex = TypedFieldLayoutIndexOf(OutLayout, PropertyName);
		return Test.TestNotEqual(TEXT("the property is in the layout"), OutIndex, static_cast<int32>(INDEX_NONE));
	}
}

/**
 * A field registered for a uint8 reads a uint8 property.
 *
 * The type that was the only one there used to be, kept as a case of its own so a widening that dropped it
 * would be caught by something other than the gates.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassTypedFieldReadsAUint8Test,
	"CrowdySDK.Mass.StateFragmentTypedFieldReadsAUint8", CrowdyMassTypedFieldTestFlags)
bool FCrowdyMassTypedFieldReadsAUint8Test::RunTest(const FString& Parameters)
{
	FCrowdyMassTypedFieldFixture Fixture;
	const FCrowdyMassTypedFieldReadRef Record = Fixture.RegisterRecordingField(TEXT("Posture"), ECrowdyMassStateFieldType::UInt8);

	FCrowdyRepLayout Layout;
	int32 Index = INDEX_NONE;
	if (!BuildLayoutAndIndex(*this, UCrowdyMassStateFragmentTypedTarget::StaticClass(), TEXT("Posture"), Layout, Index))
	{
		return false;
	}

	const FGuid UUID = Fixture.MakeTrackedEntity(UCrowdyMassStateFragmentTypedTarget::StaticClass());

	UCrowdyMassStateFragmentTypedTarget* Decoded = NewObject<UCrowdyMassStateFragmentTypedTarget>();
	Decoded->Posture = 200;
	Fixture.Subsystem->ApplyDecodedState(UUID, Layout, Decoded, TArray<int32>({Index}));

	TestTrue(TEXT("the ingestor ran"), Record->bRan);
	TestEqual(TEXT("and read the byte it was sent"), Record->Value, 200.0);

	return true;
}

/**
 * A field registered for a uint8 reads a byte-wide enum property of the same name.
 *
 * The ordinary way a game declares a replicated state is `enum class E : uint8`, which reflects as an enum
 * property wrapping a byte rather than as a plain byte. Matching on the declaration rather than on the type
 * it is declared over would refuse exactly that, and the refusal would look like the field never arriving.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassTypedFieldReadsAByteEnumByNameTest,
	"CrowdySDK.Mass.StateFragmentTypedFieldReadsAByteEnumByName", CrowdyMassTypedFieldTestFlags)
bool FCrowdyMassTypedFieldReadsAByteEnumByNameTest::RunTest(const FString& Parameters)
{
	FCrowdyMassTypedFieldFixture Fixture;
	const FCrowdyMassTypedFieldReadRef Record = Fixture.RegisterRecordingField(TEXT("Mood"), ECrowdyMassStateFieldType::UInt8);

	FCrowdyRepLayout Layout;
	int32 Index = INDEX_NONE;
	if (!BuildLayoutAndIndex(*this, UCrowdyMassStateFragmentTypedTarget::StaticClass(), TEXT("Mood"), Layout, Index))
	{
		return false;
	}

	const FGuid UUID = Fixture.MakeTrackedEntity(UCrowdyMassStateFragmentTypedTarget::StaticClass());

	UCrowdyMassStateFragmentTypedTarget* Decoded = NewObject<UCrowdyMassStateFragmentTypedTarget>();
	Decoded->Mood = ECrowdyMassStateFragmentTestMood::Angry;
	Fixture.Subsystem->ApplyDecodedState(UUID, Layout, Decoded, TArray<int32>({Index}));

	TestTrue(TEXT("the ingestor ran"), Record->bRan);
	TestEqual(TEXT("and read the enum's value through the byte it occupies"), Record->Value, 1.0);

	return true;
}

/**
 * A field registered for an int32 reads an int32 property.
 *
 * The value is negative and larger than a byte, so a build still reading four bytes through a byte pointer
 * cannot answer it by coincidence.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassTypedFieldReadsAnInt32Test,
	"CrowdySDK.Mass.StateFragmentTypedFieldReadsAnInt32", CrowdyMassTypedFieldTestFlags)
bool FCrowdyMassTypedFieldReadsAnInt32Test::RunTest(const FString& Parameters)
{
	FCrowdyMassTypedFieldFixture Fixture;
	const FCrowdyMassTypedFieldReadRef Record = Fixture.RegisterRecordingField(TEXT("Health"), ECrowdyMassStateFieldType::Int32);

	FCrowdyRepLayout Layout;
	int32 Index = INDEX_NONE;
	if (!BuildLayoutAndIndex(*this, UCrowdyMassStateFragmentTypedTarget::StaticClass(), TEXT("Health"), Layout, Index))
	{
		return false;
	}

	const FGuid UUID = Fixture.MakeTrackedEntity(UCrowdyMassStateFragmentTypedTarget::StaticClass());

	UCrowdyMassStateFragmentTypedTarget* Decoded = NewObject<UCrowdyMassStateFragmentTypedTarget>();
	Decoded->Health = -70000;
	Fixture.Subsystem->ApplyDecodedState(UUID, Layout, Decoded, TArray<int32>({Index}));

	TestTrue(TEXT("the ingestor ran"), Record->bRan);
	TestEqual(TEXT("and read the whole int rather than a byte of it"), Record->Value, -70000.0);

	return true;
}

/**
 * A field registered for a float reads a float property.
 *
 * The value is deliberately not a whole number: a float read a byte at a time answers 0 for most round
 * values, which is the misread this check exists to make impossible rather than unlikely.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassTypedFieldReadsAFloatTest,
	"CrowdySDK.Mass.StateFragmentTypedFieldReadsAFloat", CrowdyMassTypedFieldTestFlags)
bool FCrowdyMassTypedFieldReadsAFloatTest::RunTest(const FString& Parameters)
{
	FCrowdyMassTypedFieldFixture Fixture;
	const FCrowdyMassTypedFieldReadRef Record = Fixture.RegisterRecordingField(TEXT("Stamina"), ECrowdyMassStateFieldType::Float);

	FCrowdyRepLayout Layout;
	int32 Index = INDEX_NONE;
	if (!BuildLayoutAndIndex(*this, UCrowdyMassStateFragmentTypedTarget::StaticClass(), TEXT("Stamina"), Layout, Index))
	{
		return false;
	}

	const FGuid UUID = Fixture.MakeTrackedEntity(UCrowdyMassStateFragmentTypedTarget::StaticClass());

	UCrowdyMassStateFragmentTypedTarget* Decoded = NewObject<UCrowdyMassStateFragmentTypedTarget>();
	Decoded->Stamina = 73.5f;
	Fixture.Subsystem->ApplyDecodedState(UUID, Layout, Decoded, TArray<int32>({Index}));

	TestTrue(TEXT("the ingestor ran"), Record->bRan);
	TestEqual(TEXT("and read the float it was sent"), Record->Value, 73.5);

	return true;
}

/**
 * A field registered for an int32 reads an enum declared over an int32.
 *
 * An enum is read as the type it is declared over, and that rule is not a uint8 special case. Without it a
 * game would have to choose between declaring its state as an enum and declaring it as anything but a byte.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassTypedFieldReadsAnIntEnumTest,
	"CrowdySDK.Mass.StateFragmentTypedFieldReadsAnIntEnum", CrowdyMassTypedFieldTestFlags)
bool FCrowdyMassTypedFieldReadsAnIntEnumTest::RunTest(const FString& Parameters)
{
	FCrowdyMassTypedFieldFixture Fixture;
	const FCrowdyMassTypedFieldReadRef Record = Fixture.RegisterRecordingField(TEXT("Band"), ECrowdyMassStateFieldType::Int32);

	FCrowdyRepLayout Layout;
	int32 Index = INDEX_NONE;
	if (!BuildLayoutAndIndex(*this, UCrowdyMassStateFragmentBandTarget::StaticClass(), TEXT("Band"), Layout, Index))
	{
		return false;
	}

	const FGuid UUID = Fixture.MakeTrackedEntity(UCrowdyMassStateFragmentBandTarget::StaticClass());

	UCrowdyMassStateFragmentBandTarget* Decoded = NewObject<UCrowdyMassStateFragmentBandTarget>();
	Decoded->Band = ECrowdyMassStateFragmentTestBand::High;
	Fixture.Subsystem->ApplyDecodedState(UUID, Layout, Decoded, TArray<int32>({Index}));

	TestTrue(TEXT("the ingestor ran"), Record->bRan);
	TestEqual(TEXT("and read the enum's whole value"), Record->Value, 70000.0);

	return true;
}

/**
 * A property whose name matches and whose type does not is refused, and the ingestor never runs.
 *
 * This is the check the byte rule used to stand in for, and it is stronger than what it replaced: a name
 * match alone would hand a float to an ingestor casting to int32, and the number that comes out of that is
 * not detectably wrong anywhere downstream. Asserting the refusal line alone would not be enough either,
 * since a build that said the line and then ran the ingestor anyway would still ship the misread.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassTypedFieldRefusesAMismatchedTypeTest,
	"CrowdySDK.Mass.StateFragmentTypedFieldRefusesAMismatchedType", CrowdyMassTypedFieldTestFlags)
bool FCrowdyMassTypedFieldRefusesAMismatchedTypeTest::RunTest(const FString& Parameters)
{
	FCrowdyMassTypedFieldFixture Fixture;
	const FCrowdyMassTypedFieldReadRef Record = Fixture.RegisterRecordingField(TEXT("Stamina"), ECrowdyMassStateFieldType::Int32);

	FCrowdyRepLayout Layout;
	int32 Index = INDEX_NONE;
	if (!BuildLayoutAndIndex(*this, UCrowdyMassStateFragmentTypedTarget::StaticClass(), TEXT("Stamina"), Layout, Index))
	{
		return false;
	}

	const FGuid UUID = Fixture.MakeTrackedEntity(UCrowdyMassStateFragmentTypedTarget::StaticClass());

	UCrowdyMassStateFragmentTypedTarget* Decoded = NewObject<UCrowdyMassStateFragmentTypedTarget>();
	Decoded->Stamina = 73.5f;

	FCrowdyMassStateLogCapture Capture;
	Fixture.Subsystem->ApplyDecodedState(UUID, Layout, Decoded, TArray<int32>({Index}));
	const TArray<FString> Warnings = Capture.TakeWarnings();

	TestFalse(TEXT("the ingestor never ran, so no float was read as an int"), Record->bRan);

	// Names both sides, because the author's choice is between two ends to fix and a line naming only one
	// of them is unfollowable from the other.
	TestEqual(TEXT("and the refusal names the declared type and the registered one"),
		CrowdyMassCountLinesContaining(Warnings,
			TEXT("which is float, but the field registered for that name reads it as int32")), 1);

	// The remedy names the type to register for rather than telling the author to register for "the type the
	// property is declared as", which is only followable by someone who already knows what that resolves to.
	TestEqual(TEXT("and it names the type that would bind, so the fix can be applied without deriving it"),
		CrowdyMassCountLinesContaining(Warnings,
			TEXT("Register the field for float, which is what this property reads as.")), 1);

	return true;
}

/**
 * A refused name-matched property is told about its registration, never about the animation set's enum.
 *
 * The two refusals are two different faults and the sentence has to follow the MATCH rather than whether
 * the property happens to name an enum. This class has no animation set at all, so telling its author to
 * declare "the enum its animation set names" over a byte would point at a rule their registration is not
 * held to and at a set they never authored.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassTypedFieldWideEnumGetsTheByNameRefusalTest,
	"CrowdySDK.Mass.StateFragmentTypedFieldWideEnumGetsTheByNameRefusal", CrowdyMassTypedFieldTestFlags)
bool FCrowdyMassTypedFieldWideEnumGetsTheByNameRefusalTest::RunTest(const FString& Parameters)
{
	FCrowdyMassTypedFieldFixture Fixture;
	const FCrowdyMassTypedFieldReadRef Record = Fixture.RegisterRecordingField(TEXT("Band"), ECrowdyMassStateFieldType::UInt8);

	FCrowdyRepLayout Layout;
	int32 Index = INDEX_NONE;
	if (!BuildLayoutAndIndex(*this, UCrowdyMassStateFragmentBandTarget::StaticClass(), TEXT("Band"), Layout, Index))
	{
		return false;
	}

	const FGuid UUID = Fixture.MakeTrackedEntity(UCrowdyMassStateFragmentBandTarget::StaticClass());

	UCrowdyMassStateFragmentBandTarget* Decoded = NewObject<UCrowdyMassStateFragmentBandTarget>();
	Decoded->Band = ECrowdyMassStateFragmentTestBand::High;

	FCrowdyMassStateLogCapture Capture;
	Fixture.Subsystem->ApplyDecodedState(UUID, Layout, Decoded, TArray<int32>({Index}));
	const TArray<FString> Warnings = Capture.TakeWarnings();

	TestFalse(TEXT("nothing is read through a byte pointer"), Record->bRan);

	TestEqual(TEXT("the refusal names the registration the property was matched against"),
		CrowdyMassCountLinesContaining(Warnings, TEXT("the field registered for that name reads it as uint8")), 1);

	// And never the pose's sentence, which would send this author to redeclare an enum over a byte to
	// satisfy an animation set they have not got.
	TestEqual(TEXT("and never the animation set's, which this class has not got"),
		CrowdyMassCountLinesContaining(Warnings, TEXT("underlying type is wider than a byte")), 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_METADATA
