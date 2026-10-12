#include "Trace/CrowdyMassTraceBlueprintLibrary.h"

#include "Animation/AnimSequence.h"
#include "Assets/CrowdyMassHitZoneTable.h"
#include "Animation/AnimationAsset.h"
#include "Animation/AnimSequenceTransformProviderData.h"
#include "Animation/AnimCompressionTypes.h"
#include "BoneIndices.h"
#include "Components/InstancedSkinnedMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "CrowdyMassLog.h"
#include "DrawDebugHelpers.h"
#include "Engine/ActorInstanceHandle.h"
#include "Engine/CollisionProfile.h"
#include "Engine/Engine.h"
#include "Engine/OverlapResult.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/World.h"
#include "Fragments/CrowdyMassFragments.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "KismetTraceUtils.h"
#include "MassActorSubsystem.h"
#include "MassCommonFragments.h"
#include "MassEntityManager.h"
#include "MassEntitySubsystem.h"
#include "MassEntityView.h"
#include "MassRepresentationFragments.h"
#include "MassRepresentationSubsystem.h"
#include "MassRepresentationTypes.h"
#include "MassSkinnedMeshRepresentationTypes.h"
#include "MassVisualizationComponent.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "PhysicsEngine/BoxElem.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "PhysicsEngine/SphereElem.h"
#include "PhysicsEngine/SphylElem.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Settings/CrowdyMassDeveloperSettings.h"
#include "Mass/CrowdyMassEntity.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"
#include "UObject/ObjectKey.h"

namespace CrowdyMassTrace
{
struct FAnimSample
{
	TWeakObjectPtr<UAnimSequence> AnimSequence;
	float Position = 0.0f;
};

struct FInternalHit
{
	bool bHit = false;
	float Distance = TNumericLimits<float>::Max();
	FVector ImpactPoint = FVector::ZeroVector;
	FVector ImpactNormal = FVector::UpVector;
	FName BoneName = NAME_None;
	FMassEntityHandle Entity;
	FGuid UUID;
	TWeakObjectPtr<UInstancedSkinnedMeshComponent> Component;
	int32 InstanceIndex = INDEX_NONE;
};

struct FTraceDebugStats
{
	bool bWorldHit = false;
	float TraceLength = 0.0f;
	float MaxMassDistance = 0.0f;

	int32 TrackedEntityCount = 0;
	bool bSpatialGridUsed = false;
	int32 SpatialGridVisitedCellCount = 0;
	int32 SpatialGridCandidateCount = 0;
	int32 ValidEntityCount = 0;
	int32 MissingFragmentCount = 0;
	int32 MissingRepresentationSharedFragmentCount = 0;
	int32 TraceCollisionRejectedCount = 0;
	int32 NonSkinnedRepresentationCount = 0;
	int32 SkinnedRepresentationCount = 0;
	int32 InvalidSkinnedMeshInfoCount = 0;
	int32 MissingLODRangeCount = 0;
	int32 ComponentRefCount = 0;
	int32 InvalidComponentCount = 0;
	int32 ResolvedComponentCount = 0;
	int32 MissingSharedDataCount = 0;
	int32 MissingInstanceMappingCount = 0;
	int32 InvalidPrimitiveIdCount = 0;
	int32 MappedInstanceCount = 0;
	int32 MissingSkinnedAssetCount = 0;
	int32 MissingPhysicsAssetCount = 0;
	int32 FailedInstanceTransformCount = 0;
	int32 BoundsRejectedCount = 0;
	int32 BoundsCandidateCount = 0;
	int32 AnimSampleResolvedCount = 0;
	int32 AnimSampleMissingCount = 0;
	int32 PhysicsAssetTestCount = 0;
	int32 PhysicsShapeTestCount = 0;
	int32 PhysicsAssetNoShapeCount = 0;
	int32 PhysicsShapeMissCount = 0;
	int32 MassHitCount = 0;
};

struct FCandidateShapeDebug
{
	UWorld* World = nullptr;
	bool bEnabled = false;
	bool bPersistent = false;
	float Lifetime = 0.0f;
	FColor Color = FColor::Cyan;
	float Thickness = 1.0f;

	bool ShouldDraw() const
	{
		return bEnabled && World;
	}
};

struct FSpatialGridDebug
{
	UWorld* World = nullptr;
	bool bEnabled = false;
	bool bPersistent = false;
	float Lifetime = 0.0f;
	FColor Color = FColor::Red;
	float Thickness = 1.0f;
	bool bDrawTwoDimensionalCells = false;
	bool bHasTwoDimensionalBaseZ = false;
	double TwoDimensionalBaseZ = 0.0;
	float TwoDimensionalCornerLineHalfLength = 100000.0f;

	bool ShouldDraw() const
	{
		return bEnabled && World;
	}
};

static FString CollisionChannelToString(ECollisionChannel CollisionChannel);

enum class ECrowdyMassTraceShapeType : uint8
{
	Line,
	Sphere,
	Box,
	Capsule
};

struct FCrowdyMassTraceShape
{
	ECrowdyMassTraceShapeType Type = ECrowdyMassTraceShapeType::Line;
	float Radius = 0.0f;
	float HalfHeight = 0.0f;
	FVector HalfSize = FVector::ZeroVector;
	FRotator Orientation = FRotator::ZeroRotator;

	static FCrowdyMassTraceShape MakeLine()
	{
		return FCrowdyMassTraceShape();
	}

	static FCrowdyMassTraceShape MakeSphere(const float InRadius)
	{
		FCrowdyMassTraceShape Shape;
		Shape.Type = ECrowdyMassTraceShapeType::Sphere;
		Shape.Radius = FMath::Max(InRadius, 0.0f);
		return Shape;
	}

	static FCrowdyMassTraceShape MakeBox(const FVector& InHalfSize, const FRotator& InOrientation)
	{
		FCrowdyMassTraceShape Shape;
		Shape.Type = ECrowdyMassTraceShapeType::Box;
		Shape.HalfSize = InHalfSize.GetAbs();
		Shape.Orientation = InOrientation;
		return Shape;
	}

	static FCrowdyMassTraceShape MakeCapsule(const float InRadius, const float InHalfHeight)
	{
		FCrowdyMassTraceShape Shape;
		Shape.Type = ECrowdyMassTraceShapeType::Capsule;
		Shape.Radius = FMath::Max(InRadius, 0.0f);
		Shape.HalfHeight = FMath::Max(InHalfHeight, 0.0f);
		return Shape;
	}

	bool IsLine() const
	{
		return Type == ECrowdyMassTraceShapeType::Line;
	}

	FQuat GetRotation() const
	{
		return Type == ECrowdyMassTraceShapeType::Box ? Orientation.Quaternion() : FQuat::Identity;
	}

	FCollisionShape MakeCollisionShape() const
	{
		switch (Type)
		{
		case ECrowdyMassTraceShapeType::Sphere:
			return FCollisionShape::MakeSphere(Radius);
		case ECrowdyMassTraceShapeType::Box:
			return FCollisionShape::MakeBox(HalfSize);
		case ECrowdyMassTraceShapeType::Capsule:
			return FCollisionShape::MakeCapsule(Radius, HalfHeight);
		default:
			return FCollisionShape::MakeSphere(0.0f);
		}
	}

