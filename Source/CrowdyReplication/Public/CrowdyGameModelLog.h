// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "Logging/LogMacros.h"

// Log category for Game Model integration work: identity resolution, the runtime invoke client,
// container subsystem, and effect lowering (later phases). CrowdyReplication keeps LogCrowdyReplication
// for everything else and the RPC subsystem keeps its own LogCrowdyRPC + crowdy.rpc.trace; this
// category is scoped to Game Model specifically, mirroring that precedent. _API-exported because
// public headers in this module log inline and are instantiated by other modules across the DLL
// boundary.
CROWDYREPLICATION_API DECLARE_LOG_CATEGORY_EXTERN(LogCrowdyGameModel, Log, All);

// Verbose trace gate for Game Model activity, in the spirit of crowdy.rpc.trace / crowdy.entity.trace.
// Off by default; flip crowdy.gamemodel.trace to surface identity/invoke/container chatter. Warnings
// and errors are never gated.
namespace CrowdyGameModelTrace
{
	// crowdy.gamemodel.trace = identity resolution, invoke client, container cache/diffing, and
	// effect-lowering trace lines (later phases).
	CROWDYREPLICATION_API bool GameModel();
}
