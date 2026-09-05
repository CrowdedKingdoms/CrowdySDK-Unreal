#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

struct FCrowdySubscriptionRecord;

/**
 * Holds one live subscription. Destroying or releasing it stops the handler being invoked again;
 * it does not unwind a call already on the stack. The threading rules that make that a sequential
 * guarantee rather than a racy one are stated in full on FCrowdyDelivery, which is the same
 * contract seen from the handler's side. A system keeps as many of these as it has narrow
 * interests.
 *
 * A callback that captures a raw `this` is safe only while the handle is a member of that
 * same object: the capture and the unsubscribe then die together. Store it anywhere else and
 * the capture can outlive what it points at.
 */
class CROWDYNET_API FCrowdySubscription
{
public:

	FCrowdySubscription() = default;
	~FCrowdySubscription();

	FCrowdySubscription(FCrowdySubscription&& Other) noexcept;
	FCrowdySubscription& operator=(FCrowdySubscription&& Other) noexcept;
	FCrowdySubscription(const FCrowdySubscription&) = delete;
	FCrowdySubscription& operator=(const FCrowdySubscription&) = delete;

	bool IsValid() const;
	void Release();

private:

	friend class FCrowdyServiceRegistry;

	TSharedPtr<FCrowdySubscriptionRecord, ESPMode::ThreadSafe> Record;
};