	float GetMassInflationRadius() const
	{
		switch (Type)
		{
		case ECrowdyMassTraceShapeType::Sphere:
			return Radius;
		case ECrowdyMassTraceShapeType::Box:
			return HalfSize.Size();
		case ECrowdyMassTraceShapeType::Capsule:
			return Radius + HalfHeight;
		default:
			return 0.0f;
		}
	}
};

enum class ECrowdyMassTraceFilterType : uint8
{
	Channel,
	ObjectTypes,
	Profile
};

struct FCrowdyMassTraceFilter
{
	ECrowdyMassTraceFilterType Type = ECrowdyMassTraceFilterType::Channel;
	ECollisionChannel TraceChannel = ECC_Visibility;
	FName ProfileName = NAME_None;
	FCollisionObjectQueryParams ObjectParams;
	TArray<ECollisionChannel> ObjectChannels;
	FCollisionResponseParams ProfileResponseParams;
	bool bIsValid = true;
	bool bProfileResolved = false;
};

static FCrowdyMassTraceFilter MakeChannelTraceFilter(const ETraceTypeQuery TraceChannel)
{
	FCrowdyMassTraceFilter Filter;
	Filter.Type = ECrowdyMassTraceFilterType::Channel;
	Filter.TraceChannel = UEngineTypes::ConvertToCollisionChannel(TraceChannel);
	return Filter;
}

static FCrowdyMassTraceFilter MakeObjectTraceFilter(const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes)
{
	FCrowdyMassTraceFilter Filter;
	Filter.Type = ECrowdyMassTraceFilterType::ObjectTypes;

	for (const TEnumAsByte<EObjectTypeQuery> ObjectType : ObjectTypes)
	{
		const ECollisionChannel CollisionChannel = UCollisionProfile::Get()->ConvertToCollisionChannel(false, ObjectType);
		Filter.ObjectParams.AddObjectTypesToQuery(CollisionChannel);
		Filter.ObjectChannels.AddUnique(CollisionChannel);
	}

	Filter.bIsValid = Filter.ObjectParams.IsValid();
	return Filter;
}

static FCrowdyMassTraceFilter MakeProfileTraceFilter(const FName ProfileName)
{
	FCrowdyMassTraceFilter Filter;
	Filter.Type = ECrowdyMassTraceFilterType::Profile;
	Filter.ProfileName = ProfileName;
	Filter.bProfileResolved = UCollisionProfile::GetChannelAndResponseParams(ProfileName, Filter.TraceChannel, Filter.ProfileResponseParams);
	return Filter;
}

static FString TraceFilterToString(const FCrowdyMassTraceFilter& Filter)
{
	switch (Filter.Type)
	{
	case ECrowdyMassTraceFilterType::ObjectTypes:
		return TEXT("object query");
	case ECrowdyMassTraceFilterType::Profile:
		return FString::Printf(TEXT("profile '%s'"), *Filter.ProfileName.ToString());
	default:
		return CollisionChannelToString(Filter.TraceChannel);
	}
}

static FString RepresentationTypeToString(const EMassRepresentationType RepresentationType)
{
	switch (RepresentationType)
	{
	case EMassRepresentationType::None:
		return TEXT("None");
	case EMassRepresentationType::StaticMeshInstance:
		return TEXT("StaticMeshInstance");
	case EMassRepresentationType::HighResSpawnedActor:
		return TEXT("HighResSpawnedActor");
	case EMassRepresentationType::LowResSpawnedActor:
		return TEXT("LowResSpawnedActor");
	case EMassRepresentationType::SkinnedMeshInstance:
		return TEXT("SkinnedMeshInstance");
	default:
		return FString::Printf(TEXT("Unknown(%d)"), static_cast<int32>(RepresentationType));
	}
}

static bool GetSelfActorToIgnore(const UObject* WorldContextObject, AActor*& OutActor)
{
	OutActor = nullptr;

	if (AActor* Actor = const_cast<AActor*>(Cast<AActor>(WorldContextObject)))
	{
		OutActor = Actor;
		return true;
	}

	if (const UActorComponent* Component = Cast<UActorComponent>(WorldContextObject))
	{
		OutActor = Component->GetOwner();
		return OutActor != nullptr;
	}

	return false;
}

static void ConfigureQueryParams(FCollisionQueryParams& QueryParams, const TArray<AActor*>& ActorsToIgnore, const UObject* WorldContextObject, const bool bIgnoreSelf)
{
	QueryParams.bReturnPhysicalMaterial = true;
	QueryParams.bReturnFaceIndex = !UPhysicsSettings::Get()->bSuppressFaceRemapTable;
	QueryParams.AddIgnoredActors(ActorsToIgnore);

	if (bIgnoreSelf)
	{
		AActor* SelfActor = nullptr;
		if (GetSelfActorToIgnore(WorldContextObject, SelfActor))
		{
			QueryParams.AddIgnoredActor(SelfActor);
		}
	}
}

static float GetDebugLifetime(const EDrawDebugTrace::Type DrawDebugType, const float DrawTime)
{
	if (DrawDebugType == EDrawDebugTrace::ForDuration)
	{
		return DrawTime;
	}

	if (DrawDebugType == EDrawDebugTrace::Persistent)
	{
		return -1.0f;
	}

	return 0.0f;
}

static bool GetPossessedPawnRootZ(UWorld* World, double& OutRootZ)
{
	if (!World)
	{
		return false;
	}

	if (const APlayerController* PlayerController = World->GetFirstPlayerController())
	{
		if (const APawn* Pawn = PlayerController->GetPawn())
		{
			OutRootZ = Pawn->GetActorLocation().Z;
			return true;
		}
	}

	return false;
}

static void ConfigureSpatialGridDebug(FSpatialGridDebug& GridDebug, UWorld* World, const bool bDrawSpatialGridCells, const EDrawDebugTrace::Type DrawDebugType, const float DrawTime)
{
	GridDebug.World = World;
	GridDebug.bEnabled = bDrawSpatialGridCells && DrawDebugType != EDrawDebugTrace::None;
	GridDebug.bPersistent = DrawDebugType == EDrawDebugTrace::Persistent;
	GridDebug.Lifetime = GetDebugLifetime(DrawDebugType, DrawTime);

	if (const UCrowdyMassDeveloperSettings* Settings = GetDefault<UCrowdyMassDeveloperSettings>())
	{
		GridDebug.Color = Settings->SpatialGridDebugColor;
		GridDebug.Thickness = FMath::Max(Settings->SpatialGridDebugLineThickness, 0.0f);
		GridDebug.bDrawTwoDimensionalCells = Settings->SpatialGridMode == ECrowdyMassSpatialGridMode::TwoDimensional;
		GridDebug.TwoDimensionalCornerLineHalfLength = FMath::Max(Settings->SpatialGridDebugTwoDimensionalCornerLineHalfLength, 0.0f);
		if (GridDebug.bDrawTwoDimensionalCells)
		{
			GridDebug.bHasTwoDimensionalBaseZ = GetPossessedPawnRootZ(World, GridDebug.TwoDimensionalBaseZ);
		}
	}
}

static void DrawSpatialGridTwoDimensionalCell(const FBox& Bounds, const FSpatialGridDebug& GridDebug)
{
	const FVector Min = Bounds.Min;
	const FVector Max = Bounds.Max;
	const double BaseZ = GridDebug.bHasTwoDimensionalBaseZ ? GridDebug.TwoDimensionalBaseZ : Min.Z;
	const double CornerLineHalfLength = GridDebug.TwoDimensionalCornerLineHalfLength;
	const double BottomZ = BaseZ - CornerLineHalfLength;
	const double TopZ = BaseZ + CornerLineHalfLength;

	const FVector BaseA(Min.X, Min.Y, BaseZ);
	const FVector BaseB(Max.X, Min.Y, BaseZ);
	const FVector BaseC(Max.X, Max.Y, BaseZ);
	const FVector BaseD(Min.X, Max.Y, BaseZ);

	const FVector BottomA(Min.X, Min.Y, BottomZ);
	const FVector BottomB(Max.X, Min.Y, BottomZ);
	const FVector BottomC(Max.X, Max.Y, BottomZ);
	const FVector BottomD(Min.X, Max.Y, BottomZ);

	const FVector TopA(Min.X, Min.Y, TopZ);
	const FVector TopB(Max.X, Min.Y, TopZ);
	const FVector TopC(Max.X, Max.Y, TopZ);
	const FVector TopD(Min.X, Max.Y, TopZ);

	DrawDebugLine(GridDebug.World, BaseA, BaseB, GridDebug.Color, GridDebug.bPersistent, GridDebug.Lifetime, 0, GridDebug.Thickness);
	DrawDebugLine(GridDebug.World, BaseB, BaseC, GridDebug.Color, GridDebug.bPersistent, GridDebug.Lifetime, 0, GridDebug.Thickness);
	DrawDebugLine(GridDebug.World, BaseC, BaseD, GridDebug.Color, GridDebug.bPersistent, GridDebug.Lifetime, 0, GridDebug.Thickness);
	DrawDebugLine(GridDebug.World, BaseD, BaseA, GridDebug.Color, GridDebug.bPersistent, GridDebug.Lifetime, 0, GridDebug.Thickness);

	DrawDebugLine(GridDebug.World, BaseA, TopA, GridDebug.Color, GridDebug.bPersistent, GridDebug.Lifetime, 0, GridDebug.Thickness);
	DrawDebugLine(GridDebug.World, BaseB, TopB, GridDebug.Color, GridDebug.bPersistent, GridDebug.Lifetime, 0, GridDebug.Thickness);
	DrawDebugLine(GridDebug.World, BaseC, TopC, GridDebug.Color, GridDebug.bPersistent, GridDebug.Lifetime, 0, GridDebug.Thickness);
	DrawDebugLine(GridDebug.World, BaseD, TopD, GridDebug.Color, GridDebug.bPersistent, GridDebug.Lifetime, 0, GridDebug.Thickness);

	DrawDebugLine(GridDebug.World, BaseA, BottomA, GridDebug.Color, GridDebug.bPersistent, GridDebug.Lifetime, 0, GridDebug.Thickness);
	DrawDebugLine(GridDebug.World, BaseB, BottomB, GridDebug.Color, GridDebug.bPersistent, GridDebug.Lifetime, 0, GridDebug.Thickness);
	DrawDebugLine(GridDebug.World, BaseC, BottomC, GridDebug.Color, GridDebug.bPersistent, GridDebug.Lifetime, 0, GridDebug.Thickness);
	DrawDebugLine(GridDebug.World, BaseD, BottomD, GridDebug.Color, GridDebug.bPersistent, GridDebug.Lifetime, 0, GridDebug.Thickness);
}

static void DrawSpatialGridCell(const FBox& Bounds, const FSpatialGridDebug& GridDebug)
{
	if (!GridDebug.ShouldDraw())
	{
		return;
	}

	if (!Bounds.IsValid)
	{
		return;
	}

	if (GridDebug.bDrawTwoDimensionalCells)
	{
		DrawSpatialGridTwoDimensionalCell(Bounds, GridDebug);
	}
	else
	{
		DrawDebugBox(GridDebug.World, Bounds.GetCenter(), Bounds.GetExtent(), FQuat::Identity, GridDebug.Color, GridDebug.bPersistent, GridDebug.Lifetime, 0, GridDebug.Thickness);
	}
}

static void DrawSpatialGridCells(const TArray<FBox>& CellBounds, const FSpatialGridDebug& GridDebug)
{
	if (!GridDebug.ShouldDraw())
	{
		return;
	}

	for (const FBox& Bounds : CellBounds)
	{
		DrawSpatialGridCell(Bounds, GridDebug);
	}
}

static void DrawTraceDebug(UWorld& World, const FCrowdyMassTraceShape& Shape, const FVector& Start, const FVector& End, const FCrowdyMassTraceHit& Hit, const EDrawDebugTrace::Type DrawDebugType, const FLinearColor& TraceColor, const FLinearColor& TraceHitColor, const float DrawTime)
{
	// The DrawDebug*TraceSingle helpers (KismetTraceUtils.h) exist only under ENABLE_DRAW_DEBUG, which is 0 in
	// Shipping/Test; guard the call so this module compiles in those configs (mirrors KismetSystemLibrary).
#if ENABLE_DRAW_DEBUG
	switch (Shape.Type)
	{
	case ECrowdyMassTraceShapeType::Sphere:
		DrawDebugSphereTraceSingle(&World, Start, End, Shape.Radius, DrawDebugType, Hit.bBlockingHit, Hit.HitResult, TraceColor, TraceHitColor, DrawTime);
		break;
	case ECrowdyMassTraceShapeType::Box:
		DrawDebugBoxTraceSingle(&World, Start, End, Shape.HalfSize, Shape.Orientation, DrawDebugType, Hit.bBlockingHit, Hit.HitResult, TraceColor, TraceHitColor, DrawTime);
		break;
	case ECrowdyMassTraceShapeType::Capsule:
		DrawDebugCapsuleTraceSingle(&World, Start, End, Shape.Radius, Shape.HalfHeight, DrawDebugType, Hit.bBlockingHit, Hit.HitResult, TraceColor, TraceHitColor, DrawTime);
		break;
	default:
		DrawDebugLineTraceSingle(&World, Start, End, DrawDebugType, Hit.bBlockingHit, Hit.HitResult, TraceColor, TraceHitColor, DrawTime);
		break;
	}
#endif // ENABLE_DRAW_DEBUG
}

static void DrawTraceDebug(UWorld& World, const FCrowdyMassTraceShape& Shape, const FVector& Start, const FVector& End, const TArray<FCrowdyMassTraceHit>& Hits, const EDrawDebugTrace::Type DrawDebugType, const FLinearColor& TraceColor, const FLinearColor& TraceHitColor, const float DrawTime)
{
	// The DrawDebug*TraceMulti helpers exist only under ENABLE_DRAW_DEBUG (0 in Shipping/Test); the whole body
	// is debug-draw, so guard it entirely so the module compiles in those configs.
#if ENABLE_DRAW_DEBUG
	TArray<FHitResult> HitResults;
	HitResults.Reserve(Hits.Num());

	bool bAnyBlockingHit = false;
	for (const FCrowdyMassTraceHit& Hit : Hits)
	{
		HitResults.Add(Hit.HitResult);
		bAnyBlockingHit |= Hit.bBlockingHit;
	}

	switch (Shape.Type)
	{
	case ECrowdyMassTraceShapeType::Sphere:
		DrawDebugSphereTraceMulti(&World, Start, End, Shape.Radius, DrawDebugType, bAnyBlockingHit, HitResults, TraceColor, TraceHitColor, DrawTime);
		break;
	case ECrowdyMassTraceShapeType::Box:
		DrawDebugBoxTraceMulti(&World, Start, End, Shape.HalfSize, Shape.Orientation, DrawDebugType, bAnyBlockingHit, HitResults, TraceColor, TraceHitColor, DrawTime);
		break;
	case ECrowdyMassTraceShapeType::Capsule:
		DrawDebugCapsuleTraceMulti(&World, Start, End, Shape.Radius, Shape.HalfHeight, DrawDebugType, bAnyBlockingHit, HitResults, TraceColor, TraceHitColor, DrawTime);
		break;
	default:
		DrawDebugLineTraceMulti(&World, Start, End, DrawDebugType, bAnyBlockingHit, HitResults, TraceColor, TraceHitColor, DrawTime);
		break;
	}
#endif // ENABLE_DRAW_DEBUG
}

static FString CollisionChannelToString(const ECollisionChannel CollisionChannel)
{
	if (const UEnum* CollisionChannelEnum = StaticEnum<ECollisionChannel>())
	{
		return CollisionChannelEnum->GetNameStringByValue(CollisionChannel);
	}

	return FString::FromInt(CollisionChannel);
}

/**
 * The hit zone lookup this world's trace results are resolved against.
 *
 * World-scoped, on the Crowdy Mass subsystem, so two PIE sessions and the two worlds alive during level
 * travel each answer from their own table. A table held in a process-wide static would let one session's
 * assignment decide the other's zones and would survive into the next PIE run, which is a state no cold
 * start can reproduce.
 */
static const FCrowdyMassResolvedHitZoneTable& GetResolvedHitZoneTable(const UWorld* World)
{
	if (const UCrowdyMassEntitySubsystem* CrowdySubsystem = World ? World->GetSubsystem<UCrowdyMassEntitySubsystem>() : nullptr)
	{
		return CrowdySubsystem->GetResolvedHitZoneTable();
	}

	// A world with no Crowdy Mass subsystem holds no crowd and no table, so every bone reports the
	// built-in default. Const and never written, so unlike a mutable static this shares nothing.
	static const FCrowdyMassResolvedHitZoneTable BuiltInDefault;
	return BuiltInDefault;
}

static void ApplyHitZone(const FCrowdyMassResolvedHitZoneTable& HitZones, FCrowdyMassTraceHit& OutHit)
{
	HitZones.Resolve(OutHit.BoneName, OutHit.HitZoneName, OutHit.DamageMultiplier);
}

bool DoesTraceCollisionFragmentAllowParticipation(const FCrowdyMassTraceCollisionFragment* TraceCollisionFragment)
{
	return TraceCollisionFragment && TraceCollisionFragment->bEnableTraceCollision;
}

bool ShouldTraceEntityAsRow(const FMassRepresentationFragment& Representation, const bool bReportedByWorldQuery)
{
	// The one reason to leave an entity out: the world trace already reported it, joined back to this
	// same entity, so testing the row would report one player twice.
	if (bReportedByWorldQuery)
	{
		return false;
	}

	// Nothing has reported this entity yet, so it traces as a row whenever it has one. The current
	// representation covers the crowd being drawn as rows and the whole of an asynchronous promotion, which
	// rolls back to the mesh representation while no actor is in hand; the previous one covers the frames
	// after the actor arrives, where it is held disabled and the row it is replacing is still on screen.
	return Representation.CurrentRepresentation == EMassRepresentationType::SkinnedMeshInstance
		|| Representation.PrevRepresentation == EMassRepresentationType::SkinnedMeshInstance;
}

bool JoinEntityIdentityToTraceHit(const FMassEntityHandle Entity, const FCrowdyRemoteIdentityFragment* IdentityFragment, UWorld* World, FCrowdyMassTraceHit& OutHit)
{
	if (!Entity.IsSet() || !IdentityFragment || !IdentityFragment->UUID.IsValid())
	{
		return false;
	}

	OutHit.bHitMassEntity = true;
	OutHit.bHitCrowdyEntity = true;
	OutHit.UUID = IdentityFragment->UUID;
	OutHit.Entity = Entity;
	OutHit.Target.Entity = Entity;
	OutHit.Target.World = World;
	return true;
}

static bool JoinEntityIdentityToOverlapResult(const FMassEntityHandle Entity, const FCrowdyRemoteIdentityFragment* IdentityFragment, UWorld* World, FCrowdyMassOverlapResult& OutOverlap)
{
	if (!Entity.IsSet() || !IdentityFragment || !IdentityFragment->UUID.IsValid())
	{
		return false;
	}

	OutOverlap.bOverlapMassEntity = true;
	OutOverlap.bOverlapCrowdyEntity = true;
	OutOverlap.UUID = IdentityFragment->UUID;
	OutOverlap.Entity = Entity;
	OutOverlap.Target.Entity = Entity;
	OutOverlap.Target.World = World;
	return true;
}

void RecordWorldReportedEntity(const FCrowdyMassTraceHit& WorldHit, FCrowdyMassWorldReportedEntities& OutReported)
{
	// Only a result that carries the entity's own identity counts. A hit that landed on an entity's actor
	// and could not be named from the entity's fragment is reported as an ordinary actor, so the entity
	// itself was not reported and its row is still the only way to hit it.
	if (!WorldHit.bHitMassEntity)
	{
		return;
	}

	OutReported.Add(WorldHit.Entity);
}

void RecordWorldReportedEntity(const FCrowdyMassOverlapResult& WorldOverlap, FCrowdyMassWorldReportedEntities& OutReported)
{
	if (!WorldOverlap.bOverlapMassEntity)
	{
		return;
	}

	OutReported.Add(WorldOverlap.Entity);
}

// Every one of these takes the fragment rather than the entity on purpose. Resolving a fragment is an
// entity-storage indirection plus two hash probes, broadphase candidates span archetypes so those probes
// are cold, and the filter used to resolve the same fragment three times for one answer. The caller
// resolves it once and the whole decision reads from that pointer.
static ECollisionChannel GetEntityObjectType(const FCrowdyMassTraceCollisionFragment* TraceCollisionFragment)
{
	return TraceCollisionFragment ? TraceCollisionFragment->ObjectType.GetValue() : ECC_WorldDynamic;
}

static bool DoesEntityBlockTraceChannel(const FCrowdyMassTraceCollisionFragment* TraceCollisionFragment, const ECollisionChannel CollisionChannel)
{
	if (!CrowdyMassTrace::DoesTraceCollisionFragmentAllowParticipation(TraceCollisionFragment))
	{
		return false;
	}

	return TraceCollisionFragment->ResponseToChannels.GetResponse(CollisionChannel) == ECR_Block;
}

static bool DoesEntityPassTraceFilter(const FCrowdyMassTraceCollisionFragment* TraceCollisionFragment, const FCrowdyMassTraceFilter& Filter)
{
	if (!CrowdyMassTrace::DoesTraceCollisionFragmentAllowParticipation(TraceCollisionFragment))
	{
		return false;
	}

	// The object type is read inside the two branches that use it rather than above the switch. A channel
	// filter, which is the common one, does not use it at all, so computing it first meant every
	// candidate of every channel trace paid for a value that was thrown away.
	switch (Filter.Type)
	{
	case ECrowdyMassTraceFilterType::ObjectTypes:
		return Filter.ObjectChannels.Contains(GetEntityObjectType(TraceCollisionFragment));

	case ECrowdyMassTraceFilterType::Profile:
		if (Filter.bProfileResolved && Filter.ProfileResponseParams.CollisionResponse.GetResponse(GetEntityObjectType(TraceCollisionFragment)) != ECR_Block)
		{
			return false;
		}
		return DoesEntityBlockTraceChannel(TraceCollisionFragment, Filter.TraceChannel);

	default:
		return DoesEntityBlockTraceChannel(TraceCollisionFragment, Filter.TraceChannel);
	}
}

static void PopulateCrowdyEntityInfoFromActor(const AActor* Actor, FCrowdyMassTraceHit& OutHit)
{
	if (!Actor)
	{
		return;
	}

	FGuid NetID;
	if (const UCrowdyEntityComponent* CrowdyEntityComponent = Actor->FindComponentByClass<UCrowdyEntityComponent>())
	{
		NetID = CrowdyEntityComponent->GetNetID();
	}

	if (!NetID.IsValid())
	{
		if (const UWorld* World = Actor->GetWorld())
		{
			if (const UCrowdyEntitySubsystem* EntitySubsystem = World->GetSubsystem<UCrowdyEntitySubsystem>())
			{
				NetID = EntitySubsystem->FindEntityID(Actor);
			}
		}
	}

	if (NetID.IsValid())
	{
		OutHit.UUID = NetID;
		OutHit.bHitCrowdyEntity = true;
	}
}

static void PopulateCrowdyEntityInfoFromActor(const AActor* Actor, FCrowdyMassOverlapResult& OutOverlap)
{
	if (!Actor)
	{
		return;
	}

	FGuid NetID;
	if (const UCrowdyEntityComponent* CrowdyEntityComponent = Actor->FindComponentByClass<UCrowdyEntityComponent>())
	{
		NetID = CrowdyEntityComponent->GetNetID();
	}

	if (!NetID.IsValid())
	{
		if (const UWorld* World = Actor->GetWorld())
		{
			if (const UCrowdyEntitySubsystem* EntitySubsystem = World->GetSubsystem<UCrowdyEntitySubsystem>())
			{
				NetID = EntitySubsystem->FindEntityID(Actor);
			}
		}
	}

	if (NetID.IsValid())
	{
		OutOverlap.UUID = NetID;
		OutOverlap.bOverlapCrowdyEntity = true;
	}
}

static bool TryResolveAnimSample(const UMassRepresentationSubsystem& RepresentationSubsystem, const FMassEntityHandle Entity, UInstancedSkinnedMeshComponent& Component, const FPrimitiveInstanceId InstanceId, FAnimSample& OutSample)
{
	OutSample = FAnimSample();

	if (const UMassVisualizationComponent* VisualizationComponent = RepresentationSubsystem.GetVisualizationComponent())
	{
		if (const FMassSkinnedMeshResolvedAnimState* AnimState = VisualizationComponent->FindResolvedAnimState(Entity))
		{
			if (UAnimSequence* AnimSequence = AnimState->AnimSequence.Get())
			{
				OutSample.AnimSequence = AnimSequence;
				OutSample.Position = AnimState->Position;
				return true;
			}
		}
	}

	int32 AnimationIndex = INDEX_NONE;
	if (!Component.GetInstanceAnimationIndex(InstanceId, AnimationIndex))
	{
		return false;
	}

	if (UAnimSequenceTransformProviderDataInstance* ProviderInstance = Cast<UAnimSequenceTransformProviderDataInstance>(Component.GetTransformProvider()))
	{
		const int32 SequenceIndex = ProviderInstance->GetSequenceIndex(AnimationIndex, 0);
		const TArray<FAnimSequenceTransformProviderSequence>& Sequences = ProviderInstance->GetSequences();
		if (Sequences.IsValidIndex(SequenceIndex) && Sequences[SequenceIndex].Sequence)
		{
			OutSample.AnimSequence = Sequences[SequenceIndex].Sequence.Get();
			OutSample.Position = ProviderInstance->GetPosition(AnimationIndex, 0).TargetPosition;
			return true;
		}
	}
	else if (UAnimSequenceTransformProviderData* Provider = Cast<UAnimSequenceTransformProviderData>(Component.GetTransformProvider()))
	{
		const TArray<FAnimSequenceTransformProviderSequence>& Sequences = Provider->GetSequences();
		if (Sequences.IsValidIndex(AnimationIndex) && Sequences[AnimationIndex].Sequence)
		{
			OutSample.AnimSequence = Sequences[AnimationIndex].Sequence.Get();
			OutSample.Position = Sequences[AnimationIndex].Position;
			return true;
		}
	}

	return false;
}

static bool GetAnimatedComponentSpaceBoneTransform(
	const USkinnedAsset& SkinnedAsset,
	const UAnimSequence* AnimSequence,
	const float Position,
	const int32 BoneIndex,
	TMap<int32, FTransform>& ComponentSpaceCache,
	FTransform& OutComponentSpaceTransform)
{
	if (const FTransform* CachedTransform = ComponentSpaceCache.Find(BoneIndex))
	{
		OutComponentSpaceTransform = *CachedTransform;
		return true;
	}

	const FReferenceSkeleton& RefSkeleton = SkinnedAsset.GetRefSkeleton();
	const TArray<FTransform>& RefBonePose = RefSkeleton.GetRefBonePose();
	if (!RefBonePose.IsValidIndex(BoneIndex))
	{
		return false;
	}

	FTransform LocalTransform = RefBonePose[BoneIndex];
	const FName BoneName = RefSkeleton.GetBoneName(BoneIndex);

	if (AnimSequence && AnimSequence->GetSkeleton())
	{
		const int32 AnimBoneIndex = AnimSequence->GetSkeleton()->GetReferenceSkeleton().FindBoneIndex(BoneName);
		if (AnimBoneIndex != INDEX_NONE)
		{
			FTransform AnimatedLocalTransform = LocalTransform;
			const FAnimExtractContext ExtractContext(Position, AnimSequence->bEnableRootMotion, FDeltaTimeRecord(), true);
			AnimSequence->GetBoneTransform(AnimatedLocalTransform, FSkeletonPoseBoneIndex(AnimBoneIndex), ExtractContext, false);
			if (AnimatedLocalTransform.IsValid())
			{
				LocalTransform = AnimatedLocalTransform;
			}
		}
	}

	const int32 ParentIndex = RefSkeleton.GetParentIndex(BoneIndex);
	if (ParentIndex == INDEX_NONE && AnimSequence && (AnimSequence->bForceRootLock || AnimSequence->bEnableRootMotion))
	{
		const FAnimExtractContext FirstFrameContext(0.0);
		const FTransform AnimFirstFrame = AnimSequence->ExtractRootTrackTransform(FirstFrameContext, nullptr);
		const FRootMotionReset RootMotionReset(
			AnimSequence->bEnableRootMotion,
			AnimSequence->RootMotionRootLock,
			AnimSequence->bForceRootLock,
			AnimFirstFrame,
			AnimSequence->IsValidAdditive());
		RootMotionReset.ResetRootBoneForRootMotion(LocalTransform, RefBonePose[BoneIndex]);
	}

	if (ParentIndex != INDEX_NONE)
	{
		FTransform ParentComponentSpaceTransform;
		if (GetAnimatedComponentSpaceBoneTransform(SkinnedAsset, AnimSequence, Position, ParentIndex, ComponentSpaceCache, ParentComponentSpaceTransform))
		{
			OutComponentSpaceTransform = LocalTransform * ParentComponentSpaceTransform;
		}
		else
		{
			OutComponentSpaceTransform = LocalTransform;
		}
	}
	else
	{
		OutComponentSpaceTransform = LocalTransform;
	}

	ComponentSpaceCache.Add(BoneIndex, OutComponentSpaceTransform);
	return true;
}

static bool GetAnimatedComponentSpaceBoneTransform(
	const USkinnedAsset& SkinnedAsset,
	const UAnimSequence* AnimSequence,
	const float Position,
	const FName BoneName,
	TMap<int32, FTransform>& ComponentSpaceCache,
	FTransform& OutComponentSpaceTransform)
{
	const int32 BoneIndex = SkinnedAsset.GetRefSkeleton().FindBoneIndex(BoneName);
	if (BoneIndex == INDEX_NONE)
	{
		return false;
	}

	return GetAnimatedComponentSpaceBoneTransform(SkinnedAsset, AnimSequence, Position, BoneIndex, ComponentSpaceCache, OutComponentSpaceTransform);
}

static bool IsCandidateNearSegment(
	const FTransform& InstanceWorldTransform,
	const USkinnedAsset& SkinnedAsset,
	const FVector& Start,
	const FVector& End,
	const float ExtraRadius,
	FVector* OutBoundsOrigin = nullptr,
	float* OutBoundsRadius = nullptr,
	float* OutDistanceToTrace = nullptr)
{
	const FBoxSphereBounds AssetBounds = SkinnedAsset.GetBounds();
	const FVector BoundsOrigin = InstanceWorldTransform.TransformPosition(AssetBounds.Origin);
	const float BoundsRadius = AssetBounds.SphereRadius * InstanceWorldTransform.GetScale3D().GetAbsMax() + ExtraRadius;
	const FVector ClosestPoint = FMath::ClosestPointOnSegment(BoundsOrigin, Start, End);
	const float DistanceToTrace = FVector::Dist(ClosestPoint, BoundsOrigin);

	if (OutBoundsOrigin)
	{
		*OutBoundsOrigin = BoundsOrigin;
	}
	if (OutBoundsRadius)
	{
		*OutBoundsRadius = BoundsRadius;
	}
	if (OutDistanceToTrace)
	{
		*OutDistanceToTrace = DistanceToTrace;
	}

	return DistanceToTrace <= BoundsRadius;
}

static bool RaycastLocalSphere(const FVector& LocalStart, const FVector& LocalEnd, const FVector& Center, const float Radius, float& OutTime, FVector& OutLocalNormal)
{
	const FVector Segment = LocalEnd - LocalStart;
	const float A = Segment.SizeSquared();
	if (A <= UE_KINDA_SMALL_NUMBER)
	{
		return false;
	}

	const FVector Offset = LocalStart - Center;
	const float RadiusSq = FMath::Square(Radius);
	if (Offset.SizeSquared() <= RadiusSq)
	{
		OutTime = 0.0f;
		OutLocalNormal = Offset.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
		return true;
	}

	const float B = 2.0f * FVector::DotProduct(Offset, Segment);
	const float C = Offset.SizeSquared() - RadiusSq;
	const float Discriminant = B * B - 4.0f * A * C;
	if (Discriminant < 0.0f)
	{
		return false;
	}

	const float SqrtDiscriminant = FMath::Sqrt(Discriminant);
	const float InvDenominator = 1.0f / (2.0f * A);
	const float T0 = (-B - SqrtDiscriminant) * InvDenominator;
	const float T1 = (-B + SqrtDiscriminant) * InvDenominator;
	const float Time = (T0 >= 0.0f && T0 <= 1.0f) ? T0 : T1;
	if (Time < 0.0f || Time > 1.0f)
	{
		return false;
	}

	const FVector LocalPoint = LocalStart + Segment * Time;
	OutTime = Time;
	OutLocalNormal = (LocalPoint - Center).GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
	return true;
}

static FTransform MakeUnscaledTransform(const FTransform& Transform)
{
	return FTransform(Transform.GetRotation(), Transform.GetTranslation(), FVector::OneVector);
}

static float GetSegmentTime(const FVector& SegmentStart, const FVector& SegmentEnd, const FVector& Point)
{
	const FVector Segment = SegmentEnd - SegmentStart;
	const double SegmentLengthSq = Segment.SizeSquared();
	if (SegmentLengthSq <= UE_DOUBLE_KINDA_SMALL_NUMBER)
	{
		return 0.0f;
	}

	return static_cast<float>(FMath::Clamp(FVector::DotProduct(Point - SegmentStart, Segment) / SegmentLengthSq, 0.0, 1.0));
}

static float GetLocalCapsuleDistanceSquared(const FVector& Point, const FVector& CapsuleStart, const FVector& CapsuleEnd, FVector* OutClosestPoint = nullptr)
{
	const FVector ClosestPoint = FMath::ClosestPointOnSegment(Point, CapsuleStart, CapsuleEnd);
	if (OutClosestPoint)
	{
		*OutClosestPoint = ClosestPoint;
	}

	return FVector::DistSquared(Point, ClosestPoint);
}

static void TryCommitShapeHit(
	const float Time,
	const FVector& LocalNormal,
	const FTransform& ShapeToWorld,
	const FVector& Start,
	const FVector& End,
	const FName BoneName,
	FInternalHit& InOutHit)
{
	const float ClampedTime = FMath::Clamp(Time, 0.0f, 1.0f);
	const FVector ImpactPoint = FMath::Lerp(Start, End, ClampedTime);
	const float Distance = FVector::Dist(Start, ImpactPoint);
	if (Distance >= InOutHit.Distance)
	{
		return;
	}

	InOutHit.bHit = true;
	InOutHit.Distance = Distance;
	InOutHit.ImpactPoint = ImpactPoint;
	InOutHit.ImpactNormal = ShapeToWorld.TransformVectorNoScale(LocalNormal).GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
	InOutHit.BoneName = BoneName;
}

static bool RaycastLocalCapsuleSegment(
	const FVector& LocalStart,
	const FVector& LocalEnd,
	const FVector& CapsuleStart,
	const FVector& CapsuleEnd,
	const float Radius,
	float& OutTime,
	FVector& OutLocalNormal,
	float* OutClosestDistance = nullptr,
	float* OutClosestTime = nullptr)
{
	const FVector LocalSegment = LocalEnd - LocalStart;
	if (LocalSegment.SizeSquared() <= UE_KINDA_SMALL_NUMBER)
	{
		return false;
	}

	FVector ClosestOnCapsule = FVector::ZeroVector;
	const float RadiusSq = FMath::Square(Radius);
	if (GetLocalCapsuleDistanceSquared(LocalStart, CapsuleStart, CapsuleEnd, &ClosestOnCapsule) <= RadiusSq)
	{
		OutTime = 0.0f;
		OutLocalNormal = (LocalStart - ClosestOnCapsule).GetSafeNormal(UE_SMALL_NUMBER, -LocalSegment.GetSafeNormal());
		if (OutClosestDistance)
		{
			*OutClosestDistance = 0.0f;
		}
		if (OutClosestTime)
		{
			*OutClosestTime = 0.0f;
		}
		return true;
	}

	FVector ClosestCapsulePoint = FVector::ZeroVector;
	FVector ClosestRayPoint = FVector::ZeroVector;
	FMath::SegmentDistToSegmentSafe(CapsuleStart, CapsuleEnd, LocalStart, LocalEnd, ClosestCapsulePoint, ClosestRayPoint);

	const float ClosestDistanceSq = FVector::DistSquared(ClosestCapsulePoint, ClosestRayPoint);
	const float ClosestDistance = FMath::Sqrt(ClosestDistanceSq);
	const float ClosestTime = GetSegmentTime(LocalStart, LocalEnd, ClosestRayPoint);
	if (OutClosestDistance)
	{
		*OutClosestDistance = ClosestDistance;
	}
	if (OutClosestTime)
	{
		*OutClosestTime = ClosestTime;
	}

	if (ClosestDistanceSq > RadiusSq + UE_KINDA_SMALL_NUMBER)
	{
		return false;
	}

	float LowTime = 0.0f;
	float HighTime = ClosestTime;
	for (int32 Iteration = 0; Iteration < 24; ++Iteration)
	{
		const float MidTime = (LowTime + HighTime) * 0.5f;
		const FVector MidPoint = LocalStart + LocalSegment * MidTime;
		if (GetLocalCapsuleDistanceSquared(MidPoint, CapsuleStart, CapsuleEnd) <= RadiusSq)
		{
			HighTime = MidTime;
		}
		else
		{
			LowTime = MidTime;
		}
	}

	OutTime = HighTime;
	const FVector HitPoint = LocalStart + LocalSegment * OutTime;
	FVector ClosestAtHit = FVector::ZeroVector;
	GetLocalCapsuleDistanceSquared(HitPoint, CapsuleStart, CapsuleEnd, &ClosestAtHit);
	OutLocalNormal = (HitPoint - ClosestAtHit).GetSafeNormal(UE_SMALL_NUMBER, -LocalSegment.GetSafeNormal());
	return true;
}

static void RaycastSphereElem(const FKSphereElem& SphereElem, const FTransform& BoneWorldTransform, const FVector& Start, const FVector& End, const FName BoneName, const float SweepInflationRadius, FInternalHit& InOutHit)
{
	const FKSphereElem ScaledSphere = SphereElem.GetFinalScaled(BoneWorldTransform.GetScale3D(), FTransform::Identity);
	const FTransform ShapeToWorld = ScaledSphere.GetTransform() * MakeUnscaledTransform(BoneWorldTransform);
	const FVector LocalStart = ShapeToWorld.InverseTransformPositionNoScale(Start);
	const FVector LocalEnd = ShapeToWorld.InverseTransformPositionNoScale(End);

	float Time = 0.0f;
	FVector LocalNormal = FVector::UpVector;
	if (RaycastLocalSphere(LocalStart, LocalEnd, FVector::ZeroVector, ScaledSphere.Radius + SweepInflationRadius, Time, LocalNormal))
	{
		TryCommitShapeHit(Time, LocalNormal, ShapeToWorld, Start, End, BoneName, InOutHit);
	}
}

static void DrawSphereElem(const FKSphereElem& SphereElem, const FTransform& BoneWorldTransform, const FCandidateShapeDebug& ShapeDebug)
{
	if (!ShapeDebug.ShouldDraw())
	{
		return;
	}

	const FKSphereElem ScaledSphere = SphereElem.GetFinalScaled(BoneWorldTransform.GetScale3D(), FTransform::Identity);
	const FTransform ShapeToWorld = ScaledSphere.GetTransform() * MakeUnscaledTransform(BoneWorldTransform);
	DrawDebugSphere(ShapeDebug.World, ShapeToWorld.GetLocation(), ScaledSphere.Radius, 16, ShapeDebug.Color, ShapeDebug.bPersistent, ShapeDebug.Lifetime, 0, ShapeDebug.Thickness);
}

static void RaycastBoxElem(const FKBoxElem& BoxElem, const FTransform& BoneWorldTransform, const FVector& Start, const FVector& End, const FName BoneName, const float SweepInflationRadius, FInternalHit& InOutHit)
{
	const FKBoxElem ScaledBox = BoxElem.GetFinalScaled(BoneWorldTransform.GetScale3D(), FTransform::Identity);
	const FTransform ShapeToWorld = ScaledBox.GetTransform() * MakeUnscaledTransform(BoneWorldTransform);
	const FVector LocalStart = ShapeToWorld.InverseTransformPositionNoScale(Start);
	const FVector LocalEnd = ShapeToWorld.InverseTransformPositionNoScale(End);
	const FVector HalfExtents(ScaledBox.X * 0.5f + SweepInflationRadius, ScaledBox.Y * 0.5f + SweepInflationRadius, ScaledBox.Z * 0.5f + SweepInflationRadius);
	const FBox LocalBox(-HalfExtents, HalfExtents);

	FVector HitLocation = FVector::ZeroVector;
	FVector HitNormal = FVector::UpVector;
	float HitTime = 0.0f;
	if (FMath::LineExtentBoxIntersection(LocalBox, LocalStart, LocalEnd, FVector::ZeroVector, HitLocation, HitNormal, HitTime))
	{
		TryCommitShapeHit(HitTime, HitNormal, ShapeToWorld, Start, End, BoneName, InOutHit);
	}
}

static void DrawBoxElem(const FKBoxElem& BoxElem, const FTransform& BoneWorldTransform, const FCandidateShapeDebug& ShapeDebug)
{
	if (!ShapeDebug.ShouldDraw())
	{
		return;
	}

	const FKBoxElem ScaledBox = BoxElem.GetFinalScaled(BoneWorldTransform.GetScale3D(), FTransform::Identity);
	const FTransform ShapeToWorld = ScaledBox.GetTransform() * MakeUnscaledTransform(BoneWorldTransform);
	const FVector HalfExtents(ScaledBox.X * 0.5f, ScaledBox.Y * 0.5f, ScaledBox.Z * 0.5f);
	DrawDebugBox(ShapeDebug.World, ShapeToWorld.GetLocation(), HalfExtents, ShapeToWorld.GetRotation(), ShapeDebug.Color, ShapeDebug.bPersistent, ShapeDebug.Lifetime, 0, ShapeDebug.Thickness);
}

static void RaycastSphylElem(const FKSphylElem& SphylElem, const FTransform& BoneWorldTransform, const FVector& Start, const FVector& End, const FName BoneName, const float SweepInflationRadius, FInternalHit& InOutHit)
{
	const FKSphylElem ScaledSphyl = SphylElem.GetFinalScaled(BoneWorldTransform.GetScale3D(), FTransform::Identity);
	const FTransform ShapeToWorld = ScaledSphyl.GetTransform() * MakeUnscaledTransform(BoneWorldTransform);
	const FVector LocalStart = ShapeToWorld.InverseTransformPositionNoScale(Start);
	const FVector LocalEnd = ShapeToWorld.InverseTransformPositionNoScale(End);

	const float HalfLength = ScaledSphyl.Length * 0.5f;
	const float Radius = ScaledSphyl.Radius + SweepInflationRadius;
	const FVector CapsuleStart(0.0f, 0.0f, -HalfLength);
	const FVector CapsuleEnd(0.0f, 0.0f, HalfLength);

	float Time = 0.0f;
	FVector LocalNormal = FVector::UpVector;
	const bool bHit = RaycastLocalCapsuleSegment(LocalStart, LocalEnd, CapsuleStart, CapsuleEnd, Radius, Time, LocalNormal);

	if (bHit)
	{
		TryCommitShapeHit(Time, LocalNormal, ShapeToWorld, Start, End, BoneName, InOutHit);
	}
}

static void DrawSphylElem(const FKSphylElem& SphylElem, const FTransform& BoneWorldTransform, const FCandidateShapeDebug& ShapeDebug)
{
	if (!ShapeDebug.ShouldDraw())
	{
		return;
	}

	const FKSphylElem ScaledSphyl = SphylElem.GetFinalScaled(BoneWorldTransform.GetScale3D(), FTransform::Identity);
	const FTransform ShapeToWorld = ScaledSphyl.GetTransform() * MakeUnscaledTransform(BoneWorldTransform);
	DrawDebugCapsule(
		ShapeDebug.World,
		ShapeToWorld.GetLocation(),
		ScaledSphyl.Length * 0.5f + ScaledSphyl.Radius,
		ScaledSphyl.Radius,
		ShapeToWorld.GetRotation(),
		ShapeDebug.Color,
		ShapeDebug.bPersistent,
		ShapeDebug.Lifetime,
		0,
		ShapeDebug.Thickness);
}

enum class ECrowdyCachedPhysicsShapeType : uint8
{
	Sphere,
	Sphyl,
	Box
};

struct FCrowdyCachedPhysicsShape
{
	ECrowdyCachedPhysicsShapeType ShapeType = ECrowdyCachedPhysicsShapeType::Sphere;
	int32 BoneIndex = INDEX_NONE;
	FName BoneName = NAME_None;
	FKSphereElem SphereElem;
	FKSphylElem SphylElem;
	FKBoxElem BoxElem;
};

struct FCrowdyPhysicsAssetTraceMetadata
{
	TArray<FCrowdyCachedPhysicsShape> Shapes;
};

struct FCrowdyPhysicsAssetTraceMetadataKey
{
	const USkinnedAsset* SkinnedAsset = nullptr;
	const UPhysicsAsset* PhysicsAsset = nullptr;

