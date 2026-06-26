#pragma once
#include "Core/FCrowdyTypeID.h"
#include "UObject/SoftObjectPath.h"
#include "UCrowdyClassRegistry.generated.h"

/**
 * ClassID -> entity class path map for spawn events. Populated by
 * UCrowdyAutoRegistry at startup from classes carrying a UCrowdyEntityComponent,
 * then sealed for lock-free reads.
 */
UCLASS()
class CROWDYNET_API UCrowdyClassRegistry : public UObject
{
	GENERATED_BODY()

public:

	static UCrowdyClassRegistry* Get()
	{
		if (!Instance.load(std::memory_order_acquire))
		{
			UCrowdyClassRegistry* NewRegistry = NewObject<UCrowdyClassRegistry>();
			NewRegistry->AddToRoot();

			UCrowdyClassRegistry* Expected = nullptr;
			if (!Instance.compare_exchange_strong(Expected, NewRegistry, std::memory_order_release))
				NewRegistry->RemoveFromRoot();
		}
		return Instance.load(std::memory_order_acquire);
	}

	static void Shutdown()
	{
		UCrowdyClassRegistry* Current = Instance.exchange(nullptr, std::memory_order_acq_rel);
		if (Current) Current->RemoveFromRoot();
	}

	// Returns true when the class is newly inserted. Returns false (silently)
	// for benign duplicates and for calls after Seal — the second GI init in
	// multi-client PIE hits both cases.
	bool RegisterClass(FCrowdyClassID ClassID, const FSoftClassPath& ClassPath);

	// Returns an invalid path for unknown IDs; callers fall back to the
	// ClassPath carried on the spawn event.
	FSoftClassPath Resolve(FCrowdyClassID ClassID) const;

	/**
	 * Registered classes return their (possibly overridden) ID. Unregistered
	 * classes fall back to the path hash so a class that was not loaded during
	 * the startup scan still produces the same ID on every client.
	 */
	FCrowdyClassID GetID(const UClass* Class) const;

	// Called after registration completes; switches to read-only mode.
	void Seal();

	bool IsSealed() const
	{
		return bSealed.load(std::memory_order_acquire);
	}

	int32 NumRegistered() const { return IDToClassPath.Num(); }

private:

	TMap<FCrowdyClassID, FSoftClassPath> IDToClassPath;

	TMap<FName, FCrowdyClassID> PathToID;

	static std::atomic<UCrowdyClassRegistry*> Instance;
	std::atomic<bool> bSealed { false };
};
