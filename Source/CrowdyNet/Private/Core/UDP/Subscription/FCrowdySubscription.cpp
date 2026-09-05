#include "Core/UDP/Subscription/FCrowdySubscription.h"

#include "Internal/FCrowdyRoutingTable.h"
#include "Internal/FCrowdyServiceRegistry.h"

FCrowdySubscription::~FCrowdySubscription()
{
	Release();
}

FCrowdySubscription::FCrowdySubscription(FCrowdySubscription&& Other) noexcept
	: Record(MoveTemp(Other.Record))
{
	Other.Record.Reset();
}

FCrowdySubscription& FCrowdySubscription::operator=(FCrowdySubscription&& Other) noexcept
{
	if (this != &Other)
	{
		Release();
		Record = MoveTemp(Other.Record);
		Other.Record.Reset();
	}
	return *this;
}

bool FCrowdySubscription::IsValid() const
{
	return Record.IsValid() && Record->bActive.load(std::memory_order_acquire);
}

void FCrowdySubscription::Release()
{
	const TSharedPtr<FCrowdySubscriptionRecord, ESPMode::ThreadSafe> Released = MoveTemp(Record);
	Record.Reset();

	if (!Released.IsValid())
	{
		return;
	}

	// Cleared before the table is rebuilt so a delivery already walking an older table stops calling
	// this subscriber on the very message it is in the middle of.
	Released->bActive.store(false, std::memory_order_release);

	if (const TSharedPtr<FCrowdyRouterState, ESPMode::ThreadSafe> State = Released->Router.Pin())
	{
		FScopeLock Lock(&State->Mutex);
		if (State->Owner)
		{
			State->Owner->OnSubscriptionReleased(Released);
		}
	}
}
