// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Replication/GameModel/CrowdyModelRef.h"
#include "UObject/Object.h"
#include "CrowdySchemaSyncTestTarget.generated.h"

// A plain native struct default for the "object" schema-sync case: the codec encodes the CDO default with sorted
// member keys, so "Power" precedes "Rank".
USTRUCT()
struct FCrowdySchemaSyncTestStats
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Power = 3;

	UPROPERTY()
	FString Rank = TEXT("gold");
};

/**
 * Reflection fixture for the code-to-server schema sync: a CrowdyContainer class with Server Owned
 * scalar/string attributes carrying known CDO defaults, so FCrowdySchemaSync::BuildDesiredForClass +
 * PropertyDefaultToJson can be asserted headlessly (mirrors the CrowdyReplication discovery fixtures).
 * Editor test target; like the runtime discovery fixtures it is inert reflected metadata.
 */
UCLASS(meta = (CrowdyContainer = "SchemaSyncHero", CrowdyContainerTest))
class UCrowdySchemaSyncTestTarget : public UObject
{
	GENERATED_BODY()

public:
	// int, native clamp, default 75 -> key "health", valueType "int", defaultValueJson "75".
	UPROPERTY(meta = (CrowdyModel, ClampMin = "0", ClampMax = "100"))
	int32 Health = 75;

	// float, fractional default -> "float", "3.5".
	UPROPERTY(meta = (CrowdyModel))
	float Speed = 3.5f;

	// float, INTEGRAL default -> "float", "2" (integral canonicalization, so 2.0 and 2 compare equal).
	UPROPERTY(meta = (CrowdyModel))
	float Scale = 2.0f;

	// bool, default true -> "bready" (the b-prefix leaks into the lowercased key), "bool", "true".
	UPROPERTY(meta = (CrowdyModel))
	bool bReady = true;

	// string, default "Squire" -> "string", "\"Squire\"" (JSON-quoted).
	UPROPERTY(meta = (CrowdyModel))
	FString Title = TEXT("Squire");

	// CrowdyKey override + owner visibility: the "SecretScore" property is keyed "secret" with
	// read-visibility "owner", proving the override + visibility flow through BuildDesiredForClass into the
	// desired property def (Writable stays the "function" default).
	UPROPERTY(meta = (CrowdyModel, CrowdyKey = "secret", CrowdyVisibility = "owner"))
	int32 SecretScore = 0;

	// array of int, default [10,20] -> key "loadout", valueType "array", defaultValueJson "[10,20]" (the codec
	// encodes the CDO default).
	UPROPERTY(meta = (CrowdyModel))
	TArray<int32> Loadout = { 10, 20 };

	// container_ref, unset by default -> key "equipped", valueType "container_ref", no defaultValueJson (an
	// empty reference sends no default).
	UPROPERTY(meta = (CrowdyModel))
	FCrowdyModelRef Equipped;

	// object, a plain-struct default -> key "stats", valueType "object", defaultValueJson the canonical sorted
	// object {"Power":3,"Rank":"gold"}.
	UPROPERTY(meta = (CrowdyModel))
	FCrowdySchemaSyncTestStats Stats;
};

/**
 * Reflection fixture for a container that declares NO Server Owned attribute: the shape of a model whose whole
 * contribution is functions (signals, timers, automations), which are authored on effect assets rather than on the
 * class. It exists to prove such a container is still gathered, since gating the gather on attributes dropped its
 * type out of the schema entirely. Its parameterless OnSignal_ handler is the handler DispatchSignal resolves by
 * name, included so the fixture is the complete shape rather than a bare tag.
 */
UCLASS(meta = (CrowdyContainer = "SchemaSyncSignalsOnly", CrowdyContainerTest))
class UCrowdySchemaSyncSignalsOnlyTarget : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION()
	void OnSignal_BossWave() {}
};

// An app-scoped, admin-instantiable container: the values are authored capitalized to prove the read is case-insensitive.
UCLASS(meta = (CrowdyContainer = "SchemaSyncLandmark", CrowdyContainerTest, CrowdyScope = "App", CrowdyInstantiableBy = "Admin"))
class UCrowdySchemaSyncAppScopedTarget : public UObject
{
	GENERATED_BODY()
};

// Unrecognized scope and instantiableBy words: both fall back to their defaults with a warning each.
UCLASS(meta = (CrowdyContainer = "SchemaSyncBadWords", CrowdyContainerTest, CrowdyScope = "Galaxy", CrowdyInstantiableBy = "Anyone"))
class UCrowdySchemaSyncBadWordsTarget : public UObject
{
	GENERATED_BODY()
};
