#include "Subsystem/CrowdyProximityChat.h"

#include "Core/CrowdySDKBridgeSubsystem.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Internal/FCrowdyServiceRegistry.h"
#include "Messages/Communication/FTextMessageNotification.h"
#include "Messages/Communication/FTextMessageRequest.h"
#include "Subsystem/CrowdyGameSession.h"

void UCrowdyProximityChat::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	if (UCrowdyGameSession* GameSession = Collection.InitializeDependency<UCrowdyGameSession>())
	{
		GameSession->OnOwnerUUIDUpdated.AddUniqueDynamic(this, &UCrowdyProximityChat::HandleOwnerUUIDUpdated);
	}
	if (UCrowdyCppReplicationSubsystem* Replication = Collection.InitializeDependency<UCrowdyCppReplicationSubsystem>())
	{
		Replication->OnSendRefused.AddUniqueDynamic(this, &UCrowdyProximityChat::HandleSendRefused);
	}

	// The registry may not exist yet; the owner id arriving and every send try again.
	EnsureSubscription();
}

void UCrowdyProximityChat::Deinitialize()
{
	TextSubscription.Release();

	if (const UGameInstance* GameInstance = GetGameInstance())
	{
		if (UCrowdyGameSession* GameSession = GameInstance->GetSubsystem<UCrowdyGameSession>())
		{
			GameSession->OnOwnerUUIDUpdated.RemoveAll(this);
		}
		if (UCrowdyCppReplicationSubsystem* Replication = GameInstance->GetSubsystem<UCrowdyCppReplicationSubsystem>())
		{
			Replication->OnSendRefused.RemoveAll(this);
		}
	}

	Super::Deinitialize();
}

ECrowdyProximityChatResult UCrowdyProximityChat::SendProximityText(const FString& Text, const int32 DistanceChunks)
{
	EnsureSubscription();

	UGameInstance* GameInstance = GetGameInstance();
	const UWorld* World = GameInstance ? GameInstance->GetWorld() : nullptr;
	UCrowdyGameSession* GameSession = GameInstance ? GameInstance->GetSubsystem<UCrowdyGameSession>() : nullptr;
	const UCrowdySDKBridgeSubsystem* Bridge = GameInstance ? GameInstance->GetSubsystem<UCrowdySDKBridgeSubsystem>() : nullptr;
	const UCrowdyCppReplicationSubsystem* Replication = GameInstance ? GameInstance->GetSubsystem<UCrowdyCppReplicationSubsystem>() : nullptr;

	const FCrowdyActorId SenderId = GameSession ? FCrowdyActorId::FromStringOrUnset(GameSession->GetUUID()) : FCrowdyActorId();
	const bool bConnected = Bridge && Bridge->SendMessageFn && Replication && Replication->IsRouting() && SenderId.IsSet();
	const APlayerController* PlayerController = World ? World->GetFirstPlayerController() : nullptr;
	const bool bHasPosition = PlayerController && PlayerController->GetPawn();

	const ECrowdyProximityChatResult Verdict = CheckText(Text, bConnected, bHasPosition);
	if (Verdict != ECrowdyProximityChatResult::Sent)
	{
		return Verdict;
	}

	const FInt64Vector Chunk = GameSession->GetLocalPlayerChunk(World);
	FTextMessageRequest Request;
	Request.AppID = GameSession->GetAppID();
	Request.ChunkX = Chunk.X;
	Request.ChunkY = Chunk.Y;
	Request.ChunkZ = Chunk.Z;
	Request.ReplicationDistance = static_cast<ECrowdyReplicationDistance>(FMath::Clamp(DistanceChunks, 0, 8));
	Request.DecayRate = ECrowdyDecayRate::No_Decay;
	Request.UUID = SenderId;
	Request.Message = Text;
	Bridge->SendMessageFn(Request);
	return Verdict;
}

FString UCrowdyProximityChat::DescribeProximityChatResult(const ECrowdyProximityChatResult Result)
{
	switch (Result)
	{
	case ECrowdyProximityChatResult::Sent:
		return TEXT("The line left this client. There is no delivery receipt; a refusal arrives later on On Proximity Text Refused.");
	case ECrowdyProximityChatResult::EmptyText:
		return TEXT("There is no text to send.");
	case ECrowdyProximityChatResult::TooLong:
		return FString::Printf(TEXT("A line can be at most %d bytes of UTF-8."), MaxTextBytes);
	case ECrowdyProximityChatResult::NotConnected:
		return TEXT("The realtime connection is not up, or the player has no actor id yet.");
	case ECrowdyProximityChatResult::NoPosition:
		return TEXT("There is no local pawn, and a line's reach is measured from the chunk the pawn stands in.");
	}
	return FString();
}

