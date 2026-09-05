// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "CrowdyEffectGraphTestContainer.generated.h"

/**
 * Minimal container fixture for the graph-asset compile tests: a CrowdyContainer-tagged UObject with one Server
 * Owned int attribute (health, native clamp [0,100]), so UCrowdyEffect::Compile() can discover a type name and an
 * attribute in the editor. Lives in this editor module so the graph tests, which build the editor node classes, can
 * also drive a real UCrowdyEffect::Compile() without reaching into another module's private test targets.
 */
UCLASS(meta = (CrowdyContainer = "GraphAssetTestHero", CrowdyContainerTest))
class UCrowdyEffectGraphTestContainer : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(meta = (CrowdyModel, ClampMin = "0", ClampMax = "100"))
	int32 Health = 100;
};
