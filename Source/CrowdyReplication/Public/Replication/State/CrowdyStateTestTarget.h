#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/NetSerialization.h" // FVector_NetQuantize
#include "GameFramework/Actor.h"
#include "Replication/Executor/ActorUpdateExecutor.h"
#include "CrowdyStateTestTarget.generated.h"

class UCrowdyEntityComponent;

// A modern enum-class property reflects as an FEnumProperty, one of the accepted CrowdyState leaf
// kinds. Used by the layout builder tests to prove enums are collected.
UENUM()
enum class ECrowdyStateTestEnum : uint8
{
	Alpha,
	Beta,
	Gamma
};

/**
 * Fixture for the CrowdyState layout builder automation tests, mirroring UCrowdyRpcTestTarget. It
 * carries a meta=(CrowdyState) property of every supported leaf and struct type, plus an OwnerOnly,
 * a ManualDirty, and an OnRep example, so a test can build a layout and assert the collected set,
 * ordering, per-property flags, and stable IDs.
 *
 * The rejected object-ref and container cases live on their own fixtures (UCrowdyStateObjectRejectTarget
 * / UCrowdyStateContainerRejectTarget) rather than here: building this target must not emit discovery
 * errors, so the accepted-path tests stay clean, while each reject test whitelists exactly the one
 * error it exercises.
 */
UCLASS()
class UCrowdyStateTestTarget : public UObject
{
	GENERATED_BODY()

public:

	UPROPERTY(meta = (CrowdyState))
	int32 RepInt = 0;

	UPROPERTY(meta = (CrowdyState))
	int64 RepBigInt = 0;

	UPROPERTY(meta = (CrowdyState))
	float RepFloat = 0.f;

	UPROPERTY(meta = (CrowdyState))
	double RepDouble = 0.0;

	UPROPERTY(meta = (CrowdyState))
	bool bRepFlag = false;

	UPROPERTY(meta = (CrowdyState))
	uint8 RepByte = 0;

	UPROPERTY(meta = (CrowdyState))
	ECrowdyStateTestEnum RepEnum = ECrowdyStateTestEnum::Alpha;

	UPROPERTY(meta = (CrowdyState))
	FName RepName = NAME_None;

	UPROPERTY(meta = (CrowdyState))
	FString RepString;

	UPROPERTY(meta = (CrowdyState))
	FVector RepVector = FVector::ZeroVector;

	UPROPERTY(meta = (CrowdyState))
	FRotator RepRotator = FRotator::ZeroRotator;

	// Delivery scope example: owner-directed rather than spatial (Phase 5).
	UPROPERTY(meta = (CrowdyState, CrowdyOwnerOnly))
	int32 RepOwnerOnly = 0;

	// Manual-dirty example: excluded from the per-tick diff, pushed on explicit mark (Phase 5).
	UPROPERTY(meta = (CrowdyState, CrowdyManualDirty))
	int32 RepManualDirty = 0;

	// OnRep + heartbeat example: the OnRep value names a parameterless notify (Phase 4); CrowdyHeartbeat opts the
	// property into the keyframe heartbeat, so the discovery/layout/bake tests cover a true bHeartbeat round-trip.
	UPROPERTY(meta = (CrowdyState, CrowdyOnRep = "OnRep_Health", CrowdyHeartbeat))
	float RepHealth = 0.f;

	// Not marked: must never appear in a layout.
	UPROPERTY()
	int32 NotReplicated = 0;

	UFUNCTION()
	void OnRep_Health();
};

/**
 * A subclass adding exactly one more replicated property, for the layout-hash-changes-on-add test:
 * two real classes differing by one added property, not runtime mutation. Also exercises the
 * super-class inclusion  its layout is the parent's set plus RepExtra.
 */
UCLASS()
class UCrowdyStateTestTargetExtra : public UCrowdyStateTestTarget
{
	GENERATED_BODY()

public:

	UPROPERTY(meta = (CrowdyState))
	int32 RepExtra = 0;
};

/**
 * Reject fixture for the object-reference case: one accepted property and one marked object-ref
 * property. The builder must omit the object-ref (logging an error) and keep the accepted one.
 */
UCLASS()
class UCrowdyStateObjectRejectTarget : public UObject
{
	GENERATED_BODY()

public:

	UPROPERTY(meta = (CrowdyState))
	int32 RepGood = 0;

