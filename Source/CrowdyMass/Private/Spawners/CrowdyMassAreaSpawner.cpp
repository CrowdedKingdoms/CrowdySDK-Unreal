#include "Spawners/CrowdyMassAreaSpawner.h"

#include "Components/BoxComponent.h"
#include "CrowdyMassLog.h"
#include "Engine/World.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"

ACrowdyMassAreaSpawner::ACrowdyMassAreaSpawner()
{
	PrimaryActorTick.bCanEverTick = false;

	SpawnArea = CreateDefaultSubobject<UBoxComponent>(TEXT("SpawnArea"));
	SetRootComponent(SpawnArea);
	SpawnArea->SetBoxExtent(FVector(1000.0f, 1000.0f, 100.0f));
	SpawnArea->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SpawnArea->SetCanEverAffectNavigation(false);
}

void ACrowdyMassAreaSpawner::BeginPlay()
{
	Super::BeginPlay();

	if (bSpawnOnBeginPlay)
	{
		SpawnEntities();
	}
}

void ACrowdyMassAreaSpawner::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (bDestroySpawnedOnEndPlay)
	{
		DestroySpawnedEntities();
	}

	Super::EndPlay(EndPlayReason);
}

int32 ACrowdyMassAreaSpawner::SpawnEntities()
{
	UWorld* World = GetWorld();
	UCrowdyMassEntitySubsystem* CrowdyMassSubsystem = World ? World->GetSubsystem<UCrowdyMassEntitySubsystem>() : nullptr;
	if (!CrowdyMassSubsystem)
	{
		const bool bPlayWorld = World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE);
		UE_CLOG(bPlayWorld && !bToldNoCrowdRows, LogCrowdyMass, Warning,
			TEXT("[CrowdyMassAreaSpawner] '%s' spawned nothing: this map's profile does not select the Mass backend, so it has no crowd rows. Set the Backend Class on this map's profile to the Mass backend."),
			*GetName());
		bToldNoCrowdRows = true;
		return 0;
	}

	if (bClearBeforeSpawn)
	{
		DestroySpawnedEntities();
	}

	TArray<FTransform> SpawnTransforms;
	BuildSpawnTransforms(SpawnTransforms);

	int32 SpawnedCount = 0;
	SpawnedUUIDs.Reserve(SpawnedUUIDs.Num() + SpawnTransforms.Num());
	SpawnedEntities.Reserve(SpawnedEntities.Num() + SpawnTransforms.Num());

	// Loaded once rather than per entity, and recorded on each one. Without a class an entity is live and
	// traceable but no event can be sent to it, and the refusal reads as "not learned yet" forever.
	UClass* ResolvedEntityClass = EntityClass.IsNull() ? nullptr : EntityClass.LoadSynchronous();
	UE_CLOG(EntityClass.IsNull() && !bToldNoEntityClass, LogCrowdyMass, Log,
		TEXT("[CrowdyMassAreaSpawner] '%s' has no Entity Class set, so its entities record none and no Crowdy event can be sent to them."),
		*GetName());
	bToldNoEntityClass |= EntityClass.IsNull();

	for (const FTransform& SpawnTransform : SpawnTransforms)
	{
		const FGuid UUID = FGuid::NewGuid();
		const FMassEntityHandle Entity = CrowdyMassSubsystem->SpawnRemoteEntity(
			UUID, SpawnTransform, FInstancedStruct(), ResolvedEntityClass);
		if (!Entity.IsValid())
		{
			UE_LOG(LogCrowdyMass, Warning, TEXT("[CrowdyMassAreaSpawner] '%s' stopped spawning: %d of %d entities were not spawned. Usually no Remote Entity Config resolves, or the Mass spawner subsystem is unavailable."),
				*GetName(), SpawnTransforms.Num() - SpawnedCount, SpawnTransforms.Num());
			break;
		}

		SpawnedUUIDs.Add(UUID);
		SpawnedEntities.Add(Entity);
		++SpawnedCount;
	}

	return SpawnedCount;
}

int32 ACrowdyMassAreaSpawner::RespawnEntities()
{
	DestroySpawnedEntities();
	return SpawnEntities();
}

