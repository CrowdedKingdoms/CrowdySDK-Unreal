// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "Logging/LogMacros.h"

// Module-wide log category for CrowdyServices (auth, teams, avatars, persistence, host, HUD).
// _API-exported so inline logging in public headers links across module boundaries.
CROWDYSERVICES_API DECLARE_LOG_CATEGORY_EXTERN(LogCrowdyServices, Log, All);

// Per-subsystem verbose trace gates, in the spirit of crowdy.rpc.trace. Off by default; flip the
// matching console variable to surface that area's chatty trace lines. Warnings/errors are not gated.
namespace CrowdyServicesTrace
{
	// crowdy.services.trace — authentication, teams, avatars, persistence, host subsystem,
	// utilities, Blueprint reception layer.
	CROWDYSERVICES_API bool Services();

	// crowdy.hud.trace — HUD base and HUD widgets.
	CROWDYSERVICES_API bool Hud();
}
