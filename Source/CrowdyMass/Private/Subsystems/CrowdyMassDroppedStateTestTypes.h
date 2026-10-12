#pragma once

#include "CoreMinimal.h"
#include "CrowdyMassDroppedStateTestTypes.generated.h"

// A class replicating one property a registered fragment field asks for and two it does not. The two are
// the subject: they encode, cross, decode and are then thrown away, which is indistinguishable from the
// crowd simply ignoring them.
//
// Claimed is a uint8 because that is the type the fixture's one registration reads; the other two are
// deliberately of other types, so a case can tell the "nothing claims this" report apart from the separate
// refusal a property gets for being declared as something other than what a field that DID match it reads.
// Both of those types are bindable, which is the half the report has to get right: their remedy is to
// register a field for that type, not to redeclare them.
UCLASS(meta = (CrowdyTestFixture))
class UCrowdyMassDroppedStateTarget : public UObject
{
	GENERATED_BODY()

public:

	UPROPERTY(meta = (CrowdyState))
	uint8 Claimed = 0;

	UPROPERTY(meta = (CrowdyState))
	int32 Health = 0;

	UPROPERTY(meta = (CrowdyState))
	float Stamina = 0.0f;
};

// A second class replicating a property of the SAME name as one of the dropped properties above. Two
// classes replicating a Health each are two separate authoring facts with two separate remedies, so a
// report keyed on the property name alone would silence the second one for good.
UCLASS(meta = (CrowdyTestFixture))
class UCrowdyMassDroppedStateSecondTarget : public UObject
{
	GENERATED_BODY()

public:

	UPROPERTY(meta = (CrowdyState))
	int32 Health = 0;
};

// A class dropping a property a registered field COULD have bound, simply unclaimed because nothing
// registered its name. It has to be its own class: the report is written once per (class, property) and the
// class above has already spent its lines.
UCLASS(meta = (CrowdyTestFixture))
class UCrowdyMassDroppedStateByteTarget : public UObject
{
	GENERATED_BODY()

public:

	UPROPERTY(meta = (CrowdyState))
	uint8 Posture = 0;
};

// A class carrying three properties whose reports have to differ, one per reason.
//
// Sprinting is a native bool: bindable, so its remedy is to register a field that reads a bool. It is the
// property that makes a width-based sentence read as nonsense, since it is one byte and a four-byte int
// beside it binds too.
//
// Nickname is an FString: it replicates perfectly well and no field type can read it, so its remedy is the
// actor representation and it must never be told to register a field.
//
// Secret is bindable, and delivered only to the entity's owner, so a crowd observer never receives it and a
// field registered for it would be handed nothing.
UCLASS(meta = (CrowdyTestFixture))
class UCrowdyMassDroppedStateUnbindableTarget : public UObject
{
	GENERATED_BODY()

public:

	UPROPERTY(meta = (CrowdyState))
	bool bSprinting = false;

	UPROPERTY(meta = (CrowdyState))
	FString Nickname;

	UPROPERTY(meta = (CrowdyState, CrowdyOwnerOnly))
	uint8 Secret = 0;
};

// A class whose only replicated property is a bitfield.
//
// A bitfield is the case where naming the C++ type alone misleads: it reports the width it is packed into,
// so a message built from that reads "declared as uint8" for a property no field reading a uint8 can bind,
// while the plain bool beside it in the fixture above binds fine. Its fix is on the declaration, and it is
// the only property whose remedy is to stop being a bitfield.
UCLASS(meta = (CrowdyTestFixture))
class UCrowdyMassDroppedStateBitfieldTarget : public UObject
{
	GENERATED_BODY()

public:

	UPROPERTY(meta = (CrowdyState))
	uint8 bCrouched : 1;
};

// A class every registered field claims, so nothing about it is dropped. It exists to keep the report from
// degrading into one that fires for any class at all, which would train an author to ignore the line that
// names their real mistake.
UCLASS(meta = (CrowdyTestFixture))
class UCrowdyMassDroppedStateFullyClaimedTarget : public UObject
{
	GENERATED_BODY()

public:

	UPROPERTY(meta = (CrowdyState))
	uint8 Claimed = 0;
};
