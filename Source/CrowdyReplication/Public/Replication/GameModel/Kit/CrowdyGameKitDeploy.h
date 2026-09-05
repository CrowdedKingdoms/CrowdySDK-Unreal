#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

class UCrowdyKitLayerPreset;

/**
 * The aggregate outcome of deploying a set of kit layers from an editor surface. bOk is true only when every
 * step of the deploy (the schema seed, then each automation, then each trigger) succeeded. On failure Error
 * explains why: an emit or validation error (no network was attempted), an inability to construct the API
 * client, or the server reason for the failing deploy step. StepsCompleted and StepsTotal describe how far the
 * deploy got, and are both zero when the failure was before any network step.
 */
struct FCrowdyKitDeployOutcome
{
	bool bOk = false;
	FString Error;
	int32 StepsCompleted = 0;
	int32 StepsTotal = 0;
};

/**
 * Emit the given kit layers and deploy the resulting bundle against the Game API, then report the outcome. This
 * is the single seam an editor surface (the Studio "Deploy Kit" card) uses so it never depends on the CrowdyCPP
 * bridge: the emit, the admin-token API client, and the async completion pump all live behind this call.
 *
 * AppId scopes the emitted schema and must match the admin token's app. SessionId, when non-empty, scopes the
 * seed. GameApiUrl is the Game API GraphQL endpoint; AdminToken must carry the manage_apps permission. OnDone is
 * delivered on the game thread exactly once (even a synchronous emit failure is deferred to the next tick, never
 * re-entrant). The call owns the API client and drives its completion pump on the core ticker until OnDone
 * fires, so the caller need not tick anything.
 */
CROWDYREPLICATION_API void CrowdyKitDeployLayers(
	const TArray<TObjectPtr<UCrowdyKitLayerPreset>>& Layers,
	int64 AppId, const FString& SessionId,
	const FString& GameApiUrl, const FString& AdminToken,
	TFunction<void(FCrowdyKitDeployOutcome)> OnDone);
