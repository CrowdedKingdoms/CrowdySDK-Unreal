// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "CrowdyPlayer.generated.h"

// This class does not need to be modified.
UINTERFACE(Blueprintable, BlueprintType)
class UCrowdyPlayer : public UInterface
{
	GENERATED_BODY()
};

/**
 * 
 */
class CROWDYSDK_API ICrowdyPlayer
{
	GENERATED_BODY()

	// Add interface functions to this class. This is the class that will be inherited to implement this interface.
public:
	
	// Just dummy function, this will probably be filled with useful information about the player later
	UFUNCTION(BlueprintCallable, BlueprintNativeEvent, Category = "CrowdySDK|Player")
	FName GetPlayerName();
	
	UFUNCTION(BlueprintCallable, BlueprintNativeEvent, Category = "CrowdySDK|Player")
	void InteractMessage(const FString& Message, const float Duration);
};
