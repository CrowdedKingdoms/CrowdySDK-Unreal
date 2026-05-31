#include "FCrowdyMessageParser.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Internal/FCrowdyServiceRegistry.h"
#include "Messages/FDefaultMessage.h"
#include "Messages/FPingTestMessage.h"
#include "Messages/Actor/FActorUpdateNotificationMessage.h"
#include "Messages/Actor/FActorUpdateResponseMessage.h"
#include "Messages/Communication/FClientAudioNotification.h"
#include "Messages/GameObjects/FGameEventNotification.h"
#include "Messages/GameObjects/FGameObjectActivationNotification.h"
#include "Messages/Voxel/FVoxelUpdateNotificationMessage.h"
#include "Messages/Voxel/FVoxelUpdateResponseMessage.h"
#include "Network/UDP/CrowdyUDPSubsystem.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Subsystem/CrowdySDKSubsystem.h"

FCrowdyMessageParser::FCrowdyMessageParser(FCrowdyServiceRegistry* InServiceRegistry,
                                           UCrowdyUDPSubsystem* InUDPSubsystem, UCrowdySDKSubsystem* InSDK, UCrowdyGameSession* InGameSession)
{
	ServiceRegistry = InServiceRegistry;
	UDPSubsystem = InUDPSubsystem;
	SDK = InSDK;
	GameSession = InGameSession;
}

TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe> FCrowdyMessageParser::ParseMessage(const TArray<uint8>& Data)
{
	if (Data.IsEmpty())
	{
		return MakeShared<FDefaultMessage>();
	}
	
	const ECrowdyMessageType MessageType = static_cast<ECrowdyMessageType>(Data[0]);
	
	if (MessageType == ECrowdyMessageType::MESSAGE_BUNDLE)
	{
		int32 Offset = 1;
		TArray<TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe>> MessagesInBundle;

		while (Offset < Data.Num())
		{
			if (Offset + 2 > Data.Num())
				break;

			uint16 MessageLength = 0;
			FMemory::Memcpy(&MessageLength, Data.GetData() + Offset, 2);
			Offset += 2;

			if (Offset + MessageLength > Data.Num())
				break;

			if (MessageLength > 0)
			{
				TArray<uint8> EmbeddedMessage;
				EmbeddedMessage.SetNumUninitialized(MessageLength);
				FMemory::Memcpy(EmbeddedMessage.GetData(), Data.GetData() + Offset, MessageLength);

				// Recursively parse each embedded message
				MessagesInBundle.Add(ParseMessage(EmbeddedMessage));
			}

			Offset += MessageLength;
		}

		// Dispatch all messages in the bundle
		for (auto& Msg : MessagesInBundle)
		{
			ServiceRegistry->DispatchMessage(Msg);
			UDPSubsystem->IncrementReceivedMessageCount();
		}

		return MakeShared<FDefaultMessage>(); // bundle itself doesn't count as a message
	}
	
	
	const TArray Payload(Data.GetData() + 1, Data.Num() - 1);

	//UDPSubsystem->IncrementReceivedMessageCount();
	
	switch (MessageType)
	{
	case ECrowdyMessageType::ACTOR_UPDATE_NOTIFICATION:
		{
			TSharedRef<FActorUpdateNotificationMessage, ESPMode::ThreadSafe> Message = MakeShared<FActorUpdateNotificationMessage>();
			Message->SetExpectedStateSize(ExpectedActorStateSize);
			
			if (!Message->Deserialize(Payload))
			{
				return MakeShared<FDefaultMessage>();
			}
			
			if (Message->UUID == GameSession->GetUUID())
				SDK->TriggerUdpHeartbeat();
			
			return Message;
		}
	case ECrowdyMessageType::ACTOR_UPDATE_RESPONSE:
		{
			TSharedRef<FActorUpdateResponseMessage> Message = MakeShared<FActorUpdateResponseMessage>();
			if (!Message->Deserialize(Payload))
			{
				return MakeShared<FDefaultMessage>();
			}
			return Message;
		}
	case ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION:
		{
			TSharedRef<FGameEventNotification> Message = MakeShared<FGameEventNotification>();
			if (!Message->Deserialize(Payload))
			{
				UDPSubsystem->IncrementTotalClientNotifiesReceived();
				return MakeShared<FDefaultMessage>();
			}
			UDPSubsystem->IncrementTotalClientNotifiesReceived();
			return Message;
		}
	case ECrowdyMessageType::VOXEL_UPDATE_NOTIFICATION:
		{
			TSharedRef<FVoxelUpdateNotificationMessage> Message = MakeShared<FVoxelUpdateNotificationMessage>();
			if (!Message->Deserialize(Payload))
			{
				return MakeShared<FDefaultMessage>();
			}
			return Message;
		}
	case ECrowdyMessageType::VOXEL_UPDATE_RESPONSE:
		{
			TSharedRef<FVoxelUpdateResponseMessage> Message = MakeShared<FVoxelUpdateResponseMessage>();
			
			if (!Message->Deserialize(Payload))
			{
				return MakeShared<FDefaultMessage>();
			}
			return Message;
		}
	case ECrowdyMessageType::CLIENT_AUDIO_NOTIFICATION:
		{
			TSharedRef<FClientAudioNotification> Message = MakeShared<FClientAudioNotification>();
			if (!Message->Deserialize(Payload))
			{
				return MakeShared<FDefaultMessage>();
			}
			return Message;
		}
	case ECrowdyMessageType::GENERIC_ERROR_MESSAGE:
		{
			TSharedRef<FVoxelUpdateResponseMessage> Message = MakeShared<FVoxelUpdateResponseMessage>();
			if (!Message->Deserialize(Payload))
			{
				return MakeShared<FDefaultMessage>();
			}
			return Message;
		}
	case ECrowdyMessageType::GENERIC_SPATIAL_1:
		{
			TSharedRef<FPingTestMessage> Message = MakeShared<FPingTestMessage>();
			if (!Message->Deserialize(Payload))
			{
				return MakeShared<FDefaultMessage>();
			}
			return Message;
		}
		
	default:
		return MakeShared<FDefaultMessage>();
	}
	
}

void FCrowdyMessageParser::SetExpectedActorStateSize(const int32 NewSize)
{
	ExpectedActorStateSize = NewSize;
}