void ACrowdyMassAreaSpawner::DestroySpawnedEntities()
{
	UWorld* World = GetWorld();
	UCrowdyMassEntitySubsystem* CrowdyMassSubsystem = World ? World->GetSubsystem<UCrowdyMassEntitySubsystem>() : nullptr;
	if (CrowdyMassSubsystem)
	{
		for (const FGuid& UUID : SpawnedUUIDs)
		{
			CrowdyMassSubsystem->DestroyRemoteEntity(UUID);
		}
	}

	SpawnedUUIDs.Reset();
	SpawnedEntities.Reset();
}

void ACrowdyMassAreaSpawner::GetSpawnedUUIDs(TArray<FGuid>& OutUUIDs) const
{
	OutUUIDs = SpawnedUUIDs;
}

void ACrowdyMassAreaSpawner::GetSpawnedEntities(TArray<FMassEntityHandle>& OutEntities) const
{
	OutEntities = SpawnedEntities;
}

void ACrowdyMassAreaSpawner::BuildSpawnTransforms(TArray<FTransform>& OutTransforms) const
{
	OutTransforms.Reset();

	if (!SpawnArea || EntityCount <= 0)
	{
		return;
	}

	const FVector LocalExtent = SpawnArea->GetUnscaledBoxExtent();
	const float AreaX = FMath::Max(LocalExtent.X * 2.0f, 1.0f);
	const float AreaY = FMath::Max(LocalExtent.Y * 2.0f, 1.0f);
	const float AspectRatio = AreaX / AreaY;
	const int32 Columns = FMath::Max(1, FMath::CeilToInt(FMath::Sqrt(static_cast<float>(EntityCount) * AspectRatio)));
	const int32 Rows = FMath::Max(1, FMath::CeilToInt(static_cast<float>(EntityCount) / static_cast<float>(Columns)));
	const float CellSizeX = AreaX / static_cast<float>(Columns);
	const float CellSizeY = AreaY / static_cast<float>(Rows);
	const float ClampedJitter = FMath::Clamp(PositionJitter, 0.0f, 1.0f);

	FRandomStream RandomStream;
	if (bUseDeterministicSeed)
	{
		RandomStream.Initialize(RandomSeed);
	}
	else
	{
		RandomStream.GenerateNewSeed();
	}

	const FTransform AreaTransform = SpawnArea->GetComponentTransform();
	OutTransforms.Reserve(EntityCount);

	for (int32 Index = 0; Index < EntityCount; ++Index)
	{
		const int32 Column = Index % Columns;
		const int32 Row = Index / Columns;

		float LocalX = -LocalExtent.X + CellSizeX * (static_cast<float>(Column) + 0.5f);
		float LocalY = -LocalExtent.Y + CellSizeY * (static_cast<float>(Row) + 0.5f);

		if (ClampedJitter > 0.0f)
		{
			LocalX += RandomStream.FRandRange(-0.5f, 0.5f) * CellSizeX * ClampedJitter;
			LocalY += RandomStream.FRandRange(-0.5f, 0.5f) * CellSizeY * ClampedJitter;
			LocalX = FMath::Clamp(LocalX, -LocalExtent.X, LocalExtent.X);
			LocalY = FMath::Clamp(LocalY, -LocalExtent.Y, LocalExtent.Y);
		}

		FVector WorldLocation = AreaTransform.TransformPosition(FVector(LocalX, LocalY, 0.0f));
		if (bProjectToGround)
		{
			ProjectSpawnLocationToGround(WorldLocation);
		}

		FRotator WorldRotation = SpawnArea->GetComponentRotation();
		if (bRandomizeYaw)
		{
			WorldRotation.Yaw += RandomStream.FRandRange(0.0f, 360.0f);
		}

		OutTransforms.Emplace(WorldRotation, WorldLocation, FVector::OneVector);
	}
}

bool ACrowdyMassAreaSpawner::ProjectSpawnLocationToGround(FVector& InOutLocation) const
{
	UWorld* World = GetWorld();
	if (!World || GroundTraceDistance <= 0.0f)
	{
		return false;
	}

	const FVector TraceStart = InOutLocation + FVector::UpVector * GroundTraceDistance;
	const FVector TraceEnd = InOutLocation - FVector::UpVector * GroundTraceDistance;

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(CrowdyMassAreaSpawnerGroundTrace), false);
	QueryParams.AddIgnoredActor(this);

	FHitResult Hit;
	if (World->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, GroundTraceChannel.GetValue(), QueryParams))
	{
		InOutLocation = Hit.Location;
		return true;
	}

	return false;
}
