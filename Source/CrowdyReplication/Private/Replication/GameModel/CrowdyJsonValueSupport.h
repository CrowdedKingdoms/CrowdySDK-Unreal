#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/CrowdyJsonSafety.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// Parses a cached canonical value string ("87", "12.5", "true", "\"Aria\"") back to a JSON value. The JSON
// reader rejects a bare top-level scalar, so wrap it before parsing. Null when the input is empty or too
// deeply nested (a forged blob would overflow the stack on the DOM's recursive teardown) or unparseable.
inline TSharedPtr<FJsonValue> ParseCachedValue(const FString& Json)
{
	if (Json.IsEmpty() || !CrowdyJsonSafety::IsNestingWithinLimit(Json))
	{
		return nullptr;
	}
	const FString Wrapped = FString::Printf(TEXT("{\"v\":%s}"), *Json);
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Wrapped);
	TSharedPtr<FJsonObject> Object;
	if (FJsonSerializer::Deserialize(Reader, Object) && Object.IsValid())
	{
		return Object->TryGetField(TEXT("v"));
	}
	return nullptr;
}
