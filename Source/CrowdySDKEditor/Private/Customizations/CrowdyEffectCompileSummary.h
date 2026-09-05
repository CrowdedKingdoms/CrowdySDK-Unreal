// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

class UCrowdyEffect;

/**
 * Pure, headless-testable readouts of what an effect currently compiles to. Shared by the effect's Details panel and
 * by the compile panel docked under the authoring canvas, so the two never describe the same effect differently.
 */
namespace CrowdyEffectCompileSummary
{
	// The tuning parameters the effect's active authoring surface references but never declares, which is what the
	// "Add missing magnitudes" affordance offers to fix. Reads whichever front end the effect is actually using
	// (graph, structured, or script), the same three-way selection Compile makes: a Graph effect has an empty script,
	// so parsing the script instead would always report nothing missing.
	TArray<FString> MissingMagnitudes(const UCrowdyEffect* Effect);

	// The human-readable compile readout: the function this effect compiles to, whether it needs a Source object, any
	// undeclared tuning parameters, and the compiler diagnostics. Empty when there is no effect.
	FString BuildSummary(const UCrowdyEffect* Effect);
}
