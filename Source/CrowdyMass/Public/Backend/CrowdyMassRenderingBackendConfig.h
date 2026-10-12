// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Data/CrowdyRenderingBackendConfig.h"
#include "CrowdyMassRenderingBackendConfig.generated.h"

class UMassEntityConfigAsset;

/**
 * One entity class and the Mass Entity Config that should build its rows.
 *
 * Keyed by the resolved entity CLASS rather than by the wire struct it sends, for two reasons. The
 * backend is already handed the resolved class at activation, so nothing new has to be threaded
 * anywhere; and two classes are free to share one wire struct now that class identity travels on
 * every update, which is exactly the case a struct key could not tell apart.
 */
USTRUCT(BlueprintType)
struct FCrowdyMassEntityConfigBinding
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Mass Backend")
	TSoftClassPtr<AActor> EntityClass;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Mass Backend")
	TSoftObjectPtr<UMassEntityConfigAsset> EntityConfig;
};

/**
 * Configuration for UCrowdyMassRenderingBackend.
 *
 * The Mass backend renders every remote Crowdy entity as a lightweight Mass entity.
 * By default it inherits the Remote Entity Config and Default Smoothing Duration from
 * Project Settings > Plugins > Crowdy Mass. The overrides below let a specific map
 * profile use a different Mass Entity Config or smoothing without touching the
 * project-wide settings; leave them at their defaults to fall back to those settings.
 */
UCLASS(Blueprintable, BlueprintType, EditInlineNew, DefaultToInstanced,
	meta=(DisplayName="Crowdy Mass Backend Config"))
class CROWDYMASS_API UCrowdyMassRenderingBackendConfig : public UCrowdyRenderingBackendConfig
{
	GENERATED_BODY()

public:

	/**
	 * Optional override for the Mass Entity Config used when this backend spawns remote
	 * entities. Leave unset to use the project-wide Remote Entity Config from
	 * Project Settings > Plugins > Crowdy Mass. Must include the Crowdy Remote Entity trait.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Mass Backend")
	TSoftObjectPtr<UMassEntityConfigAsset> RemoteEntityConfigOverride;

	/**
	 * Per-class Mass Entity Configs. An entity whose resolved class appears here is built from the
	 * config named beside it; everything else falls back to Remote Entity Config Override above, and
	 * then to the project-wide setting. Leave empty for one config serving every remote entity, which
	 * is the behaviour that existed before this list.
	 *
	 * This is what lets two kinds of crowd entity look different. It lives here rather than in project
	 * settings because the backend config is Instanced, so a Mass-only list cannot appear at all for a
	 * project using a different rendering backend.
	 *
	 * Flattened once when the backend configures, so a spawn is a single pointer-keyed lookup and an
	 * update is none.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Mass Backend")
	TArray<FCrowdyMassEntityConfigBinding> PerClassEntityConfigs;

	/**
	 * Optional override for interpolation smoothing duration (seconds). A negative value
	 * means "use the project-wide Default Smoothing Duration" from the Crowdy Mass settings.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Mass Backend",
		meta=(ClampMin="-1.0", UIMin="0.0", Units="s"))
	float SmoothingDurationOverride = -1.0f;

	/**
	 * Debug aid. When true the backend ALSO spawns a Mass entity for the locally-owned entity
	 * (your own avatar). Normally the owner is skipped because it already has its real pawn, so
	 * with this on you will see your real pawn AND an overlapping Mass duplicate. Leave this off
	 * for shipping; use it to validate Mass visualization with a single client (no second player).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Mass Backend|Debug")
	bool bRenderLocalOwner = false;
};
