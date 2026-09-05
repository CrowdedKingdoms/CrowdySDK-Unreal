#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

/**
 * Hands out decoded message objects without allocating one per received message.
 *
 * A slot is reusable only while the pool holds the only reference to it, so a message a subscriber kept
 * past its delivery is never recycled underneath that subscriber. That is what makes this safe against a
 * receive path that retains the message it decoded rather than copying the payload out of it.
 *
 * Single threaded by contract: inbound delivery is drained on the game thread, and nothing here locks.
 */
template <typename MessageType>
class TCrowdyMessagePool
{
public:

	explicit TCrowdyMessagePool(const int32 InMaxSlots = 4096)
		: MaxSlots(FMath::Max(1, InMaxSlots))
	{
	}

	/**
	 * A message reset to the state a freshly constructed one would be in.
	 *
	 * Reset by whole-object assignment rather than field by field, so a member added later cannot carry a
	 * previous message's value into the next decode without anyone noticing.
	 */
	TSharedRef<MessageType, ESPMode::ThreadSafe> Acquire()
	{
		if (Slots.Num() > 0)
		{
			Cursor = (Cursor + 1) % Slots.Num();

			// One probe, not a scan, because the cursor lands on the slot handed out longest ago and that
			// is the one most likely to have been released. A holder that keeps its message longer than
			// the rest simply misses here and costs an allocation, which is why the miss path below grows
			// the pool rather than waiting: the pool settles at however many are genuinely held at once.
			TSharedRef<MessageType, ESPMode::ThreadSafe>& Slot = Slots[Cursor];
			if (Slot.IsUnique())
			{
				*Slot = MessageType();
				++ReuseCount;
				return Slot;
			}
		}

		TSharedRef<MessageType, ESPMode::ThreadSafe> Fresh = MakeShared<MessageType, ESPMode::ThreadSafe>();

		if (Slots.Num() < MaxSlots)
		{
			Slots.Add(Fresh);
			Cursor = Slots.Num() - 1;
		}
		else
		{
			++OverflowCount;
		}

		return Fresh;
	}

	/** Slots the pool has grown to, which settles at the number of messages in flight at once. */
	int32 NumSlots() const { return Slots.Num(); }

	/** Messages served from a slot rather than allocated. Zero after a drain means the pool did nothing. */
	int64 GetReuseCount() const { return ReuseCount; }

	/** Messages allocated because every slot was still held and the pool had reached its ceiling. */
	int64 GetOverflowCount() const { return OverflowCount; }

	void Reset()
	{
		Slots.Reset();
		Cursor = 0;
		ReuseCount = 0;
		OverflowCount = 0;
	}

private:

	TArray<TSharedRef<MessageType, ESPMode::ThreadSafe>> Slots;
	int32 Cursor = 0;
	int32 MaxSlots = 4096;
	int64 ReuseCount = 0;
	int64 OverflowCount = 0;
};
