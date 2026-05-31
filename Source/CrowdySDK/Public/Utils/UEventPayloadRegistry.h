#pragma once
#include "Core/FCrowdyTypeID.h"
#include "Data/EventPayloadType.h"
#include "UEventPayloadRegistry.generated.h"

UCLASS()
class CROWDYSDK_API UEventPayloadRegistry : public UObject
{
	GENERATED_BODY()

public:
	
	static UEventPayloadRegistry* Get()
	{
		if (!Instance.load(std::memory_order_acquire))
		{
			UEventPayloadRegistry* NewRegistry = NewObject<UEventPayloadRegistry>();
			NewRegistry->AddToRoot();

			UEventPayloadRegistry* Expected = nullptr;
			if (!Instance.compare_exchange_strong(Expected, NewRegistry, std::memory_order_release))
				NewRegistry->RemoveFromRoot();
		}
		return Instance.load(std::memory_order_acquire);
	}
	
	static void Shutdown()
	{
		UEventPayloadRegistry* Current = Instance.exchange(nullptr, std::memory_order_acq_rel);
		if (Current) Current->RemoveFromRoot();
	}
	
	void LoadFromDataAsset(const UEventPayloadType* DataAsset);
	bool GetID(const UScriptStruct* Struct, FCrowdyTypeID& OutID) const;
	bool GetName(const FCrowdyTypeID ID, FName& OutName) const;
	UScriptStruct* Resolve(const FCrowdyTypeID ID) const;
	
	bool IsLoaded() const { return bLoaded.load(std::memory_order_acquire); }
	
	void Reset()
	{
		ensure(IsInGameThread());
		IDToStruct.Reset();
		StructPathToID.Reset();
		IDToName.Reset();
		bLoaded.store(false, std::memory_order_release);
	}
	
	// Auto-registration path — called by UCrowdyAutoRegistry.
	// Safe to call multiple times; duplicates are silently ignored.
	void RegisterStruct(UScriptStruct* Struct, FCrowdyTypeID TypeID);
	
	// Called after all structs are registered.
	// Switches the registry to read-only mode.
	// Worker threads may read after this point — no lock needed.
	void Seal();

	bool IsSealed() const
	{
		return bSealed.load(std::memory_order_acquire);
	}
	
	void GetAllRegisteredNames(TArray<const UScriptStruct*> RegisteredEvents ,TArray<FName>& OutNames) const;
	
private:
	
	TMap<FCrowdyTypeID, TObjectPtr<UScriptStruct>> IDToStruct;
	
	TMap<FName, FCrowdyTypeID> StructPathToID;
	
	TMap<FCrowdyTypeID, FName> IDToName;
	
	std::atomic<bool> bLoaded { false };
	static std::atomic<UEventPayloadRegistry*> Instance;
	std::atomic<bool> bSealed { false };
};

