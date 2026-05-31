// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "EventPayloadType.generated.h"


USTRUCT(BlueprintType, Category="Crowdy SDK|Data")
struct FEventPayloadTypeEntry
{
	GENERATED_BODY()
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Event Payload Type", meta=(ClampMin=50, ClampMax=65535, DisplayName="ID"))
	int32 TypeID = 50;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Event Payload Type")
	FName EventName;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Event Payload Type")
	TObjectPtr<UScriptStruct> EventType = nullptr;
	
};

/**
 * 
 */
UCLASS(BlueprintType)
class CROWDYSDK_API UEventPayloadType : public UDataAsset
{
	GENERATED_BODY()
	
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Event Payload Type")
	TArray<FEventPayloadTypeEntry> Entries;
	
	TArray<FEventPayloadTypeEntry> GetAllEntries() const;
	
	UEventPayloadType();
	
#if WITH_EDITOR
	
	virtual void PostLoad() override;	
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;

#endif
private:
	
	UPROPERTY(VisibleAnywhere, Category="Crowdy SDK|Event Payload Type|Internal")
	TArray<FEventPayloadTypeEntry> InternalEntries;
	
	void BuildInternalEntries();
	void SanitizeCustomEntries();
};
