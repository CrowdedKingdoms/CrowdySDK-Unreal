#pragma once
#include "Templates/SharedPointer.h"
#include "HAL/PlatformAtomics.h"

enum class ECrowdyMessageType : uint8;
class ICrowdyService;
class ICrowdyReceptionLayer;
class ICrowdyMessage;

/**
 * Internal-only registry.
 * Owns services and dispatches messages to reception layers.
 */
class CROWDYSDK_API FCrowdyServiceRegistry
{
public:
	
	FCrowdyServiceRegistry() = default;
	~FCrowdyServiceRegistry() = default;
	
	void RegisterService(FName Name, ICrowdyService* Service);
	
	void RegisterReceptionLayer(ICrowdyReceptionLayer* Layer);
	
	void DeregisterAllReceptionLayers();

	bool IsLayerRegistered(const ICrowdyReceptionLayer* Layer) const;
	
	void DispatchMessage(const TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe>& Message);

	
private:
	
	void DispatchToLayers(ECrowdyMessageType ResponseType, const TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe>& Message);
	void DispatchEventNotification(const TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe>& Message);
	void DispatchActorUpdateNotification(const TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe>& Message);
	
	/** Named Serviced **/
	TMap<FName, ICrowdyService*> Services;
	
	/** All Message Listeners **/
	TArray<ICrowdyReceptionLayer*> ReceptionLayers;
	
	/** Map of a message type to reception layers */
	TMap<ECrowdyMessageType, TArray<ICrowdyReceptionLayer*>> ReceptionLayersByType;
	
	// Specific Game Events handled, not Message Types
	TMap<FName, TArray<ICrowdyReceptionLayer*>> SubscribedEventLayers;
	
	// Specific Actor updates handled
	TMap<FName, TArray<ICrowdyReceptionLayer*>> SubscribedActorUpdateLayers;
	
	// For legacy support
	TArray<ICrowdyReceptionLayer*> UnfilteredEventLayers;
	
	// For Legacy support
	TArray<ICrowdyReceptionLayer*> UnfilteredActorUpdateLayers;

};
