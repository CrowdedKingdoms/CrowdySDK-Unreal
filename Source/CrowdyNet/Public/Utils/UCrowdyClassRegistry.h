#pragma once
#include "Core/CrowdyCategory/FCrowdyIDConflict.h"
#include "Core/FCrowdyTypeID.h"
#include "UObject/ObjectKey.h"
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
	// for benign duplicates and for calls after Seal; the second GI init in
	// multi-client PIE hits both cases.
	bool RegisterClass(FCrowdyClassID ClassID, const FSoftClassPath& ClassPath);

	// Registrations refused because another class already held the ClassID.
	TArray<FCrowdyIDConflict> GetConflicts() const { return Conflicts; }

	bool HasConflicts() const { return Conflicts.Num() > 0; }

	// Forgets the recorded conflicts and the refusals that go with them, so the classes involved go
	// back to deriving their ID from their path. Call it once the clash has actually been resolved.
	void ClearConflicts();

	// Returns an invalid path for unknown IDs; callers fall back to the
	// ClassPath carried on the spawn event.
	FSoftClassPath Resolve(FCrowdyClassID ClassID) const;

	/**
	 * Registered classes return their (possibly overridden) ID. Unregistered
	 * classes fall back to the path hash so a class that was not loaded during
	 * the startup scan still produces the same ID on every client.
	 *
	 * A class whose registration was refused because another class already held
	 * that ID returns CROWDY_INVALID_CLASS_ID instead, so its spawn events carry
	 * no ID and receivers resolve them from the class path the event also
	 * carries rather than from the other class.
	 */
	FCrowdyClassID GetID(const UClass* Class) const;

	// Called after registration completes; switches to read-only mode.
	void Seal();

	// Forgets the per-class answers GetID has memoized. Wanted only to reclaim the entries a reload
	// orphans: an entry keyed on a class that was reinstanced can no longer be found by the new class, so
	// a stale one is never read, it just sits there. A Blueprint recompile reinstances without firing the
	// reload delegate, so entries accumulate there whatever this is called on.
	void InvalidateIDMemo();

	bool IsSealed() const
	{
		return bSealed.load(std::memory_order_acquire);
	}

	int32 NumRegistered() const { return IDToClassPath.Num(); }

private:

	TMap<FCrowdyClassID, FSoftClassPath> IDToClassPath;

	TMap<FName, FCrowdyClassID> PathToID;

	TArray<FCrowdyIDConflict> Conflicts;

	// Class paths whose registration was refused over an ID clash. Written only while registration
	// is still open, on the game thread, so GetID reads it under the same guarantee as PathToID.
	TSet<FName> RefusedPaths;

	// GetID's answer per class, so the path string and its FName are built once per class instead of once
	// per call. Written and read only on the game thread and only once the registry is sealed, which is
	// what keeps it lock-free on a registry that has no lock: before the seal the answer can still change,
	// and every other thread takes the path lookup exactly as it does today.
	mutable TMap<TObjectKey<UClass>, FCrowdyClassID> ClassIDMemo;

	static std::atomic<UCrowdyClassRegistry*> Instance;
	std::atomic<bool> bSealed { false };
};
