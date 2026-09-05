// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Core/CrowdyCategory/FCrowdyIDConflict.h"
#include "Core/FCrowdyTypeID.h"
#include "Data/ActorUpdatePayloadType.h"
#include "UObject/ObjectKey.h"

#include <type_traits>
#include "Misc/ScopeRWLock.h"
#include "UObject/Object.h"
#include "UActorUpdatePayloadRegistry.generated.h"

/**
 * TypeID <-> struct map for actor update payloads. Guarded by a reader/writer
 * lock: network threads resolve types while the game thread keeps registering
 * new ones as levels (and their executor classes) load.
 */
UCLASS()
class CROWDYNET_API UActorUpdatePayloadRegistry : public UObject
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
	// the permanent pool and hid this. It is also what makes the raw pointer keys below safe. The key is
	// spelled out because UnrealHeaderTool does not follow the FCrowdyTypeID alias; the static_assert
	// below the class keeps the two from drifting.
	UPROPERTY()
	TMap<uint16, TObjectPtr<UScriptStruct>> IDToStruct;

	TMap<FName, FCrowdyTypeID> StructPathToID;

	// The same mapping keyed on the struct itself, so the once-per-outbound-update lookup does not have
	// to rebuild a path string and hash it into an FName to find something the caller already holds a
	// pointer to. StructPathToID stays authoritative; this only short-circuits it. Keyed by TObjectKey
	// rather than by the raw pointer: reflecting IDToStruct stops a registered struct being collected,
	// but a REINSTANCED one is a different object, and GC clears the reference to the old one and frees
	// it. An object key carries the serial number that tells the two apart, so a stale entry misses and
	// the path map answers instead of a freed address matching a later struct.
	TMap<TObjectKey<UScriptStruct>, FCrowdyTypeID> StructToID;

	TMap<FCrowdyTypeID, FName> IDToName;

	TFunction<FCrowdyTypeID(const UScriptStruct*)> IDResolver;

	TArray<FCrowdyIDConflict> Conflicts;

	// Caller must already hold the write lock.
	void RecordConflict(FCrowdyTypeID TypeID, const UScriptStruct* Incumbent, const UScriptStruct* Rejected);

	mutable FRWLock RegistryLock;

	static std::atomic<UActorUpdatePayloadRegistry*> Instance;
};

static_assert(std::is_same_v<FCrowdyTypeID, uint16>,
	"IDToStruct spells its key uint16 so UnrealHeaderTool can reflect it, which is what keeps the registered "
	"structs alive. Widening FCrowdyTypeID must widen that map with it.");