	bool operator==(const FCrowdyPhysicsAssetTraceMetadataKey& Other) const
	{
		return SkinnedAsset == Other.SkinnedAsset && PhysicsAsset == Other.PhysicsAsset;
	}
};

static uint32 GetTypeHash(const FCrowdyPhysicsAssetTraceMetadataKey& Key)
{
	return HashCombine(PointerHash(Key.SkinnedAsset), PointerHash(Key.PhysicsAsset));
}

static TMap<FCrowdyPhysicsAssetTraceMetadataKey, FCrowdyPhysicsAssetTraceMetadata>& GetPhysicsAssetTraceMetadataCache()
{
	static TMap<FCrowdyPhysicsAssetTraceMetadataKey, FCrowdyPhysicsAssetTraceMetadata> Cache;
	return Cache;
}

static const FCrowdyPhysicsAssetTraceMetadata& FindOrBuildPhysicsAssetTraceMetadata(const UPhysicsAsset& PhysicsAsset, const USkinnedAsset& SkinnedAsset)
{
	FCrowdyPhysicsAssetTraceMetadataKey Key;
	Key.SkinnedAsset = &SkinnedAsset;
	Key.PhysicsAsset = &PhysicsAsset;

	TMap<FCrowdyPhysicsAssetTraceMetadataKey, FCrowdyPhysicsAssetTraceMetadata>& Cache = GetPhysicsAssetTraceMetadataCache();
	if (const FCrowdyPhysicsAssetTraceMetadata* ExistingMetadata = Cache.Find(Key))
	{
		return *ExistingMetadata;
	}

	FCrowdyPhysicsAssetTraceMetadata& Metadata = Cache.Add(Key);
	const FReferenceSkeleton& RefSkeleton = SkinnedAsset.GetRefSkeleton();
	for (const TObjectPtr<USkeletalBodySetup>& BodySetupPtr : PhysicsAsset.SkeletalBodySetups)
	{
		const USkeletalBodySetup* BodySetup = BodySetupPtr.Get();
		if (!BodySetup || BodySetup->BoneName.IsNone())
		{
			continue;
		}

		const int32 BoneIndex = RefSkeleton.FindBoneIndex(BodySetup->BoneName);
		if (BoneIndex == INDEX_NONE)
		{
			continue;
		}

		for (const FKSphereElem& SphereElem : BodySetup->AggGeom.SphereElems)
		{
			FCrowdyCachedPhysicsShape& Shape = Metadata.Shapes.AddDefaulted_GetRef();
			Shape.ShapeType = ECrowdyCachedPhysicsShapeType::Sphere;
			Shape.BoneIndex = BoneIndex;
			Shape.BoneName = BodySetup->BoneName;
			Shape.SphereElem = SphereElem;
		}

		for (const FKSphylElem& SphylElem : BodySetup->AggGeom.SphylElems)
		{
			FCrowdyCachedPhysicsShape& Shape = Metadata.Shapes.AddDefaulted_GetRef();
			Shape.ShapeType = ECrowdyCachedPhysicsShapeType::Sphyl;
			Shape.BoneIndex = BoneIndex;
			Shape.BoneName = BodySetup->BoneName;
			Shape.SphylElem = SphylElem;
		}

		for (const FKBoxElem& BoxElem : BodySetup->AggGeom.BoxElems)
		{
			FCrowdyCachedPhysicsShape& Shape = Metadata.Shapes.AddDefaulted_GetRef();
			Shape.ShapeType = ECrowdyCachedPhysicsShapeType::Box;
			Shape.BoneIndex = BoneIndex;
			Shape.BoneName = BodySetup->BoneName;
			Shape.BoxElem = BoxElem;
		}
	}

	return Metadata;
}

static bool RaycastPhysicsAssetDirect(
	const UPhysicsAsset& PhysicsAsset,
	const USkinnedAsset& SkinnedAsset,
	const FAnimSample& AnimSample,
	const FTransform& InstanceWorldTransform,
	const FVector& Start,
	const FVector& End,
	const float SweepInflationRadius,
	FInternalHit& InOutHit,
	int32* OutShapesTested = nullptr,
	const FCandidateShapeDebug* ShapeDebug = nullptr)
{
	UAnimSequence* AnimSequence = AnimSample.AnimSequence.Get();
	TMap<int32, FTransform> ComponentSpaceBoneCache;
	bool bAnyShapeTested = false;
	int32 ShapesTested = 0;

	for (const TObjectPtr<USkeletalBodySetup>& BodySetupPtr : PhysicsAsset.SkeletalBodySetups)
	{
		const USkeletalBodySetup* BodySetup = BodySetupPtr.Get();
		if (!BodySetup || BodySetup->BoneName.IsNone())
		{
			continue;
		}

		FTransform BoneComponentSpaceTransform;
		if (!GetAnimatedComponentSpaceBoneTransform(SkinnedAsset, AnimSequence, AnimSample.Position, BodySetup->BoneName, ComponentSpaceBoneCache, BoneComponentSpaceTransform))
		{
			continue;
		}

		const FTransform BoneWorldTransform = BoneComponentSpaceTransform * InstanceWorldTransform;
		for (const FKSphereElem& SphereElem : BodySetup->AggGeom.SphereElems)
		{
			bAnyShapeTested = true;
			++ShapesTested;
			if (ShapeDebug)
			{
				DrawSphereElem(SphereElem, BoneWorldTransform, *ShapeDebug);
			}
			RaycastSphereElem(SphereElem, BoneWorldTransform, Start, End, BodySetup->BoneName, SweepInflationRadius, InOutHit);
		}

		for (const FKSphylElem& SphylElem : BodySetup->AggGeom.SphylElems)
		{
			bAnyShapeTested = true;
			++ShapesTested;
			if (ShapeDebug)
			{
				DrawSphylElem(SphylElem, BoneWorldTransform, *ShapeDebug);
			}
			RaycastSphylElem(SphylElem, BoneWorldTransform, Start, End, BodySetup->BoneName, SweepInflationRadius, InOutHit);
		}

		for (const FKBoxElem& BoxElem : BodySetup->AggGeom.BoxElems)
		{
			bAnyShapeTested = true;
			++ShapesTested;
			if (ShapeDebug)
			{
				DrawBoxElem(BoxElem, BoneWorldTransform, *ShapeDebug);
			}
			RaycastBoxElem(BoxElem, BoneWorldTransform, Start, End, BodySetup->BoneName, SweepInflationRadius, InOutHit);
		}
	}

	if (OutShapesTested)
	{
		*OutShapesTested = ShapesTested;
	}

	return bAnyShapeTested && InOutHit.bHit;
}

static bool RaycastPhysicsAssetCached(
	const UPhysicsAsset& PhysicsAsset,
	const USkinnedAsset& SkinnedAsset,
	const FAnimSample& AnimSample,
	const FTransform& InstanceWorldTransform,
	const FVector& Start,
	const FVector& End,
	const float SweepInflationRadius,
	FInternalHit& InOutHit,
	int32* OutShapesTested = nullptr,
	const FCandidateShapeDebug* ShapeDebug = nullptr)
{
	const FCrowdyPhysicsAssetTraceMetadata& Metadata = FindOrBuildPhysicsAssetTraceMetadata(PhysicsAsset, SkinnedAsset);
	UAnimSequence* AnimSequence = AnimSample.AnimSequence.Get();
	TMap<int32, FTransform> ComponentSpaceBoneCache;
	bool bAnyShapeTested = false;
	int32 ShapesTested = 0;

	for (const FCrowdyCachedPhysicsShape& Shape : Metadata.Shapes)
	{
		if (Shape.BoneIndex == INDEX_NONE)
		{
			continue;
		}

		FTransform BoneComponentSpaceTransform;
		if (!GetAnimatedComponentSpaceBoneTransform(SkinnedAsset, AnimSequence, AnimSample.Position, Shape.BoneIndex, ComponentSpaceBoneCache, BoneComponentSpaceTransform))
		{
			continue;
		}

		const FTransform BoneWorldTransform = BoneComponentSpaceTransform * InstanceWorldTransform;
		bAnyShapeTested = true;
		++ShapesTested;

		switch (Shape.ShapeType)
		{
		case ECrowdyCachedPhysicsShapeType::Sphere:
			if (ShapeDebug)
			{
				DrawSphereElem(Shape.SphereElem, BoneWorldTransform, *ShapeDebug);
			}
			RaycastSphereElem(Shape.SphereElem, BoneWorldTransform, Start, End, Shape.BoneName, SweepInflationRadius, InOutHit);
			break;
		case ECrowdyCachedPhysicsShapeType::Sphyl:
			if (ShapeDebug)
			{
				DrawSphylElem(Shape.SphylElem, BoneWorldTransform, *ShapeDebug);
			}
			RaycastSphylElem(Shape.SphylElem, BoneWorldTransform, Start, End, Shape.BoneName, SweepInflationRadius, InOutHit);
			break;
		case ECrowdyCachedPhysicsShapeType::Box:
			if (ShapeDebug)
			{
				DrawBoxElem(Shape.BoxElem, BoneWorldTransform, *ShapeDebug);
			}
			RaycastBoxElem(Shape.BoxElem, BoneWorldTransform, Start, End, Shape.BoneName, SweepInflationRadius, InOutHit);
			break;
		}
	}

	if (OutShapesTested)
	{
		*OutShapesTested = ShapesTested;
	}

	return bAnyShapeTested && InOutHit.bHit;
}

static bool RaycastPhysicsAsset(
	const UPhysicsAsset& PhysicsAsset,
	const USkinnedAsset& SkinnedAsset,
	const FAnimSample& AnimSample,
	const FTransform& InstanceWorldTransform,
	const FVector& Start,
	const FVector& End,
	const float SweepInflationRadius,
	const bool bUseMetadataCache,
	FInternalHit& InOutHit,
	int32* OutShapesTested = nullptr,
	const FCandidateShapeDebug* ShapeDebug = nullptr)
{
	if (bUseMetadataCache)
	{
		return RaycastPhysicsAssetCached(PhysicsAsset, SkinnedAsset, AnimSample, InstanceWorldTransform, Start, End, SweepInflationRadius, InOutHit, OutShapesTested, ShapeDebug);
	}

	return RaycastPhysicsAssetDirect(PhysicsAsset, SkinnedAsset, AnimSample, InstanceWorldTransform, Start, End, SweepInflationRadius, InOutHit, OutShapesTested, ShapeDebug);
}

struct FPreparedMassTraceCandidate
{
	FMassEntityHandle Entity;
	FGuid UUID;
	UMassRepresentationSubsystem* RepresentationSubsystem = nullptr;
	UInstancedSkinnedMeshComponent* Component = nullptr;
	FPrimitiveInstanceId InstanceId;
	USkinnedAsset* SkinnedAsset = nullptr;
	UPhysicsAsset* PhysicsAsset = nullptr;
	FTransform InstanceWorldTransform;
	float BoundsEntryDistance = 0.0f;
};

static float GetBoundsEntryDistance(const FVector& Start, const FVector& TraceDirection, const FVector& BoundsOrigin, const float BoundsRadius)
{
	const float CenterDistance = FVector::DotProduct(BoundsOrigin - Start, TraceDirection);
	return CenterDistance - BoundsRadius;
}

static bool TryPrepareMassTraceCandidateFromCache(
	UCrowdyMassEntitySubsystem& CrowdySubsystem,
	const FMassEntityHandle Entity,
	const FCrowdyRemoteIdentityFragment& IdentityFragment,
	const FMassRepresentationFragment& RepresentationFragment,
	const FMassRepresentationLODFragment& RepresentationLODFragment,
	const FVector& Start,
	const FVector& CappedEnd,
	const FVector& TraceDirection,
	const float ClampedSweepInflationRadius,
	TArray<FPreparedMassTraceCandidate>& OutCandidates,
	FTraceDebugStats* DebugStats)
{
	const FCrowdyMassTraceInstanceCacheEntry* CacheEntry = CrowdySubsystem.FindTraceInstanceCache(Entity);
	if (!CacheEntry)
	{
		return false;
	}

	const int32 CurrentSkinnedMeshInfoIndex = RepresentationFragment.SkinnedMeshDescHandle.ToIndex();
	if (CacheEntry->UUID != IdentityFragment.UUID ||
		CacheEntry->SkinnedMeshInfoIndex != CurrentSkinnedMeshInfoIndex ||
		!FMath::IsNearlyEqual(CacheEntry->LODSignificance, static_cast<float>(RepresentationLODFragment.LODSignificance)))
	{
		CrowdySubsystem.RemoveTraceInstanceCache(Entity);
		return false;
	}

	UMassRepresentationSubsystem* EntityRepresentationSubsystem = CacheEntry->RepresentationSubsystem.Get();
	UInstancedSkinnedMeshComponent* Component = CacheEntry->Component.Get();
	USkinnedAsset* SkinnedAsset = CacheEntry->SkinnedAsset.Get();
	UPhysicsAsset* PhysicsAsset = CacheEntry->PhysicsAsset.Get();
	if (!EntityRepresentationSubsystem || !IsValid(Component) || !IsValid(SkinnedAsset) || !IsValid(PhysicsAsset) || !CacheEntry->InstanceId.IsValid())
	{
		CrowdySubsystem.RemoveTraceInstanceCache(Entity);
		return false;
	}

	if (Component->GetSkinnedAsset() != SkinnedAsset || Component->GetPhysicsAsset() != PhysicsAsset)
	{
		CrowdySubsystem.RemoveTraceInstanceCache(Entity);
		return false;
	}

	FTransform InstanceWorldTransform;
	if (!Component->GetInstanceTransform(CacheEntry->InstanceId, InstanceWorldTransform, true))
	{
		CrowdySubsystem.RemoveTraceInstanceCache(Entity);
		return false;
	}

	if (DebugStats)
	{
		++DebugStats->ResolvedComponentCount;
		++DebugStats->MappedInstanceCount;
	}

	FVector BoundsOrigin = FVector::ZeroVector;
	float BoundsRadius = 0.0f;
	if (!IsCandidateNearSegment(InstanceWorldTransform, *SkinnedAsset, Start, CappedEnd, 20.0f + ClampedSweepInflationRadius, &BoundsOrigin, &BoundsRadius))
	{
		if (DebugStats)
		{
			++DebugStats->BoundsRejectedCount;
		}
		return true;
	}
	if (DebugStats)
	{
		++DebugStats->BoundsCandidateCount;
	}

	FPreparedMassTraceCandidate& PreparedCandidate = OutCandidates.AddDefaulted_GetRef();
	PreparedCandidate.Entity = Entity;
	PreparedCandidate.UUID = IdentityFragment.UUID;
	PreparedCandidate.RepresentationSubsystem = EntityRepresentationSubsystem;
	PreparedCandidate.Component = Component;
	PreparedCandidate.InstanceId = CacheEntry->InstanceId;
	PreparedCandidate.SkinnedAsset = SkinnedAsset;
	PreparedCandidate.PhysicsAsset = PhysicsAsset;
	PreparedCandidate.InstanceWorldTransform = InstanceWorldTransform;
	PreparedCandidate.BoundsEntryDistance = GetBoundsEntryDistance(Start, TraceDirection, BoundsOrigin, BoundsRadius);
	return true;
}

static void PrepareMassTraceCandidatesForEntity(
	UCrowdyMassEntitySubsystem& CrowdySubsystem,
	FMassEntityManager& EntityManager,
	const FMassEntityHandle Entity,
	const FVector& Start,
	const FVector& CappedEnd,
	const FVector& TraceDirection,
	const float ClampedSweepInflationRadius,
	const FCrowdyMassTraceFilter& TraceFilter,
	const FCrowdyMassWorldReportedEntities& ReportedEntities,
	const bool bUseTraceInstanceCache,
	TArray<FPreparedMassTraceCandidate>& OutCandidates,
	FTraceDebugStats* DebugStats)
{
	// One view per candidate, and every fragment below comes out of it. Asking the entity manager for a
	// fragment resolves the entity to its archetype and then to its slot in a chunk before it can look
	// the fragment type up, and this function used to pay that seven times over for one entity. The view
	// does the entity-to-slot half once at construction and each fragment after it is a lookup inside the
	// archetype it already holds.
	//
	// The view is valid only while the entity keeps its archetype, which nothing here changes.
	const FMassEntityView EntityView = FMassEntityView::TryMakeView(EntityManager, Entity);
	if (!EntityView.IsSet())
	{
		return;
	}
	if (DebugStats)
	{
		++DebugStats->ValidEntityCount;
	}

	const FCrowdyMassTraceCollisionFragment* TraceCollisionFragment = EntityView.GetFragmentDataPtr<FCrowdyMassTraceCollisionFragment>();
	if (!TraceCollisionFragment)
	{
		// A crowd whose config omits the Crowdy Mass Trace Collision trait is untraceable and says
		// nothing about it, which looks exactly like an empty spatial grid. Named once per world.
		CrowdySubsystem.ReportMissingTraceCollisionFragment();
		if (DebugStats)
		{
			++DebugStats->TraceCollisionRejectedCount;
		}
		return;
	}

	if (!DoesEntityPassTraceFilter(TraceCollisionFragment, TraceFilter))
	{
		if (DebugStats)
		{
			++DebugStats->TraceCollisionRejectedCount;
		}
		return;
	}

	const FCrowdyRemoteIdentityFragment* IdentityFragment = EntityView.GetFragmentDataPtr<FCrowdyRemoteIdentityFragment>();
	// Read for its presence only: an archetype with no transform is not a crowd entity this trace can
	// place, and the instance transform the raycast actually uses comes from the renderer instead.
	const FTransformFragment* TransformFragment = EntityView.GetFragmentDataPtr<FTransformFragment>();
	const FMassRepresentationFragment* RepresentationFragment = EntityView.GetFragmentDataPtr<FMassRepresentationFragment>();
	const FMassRepresentationLODFragment* RepresentationLODFragment = EntityView.GetFragmentDataPtr<FMassRepresentationLODFragment>();
	if (!IdentityFragment || !TransformFragment || !RepresentationFragment || !RepresentationLODFragment)
	{
		if (DebugStats)
		{
			++DebugStats->MissingFragmentCount;
		}
		return;
	}

	// The world trace ran first, and this is where its results are honoured: an entity it already reported
	// is left to that hit, and an entity it did not report is tested here whatever it is being drawn by.
	if (!ShouldTraceEntityAsRow(*RepresentationFragment, ReportedEntities.Contains(Entity)))
	{
		if (DebugStats)
		{
			++DebugStats->NonSkinnedRepresentationCount;
		}
		return;
	}
	if (DebugStats)
	{
		++DebugStats->SkinnedRepresentationCount;
	}

	if (bUseTraceInstanceCache && TryPrepareMassTraceCandidateFromCache(CrowdySubsystem, Entity, *IdentityFragment, *RepresentationFragment, *RepresentationLODFragment, Start, CappedEnd, TraceDirection, ClampedSweepInflationRadius, OutCandidates, DebugStats))
	{
		return;
	}

	// Through the same view as the fragments above, and only here: the cache path returns before this
	// line, so a candidate that hits the cache never resolves the shared fragment at all.
	const FMassRepresentationSubsystemSharedFragment* RepresentationSubsystemFragment =
		EntityView.GetSharedFragmentDataPtr<FMassRepresentationSubsystemSharedFragment>();
	UMassRepresentationSubsystem* EntityRepresentationSubsystem =
		RepresentationSubsystemFragment ? RepresentationSubsystemFragment->RepresentationSubsystem : nullptr;
	if (!EntityRepresentationSubsystem)
	{
		if (DebugStats)
		{
			++DebugStats->MissingRepresentationSharedFragmentCount;
		}
		return;
	}

	FMassInstancedSkinnedMeshInfoArrayView SkinnedMeshInfos = EntityRepresentationSubsystem->GetMutableInstancedSkinnedMeshInfos();
	const int32 SkinnedMeshInfoIndex = RepresentationFragment->SkinnedMeshDescHandle.ToIndex();
	if (!SkinnedMeshInfos.IsValidIndex(SkinnedMeshInfoIndex))
	{
		if (DebugStats)
		{
			++DebugStats->InvalidSkinnedMeshInfoCount;
		}
		return;
	}

	FMassInstancedSkinnedMeshInfo& SkinnedMeshInfo = SkinnedMeshInfos[SkinnedMeshInfoIndex];
	FMassLODInstancedSkinnedMeshSignificanceRange* LODRange =
		SkinnedMeshInfo.GetLODSignificanceRange(RepresentationLODFragment->LODSignificance);
	if (!LODRange || !LODRange->InstancedSkinnedMeshSharedDataPtr)
	{
		if (DebugStats)
		{
			++DebugStats->MissingLODRangeCount;
		}
		return;
	}

	for (int32 ComponentIt = 0; ComponentIt < LODRange->SkinnedMeshComponentRefs.Num(); ++ComponentIt)
	{
		if (DebugStats)
		{
			++DebugStats->ComponentRefCount;
		}

		const FInstancedSkinnedMeshComponentSharedDataKey& ComponentRef = LODRange->SkinnedMeshComponentRefs[ComponentIt];
		UInstancedSkinnedMeshComponent* Component = ComponentRef.ResolveObjectPtr();
		if (!IsValid(Component))
		{
			if (DebugStats)
			{
				++DebugStats->InvalidComponentCount;
			}
			continue;
		}
		if (DebugStats)
		{
			++DebugStats->ResolvedComponentCount;
		}

		const FMassInstancedSkinnedMeshComponentSharedData* SharedData =
			LODRange->InstancedSkinnedMeshSharedDataPtr->GetDataForKey(ComponentRef);
		if (!SharedData)
		{
			if (DebugStats)
			{
				++DebugStats->MissingSharedDataCount;
			}
			continue;
		}

		const FMassInstancedSkinnedMeshComponentSharedData::FEntityToPrimitiveIdMap& EntityToPrimitiveIdMap =
			SharedData->GetEntityToPrimitiveIdMap();
		const Experimental::FHashElementId ElementId = EntityToPrimitiveIdMap.FindId(Entity);
		if (!ElementId.IsValid() || !EntityToPrimitiveIdMap.ContainsElementId(ElementId))
		{
			if (DebugStats)
			{
				++DebugStats->MissingInstanceMappingCount;
			}
			continue;
		}

		const FPrimitiveInstanceId InstanceId = EntityToPrimitiveIdMap.GetByElementId(ElementId).Value;
		if (!InstanceId.IsValid())
		{
			if (DebugStats)
			{
				++DebugStats->InvalidPrimitiveIdCount;
			}
			continue;
		}
		if (DebugStats)
		{
			++DebugStats->MappedInstanceCount;
		}

		USkinnedAsset* SkinnedAsset = Component->GetSkinnedAsset();
		UPhysicsAsset* PhysicsAsset = Component->GetPhysicsAsset();
		if (!SkinnedAsset)
		{
			if (DebugStats)
			{
				++DebugStats->MissingSkinnedAssetCount;
			}
			continue;
		}

		if (!PhysicsAsset)
		{
			if (DebugStats)
			{
				++DebugStats->MissingPhysicsAssetCount;
			}
			continue;
		}

		FTransform InstanceWorldTransform;
		if (!Component->GetInstanceTransform(InstanceId, InstanceWorldTransform, true))
		{
			if (DebugStats)
			{
				++DebugStats->FailedInstanceTransformCount;
			}
			continue;
		}

		if (bUseTraceInstanceCache)
		{
			FCrowdyMassTraceInstanceCacheEntry CacheEntry;
			CacheEntry.RepresentationSubsystem = EntityRepresentationSubsystem;
			CacheEntry.Component = Component;
			CacheEntry.SkinnedAsset = SkinnedAsset;
			CacheEntry.PhysicsAsset = PhysicsAsset;
			CacheEntry.InstanceId = InstanceId;
			CacheEntry.UUID = IdentityFragment->UUID;
			CacheEntry.SkinnedMeshInfoIndex = SkinnedMeshInfoIndex;
			CacheEntry.LODSignificance = static_cast<float>(RepresentationLODFragment->LODSignificance);
			CrowdySubsystem.UpdateTraceInstanceCache(Entity, CacheEntry);
		}

		FVector BoundsOrigin = FVector::ZeroVector;
		float BoundsRadius = 0.0f;
		if (!IsCandidateNearSegment(InstanceWorldTransform, *SkinnedAsset, Start, CappedEnd, 20.0f + ClampedSweepInflationRadius, &BoundsOrigin, &BoundsRadius))
		{
			if (DebugStats)
			{
				++DebugStats->BoundsRejectedCount;
			}
			continue;
		}
		if (DebugStats)
		{
			++DebugStats->BoundsCandidateCount;
		}

		FPreparedMassTraceCandidate& PreparedCandidate = OutCandidates.AddDefaulted_GetRef();
		PreparedCandidate.Entity = Entity;
		PreparedCandidate.UUID = IdentityFragment->UUID;
		PreparedCandidate.RepresentationSubsystem = EntityRepresentationSubsystem;
		PreparedCandidate.Component = Component;
		PreparedCandidate.InstanceId = InstanceId;
		PreparedCandidate.SkinnedAsset = SkinnedAsset;
		PreparedCandidate.PhysicsAsset = PhysicsAsset;
		PreparedCandidate.InstanceWorldTransform = InstanceWorldTransform;
		PreparedCandidate.BoundsEntryDistance = GetBoundsEntryDistance(Start, TraceDirection, BoundsOrigin, BoundsRadius);
	}
}

static void ProcessPreparedMassTraceCandidate(
	const FPreparedMassTraceCandidate& Candidate,
	const FVector& Start,
	const FVector& CappedEnd,
	const float MaxDistance,
	const float ClampedSweepInflationRadius,
	FInternalHit& OutBestHit,
	TArray<FInternalHit>* OutHits,
	FTraceDebugStats* DebugStats,
	const bool bUsePhysicsAssetTraceMetadataCache,
	const FCandidateShapeDebug* ShapeDebug)
{
	if (!Candidate.RepresentationSubsystem || !IsValid(Candidate.Component) || !IsValid(Candidate.SkinnedAsset) || !IsValid(Candidate.PhysicsAsset))
	{
		return;
	}

	FAnimSample AnimSample;
	if (TryResolveAnimSample(*Candidate.RepresentationSubsystem, Candidate.Entity, *Candidate.Component, Candidate.InstanceId, AnimSample))
	{
		if (DebugStats)
		{
			++DebugStats->AnimSampleResolvedCount;
		}
	}
	else if (DebugStats)
	{
		++DebugStats->AnimSampleMissingCount;
	}

	FInternalHit CandidateHit;
	CandidateHit.Entity = Candidate.Entity;
	CandidateHit.UUID = Candidate.UUID;
	CandidateHit.Component = Candidate.Component;
	CandidateHit.InstanceIndex = Candidate.InstanceId.GetAsIndex();
	CandidateHit.Distance = OutBestHit.Distance;

	int32 ShapesTested = 0;
	const bool bShapeHit = RaycastPhysicsAsset(*Candidate.PhysicsAsset, *Candidate.SkinnedAsset, AnimSample, Candidate.InstanceWorldTransform, Start, CappedEnd, ClampedSweepInflationRadius, bUsePhysicsAssetTraceMetadataCache, CandidateHit, &ShapesTested, ShapeDebug);
	if (DebugStats)
	{
		++DebugStats->PhysicsAssetTestCount;
		DebugStats->PhysicsShapeTestCount += ShapesTested;

		if (ShapesTested == 0)
		{
			++DebugStats->PhysicsAssetNoShapeCount;
		}
		else if (!bShapeHit)
		{
			++DebugStats->PhysicsShapeMissCount;
		}
	}

	if (bShapeHit)
	{
		if (DebugStats)
		{
			++DebugStats->MassHitCount;
		}

		if (CandidateHit.Distance < OutBestHit.Distance)
		{
			OutBestHit = CandidateHit;
		}

		if (OutHits)
		{
			OutHits->Add(CandidateHit);
		}
	}
}

static bool TraceMassSkinnedMeshInstances(
	UCrowdyMassEntitySubsystem& CrowdySubsystem,
	FMassEntityManager& EntityManager,
	const FVector& Start,
	const FVector& End,
	const float MaxDistance,
	const FCrowdyMassTraceFilter& TraceFilter,
	const FCrowdyMassWorldReportedEntities& ReportedEntities,
	FInternalHit& OutBestHit,
	TArray<FInternalHit>* OutHits = nullptr,
	FTraceDebugStats* DebugStats = nullptr,
	const FCandidateShapeDebug* ShapeDebug = nullptr,
	const FSpatialGridDebug* GridDebug = nullptr,
	const float SweepInflationRadius = 0.0f)
{
	const FVector TraceDirection = (End - Start).GetSafeNormal();
	const FVector CappedEnd = Start + TraceDirection * MaxDistance;
	const float ClampedSweepInflationRadius = FMath::Max(SweepInflationRadius, 0.0f);
	const int32 InitialHitCount = OutHits ? OutHits->Num() : 0;
	const UCrowdyMassDeveloperSettings* Settings = GetDefault<UCrowdyMassDeveloperSettings>();
	const bool bUseTraceInstanceCache = Settings ? Settings->bEnableTraceInstanceCache : true;
	const bool bUsePhysicsAssetTraceMetadataCache = Settings ? Settings->bEnablePhysicsAssetTraceMetadataCache : true;

	{
		int32 SpatialGridVisitedCellCount = 0;

		// Borrowed from the subsystem rather than built here. Growing this set from empty on every trace
		// rehashes it about a dozen times on the way up to a busy crowd, allocating at each step, and a
		// turret attack runs two full traces. Keeping the storage on the world's subsystem lets it settle
		// at whatever this world's traces actually need; it cannot live in this library, which is static,
		// because that would share one set between two PIE sessions and between the two worlds alive
		// during level travel.
		TSet<FMassEntityHandle>& TestedEntities = CrowdySubsystem.ResetTraceTestedEntitiesScratch();
		TArray<FPreparedMassTraceCandidate> CellCandidates;
		const bool bSpatialGridUsed = CrowdySubsystem.ForEachTraceCandidateCell(
			Start,
			CappedEnd,
			ClampedSweepInflationRadius,
			[&](const FCrowdyMassTraceCandidateCell& Cell, const bool bTraversalIsEntrySorted)
			{
				if (bTraversalIsEntrySorted && Cell.EntryDistance >= OutBestHit.Distance - UE_KINDA_SMALL_NUMBER)
				{
					return false;
				}

				if (GridDebug)
				{
					DrawSpatialGridCell(Cell.DebugBounds, *GridDebug);
				}

				if (!Cell.Entities)
				{
					return true;
				}

				CellCandidates.Reset();
				for (const FMassEntityHandle Entity : *Cell.Entities)
				{
					// One hash, not two: cells overlap, so most of what this loop sees on a busy trace has
					// already been tested, and the probe-then-insert pair used to hash every candidate twice.
					bool bAlreadyTested = false;
					TestedEntities.Add(Entity, &bAlreadyTested);
					if (bAlreadyTested)
					{
						continue;
					}

					if (DebugStats)
					{
						DebugStats->SpatialGridCandidateCount = TestedEntities.Num();
					}

					PrepareMassTraceCandidatesForEntity(CrowdySubsystem, EntityManager, Entity, Start, CappedEnd, TraceDirection, ClampedSweepInflationRadius, TraceFilter, ReportedEntities, bUseTraceInstanceCache, CellCandidates, DebugStats);
				}

				if (CellCandidates.Num() > 1)
				{
					CellCandidates.Sort([](const FPreparedMassTraceCandidate& A, const FPreparedMassTraceCandidate& B)
					{
						return A.BoundsEntryDistance < B.BoundsEntryDistance;
					});
				}

				for (const FPreparedMassTraceCandidate& Candidate : CellCandidates)
				{
					if (Candidate.BoundsEntryDistance >= OutBestHit.Distance - UE_KINDA_SMALL_NUMBER)
					{
						break;
					}

					ProcessPreparedMassTraceCandidate(Candidate, Start, CappedEnd, MaxDistance, ClampedSweepInflationRadius, OutBestHit, OutHits, DebugStats, bUsePhysicsAssetTraceMetadataCache, ShapeDebug);
				}

				return true;
			},
			SpatialGridVisitedCellCount);
		if (bSpatialGridUsed)
		{
			if (DebugStats)
			{
				DebugStats->TrackedEntityCount = CrowdySubsystem.GetTrackedRemoteEntityCount();
				DebugStats->bSpatialGridUsed = true;
				DebugStats->SpatialGridVisitedCellCount = SpatialGridVisitedCellCount;
			}

			return OutHits ? OutHits->Num() > InitialHitCount : OutBestHit.bHit;
		}
	}

	TArray<FMassEntityHandle> RemoteEntities;
	TArray<FBox> SpatialGridCellBounds;
	int32 SpatialGridVisitedCellCount = 0;
	const bool bSpatialGridUsed = CrowdySubsystem.GetTraceCandidateEntities(
		Start,
		CappedEnd,
		ClampedSweepInflationRadius,
		RemoteEntities,
		SpatialGridVisitedCellCount,
		GridDebug && GridDebug->ShouldDraw() ? &SpatialGridCellBounds : nullptr);
	if (DebugStats)
	{
		DebugStats->TrackedEntityCount = CrowdySubsystem.GetTrackedRemoteEntityCount();
		DebugStats->bSpatialGridUsed = bSpatialGridUsed;
		DebugStats->SpatialGridVisitedCellCount = SpatialGridVisitedCellCount;
		DebugStats->SpatialGridCandidateCount = RemoteEntities.Num();
	}
	if (GridDebug)
	{
		DrawSpatialGridCells(SpatialGridCellBounds, *GridDebug);
	}

	if (RemoteEntities.IsEmpty())
	{
		if (DebugStats)
		{
			if (DebugStats->TrackedEntityCount == 0)
			{
			}
			else if (bSpatialGridUsed)
			{
			}
		}
		return false;
	}

	TArray<FPreparedMassTraceCandidate> PreparedCandidates;
	PreparedCandidates.Reserve(RemoteEntities.Num());

	for (const FMassEntityHandle Entity : RemoteEntities)
	{
		PrepareMassTraceCandidatesForEntity(CrowdySubsystem, EntityManager, Entity, Start, CappedEnd, TraceDirection, ClampedSweepInflationRadius, TraceFilter, ReportedEntities, bUseTraceInstanceCache, PreparedCandidates, DebugStats);
	}

	if (PreparedCandidates.Num() > 1)
	{
		PreparedCandidates.Sort([](const FPreparedMassTraceCandidate& A, const FPreparedMassTraceCandidate& B)
		{
			return A.BoundsEntryDistance < B.BoundsEntryDistance;
		});
	}

	for (const FPreparedMassTraceCandidate& Candidate : PreparedCandidates)
	{
		if (Candidate.BoundsEntryDistance >= OutBestHit.Distance - UE_KINDA_SMALL_NUMBER)
		{
			break;
		}

		ProcessPreparedMassTraceCandidate(Candidate, Start, CappedEnd, MaxDistance, ClampedSweepInflationRadius, OutBestHit, OutHits, DebugStats, bUsePhysicsAssetTraceMetadataCache, ShapeDebug);
	}

	return OutHits ? OutHits->Num() > InitialHitCount : OutBestHit.bHit;
}

/**
 * What a world query result needs to be joined back to the Crowdy Mass entity it landed on, resolved once
 * per query rather than once per result.
 *
 * The join exists because a Crowdy Mass entity close enough to be given its own actor is hit through that
 * actor by the world query, not as an instanced row. Without it such a hit reports a bare actor and the
 * closest players in the crowd become the only ones nothing can damage.
 */
struct FCrowdyMassEntityJoinContext
{
	UWorld* World = nullptr;
	UMassActorSubsystem* ActorSubsystem = nullptr;
	const UCrowdyMassEntitySubsystem* CrowdySubsystem = nullptr;
	FMassEntityManager* EntityManager = nullptr;

