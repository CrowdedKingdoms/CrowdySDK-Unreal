#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "CrowdyGameKitConfig.generated.h"

class UCrowdyKitLayerPreset;
class FDataValidationContext;

/**
 * A deployable GameKit configuration: a list of genre layer presets plus an optional
 * session scope. Editor validation runs the same emit the deploy step uses (through
 * the bridge), so a cross-genre name collision, a Guild layer with no group id, or a
 * Living World layer with every section disabled is reported at author time rather
 * than surfacing only when the kit is deployed.
 *
 * The emit is pure and synchronous; deploying the emitted bundle is a separate async
 * step (a later slice). The bridge type stays out of this header so the editor and
 * Studio can reference the config without the bridge dependency.
 */
UCLASS(BlueprintType)
class CROWDYREPLICATION_API UCrowdyGameKitConfig : public UDataAsset
{
	GENERATED_BODY()

public:
	/** The genre layers to deploy. All layers are merged in one pass so a name defined by two layers is rejected. */
	UPROPERTY(EditAnywhere, Instanced, Category = "Crowdy SDK|Game Kit")
	TArray<TObjectPtr<UCrowdyKitLayerPreset>> Layers;

	/** An optional session id that scopes the seeded starter containers; empty seeds at app scope. */
	UPROPERTY(EditAnywhere, Category = "Crowdy SDK|Game Kit")
	FString SessionId;

#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(FDataValidationContext& ValidationContext) const override;
#endif
};
