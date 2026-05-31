// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "CrowdyActorUpdateComponentInterface.generated.h"

class UCrowdyActorUpdateComponent;
// This class does not need to be modified.
UINTERFACE()
class UCrowdyActorUpdateComponentInterface : public UInterface
{
	GENERATED_BODY()
};

/**
 * 
 */
class CROWDYSDK_API ICrowdyActorUpdateComponentInterface
{
	GENERATED_BODY()

	// Add interface functions to this class. This is the class that will be inherited to implement this interface.
public:
	
	virtual UCrowdyActorUpdateComponent* GetCrowdyActorUpdateComponent() const = 0;
};
