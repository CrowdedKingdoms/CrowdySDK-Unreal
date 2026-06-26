// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "Logging/LogMacros.h"

// Module-wide log category for CrowdyNet. Replaces the generic LogTemp this module used to log
// under, so its networking output can be filtered and silenced on its own (e.g. in console:
// `Log LogCrowdyNet Verbose`, or in DefaultEngine.ini under [Core.Log]).
CROWDYNET_API DECLARE_LOG_CATEGORY_EXTERN(LogCrowdyNet, Log, All);

// Per-subsystem verbose trace gates, in the spirit of crowdy.rpc.trace. Each returns whether its
// console variable is set; they are off by default. Flip one (e.g. `crowdy.net.trace 1`) to
// surface the chatty, high-frequency trace lines for that area. Genuine warnings and errors are
// NOT gated — they always log under LogCrowdyNet so problems stay visible.
namespace CrowdyNetTrace
{
	// crowdy.net.trace — UDP transport: socket open/close, sends, receives, worker threads, buffers.
	CROWDYNET_API bool Net();

	// crowdy.query.trace — GraphQL: query dispatch/response and live subscription lifecycle.
	CROWDYNET_API bool Query();

	// crowdy.serialize.trace — payload serialization: message encode/decode and registry resolves.
	CROWDYNET_API bool Serialize();
}
