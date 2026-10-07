#pragma once

#include "CoreMinimal.h"

class FProperty;
class UCrowdyServerObjectDefinition;
class UScriptStruct;
struct FInstancedPropertyBag;
struct FInstancedStruct;

/** MessagePack encoding of a definition's structs, as its Server Object's server code expects them. Game thread only. */
namespace CrowdyExec
{
	/** Largest message either side sends. */
	constexpr int32 MaxPayloadBytes = 1024 * 1024;

	/**
	 * Encodes Value, an instance of Struct, as a map of its fields. A null Struct encodes an empty map. OnlyFields, when
	 * not empty, limits the map to the fields with those server names.
	 */
	CROWDYEXEC_API bool Encode(const UCrowdyServerObjectDefinition& Definition, const UScriptStruct* Struct, const void* Value,
		TArray<uint8>& OutBytes, FString& OutError, TConstArrayView<FString> OnlyFields = {});

	/** Decodes Bytes into Value, an instance of Struct. Missing fields take Defaults' values, or Struct's starting values when it is null; on failure Value is untouched. */
	CROWDYEXEC_API bool Decode(const UCrowdyServerObjectDefinition& Definition, const UScriptStruct& Struct, TConstArrayView<uint8> Bytes,
		void* Value, FString& OutError, const void* Defaults = nullptr);

	/** Drops every resolved field table, so the next use re-finds each field. Call after a struct's fields are rebuilt. */
	CROWDYEXEC_API void NotifyStructsChanged();

	/** A List's values (a State, reply or params of a List), copied into a List to read by name. Empty for a struct of your own. */
	CROWDYEXEC_API FInstancedPropertyBag ToList(const FInstancedStruct& Values);

#if WITH_DEV_AUTOMATION_TESTS
	CROWDYEXEC_API const FProperty* FindResolvedPropertyForTest(const UCrowdyServerObjectDefinition& Definition, const UScriptStruct& Struct,
		const FString& ServerName);
#endif
}