	bool IsUsable() const
	{
		return World && ActorSubsystem && CrowdySubsystem && EntityManager;
	}
};

static FCrowdyMassEntityJoinContext MakeEntityJoinContext(UWorld* World)
{
	FCrowdyMassEntityJoinContext JoinContext;
	if (!World)
	{
		return JoinContext;
	}

	UMassEntitySubsystem* MassEntitySubsystem = World->GetSubsystem<UMassEntitySubsystem>();

	JoinContext.World = World;
	JoinContext.ActorSubsystem = World->GetSubsystem<UMassActorSubsystem>();
	JoinContext.CrowdySubsystem = World->GetSubsystem<UCrowdyMassEntitySubsystem>();
	JoinContext.EntityManager = MassEntitySubsystem ? &MassEntitySubsystem->GetMutableEntityManager() : nullptr;
	return JoinContext;
}

/**
 * Resolves the actor a world query result landed on back to the Crowdy Mass entity it is drawing, and
 * does nothing at all for anything else.
 *
 * The first lookup is also the test for whether this is one of ours: an ordinary actor is not registered
 * with Mass and leaves immediately, which is what keeps the cost of a trace that never touches the crowd
 * where it was.
 *
 * The entity is then required to be tracked by THIS world's Crowdy Mass subsystem, which is exactly the
 * set the row path traces. Inclusion here and exclusion there therefore mirror each other, and an actor
 * belonging to some other Mass agent is not claimed as a Crowdy entity.
 */
static const FCrowdyRemoteIdentityFragment* ResolveEntityBehindActor(const FCrowdyMassEntityJoinContext& JoinContext, const AActor* HitActor, FMassEntityHandle& OutEntity)
{
	OutEntity.Reset();
	if (!HitActor || !JoinContext.IsUsable())
	{
		return nullptr;
	}

	const FMassEntityHandle Entity = JoinContext.ActorSubsystem->GetEntityHandleFromActor(HitActor);
	if (!Entity.IsSet() || !JoinContext.EntityManager->IsEntityValid(Entity))
	{
		return nullptr;
	}

	FGuid TrackedUUID;
	if (!JoinContext.CrowdySubsystem->FindUUIDByEntity(Entity, TrackedUUID))
	{
		return nullptr;
	}

	OutEntity = Entity;
	return JoinContext.EntityManager->GetFragmentDataPtr<FCrowdyRemoteIdentityFragment>(Entity);
}

static void JoinWorldHitToEntity(const FCrowdyMassEntityJoinContext& JoinContext, const AActor* HitActor, FCrowdyMassTraceHit& OutHit)
{
	FMassEntityHandle Entity;
	const FCrowdyRemoteIdentityFragment* IdentityFragment = ResolveEntityBehindActor(JoinContext, HitActor, Entity);
	JoinEntityIdentityToTraceHit(Entity, IdentityFragment, JoinContext.World, OutHit);
}

static void JoinWorldOverlapToEntity(const FCrowdyMassEntityJoinContext& JoinContext, const AActor* OverlapActor, FCrowdyMassOverlapResult& OutOverlap)
{
	FMassEntityHandle Entity;
	const FCrowdyRemoteIdentityFragment* IdentityFragment = ResolveEntityBehindActor(JoinContext, OverlapActor, Entity);
	JoinEntityIdentityToOverlapResult(Entity, IdentityFragment, JoinContext.World, OutOverlap);
}

static void FillWorldTraceResult(const FCrowdyMassResolvedHitZoneTable& HitZones, const FHitResult& WorldHit, const FCrowdyMassEntityJoinContext& JoinContext, FCrowdyMassTraceHit& OutHit)
{
	OutHit.Reset();
	OutHit.bHitMassEntity = false;

	FHitResult NormalizedHit = WorldHit;
	if (!NormalizedHit.GetActor())
	{
		if (UPrimitiveComponent* HitComponent = NormalizedHit.GetComponent())
		{
			if (AActor* HitActor = HitComponent->GetOwner())
			{
				NormalizedHit.HitObjectHandle = FActorInstanceHandle(HitActor, HitComponent, NormalizedHit.Item);
			}
		}
	}

	OutHit.SetHitResult(NormalizedHit);
	PopulateCrowdyEntityInfoFromActor(NormalizedHit.GetActor(), OutHit);

	// After the actor-side identity, and overwriting it where it applies. When the hit landed on a Crowdy
	// Mass entity's own actor, the entity's fragment is the identity that cannot be stale, and the actor's
	// is one lent to it.
	JoinWorldHitToEntity(JoinContext, NormalizedHit.GetActor(), OutHit);

	// Read from the same bone the instanced rows key their shapes on, so a hit through an entity's actor
	// resolves to the same zone the same hit on its row would have.
	OutHit.BoneName = NormalizedHit.BoneName;
	ApplyHitZone(HitZones, OutHit);
}

static void FillMassTraceResult(const FCrowdyMassResolvedHitZoneTable& HitZones, const FInternalHit& MassHit, const FVector& Start, const FVector& End, UWorld* World, FCrowdyMassTraceHit& OutHit)
{
	OutHit.Reset();

	UInstancedSkinnedMeshComponent* Component = MassHit.Component.Get();
	AActor* HitActor = Component ? Component->GetOwner() : nullptr;
	FHitResult SyntheticHit(HitActor, Component, MassHit.ImpactPoint, MassHit.ImpactNormal);
	SyntheticHit.bBlockingHit = true;
	SyntheticHit.TraceStart = Start;
	SyntheticHit.TraceEnd = End;
	SyntheticHit.ImpactPoint = MassHit.ImpactPoint;
	SyntheticHit.Location = MassHit.ImpactPoint;
	SyntheticHit.ImpactNormal = MassHit.ImpactNormal;
	SyntheticHit.Normal = MassHit.ImpactNormal;
	SyntheticHit.Distance = MassHit.Distance;
	SyntheticHit.Time = FVector::Dist(Start, End) > UE_KINDA_SMALL_NUMBER
		? MassHit.Distance / FVector::Dist(Start, End)
		: 0.0f;
	SyntheticHit.BoneName = MassHit.BoneName;
	SyntheticHit.Item = MassHit.InstanceIndex;
	if (Component)
	{
		SyntheticHit.HitObjectHandle = FActorInstanceHandle(HitActor, Component, MassHit.InstanceIndex);
	}

	OutHit.bHitMassEntity = true;
	OutHit.bHitCrowdyEntity = true;
	OutHit.SetHitResult(SyntheticHit);
	OutHit.UUID = MassHit.UUID;
	OutHit.Entity = MassHit.Entity;
	// The world context every entry point already carries as WorldContextObject, threaded through here
	// so a hit can be sent an event to without minting an avatar or re-resolving a world downstream.
	OutHit.Target.Entity = MassHit.Entity;
	OutHit.Target.World = World;
	OutHit.BoneName = MassHit.BoneName;
	ApplyHitZone(HitZones, OutHit);
	OutHit.InstanceIndex = MassHit.InstanceIndex;
}

static bool ExecuteWorldTraceSingle(UWorld& World, const FCrowdyMassTraceShape& Shape, const FCrowdyMassTraceFilter& Filter, const FVector& Start, const FVector& End, const FCollisionQueryParams& QueryParams, FHitResult& OutHit)
{
	if (!Shape.IsLine())
	{
		const FCollisionShape CollisionShape = Shape.MakeCollisionShape();
		switch (Filter.Type)
		{
		case ECrowdyMassTraceFilterType::ObjectTypes:
			return World.SweepSingleByObjectType(OutHit, Start, End, Shape.GetRotation(), Filter.ObjectParams, CollisionShape, QueryParams);
		case ECrowdyMassTraceFilterType::Profile:
			return World.SweepSingleByProfile(OutHit, Start, End, Shape.GetRotation(), Filter.ProfileName, CollisionShape, QueryParams);
		default:
			return World.SweepSingleByChannel(OutHit, Start, End, Shape.GetRotation(), Filter.TraceChannel, CollisionShape, QueryParams);
		}
	}

	switch (Filter.Type)
	{
	case ECrowdyMassTraceFilterType::ObjectTypes:
		return World.LineTraceSingleByObjectType(OutHit, Start, End, Filter.ObjectParams, QueryParams);
	case ECrowdyMassTraceFilterType::Profile:
		return World.LineTraceSingleByProfile(OutHit, Start, End, Filter.ProfileName, QueryParams);
	default:
		return World.LineTraceSingleByChannel(OutHit, Start, End, Filter.TraceChannel, QueryParams);
	}
}

static bool ExecuteWorldTraceMulti(UWorld& World, const FCrowdyMassTraceShape& Shape, const FCrowdyMassTraceFilter& Filter, const FVector& Start, const FVector& End, const FCollisionQueryParams& QueryParams, TArray<FHitResult>& OutHits)
{
	if (!Shape.IsLine())
	{
		const FCollisionShape CollisionShape = Shape.MakeCollisionShape();
		switch (Filter.Type)
		{
		case ECrowdyMassTraceFilterType::ObjectTypes:
			return World.SweepMultiByObjectType(OutHits, Start, End, Shape.GetRotation(), Filter.ObjectParams, CollisionShape, QueryParams);
		case ECrowdyMassTraceFilterType::Profile:
			return World.SweepMultiByProfile(OutHits, Start, End, Shape.GetRotation(), Filter.ProfileName, CollisionShape, QueryParams);
		default:
			return World.SweepMultiByChannel(OutHits, Start, End, Shape.GetRotation(), Filter.TraceChannel, CollisionShape, QueryParams);
		}
	}

	switch (Filter.Type)
	{
	case ECrowdyMassTraceFilterType::ObjectTypes:
		return World.LineTraceMultiByObjectType(OutHits, Start, End, Filter.ObjectParams, QueryParams);
	case ECrowdyMassTraceFilterType::Profile:
		return World.LineTraceMultiByProfile(OutHits, Start, End, Filter.ProfileName, QueryParams);
	default:
		return World.LineTraceMultiByChannel(OutHits, Start, End, Filter.TraceChannel, QueryParams);
	}
}

static float GetFirstBlockingDistance(const TArray<FHitResult>& Hits, const float FallbackDistance)
{
	float BlockingDistance = FallbackDistance;
	for (const FHitResult& Hit : Hits)
	{
		if (Hit.bBlockingHit)
		{
			BlockingDistance = FMath::Min(BlockingDistance, Hit.Distance);
		}
	}
	return BlockingDistance;
}

static void PruneAfterFirstBlockingHit(TArray<FCrowdyMassTraceHit>& Hits)
{
	for (int32 Index = 0; Index < Hits.Num(); ++Index)
	{
		if (Hits[Index].bBlockingHit)
		{
			Hits.SetNum(Index + 1);
			return;
		}
	}
}

static bool ExecuteCrowdyMassTraceSingle(
	const UObject* WorldContextObject,
	const FVector& Start,
	const FVector& End,
	const FCrowdyMassTraceShape& Shape,
	const FCrowdyMassTraceFilter& Filter,
	const bool bTraceComplex,
	const TArray<AActor*>& ActorsToIgnore,
	const EDrawDebugTrace::Type DrawDebugType,
	FCrowdyMassTraceHit& OutHit,
	const bool bIgnoreSelf,
	const FLinearColor& TraceColor,
	const FLinearColor& TraceHitColor,
	const float DrawTime,
	const bool bDrawMassCandidateShapes,
	const bool bDrawSpatialGridCells,
	const FName TraceStatName)
{
	OutHit.Reset();

	UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
	if (!World)
	{
		return false;
	}

	if (!Filter.bIsValid)
	{
		DrawTraceDebug(*World, Shape, Start, End, OutHit, DrawDebugType, TraceColor, TraceHitColor, DrawTime);
		return false;
	}

	const float TraceLength = FVector::Dist(Start, End);
	if (TraceLength <= UE_KINDA_SMALL_NUMBER)
	{
		DrawTraceDebug(*World, Shape, Start, End, OutHit, DrawDebugType, TraceColor, TraceHitColor, DrawTime);
		return false;
	}

	FCollisionQueryParams QueryParams(TraceStatName, bTraceComplex);
	ConfigureQueryParams(QueryParams, ActorsToIgnore, WorldContextObject, bIgnoreSelf);

	FHitResult WorldHit;
	const bool bWorldHit = ExecuteWorldTraceSingle(*World, Shape, Filter, Start, End, QueryParams, WorldHit);
	const float MaxMassDistance = bWorldHit ? WorldHit.Distance : TraceLength;

	// Resolved once for this trace: the table belongs to this world, and looking it up per hit would put
	// a subsystem lookup on the per-hit path for an answer that cannot change during one trace.
	const FCrowdyMassResolvedHitZoneTable& HitZones = GetResolvedHitZoneTable(World);

	// Built before the crowd is walked rather than after, because the crowd pass needs to know what this
	// hit reported. A world hit on a crowd entity's own actor is that entity and its row must not be
	// tested again; a world hit that is not one, including one filtered away before it could be, reports
	// no entity and suppresses nothing.
	FCrowdyMassWorldReportedEntities ReportedEntities;
	if (bWorldHit)
	{
		// Filled straight into the result. A Mass hit is nearer by construction and overwrites this.
		FillWorldTraceResult(HitZones, WorldHit, MakeEntityJoinContext(World), OutHit);
		RecordWorldReportedEntity(OutHit, ReportedEntities);
	}

	FInternalHit MassHit;
	MassHit.Distance = MaxMassDistance;
	FCandidateShapeDebug ShapeDebug;
	ShapeDebug.World = World;
	ShapeDebug.bEnabled = bDrawMassCandidateShapes && DrawDebugType != EDrawDebugTrace::None;
	ShapeDebug.bPersistent = DrawDebugType == EDrawDebugTrace::Persistent;
	ShapeDebug.Lifetime = GetDebugLifetime(DrawDebugType, DrawTime);
	ShapeDebug.Color = FColor::Cyan;

	FSpatialGridDebug GridDebug;
	ConfigureSpatialGridDebug(GridDebug, World, bDrawSpatialGridCells, DrawDebugType, DrawTime);

	if (UCrowdyMassEntitySubsystem* CrowdySubsystem = World->GetSubsystem<UCrowdyMassEntitySubsystem>())
	{
		if (UMassEntitySubsystem* MassEntitySubsystem = World->GetSubsystem<UMassEntitySubsystem>())
		{
			FMassEntityManager& EntityManager = MassEntitySubsystem->GetMutableEntityManager();
			TraceMassSkinnedMeshInstances(*CrowdySubsystem, EntityManager, Start, End, MaxMassDistance, Filter, ReportedEntities, MassHit, nullptr, nullptr, ShapeDebug.ShouldDraw() ? &ShapeDebug : nullptr, GridDebug.ShouldDraw() ? &GridDebug : nullptr, Shape.GetMassInflationRadius());
		}
	}

	if (MassHit.bHit)
	{
		FillMassTraceResult(HitZones, MassHit, Start, End, World, OutHit);
	}

	DrawTraceDebug(*World, Shape, Start, End, OutHit, DrawDebugType, TraceColor, TraceHitColor, DrawTime);
	return OutHit.bBlockingHit;
}

static bool ExecuteCrowdyMassTraceMulti(
	const UObject* WorldContextObject,
	const FVector& Start,
	const FVector& End,
	const FCrowdyMassTraceShape& Shape,
	const FCrowdyMassTraceFilter& Filter,
	const bool bTraceComplex,
	const TArray<AActor*>& ActorsToIgnore,
	const EDrawDebugTrace::Type DrawDebugType,
	TArray<FCrowdyMassTraceHit>& OutHits,
	const bool bIgnoreSelf,
	const FLinearColor& TraceColor,
	const FLinearColor& TraceHitColor,
	const float DrawTime,
	const bool bDrawMassCandidateShapes,
	const bool bDrawSpatialGridCells,
	const FName TraceStatName)
{
	OutHits.Reset();

	UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
	if (!World)
	{
		return false;
	}

	if (!Filter.bIsValid)
	{
		DrawTraceDebug(*World, Shape, Start, End, OutHits, DrawDebugType, TraceColor, TraceHitColor, DrawTime);
		return false;
	}

	const float TraceLength = FVector::Dist(Start, End);
	if (TraceLength <= UE_KINDA_SMALL_NUMBER)
	{
		DrawTraceDebug(*World, Shape, Start, End, OutHits, DrawDebugType, TraceColor, TraceHitColor, DrawTime);
		return false;
	}

	FCollisionQueryParams QueryParams(TraceStatName, bTraceComplex);
	ConfigureQueryParams(QueryParams, ActorsToIgnore, WorldContextObject, bIgnoreSelf);

	TArray<FHitResult> WorldHits;
	ExecuteWorldTraceMulti(*World, Shape, Filter, Start, End, QueryParams, WorldHits);
	const float MaxMassDistance = GetFirstBlockingDistance(WorldHits, TraceLength);

	// Resolved once for this trace rather than per hit; see the single-hit path for why.
	const FCrowdyMassResolvedHitZoneTable& HitZones = GetResolvedHitZoneTable(World);

	// The world results are named first, before the crowd is walked, because which entities they named is
	// what decides whose row may be skipped. An entity whose actor this query filtered out, ignored, or
	// simply did not reach appears in none of them, and its row is the only way anything can hit it.
	OutHits.Reserve(WorldHits.Num());
	FCrowdyMassWorldReportedEntities ReportedEntities;
	if (!WorldHits.IsEmpty())
	{
		// Resolved once for the whole set of world hits, and not at all when there are none.
		const FCrowdyMassEntityJoinContext JoinContext = MakeEntityJoinContext(World);
		for (const FHitResult& WorldHit : WorldHits)
		{
			FCrowdyMassTraceHit& CrowdyHit = OutHits.AddDefaulted_GetRef();
			FillWorldTraceResult(HitZones, WorldHit, JoinContext, CrowdyHit);
			RecordWorldReportedEntity(CrowdyHit, ReportedEntities);
		}
	}

	TArray<FInternalHit> MassHits;
	FInternalHit UnusedBestHit;
	UnusedBestHit.Distance = MaxMassDistance;
	FCandidateShapeDebug ShapeDebug;
	ShapeDebug.World = World;
	ShapeDebug.bEnabled = bDrawMassCandidateShapes && DrawDebugType != EDrawDebugTrace::None;
	ShapeDebug.bPersistent = DrawDebugType == EDrawDebugTrace::Persistent;
	ShapeDebug.Lifetime = GetDebugLifetime(DrawDebugType, DrawTime);
	ShapeDebug.Color = FColor::Cyan;

	FSpatialGridDebug GridDebug;
	ConfigureSpatialGridDebug(GridDebug, World, bDrawSpatialGridCells, DrawDebugType, DrawTime);

	if (UCrowdyMassEntitySubsystem* CrowdySubsystem = World->GetSubsystem<UCrowdyMassEntitySubsystem>())
	{
		if (UMassEntitySubsystem* MassEntitySubsystem = World->GetSubsystem<UMassEntitySubsystem>())
		{
			FMassEntityManager& EntityManager = MassEntitySubsystem->GetMutableEntityManager();
			TraceMassSkinnedMeshInstances(*CrowdySubsystem, EntityManager, Start, End, MaxMassDistance, Filter, ReportedEntities, UnusedBestHit, &MassHits, nullptr, ShapeDebug.ShouldDraw() ? &ShapeDebug : nullptr, GridDebug.ShouldDraw() ? &GridDebug : nullptr, Shape.GetMassInflationRadius());
		}
	}

	OutHits.Reserve(OutHits.Num() + MassHits.Num());
	for (const FInternalHit& MassHit : MassHits)
	{
		FCrowdyMassTraceHit CrowdyHit;
		FillMassTraceResult(HitZones, MassHit, Start, End, World, CrowdyHit);
		OutHits.Add(CrowdyHit);
	}

	OutHits.Sort([](const FCrowdyMassTraceHit& A, const FCrowdyMassTraceHit& B)
	{
		return A.HitResult.Distance < B.HitResult.Distance;
	});
	PruneAfterFirstBlockingHit(OutHits);

	DrawTraceDebug(*World, Shape, Start, End, OutHits, DrawDebugType, TraceColor, TraceHitColor, DrawTime);
	return !OutHits.IsEmpty();
}

struct FCrowdyMassOverlapShape
{
	ECrowdyMassTraceShapeType Type = ECrowdyMassTraceShapeType::Sphere;
	FVector Position = FVector::ZeroVector;
	FVector Extent = FVector::ZeroVector;
	float Radius = 0.0f;
	float HalfHeight = 0.0f;
	FRotator Orientation = FRotator::ZeroRotator;

