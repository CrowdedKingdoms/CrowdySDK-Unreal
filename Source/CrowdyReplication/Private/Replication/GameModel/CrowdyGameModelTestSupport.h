#pragma once

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// Parses a JSON object literal for a test, returning null on malformed input.
inline TSharedPtr<FJsonObject> ParseObject(const FString& Json)
{
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	TSharedPtr<FJsonObject> Obj;
	FJsonSerializer::Deserialize(Reader, Obj);
	return Obj;
}

#endif
