// Fill out your copyright notice in the Description page of Project Settings.


#include "Core/Blueprints/CrowdyBlueprintReceptionLayer.h"
#include "Kismet/GameplayStatics.h"


UCrowdyBlueprintReceptionLayer* UCrowdyBlueprintReceptionLayer::CreateAndRegisterLayer(UObject* WorldContextObject,
	const TSubclassOf<UCrowdyBlueprintReceptionLayer> Class)
{
	if (!WorldContextObject || !Class)
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdySDK][BlueprintReceptionLayer]: WorldContextObject or Class is null."));
		return nullptr;
	}

	const UGameInstance* GameInstance = UGameplayStatics::GetGameInstance(WorldContextObject);
	if (!GameInstance)
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdySDK][BlueprintReceptionLayer]: GameInstance is null."));
		return nullptr;
	}
	
	UCrowdySDKSubsystem* Subsystem = GameInstance->GetSubsystem<UCrowdySDKSubsystem>();
	if (!Subsystem)
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdySDK][BlueprintReceptionLayer]: Subsystem is null."));
		return nullptr;
	}
	
	UCrowdyBlueprintReceptionLayer* Layer = NewObject<UCrowdyBlueprintReceptionLayer>(
		WorldContextObject, Class);
    
	check(IsValid(Layer))
	
	if (!IsValid(Layer))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdySDK][BlueprintReceptionLayer]: Layer is null."));
		return nullptr;
	}
	
	Layer->Initialize(Subsystem);
	Layer->RegisterLayer();
	//Layer->AddToRoot();
	return Layer;
}

void UCrowdyBlueprintReceptionLayer::DispatchToBlueprint(
	const TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe>& Message) const
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
			BP.UUID = Msg.UUID;
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
			BP.UUID = Msg.UUID;
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
	Super::BeginDestroy();
}
