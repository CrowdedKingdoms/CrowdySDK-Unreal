#pragma once
#include "CoreMinimal.h"

/**
 * Opaque token returned by UCrowdySubscriptionSubsystem::Subscribe().
 *
 * Store it and pass it to Unsubscribe() when you want to cancel.
 * Check IsValid() before use — an invalid handle means Subscribe() failed
 * (e.g. a null handler was passed, or the subsystem is not yet connected).
 */
struct CROWDYSDK_API FCrowdySubscriptionHandle
{
	/** Internal graphql-transport-ws subscription ID. Treat as opaque. */
	FString ID;

	/** True if this handle refers to a real subscription. */
	bool IsValid() const { return !ID.IsEmpty(); }

	/** Returns a handle that IsValid() == false. */
	static FCrowdySubscriptionHandle Invalid() { return {}; }

	bool operator==(const FCrowdySubscriptionHandle& Other) const { return ID == Other.ID; }
	bool operator!=(const FCrowdySubscriptionHandle& Other) const { return ID != Other.ID; }
};
