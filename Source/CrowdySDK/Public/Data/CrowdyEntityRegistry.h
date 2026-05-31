// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Replication/ObjectHandler/CrowdyEntityEventHandler.h"
#include "CrowdyEntityRegistry.generated.h"



USTRUCT(BlueprintType)
struct FCrowdyEntityTypeEntry
{
	GENERATED_BODY()

	/** Numeric type ID — this is what travels over the wire. Compact and fast. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly)
	int32 TypeID = 0;

	/** Human-readable name for debugging */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly)
	FName DebugTypeName;

	/** The actor class to spawn for this object type */
	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	TSoftClassPtr<AActor> EntityActorClass;

	/** Handler that knows how to apply state to this object type */
	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	TSoftClassPtr<UCrowdyEntityEventHandler> HandlerClass;
};


/**
 * 
 */
UCLASS(BlueprintType, meta=(DisplayName="Crowdy Object Registry"))
class CROWDYSDK_API UCrowdyEntityRegistry : public UDataAsset
{
	GENERATED_BODY()
	
public:
	
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Crowdy SDK|Object Registry")
	TArray<FCrowdyEntityTypeEntry> EntityTypes;
	
	const FCrowdyEntityTypeEntry* FindByTypeID(const int32 TypeID) const
	{
		for (const FCrowdyEntityTypeEntry& Entry : EntityTypes)
		{
			if (Entry.TypeID == TypeID)
			{
				return &Entry;
			}
		}
		
		return nullptr;
	};
	
	const FCrowdyEntityTypeEntry* FindByClass(TSubclassOf<AActor> Class) const
	{
		for (const FCrowdyEntityTypeEntry& Entry : EntityTypes)
		{
			if (Entry.EntityActorClass.Get() == Class)
			{
				return &Entry;
			}
		}
		
		return nullptr;
	};
	
	const FCrowdyEntityTypeEntry* FindByEventType(const TObjectPtr<UScriptStruct> EventType) const
	{
		for (const FCrowdyEntityTypeEntry& Entry : EntityTypes)
		{
			if (Entry.HandlerClass.Get() == EventType.GetClass())
			{
				return &Entry;
			}
		}
		return nullptr;
	}
	
	
};
