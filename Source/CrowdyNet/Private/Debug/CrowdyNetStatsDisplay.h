// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

// Registers `stat crowdyudp` / `stat crowdyquery` engine-stat overlays that draw the live
// FUDPNetworkStatistics / FQueryStats in the viewport, exactly like the engine's own
// `stat unit` / `stat fps`. Dev/debug/editor only compiled out of Shipping builds.
#if !UE_BUILD_SHIPPING

namespace CrowdyNetStats
{
	/** Register the engine stat commands. Requires GEngine to be valid (call after engine init). */
	void Register();

	/** Remove the engine stat commands. Safe to call even if Register() never ran. */
	void Unregister();
}

#endif // !UE_BUILD_SHIPPING