	UPROPERTY(meta = (CrowdyState))
	TObjectPtr<UObject> RepBadObject = nullptr;
};

/**
 * Reject fixture for the container case: one accepted property and one marked container property. The
 * builder must omit the container (logging an error) and keep the accepted one.
 */
UCLASS()
class UCrowdyStateContainerRejectTarget : public UObject
{
	GENERATED_BODY()

public:

	UPROPERTY(meta = (CrowdyState))
	int32 RepGood = 0;

	UPROPERTY(meta = (CrowdyState))
	TArray<int32> RepBadArray;
};

/**
 * A plain (non-net-serialized) USTRUCT that transitively contains a container, for the nested-container
 * reject test. On the vulnerable decode path FProperty::SerializeItem reads an untrusted element count for
 * the inner TArray and allocates before the short read is caught, so a CrowdyState property of this type
 * must be omitted from the layout (a remote-OOM hardening rule).
 */
USTRUCT()
struct FCrowdyStateNestedContainerStruct
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Scalar = 0;

	UPROPERTY()
	TArray<int32> Items;
};

/**
 * Reject fixture for the nested-container case: one accepted scalar and one CrowdyState USTRUCT property
 * whose struct transitively contains a container. The builder must omit the struct property (logging an
 * error) and keep the accepted scalar.
 */
UCLASS()
class UCrowdyStateNestedContainerRejectTarget : public UObject
{
	GENERATED_BODY()

public:

	UPROPERTY(meta = (CrowdyState))
	int32 RepGood = 0;

	UPROPERTY(meta = (CrowdyState))
	FCrowdyStateNestedContainerStruct RepBadStruct;
};

/**
 * Reject fixture for the fixed-size-array case: one accepted scalar and one CrowdyState C array. The builder
 * must omit the array (its positional two-argument Identical would compare only element [0], silently
 * dropping changes to later elements) and keep the accepted scalar.
 */
UCLASS()
class UCrowdyStateStaticArrayRejectTarget : public UObject
{
	GENERATED_BODY()

public:

	UPROPERTY(meta = (CrowdyState))
	int32 RepGood = 0;

	UPROPERTY(meta = (CrowdyState))
	int32 RepBadArray[4];
};

/**
 * A pair of fixtures identical but for their single property's type (same slot, same name, different
 * type), for the type-participates-in-identity test: proves a retype changes PropertyID and LayoutHash
 * so a drifted peer drops instead of misparsing by position. Two real classes rather than runtime
 * mutation, mirroring the layout-hash-changes-on-add approach.
 */
UCLASS()
class UCrowdyStateRetypeIntTarget : public UObject
{
	GENERATED_BODY()

public:

	UPROPERTY(meta = (CrowdyState))
	int32 RepValue = 0;
};

UCLASS()
class UCrowdyStateRetypeInt64Target : public UObject
{
	GENERATED_BODY()

public:

	UPROPERTY(meta = (CrowdyState))
	int64 RepValue = 0;
};

/**
 * Codec round-trip fixture: one meta=(CrowdyState) property of each supported kind, with distinct types
 * so a full-keyframe round-trip exercises every value branch  numerics, bool, byte, enum, name, string,
 * plain (exact) structs, and one net-serialized (quantized) struct. New for Phase 2; deliberately NOT
 * UCrowdyStateTestTarget, whose exact property set and order the Phase 0/1 tests hard-code.
 */
UCLASS()
class UCrowdyStateCodecTarget : public UObject
{
	GENERATED_BODY()

public:

	UPROPERTY(meta = (CrowdyState))
	int32 RepInt = 0;

	UPROPERTY(meta = (CrowdyState))
	int64 RepBigInt = 0;

	UPROPERTY(meta = (CrowdyState))
	float RepFloat = 0.f;

	UPROPERTY(meta = (CrowdyState))
	double RepDouble = 0.0;

	UPROPERTY(meta = (CrowdyState))
	bool bRepFlag = false;

	UPROPERTY(meta = (CrowdyState))
	uint8 RepByte = 0;

	UPROPERTY(meta = (CrowdyState))
	ECrowdyStateTestEnum RepEnum = ECrowdyStateTestEnum::Alpha;

	UPROPERTY(meta = (CrowdyState))
	FName RepName = NAME_None;

