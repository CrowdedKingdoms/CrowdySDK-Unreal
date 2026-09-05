// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

class FProperty;
class FJsonValue;

/**
 * The two-way codec between a Server Owned UPROPERTY and its server JSON value.
 *
 * The write direction (server JSON -> live member) is centralized here for every supported type: the scalar
 * leaves (int/float/bool/string/enum) and the aggregates - a scalar array -> the server "array" value type, an
 * FCrowdyModelRef -> "container_ref", and a plain native struct -> "object". The subsystem apply path calls
 * DecodeJsonToProperty and no longer hand-writes scalars, so an array element (or an object member) is written
 * exactly like a top-level scalar of the same leaf. The CDO-default read direction handles the aggregates only
 * (EncodePropertyDefaultToJson); scalar defaults keep their module-local canonicalization in the schema sync.
 *
 * Decoding treats every server value as forged. An array is bounded by MaxArrayElements (a longer value is
 * rejected whole, never truncated) and every element must strictly match the destination leaf type (a
 * mismatch, including a nested array or object, rejects the whole value). An object is decoded into a temporary
 * struct and committed only on full success, so a missing member, a wrong-typed member, or nesting past
 * MaxStructDepth rejects the whole value and never leaves a live TArray or struct half-written. An "object" is a
 * plain NATIVE struct (a Blueprint user struct's member names differ between editor and cooked, so its keys
 * would not round-trip) whose every member is itself in scope: a scalar leaf, a scalar array, or a nested plain
 * native struct. An object-reference member, a map/set, an array of structs, or an FCrowdyModelRef member all
 * take the struct out of scope.
 */
struct CROWDYREPLICATION_API FCrowdyModelValueCodec
{
	// The most elements decoded into a live array from one server value. A longer array is rejected whole (the
	// property is left unchanged) rather than silently truncated. A Server Owned array models a small fixed list
	// (tags, a loadout); large collections use the container graph, not an attribute array.
	static constexpr int32 MaxArrayElements = 4096;

	// The deepest nesting a plain-struct "object" attribute may reach (the root object is depth 0, a struct
	// member is depth 1, and so on). A struct type nested deeper is not classified as an object, and a decode
	// that would recurse past this bound rejects the whole value - a defensive cap on a forged/pathological type,
	// well beyond the depth a real game-data struct (a stats block, an FTransform) needs.
	static constexpr int32 MaxStructDepth = 4;

	// True when Struct is FCrowdyModelRef, the only struct that maps to the container_ref value type.
	static bool IsModelRefStruct(const UStruct* Struct);

	// True when Inner is a supported array element leaf: a numeric width, an enum, a bool, or an FString (the
	// same leaves MapPropertyToValueType accepts as a top-level scalar).
	static bool IsSupportedArrayInner(const FProperty* Inner);

	// Classifies the aggregate Server Owned types: an FArrayProperty of a supported leaf -> "array"; an
	// FStructProperty of FCrowdyModelRef -> "container_ref"; a plain native struct whose every member is in
	// scope -> "object"; empty for anything else. Composed into FCrowdyAttributeRegistry::MapPropertyToValueType,
	// which owns the scalar leaves.
	static FString MapAggregatePropertyToValueType(const FProperty* Property);

	// Writes a decoded server JSON value onto a live property: a scalar leaf, a scalar array, a container_ref, or
	// a plain-struct object. Returns true when a value was actually written; false (property left unchanged) when
	// the value is invalid, the type is unsupported, or the value fails the forged-input bounds. The caller uses
	// the result to keep its diff cache and change notifications in step with the member: a rejected value must
	// notify nothing. ValueAddr is the address of the property value inside its container.
	static bool DecodeJsonToProperty(const FProperty* Property, void* ValueAddr, const TSharedPtr<FJsonValue>& Value);

	// Encodes a live aggregate property value (array, container_ref, or plain-struct object) to canonical compact
	// JSON text for a container type's CDO default. Object members are emitted in sorted key order so the default
	// matches the schema sync's canonicalization. Returns false (OutJson untouched) for a scalar leaf (the schema
	// sync owns those) or an unsupported type, and for an empty container ref (an unset reference sends no default).
	static bool EncodePropertyDefaultToJson(const FProperty* Property, const void* ValueAddr, FString& OutJson);
};
