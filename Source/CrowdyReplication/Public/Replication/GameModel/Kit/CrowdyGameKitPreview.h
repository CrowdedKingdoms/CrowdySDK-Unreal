#pragma once

#include "CoreMinimal.h"

class UCrowdyKitLayerPreset;

/**
 * The reflection-friendly result of emitting a set of kit layers: whether they form a deployable bundle and, if
 * so, the artifact counts to preview. It deliberately carries no bridge type, so an editor or authoring surface
 * can validate and preview a kit without depending on the CrowdyCPP bridge (a private dependency). The full
 * deployable bundle stays inside the bridge boundary; only this summary crosses out.
 */
struct FCrowdyKitPreview
{
	bool bOk = false;
	FString Error;
	int32 NumContainerTypes = 0;
	int32 NumPropertyDefs = 0;
	int32 NumFunctions = 0;
	int32 NumAutomations = 0;
};

/**
 * Emit the given kit layers and return a preview summary. Runs the same pure emit the deploy path uses (so a
 * failure here is a real deploy blocker), but returns only counts and an error message, never the bundle. Null
 * layers are skipped. AppId is used only to stamp the emitted JSON and does not affect validity, so a caller
 * validating in the editor may pass any non-zero placeholder.
 */
CROWDYREPLICATION_API FCrowdyKitPreview CrowdyKitPreviewLayers(
	const TArray<TObjectPtr<UCrowdyKitLayerPreset>>& Layers, int64 AppId, const FString& SessionId);

/**
 * Emit the given kit layers and return the exact container-type names and function names the deploy would seed.
 * Runs the same pure emit the deploy path uses, then reads the names out of the emitted seed (containerTypes[].typeName
 * and functions[].name), so no bridge type crosses out. This is the source of truth for prune-protection: a schema
 * sync can protect kit-deployed schema by exact name, which is the only way to recognize a kit whose type prefix is
 * empty (its bare "Combatant"/"attack" names cannot be told from hand-authored schema by a prefix rule).
 *
 * Returns false (and leaves both arrays empty) when the emit fails or the seed cannot be read; the names are appended
 * uniquely. Null layers are skipped. AppId only stamps the emitted JSON, so a caller may pass any non-zero placeholder.
 */
CROWDYREPLICATION_API bool CrowdyKitDeployedNames(
	const TArray<TObjectPtr<UCrowdyKitLayerPreset>>& Layers, int64 AppId, const FString& SessionId,
	TArray<FString>& OutTypeNames, TArray<FString>& OutFunctionNames);
