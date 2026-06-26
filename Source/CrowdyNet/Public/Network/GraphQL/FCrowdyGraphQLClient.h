// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Templates/Function.h"

// A single GraphQL POST. Everything needed to build the request lives here so the
// client never has to reach into a UObject or a world.
struct FCrowdyGqlRequest
{
	FString Endpoint;
	FString BearerToken;
	FString Query;
	TSharedPtr<FJsonObject> Variables;
	float TimeoutSeconds = 10.f;
};

struct FCrowdyGqlResult
{
	// True only when the request completed with a 2xx status. HttpCode stays 0 when
	// the request never reached the server (couldn't start, or transport failed).
	bool bSuccess = false;
	int32 HttpCode = 0;
	FString RawJson;

	// The whole parsed response envelope, i.e. { "data": ..., "errors": ... } — not
	// just the inner "data" field. Null when the body was empty or unparseable.
	TSharedPtr<FJsonObject> Data;

	// "errors[].message" pulled out of the envelope; empty when the query succeeded.
	TArray<FString> Errors;

	int32 RequestBytes = 0;
};

/**
 * GameInstance-free GraphQL transport: HTTP POST + JSON + GraphQL-error parsing, and
 * nothing else. It has no UObject and no world, so the editor can call it at design
 * time and the runtime query subsystem can wrap it — one code path for both.
 */
class CROWDYNET_API FCrowdyGraphQLClient
{
public:
	// OnDone is always invoked on the game thread, exactly once.
	static void Send(const FCrowdyGqlRequest& Request, TFunction<void(FCrowdyGqlResult)> OnDone);
};
