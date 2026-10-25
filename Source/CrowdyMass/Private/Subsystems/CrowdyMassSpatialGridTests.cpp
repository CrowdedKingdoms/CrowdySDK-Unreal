#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Fragments/CrowdyMassAnimFragments.h"
#include "MassEntityManager.h"
#include "Settings/CrowdyMassDeveloperSettings.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"

#include <limits>

// The trace grid's sphere query sizes its cell box before building it, so a radius or position that came off the wire
// is refused rather than enumerated. Expectations are derived from the project's own grid settings, so the cases hold
// under any cell size or grid mode a project picks.
namespace
{
	constexpr EAutomationTestFlags CrowdyMassSpatialGridTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// A standalone Mass entity manager with no World, as the model field fixture builds one.
	struct FCrowdyMassSpatialGridTestFixture
	{
		TSharedPtr<FMassEntityManager> EntityManager;
		UCrowdyMassEntitySubsystem* Subsystem = nullptr;
		const UCrowdyMassDeveloperSettings* Settings = GetDefault<UCrowdyMassDeveloperSettings>();

		FCrowdyMassSpatialGridTestFixture()
		{
			EntityManager = MakeShareable(new FMassEntityManager(GetTransientPackage()));

			FMassEntityManagerStorageInitParams InitParams;
			InitParams.Emplace<FMassEntityManager_InitParams_Concurrent>();
			EntityManager->Initialize(InitParams);
			EntityManager->PostInitialize();

			Subsystem = NewObject<UCrowdyMassEntitySubsystem>(GetTransientPackage());
			Subsystem->SetMassEntityManagerForTest(EntityManager.Get());
		}

		~FCrowdyMassSpatialGridTestFixture()
		{
			EntityManager->Deinitialize();
		}

		FMassEntityHandle MakeTrackedEntity()
		{
			const UScriptStruct* Fragments[] = {FCrowdyMassViewPoseFragment::StaticStruct()};
			const FMassArchetypeHandle Archetype = EntityManager->CreateArchetype(MakeArrayView(Fragments, 1));
			const FMassEntityHandle Entity = EntityManager->CreateEntity(Archetype);
			Subsystem->RegisterTrackedEntityForTest(FGuid::NewGuid(), Entity);
			return Entity;
		}

		double CellSize() const { return FMath::Max(Settings->SpatialGridCellSize, 100.0f); }

		// The centre of one cell, so a radius of one cell size covers exactly three cells on every axis in use.
		FVector CellCentre() const { return FVector(CellSize() * 0.5); }

		int32 CellsForOneCellRadius() const
		{
			return Settings->SpatialGridMode == ECrowdyMassSpatialGridMode::ThreeDimensional ? 27 : 9;
		}

