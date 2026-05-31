// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Core/FCrowdyTypeID.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "CrowdyAutoRegistry.generated.h"

/**
 * 
 */
UCLASS()
class CROWDYSDK_API UCrowdyAutoRegistry : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	TArray<UScriptStruct*> GetAllRegisteredGameEvents();

private:
	void ScanAndRegisterAllStructs();
	void MergeDataAssetOverrides();        // backward compat — existing data assets still work
	void ProcessStruct(UScriptStruct* Struct);

	FCrowdyTypeID ResolveTypeID(const UScriptStruct* Struct) const;
	
	UPROPERTY()
	TArray<UScriptStruct*> AllRegisteredGameEvents;
};
