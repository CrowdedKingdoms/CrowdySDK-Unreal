#pragma once

#include "CoreMinimal.h"
#include "CrowdyCppClient.h"

#include <string>

namespace crowdy::graphql
{
	struct HttpOutcome;
}

// The SDK's own bounds around CrowdyCPP's Studio model list.
namespace CrowdyCppStudioModels
{
	// Why the model list for AppId may not be asked on ApiEndpoint's origin under AppToken, or empty when it may.
	FString ModelsRefusal(const FString& ApiEndpoint, int64 AppId, const FString& AppToken);

	// The model list's route on the API origin, for AppId.
	std::string ModelsPath(int64 AppId);

	// The model list read from an answer as untrusted input.
	FCrowdyCppStudioModelsResult MapModels(const crowdy::graphql::HttpOutcome& Out);

	// A short name for why a model list failed, safe to count: the platform's code, the HTTP status, or "transport".
	FString ModelsFailureReason(const FCrowdyCppStudioModelsResult& Result);

	// Url's scheme and authority, without its path, query or fragment; empty when it names no host.
	FString UrlOrigin(const FString& Url);

	// The largest model list body read.
	inline constexpr int32 MaxModelsBodyBytes = 256 * 1024;
}
