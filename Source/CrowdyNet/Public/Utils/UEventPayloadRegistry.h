#pragma once
#include "Core/FCrowdyTypeID.h"
#include "Data/EventPayloadType.h"
#include "Misc/ScopeRWLock.h"
#include "UEventPayloadRegistry.generated.h"

/**
 * TypeID <-> struct map for event payloads. Guarded by a reader/writer lock:
 * network threads resolve types while the game thread keeps registering new
 * ones as levels (and their handler classes) load.
 */
UCLASS()
class CROWDYNET_API UEventPayloadRegistry : public UObject
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

	void Reset()
	{
		FRWScopeLock WriteLock(RegistryLock, SLT_Write);
		IDToStruct.Reset();
		StructPathToID.Reset();
		IDToName.Reset();
	}

	// Safe to call multiple times; duplicates are silently ignored.
	void RegisterStruct(UScriptStruct* Struct, FCrowdyTypeID TypeID);

	// Registers with the ID resolver (collision overrides) or the path hash.
	// No-op when the struct is already registered.
	void RegisterStructAuto(UScriptStruct* Struct);

	// Installed by UCrowdyAutoRegistry so ID collision overrides from developer
	// settings apply to every registration path without a module dependency.
	void SetIDResolver(TFunction<FCrowdyTypeID(const UScriptStruct*)> InResolver)
	{
		IDResolver = MoveTemp(InResolver);
	}

private:

	TMap<FCrowdyTypeID, TObjectPtr<UScriptStruct>> IDToStruct;

	TMap<FName, FCrowdyTypeID> StructPathToID;

	TMap<FCrowdyTypeID, FName> IDToName;

	TFunction<FCrowdyTypeID(const UScriptStruct*)> IDResolver;

	mutable FRWLock RegistryLock;

	static std::atomic<UEventPayloadRegistry*> Instance;
};
