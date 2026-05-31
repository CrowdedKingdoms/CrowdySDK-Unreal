#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Implement this interface to receive events from a single GraphQL subscription.
 *
 * Usage:
 *
 *   class FMyHandler : public ICrowdySubscriptionHandler
 *   {
 *   public:
 *       virtual void OnSubscriptionData(const TSharedPtr<FJsonObject>& Data) override
 *       {
 *           // Data is the "data" object from the GraphQL response.
 *           // Read your fields directly:
 *           FString Value;
 *           Data->TryGetStringField(TEXT("myField"), Value);
 *       }
 *   };
 *
 * Lifetime rule: the handler pointer must remain valid for as long as the
 * subscription is active. Call UCrowdySubscriptionSubsystem::Unsubscribe()
 * before destroying the handler object.
 */
class CROWDYSDK_API ICrowdySubscriptionHandler
{
public:
	virtual ~ICrowdySubscriptionHandler() = default;

	/**
	 * Called on the game thread each time the server pushes a new event.
	 *
	 * @param Data  The "data" field from the GraphQL subscription payload.
	 *              Will be valid; an invalid JSON object is never passed here.
	 */
	virtual void OnSubscriptionData(const TSharedPtr<FJsonObject>& Data) = 0;

	/**
	 * Called when the server sends a GraphQL error for this subscription.
	 * The subscription remains active — you may still receive future data.
	 * Override to handle or log the error.
	 */
	virtual void OnSubscriptionError(const FString& ErrorMessage) {}

	/**
	 * Called when the server sends a "complete" message for this subscription,
	 * meaning no further data will arrive. You do not need to call Unsubscribe
	 * — the entry is already removed from the active set.
	 */
	virtual void OnSubscriptionComplete() {}
};
