// Fill out your copyright notice in the Description page of Project Settings.


#include "Core/Blueprints/CrowdyBlueprintReceptionLayer.h"
#include "CrowdyServicesLog.h"
#include "Kismet/GameplayStatics.h"
#include "Utils/UActorUpdatePayloadRegistry.h"


UCrowdyBlueprintReceptionLayer* UCrowdyBlueprintReceptionLayer::CreateAndRegisterLayer(UObject* WorldContextObject,
	const TSubclassOf<UCrowdyBlueprintReceptionLayer> Class)
{
	if (!WorldContextObject || !Class)
	{
		UE_LOG(LogCrowdyServices, Error, TEXT("[CrowdySDK][BlueprintReceptionLayer]: WorldContextObject or Class is null."));
		return nullptr;
	}

	const UGameInstance* GameInstance = UGameplayStatics::GetGameInstance(WorldContextObject);
	if (!GameInstance)
	{
		UE_LOG(LogCrowdyServices, Error, TEXT("[CrowdySDK][BlueprintReceptionLayer]: GameInstance is null."));
		return nullptr;
	}
	
	UCrowdySDKBridgeSubsystem* Subsystem = GameInstance->GetSubsystem<UCrowdySDKBridgeSubsystem>();
	if (!Subsystem)
	{
		UE_LOG(LogCrowdyServices, Error, TEXT("[CrowdySDK][BlueprintReceptionLayer]: Bridge subsystem is null."));
		return nullptr;
	}
	
	UCrowdyBlueprintReceptionLayer* Layer = NewObject<UCrowdyBlueprintReceptionLayer>(
		WorldContextObject, Class);
    
	check(IsValid(Layer))
	
	if (!IsValid(Layer))
	{
		UE_LOG(LogCrowdyServices, Error, TEXT("[CrowdySDK][BlueprintReceptionLayer]: Layer is null."));
		return nullptr;
	}
	
	Layer->Initialize(Subsystem);
	Layer->RegisterLayer();
	//Layer->AddToRoot();
	return Layer;
}

