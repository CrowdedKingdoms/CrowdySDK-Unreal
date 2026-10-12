#pragma once

#include "CoreMinimal.h"
#include "CrowdyMassStateFragmentTestTypes.generated.h"

// A game's own state enum, declared the ordinary way: an enum class with a uint8 underlying type. A
// property of this type reflects as an enum property wrapping a byte rather than as a plain byte.
//
// Dead is value 1 and Mounted is value 2, which matters to the cases below: an animation set is free to
// author rows in any order, so a row id and an enum value are only equal by coincidence and the fixtures
// deliberately arrange for them not to be.
UENUM()
enum class ECrowdyMassStateFragmentTestStance : uint8
{
	Normal,
	Dead,
	Mounted
};

// A second byte-wide enum, unrelated to the one an animation set names. It exists to keep "matched by
// type" from degrading into "matched by being a byte enum": a property of this type is exactly as
// readable through a byte pointer as a stance is, and must still not be taken for one.
UENUM()
enum class ECrowdyMassStateFragmentTestMood : uint8
{
	Calm,
	Angry
};

// Fixture entity class carrying a single CrowdyState property of the stance enum. Stands in for a real
// CrowdyState-native game class with no participant and no component of its own.
//
// Called Stance rather than PoseId on purpose. The name is no longer part of the contract, so a fixture
// still called PoseId would let a build that quietly went back to matching by name pass every case here.
UCLASS(meta = (CrowdyTestFixture))
class UCrowdyMassStateFragmentPoseTarget : public UObject
{
	GENERATED_BODY()

public:

	UPROPERTY(meta = (CrowdyState))
	ECrowdyMassStateFragmentTestStance Stance = ECrowdyMassStateFragmentTestStance::Normal;
};

// A second fixture, layout-identical in spirit but with an EXTRA CrowdyState property declared BEFORE the
// stance, so the stance sits at a different layout index than in UCrowdyMassStateFragmentPoseTarget.
// Proves resolution is by declared type, not by position: a positional implementation would read
// SomeOtherState's slot (an int32) through the pose ingestor's byte pointer instead of the stance's.
UCLASS(meta = (CrowdyTestFixture))
class UCrowdyMassStateFragmentReorderedPoseTarget : public UObject
{
	GENERATED_BODY()

public:

	UPROPERTY(meta = (CrowdyState))
	int32 SomeOtherState = 0;

	UPROPERTY(meta = (CrowdyState))
	ECrowdyMassStateFragmentTestStance Stance = ECrowdyMassStateFragmentTestStance::Normal;
};

// A fixture whose stance is declared OwnerOnly: delivered only to the entity's own owning client, so this
// crowd-observer provider will never actually see it change. Binding still happens; the diagnostic this
// fixture exercises is the one-time warning that the bound ingestor is a permanent no-op here.
UCLASS(meta = (CrowdyTestFixture))
class UCrowdyMassStateFragmentOwnerOnlyPoseTarget : public UObject
{
	GENERATED_BODY()

public:

	UPROPERTY(meta = (CrowdyState, CrowdyOwnerOnly))
	ECrowdyMassStateFragmentTestStance Stance = ECrowdyMassStateFragmentTestStance::Normal;
};

// A state enum too wide for the one byte a replicated pose travels in. It has to be REFUSED rather than
// truncated: reading its low byte would answer a real pose id for a value that never fits, so a wider
// state would select whichever row that byte happened to name.
UENUM()
enum class ECrowdyMassStateFragmentTestWideStance : uint16
{
	Normal = 0,
	Distant = 300
};

UCLASS(meta = (CrowdyTestFixture))
class UCrowdyMassStateFragmentWideEnumPoseTarget : public UObject
{
	GENERATED_BODY()

public:

	UPROPERTY(meta = (CrowdyState))
	ECrowdyMassStateFragmentTestWideStance Stance = ECrowdyMassStateFragmentTestWideStance::Normal;
};

// TWO properties of the wide enum. Both are refused for their width, so neither drives the pose, and the
// point of the fixture is that saying so stays true of BOTH: the first one refused must not consume the
// pose field on its way out, because a consumed field makes every later property of that enum unreachable
// and makes the report about them a confident lie.
UCLASS(meta = (CrowdyTestFixture))
class UCrowdyMassStateFragmentTwoWideEnumTarget : public UObject
{
	GENERATED_BODY()

public:

	UPROPERTY(meta = (CrowdyState))
	ECrowdyMassStateFragmentTestWideStance Stance = ECrowdyMassStateFragmentTestWideStance::Normal;

	UPROPERTY(meta = (CrowdyState))
	ECrowdyMassStateFragmentTestWideStance SecondStance = ECrowdyMassStateFragmentTestWideStance::Normal;
};

