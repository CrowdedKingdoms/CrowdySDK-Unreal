// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "CrowdyMinigame.generated.h"

// This class does not need to be modified.
UINTERFACE(Blueprintable, BlueprintType)
class UCrowdyMinigame : public UInterface
{
	GENERATED_BODY()
};

/**
 * 
 */
class CROWDYSDK_API ICrowdyMinigame
{
	GENERATED_BODY()

	// Add interface functions to this class. This is the class that will be inherited to implement this interface.
public:
	
	// Just a dummy function, this will probably be filled with useful information about the minigame later
	UFUNCTION(BlueprintCallable, BlueprintNativeEvent, Category = "CrowdySDK|Minigame")
	FName GetMinigameName();
	
	UFUNCTION(BlueprintCallable, BlueprintNativeEvent, Category = "CrowdySDK|Minigame")
	void JoinMinigame();
};
