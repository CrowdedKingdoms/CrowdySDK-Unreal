// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "CrowdyRenderingBackendConfig.generated.h"

/**
 * Base class for backend-specific configuration.
 * Subclass this alongside your UCrowdyRenderingBackend subclass and
 * add whatever properties your backend needs (actor class, pool size, etc.).
 */
UCLASS(Abstract, Blueprintable, BlueprintType, EditInlineNew, DefaultToInstanced,
	meta=(DisplayName="Crowdy Rendering Backend Config"))
class CROWDYREPLICATION_API UCrowdyRenderingBackendConfig : public UObject
{
	GENERATED_BODY()
};
