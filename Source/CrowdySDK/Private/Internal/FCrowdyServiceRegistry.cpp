#include "FCrowdyServiceRegistry.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Core/UDP/Interfaces/ICrowdyReceptionLayer.h"
#include "Messages/Actor/FActorUpdateNotificationMessage.h"
#include "Messages/GameObjects/FGameEventNotification.h"
#include "Utils/UActorUpdatePayloadRegistry.h"
#include "Utils/UEventPayloadRegistry.h"

void FCrowdyServiceRegistry::RegisterService(FName Name, ICrowdyService* Service)
{
	Services.Add(Name, Service);
}

void FCrowdyServiceRegistry::RegisterReceptionLayer(ICrowdyReceptionLayer* Layer)
{
	
	if (!Layer) return;

	auto SubscribedEvents = Layer->GetSupportedEventTypes();
	
	const auto SubscribedStructs = Layer->GetSupportedEvents();
	
	if (!SubscribedStructs.IsEmpty())
	{
		SubscribedEvents.Reserve(SubscribedStructs.Num());
		
		for (auto SubscribedEvent : SubscribedStructs)
		{
			FCrowdyTypeID ID;
			if (!UEventPayloadRegistry::Get()->GetID(SubscribedEvent, ID))
				continue;
			
			FName EventName;
			if (!UEventPayloadRegistry::Get()->GetName(ID, EventName))
				continue;
			
			SubscribedEvents.AddUnique(EventName);
		}
	}
	
	const auto& SubscribedActorUpdates = Layer->GetSupportedActorUpdateTypes();
	const auto& SupportedTypes = Layer->GetSupportedResponseTypes();
	const bool bHasSubscriptions = !SubscribedEvents.IsEmpty() || !SubscribedActorUpdates.IsEmpty();
	
	
	if (!bHasSubscriptions)
	{
		for (ECrowdyMessageType Type : SupportedTypes)
		{
			if (!ReceptionLayersByType.Contains(Type))
			{
				ReceptionLayersByType.Add(Type, TArray<ICrowdyReceptionLayer*>());
				ReceptionLayersByType[Type].Reserve(8);
			}
			ReceptionLayersByType[Type].Add(Layer);
		}
	}

	// Events
	if (SubscribedEvents.IsEmpty() && SupportedTypes.Contains(ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION))
	{
		UnfilteredEventLayers.Add(Layer);
	}
	else
	{
		for (auto& Event : SubscribedEvents)
		{
			if (!SubscribedEventLayers.Contains(Event))
			{
				SubscribedEventLayers.Add(Event, TArray<ICrowdyReceptionLayer*>());
				SubscribedEventLayers[Event].Reserve(8);
			}
			SubscribedEventLayers[Event].Add(Layer);
		}
	}

	// Actor Updates
	if (SubscribedActorUpdates.IsEmpty() && SupportedTypes.Contains(ECrowdyMessageType::ACTOR_UPDATE_NOTIFICATION))
	{
		UnfilteredActorUpdateLayers.Add(Layer);
	}
	else
	{
		for (auto& ActorUpdate : SubscribedActorUpdates)
		{
			if (!SubscribedActorUpdateLayers.Contains(ActorUpdate))
			{
				SubscribedActorUpdateLayers.Add(ActorUpdate, TArray<ICrowdyReceptionLayer*>());
				SubscribedActorUpdateLayers[ActorUpdate].Reserve(8);
			}
			SubscribedActorUpdateLayers[ActorUpdate].Add(Layer);
		}
	}
}

void FCrowdyServiceRegistry::DeregisterAllReceptionLayers()
{
	ReceptionLayersByType.Reset();
	UnfilteredEventLayers.Reset();
	SubscribedEventLayers.Reset();
	SubscribedActorUpdateLayers.Reset();
	UnfilteredActorUpdateLayers.Reset();
	UE_LOG(LogTemp, Log, TEXT("Deregistered all reception layers."));
}