		// A cap too large makes the cases below enumerate millions of cells; one too small cannot hold a one-cell radius.
		bool ProjectCapIsTestable() const
		{
			return Settings->SpatialGridMaxRayCells >= CellsForOneCellRadius() && Settings->SpatialGridMaxRayCells <= 65536;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassSpatialGridSphereRefusesPastTheCapTest,
	"CrowdySDK.Mass.SpatialGridSphereRefusesPastTheCap", CrowdyMassSpatialGridTestFlags)
bool FCrowdyMassSpatialGridSphereRefusesPastTheCapTest::RunTest(const FString& Parameters)
{
	FCrowdyMassSpatialGridTestFixture Fixture;
	const int32 Expected = Fixture.CellsForOneCellRadius();
	TArray<FIntVector> Cells;

	TestTrue(TEXT("a box exactly at the cap is built"),
		Fixture.Subsystem->GetSpatialGridCellsForSphereForTest(Fixture.CellCentre(), Fixture.CellSize(), Expected, Cells));
	TestEqual(TEXT("every cell of it"), Cells.Num(), Expected);

	TestFalse(TEXT("one cell past the cap is refused"),
		Fixture.Subsystem->GetSpatialGridCellsForSphereForTest(Fixture.CellCentre(), Fixture.CellSize(), Expected - 1, Cells));
	TestEqual(TEXT("a refusal leaves no cells behind"), Cells.Num(), 0);

	TestFalse(TEXT("a huge radius is refused, not enumerated"),
		Fixture.Subsystem->GetSpatialGridCellsForSphereForTest(Fixture.CellCentre(), 1.0e7f, 4096, Cells));
	TestFalse(TEXT("an infinite radius is refused"),
		Fixture.Subsystem->GetSpatialGridCellsForSphereForTest(Fixture.CellCentre(), std::numeric_limits<float>::infinity(), 4096, Cells));
	TestFalse(TEXT("a position that is not a number is refused"),
		Fixture.Subsystem->GetSpatialGridCellsForSphereForTest(FVector(std::numeric_limits<double>::quiet_NaN()), 0.0f, 4096, Cells));
	TestFalse(TEXT("a position past the cell index range is refused"),
		Fixture.Subsystem->GetSpatialGridCellsForSphereForTest(FVector(1.0e300), 0.0f, 4096, Cells));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassSpatialGridEntityPastTheCapIsPlacedWithinItTest,
	"CrowdySDK.Mass.SpatialGridEntityPastTheCapIsPlacedWithinIt", CrowdyMassSpatialGridTestFlags)
bool FCrowdyMassSpatialGridEntityPastTheCapIsPlacedWithinItTest::RunTest(const FString& Parameters)
{
	FCrowdyMassSpatialGridTestFixture Fixture;
	if (!Fixture.Settings->bEnableSpatialGrid || !Fixture.ProjectCapIsTestable())
	{
		AddWarning(TEXT("The project turns the trace grid off or sets a cap too large to enumerate here."));
		return true;
	}

	const FMassEntityHandle Entity = Fixture.MakeTrackedEntity();
	Fixture.Subsystem->UpdateSpatialGridEntityForTest(Entity, Fixture.CellCentre(), Fixture.CellSize());
	TestEqual(TEXT("the entity is placed"), Fixture.Subsystem->NumSpatialGridCellsForTest(Entity), Fixture.CellsForOneCellRadius());

	// A sweep past the cap is shrunk to fit it, so the entity is still placed where it now claims to be.
	Fixture.Subsystem->UpdateSpatialGridEntityForTest(Entity, Fixture.CellCentre(), 1.0e7f);
	const int32 Swept = Fixture.Subsystem->NumSpatialGridCellsForTest(Entity);
	TestTrue(TEXT("a radius past the cap is placed with the widest box that fits"), Swept > Fixture.CellsForOneCellRadius());
	TestTrue(TEXT("and never past the cap"), Swept <= Fixture.Settings->SpatialGridMaxRayCells);

	// Only a position that means nothing keeps the last placement.
	Fixture.Subsystem->UpdateSpatialGridEntityForTest(Entity, FVector(std::numeric_limits<double>::quiet_NaN()), 0.0f);
	TestEqual(TEXT("a position that is not a number keeps the last placement"), Fixture.Subsystem->NumSpatialGridCellsForTest(Entity), Swept);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassSpatialGridOverlapPastTheCapScansTrackedTest,
	"CrowdySDK.Mass.SpatialGridOverlapPastTheCapScansTracked", CrowdyMassSpatialGridTestFlags)
bool FCrowdyMassSpatialGridOverlapPastTheCapScansTrackedTest::RunTest(const FString& Parameters)
{
	FCrowdyMassSpatialGridTestFixture Fixture;
	if (!Fixture.Settings->bEnableSpatialGrid || !Fixture.ProjectCapIsTestable())
	{
		AddWarning(TEXT("The project turns the trace grid off or sets a cap too large to enumerate here."));
		return true;
	}

	const FMassEntityHandle Entity = Fixture.MakeTrackedEntity();
	Fixture.Subsystem->UpdateSpatialGridEntityForTest(Entity, Fixture.CellCentre(), 0.0f);

	// Far from the entity, so the grid answers nothing and only a scan can return it.
	const FVector FarAway = Fixture.CellCentre() + FVector(Fixture.CellSize() * 1000.0, 0.0, 0.0);
	TArray<FMassEntityHandle> Candidates;
	int32 VisitedCells = 0;

	TestTrue(TEXT("a small overlap is answered by the grid"),
		Fixture.Subsystem->GetOverlapCandidateEntities(FarAway, 1.0f, Candidates, VisitedCells));
	TestEqual(TEXT("which finds nothing out there"), Candidates.Num(), 0);
	TestTrue(TEXT("after visiting its cells"), VisitedCells > 0);

	TestFalse(TEXT("an overlap past the cap falls back to the scan"),
		Fixture.Subsystem->GetOverlapCandidateEntities(FarAway, 1.0e7f, Candidates, VisitedCells));
	TestTrue(TEXT("which returns every tracked entity"), Candidates.Contains(Entity));
	TestEqual(TEXT("without visiting a cell"), VisitedCells, 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassSpatialGridLongZeroRadiusTraceScansInsteadTest,
	"CrowdySDK.Mass.SpatialGridLongZeroRadiusTraceScansInstead", CrowdyMassSpatialGridTestFlags)
bool FCrowdyMassSpatialGridLongZeroRadiusTraceScansInsteadTest::RunTest(const FString& Parameters)
{
	FCrowdyMassSpatialGridTestFixture Fixture;
	if (!Fixture.Settings->bEnableSpatialGrid || !Fixture.ProjectCapIsTestable() || Fixture.Settings->SpatialGridTraceQueryRadius > 0.0f)
	{
		AddWarning(TEXT("The project turns the trace grid off, sets a cap too large to enumerate here, or widens every trace."));
		return true;
	}

	const FMassEntityHandle Entity = Fixture.MakeTrackedEntity();
	Fixture.Subsystem->UpdateSpatialGridEntityForTest(Entity, Fixture.CellCentre(), 0.0f);

	int32 Visits = 0;
	int32 VisitedCells = 0;
	const auto CountVisit = [&Visits](const FCrowdyMassTraceCandidateCell&, const bool)
	{
		++Visits;
		return true;
	};

	const FVector Start = Fixture.CellCentre();
	const FVector Short = Start + FVector(Fixture.CellSize() * 2.0, 0.0, 0.0);
	TestTrue(TEXT("a short trace is answered by the grid"),
		Fixture.Subsystem->ForEachTraceCandidateCell(Start, Short, 0.0f, CountVisit, VisitedCells));
	TestTrue(TEXT("and visits its cells"), Visits > 0);

	Visits = 0;
	const double CapCells = Fixture.Settings->SpatialGridMaxRayCells;
	const FVector Long = Start + FVector(Fixture.CellSize() * (CapCells + 10.0), 0.0, 0.0);
	TestFalse(TEXT("a trace longer than the cap in cells is left to the scan"),
		Fixture.Subsystem->ForEachTraceCandidateCell(Start, Long, 0.0f, CountVisit, VisitedCells));
	TestEqual(TEXT("without visiting a cell, so the scan does not test anything twice"), Visits, 0);

	TArray<FIntVector> RayCells;
	TestTrue(TEXT("the ray cell query answers a short ray"),
		Fixture.Subsystem->GetSpatialGridCellsForRayForTest(Start, Short, 0.0f, Fixture.Settings->SpatialGridMaxRayCells, RayCells));
	TestTrue(TEXT("with its cells"), RayCells.Num() > 0);
	const int32 SeenAfterShort = Fixture.Subsystem->NumRaySeenCellsForTest();
	TestTrue(TEXT("a short ray leaves room under the cap to tell a walk from a refusal"), SeenAfterShort < Fixture.Settings->SpatialGridMaxRayCells);

	TestFalse(TEXT("the ray cell query refuses a ray longer than the cap in cells"),
		Fixture.Subsystem->GetSpatialGridCellsForRayForTest(Start, Long, 0.0f, Fixture.Settings->SpatialGridMaxRayCells, RayCells));
	TestEqual(TEXT("with no cells"), RayCells.Num(), 0);
	TestEqual(TEXT("before it walked or recorded a cell"), Fixture.Subsystem->NumRaySeenCellsForTest(), SeenAfterShort);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassSpatialGridHugeSweepRadiusScansInsteadTest,
	"CrowdySDK.Mass.SpatialGridHugeSweepRadiusScansInstead", CrowdyMassSpatialGridTestFlags)
bool FCrowdyMassSpatialGridHugeSweepRadiusScansInsteadTest::RunTest(const FString& Parameters)
{
	FCrowdyMassSpatialGridTestFixture Fixture;
	if (!Fixture.Settings->bEnableSpatialGrid || !Fixture.ProjectCapIsTestable())
	{
		AddWarning(TEXT("The project turns the trace grid off or sets a cap too large to enumerate here."));
		return true;
	}

	const FMassEntityHandle Entity = Fixture.MakeTrackedEntity();
	const FVector EntityAt = Fixture.CellCentre() + FVector(Fixture.CellSize() * 1000.0, 0.0, 0.0);
	Fixture.Subsystem->UpdateSpatialGridEntityForTest(Entity, EntityAt, 0.0f);

	const FVector Start = Fixture.CellCentre();
	const FVector End = Start + FVector(Fixture.CellSize(), 0.0, 0.0);
	int32 Visits = 0;
	int32 VisitedCells = 0;
	TestFalse(TEXT("a huge sweep radius is left to the scan"),
		Fixture.Subsystem->ForEachTraceCandidateCell(Start, End, 1.0e30f,
			[&Visits](const FCrowdyMassTraceCandidateCell&, const bool) { ++Visits; return true; }, VisitedCells));
	TestEqual(TEXT("without visiting a cell"), Visits, 0);

	TArray<FMassEntityHandle> Candidates;
	TestFalse(TEXT("the candidate query falls back to the scan"),
		Fixture.Subsystem->GetTraceCandidateEntities(Start, End, 1.0e30f, Candidates, VisitedCells));
	TestTrue(TEXT("so an entity far off the ray is still a candidate"), Candidates.Contains(Entity));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassSpatialGridOutOfRangeTraceEndpointScansTest,
	"CrowdySDK.Mass.SpatialGridOutOfRangeTraceEndpointScans", CrowdyMassSpatialGridTestFlags)
bool FCrowdyMassSpatialGridOutOfRangeTraceEndpointScansTest::RunTest(const FString& Parameters)
{
	FCrowdyMassSpatialGridTestFixture Fixture;
	if (!Fixture.Settings->bEnableSpatialGrid || !Fixture.ProjectCapIsTestable())
	{
		AddWarning(TEXT("The project turns the trace grid off or sets a cap too large to enumerate here."));
		return true;
	}

	const FMassEntityHandle Entity = Fixture.MakeTrackedEntity();
	Fixture.Subsystem->UpdateSpatialGridEntityForTest(Entity, Fixture.CellCentre(), 0.0f);

	const FVector Start = Fixture.CellCentre();
	const FVector OutOfRange(1.0e18, 0.0, 0.0);
	const FVector NotANumber(std::numeric_limits<double>::quiet_NaN());
	int32 Visits = 0;
	int32 VisitedCells = 0;
	const auto CountVisit = [&Visits](const FCrowdyMassTraceCandidateCell&, const bool)
	{
		++Visits;
		return true;
	};

	TestFalse(TEXT("an end past the cell index range is left to the scan"),
		Fixture.Subsystem->ForEachTraceCandidateCell(Start, OutOfRange, 0.0f, CountVisit, VisitedCells));
	TestFalse(TEXT("a start that is not a number is left to the scan"),
		Fixture.Subsystem->ForEachTraceCandidateCell(NotANumber, Start, 0.0f, CountVisit, VisitedCells));
	TestEqual(TEXT("neither visits a cell"), Visits, 0);

	TArray<FMassEntityHandle> Candidates;
	TestFalse(TEXT("the candidate query falls back to the scan for an out of range end"),
		Fixture.Subsystem->GetTraceCandidateEntities(Start, OutOfRange, 0.0f, Candidates, VisitedCells));
	TestTrue(TEXT("which returns the tracked entity"), Candidates.Contains(Entity));
	return true;
}

#endif
