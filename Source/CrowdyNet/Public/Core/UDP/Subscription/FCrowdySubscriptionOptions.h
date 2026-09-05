#pragma once

#include "CoreMinimal.h"

/** What my presence means for everyone else subscribed to the same thing. */
enum class ECrowdySubscriptionRole : uint8
{
	/** Give it to me. Nothing about anyone else's delivery changes because I am here. */
	Observe,
	/** Give it to me, and count it as handled so no fallback runs. Several handlers are fine. */
	Handle,
	/** Give it to me only when nothing handles it. */
	Fallback,
};

struct CROWDYNET_API FCrowdySubscriptionOptions
{
	ECrowdySubscriptionRole Role = ECrowdySubscriptionRole::Observe;

	/**
	 * Only meaningful with Handle. Two systems both spawning an actor for one spawn event
	 * gives two actors, so a subscription that cannot tolerate a second handler says so and
	 * a second handler is reported at startup naming both.
	 */
	bool bRequiresExclusiveHandling = false;

	/** Names this subscriber in conflict reports and in the route dump. */
	FName SubscriberName;
};
