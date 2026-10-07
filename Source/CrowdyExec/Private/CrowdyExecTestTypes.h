#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/SoftObjectPtr.h"
#include "CrowdyExecTestTypes.generated.h"

UENUM()
enum class ECrowdyExecTestPhase : uint8
{
	Calm,
	Enraged,
	Final
};

USTRUCT()
struct FCrowdyExecTestScalars
{
	GENERATED_BODY()

	UPROPERTY()
	bool bDefeated = false;

	UPROPERTY()
	int32 Health = 0;

	UPROPERTY()
	int32 Level = 0;

	UPROPERTY()
	int64 Big = 0;

	UPROPERTY()
	float Speed = 0.f;

	UPROPERTY()
	double Precise = 0.0;

	UPROPERTY()
	FString Name;

	UPROPERTY()
	ECrowdyExecTestPhase Phase = ECrowdyExecTestPhase::Calm;
};

USTRUCT()
struct FCrowdyExecTestContainers
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<int32> Hits;

	UPROPERTY()
	TSet<FString> Tags;

	UPROPERTY()
	TMap<FString, int32> DamageByPlayer;

	UPROPERTY()
	TArray<uint8> Bytes;
};

USTRUCT()
struct FCrowdyExecTestEngine
{
	GENERATED_BODY()

	UPROPERTY()
	FVector Arena = FVector::ZeroVector;

	UPROPERTY()
	FDateTime When;

	UPROPERTY()
	FGuid Id;

	UPROPERTY()
	TOptional<int32> Maybe;
};

/** The state struct of the watched-values fixtures. Health's default is not zero, so a default is told apart from a decode. */
USTRUCT()
struct FCrowdyExecTestBossState
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Health = 4000;

	UPROPERTY()
	bool bDefeated = false;

	UPROPERTY()
	ECrowdyExecTestPhase Phase = ECrowdyExecTestPhase::Calm;

	UPROPERTY()
	int32 Secret = 0;
};

USTRUCT()
struct FCrowdyExecTestHit
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Damage = 0;

	UPROPERTY()
	FName Source;
};

/** A field named like a Rust keyword, which the generated server code must write raw. */
USTRUCT()
struct FCrowdyExecTestRaw
{
	GENERATED_BODY()

	UPROPERTY()
	FString type;
};

/** A default that differs each time the struct is made, which generated server code cannot write down. */
USTRUCT()
struct FCrowdyExecTestRandomDefault
{
	GENERATED_BODY()

	UPROPERTY()
	FGuid Id = FGuid::NewGuid();
};

USTRUCT()
struct FCrowdyExecTestHeavyHit : public FCrowdyExecTestHit
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Stagger = 0;
};

// A struct cannot contain itself, so nesting depth needs a chain: each link is a struct and an array, two levels.
USTRUCT()
struct FCrowdyExecTestDepth1
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FVector> Items;
};

USTRUCT()
struct FCrowdyExecTestDepth2
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FCrowdyExecTestDepth1> Items;
};

USTRUCT()
struct FCrowdyExecTestDepth3
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FCrowdyExecTestDepth2> Items;
};

USTRUCT()
struct FCrowdyExecTestDepth4
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FCrowdyExecTestDepth3> Items;
};

USTRUCT()
struct FCrowdyExecTestDepth5
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FCrowdyExecTestDepth4> Items;
};

USTRUCT()
struct FCrowdyExecTestDepth6
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FCrowdyExecTestDepth5> Items;
};

USTRUCT()
struct FCrowdyExecTestDepth7
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FCrowdyExecTestDepth6> Items;
};

USTRUCT()
struct FCrowdyExecTestDepth8
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FCrowdyExecTestDepth7> Items;
};

USTRUCT()
struct FCrowdyExecTestExtras
{
	GENERATED_BODY()

	UPROPERTY()
	uint8 Small = 0;

	UPROPERTY()
	int8 Tiny = 0;

	UPROPERTY()
	uint64 Huge = 0;

	UPROPERTY()
	FRotator Rotation = FRotator::ZeroRotator;

	UPROPERTY()
	FQuat Orientation = FQuat::Identity;

	UPROPERTY()
	FLinearColor Tint = FLinearColor::White;

	UPROPERTY()
	FColor Color = FColor::White;

	UPROPERTY()
	FIntPoint Cell = FIntPoint::ZeroValue;

	UPROPERTY()
	FIntVector Block = FIntVector::ZeroValue;

	UPROPERTY()
	FVector2D Offset = FVector2D::ZeroVector;

	UPROPERTY()
	FTimespan Cooldown;

	UPROPERTY()
	FGameplayTag Tag;

	UPROPERTY()
	TSoftObjectPtr<UObject> Mesh;

