#pragma once

#include "CoreMinimal.h"
#include "CrowdyExecTestTypes.h"
#include "CrowdyServerObjectTypes.h"
#include "Engine/EngineTypes.h"
#include "GameFramework/Actor.h"
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

/** What the test boss's Server Messages carry: as a struct, and as the List of the same two values. */
USTRUCT()
struct FCrowdyServerObjectTestMessagePayload
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Amount = 0;

	UPROPERTY()
	FString Source;
};

/** A row of a record's grid. */
USTRUCT()
struct FCrowdyServerObjectTestRecordRow
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<int32> Cells;
};

/** One entity's record, as the server's records test type keeps it: Health, Label and Rows for everyone, Gold for its owner, Ammo for its team. */
USTRUCT()
struct FCrowdyServerObjectTestRecord
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Health = 100;

	UPROPERTY()
	int32 Gold = 0;

	UPROPERTY()
	int32 Ammo = 0;

	UPROPERTY()
	int32 Secret = 0;

	UPROPERTY()
	FString Label;

	UPROPERTY()
	TArray<FCrowdyServerObjectTestRecordRow> Rows;
};

/** A record nested to the 16 levels a bake allows: Deep's innermost vectors sit at level 16. */
USTRUCT()
struct FCrowdyServerObjectTestDeepRecord
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Health = 100;

	UPROPERTY()
	FCrowdyExecTestDepth7 Deep;
};

/** Stands in for an actor or widget that holds a Server Object. */
UCLASS(Transient)
class UCrowdyServerObjectTestOwner : public UObject
{
	GENERATED_BODY()
};

/** An actor class only the missing-definition warning test uses, so no earlier test has warned about it. */
UCLASS(Transient, NotPlaceable)
class ACrowdyServerObjectTestCrowdActor : public AActor
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
	TArray<FName> FailedCodes;
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

	/** Runs after HandleStatus records the status. */
	TFunction<void()> OnStatus;

	UFUNCTION()
	void HandleStatus(UCrowdyServerObject* Object, ECrowdyServerObjectStatus NewStatus)
	{
		StatusEvents.Add(NewStatus);
		StatusHadObject.Add(Object != nullptr);
		if (OnStatus)
		{
			OnStatus();
		}
	}

	UFUNCTION()
	void HandleSuccess(const FInstancedStruct& Outputs, ECrowdyServerCallOutcome Outcome, const FString& Reason, bool bRetryable, FName Code)
	{
		SuccessOutputs.Add(Outputs);
		SuccessOutcomes.Add(Outcome);
	}

	UFUNCTION()
	void HandleFailed(const FInstancedStruct& Outputs, ECrowdyServerCallOutcome Outcome, const FString& Reason, bool bRetryable, FName Code)
	{
		FailedOutcomes.Add(Outcome);
		FailedReasons.Add(Reason);
		FailedRetryable.Add(bRetryable);
		FailedCodes.Add(Code);
	}

	TArray<FInstancedStruct> MessagePayloads;
	TArray<FInstancedStruct> SecondMessagePayloads;
	TArray<FName> EventMessages;
	TArray<FInstancedStruct> EventPayloads;

	/** A function a Server Message node's event stands in for: the payload is its one parameter. */
	UFUNCTION()
	void HandleMessagePayload(const FInstancedStruct& Payload)
	{
		MessagePayloads.Add(Payload);
		if (OnMessage)
		{
			OnMessage();
		}
	}

	/** Runs after HandleMessagePayload records the payload, as more of a handler's graph would. */
	TFunction<void()> OnMessage;

	UFUNCTION()
	void HandleMessagePayloadSecond(const FInstancedStruct& Payload)
	{
		SecondMessagePayloads.Add(Payload);
	}

	/** The Blueprint event Watch Message takes. */
	UFUNCTION()
	void HandleMessageEvent(UCrowdyServerObject* Object, FName Message, const FInstancedStruct& Payload)
	{
		EventMessages.Add(Message);
		EventPayloads.Add(Payload);
	}
};

/** Records each time the game session announces that the player's access ended. */
UCLASS(Transient)
class UCrowdyServerObjectTestAccessSpy : public UObject
{
	GENERATED_BODY()

public:
	TArray<FString> Reasons;

	UFUNCTION()
	void HandleAccessEnded(const FString& Reason, const FString& BlockedUntil)
	{
		Reasons.Add(Reason);
	}
};

/** Records each pin of Find Match that fires, as a Blueprint graph bound to them would. */
UCLASS(Transient)
class UCrowdyKitMatchTestListener : public UObject
{
	GENERATED_BODY()

public:
	TArray<FString> Pins;

	UFUNCTION()
	void HandleQueued(const TArray<int64>& Players, const TArray<int64>& Accepted, const FString& SessionId, FName Code, const FString& Reason)
	{
		Pins.Add(TEXT("Queued"));
	}

	UFUNCTION()
	void HandleProposed(const TArray<int64>& Players, const TArray<int64>& Accepted, const FString& SessionId, FName Code, const FString& Reason)
	{
		Pins.Add(FString::Printf(TEXT("Proposed %d/%d"), Accepted.Num(), Players.Num()));
	}

	UFUNCTION()
	void HandleMatched(const TArray<int64>& Players, const TArray<int64>& Accepted, const FString& SessionId, FName Code, const FString& Reason)
	{
		Pins.Add(TEXT("Matched ") + SessionId);
	}

	UFUNCTION()
	void HandleFailed(const TArray<int64>& Players, const TArray<int64>& Accepted, const FString& SessionId, FName Code, const FString& Reason)
	{
		Pins.Add(TEXT("Failed ") + Code.ToString());
	}
};
