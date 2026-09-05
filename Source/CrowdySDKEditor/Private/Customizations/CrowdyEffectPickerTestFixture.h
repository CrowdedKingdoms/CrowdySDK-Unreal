// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "CrowdyEffectPickerTestFixture.generated.h"

/**
 * Headless fixture for CrowdyEffectPickerOptionsTests.cpp: one CrowdyModel attribute per supported Game Model
 * value type. Exercises FCrowdyAttributeRegistry::DiscoverForClass, which keys off the CrowdyModel property
 * meta, not a container tag.
 *
 * Deliberately NOT tagged meta=(CrowdyContainer=...). The container tag is what
 * FCrowdySchemaSync::GatherContainerClasses collects, so a tagged fixture would be pushed to a live app as a
 * bogus container type the moment someone runs Studio's "Sync Schema from Code". DiscoverForClass needs only
 * the CrowdyModel props, so the picker tests stay valid without it. (A broader fix that also excludes the
 * pre-existing runtime test fixtures from GatherContainerClasses is tracked as a separate schema-sync
 * follow-up.) Do not re-add the tag to make it "look like a real container"; that reintroduces the leak.
 */
UCLASS()
class UCrowdyEffectPickerTestTarget : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(meta = (CrowdyModel))
	int32 Health = 100;

	UPROPERTY(meta = (CrowdyModel))
	float Speed = 1.0f;

	UPROPERTY(meta = (CrowdyModel))
	bool bStunned = false;

	UPROPERTY(meta = (CrowdyModel))
	FString Title;

	// Never a CrowdyModel attribute; proves ValueTypeForAttribute correctly misses a non-attribute property.
	UPROPERTY()
	int32 PlainInt = 0;
};
