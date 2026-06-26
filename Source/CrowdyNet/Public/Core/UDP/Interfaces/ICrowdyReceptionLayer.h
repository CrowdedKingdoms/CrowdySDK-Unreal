#pragma once
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

enum class ECrowdyMessageType : uint8;
class ICrowdyMessage;

/**
 * Receives inbound network messages from FCrowdyServiceRegistry.
 *
 * A layer that returns structs from GetSupportedEvents() claims those event
 * types: claimed events are routed only to their claimants, everything else
 * falls through to layers with no event subscriptions (the event router).
 * Declared structs are auto-registered in UEventPayloadRegistry on layer
 * registration, so no struct annotation is needed.
 */
class CROWDYNET_API ICrowdyReceptionLayer
{
public:

	virtual ~ICrowdyReceptionLayer() = default;

	/** May be called from the network thread — implementations must be thread-safe. */
	virtual void OnMessageReceived(TSharedRef<ICrowdyMessage> Message) = 0;

	/** Message types this layer wants when it has no event/actor-update subscriptions. */
	virtual TArray<ECrowdyMessageType> GetSupportedResponseTypes() const = 0;

	/** Actor-update payload names this layer consumes; empty = all actor updates. */
	virtual TArray<FName> GetSupportedActorUpdateTypes() const
	{
		return {};
	}

	/** Event payload structs this layer claims; empty = unclaimed-event fallback. */
	virtual TArray<UScriptStruct*> GetSupportedEvents() const
	{
		return {};
	}
};
