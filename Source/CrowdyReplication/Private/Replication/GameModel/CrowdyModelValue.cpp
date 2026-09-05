// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/CrowdyModelValue.h"

#include "Replication/GameModel/CrowdyNumericSupport.h"
#include "Serialization/CrowdyJsonSafety.h" // bound a forged value before parsing it
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	// Parses a lone serialized JSON value ("5", "12.5", "true", "\"text\"") back to a JSON value. The reader rejects
	// a bare top-level scalar, so wrap it in an object first (mirroring UCrowdyGameModel's cached-value parse). Null
	// when the input is empty, too deeply nested (a forged blob would overflow the stack on the DOM's recursive
	// teardown), or unparseable.
	TSharedPtr<FJsonValue> ParseModelValue(const FString& ValueJson)
	{
		if (ValueJson.IsEmpty() || !CrowdyJsonSafety::IsNestingWithinLimit(ValueJson))
		{
			return nullptr;
		}
		const FString Wrapped = FString::Printf(TEXT("{\"v\":%s}"), *ValueJson);
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Wrapped);
		TSharedPtr<FJsonObject> Object;
		if (FJsonSerializer::Deserialize(Reader, Object) && Object.IsValid())
		{
			return Object->TryGetField(TEXT("v"));
		}
		return nullptr;
	}
}

int32 UCrowdyModelValue::AsInt(const FString& ValueJson, int32 Default)
{
	const TSharedPtr<FJsonValue> Value = ParseModelValue(ValueJson);
	return (Value.IsValid() && Value->Type == EJson::Number) ? ClampDoubleToInt32(Value->AsNumber()) : Default;
}

float UCrowdyModelValue::AsFloat(const FString& ValueJson, float Default)
{
	return static_cast<float>(AsDouble(ValueJson, Default));
}

double UCrowdyModelValue::AsDouble(const FString& ValueJson, double Default)
{
	const TSharedPtr<FJsonValue> Value = ParseModelValue(ValueJson);
	return (Value.IsValid() && Value->Type == EJson::Number) ? Value->AsNumber() : Default;
}

bool UCrowdyModelValue::AsBool(const FString& ValueJson, bool bDefault)
{
	const TSharedPtr<FJsonValue> Value = ParseModelValue(ValueJson);
	return (Value.IsValid() && Value->Type == EJson::Boolean) ? Value->AsBool() : bDefault;
}

FString UCrowdyModelValue::AsString(const FString& ValueJson, const FString& Default)
{
	const TSharedPtr<FJsonValue> Value = ParseModelValue(ValueJson);
	return (Value.IsValid() && Value->Type == EJson::String) ? Value->AsString() : Default;
}
