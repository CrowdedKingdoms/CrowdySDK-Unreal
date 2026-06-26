#pragma once
#include "CoreMinimal.h"
#include <atomic>


class FLockFreeGuidSet
{
public:
	
	static constexpr int32 EMPTY_SLOT = 0;
	static constexpr int32 PENDING = 1;
	static constexpr int32 OCCUPIED = 2;
	
	explicit FLockFreeGuidSet(const int32 Capacity)
		: Cap(Capacity)
	{
		Slots.SetNum(Capacity);
		States.SetNum(Capacity);
		for (int32 i = 0; i < Capacity; i++)
		{
			Slots[i]  = FGuid{};
			States[i].store(EMPTY_SLOT, std::memory_order_relaxed);
		}
	}

	// Returns true if newly added, false if already present
	bool Add(const FGuid& GUID)
	{
		const int32 Start = GuidToIndex(GUID);
		for (int32 i = 0; i < Cap; i++)
		{
			const int32 Idx = (Start + i) % Cap;
			int32 Expected  = EMPTY_SLOT;

			if (States[Idx].compare_exchange_strong(
					Expected, PENDING,
					std::memory_order_acq_rel,
					std::memory_order_acquire))
			{
				Slots[Idx] = GUID;
				States[Idx].store(OCCUPIED, std::memory_order_release);
				return true;
			}

			// Spin until slot finishes being written
			while (States[Idx].load(std::memory_order_acquire) == PENDING) {}

			if (Slots[Idx] == GUID)
				return false; // Already tracked
		}
		return false; // Full — shouldn't happen if sized correctly
	}
	
	bool Contains(const FGuid& GUID) const
	{
		const int32 Start = GuidToIndex(GUID);
		for (int32 i = 0; i < Cap; i++)
		{
			const int32 Idx = (Start + i) % Cap;
			const int32 State = States[Idx].load(std::memory_order_acquire);

			if (State == EMPTY_SLOT)  return false;
			if (State == OCCUPIED && Slots[Idx] == GUID) return true;
		}
		return false;
	}
	
	bool Remove(const FGuid& GUID)
	{
		const int32 Start = GuidToIndex(GUID);
		for (int32 i = 0; i < Cap; i++)
		{
			const int32 Idx = (Start + i) % Cap;
			if (States[Idx].load(std::memory_order_acquire) == OCCUPIED &&
				Slots[Idx] == GUID)
			{
				States[Idx].store(EMPTY_SLOT, std::memory_order_release);
				return true;
			}
		}
		return false;
	}
	
private:
	
	int32 GuidToIndex(const FGuid& GUID) const
	{
		return FMath::Abs(static_cast<int32>(GetTypeHash(GUID))) % Cap;
	}

	int32 Cap;
	TArray<FGuid>          Slots;
	TArray<std::atomic<int32>> States;
};
