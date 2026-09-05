// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "CrowdyEffectPickerContainerTypeTestFixture.generated.h"

/**
 * Headless fixture for the "known container type" picker tests (CrowdyEffectPickerOptionsTests.cpp).
 *
 * Deliberately marked meta=(CrowdyContainerTest) alongside its CrowdyContainer tag. A container type MUST be
 * excluded from every live-scan surface (the code-to-server schema sync, and this picker's own
 * KnownContainerTypeNames) the moment it carries that tag - see CrowdyEffectPickerTestFixture.h's own warning
 * about why an untagged-for-real-sync fixture is load-bearing. Do not drop CrowdyContainerTest to "make the
 * positive case easier to test"; that reintroduces the exact leak this tag exists to prevent.
 */
UCLASS(meta = (CrowdyContainer = "CrowdyEffectPickerContainerTypeFixtureType", CrowdyContainerTest))
class UCrowdyEffectPickerContainerTypeTestFixture : public UObject
{
	GENERATED_BODY()
};