ECrowdyProximityChatResult UCrowdyProximityChat::CheckText(const FString& Text, const bool bConnected, const bool bHasPosition)
{
	if (Text.IsEmpty())
	{
		return ECrowdyProximityChatResult::EmptyText;
	}

	const FTCHARToUTF8 Utf8(*Text);
	if (Utf8.Length() > MaxTextBytes)
	{
		return ECrowdyProximityChatResult::TooLong;
	}
	if (!bConnected)
	{
		return ECrowdyProximityChatResult::NotConnected;
	}
	return bHasPosition ? ECrowdyProximityChatResult::Sent : ECrowdyProximityChatResult::NoPosition;
}

FGuid UCrowdyProximityChat::EntityIdFromSenderUuid(const FString& SenderUuid)
{
	// A player's actor id is its entity id written as 32 hex digits.
	FGuid EntityId;
	return FGuid::ParseExact(SenderUuid, EGuidFormats::Digits, EntityId) ? EntityId : FGuid();
}

bool UCrowdyProximityChat::ReadReceivedText(const FTextMessageNotification& Notification, const FCrowdyActorId& LocalSenderId,
	FCrowdyProximityText& OutMessage)
{
	// The server does not echo text, but a peer replaying our id must not read as us either.
	if (LocalSenderId.IsSet() && Notification.UUID == LocalSenderId)
	{
		return false;
	}

	OutMessage.SenderUuid = Notification.UUID.ToString();
	OutMessage.SenderEntityId = EntityIdFromSenderUuid(OutMessage.SenderUuid);
	OutMessage.Text = Notification.Message;
	return true;
}

bool UCrowdyProximityChat::IsTextRefusal(const FCrowdySendRefusal& Refusal)
{
	return !Refusal.bChannelMessage && Refusal.MessageType == static_cast<int32>(ECrowdyMessageType::CLIENT_TEXT_PACKET);
}

void UCrowdyProximityChat::HandleTextNotification(const FTextMessageNotification& Notification)
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UCrowdyGameSession* GameSession = GameInstance ? GameInstance->GetSubsystem<UCrowdyGameSession>() : nullptr;
	const FCrowdyActorId LocalSenderId = GameSession ? FCrowdyActorId::FromStringOrUnset(GameSession->GetUUID()) : FCrowdyActorId();

	FCrowdyProximityText Message;
	if (ReadReceivedText(Notification, LocalSenderId, Message))
	{
		OnProximityTextReceived.Broadcast(Message);
	}
}

void UCrowdyProximityChat::EnsureSubscription()
{
	if (TextSubscription.IsValid())
	{
		return;
	}

	UGameInstance* GameInstance = GetGameInstance();
	UCrowdySDKBridgeSubsystem* Bridge = GameInstance ? GameInstance->GetSubsystem<UCrowdySDKBridgeSubsystem>() : nullptr;
	if (!Bridge || !Bridge->ServiceRegistry)
	{
		return;
	}

	TWeakObjectPtr<UCrowdyProximityChat> WeakThis(this);
	TextSubscription = Bridge->ServiceRegistry->SubscribeToOpcode<FTextMessageNotification>(
		ECrowdyMessageType::CLIENT_TEXT_NOTIFICATION,
		{ ECrowdySubscriptionRole::Observe, false, TEXT("CrowdyProximityChat") },
		[WeakThis](const FTextMessageNotification& Notification, const FCrowdyDelivery&)
		{
			if (UCrowdyProximityChat* Self = WeakThis.Get())
			{
				Self->HandleTextNotification(Notification);
			}
		});
}

void UCrowdyProximityChat::HandleOwnerUUIDUpdated(FString NewUUID)
{
	EnsureSubscription();
}

void UCrowdyProximityChat::HandleSendRefused(const FCrowdySendRefusal& Refusal)
{
	if (IsTextRefusal(Refusal))
	{
		OnProximityTextRefused.Broadcast(Refusal);
	}
}
