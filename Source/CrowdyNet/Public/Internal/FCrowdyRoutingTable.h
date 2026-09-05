#pragma once

#include "Algo/BinarySearch.h"
#include "CoreMinimal.h"
#include "Core/FCrowdyTypeID.h"
#include "Core/UDP/Subscription/FCrowdyDelivery.h"
#include "Core/UDP/Subscription/FCrowdyPayloadKey.h"
#include "Core/UDP/Subscription/FCrowdySubscriptionOptions.h"
#include "HAL/CriticalSection.h"
#include "Internal/FCrowdyServiceRegistry.h"
#include "Templates/SharedPointer.h"

#include <atomic>

class FCrowdyServiceRegistry;

/**
 * Shared indirection from a live handle back to the registry that issued it. The registry clears
 * Owner while holding Mutex as it is destroyed, so a handle that outlives it releases harmlessly.
 */
struct FCrowdyRouterState
{
	FCriticalSection Mutex;
	FCrowdyServiceRegistry* Owner = nullptr;
};

/**
 * One live subscription. Owned jointly by its handle and by every published table that lists it,
 * so a record stays alive for the whole of a delivery that is already walking an older table.
 */
struct FCrowdySubscriptionRecord
{
	FCrowdyDeliveryHandler Handler;
	FCrowdySubscriptionOptions Options;
	TArray<FCrowdySubscriptionKey> Keys;

	/** Cleared before the table is rebuilt, and tested immediately before every invocation. */
	std::atomic<bool> bActive{ true };

	TWeakPtr<FCrowdyRouterState, ESPMode::ThreadSafe> Router;
};

using FCrowdySubscriptionRecordPtr = TSharedPtr<FCrowdySubscriptionRecord, ESPMode::ThreadSafe>;

struct FCrowdyRouteSlot
{
	TArray<FCrowdySubscriptionRecordPtr> Observers;
	TArray<FCrowdySubscriptionRecordPtr> Handlers;
	TArray<FCrowdySubscriptionRecordPtr> Fallbacks;

	bool IsEmpty() const
	{
		return Observers.IsEmpty() && Handlers.IsEmpty() && Fallbacks.IsEmpty();
	}
};

/**
 * The routes of one payload category. Keys is sorted and parallel to Slots, so a lookup is a
 * binary search over a few dozen uint16 with no hashing and no allocation.
 */
struct FCrowdyPayloadRoutes
{
	TArray<FCrowdyTypeID> Keys;
	TArray<FCrowdyRouteSlot> Slots;
	FCrowdyRouteSlot Wildcard;

	const FCrowdyRouteSlot* Find(const FCrowdyTypeID TypeID) const
	{
		const int32 Index = Algo::LowerBound(Keys, TypeID);
		return Keys.IsValidIndex(Index) && Keys[Index] == TypeID ? &Slots[Index] : nullptr;
	}

	FCrowdyRouteSlot& FindOrAdd(const FCrowdyTypeID TypeID)
	{
		const int32 Index = Algo::LowerBound(Keys, TypeID);
		if (Keys.IsValidIndex(Index) && Keys[Index] == TypeID)
		{
			return Slots[Index];
		}

		Keys.Insert(TypeID, Index);
		Slots.InsertDefaulted(Index);
		return Slots[Index];
	}
};

/**
 * An immutable snapshot of every subscription. A writer builds a whole new one and swaps it in;
 * a reader copies the shared pointer and walks it with no lock held.
 */
struct FCrowdyRoutingTable
{
	FCrowdyPayloadRoutes EventRoutes;
	FCrowdyPayloadRoutes ActorUpdateRoutes;
	FCrowdyRouteSlot OpcodeRoutes[256];
	uint32 Generation = 0;

	FCrowdyPayloadRoutes& RoutesFor(const ECrowdyPayloadCategory Category)
	{
		return Category == ECrowdyPayloadCategory::ActorUpdate ? ActorUpdateRoutes : EventRoutes;
	}

	const FCrowdyPayloadRoutes& RoutesFor(const ECrowdyPayloadCategory Category) const
	{
		return Category == ECrowdyPayloadCategory::ActorUpdate ? ActorUpdateRoutes : EventRoutes;
	}
};