	FCollisionShape MakeCollisionShape() const
	{
		switch (Type)
		{
		case ECrowdyMassTraceShapeType::Box:
			return FCollisionShape::MakeBox(Extent.GetAbs());
		case ECrowdyMassTraceShapeType::Capsule:
			return FCollisionShape::MakeCapsule(FMath::Max(Radius, 0.0f), FMath::Max(HalfHeight, 0.0f));
		default:
			return FCollisionShape::MakeSphere(FMath::Max(Radius, 0.0f));
		}
	}

	FQuat GetRotation() const
	{
		return Type == ECrowdyMassTraceShapeType::Sphere ? FQuat::Identity : Orientation.Quaternion();
	}

	float GetQueryRadius() const
	{
		switch (Type)
		{
		case ECrowdyMassTraceShapeType::Box:
			return Extent.GetAbs().Size();
		case ECrowdyMassTraceShapeType::Capsule:
			return FMath::Max(Radius, 0.0f) + FMath::Max(HalfHeight, 0.0f);
		default:
			return FMath::Max(Radius, 0.0f);
		}
	}
};

static bool DoesBoundsOverlapShape(const FVector& BoundsOrigin, const float BoundsRadius, const FCrowdyMassOverlapShape& Shape)
{
	switch (Shape.Type)
	{
	case ECrowdyMassTraceShapeType::Box:
	{
		const FVector LocalPoint = Shape.Orientation.UnrotateVector(BoundsOrigin - Shape.Position);
		const FVector AbsExtent = Shape.Extent.GetAbs();
		const FVector ClosestPoint(
			FMath::Clamp(LocalPoint.X, -AbsExtent.X, AbsExtent.X),
			FMath::Clamp(LocalPoint.Y, -AbsExtent.Y, AbsExtent.Y),
			FMath::Clamp(LocalPoint.Z, -AbsExtent.Z, AbsExtent.Z));
		return FVector::DistSquared(LocalPoint, ClosestPoint) <= FMath::Square(BoundsRadius);
	}
	case ECrowdyMassTraceShapeType::Capsule:
	{
		const FVector LocalPoint = Shape.Orientation.UnrotateVector(BoundsOrigin - Shape.Position);
		const FVector CapsuleStart(0.0f, 0.0f, -Shape.HalfHeight);
		const FVector CapsuleEnd(0.0f, 0.0f, Shape.HalfHeight);
		return GetLocalCapsuleDistanceSquared(LocalPoint, CapsuleStart, CapsuleEnd) <= FMath::Square(Shape.Radius + BoundsRadius);
	}
	default:
		return FVector::DistSquared(BoundsOrigin, Shape.Position) <= FMath::Square(Shape.Radius + BoundsRadius);
	}
}

static void AddWorldOverlapResult(UPrimitiveComponent* Component, const bool bActorQuery, UClass* ClassFilter, const FCrowdyMassEntityJoinContext& JoinContext, TSet<TWeakObjectPtr<AActor>>& AddedActors, FCrowdyMassWorldReportedEntities& ReportedEntities, TArray<FCrowdyMassOverlapResult>& OutOverlaps)
{
	if (!Component)
	{
		return;
	}

	AActor* Actor = Component->GetOwner();
	if (bActorQuery)
	{
		if (!Actor || (ClassFilter && !Actor->IsA(ClassFilter)) || AddedActors.Contains(Actor))
		{
			return;
		}
		AddedActors.Add(Actor);
	}
	else if (ClassFilter && !Component->IsA(ClassFilter))
	{
		return;
	}

	FCrowdyMassOverlapResult Result;
	Result.Actor = Actor;
	Result.Component = Component;
	PopulateCrowdyEntityInfoFromActor(Actor, Result);

	// An actor drawing a Crowdy Mass entity is reported as that entity, addressable and named from the
	// entity's own fragment, and recorded so the Mass pass below leaves it alone. Both of the early
	// returns above deliberately record nothing: a result rejected by the class filter, or already
	// reported through another of its components, did not add an entity here for anything to skip.
	JoinWorldOverlapToEntity(JoinContext, Actor, Result);
	RecordWorldReportedEntity(Result, ReportedEntities);
	OutOverlaps.Add(Result);
}

static bool ExecuteCrowdyMassOverlap(
	const UObject* WorldContextObject,
	const FCrowdyMassOverlapShape& Shape,
	const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes,
	UClass* ClassFilter,
	const TArray<AActor*>& ActorsToIgnore,
	const bool bActorQuery,
	TArray<FCrowdyMassOverlapResult>& OutOverlaps)
{
	OutOverlaps.Reset();

	UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
	if (!World)
	{
		return false;
	}

	const FCrowdyMassTraceFilter ObjectFilter = MakeObjectTraceFilter(ObjectTypes);
	if (!ObjectFilter.bIsValid)
	{
		return false;
	}

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT_NAME_ONLY(CrowdyMassOverlap), false);
	ConfigureQueryParams(QueryParams, ActorsToIgnore, WorldContextObject, false);

