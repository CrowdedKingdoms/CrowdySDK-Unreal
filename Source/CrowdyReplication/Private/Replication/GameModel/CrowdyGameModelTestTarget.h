// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"
#include "Replication/GameModel/CrowdyBindingKeyProvider.h"
#include "Replication/GameModel/CrowdyModelRef.h"
#include "UObject/Object.h"
#include "CrowdyGameModelTestTarget.generated.h"

/**
 * Headless test target for the Game Model apply/OnRep path: a UObject carrying CrowdyModel attributes with
 * parameterless CrowdyOnRep notifies that count their firings, so ApplyState/ApplyMutations + OnRep can be
 * exercised without a world or a live Game API (mirrors CrowdyStateTestTarget / CrowdyRpcTestTarget).
 */
UCLASS()
class UCrowdyGameModelTestTarget : public UObject
{
	GENERATED_BODY()

public:
	// Two authoritative attributes with OnRep notifies. Server keys are the lowercased names: "hp", "mana".
	UPROPERTY(meta = (CrowdyModel, CrowdyOnRep = "OnRep_Hp"))
	int32 Hp = 100;

	UPROPERTY(meta = (CrowdyModel, CrowdyOnRep = "OnRep_Mana"))
	int32 Mana = 50;

	// An authoritative attribute with NO OnRep a change updates the cache but fires nothing.
	UPROPERTY(meta = (CrowdyModel))
	int32 Gold = 0;

	// A float attribute with OnRep and a string attribute exercise the non-integer canonicalization + member
	// write paths (an int-only fixture masked a bare-scalar parse bug the adversarial verify pass flagged).
	UPROPERTY(meta = (CrowdyModel, CrowdyOnRep = "OnRep_Speed"))
	float Speed = 1.0f;

	UPROPERTY(meta = (CrowdyModel))
	FString Title;

	// A uint8 attribute (discovered as "int") exercises the generic numeric write branch an int32-only write
	// path silently no-ops a byte/enum member while its OnRep still fires, reading a stale value.
	UPROPERTY(meta = (CrowdyModel, CrowdyOnRep = "OnRep_Level"))
	uint8 Level = 1;

	// A plain (non-Model) property never matched by the server-key map, so a same-named key is ignored.
	UPROPERTY()
	int32 NotAnAttribute = 0;

	int32 HpOnRepCount = 0;
	int32 ManaOnRepCount = 0;
	int32 SpeedOnRepCount = 0;
	int32 LevelOnRepCount = 0;

	UFUNCTION()
	void OnRep_Hp() { ++HpOnRepCount; }

	UFUNCTION()
	void OnRep_Mana() { ++ManaOnRepCount; }

	UFUNCTION()
	void OnRep_Speed() { ++SpeedOnRepCount; }

	UFUNCTION()
	void OnRep_Level() { ++LevelOnRepCount; }

	// Spy for the free/data-container change delegate: OnDataContainerChanged is a UObject dynamic
	// multicast, so its handler must be a UFUNCTION; this counts firings and records the last container id.
	UPROPERTY()
	int32 DataChangedCount = 0;

	UPROPERTY()
	FString LastChangedContainerId;

	UFUNCTION()
	void HandleDataContainerChanged(const FString& ContainerId)
	{
		++DataChangedCount;
		LastChangedContainerId = ContainerId;
	}

	// Spy for the per-attribute change delegate (OnModelAttributeChanged): a UObject dynamic multicast, so its
	// handler must be a UFUNCTION. Counts firings and records the last change so a test can assert the
	// Target/ModelId/Attribute/old/new it carried.
	int32 AttributeChangedCount = 0;
	TWeakObjectPtr<UObject> LastAttrTarget;
	FString LastAttrModelId;
	FName LastAttrKey;
	FString LastAttrOldJson;
	FString LastAttrNewJson;

	UFUNCTION()
	void HandleModelAttributeChanged(UObject* InTarget, const FString& InModelId, FName Attribute,
		const FString& OldValueJson, const FString& NewValueJson)
	{
		++AttributeChangedCount;
		LastAttrTarget = InTarget;
		LastAttrModelId = InModelId;
		LastAttrKey = Attribute;
		LastAttrOldJson = OldValueJson;
		LastAttrNewJson = NewValueJson;
	}
};

