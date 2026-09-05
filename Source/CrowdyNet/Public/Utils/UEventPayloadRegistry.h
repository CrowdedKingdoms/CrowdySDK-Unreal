#pragma once
#include "Core/CrowdyCategory/FCrowdyIDConflict.h"
#include "Core/FCrowdyTypeID.h"
#include "Data/EventPayloadType.h"
#include "Misc/ScopeRWLock.h"
#include "UObject/ObjectKey.h"

#include <type_traits>

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
		StructToID.Reset();
		IDToName.Reset();
		Conflicts.Reset();
	}

	// Safe to call multiple times; duplicates are silently ignored.
	void RegisterStruct(UScriptStruct* Struct, FCrowdyTypeID TypeID);

	// Registrations refused because another struct already held the TypeID. The
	// refused struct stays unregistered, so a send of it fails rather than being
	// decoded as the incumbent by the receiver.
	TArray<FCrowdyIDConflict> GetConflicts() const;

	bool HasConflicts() const;

	// Forgets the recorded conflicts. Call it once the clash has actually been resolved; the
	// registrations themselves are untouched.
	void ClearConflicts();

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

	// UPROPERTY is what makes this a real reference. A TObjectPtr in an unreflected container is not one,
	// so without it a Blueprint-authored payload struct can be collected while still registered and
	// Resolve hands the decoder a freed UScriptStruct to read wire bytes against. Native structs live in
	// the permanent pool and hid this. The key is spelled out because UnrealHeaderTool does not follow the
	// FCrowdyTypeID alias; the static_assert below the class keeps the two from drifting.
	UPROPERTY()
	TMap<uint16, TObjectPtr<UScriptStruct>> IDToStruct;

	TMap<FName, FCrowdyTypeID> StructPathToID;

	// The same answer keyed on the struct the caller already holds, so an outbound event does not rebuild
	// and rehash a path string to find it. Keyed by TObjectKey rather than by the raw pointer because a
	// recompiled Blueprint struct is destroyed and a later one can land at the same address: an object key
	// carries the serial number that tells them apart, so a stale entry misses and the path map below
	// answers instead of the wrong type ID being put on the wire.
	TMap<TObjectKey<UScriptStruct>, FCrowdyTypeID> StructToID;

	TMap<FCrowdyTypeID, FName> IDToName;

	TFunction<FCrowdyTypeID(const UScriptStruct*)> IDResolver;

	TArray<FCrowdyIDConflict> Conflicts;

	// Caller must already hold the write lock.
	void RecordConflict(FCrowdyTypeID TypeID, const UScriptStruct* Incumbent, const UScriptStruct* Rejected);

	mutable FRWLock RegistryLock;

	static std::atomic<UEventPayloadRegistry*> Instance;
};

static_assert(std::is_same_v<FCrowdyTypeID, uint16>,
	"IDToStruct spells its key uint16 so UnrealHeaderTool can reflect it, which is what keeps the registered "
	"structs alive. Widening FCrowdyTypeID must widen that map with it.");