	TArray<FOverlapResult> WorldOverlaps;
	World->OverlapMultiByObjectType(WorldOverlaps, Shape.Position, Shape.GetRotation(), ObjectFilter.ObjectParams, Shape.MakeCollisionShape(), QueryParams);

	const FCrowdyMassEntityJoinContext JoinContext = MakeEntityJoinContext(World);

	TSet<TWeakObjectPtr<AActor>> AddedActors;
	FCrowdyMassWorldReportedEntities ReportedEntities;
	for (const FOverlapResult& WorldOverlap : WorldOverlaps)
	{
		AddWorldOverlapResult(WorldOverlap.Component.Get(), bActorQuery, ClassFilter, JoinContext, AddedActors, ReportedEntities, OutOverlaps);
	}

	UCrowdyMassEntitySubsystem* CrowdySubsystem = World->GetSubsystem<UCrowdyMassEntitySubsystem>();
	UMassEntitySubsystem* MassEntitySubsystem = World->GetSubsystem<UMassEntitySubsystem>();
	if (!CrowdySubsystem || !MassEntitySubsystem)
	{
		return !OutOverlaps.IsEmpty();
	}

	FMassEntityManager& EntityManager = MassEntitySubsystem->GetMutableEntityManager();
	TArray<FMassEntityHandle> CandidateEntities;
	int32 VisitedCellCount = 0;
	CrowdySubsystem->GetOverlapCandidateEntities(Shape.Position, Shape.GetQueryRadius() + 150.0f, CandidateEntities, VisitedCellCount);

	for (const FMassEntityHandle Entity : CandidateEntities)
	{
		// An entity the world pass above already reported, through an actor of its own and joined back to
		// this same entity there. Reporting it again here would hand every overlap two results for one
		// player, which no caller can tell from two players. Asked before the entity is resolved at all,
		// since it needs nothing but the handle.
		//
		// Only what that pass actually reported is skipped. An entity whose actor the class filter or the
		// ignore list dropped is not in here, and is reported from its bounds below: the alternative is an
		// overlap that silently returns fewer entities the closer they stand to its centre.
		if (ReportedEntities.Contains(Entity))
		{
			continue;
		}

		// One view per candidate, exactly as the trace path does it: the three fragments below would
		// otherwise each resolve the entity to its chunk before looking their own type up.
		const FMassEntityView EntityView = FMassEntityView::TryMakeView(EntityManager, Entity);
		if (!EntityView.IsSet())
		{
			continue;
		}

		const FCrowdyMassTraceCollisionFragment* TraceCollisionFragment = EntityView.GetFragmentDataPtr<FCrowdyMassTraceCollisionFragment>();
		if (!TraceCollisionFragment)
		{
			CrowdySubsystem->ReportMissingTraceCollisionFragment();
			continue;
		}

		if (!DoesEntityPassTraceFilter(TraceCollisionFragment, ObjectFilter))
		{
			continue;
		}

		const FCrowdyRemoteIdentityFragment* IdentityFragment = EntityView.GetFragmentDataPtr<FCrowdyRemoteIdentityFragment>();
		const FTransformFragment* TransformFragment = EntityView.GetFragmentDataPtr<FTransformFragment>();
		if (!IdentityFragment || !TransformFragment)
		{
			continue;
		}

		const FVector BoundsOrigin = TransformFragment->GetTransform().GetLocation();
		constexpr float DefaultMassOverlapBoundsRadius = 120.0f;
		if (!DoesBoundsOverlapShape(BoundsOrigin, DefaultMassOverlapBoundsRadius, Shape))
		{
			continue;
		}

		FCrowdyMassOverlapResult Result;
		Result.bOverlapMassEntity = true;
		Result.bOverlapCrowdyEntity = true;
		Result.UUID = IdentityFragment->UUID;
		Result.Entity = Entity;
		// The world this overlap was run in, carried beside the handle so an overlap can be acted on
		// without anything downstream having to re-resolve a world or handle an id.
		Result.Target.Entity = Entity;
		Result.Target.World = World;
		OutOverlaps.Add(Result);
	}

