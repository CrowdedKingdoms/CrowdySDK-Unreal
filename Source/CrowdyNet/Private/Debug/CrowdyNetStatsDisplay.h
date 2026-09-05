// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

// Registers the `stat crowdyudp` engine-stat overlay that draws the live FUDPNetworkStatistics
// in the viewport, exactly like the engine's own `stat unit` / `stat fps`. Dev/debug/editor only
// compiled out of Shipping builds.
#if !UE_BUILD_SHIPPING

namespace CrowdyNetStats
{
	/** Register the engine stat command. Requires GEngine to be valid (call after engine init). */
	void Register();

	/** Remove the engine stat command. Safe to call even if Register() never ran. */
	void Unregister();
}

#endif // !UE_BUILD_SHIPPING