/**
 * A subclass that declares no attributes of its own, so every attribute it exposes is owned by its parent.
 * Lets a test show that a class's resolved attributes can point at another class's reflection data, which is
 * what makes dropping a single class's cached lookup insufficient. Deliberately carries no container tag, so
 * it never appears to the sweeps that collect container classes.
 */
UCLASS()
class UCrowdyGameModelTestTargetDerived : public UCrowdyGameModelTestTarget
{
	GENERATED_BODY()
};

/**
 * Component container fixture for Stage 2 sub-participants: a CrowdyContainer-tagged UActorComponent with one
 * CrowdyModel attribute + a counting parameterless CrowdyOnRep. Standing in for the reusable attributes component
 * (health/armor on a boss or player), it lets the sub-participant enrollment + GetModelComponent resolution be
 * exercised headless. CrowdyContainerTest keeps it out of the real schema sync.
 */
UCLASS(meta = (CrowdyContainer = "TestAttributes", CrowdyContainerTest))
class UCrowdyGameModelTestComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UPROPERTY(meta = (CrowdyModel, CrowdyOnRep = "OnRep_Armor"))
	int32 Armor = 10;

	int32 ArmorOnRepCount = 0;

	UFUNCTION()
	void OnRep_Armor() { ++ArmorOnRepCount; }
};

/**
 * A CrowdyContainer component that supplies its own binding key via ICrowdyBindingKeyProvider, standing in for a
 * runtime-added component that needs a cross-client-stable identity the engine cannot give. The key is settable so
 * a test can point two of them at the same key (collision) or distinct keys. CrowdyContainerTest keeps it out of
 * the real schema sync.
 */
UCLASS(meta = (CrowdyContainer = "TestKeyedAttributes", CrowdyContainerTest))
class UCrowdyGameModelTestKeyedComponent : public UActorComponent, public ICrowdyBindingKeyProvider
{
	GENERATED_BODY()

public:
	UPROPERTY(meta = (CrowdyModel))
	int32 Charge = 0;

	// The key this component reports; a test sets it before enrollment.
	FString BindingKey = TEXT("keyed-instance");

	virtual FString GetCrowdyBindingKey_Implementation() const override { return BindingKey; }
};

/**
 * An actor that supplies its own binding key via ICrowdyBindingKeyProvider, standing in for an independently
 * client-spawned world object whose identity the engine cannot place-derive. Lets the actor-side ResolveIdentity
 * key path be exercised headless.
 */
UCLASS()
class ACrowdyBindingKeyTestActor : public AActor, public ICrowdyBindingKeyProvider
{
	GENERATED_BODY()

public:
	FString BindingKey = TEXT("world_object");

	virtual FString GetCrowdyBindingKey_Implementation() const override { return BindingKey; }
};

/**
 * A component tagged CrowdyContainer but carrying NO supported Server Owned attribute, so the Stage 2 sweep warns
 * and skips it (never enrolling a useless participant). CrowdyContainerTest keeps it out of the real schema sync.
 */
UCLASS(meta = (CrowdyContainer = "TestEmptyAttributes", CrowdyContainerTest))
class UCrowdyGameModelTestEmptyComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	// Not a CrowdyModel attribute, so the class has no bindable Server Owned state.
	UPROPERTY()
	int32 Bookkeeping = 0;
};

// A reflected enum for the discovery fixture: an `enum class : uint8` marked CrowdyModel must map to "int"
// (parity with TEnumAsByte), not be silently dropped.
UENUM()
enum class ECrowdyGameModelTestTier : uint8
{
	Bronze,
	Silver,
	Gold
};

/**
 * Discovery fixture for FCrowdyAttributeRegistry. Exercises every branch of DiscoverForClass: the
 * four supported value types, a native ClampMin/ClampMax pair, a valid parameterless notify, a notify with the
 * wrong arity (dropped), a plane-exclusivity conflict (CrowdyModel + CrowdyState, rejected), an unsupported
 * type (rejected), and a plain property (never a CrowdyModel). Kept separate from the apply target above so
 * its deliberately-invalid metadata never perturbs the apply/OnRep tests. Carries the CrowdyContainer class
 * tag so GetContainerTypeName resolves.
 */
