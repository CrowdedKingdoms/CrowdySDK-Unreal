// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "ActorUpdatePayloadType.generated.h"



USTRUCT(BlueprintType, Category = "Crowdy SDK|Data")
struct FActorUpdatePayloadTypeEntry
{
	GENERATED_BODY()
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CrowdySDK|Actor Update Payload Type", meta = (DisplayName = "ID"))
	uint8 TypeID = 0;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CrowdySDK|Actor Update Payload Type")
	FName ActorUpdateName;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CrowdySDK|Actor Update Payload Type")
	TObjectPtr<UScriptStruct> ActorUpdateType = nullptr;
};


/**
 * 
 */
UCLASS(BlueprintType, Category = "Crowdy SDK|Data")
class CROWDYSDK_API UActorUpdatePayloadType : public UDataAsset
{
	GENERATED_BODY()
	
public:
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy SDK|Actor Update Payload Type")
	TArray<FActorUpdatePayloadTypeEntry> Entries;

#if WITH_EDITOR
	
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;	

#endif
	
};
