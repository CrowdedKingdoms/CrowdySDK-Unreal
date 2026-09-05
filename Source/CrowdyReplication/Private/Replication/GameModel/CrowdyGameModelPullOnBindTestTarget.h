// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "CrowdyGameModelPullOnBindTestTarget.generated.h"

/**
 * Container fixtures for the pull-on-start setting. Pull on start is a property of the container type, carried by
 * meta=(CrowdyPullOnStart) on the class, so these cover the four shapes the resolution has to tell apart: a
 * container that never opts out, one that does, a subclass that inherits the opt-out without restating it, and a
 * subclass that opts back in. CrowdyContainerTest keeps every one of them out of the real schema sync and the
 * baked registry.
 */

/** A container that declares no pull-on-start tag at all, so it takes the default and pulls. */
UCLASS(meta = (CrowdyContainer = "TestPullOn", CrowdyContainerTest))
class UCrowdyGameModelPullOnTarget : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(meta = (CrowdyModel))
	int32 Score = 0;
};

/** A container whose author turned the pull off, the only value the tag is ever written with. */
UCLASS(meta = (CrowdyContainer = "TestPullOff", CrowdyContainerTest, CrowdyPullOnStart = "False"))
class UCrowdyGameModelPullOffTarget : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(meta = (CrowdyModel))
	int32 Score = 0;
};

/**
 * A container subclass that restates nothing. Class metadata is not inherited, so this class carries no tag of its
 * own and only follows its base's opt-out if the resolution walks the super chain.
 */
UCLASS(meta = (CrowdyContainer = "TestPullOffChild", CrowdyContainerTest))
class UCrowdyGameModelPullInheritedTarget : public UCrowdyGameModelPullOffTarget
{
	GENERATED_BODY()
};

/** A container subclass that opts back in to the pull its base turned off. */
UCLASS(meta = (CrowdyContainer = "TestPullReenabled", CrowdyContainerTest, CrowdyPullOnStart = "True"))
class UCrowdyGameModelPullReenabledTarget : public UCrowdyGameModelPullOffTarget
{
	GENERATED_BODY()
};

/**
 * A subclass of an opted-out container that is NOT itself tagged as a container, so it is not a container type at
 * all. It pulls, because the setting only exists for container types; without that gate it would inherit an
 * opt-out from a base it has nothing else to do with.
 */
UCLASS()
class UCrowdyGameModelPullUntaggedSubclass : public UCrowdyGameModelPullOffTarget
{
	GENERATED_BODY()
};
