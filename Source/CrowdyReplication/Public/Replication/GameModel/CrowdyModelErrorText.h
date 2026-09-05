// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

struct FCrowdyInvokeResult;

/**
 * Shared, pure text for Game Model action failures, so every Failed path and the "Get Last Crowdy Model Error"
 * node phrase a failure the same way. Centralizing the wording here keeps a denial, a guard failure, and a
 * server/transport error reading consistently whichever node produced it, and gives headless tests a pure seam
 * to prove a failure is never surfaced as an empty string.
 */
namespace CrowdyModelErrorText
{
	// The message for a Game Model action that could not find its world subsystem (invoked outside a running
	// session / play world). Stable and user-facing.
	CROWDYREPLICATION_API FString SubsystemUnavailable();

	// The best human-readable message for a finished invoke/effect result: the server or transport error verbatim
	// when the result carries one, otherwise a phrasing that still distinguishes a rolled-back call (reached the
	// server, logic/authority rejected it) from a transport failure (never reached the server). Never returns an
	// empty string for a non-success result.
	CROWDYREPLICATION_API FString FromInvokeResult(const FCrowdyInvokeResult& Result);
}