	return !OutOverlaps.IsEmpty();
}
}

bool UCrowdyMassTraceBlueprintLibrary::CrowdyMassLineTraceSingle(
	const UObject* WorldContextObject,
	const FVector Start,
	const FVector End,
	const ETraceTypeQuery TraceChannel,
	const bool bTraceComplex,
	const TArray<AActor*>& ActorsToIgnore,
	const EDrawDebugTrace::Type DrawDebugType,
	FCrowdyMassTraceHit& OutHit,
	const bool bIgnoreSelf,
	const FLinearColor TraceColor,
	const FLinearColor TraceHitColor,
	const float DrawTime,
	const bool bDrawMassCandidateShapes,
	const bool bDrawSpatialGridCells)
{
	return CrowdyMassTrace::ExecuteCrowdyMassTraceSingle(
		WorldContextObject,
		Start,
		End,
		CrowdyMassTrace::FCrowdyMassTraceShape::MakeLine(),
		CrowdyMassTrace::MakeChannelTraceFilter(TraceChannel),
		bTraceComplex,
		ActorsToIgnore,
		DrawDebugType,
		OutHit,
		bIgnoreSelf,
		TraceColor,
		TraceHitColor,
		DrawTime,
		bDrawMassCandidateShapes,
		bDrawSpatialGridCells,
		SCENE_QUERY_STAT_NAME_ONLY(CrowdyMassLineTraceSingle));
}

bool UCrowdyMassTraceBlueprintLibrary::CrowdyMassLineTraceMulti(const UObject* WorldContextObject, const FVector Start, const FVector End, const ETraceTypeQuery TraceChannel, const bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, const EDrawDebugTrace::Type DrawDebugType, TArray<FCrowdyMassTraceHit>& OutHits, const bool bIgnoreSelf, const FLinearColor TraceColor, const FLinearColor TraceHitColor, const float DrawTime, const bool bDrawMassCandidateShapes, const bool bDrawSpatialGridCells)
{
	return CrowdyMassTrace::ExecuteCrowdyMassTraceMulti(WorldContextObject, Start, End, CrowdyMassTrace::FCrowdyMassTraceShape::MakeLine(), CrowdyMassTrace::MakeChannelTraceFilter(TraceChannel), bTraceComplex, ActorsToIgnore, DrawDebugType, OutHits, bIgnoreSelf, TraceColor, TraceHitColor, DrawTime, bDrawMassCandidateShapes, bDrawSpatialGridCells, SCENE_QUERY_STAT_NAME_ONLY(CrowdyMassLineTraceMulti));
}

bool UCrowdyMassTraceBlueprintLibrary::CrowdyMassSphereTraceSingle(const UObject* WorldContextObject, const FVector Start, const FVector End, const float Radius, const ETraceTypeQuery TraceChannel, const bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, const EDrawDebugTrace::Type DrawDebugType, FCrowdyMassTraceHit& OutHit, const bool bIgnoreSelf, const FLinearColor TraceColor, const FLinearColor TraceHitColor, const float DrawTime, const bool bDrawMassCandidateShapes, const bool bDrawSpatialGridCells)
{
	return CrowdyMassTrace::ExecuteCrowdyMassTraceSingle(WorldContextObject, Start, End, CrowdyMassTrace::FCrowdyMassTraceShape::MakeSphere(Radius), CrowdyMassTrace::MakeChannelTraceFilter(TraceChannel), bTraceComplex, ActorsToIgnore, DrawDebugType, OutHit, bIgnoreSelf, TraceColor, TraceHitColor, DrawTime, bDrawMassCandidateShapes, bDrawSpatialGridCells, SCENE_QUERY_STAT_NAME_ONLY(CrowdyMassSphereTraceSingle));
}

bool UCrowdyMassTraceBlueprintLibrary::CrowdyMassSphereTraceMulti(const UObject* WorldContextObject, const FVector Start, const FVector End, const float Radius, const ETraceTypeQuery TraceChannel, const bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, const EDrawDebugTrace::Type DrawDebugType, TArray<FCrowdyMassTraceHit>& OutHits, const bool bIgnoreSelf, const FLinearColor TraceColor, const FLinearColor TraceHitColor, const float DrawTime, const bool bDrawMassCandidateShapes, const bool bDrawSpatialGridCells)
{
	return CrowdyMassTrace::ExecuteCrowdyMassTraceMulti(WorldContextObject, Start, End, CrowdyMassTrace::FCrowdyMassTraceShape::MakeSphere(Radius), CrowdyMassTrace::MakeChannelTraceFilter(TraceChannel), bTraceComplex, ActorsToIgnore, DrawDebugType, OutHits, bIgnoreSelf, TraceColor, TraceHitColor, DrawTime, bDrawMassCandidateShapes, bDrawSpatialGridCells, SCENE_QUERY_STAT_NAME_ONLY(CrowdyMassSphereTraceMulti));
}

bool UCrowdyMassTraceBlueprintLibrary::CrowdyMassBoxTraceSingle(const UObject* WorldContextObject, const FVector Start, const FVector End, const FVector HalfSize, const FRotator Orientation, const ETraceTypeQuery TraceChannel, const bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, const EDrawDebugTrace::Type DrawDebugType, FCrowdyMassTraceHit& OutHit, const bool bIgnoreSelf, const FLinearColor TraceColor, const FLinearColor TraceHitColor, const float DrawTime, const bool bDrawMassCandidateShapes, const bool bDrawSpatialGridCells)
{
	return CrowdyMassTrace::ExecuteCrowdyMassTraceSingle(WorldContextObject, Start, End, CrowdyMassTrace::FCrowdyMassTraceShape::MakeBox(HalfSize, Orientation), CrowdyMassTrace::MakeChannelTraceFilter(TraceChannel), bTraceComplex, ActorsToIgnore, DrawDebugType, OutHit, bIgnoreSelf, TraceColor, TraceHitColor, DrawTime, bDrawMassCandidateShapes, bDrawSpatialGridCells, SCENE_QUERY_STAT_NAME_ONLY(CrowdyMassBoxTraceSingle));
}

bool UCrowdyMassTraceBlueprintLibrary::CrowdyMassBoxTraceMulti(const UObject* WorldContextObject, const FVector Start, const FVector End, const FVector HalfSize, const FRotator Orientation, const ETraceTypeQuery TraceChannel, const bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, const EDrawDebugTrace::Type DrawDebugType, TArray<FCrowdyMassTraceHit>& OutHits, const bool bIgnoreSelf, const FLinearColor TraceColor, const FLinearColor TraceHitColor, const float DrawTime, const bool bDrawMassCandidateShapes, const bool bDrawSpatialGridCells)
{
	return CrowdyMassTrace::ExecuteCrowdyMassTraceMulti(WorldContextObject, Start, End, CrowdyMassTrace::FCrowdyMassTraceShape::MakeBox(HalfSize, Orientation), CrowdyMassTrace::MakeChannelTraceFilter(TraceChannel), bTraceComplex, ActorsToIgnore, DrawDebugType, OutHits, bIgnoreSelf, TraceColor, TraceHitColor, DrawTime, bDrawMassCandidateShapes, bDrawSpatialGridCells, SCENE_QUERY_STAT_NAME_ONLY(CrowdyMassBoxTraceMulti));
}

bool UCrowdyMassTraceBlueprintLibrary::CrowdyMassCapsuleTraceSingle(const UObject* WorldContextObject, const FVector Start, const FVector End, const float Radius, const float HalfHeight, const ETraceTypeQuery TraceChannel, const bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, const EDrawDebugTrace::Type DrawDebugType, FCrowdyMassTraceHit& OutHit, const bool bIgnoreSelf, const FLinearColor TraceColor, const FLinearColor TraceHitColor, const float DrawTime, const bool bDrawMassCandidateShapes, const bool bDrawSpatialGridCells)
{
	return CrowdyMassTrace::ExecuteCrowdyMassTraceSingle(WorldContextObject, Start, End, CrowdyMassTrace::FCrowdyMassTraceShape::MakeCapsule(Radius, HalfHeight), CrowdyMassTrace::MakeChannelTraceFilter(TraceChannel), bTraceComplex, ActorsToIgnore, DrawDebugType, OutHit, bIgnoreSelf, TraceColor, TraceHitColor, DrawTime, bDrawMassCandidateShapes, bDrawSpatialGridCells, SCENE_QUERY_STAT_NAME_ONLY(CrowdyMassCapsuleTraceSingle));
}

bool UCrowdyMassTraceBlueprintLibrary::CrowdyMassCapsuleTraceMulti(const UObject* WorldContextObject, const FVector Start, const FVector End, const float Radius, const float HalfHeight, const ETraceTypeQuery TraceChannel, const bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, const EDrawDebugTrace::Type DrawDebugType, TArray<FCrowdyMassTraceHit>& OutHits, const bool bIgnoreSelf, const FLinearColor TraceColor, const FLinearColor TraceHitColor, const float DrawTime, const bool bDrawMassCandidateShapes, const bool bDrawSpatialGridCells)
{
	return CrowdyMassTrace::ExecuteCrowdyMassTraceMulti(WorldContextObject, Start, End, CrowdyMassTrace::FCrowdyMassTraceShape::MakeCapsule(Radius, HalfHeight), CrowdyMassTrace::MakeChannelTraceFilter(TraceChannel), bTraceComplex, ActorsToIgnore, DrawDebugType, OutHits, bIgnoreSelf, TraceColor, TraceHitColor, DrawTime, bDrawMassCandidateShapes, bDrawSpatialGridCells, SCENE_QUERY_STAT_NAME_ONLY(CrowdyMassCapsuleTraceMulti));
}

bool UCrowdyMassTraceBlueprintLibrary::CrowdyMassLineTraceSingleForObjects(const UObject* WorldContextObject, const FVector Start, const FVector End, const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes, const bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, const EDrawDebugTrace::Type DrawDebugType, FCrowdyMassTraceHit& OutHit, const bool bIgnoreSelf, const FLinearColor TraceColor, const FLinearColor TraceHitColor, const float DrawTime, const bool bDrawMassCandidateShapes, const bool bDrawSpatialGridCells)
{
	return CrowdyMassTrace::ExecuteCrowdyMassTraceSingle(WorldContextObject, Start, End, CrowdyMassTrace::FCrowdyMassTraceShape::MakeLine(), CrowdyMassTrace::MakeObjectTraceFilter(ObjectTypes), bTraceComplex, ActorsToIgnore, DrawDebugType, OutHit, bIgnoreSelf, TraceColor, TraceHitColor, DrawTime, bDrawMassCandidateShapes, bDrawSpatialGridCells, SCENE_QUERY_STAT_NAME_ONLY(CrowdyMassLineTraceSingleForObjects));
}

bool UCrowdyMassTraceBlueprintLibrary::CrowdyMassLineTraceMultiForObjects(const UObject* WorldContextObject, const FVector Start, const FVector End, const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes, const bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, const EDrawDebugTrace::Type DrawDebugType, TArray<FCrowdyMassTraceHit>& OutHits, const bool bIgnoreSelf, const FLinearColor TraceColor, const FLinearColor TraceHitColor, const float DrawTime, const bool bDrawMassCandidateShapes, const bool bDrawSpatialGridCells)
{
	return CrowdyMassTrace::ExecuteCrowdyMassTraceMulti(WorldContextObject, Start, End, CrowdyMassTrace::FCrowdyMassTraceShape::MakeLine(), CrowdyMassTrace::MakeObjectTraceFilter(ObjectTypes), bTraceComplex, ActorsToIgnore, DrawDebugType, OutHits, bIgnoreSelf, TraceColor, TraceHitColor, DrawTime, bDrawMassCandidateShapes, bDrawSpatialGridCells, SCENE_QUERY_STAT_NAME_ONLY(CrowdyMassLineTraceMultiForObjects));
}

bool UCrowdyMassTraceBlueprintLibrary::CrowdyMassSphereTraceSingleForObjects(const UObject* WorldContextObject, const FVector Start, const FVector End, const float Radius, const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes, const bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, const EDrawDebugTrace::Type DrawDebugType, FCrowdyMassTraceHit& OutHit, const bool bIgnoreSelf, const FLinearColor TraceColor, const FLinearColor TraceHitColor, const float DrawTime, const bool bDrawMassCandidateShapes, const bool bDrawSpatialGridCells)
{
	return CrowdyMassTrace::ExecuteCrowdyMassTraceSingle(WorldContextObject, Start, End, CrowdyMassTrace::FCrowdyMassTraceShape::MakeSphere(Radius), CrowdyMassTrace::MakeObjectTraceFilter(ObjectTypes), bTraceComplex, ActorsToIgnore, DrawDebugType, OutHit, bIgnoreSelf, TraceColor, TraceHitColor, DrawTime, bDrawMassCandidateShapes, bDrawSpatialGridCells, SCENE_QUERY_STAT_NAME_ONLY(CrowdyMassSphereTraceSingleForObjects));
}

bool UCrowdyMassTraceBlueprintLibrary::CrowdyMassSphereTraceMultiForObjects(const UObject* WorldContextObject, const FVector Start, const FVector End, const float Radius, const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes, const bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, const EDrawDebugTrace::Type DrawDebugType, TArray<FCrowdyMassTraceHit>& OutHits, const bool bIgnoreSelf, const FLinearColor TraceColor, const FLinearColor TraceHitColor, const float DrawTime, const bool bDrawMassCandidateShapes, const bool bDrawSpatialGridCells)
{
	return CrowdyMassTrace::ExecuteCrowdyMassTraceMulti(WorldContextObject, Start, End, CrowdyMassTrace::FCrowdyMassTraceShape::MakeSphere(Radius), CrowdyMassTrace::MakeObjectTraceFilter(ObjectTypes), bTraceComplex, ActorsToIgnore, DrawDebugType, OutHits, bIgnoreSelf, TraceColor, TraceHitColor, DrawTime, bDrawMassCandidateShapes, bDrawSpatialGridCells, SCENE_QUERY_STAT_NAME_ONLY(CrowdyMassSphereTraceMultiForObjects));
}

bool UCrowdyMassTraceBlueprintLibrary::CrowdyMassBoxTraceSingleForObjects(const UObject* WorldContextObject, const FVector Start, const FVector End, const FVector HalfSize, const FRotator Orientation, const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes, const bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, const EDrawDebugTrace::Type DrawDebugType, FCrowdyMassTraceHit& OutHit, const bool bIgnoreSelf, const FLinearColor TraceColor, const FLinearColor TraceHitColor, const float DrawTime, const bool bDrawMassCandidateShapes, const bool bDrawSpatialGridCells)
{
	return CrowdyMassTrace::ExecuteCrowdyMassTraceSingle(WorldContextObject, Start, End, CrowdyMassTrace::FCrowdyMassTraceShape::MakeBox(HalfSize, Orientation), CrowdyMassTrace::MakeObjectTraceFilter(ObjectTypes), bTraceComplex, ActorsToIgnore, DrawDebugType, OutHit, bIgnoreSelf, TraceColor, TraceHitColor, DrawTime, bDrawMassCandidateShapes, bDrawSpatialGridCells, SCENE_QUERY_STAT_NAME_ONLY(CrowdyMassBoxTraceSingleForObjects));
}

bool UCrowdyMassTraceBlueprintLibrary::CrowdyMassBoxTraceMultiForObjects(const UObject* WorldContextObject, const FVector Start, const FVector End, const FVector HalfSize, const FRotator Orientation, const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes, const bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, const EDrawDebugTrace::Type DrawDebugType, TArray<FCrowdyMassTraceHit>& OutHits, const bool bIgnoreSelf, const FLinearColor TraceColor, const FLinearColor TraceHitColor, const float DrawTime, const bool bDrawMassCandidateShapes, const bool bDrawSpatialGridCells)
{
	return CrowdyMassTrace::ExecuteCrowdyMassTraceMulti(WorldContextObject, Start, End, CrowdyMassTrace::FCrowdyMassTraceShape::MakeBox(HalfSize, Orientation), CrowdyMassTrace::MakeObjectTraceFilter(ObjectTypes), bTraceComplex, ActorsToIgnore, DrawDebugType, OutHits, bIgnoreSelf, TraceColor, TraceHitColor, DrawTime, bDrawMassCandidateShapes, bDrawSpatialGridCells, SCENE_QUERY_STAT_NAME_ONLY(CrowdyMassBoxTraceMultiForObjects));
}

bool UCrowdyMassTraceBlueprintLibrary::CrowdyMassCapsuleTraceSingleForObjects(const UObject* WorldContextObject, const FVector Start, const FVector End, const float Radius, const float HalfHeight, const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes, const bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, const EDrawDebugTrace::Type DrawDebugType, FCrowdyMassTraceHit& OutHit, const bool bIgnoreSelf, const FLinearColor TraceColor, const FLinearColor TraceHitColor, const float DrawTime, const bool bDrawMassCandidateShapes, const bool bDrawSpatialGridCells)
{
	return CrowdyMassTrace::ExecuteCrowdyMassTraceSingle(WorldContextObject, Start, End, CrowdyMassTrace::FCrowdyMassTraceShape::MakeCapsule(Radius, HalfHeight), CrowdyMassTrace::MakeObjectTraceFilter(ObjectTypes), bTraceComplex, ActorsToIgnore, DrawDebugType, OutHit, bIgnoreSelf, TraceColor, TraceHitColor, DrawTime, bDrawMassCandidateShapes, bDrawSpatialGridCells, SCENE_QUERY_STAT_NAME_ONLY(CrowdyMassCapsuleTraceSingleForObjects));
}

bool UCrowdyMassTraceBlueprintLibrary::CrowdyMassCapsuleTraceMultiForObjects(const UObject* WorldContextObject, const FVector Start, const FVector End, const float Radius, const float HalfHeight, const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes, const bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, const EDrawDebugTrace::Type DrawDebugType, TArray<FCrowdyMassTraceHit>& OutHits, const bool bIgnoreSelf, const FLinearColor TraceColor, const FLinearColor TraceHitColor, const float DrawTime, const bool bDrawMassCandidateShapes, const bool bDrawSpatialGridCells)
{
	return CrowdyMassTrace::ExecuteCrowdyMassTraceMulti(WorldContextObject, Start, End, CrowdyMassTrace::FCrowdyMassTraceShape::MakeCapsule(Radius, HalfHeight), CrowdyMassTrace::MakeObjectTraceFilter(ObjectTypes), bTraceComplex, ActorsToIgnore, DrawDebugType, OutHits, bIgnoreSelf, TraceColor, TraceHitColor, DrawTime, bDrawMassCandidateShapes, bDrawSpatialGridCells, SCENE_QUERY_STAT_NAME_ONLY(CrowdyMassCapsuleTraceMultiForObjects));
}

bool UCrowdyMassTraceBlueprintLibrary::CrowdyMassLineTraceSingleByProfile(const UObject* WorldContextObject, const FVector Start, const FVector End, const FName ProfileName, const bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, const EDrawDebugTrace::Type DrawDebugType, FCrowdyMassTraceHit& OutHit, const bool bIgnoreSelf, const FLinearColor TraceColor, const FLinearColor TraceHitColor, const float DrawTime, const bool bDrawMassCandidateShapes, const bool bDrawSpatialGridCells)
{
	return CrowdyMassTrace::ExecuteCrowdyMassTraceSingle(WorldContextObject, Start, End, CrowdyMassTrace::FCrowdyMassTraceShape::MakeLine(), CrowdyMassTrace::MakeProfileTraceFilter(ProfileName), bTraceComplex, ActorsToIgnore, DrawDebugType, OutHit, bIgnoreSelf, TraceColor, TraceHitColor, DrawTime, bDrawMassCandidateShapes, bDrawSpatialGridCells, SCENE_QUERY_STAT_NAME_ONLY(CrowdyMassLineTraceSingleByProfile));
}

