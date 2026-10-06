#pragma once

#include "CoreMinimal.h"
#include "CrowdyExecTestTypes.h"
#include "CrowdyServerObjectTypes.h"
#include "Engine/EngineTypes.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Object.h"
#include "CrowdyServerObjectTestTypes.generated.h"

class UCrowdyServerObject;

USTRUCT()
struct FCrowdyServerObjectTestAttackParams
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Damage = 0;

	UPROPERTY()
	FString Weapon;
};

USTRUCT()
struct FCrowdyServerObjectTestAttackReply
{
	GENERATED_BODY()

	UPROPERTY()
	int32 HealthLeft = 0;

	UPROPERTY()
	bool bKilled = false;
};

/** Stands in for an actor or widget that holds a Server Object. */
UCLASS(Transient)
class UCrowdyServerObjectTestOwner : public UObject
{
	GENERATED_BODY()
};

/** The Blueprint pins a graph hands to Set Server Value and Get Server Value. */
USTRUCT()
struct FCrowdyServerObjectTestPins
{
	GENERATED_BODY()

	UPROPERTY()
	int64 Big = 0;

	UPROPERTY()
	FString Label;

	UPROPERTY()
	int32 Count = 0;

	UPROPERTY()
	bool bFlag = false;

	UPROPERTY()
	ECrowdyExecTestPhase Phase = ECrowdyExecTestPhase::Calm;
};

/** The pins a typed Blueprint node gives FCrowdyExecTestEveryKind's variables: C++-only integers widened, an optional as its value. */
USTRUCT()
struct FCrowdyServerObjectTestEveryKindPins
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Int8Value = 0;

	UPROPERTY()
	int32 Int16Value = 0;

	UPROPERTY()
	int32 Int32Value = 0;

	UPROPERTY()
	int64 Int64Value = 0;

	UPROPERTY()
	uint8 UInt8Value = 0;

	UPROPERTY()
	int32 UInt16Value = 0;

	UPROPERTY()
	int64 UInt32Value = 0;

	UPROPERTY()
	int64 UInt64Value = 0;

	UPROPERTY()
	double FloatValue = 0.0;

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
	TArray<int32> Int16Array;

	UPROPERTY()
	TArray<int64> UInt64Array;

	UPROPERTY()
	TSet<int32> UInt16Set;

	UPROPERTY()
	TMap<int64, int32> NarrowMap;

	UPROPERTY()
	TArray<double> FloatArray;

	UPROPERTY()
	TMap<ECrowdyExecTestPhase, double> PhaseSpeeds;

	UPROPERTY()
	int32 OptionalValue = 0;

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

/** Every integer kind, for widening into and out of Blueprint's int32 and int64. */
USTRUCT()
struct FCrowdyServerObjectTestIntegers
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
	TOptional<int32> OptionalValue;

	UPROPERTY()
	TOptional<int64> OptionalBig;

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
	TArray<ECrowdyExecTestPhase> PhaseArray;

	UPROPERTY()
	TMap<ECrowdyExecTestPhase, float> PhaseSpeeds;
};

/** An enum held as a byte, as a Blueprint struct's enum field is. */
USTRUCT()
struct FCrowdyServerObjectTestByteEnum
{
	GENERATED_BODY()

	UPROPERTY()
	TEnumAsByte<ECollisionChannel> Channel = ECC_WorldStatic;

	UPROPERTY()
	TArray<TEnumAsByte<ECollisionChannel>> Channels;
};

/** Counts its constructions, to tell which code set up a function's local variable. */
USTRUCT()
struct FCrowdyServerObjectTestCounted
{
	GENERATED_BODY()

	FCrowdyServerObjectTestCounted() { ++Constructed; }

	static inline int32 Constructed = 0;

	UPROPERTY()
	int32 Value = 0;
};

/** A value that never travels beside one that does. */
USTRUCT()
struct FCrowdyServerObjectTestTransient
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Kept = 0;

	UPROPERTY(Transient)
	int32 Scratch = 0;
};

/** Records every Blueprint event bound to it, as a Blueprint graph would receive them. */
UCLASS(Transient)
class UCrowdyServerObjectTestListener : public UObject
{
	GENERATED_BODY()

public:
	TArray<TArray<FString>> VariableEvents;
	TArray<ECrowdyServerObjectStatus> StatusEvents;
	/** Parallel to StatusEvents: whether each named an object. */
	TArray<bool> StatusHadObject;
	TArray<FInstancedStruct> SuccessOutputs;
	TArray<ECrowdyServerCallOutcome> SuccessOutcomes;
	TArray<ECrowdyServerCallOutcome> FailedOutcomes;
	TArray<FString> FailedReasons;
	TArray<bool> FailedRetryable;
	TArray<int32> IntValues;
	TArray<bool> HasValues;
	TArray<FCrowdyExecTestHit> HitValues;

	/** Runs after HandleInt records a value, as more of a handler's graph would. */
	TFunction<void()> OnInt;

	UFUNCTION()
	void HandleInt(int32 Value)
	{
		IntValues.Add(Value);
		if (OnInt)
		{
			OnInt();
		}
	}

	TArray<int32> SecondIntValues;

	/** A second handler on the same holder, as a second On Changed node in one graph is. */
	UFUNCTION()
	void HandleIntSecond(int32 Value)
	{
		SecondIntValues.Add(Value);
	}

	UFUNCTION()
	void HandleOptionalInt(int32 Value, bool bHasValue)
	{
		IntValues.Add(Value);
		HasValues.Add(bHasValue);
	}

	UFUNCTION()
	void HandleHit(const FCrowdyExecTestHit& Value)
	{
		HitValues.Add(Value);
	}

	/** Runs after HandleVariables records the names. */
	TFunction<void()> OnVariables;

	UFUNCTION()
	void HandleVariables(UCrowdyServerObject* Object, const TArray<FString>& Changed)
	{
		VariableEvents.Add(Changed);
		if (OnVariables)
		{
			OnVariables();
		}
	}

	UFUNCTION()
	void HandleStatus(UCrowdyServerObject* Object, ECrowdyServerObjectStatus NewStatus)
	{
		StatusEvents.Add(NewStatus);
		StatusHadObject.Add(Object != nullptr);
	}

	UFUNCTION()
	void HandleSuccess(const FInstancedStruct& Outputs, ECrowdyServerCallOutcome Outcome, const FString& Reason, bool bRetryable)
	{
		SuccessOutputs.Add(Outputs);
		SuccessOutcomes.Add(Outcome);
	}

	UFUNCTION()
	void HandleFailed(const FInstancedStruct& Outputs, ECrowdyServerCallOutcome Outcome, const FString& Reason, bool bRetryable)
	{
		FailedOutcomes.Add(Outcome);
		FailedReasons.Add(Reason);
		FailedRetryable.Add(bRetryable);
	}
};
