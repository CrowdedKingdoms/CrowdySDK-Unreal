#include "CrowdyMassStateFragmentTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Backend/CrowdyMassRenderingBackend.h"
#include "Fragments/CrowdyMassAnimFragments.h"
#include "MassEntityManager.h"
#include "Misc/AutomationTest.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"
#include "UObject/Package.h"

// A moving entity spawned on a Mass map has no actor of its class: its spawn data lands in its row through the
// CrowdyState fragment fields a game registers, whether the row exists when the spawn event arrives or appears later.
namespace
{
	constexpr EAutomationTestFlags CrowdyMassSpawnStateTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	using FCrowdyMassSpawnStateReadsRef = TSharedRef<TArray<TPair<FName, int32>>, ESPMode::ThreadSafe>;

	// A subsystem over a real entity manager, with two int32 fields registered and every ingestor run recorded.
	struct FCrowdyMassSpawnStateFixture
	{
		TSharedPtr<FMassEntityManager> EntityManager;
		UCrowdyMassEntitySubsystem* Subsystem = nullptr;
		FCrowdyMassSpawnStateReadsRef Reads = MakeShared<TArray<TPair<FName, int32>>, ESPMode::ThreadSafe>();

		FCrowdyMassSpawnStateFixture()
		{
			// An owner-less manager takes the process down when PostInitialize builds its relation observers.
			EntityManager = MakeShareable(new FMassEntityManager(GetTransientPackage()));

			FMassEntityManagerStorageInitParams InitParams;
			InitParams.Emplace<FMassEntityManager_InitParams_Concurrent>();
			EntityManager->Initialize(InitParams);
			EntityManager->PostInitialize();

			Subsystem = NewObject<UCrowdyMassEntitySubsystem>(GetTransientPackage());
			Subsystem->SetMassEntityManagerForTest(EntityManager.Get());
			Subsystem->SetAuthoredEntityConfigForTest(nullptr, nullptr);

			for (const TCHAR* Name : { TEXT("SpawnScore"), TEXT("SpawnLevel") })
			{
				const FName FieldName(Name);
				Subsystem->RegisterStateFragmentField(FieldName, ECrowdyMassStateFieldType::Int32,
					[Reads = Reads, FieldName](const void* Value, const FCrowdyMassViewStateContext&)
					{
						Reads->Emplace(FieldName, *static_cast<const int32*>(Value));
					});
			}
		}

		~FCrowdyMassSpawnStateFixture()
		{
			EntityManager->Deinitialize();
		}

		FGuid MakeTrackedEntity() const
		{
			const UScriptStruct* Fragments[] = {FCrowdyMassViewPoseFragment::StaticStruct()};
			const FMassArchetypeHandle Archetype = EntityManager->CreateArchetype(MakeArrayView(Fragments, 1));
			const FGuid UUID = FGuid::NewGuid();
			Subsystem->RegisterTrackedEntityForTest(UUID, EntityManager->CreateEntity(Archetype));
			return UUID;
		}
	};

	FInstancedStruct MakeSpawnState(const int32 Score)
	{
		FCrowdyMassSpawnStateTestData Data;
		Data.SpawnScore = Score;
		Data.SpawnLevel = 3.5f;
		Data.SpawnUnclaimed = 99;
		return FInstancedStruct::Make(Data);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassSpawnStateLandsThroughFieldsTest,
	"CrowdySDK.Mass.SpawnStateLandsThroughRegisteredFields", CrowdyMassSpawnStateTestFlags)
bool FCrowdyMassSpawnStateLandsThroughFieldsTest::RunTest(const FString& Parameters)
{
	FCrowdyMassSpawnStateFixture Fixture;

	TestFalse(TEXT("spawn data for an id with no row is not applied"), Fixture.Subsystem->ApplySpawnState(FGuid::NewGuid(), MakeSpawnState(7)));
	TestEqual(TEXT("and runs no ingestor"), Fixture.Reads->Num(), 0);

	const FGuid UUID = Fixture.MakeTrackedEntity();
	TestTrue(TEXT("spawn data for a row is applied"), Fixture.Subsystem->ApplySpawnState(UUID, MakeSpawnState(7)));
	if (TestEqual(TEXT("through the one field whose name and type match"), Fixture.Reads->Num(), 1))
	{
		TestEqual(TEXT("the score field"), (*Fixture.Reads)[0].Key, FName(TEXT("SpawnScore")));
		TestEqual(TEXT("with the value sent"), (*Fixture.Reads)[0].Value, 7);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassBackendHoldsSpawnDataTest,
	"CrowdySDK.Mass.BackendHoldsSpawnDataUntilTheRowExists", CrowdyMassSpawnStateTestFlags)
bool FCrowdyMassBackendHoldsSpawnDataTest::RunTest(const FString& Parameters)
{
	FCrowdyMassSpawnStateFixture Fixture;
	UCrowdyMassRenderingBackend* Backend = NewObject<UCrowdyMassRenderingBackend>(GetTransientPackage());
	const FGuid Drawn = Fixture.MakeTrackedEntity();
	Backend->BindSlotForTest(0, Drawn, Fixture.Subsystem);

	// The usual order: the spawn event arrives before the entity's first update, so before its row.
	const FGuid Early = FGuid::NewGuid();
	Backend->TakeSpawnData(Early, MakeSpawnState(4));
	TestEqual(TEXT("spawn data for a row not drawn yet is held"), Backend->GetPendingSpawnDataCountForTest(), 1);
	TestEqual(TEXT("and applied to nothing yet"), Fixture.Reads->Num(), 0);

	TestFalse(TEXT("a destroy for an entity with no slot releases nothing drawn"), Backend->ReleaseDestroyedInstance(INDEX_NONE, Early));
	TestEqual(TEXT("but drops its held spawn data"), Backend->GetPendingSpawnDataCountForTest(), 0);

	// The update came first: the row exists, so the data lands at once.
	Backend->TakeSpawnData(Drawn, MakeSpawnState(5));
	TestEqual(TEXT("spawn data for a drawn row is not held"), Backend->GetPendingSpawnDataCountForTest(), 0);
	TestTrue(TEXT("it lands at once"), Fixture.Reads->Num() == 1 && (*Fixture.Reads)[0].Value == 5);
	Backend->TakeSpawnData(Drawn, MakeSpawnState(6));
	TestEqual(TEXT("a repeated spawn event does not land on the row again"), Fixture.Reads->Num(), 1);

	TestTrue(TEXT("a destroy for the slot's entity releases its row"), Backend->ReleaseDestroyedInstance(0, Drawn));
	FMassEntityHandle Row;
	TestFalse(TEXT("which is gone"), Fixture.Subsystem->FindEntityByUUID(Drawn, Row));
	TestFalse(TEXT("a destroy naming another entity's slot releases nothing"), Backend->ReleaseDestroyedInstance(0, FGuid::NewGuid()));

	// Spawn events are forgeable, so what waits for a row is bounded.
	for (int32 Index = 0; Index <= UCrowdyMassRenderingBackend::MaxPendingSpawnData; ++Index)
		Backend->TakeSpawnData(FGuid::NewGuid(), MakeSpawnState(Index));
	TestEqual(TEXT("held spawn data stops at the bound"), Backend->GetPendingSpawnDataCountForTest(), UCrowdyMassRenderingBackend::MaxPendingSpawnData);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
