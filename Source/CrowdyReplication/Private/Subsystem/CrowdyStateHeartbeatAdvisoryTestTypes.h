#pragma once

// Deliberately NOT wrapped in WITH_DEV_AUTOMATION_TESTS: UnrealHeaderTool refuses a reflected type inside
// a preprocessor block, so these fixtures are declared unconditionally and only the tests that use them
// are guarded. The sibling fixture headers for the state fragment cases are unconditional for the same
// reason.

#include "CoreMinimal.h"
#include "CrowdyStateHeartbeatAdvisoryTestTypes.generated.h"

// An enum class, the shape a C++ author reaches for and the shape that reflects as an FEnumProperty.
UENUM()
enum class ECrowdyStateHeartbeatAdvisoryStance : uint8
{
	Idle,
	Running,
	Down
};

// An old-style enum, so a TEnumAsByte property below reflects as an FByteProperty carrying an enum. That
// is the other shape an enum-typed property arrives in, and a check that only knew FEnumProperty would
// walk straight past it.
UENUM()
enum ECrowdyStateHeartbeatAdvisoryMood : int
{
	Calm,
	Restless
};

/**
 * Carries one property of every kind the advisory has to separate: an enum with no heartbeat (the
 * subject), a byte-backed enum with no heartbeat (the same subject in its other reflected shape), a
 * non-enum with no heartbeat, an enum that is already on the heartbeat, and an owner-only enum, which a
 * keyframe never carries however it is marked.
 */
UCLASS(meta = (CrowdyTestFixture))
class UCrowdyStateHeartbeatAdvisoryTarget : public UObject
{
	GENERATED_BODY()

public:

	UPROPERTY(meta = (CrowdyState))
	ECrowdyStateHeartbeatAdvisoryStance Stance = ECrowdyStateHeartbeatAdvisoryStance::Idle;

	UPROPERTY(meta = (CrowdyState))
	TEnumAsByte<ECrowdyStateHeartbeatAdvisoryMood> Mood = ECrowdyStateHeartbeatAdvisoryMood::Calm;

	UPROPERTY(meta = (CrowdyState))
	int32 Ticks = 0;

	UPROPERTY(meta = (CrowdyState, CrowdyHeartbeat))
	ECrowdyStateHeartbeatAdvisoryStance HeartbeatStance = ECrowdyStateHeartbeatAdvisoryStance::Idle;

	UPROPERTY(meta = (CrowdyState, CrowdyOwnerOnly))
	ECrowdyStateHeartbeatAdvisoryStance OwnerOnlyStance = ECrowdyStateHeartbeatAdvisoryStance::Idle;
};

// A second class with its own enum property, so a case can show that silencing one subject does not
// silence another.
UCLASS(meta = (CrowdyTestFixture))
class UCrowdyStateHeartbeatAdvisoryOtherTarget : public UObject
{
	GENERATED_BODY()

public:

	UPROPERTY(meta = (CrowdyState))
	ECrowdyStateHeartbeatAdvisoryStance Stance = ECrowdyStateHeartbeatAdvisoryStance::Idle;
};

// A base class declaring one uncovered enum property, and two subclasses that add nothing. A layout
// carries its super-class properties, so this one property appears in three layouts; the author can only
// edit the UPROPERTY on the base, so that is the class the one line has to name.
UCLASS(meta = (CrowdyTestFixture))
class UCrowdyStateHeartbeatAdvisoryBaseTarget : public UObject
{
	GENERATED_BODY()

public:

	UPROPERTY(meta = (CrowdyState))
	ECrowdyStateHeartbeatAdvisoryStance InheritedStance = ECrowdyStateHeartbeatAdvisoryStance::Idle;
};

// Marked even though it declares no CrowdyState property of its own: it inherits one, so the sweep would
// otherwise advise about the base through this class.
UCLASS(meta = (CrowdyTestFixture))
class UCrowdyStateHeartbeatAdvisoryFirstDerivedTarget : public UCrowdyStateHeartbeatAdvisoryBaseTarget
{
	GENERATED_BODY()
};

UCLASS(meta = (CrowdyTestFixture))
class UCrowdyStateHeartbeatAdvisorySecondDerivedTarget : public UCrowdyStateHeartbeatAdvisoryBaseTarget
{
	GENERATED_BODY()
};

// A class the advisory must have nothing to say about, so the line stays worth reading: its enum is on
// the heartbeat, its other enum is owner-only, and its remaining property is not an enum at all.
UCLASS(meta = (CrowdyTestFixture))
class UCrowdyStateHeartbeatAdvisoryCoveredTarget : public UObject
{
	GENERATED_BODY()

public:

	UPROPERTY(meta = (CrowdyState, CrowdyHeartbeat))
	ECrowdyStateHeartbeatAdvisoryStance Stance = ECrowdyStateHeartbeatAdvisoryStance::Idle;

	UPROPERTY(meta = (CrowdyState, CrowdyOwnerOnly))
	ECrowdyStateHeartbeatAdvisoryStance OwnerOnlyStance = ECrowdyStateHeartbeatAdvisoryStance::Idle;

	UPROPERTY(meta = (CrowdyState))
	float Speed = 0.0f;
};
