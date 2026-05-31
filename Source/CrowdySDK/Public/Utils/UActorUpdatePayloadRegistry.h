// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Core/FCrowdyTypeID.h"
#include "Data/ActorUpdatePayloadType.h"
#include "UObject/Object.h"
#include "UActorUpdatePayloadRegistry.generated.h"

/**
 * 
 */
UCLASS()
class CROWDYSDK_API UActorUpdatePayloadRegistry : public UObject
{
	GENERATED_BODY()
	
public:
	
	static UActorUpdatePayloadRegistry* Get()
	{
		if (!Instance.load(std::memory_order_acquire))
		{
			UActorUpdatePayloadRegistry* NewRegistry = NewObject<UActorUpdatePayloadRegistry>();
			NewRegistry->AddToRoot();
			
			UActorUpdatePayloadRegistry* Expected = nullptr;
			
			if (!Instance.compare_exchange_strong(Expected, NewRegistry, std::memory_order_release))
				NewRegistry->RemoveFromRoot();
		}
		return Instance.load(std::memory_order_acquire);
	}
	
	static void Shutdown()
	{
		UActorUpdatePayloadRegistry* Current = Instance.exchange(nullptr, std::memory_order_acq_rel);
		if (Current) Current->RemoveFromRoot();
	}
	
	void LoadFromDataAsset(const UActorUpdatePayloadType* DataAsset);
	bool GetID(const UScriptStruct* Struct, FCrowdyTypeID& OutID) const;
	bool GetName(const UScriptStruct* Struct, FName& OutName) const;
	UScriptStruct* Resolve(const FCrowdyTypeID OutID) const;

	void Reset()
	{
		ensure(IsInGameThread());
		IDToStruct.Reset();
		StructPathToID.Reset();
		IDToName.Reset();
		bLoaded.store(false, std::memory_order_release);
	}
	
	void RegisterStruct(UScriptStruct* Struct, FCrowdyTypeID TypeID);

	void Seal();

	bool IsSealed() const
	{
		return bSealed.load(std::memory_order_acquire);
	}
	
    void GetAllRegisteredNames(TArray<FName>& OutNames) const;
	
private:
	
	
	TMap<FCrowdyTypeID, TObjectPtr<UScriptStruct>> IDToStruct;
	
	TMap<FName, FCrowdyTypeID> StructPathToID;
	
	TMap<FCrowdyTypeID, FName> IDToName;
	
	std::atomic<bool> bLoaded { false };
	static std::atomic<UActorUpdatePayloadRegistry*> Instance;
	std::atomic<bool> bSealed { false };
};