	UPROPERTY(meta = (CrowdyState))
	FString RepString;

	// FVector has no native net serializer, so it rides SerializeItem and round-trips exactly  the
	// correctness floor the quantized cases are measured against.
	UPROPERTY(meta = (CrowdyState))
	FVector RepVector = FVector::ZeroVector;

	// FRotator DOES declare a native net serializer (SerializeCompressedShort, a uint16 per axis), so like
	// FVector_NetQuantize it rides the quantized NetSerializeItem path and round-trips within the
	// compression step (~0.0055 deg), NOT exactly: a CrowdyState rotator quantizes on the wire for free.
	UPROPERTY(meta = (CrowdyState))
	FRotator RepRotator = FRotator::ZeroRotator;

	// FVector_NetQuantize declares STRUCT_NetSerializeNative, so it rides the quantized (length-prefixed
	// NetSerializeItem) branch and round-trips within quantization tolerance, not exactly.
	UPROPERTY(meta = (CrowdyState))
	FVector_NetQuantize RepQuantized = FVector_NetQuantize(0.0, 0.0, 0.0);
};

/**
 * Selector fixture: 64 replicated int32 properties, so the smaller-of selector choice can be observed at
 * the extremes one dirty bit favours the index-list (mode 1), all 64 favour the bitmask (mode 0).
 */
UCLASS()
class UCrowdyStateWideTarget : public UObject
{
	GENERATED_BODY()

public:

	UPROPERTY(meta = (CrowdyState)) int32 Rep00 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep01 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep02 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep03 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep04 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep05 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep06 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep07 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep08 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep09 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep10 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep11 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep12 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep13 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep14 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep15 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep16 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep17 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep18 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep19 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep20 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep21 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep22 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep23 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep24 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep25 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep26 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep27 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep28 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep29 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep30 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep31 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep32 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep33 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep34 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep35 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep36 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep37 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep38 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep39 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep40 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep41 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep42 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep43 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep44 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep45 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep46 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep47 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep48 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep49 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep50 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep51 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep52 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep53 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep54 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep55 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep56 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep57 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep58 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep59 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep60 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep61 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep62 = 0;
	UPROPERTY(meta = (CrowdyState)) int32 Rep63 = 0;
};

/**
 * Send fixture (Phase 3): a CrowdyState ACTOR with a spread of leaf types plus one owner-only and one
 * manual-dirty property, so the replicator tests can prove owner-only / manual-dirty are EXCLUDED from the
 * Phase 3 spatial diff. It has no entity component/executor, so its layout is never overlap-filtered. The
 * diff container must be an AActor (DispatchGameEvent takes a context actor), so this is a native actor
 * NewObject-able without a world on the test's dispatch-hook path.
 *
 * The four spatial leaves carry CrowdyHeartbeat so the keyframe-heartbeat tests emit a keyframe (heartbeat is
 * opt-in per property); RepManualDirty is intentionally NOT a heartbeat property, so a keyframe carries exactly
 * the four marked, non-owner-only leaves.
 */
UCLASS()
class ACrowdyStateSendTestActor : public AActor
{
	GENERATED_BODY()

public:

	UPROPERTY(meta = (CrowdyState, CrowdyHeartbeat))
	int32 RepInt = 0;

	UPROPERTY(meta = (CrowdyState, CrowdyHeartbeat))
	float RepFloat = 0.f;

	UPROPERTY(meta = (CrowdyState, CrowdyHeartbeat))
	FName RepName = NAME_None;

	UPROPERTY(meta = (CrowdyState, CrowdyHeartbeat))
	FVector RepVector = FVector::ZeroVector;

	UPROPERTY(meta = (CrowdyState, CrowdyOwnerOnly))
	int32 RepOwnerOnly = 0;

	UPROPERTY(meta = (CrowdyState, CrowdyManualDirty))
	int32 RepManualDirty = 0;

	UPROPERTY()
	int32 NotReplicated = 0;
};

/**
 * HostOverride fixture (Phase C): a CrowdyState actor that ALSO carries a UCrowdyEntityComponent (default
 * subobject) so both the send-side check (MarkStateDirty) and the receive-side backstop can read
 * GetHostOverridePolicy() off it. A test sets Entity->HostOverride to Allow or OwnerOnly before the push.
 */
