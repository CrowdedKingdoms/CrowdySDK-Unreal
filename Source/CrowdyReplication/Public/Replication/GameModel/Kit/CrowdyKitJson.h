// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;

/**
 * A canonical JSON serializer for the GameKit preset layer.
 *
 * Unlike FJsonSerializer, this emits object members in alphabetically sorted key order, condensed with no
 * whitespace, and formats an integral number as an integer with no decimal point. Every *Json string a kit
 * emits (invokePolicyJson and friends) goes through Canonical, so the wire text is stable and matches the
 * sibling SDK byte for byte regardless of the order the fields were set.
 */
namespace CrowdyKitJson
{
	CROWDYREPLICATION_API FString Canonical(const TSharedPtr<FJsonObject>& Object);
	CROWDYREPLICATION_API FString Canonical(const TSharedPtr<FJsonValue>& Value);

	// Wrap a JSON object as a JSON value (for building array entries).
	CROWDYREPLICATION_API TSharedPtr<FJsonValue> ObjectValue(const TSharedPtr<FJsonObject>& Object);

	// Build a JSON array value from a list of objects, preserving order.
	CROWDYREPLICATION_API TSharedPtr<FJsonValue> ArrayOfObjects(const TArray<TSharedPtr<FJsonObject>>& Objects);
}