bool FCrowdyServiceRegistry::IsLayerRegistered(const ICrowdyReceptionLayer* Layer) const
{
	if (!Layer) return false;
    
        const auto& SubscribedEvents = Layer->GetSupportedEventTypes();
        const auto& SubscribedActorUpdates = Layer->GetSupportedActorUpdateTypes();
    
        if (!SubscribedEvents.IsEmpty())
        {
            for (const FName& Event : SubscribedEvents)
            {
                if (!SubscribedEventLayers.Contains(Event)) return false;
            }
            return true;
        }
    
        if (!SubscribedActorUpdates.IsEmpty())
        {
            for (const FName& ActorUpdate : SubscribedActorUpdates)
            {
                if (!SubscribedActorUpdateLayers.Contains(ActorUpdate)) return false;
            }
            return true;
        }
    
        // No subscriptions — must be in ReceptionLayersByType
        for (const ECrowdyMessageType Type : Layer->GetSupportedResponseTypes())
        {
            if (!ReceptionLayersByType.Contains(Type)) return false;
        }
    
        return true;
}


void FCrowdyServiceRegistry::DispatchMessage(const TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe>& Message)
{

	const auto ResponseType = Message->GetType();
	
	if (ResponseType == ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION)
	{
		DispatchEventNotification(Message);
		return;
	}
	
	if (ResponseType == ECrowdyMessageType::ACTOR_UPDATE_NOTIFICATION)
	{
		DispatchActorUpdateNotification(Message);
		return;
	}
	
	DispatchToLayers(ResponseType, Message);
}

void FCrowdyServiceRegistry::DispatchToLayers(const ECrowdyMessageType ResponseType,
	const TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe>& Message)
{
	const TArray<ICrowdyReceptionLayer*>* Layers = ReceptionLayersByType.Find(ResponseType);
	if (!Layers)
		return;

	for (ICrowdyReceptionLayer* Layer : *Layers)
	{
		if (Layer)
			Layer->OnMessageReceived(Message);
	}
}

void FCrowdyServiceRegistry::DispatchEventNotification(const TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe>& Message)
{
	const auto& EventMessage = static_cast<const FGameEventNotification&>(*Message);

	FName EventName;
	
	const bool bIsRegisteredEvent = UEventPayloadRegistry::Get()->GetName(EventMessage.EventType, EventName);
	
	if (!bIsRegisteredEvent)
	{
		// Legacy layers — always receive all events
		for (ICrowdyReceptionLayer* Layer : UnfilteredEventLayers)
		{
			if (Layer) Layer->OnMessageReceived(Message);
		}
		return;
	}
	
	// New layers — filtered by event name
	if (const TArray<ICrowdyReceptionLayer*>* SubscribedLayers = SubscribedEventLayers.Find(EventName))
	{
		for (ICrowdyReceptionLayer* Layer : *SubscribedLayers)
		{
			if (Layer) Layer->OnMessageReceived(Message);
		}
	}

	
}

void FCrowdyServiceRegistry::DispatchActorUpdateNotification(
	const TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe>& Message)
{
	// Cast to your actor update message type — adjust to whatever yours is called
	auto& ActorUpdateMessage = static_cast<FActorUpdateNotificationMessage&>(*Message);
	
	if (!ActorUpdateMessage.State.IsValid())
	{
		// Legacy layers — always receive all events
		for (ICrowdyReceptionLayer* Layer : UnfilteredActorUpdateLayers)
		{
			if (Layer) Layer->OnMessageReceived(Message);
		}
		return;
	}
	
	FName ActorUpdateName;
	
	if (UActorUpdatePayloadRegistry::Get()->GetName(ActorUpdateMessage.State.GetScriptStruct(), ActorUpdateName))

	if (const TArray<ICrowdyReceptionLayer*>* SubscribedLayers = SubscribedActorUpdateLayers.Find(ActorUpdateName))
	{
		for (ICrowdyReceptionLayer* Layer : *SubscribedLayers)
		{
			if (Layer) Layer->OnMessageReceived(Message);
		}
	}
}