UCLASS()
class ACrowdyStateHostOverrideActor : public AActor
{
	GENERATED_BODY()
public:
	ACrowdyStateHostOverrideActor();
	UPROPERTY(meta = (CrowdyState))
	int32 RepInt = 0;
	UPROPERTY()
	TObjectPtr<UCrowdyEntityComponent> Entity;
};

/**
 * Heartbeat fixture: a CrowdyState actor carrying a UCrowdyEntityComponent (so a test can set StateHeartbeat)
 * plus one CrowdyHeartbeat-marked property, so the per-entity heartbeat kill switch (B) is exercisable end to
 * end: with StateHeartbeat==Off the keyframe is suppressed even though RepBeat is marked, and with Inherit it
 * fires. Distinct from ACrowdyStateHostOverrideActor (whose RepInt is deliberately NOT heartbeat-marked, so it
 * covers the timer-due-but-no-marked-properties case instead).
 */
UCLASS()
class ACrowdyStateHeartbeatActor : public AActor
{
	GENERATED_BODY()
public:
	ACrowdyStateHeartbeatActor();
	UPROPERTY(meta = (CrowdyState, CrowdyHeartbeat))
	int32 RepBeat = 0;
	UPROPERTY()
	TObjectPtr<UCrowdyEntityComponent> Entity;
};

/**
 * Executor state struct sharing a field name+type (Health : float) with ACrowdyStateOverlapActor, so the
 * executor-overlap filter finds the collision and drops it from the CrowdyState layout.
 */
USTRUCT()
struct FCrowdyStateOverlapState
{
	GENERATED_BODY()

	UPROPERTY()
	float Health = 0.f;
};

/**
 * Executor whose state struct is FCrowdyStateOverlapState. The overlap resolver reads GetStateStruct() off
 * this (as a default subobject on the actor's entity component) to know which fields the continuous channel
 * already writes.
 */
UCLASS()
class UCrowdyStateOverlapExecutor : public UActorUpdateExecutor
{
	GENERATED_BODY()

public:

	virtual UScriptStruct* GetStateStruct_Implementation() const override
	{
		return FCrowdyStateOverlapState::StaticStruct();
	}
};

/**
 * Overlap fixture (Phase 3): its CrowdyState 'Health' ALSO lives in the executor state struct, so the
 * overlap filter must drop Health from the layout while 'Mana' survives. The entity component and its
 * StateExecutor are set as default subobjects in the constructor so the resolver reaches
 * Entity->StateExecutor->GetStateStruct() straight off the CDO (mirroring HasEntityComponent's native-CDO
 * probe: FindComponentByClass on a native actor CDO sees a CreateDefaultSubobject component).
 */
UCLASS()
class ACrowdyStateOverlapActor : public AActor
{
	GENERATED_BODY()

public:

	ACrowdyStateOverlapActor();

	// Dropped: overlaps the executor state struct.
	UPROPERTY(meta = (CrowdyState))
	float Health = 0.f;

	// Survives: no overlap.
	UPROPERTY(meta = (CrowdyState))
	float Mana = 0.f;

	UPROPERTY()
	TObjectPtr<UCrowdyEntityComponent> Entity;
};

/**
 * Apply fixture component (Phase 4): a component carrying CrowdyState leaf properties, so the receive
 * path's container resolution (an actor whose layout owner class is a component) can be exercised.
 */
UCLASS()
class UCrowdyStateApplyTestComponent : public UActorComponent
{
	GENERATED_BODY()

public:

	UPROPERTY(meta = (CrowdyState))
	int32 CompInt = 0;

	UPROPERTY(meta = (CrowdyState))
	float CompFloat = 0.f;
};

/**
 * A second, unrelated component (no CrowdyState properties) so the container-resolution test can assert
 * ResolveStateContainer picks the RIGHT component when several are present on the actor.
 */
UCLASS()
class UCrowdyStateApplyOtherComponent : public UActorComponent
{
	GENERATED_BODY()
};

/**
 * Apply fixture actor (Phase 4): CrowdyState leaf properties plus two OnRep'd properties whose notifies
 * bump distinct plain (non-replicated) counters, so a test can assert OnRep fires exactly for the changed
 * OnRep'd property and not for the unchanged one. Two components are default subobjects so container
 * resolution has both a matching and a non-matching component to choose between.
 */
