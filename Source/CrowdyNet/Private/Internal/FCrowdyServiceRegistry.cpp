#include "Internal/FCrowdyServiceRegistry.h"
#include "CrowdyNetLog.h"
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
	FRWScopeLock WriteLock(RegistryLock, SLT_Write);

	if (!Layer) return;

	TArray<FName> SubscribedEvents;
	const TArray<UScriptStruct*> SubscribedStructs = Layer->GetSupportedEvents();

	if (!SubscribedStructs.IsEmpty())
	{
		SubscribedEvents.Reserve(SubscribedStructs.Num());

		for (UScriptStruct* SubscribedEvent : SubscribedStructs)
		{
			// Declaring a struct here IS its registration — no annotation needed
			UEventPayloadRegistry::Get()->RegisterStructAuto(SubscribedEvent);

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
	FRWScopeLock WriteLock(RegistryLock, SLT_Write);
	ReceptionLayersByType.Reset();
	UnfilteredEventLayers.Reset();
	SubscribedEventLayers.Reset();
	SubscribedActorUpdateLayers.Reset();
	UnfilteredActorUpdateLayers.Reset();
	UE_CLOG(CrowdyNetTrace::Net(), LogCrowdyNet, Log, TEXT("Deregistered all reception layers."));
}

bool FCrowdyServiceRegistry::IsLayerRegistered(const ICrowdyReceptionLayer* Layer) const
{
	FRWScopeLock ReadLock(RegistryLock, SLT_ReadOnly);
	if (!Layer) return false;

	const TArray<UScriptStruct*> SubscribedStructs = Layer->GetSupportedEvents();
	const TArray<FName> SubscribedActorUpdates = Layer->GetSupportedActorUpdateTypes();

	if (!SubscribedStructs.IsEmpty())
	{
		for (const UScriptStruct* Struct : SubscribedStructs)
		{
			FName EventName;
			FCrowdyTypeID ID;
			if (!UEventPayloadRegistry::Get()->GetID(Struct, ID)
				|| !UEventPayloadRegistry::Get()->GetName(ID, EventName)
				|| !SubscribedEventLayers.Contains(EventName))
				return false;
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
	
	// A single-actor message carries the same payload as a game event (it is an
	// FSingleActorNotification, which derives FGameEventNotification), so it takes the same
	// dispatch path — the event router distinguishes the two by GetType().
	if (ResponseType == ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION
		|| ResponseType == ECrowdyMessageType::SINGLE_ACTOR_MESSAGE)
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
	TArray<ICrowdyReceptionLayer*> LayersCopy;
	{
		FRWScopeLock ReadLock(RegistryLock, SLT_ReadOnly);
		const TArray<ICrowdyReceptionLayer*>* Layers = ReceptionLayersByType.Find(ResponseType);
		if (!Layers)
			return;
		LayersCopy = *Layers;
	}

	for (ICrowdyReceptionLayer* Layer : LayersCopy)
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

	// Claimed events go only to their claimants; everything else (unknown
	// types and registered-but-unclaimed ones) falls through to the
	// unfiltered layers — that is the event router's path.
	TArray<ICrowdyReceptionLayer*> LayersCopy;
	{
		FRWScopeLock ReadLock(RegistryLock, SLT_ReadOnly);

		const TArray<ICrowdyReceptionLayer*>* SubscribedLayers =
			bIsRegisteredEvent ? SubscribedEventLayers.Find(EventName) : nullptr;

		LayersCopy = SubscribedLayers ? *SubscribedLayers : UnfilteredEventLayers;
	}

	for (ICrowdyReceptionLayer* Layer : LayersCopy)
	{
		if (Layer) Layer->OnMessageReceived(Message);
	}
}

void FCrowdyServiceRegistry::DispatchActorUpdateNotification(
	const TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe>& Message)
{
	auto& ActorUpdateMessage = static_cast<FActorUpdateNotificationMessage&>(*Message);

	// Unlike events, actor updates are additive: name-subscribed layers AND
	// the unfiltered layers (the actor tracker) both receive them — movement
	// must keep flowing even when a layer claims the payload type.
	TArray<ICrowdyReceptionLayer*> LayersCopy;
	{
		FRWScopeLock ReadLock(RegistryLock, SLT_ReadOnly);
		LayersCopy = UnfilteredActorUpdateLayers;

		if (ActorUpdateMessage.State.IsValid())
		{
			FName ActorUpdateName;
			if (UActorUpdatePayloadRegistry::Get()->GetName(ActorUpdateMessage.State.GetScriptStruct(), ActorUpdateName))
			{
				if (const TArray<ICrowdyReceptionLayer*>* SubscribedLayers = SubscribedActorUpdateLayers.Find(ActorUpdateName))
				{
					for (ICrowdyReceptionLayer* Layer : *SubscribedLayers)
						LayersCopy.AddUnique(Layer);
				}
			}
		}
	}

	for (ICrowdyReceptionLayer* Layer : LayersCopy)
	{
		if (Layer) Layer->OnMessageReceived(Message);
	}
}
