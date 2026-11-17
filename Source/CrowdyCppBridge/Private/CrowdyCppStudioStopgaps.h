#pragma once

#include "CoreMinimal.h"
#include "CrowdyCppClient.h"

#include <string_view>

namespace crowdy::graphql
{
	struct HttpRequest;
	struct HttpOutcome;
}

// Studio agent pieces the generated client does not provide yet, each written by hand until it does.
namespace CrowdyCppStudioStopgaps
{
	// The hand-written document for Operation in Domain, or empty when there is none.
	std::string_view Document(ECrowdyCppApiDomain Domain, std::string_view Operation);

	// The model list request for AppId on ApiEndpoint's origin under AppToken; false with OutRefusal set when it cannot be built.
	bool BuildModelsRequest(const FString& ApiEndpoint, int64 AppId, const FString& AppToken,
		crowdy::graphql::HttpRequest& OutRequest, FString& OutRefusal);

	// The model list read from an answer as untrusted input.
	FCrowdyCppStudioModelsResult MapModels(const crowdy::graphql::HttpOutcome& Out);

	// A short name for why a model list failed, safe to count: the platform's code, the HTTP status, or "transport".
	FString ModelsFailureReason(const FCrowdyCppStudioModelsResult& Result);

	// Url's scheme and authority, without its path, query or fragment; empty when it names no host.
	FString UrlOrigin(const FString& Url);

	// The largest model list body read.
	inline constexpr int32 MaxModelsBodyBytes = 256 * 1024;
}