UCLASS()
class ACrowdyStateApplyTestActor : public AActor
{
	GENERATED_BODY()

public:

	ACrowdyStateApplyTestActor();

	UPROPERTY(meta = (CrowdyState))
	int32 RepInt = 0;

	UPROPERTY(meta = (CrowdyState))
	float RepFloat = 0.f;

	UPROPERTY(meta = (CrowdyState))
	FVector RepVector = FVector::ZeroVector;

	// Two OnRep'd properties; each notify bumps its own counter so a test can assert the notify fired for
	// the property that changed and not for the one that did not.
	UPROPERTY(meta = (CrowdyState, CrowdyOnRep = "OnRep_Health"))
	float RepHealth = 0.f;

	UPROPERTY(meta = (CrowdyState, CrowdyOnRep = "OnRep_Score"))
	int32 RepScore = 0;

	// Notify counters plain, NOT replicated.
	UPROPERTY()
	int32 HealthOnRepCount = 0;

	UPROPERTY()
	int32 ScoreOnRepCount = 0;

	UPROPERTY()
	TObjectPtr<UCrowdyStateApplyTestComponent> ApplyComp;

	UPROPERTY()
	TObjectPtr<UCrowdyStateApplyOtherComponent> OtherComp;

	UFUNCTION()
	void OnRep_Health();

	UFUNCTION()
	void OnRep_Score();
};

/**
 * Subsystem-replication fixture (Subsystem Phase 1): a plain UObject (NOT an actor), standing in for a
 * host-owned subsystem participant. It carries CrowdyState leaf properties incl. a CrowdyManualDirty one and a
 * CrowdyOnRep + CrowdyHeartbeat one, so the enroll / non-spatial diff / channel-route / apply-OnRep tests have a
 * non-actor participant. Two of the properties (RepInt, RepNotified) are CrowdyHeartbeat-marked so the keyframe
 * heartbeat carries them. A plain UObject fires ProcessEvent (OnRep) WITHOUT a world, unlike an AActor, so the
 * apply test needs no editor world. NotifiedOnRepCount is a plain (non-replicated) counter the notify bumps so a
 * test can assert the notify fired.
 */
UCLASS()
class UCrowdyStateSubsystemTestTarget : public UObject
{
	GENERATED_BODY()

public:

	UPROPERTY(meta = (CrowdyState, CrowdyHeartbeat))
	int32 RepInt = 0;

	UPROPERTY(meta = (CrowdyState))
	float RepFloat = 0.f;

	UPROPERTY(meta = (CrowdyState, CrowdyManualDirty))
	int32 RepManualDirty = 0;

	UPROPERTY(meta = (CrowdyState, CrowdyOnRep = "OnRep_Notified", CrowdyHeartbeat))
	int32 RepNotified = 0;

	UPROPERTY()
	int32 NotifiedOnRepCount = 0;

	UFUNCTION()
	void OnRep_Notified();
};

/**
 * A second, distinct subsystem participant class so the deterministic-identity test can prove two different
 * classes mint two different NetIDs (a host participant's id is derived from its class path).
 */
UCLASS()
class UCrowdyStateSubsystemTestTargetB : public UObject
{
	GENERATED_BODY()

public:

	UPROPERTY(meta = (CrowdyState))
	int32 RepInt = 0;
};

/**
 * OnRep-signature-reject fixture (Phase 4): its first CrowdyOnRep names a function that TAKES A PARAMETER,
 * so discovery must clear that binding; the second names a valid parameterless notify, which survives.
 */
UCLASS()
class UCrowdyStateBadOnRepTarget : public UObject
{
	GENERATED_BODY()

public:

	// Bad: OnRep_Bad takes a parameter, so the binding must be cleared at discovery.
	UPROPERTY(meta = (CrowdyState, CrowdyOnRep = "OnRep_Bad"))
	int32 RepBad = 0;

	// Good: parameterless notify, binding survives.
	UPROPERTY(meta = (CrowdyState, CrowdyOnRep = "OnRep_Good"))
	int32 RepGood = 0;

	UFUNCTION()
	void OnRep_Bad(int32 NewValue);

	UFUNCTION()
	void OnRep_Good();
};
