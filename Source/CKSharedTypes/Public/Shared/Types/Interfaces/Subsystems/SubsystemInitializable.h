// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "SubsystemInitializable.generated.h"

// This class does not need to be modified.
UINTERFACE(NotBlueprintable)
class USubsystemInitializable : public UInterface
{
	GENERATED_BODY()
};

/**
 * 
 */
class CKSHAREDTYPES_API ISubsystemInitializable
{
	GENERATED_BODY()

	// Add interface functions to this class. This is the class that will be inherited to implement this interface.
public:

	UFUNCTION(BlueprintCallable, Category = "Subsystem Initializable")
	virtual void PostSubsystemInit();
};
