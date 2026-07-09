// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "Logging/LogMacros.h"

// Module-wide log category for CrowdyReplication (entity registry, event router, actor pool,
// trackers/managers). The RPC subsystem keeps its own LogCrowdyRPC + crowdy.rpc.trace; this
// category covers everything else. _API-exported because public headers in this module log inline
// and are instantiated by other modules across the DLL boundary.
CROWDYREPLICATION_API DECLARE_LOG_CATEGORY_EXTERN(LogCrowdyReplication, Log, All);

// Per-subsystem verbose trace gates, in the spirit of crowdy.rpc.trace. Off by default; flip the
// matching console variable to surface that area's chatty trace lines. Warnings/errors are not gated.
namespace CrowdyReplicationTrace
{
	// crowdy.entity.trace = entity registry add/remove, event routing/dispatch, entity components,
	// actor tracking and management.
	CROWDYREPLICATION_API bool Entity();

	// crowdy.pool.trace = actor pool subsystem and rendering backend churn (spawn/release/reuse).
	CROWDYREPLICATION_API bool Pool();

	// crowdy.state.trace = CrowdyState per-property replicator: owned-entity diffing, delta emission,
	// and datagram sizes.
	CROWDYREPLICATION_API bool State();
}
