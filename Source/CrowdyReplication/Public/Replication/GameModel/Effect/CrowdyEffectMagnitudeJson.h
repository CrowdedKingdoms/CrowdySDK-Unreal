// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"

/**
 * Pure conversions between a typed magnitude default and its canonical JSON serialization, factored so the
 * round-trip is unit-testable with no Slate. The canonical JSON is exactly what the effect lowering expects in
 * DefaultValueJson: a bare number for Int/Float, true/false for Bool, and a JSON-quoted string for
 * String/ContainerRef. The raw form is what a designer types (a plain number, a checkbox state, or unquoted text).
 */
namespace CrowdyEffectMagnitudeJson
{
	// A raw editor value -> canonical JSON for the given type. Empty in yields empty out (a required magnitude).
	CROWDYREPLICATION_API FString ToCanonicalJson(ECrowdyEffectValueType Type, const FString& RawInput);

	// Canonical JSON -> the raw value shown in the typed editor (strips the quotes off a string/ref, passes a
	// number/bool through). Empty in yields empty out.
	CROWDYREPLICATION_API FString FromCanonicalJson(ECrowdyEffectValueType Type, const FString& CanonicalJson);
}