// The OTHER declaration the SDK promises to accept for a uint8 field, in the reflection helper's own
// comment and in the frozen contract. TEnumAsByte reflects as a plain FByteProperty carrying the enum
// beside the byte rather than as an enum property wrapping one, so it reaches both the type match and the
// wire encoder down a different branch from every other fixture here, and an untested promise is how that
// branch stays broken.
UENUM()
namespace ECrowdyMassStateFragmentTestMount
{
	enum Type : int
	{
		Grounded,
		Mounted,
		Harnessed
	};
}

UCLASS(meta = (CrowdyTestFixture))
class UCrowdyMassStateFragmentEnumAsByteTarget : public UObject
{
	GENERATED_BODY()

public:

	UPROPERTY(meta = (CrowdyState))
	TEnumAsByte<ECrowdyMassStateFragmentTestMount::Type> Mount = ECrowdyMassStateFragmentTestMount::Grounded;
};

// A class carrying a CrowdyState byte enum that is NOT the enum its animation set names. Nothing about it
// is malformed; it is simply some other replicated state of the game's, and taking it for a pose would
// drive the crowd's appearance off whatever unrelated value happened to be a byte.
UCLASS(meta = (CrowdyTestFixture))
class UCrowdyMassStateFragmentOtherEnumTarget : public UObject
{
	GENERATED_BODY()

public:

	UPROPERTY(meta = (CrowdyState))
	ECrowdyMassStateFragmentTestMood Mood = ECrowdyMassStateFragmentTestMood::Calm;
};

// A class declaring the pose the way the SDK used to require: a raw uint8 called exactly PoseId, carrying
// a resolved row id rather than a state value. That contract is gone, and this fixture is here so its
// removal is asserted rather than assumed: such a property must bind nothing at all, because a row id
// chosen by the sender is exactly what the phase stopped trusting.
UCLASS(meta = (CrowdyTestFixture))
class UCrowdyMassStateFragmentLegacyByteTarget : public UObject
{
	GENERATED_BODY()

public:

	UPROPERTY(meta = (CrowdyState))
	uint8 PoseId = 0;
};

// A class declaring TWO CrowdyState properties of the stance enum. One animation set names one enum, so
// both are asking for the same rows and only one can drive the pose; which one is arbitrary to an author,
// so the choice has to be reported rather than left to declaration order.
UCLASS(meta = (CrowdyTestFixture))
class UCrowdyMassStateFragmentTwoStanceTarget : public UObject
{
	GENERATED_BODY()

public:

	UPROPERTY(meta = (CrowdyState))
	ECrowdyMassStateFragmentTestStance Stance = ECrowdyMassStateFragmentTestStance::Normal;

	UPROPERTY(meta = (CrowdyState))
	ECrowdyMassStateFragmentTestStance SecondStance = ECrowdyMassStateFragmentTestStance::Normal;
};

// A class carrying one property of each type a field can be registered for by name, so a case can bind one
// and watch the ingestor read it back. This class is never given an animation set, which is what keeps
// every case using it about NAME matching alone.
//
// The values the cases send are chosen so that a read of the wrong width cannot pass by luck: an int whose
// low byte is not its answer, and a float that is not a whole number.
UCLASS(meta = (CrowdyTestFixture))
class UCrowdyMassStateFragmentTypedTarget : public UObject
{
	GENERATED_BODY()

public:

	UPROPERTY(meta = (CrowdyState))
	uint8 Posture = 0;

	// A byte-wide enum claimed BY NAME rather than by the pose's type match, which is the only way this
	// class's properties are reached. It proves a field registered for a uint8 follows an enum to the type
	// it is declared over rather than refusing the declaration a game ordinarily writes.
	UPROPERTY(meta = (CrowdyState))
	ECrowdyMassStateFragmentTestMood Mood = ECrowdyMassStateFragmentTestMood::Calm;

	UPROPERTY(meta = (CrowdyState))
	int32 Health = 0;

	UPROPERTY(meta = (CrowdyState))
	float Stamina = 0.0f;
};

// An enum declared over something other than a byte, on a class with no animation set, so it is only ever
// reached by a NAME match. It separates two rules that used to be one: a field registered for an int32
// reads it, and a field registered for a uint8 is refused it, without the pose's own byte rule being
// involved in either answer.
UENUM()
enum class ECrowdyMassStateFragmentTestBand : int32
{
	Low = 0,
	High = 70000
};

UCLASS(meta = (CrowdyTestFixture))
class UCrowdyMassStateFragmentBandTarget : public UObject
{
	GENERATED_BODY()

public:

	UPROPERTY(meta = (CrowdyState))
	ECrowdyMassStateFragmentTestBand Band = ECrowdyMassStateFragmentTestBand::Low;
};