UCLASS(meta = (CrowdyContainer = "TestHero", CrowdyContainerTest))
class UCrowdyGameModelDiscoveryTarget : public UObject
{
	GENERATED_BODY()

public:
	// Accepted: "int", native clamp [0,100], parameterless notify resolves.
	UPROPERTY(meta = (CrowdyModel, ClampMin = "0", ClampMax = "100", CrowdyOnRep = "OnRep_Health"))
	int32 Health = 100;

	// Accepted: "float" / "bool" / "string", no clamp, no notify.
	UPROPERTY(meta = (CrowdyModel))
	float Speed = 1.0f;

	UPROPERTY(meta = (CrowdyModel))
	bool bStunned = false;

	UPROPERTY(meta = (CrowdyModel))
	FString Title;

	// Accepted: an enum class : uint8 maps to "int" (parity with a byte attribute), not dropped.
	UPROPERTY(meta = (CrowdyModel))
	ECrowdyGameModelTestTier Tier = ECrowdyGameModelTestTier::Bronze;

	// Accepted attribute, but the notify is one-arg -> dropped (OnRepFunctionName stays NAME_None).
	UPROPERTY(meta = (CrowdyModel, CrowdyOnRep = "OnRep_OneArg"))
	int32 BadNotify = 0;

	// Rejected: a field lives in exactly one plane, so CrowdyModel + CrowdyState is a discovery-time reject.
	UPROPERTY(meta = (CrowdyModel, CrowdyState))
	int32 DualPlane = 0;

	// Rejected: a TMap is not a supported Server Owned type (scalars, strings, scalar arrays, model refs, and
	// plain structs are; a map is not).
	UPROPERTY(meta = (CrowdyModel))
	TMap<FString, int32> Bag;

	// Never a CrowdyModel attribute.
	UPROPERTY()
	int32 PlainInt = 0;

	UFUNCTION()
	void OnRep_Health() {}

	// Deliberately wrong arity: a notify cannot take parameters.
	UFUNCTION()
	void OnRep_OneArg(int32 Unused) { (void)Unused; }
};

/**
 * Discovery fixture for the key-override + visibility surface: a CrowdyKey server-key override, the
 * three read-visibilities, an unrecognized visibility (warns, falls back to public), and a default (no
 * CrowdyVisibility -> public). Separate from the main discovery target so its extra attributes never perturb
 * that fixture's exact-count assertions.
 */
UCLASS(meta = (CrowdyContainer = "KeyVisHero", CrowdyContainerTest))
class UCrowdyGameModelKeyVisTarget : public UObject
{
	GENERATED_BODY()

public:
	// CrowdyKey override: the property is "Reserve" but its server key is "ammo" (not the derived "reserve").
	UPROPERTY(meta = (CrowdyModel, CrowdyKey = "ammo"))
	int32 Reserve = 0;

	// Read-visibility "owner": only the container's owner may read it. Derived key "secretscore".
	UPROPERTY(meta = (CrowdyModel, CrowdyVisibility = "owner"))
	int32 SecretScore = 0;

	// Read-visibility "hidden": server/admin only. Derived key "adminnote".
	UPROPERTY(meta = (CrowdyModel, CrowdyVisibility = "hidden"))
	FString AdminNote;

	// An unrecognized visibility warns and falls back to "public". Derived key "mistyped".
	UPROPERTY(meta = (CrowdyModel, CrowdyVisibility = "sneaky"))
	int32 Mistyped = 0;

	// No CrowdyVisibility -> the "public" default. Derived key "publicscore".
	UPROPERTY(meta = (CrowdyModel))
	int32 PublicScore = 0;
};

/**
 * Discovery fixture for the duplicate-server-key guard: two CrowdyModel attributes that resolve to
 * the same server key "ammo" (one derived, one via a CrowdyKey override). DiscoverForClass keeps the first it
 * sees and drops the second with an error, so exactly one "ammo" attribute survives.
 */
UCLASS(meta = (CrowdyContainer = "DupKeyHero", CrowdyContainerTest))
class UCrowdyGameModelDupKeyTarget : public UObject
{
	GENERATED_BODY()

public:
	// Derives the server key "ammo".
	UPROPERTY(meta = (CrowdyModel))
	int32 Ammo = 0;