void UCrowdyBlueprintReceptionLayer::RegisterLayer()
{
	if (!Subscriptions.IsEmpty())
		return; // already registered

	if (!SDKBridge || !SDKBridge->ServiceRegistry)
		return;

	FCrowdyServiceRegistry* Registry = SDKBridge->ServiceRegistry;

	const FName SubscriberName(*FString::Printf(TEXT("BlueprintReceptionLayer_%s"), *GetName()));

	FCrowdySubscriptionOptions ObserveOptions;
	ObserveOptions.Role = ECrowdySubscriptionRole::Observe;
	ObserveOptions.SubscriberName = SubscriberName;

	// Delivery is on the game thread, so the Blueprint graph is entered straight from it. The liveness
	// check stays: a layer marked for destruction releases its subscriptions in BeginDestroy, which does
	// not necessarily run before the garbage mark, so a delivery can still arrive for one.
	auto Forward = [this](const FCrowdyDelivery& Delivery)
	{
		if (IsValid(this))
		{
			DispatchToBlueprint(Delivery.Message);
		}
	};

	// Claimed event structs are routed only to their claimants; declaring one here is what registers
	// it for the wire, so no extra annotation is needed.
	if (!SupportedEvents.IsEmpty())
	{
		FCrowdySubscriptionOptions HandleOptions = ObserveOptions;
		HandleOptions.Role = ECrowdySubscriptionRole::Handle;

		for (const TObjectPtr<UScriptStruct>& Struct : SupportedEvents)
		{
			if (Struct)
				Subscriptions.Add(Registry->SubscribeToEventPayload(Struct, HandleOptions, Forward));
		}
	}
	else if (SupportedResponseTypes.Contains(ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION)
		|| SupportedResponseTypes.Contains(ECrowdyMessageType::SERVER_EVENT_NOTIFICATION))
	{
		// No claims: receive whatever event no other layer claimed, same as the old unclaimed-event
		// fallback. The client-originated (138) and server-originated (139) carriers share the same
		// Event-category routing key, so declaring either one wires this same fallback.
		FCrowdySubscriptionOptions FallbackOptions = ObserveOptions;
		FallbackOptions.Role = ECrowdySubscriptionRole::Fallback;
		Subscriptions.Add(Registry->SubscribeToAllPayloads(ECrowdyPayloadCategory::Event, FallbackOptions, Forward));
	}

	// Actor-update claims are declared as payload struct names rather than struct references, so the
	// claim can only be matched after the payload is decoded: observe every actor update and keep the
	// ones whose registered name is in the declared set.
	if (!SupportedActorUpdateTypes.IsEmpty())
	{
		const TSet<FName> ClaimedNames(SupportedActorUpdateTypes);
		Subscriptions.Add(Registry->SubscribeToAllPayloads(ECrowdyPayloadCategory::ActorUpdate, ObserveOptions,
			[this, ClaimedNames](const FCrowdyDelivery& Delivery)
			{
				const FActorUpdateNotificationMessage& Update = Delivery.GetAs<FActorUpdateNotificationMessage>();
				if (!Update.State.IsValid())
					return;

				FName PayloadName;
				if (!UActorUpdatePayloadRegistry::Get()->GetName(Update.State.GetScriptStruct(), PayloadName))
					return;

				if (!ClaimedNames.Contains(PayloadName))
					return;

				if (IsValid(this))
				{
					DispatchToBlueprint(Delivery.Message);
				}
			}));
	}
	else if (SupportedResponseTypes.Contains(ECrowdyMessageType::ACTOR_UPDATE_NOTIFICATION))
	{
		// Empty = all actor updates.
		Subscriptions.Add(Registry->SubscribeToAllPayloads(ECrowdyPayloadCategory::ActorUpdate, ObserveOptions, Forward));
	}

	// Any other declared opcode is preserved for parity with the old registration, even though
	// DispatchToBlueprint below has nothing to do with it. Skip every opcode that is routed by
	// payload type rather than by opcode: subscribing to one of those here would register a
	// handler that can never be delivered to, since the router matches its messages by payload key
	// alone. Those are wired above instead.
	for (const ECrowdyMessageType Type : SupportedResponseTypes)
	{
		if (Type == ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION
			|| Type == ECrowdyMessageType::SINGLE_ACTOR_MESSAGE
			|| Type == ECrowdyMessageType::SERVER_EVENT_NOTIFICATION
			|| Type == ECrowdyMessageType::ACTOR_UPDATE_NOTIFICATION)
		{
			continue;
		}
		Subscriptions.Add(Registry->SubscribeToOpcode(Type, ObserveOptions, Forward));
	}
}

void UCrowdyBlueprintReceptionLayer::DispatchToBlueprint(
	const TSharedRef<const ICrowdyMessage, ESPMode::ThreadSafe>& Message) const
{
	switch (Message->GetType())
	{
	case ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION:
		{
			const auto& Msg = static_cast<const FGameEventNotification&>(*Message);
			FGameEventNotificationBP BP;
			BP.ChunkX = Msg.ChunkX;
			BP.ChunkY = Msg.ChunkY;
			BP.ChunkZ = Msg.ChunkZ;
			BP.UUID = Msg.UUID.ToString();
			BP.Event = Msg.State;
			BP.Timestamp = Msg.Timestamp;
			OnEventNotificationReceived.Broadcast(BP);
			break;
		}
	case ECrowdyMessageType::ACTOR_UPDATE_NOTIFICATION:
		{
			const auto& Msg = static_cast<const FActorUpdateNotificationMessage&>(*Message);
			FActorUpdateNotificationBP BP;
			BP.ChunkX = Msg.ChunkX;
			BP.ChunkY = Msg.ChunkY;
			BP.ChunkZ = Msg.ChunkZ;
			BP.UUID = Msg.UUID.ToString();
			BP.ActorUpdate = Msg.State;
			BP.Timestamp = Msg.Timestamp;
			OnActorUpdateReceived.Broadcast(BP);
			break;
		}
	default: 
		break;
	}
}

void UCrowdyBlueprintReceptionLayer::BeginDestroy()
{
	// Releases every subscription now rather than waiting for the object's own destructor, so no
	// further delivery can call back into this object once GC starts tearing it down.
	Subscriptions.Empty();

	Super::BeginDestroy();
}