bool UCrowdyMassTraceBlueprintLibrary::CrowdyMassLineTraceMultiByProfile(const UObject* WorldContextObject, const FVector Start, const FVector End, const FName ProfileName, const bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, const EDrawDebugTrace::Type DrawDebugType, TArray<FCrowdyMassTraceHit>& OutHits, const bool bIgnoreSelf, const FLinearColor TraceColor, const FLinearColor TraceHitColor, const float DrawTime, const bool bDrawMassCandidateShapes, const bool bDrawSpatialGridCells)
{
	return CrowdyMassTrace::ExecuteCrowdyMassTraceMulti(WorldContextObject, Start, End, CrowdyMassTrace::FCrowdyMassTraceShape::MakeLine(), CrowdyMassTrace::MakeProfileTraceFilter(ProfileName), bTraceComplex, ActorsToIgnore, DrawDebugType, OutHits, bIgnoreSelf, TraceColor, TraceHitColor, DrawTime, bDrawMassCandidateShapes, bDrawSpatialGridCells, SCENE_QUERY_STAT_NAME_ONLY(CrowdyMassLineTraceMultiByProfile));
}

bool UCrowdyMassTraceBlueprintLibrary::CrowdyMassSphereTraceSingleByProfile(const UObject* WorldContextObject, const FVector Start, const FVector End, const float Radius, const FName ProfileName, const bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, const EDrawDebugTrace::Type DrawDebugType, FCrowdyMassTraceHit& OutHit, const bool bIgnoreSelf, const FLinearColor TraceColor, const FLinearColor TraceHitColor, const float DrawTime, const bool bDrawMassCandidateShapes, const bool bDrawSpatialGridCells)
{
	return CrowdyMassTrace::ExecuteCrowdyMassTraceSingle(WorldContextObject, Start, End, CrowdyMassTrace::FCrowdyMassTraceShape::MakeSphere(Radius), CrowdyMassTrace::MakeProfileTraceFilter(ProfileName), bTraceComplex, ActorsToIgnore, DrawDebugType, OutHit, bIgnoreSelf, TraceColor, TraceHitColor, DrawTime, bDrawMassCandidateShapes, bDrawSpatialGridCells, SCENE_QUERY_STAT_NAME_ONLY(CrowdyMassSphereTraceSingleByProfile));
}

bool UCrowdyMassTraceBlueprintLibrary::CrowdyMassSphereTraceMultiByProfile(const UObject* WorldContextObject, const FVector Start, const FVector End, const float Radius, const FName ProfileName, const bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, const EDrawDebugTrace::Type DrawDebugType, TArray<FCrowdyMassTraceHit>& OutHits, const bool bIgnoreSelf, const FLinearColor TraceColor, const FLinearColor TraceHitColor, const float DrawTime, const bool bDrawMassCandidateShapes, const bool bDrawSpatialGridCells)
{
	return CrowdyMassTrace::ExecuteCrowdyMassTraceMulti(WorldContextObject, Start, End, CrowdyMassTrace::FCrowdyMassTraceShape::MakeSphere(Radius), CrowdyMassTrace::MakeProfileTraceFilter(ProfileName), bTraceComplex, ActorsToIgnore, DrawDebugType, OutHits, bIgnoreSelf, TraceColor, TraceHitColor, DrawTime, bDrawMassCandidateShapes, bDrawSpatialGridCells, SCENE_QUERY_STAT_NAME_ONLY(CrowdyMassSphereTraceMultiByProfile));
}

bool UCrowdyMassTraceBlueprintLibrary::CrowdyMassBoxTraceSingleByProfile(const UObject* WorldContextObject, const FVector Start, const FVector End, const FVector HalfSize, const FRotator Orientation, const FName ProfileName, const bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, const EDrawDebugTrace::Type DrawDebugType, FCrowdyMassTraceHit& OutHit, const bool bIgnoreSelf, const FLinearColor TraceColor, const FLinearColor TraceHitColor, const float DrawTime, const bool bDrawMassCandidateShapes, const bool bDrawSpatialGridCells)
{
	return CrowdyMassTrace::ExecuteCrowdyMassTraceSingle(WorldContextObject, Start, End, CrowdyMassTrace::FCrowdyMassTraceShape::MakeBox(HalfSize, Orientation), CrowdyMassTrace::MakeProfileTraceFilter(ProfileName), bTraceComplex, ActorsToIgnore, DrawDebugType, OutHit, bIgnoreSelf, TraceColor, TraceHitColor, DrawTime, bDrawMassCandidateShapes, bDrawSpatialGridCells, SCENE_QUERY_STAT_NAME_ONLY(CrowdyMassBoxTraceSingleByProfile));
}

bool UCrowdyMassTraceBlueprintLibrary::CrowdyMassBoxTraceMultiByProfile(const UObject* WorldContextObject, const FVector Start, const FVector End, const FVector HalfSize, const FRotator Orientation, const FName ProfileName, const bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, const EDrawDebugTrace::Type DrawDebugType, TArray<FCrowdyMassTraceHit>& OutHits, const bool bIgnoreSelf, const FLinearColor TraceColor, const FLinearColor TraceHitColor, const float DrawTime, const bool bDrawMassCandidateShapes, const bool bDrawSpatialGridCells)
{
	return CrowdyMassTrace::ExecuteCrowdyMassTraceMulti(WorldContextObject, Start, End, CrowdyMassTrace::FCrowdyMassTraceShape::MakeBox(HalfSize, Orientation), CrowdyMassTrace::MakeProfileTraceFilter(ProfileName), bTraceComplex, ActorsToIgnore, DrawDebugType, OutHits, bIgnoreSelf, TraceColor, TraceHitColor, DrawTime, bDrawMassCandidateShapes, bDrawSpatialGridCells, SCENE_QUERY_STAT_NAME_ONLY(CrowdyMassBoxTraceMultiByProfile));
}

bool UCrowdyMassTraceBlueprintLibrary::CrowdyMassCapsuleTraceSingleByProfile(const UObject* WorldContextObject, const FVector Start, const FVector End, const float Radius, const float HalfHeight, const FName ProfileName, const bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, const EDrawDebugTrace::Type DrawDebugType, FCrowdyMassTraceHit& OutHit, const bool bIgnoreSelf, const FLinearColor TraceColor, const FLinearColor TraceHitColor, const float DrawTime, const bool bDrawMassCandidateShapes, const bool bDrawSpatialGridCells)
{
	return CrowdyMassTrace::ExecuteCrowdyMassTraceSingle(WorldContextObject, Start, End, CrowdyMassTrace::FCrowdyMassTraceShape::MakeCapsule(Radius, HalfHeight), CrowdyMassTrace::MakeProfileTraceFilter(ProfileName), bTraceComplex, ActorsToIgnore, DrawDebugType, OutHit, bIgnoreSelf, TraceColor, TraceHitColor, DrawTime, bDrawMassCandidateShapes, bDrawSpatialGridCells, SCENE_QUERY_STAT_NAME_ONLY(CrowdyMassCapsuleTraceSingleByProfile));
}

bool UCrowdyMassTraceBlueprintLibrary::CrowdyMassCapsuleTraceMultiByProfile(const UObject* WorldContextObject, const FVector Start, const FVector End, const float Radius, const float HalfHeight, const FName ProfileName, const bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, const EDrawDebugTrace::Type DrawDebugType, TArray<FCrowdyMassTraceHit>& OutHits, const bool bIgnoreSelf, const FLinearColor TraceColor, const FLinearColor TraceHitColor, const float DrawTime, const bool bDrawMassCandidateShapes, const bool bDrawSpatialGridCells)
{
	return CrowdyMassTrace::ExecuteCrowdyMassTraceMulti(WorldContextObject, Start, End, CrowdyMassTrace::FCrowdyMassTraceShape::MakeCapsule(Radius, HalfHeight), CrowdyMassTrace::MakeProfileTraceFilter(ProfileName), bTraceComplex, ActorsToIgnore, DrawDebugType, OutHits, bIgnoreSelf, TraceColor, TraceHitColor, DrawTime, bDrawMassCandidateShapes, bDrawSpatialGridCells, SCENE_QUERY_STAT_NAME_ONLY(CrowdyMassCapsuleTraceMultiByProfile));
}

bool UCrowdyMassTraceBlueprintLibrary::CrowdyMassSphereOverlapActors(const UObject* WorldContextObject, const FVector SpherePos, const float SphereRadius, const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes, UClass* ActorClassFilter, const TArray<AActor*>& ActorsToIgnore, TArray<FCrowdyMassOverlapResult>& OutOverlaps)
{
	CrowdyMassTrace::FCrowdyMassOverlapShape Shape;
	Shape.Type = CrowdyMassTrace::ECrowdyMassTraceShapeType::Sphere;
	Shape.Position = SpherePos;
	Shape.Radius = SphereRadius;
	return CrowdyMassTrace::ExecuteCrowdyMassOverlap(WorldContextObject, Shape, ObjectTypes, ActorClassFilter, ActorsToIgnore, true, OutOverlaps);
}

bool UCrowdyMassTraceBlueprintLibrary::CrowdyMassSphereOverlapComponents(const UObject* WorldContextObject, const FVector SpherePos, const float SphereRadius, const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes, UClass* ComponentClassFilter, const TArray<AActor*>& ActorsToIgnore, TArray<FCrowdyMassOverlapResult>& OutOverlaps)
{
	CrowdyMassTrace::FCrowdyMassOverlapShape Shape;
	Shape.Type = CrowdyMassTrace::ECrowdyMassTraceShapeType::Sphere;
	Shape.Position = SpherePos;
	Shape.Radius = SphereRadius;
	return CrowdyMassTrace::ExecuteCrowdyMassOverlap(WorldContextObject, Shape, ObjectTypes, ComponentClassFilter, ActorsToIgnore, false, OutOverlaps);
}

bool UCrowdyMassTraceBlueprintLibrary::CrowdyMassBoxOverlapActors(const UObject* WorldContextObject, const FVector BoxPos, const FVector BoxExtent, const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes, UClass* ActorClassFilter, const TArray<AActor*>& ActorsToIgnore, TArray<FCrowdyMassOverlapResult>& OutOverlaps)
{
	return CrowdyMassBoxOverlapActorsWithOrientation(WorldContextObject, BoxPos, BoxExtent, FRotator::ZeroRotator, ObjectTypes, ActorClassFilter, ActorsToIgnore, OutOverlaps);
}

bool UCrowdyMassTraceBlueprintLibrary::CrowdyMassBoxOverlapActorsWithOrientation(const UObject* WorldContextObject, const FVector BoxPos, const FVector BoxExtent, const FRotator Orientation, const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes, UClass* ActorClassFilter, const TArray<AActor*>& ActorsToIgnore, TArray<FCrowdyMassOverlapResult>& OutOverlaps)
{
	CrowdyMassTrace::FCrowdyMassOverlapShape Shape;
	Shape.Type = CrowdyMassTrace::ECrowdyMassTraceShapeType::Box;
	Shape.Position = BoxPos;
	Shape.Extent = BoxExtent;
	Shape.Orientation = Orientation;
	return CrowdyMassTrace::ExecuteCrowdyMassOverlap(WorldContextObject, Shape, ObjectTypes, ActorClassFilter, ActorsToIgnore, true, OutOverlaps);
}

bool UCrowdyMassTraceBlueprintLibrary::CrowdyMassBoxOverlapComponents(const UObject* WorldContextObject, const FVector BoxPos, const FVector BoxExtent, const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes, UClass* ComponentClassFilter, const TArray<AActor*>& ActorsToIgnore, TArray<FCrowdyMassOverlapResult>& OutOverlaps)
{
	return CrowdyMassBoxOverlapComponentsWithOrientation(WorldContextObject, BoxPos, BoxExtent, FRotator::ZeroRotator, ObjectTypes, ComponentClassFilter, ActorsToIgnore, OutOverlaps);
}

bool UCrowdyMassTraceBlueprintLibrary::CrowdyMassBoxOverlapComponentsWithOrientation(const UObject* WorldContextObject, const FVector BoxPos, const FVector BoxExtent, const FRotator Orientation, const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes, UClass* ComponentClassFilter, const TArray<AActor*>& ActorsToIgnore, TArray<FCrowdyMassOverlapResult>& OutOverlaps)
{
	CrowdyMassTrace::FCrowdyMassOverlapShape Shape;
	Shape.Type = CrowdyMassTrace::ECrowdyMassTraceShapeType::Box;
	Shape.Position = BoxPos;
	Shape.Extent = BoxExtent;
	Shape.Orientation = Orientation;
	return CrowdyMassTrace::ExecuteCrowdyMassOverlap(WorldContextObject, Shape, ObjectTypes, ComponentClassFilter, ActorsToIgnore, false, OutOverlaps);
}

bool UCrowdyMassTraceBlueprintLibrary::CrowdyMassCapsuleOverlapActors(const UObject* WorldContextObject, const FVector CapsulePos, const float Radius, const float HalfHeight, const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes, UClass* ActorClassFilter, const TArray<AActor*>& ActorsToIgnore, TArray<FCrowdyMassOverlapResult>& OutOverlaps)
{
	return CrowdyMassCapsuleOverlapActorsWithOrientation(WorldContextObject, CapsulePos, Radius, HalfHeight, FRotator::ZeroRotator, ObjectTypes, ActorClassFilter, ActorsToIgnore, OutOverlaps);
}

bool UCrowdyMassTraceBlueprintLibrary::CrowdyMassCapsuleOverlapActorsWithOrientation(const UObject* WorldContextObject, const FVector CapsulePos, const float Radius, const float HalfHeight, const FRotator Orientation, const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes, UClass* ActorClassFilter, const TArray<AActor*>& ActorsToIgnore, TArray<FCrowdyMassOverlapResult>& OutOverlaps)
{
	CrowdyMassTrace::FCrowdyMassOverlapShape Shape;
	Shape.Type = CrowdyMassTrace::ECrowdyMassTraceShapeType::Capsule;
	Shape.Position = CapsulePos;
	Shape.Radius = Radius;
	Shape.HalfHeight = HalfHeight;
	Shape.Orientation = Orientation;
	return CrowdyMassTrace::ExecuteCrowdyMassOverlap(WorldContextObject, Shape, ObjectTypes, ActorClassFilter, ActorsToIgnore, true, OutOverlaps);
}

bool UCrowdyMassTraceBlueprintLibrary::CrowdyMassCapsuleOverlapComponents(const UObject* WorldContextObject, const FVector CapsulePos, const float Radius, const float HalfHeight, const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes, UClass* ComponentClassFilter, const TArray<AActor*>& ActorsToIgnore, TArray<FCrowdyMassOverlapResult>& OutOverlaps)
{
	return CrowdyMassCapsuleOverlapComponentsWithOrientation(WorldContextObject, CapsulePos, Radius, HalfHeight, FRotator::ZeroRotator, ObjectTypes, ComponentClassFilter, ActorsToIgnore, OutOverlaps);
}

bool UCrowdyMassTraceBlueprintLibrary::CrowdyMassCapsuleOverlapComponentsWithOrientation(const UObject* WorldContextObject, const FVector CapsulePos, const float Radius, const float HalfHeight, const FRotator Orientation, const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes, UClass* ComponentClassFilter, const TArray<AActor*>& ActorsToIgnore, TArray<FCrowdyMassOverlapResult>& OutOverlaps)
{
	CrowdyMassTrace::FCrowdyMassOverlapShape Shape;
	Shape.Type = CrowdyMassTrace::ECrowdyMassTraceShapeType::Capsule;
	Shape.Position = CapsulePos;
	Shape.Radius = Radius;
	Shape.HalfHeight = HalfHeight;
	Shape.Orientation = Orientation;
	return CrowdyMassTrace::ExecuteCrowdyMassOverlap(WorldContextObject, Shape, ObjectTypes, ComponentClassFilter, ActorsToIgnore, false, OutOverlaps);
}

UCrowdyMassEntity* UCrowdyMassTraceBlueprintLibrary::GetEntity(const FCrowdyMassTraceHit& Hit)
{
	if (!Hit.bHitCrowdyEntity || !Hit.UUID.IsValid())
	{
		return nullptr;
	}

	// The hit component (or, failing that, the hit actor) is the rendering backend's proxy for
	// the entity's world; there is no other way to reach a UWorld from a bare trace hit.
	const UObject* ContextObject = Hit.Component ? static_cast<const UObject*>(Hit.Component) : static_cast<const UObject*>(Hit.Actor);
	const UWorld* World = ContextObject ? ContextObject->GetWorld() : nullptr;
	if (!World)
	{
		return nullptr;
	}

	UCrowdyMassEntitySubsystem* Subsystem = World->GetSubsystem<UCrowdyMassEntitySubsystem>();
	if (!Subsystem)
	{
		return nullptr;
	}

	return Subsystem->GetOrCreateAvatar(Hit.UUID, UCrowdyMassEntity::StaticClass());
}

void UCrowdyMassTraceBlueprintLibrary::SetHitZoneTable(const UObject* WorldContextObject, UCrowdyMassHitZoneTable* HitZoneTable)
{
	const UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
	UCrowdyMassEntitySubsystem* CrowdySubsystem = World ? World->GetSubsystem<UCrowdyMassEntitySubsystem>() : nullptr;
	if (!CrowdySubsystem)
	{
		static TSet<FObjectKey> WorldsToldNoHitZones;
		bool bAlreadyTold = false;
		WorldsToldNoHitZones.Add(FObjectKey(World), &bAlreadyTold);
		UE_CLOG(World && !bAlreadyTold, LogCrowdyMass, Warning,
			TEXT("[CrowdyMassTrace] Set Crowdy Mass Hit Zone Table did nothing in world '%s': hit zones need a map whose profile selects the Mass backend, so hits here resolve to the default zone."),
			*World->GetName());
		return;
	}

	// Rebuilt here, once, rather than lazily on first use: setting the table is already the rare,
	// deliberate call, so the trace path never has to check "has this changed" on the frequent one.
	CrowdySubsystem->SetHitZoneTable(HitZoneTable);
}

bool UCrowdyMassTraceBlueprintLibrary::SetEntityTraceCollisionEnabled(const UObject* WorldContextObject, const FMassEntityHandle Entity, const bool bEnabled)
{
	const UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
	UMassEntitySubsystem* MassEntitySubsystem = World ? World->GetSubsystem<UMassEntitySubsystem>() : nullptr;
	if (!MassEntitySubsystem)
	{
		return false;
	}

	FMassEntityManager& EntityManager = MassEntitySubsystem->GetMutableEntityManager();
	if (!EntityManager.IsEntityValid(Entity))
	{
		return false;
	}

	FCrowdyMassTraceCollisionFragment* TraceCollisionFragment = EntityManager.GetFragmentDataPtr<FCrowdyMassTraceCollisionFragment>(Entity);
	if (!TraceCollisionFragment)
	{
		// The same silence the trace path used to keep. There is nothing to toggle because the entity was
		// built without the Crowdy Mass Trace Collision trait, and a bare false here reads identically to
		// "the entity is gone", so the subsystem says which one it is (once per world).
		if (UCrowdyMassEntitySubsystem* CrowdySubsystem = World->GetSubsystem<UCrowdyMassEntitySubsystem>())
		{
			CrowdySubsystem->ReportMissingTraceCollisionFragment();
		}
		return false;
	}

	TraceCollisionFragment->bEnableTraceCollision = bEnabled;
	return true;
}