	// Overrides to "ammo" -> collides with Ammo; the guard drops one of the two with an error.
	UPROPERTY(meta = (CrowdyModel, CrowdyKey = "ammo"))
	int32 Reserve = 0;
};

/**
 * Plain native structs for the "object" value-type fixture. FCrowdyGameModelTestStats has a scalar, a nested
 * struct, and a scalar-array member (all in scope), so it maps to "object"; FCrowdyGameModelTestRefHolder carries
 * an object-reference member, so it is out of scope (an opaque server object cannot hold a UObject pointer).
 */
USTRUCT()
struct FCrowdyGameModelTestVec2
{
	GENERATED_BODY()

	UPROPERTY()
	float X = 0.0f;

	UPROPERTY()
	float Y = 0.0f;
};

USTRUCT()
struct FCrowdyGameModelTestStats
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Strength = 1;

	UPROPERTY()
	float Focus = 0.5f;

	UPROPERTY()
	FString Title;

	UPROPERTY()
	FCrowdyGameModelTestVec2 Offset;

	UPROPERTY()
	TArray<int32> Ranks;
};

USTRUCT()
struct FCrowdyGameModelTestRefHolder
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<UObject> Ref;

	UPROPERTY()
	int32 Count = 0;
};

/**
 * Rich-attribute fixture: scalar arrays (int/string/float/bool), an FCrowdyModelRef container reference, and a
 * plain-struct "object", so the value codec's read/write + the array/container_ref/object discovery + the
 * apply/OnRep path can be exercised headless. Scores, Equipped, and Stats carry a counting parameterless
 * CrowdyOnRep. Positions (an array of a struct element) and BadObject (a struct with an object-ref member) are
 * deliberately unmarked, used to prove the mapper rejects an unsupported aggregate. Kept separate from the scalar
 * fixtures so its attributes never perturb their exact-count assertions; carries the CrowdyContainer tag so it
 * reads as a container class.
 */
UCLASS(meta = (CrowdyContainer = "RichHero", CrowdyContainerTest))
class UCrowdyGameModelRichTarget : public UObject
{
	GENERATED_BODY()

public:
	// Accepted "array" of int, with an OnRep so a change fires exactly once through the apply path.
	UPROPERTY(meta = (CrowdyModel, CrowdyOnRep = "OnRep_Scores"))
	TArray<int32> Scores;

	// Accepted "array" across the remaining element leaves: string, float, bool.
	UPROPERTY(meta = (CrowdyModel))
	TArray<FString> Tags;

	UPROPERTY(meta = (CrowdyModel))
	TArray<float> Weights;

	UPROPERTY(meta = (CrowdyModel))
	TArray<bool> Flags;

	// Accepted "container_ref" (server key "equipped"), with an OnRep.
	UPROPERTY(meta = (CrowdyModel, CrowdyOnRep = "OnRep_Equipped"))
	FCrowdyModelRef Equipped;

	// Accepted "object" (server key "stats"): a plain native struct with a scalar, a nested struct, and a
	// scalar-array member, with an OnRep so a change fires once through the apply path.
	UPROPERTY(meta = (CrowdyModel, CrowdyOnRep = "OnRep_Stats"))
	FCrowdyGameModelTestStats Stats;

	// Not a CrowdyModel attribute: a struct carrying an object-reference member is not a supported object.
	// Unmarked so it does not add a discovery warning; used to prove the mapper rejects it.
	UPROPERTY()
	FCrowdyGameModelTestRefHolder BadObject;

	// Not a CrowdyModel attribute: an array of a struct element is unsupported, so the mapper returns empty
	// for it. Unmarked so it does not add a discovery warning.
	UPROPERTY()
	TArray<FVector> Positions;

	int32 ScoresOnRepCount = 0;
	int32 EquippedOnRepCount = 0;
	int32 StatsOnRepCount = 0;

	UFUNCTION()
	void OnRep_Scores() { ++ScoresOnRepCount; }

	UFUNCTION()
	void OnRep_Equipped() { ++EquippedOnRepCount; }

	UFUNCTION()
	void OnRep_Stats() { ++StatsOnRepCount; }
};
