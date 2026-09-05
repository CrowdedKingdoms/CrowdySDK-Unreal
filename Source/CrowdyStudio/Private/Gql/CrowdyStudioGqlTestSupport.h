#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

/**
 * Shared fixtures for the CrowdyStudio GraphQL parser tests.
 *
 * These live in one header rather than in each test file's anonymous namespace because adaptive unity merges
 * several test files into one translation unit, where two anonymous-namespace helpers of the same name are a
 * redefinition. The merge is not deterministic, so the same duplicate can build clean incrementally and fail
 * from a fresh checkout.
 */
namespace CrowdyStudioGqlTest
{
	/** Parse a full response envelope ({ data: ... }) from text, the shape SendGame hands the parsers. */
	inline TSharedPtr<FJsonObject> ParseEnvelope(const FString& JsonText)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		FJsonSerializer::Deserialize(Reader, Object);
		return Object;
	}
}