	UPROPERTY()
	TSoftClassPtr<UObject> Class;

	UPROPERTY()
	FSoftObjectPath Path;

	UPROPERTY()
	TMap<int32, FString> ByIndex;

	UPROPERTY()
	TSet<ECrowdyExecTestPhase> Phases;

	UPROPERTY()
	TArray<FCrowdyExecTestHit> Attacks;

	UPROPERTY()
	TArray<FString> Notes;
};

/** One variable of every kind a Server Object carries. */
USTRUCT()
struct FCrowdyExecTestEveryKind
{
	GENERATED_BODY()

	UPROPERTY()
	int8 Int8Value = 0;

	UPROPERTY()
	int16 Int16Value = 0;

	UPROPERTY()
	int32 Int32Value = 0;

	UPROPERTY()
	int64 Int64Value = 0;

	UPROPERTY()
	uint8 UInt8Value = 0;

	UPROPERTY()
	uint16 UInt16Value = 0;

	UPROPERTY()
	uint32 UInt32Value = 0;

	UPROPERTY()
	uint64 UInt64Value = 0;

	UPROPERTY()
	float FloatValue = 0.f;

	UPROPERTY()
	double DoubleValue = 0.0;

	UPROPERTY()
	bool bBoolValue = false;

	UPROPERTY()
	FString StringValue;

	UPROPERTY()
	FName NameValue;

	UPROPERTY()
	ECrowdyExecTestPhase EnumValue = ECrowdyExecTestPhase::Calm;

	UPROPERTY()
	FCrowdyExecTestHit StructValue;

	UPROPERTY()
	TArray<int32> ArrayValue;

	UPROPERTY()
	TSet<FString> SetValue;

	UPROPERTY()
	TMap<FString, int32> MapValue;

	UPROPERTY()
	TArray<int16> Int16Array;

	UPROPERTY()
	TArray<uint64> UInt64Array;

	UPROPERTY()
	TSet<uint16> UInt16Set;

	UPROPERTY()
	TMap<uint32, int8> NarrowMap;

	UPROPERTY()
	TArray<float> FloatArray;

	UPROPERTY()
	TMap<ECrowdyExecTestPhase, float> PhaseSpeeds;

	UPROPERTY()
	TOptional<int32> OptionalValue;

	UPROPERTY()
	TOptional<int32> EmptyOptional;

	UPROPERTY()
	FVector VectorValue = FVector::ZeroVector;

	UPROPERTY()
	FVector2D Vector2DValue = FVector2D::ZeroVector;

	UPROPERTY()
	FRotator RotatorValue = FRotator::ZeroRotator;

	UPROPERTY()
	FQuat QuatValue = FQuat::Identity;

	UPROPERTY()
	FIntPoint IntPointValue = FIntPoint::ZeroValue;

	UPROPERTY()
	FIntVector IntVectorValue = FIntVector::ZeroValue;

	UPROPERTY()
	FLinearColor LinearColorValue = FLinearColor::White;

	UPROPERTY()
	FColor ColorValue = FColor::White;

	UPROPERTY()
	FDateTime DateTimeValue;

	UPROPERTY()
	FTimespan TimespanValue;

	UPROPERTY()
	FGuid GuidValue;

	UPROPERTY()
	FGameplayTag TagValue;

	UPROPERTY()
	TSoftObjectPtr<UObject> SoftObjectValue;

	UPROPERTY()
	TSoftClassPtr<UObject> SoftClassValue;
};

/** Containers keyed by enums and names, for unknown enum values, duplicate keys and FName None. */
USTRUCT()
struct FCrowdyExecTestKeys
{
	GENERATED_BODY()

	UPROPERTY()
	TMap<ECrowdyExecTestPhase, FCrowdyExecTestHit> HitByPhase;

	UPROPERTY()
	TArray<ECrowdyExecTestPhase> PhaseList;

	UPROPERTY()
	TOptional<ECrowdyExecTestPhase> MaybePhase;

	UPROPERTY()
	TMap<FName, int32> ByName;

	UPROPERTY(Transient)
	int32 Cache = 0;
};

USTRUCT()
struct FCrowdyExecTestRefused
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<UObject> Target;

	UPROPERTY()
	FText Label;

	UPROPERTY()
	FInstancedStruct Payload;

	UPROPERTY()
	int32 Fixed[4];

	UPROPERTY()
	TWeakObjectPtr<UObject> Weak;

	UPROPERTY()
	TArray<TObjectPtr<UObject>> Objects;

	UPROPERTY()
	TSet<float> Ratios;

	UPROPERTY()
	FGameplayTagContainer TagSet;

	UPROPERTY()
	int32 Fine = 0;
};
