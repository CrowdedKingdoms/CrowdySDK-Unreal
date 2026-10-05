#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/SoftObjectPtr.h"
#include "CrowdyExecNodesTestTypes.generated.h"

UENUM(BlueprintType)
enum class ECrowdyExecNodesTestPhase : uint8
{
	Calm,
	Enraged
};

UENUM()
enum class ECrowdyExecNodesTestHiddenEnum : uint8
{
	First,
	Second
};

// Internal use keeps the test types out of the editor's type pickers while nodes can still give them pins.
USTRUCT(BlueprintType, meta = (BlueprintInternalUseOnly = "true"))
struct FCrowdyExecNodesTestHit
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Damage = 0;

	UPROPERTY()
	FName Source;
};

USTRUCT()
struct FCrowdyExecNodesTestHiddenStruct
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Value = 0;
};

/** One field of every kind a Server Object value can be. */
USTRUCT(BlueprintType, meta = (BlueprintInternalUseOnly = "true"))
struct FCrowdyExecNodesTestEveryType
{
	GENERATED_BODY()

	UPROPERTY()
	bool bFlag = false;

	UPROPERTY()
	uint8 Byte = 0;

	UPROPERTY()
	int8 Tiny = 0;

	UPROPERTY()
	int16 Short = 0;

	UPROPERTY()
	uint16 UShort = 0;

	UPROPERTY()
	int32 Int = 7;

	UPROPERTY()
	uint32 UInt = 0;

	UPROPERTY()
	int64 Big = 0;

	UPROPERTY()
	uint64 UBig = 0;

	UPROPERTY()
	float Float = 0.f;

	UPROPERTY()
	double Double = 0.0;

	UPROPERTY()
	FString String;

	UPROPERTY()
	FName Name;

	UPROPERTY()
	ECrowdyExecNodesTestPhase Phase = ECrowdyExecNodesTestPhase::Calm;

	UPROPERTY()
	FCrowdyExecNodesTestHit Hit;

	UPROPERTY()
	FVector Vector = FVector::ZeroVector;

	UPROPERTY()
	FVector2D Vector2D = FVector2D::ZeroVector;

	UPROPERTY()
	FRotator Rotator = FRotator::ZeroRotator;

	UPROPERTY()
	FQuat Quat = FQuat::Identity;

	UPROPERTY()
	FIntPoint IntPoint = FIntPoint::ZeroValue;

	UPROPERTY()
	FIntVector IntVector = FIntVector::ZeroValue;

	UPROPERTY()
	FLinearColor LinearColor = FLinearColor::White;

	UPROPERTY()
	FColor Color = FColor::White;

	UPROPERTY()
	FDateTime DateTime;

	UPROPERTY()
	FTimespan Timespan;

	UPROPERTY()
	FGuid Guid;

	UPROPERTY()
	FGameplayTag Tag;

	UPROPERTY()
	TSoftObjectPtr<UObject> SoftObject;

	UPROPERTY()
	TSoftClassPtr<UObject> SoftClass;

	UPROPERTY()
	FSoftObjectPath Path;

	UPROPERTY()
	TOptional<int32> MaybeInt;

	UPROPERTY()
	TOptional<int16> MaybeShort;

	UPROPERTY()
	TOptional<FString> MaybeString;

	UPROPERTY()
	TArray<int32> Ints;

	UPROPERTY()
	TArray<int16> Shorts;

	UPROPERTY()
	TSet<FName> Names;

	UPROPERTY()
	TSet<uint16> UShorts;

	UPROPERTY()
	TMap<FString, int32> Scores;

	UPROPERTY()
	TMap<uint32, int8> TinyByUInt;

	UPROPERTY()
	TArray<FCrowdyExecNodesTestHit> Hits;

	UPROPERTY(Transient)
	int32 Cache = 0;
};

/** Values Blueprints cannot hold, beside one they can. */
USTRUCT()
struct FCrowdyExecNodesTestRefused
{
	GENERATED_BODY()

	UPROPERTY()
	FCrowdyExecNodesTestHiddenStruct Hidden;

	UPROPERTY()
	ECrowdyExecNodesTestHiddenEnum HiddenEnum = ECrowdyExecNodesTestHiddenEnum::First;

	UPROPERTY()
	int32 Fine = 0;
};
