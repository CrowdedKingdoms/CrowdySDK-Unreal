#pragma once
#include "CrowdyNetLog.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Serialization/CrowdyFrame.h"
#include "Utils/SerializationFunctionLibrary.h"

struct FTextMessageNotification : ICrowdyMessage
{
	
	int64 UserID;
	FString Username;
	FString Message;
	
	
	virtual ECrowdyMessageType GetType() const override
	{
		return ECrowdyMessageType::CLIENT_TEXT_NOTIFICATION;
	}
	
	virtual FName GetTypeName() const override
	{
		return "Text Message Notification";
	}
	
	virtual TArray<uint8> Serialize() const override
	{
		return TArray<uint8>();
	}
	
	[[nodiscard]] virtual bool DecodePayload(const FCrowdyFrame& Frame) override
	{
		const TConstArrayView<uint8> Data = Frame.Body;
		int32 Offset = 0;

		ApplyEnvelope(Frame);
		if (!ApplyEnvelopeActorId(Frame))
		{
			UE_LOG(LogCrowdyNet, Warning, TEXT("FTextMessageNotification::DecodePayload - the frame carries no actor id"));
			return false;
		}

		if (!USerializationFunctionLibrary::DeserializeValue(Data, UserID, Offset))
		{
			UE_LOG(LogCrowdyNet, Warning, TEXT("FTextMessageNotification::DecodePayload - UserID deserialization failed"));
			 return false;
		}
		Offset += sizeof(UserID);
		
		if (Offset + sizeof(int32) <= Data.Num())
		{
			int32 UsernameLength;
			if (!USerializationFunctionLibrary::DeserializeValue(Data, UsernameLength, Offset))
			{
				UE_LOG(LogCrowdyNet, Warning, TEXT("FTextMessageNotification::DecodePayload - UsernameLength deserialization failed"));
				 return false;
			}
			
			Offset += sizeof(int32);
			
			// Written as a comparison against the bytes remaining rather than as an addition, because a
			// large declared length would overflow the sum and produce a negative value that passes.
			if (UsernameLength > 0 && UsernameLength <= Data.Num() - Offset)
			{
				TArray<ANSICHAR> UsernameBuffer;
				UsernameBuffer.SetNum(UsernameLength + 1);
				FMemory::Memcpy(UsernameBuffer.GetData(), Data.GetData() + Offset, UsernameLength);
				UsernameBuffer[UsernameLength] = '\0';
				Username = FString(UTF8_TO_TCHAR(UsernameBuffer.GetData()));
				Offset += UsernameLength;
			}
			else
			{
				UE_LOG(LogCrowdyNet, Warning, TEXT("FTextMessageNotification::DecodePayload - UsernameBuffer deserialization failed"));
				 return false;
			}
		}
		else
		{
			UE_LOG(LogCrowdyNet, Warning, TEXT("FTextMessageNotification::DecodePayload - UsernameLength deserialization failed"));
			 return false;
		}
		
		if (Offset + sizeof(int32) <= Data.Num())
		{
			int32 MessageLength;
			FMemory::Memcpy(&MessageLength, Data.GetData() + Offset, sizeof(int32));
			Offset += sizeof(int32);
			
			// The same bound in the same form, for the same reason.
			if (MessageLength > 0 && MessageLength <= Data.Num() - Offset)
			{
				TArray<ANSICHAR> MessageBuffer;
				MessageBuffer.SetNum(MessageLength + 1);
				FMemory::Memcpy(MessageBuffer.GetData(), Data.GetData() + Offset, MessageLength);
				MessageBuffer[MessageLength] = '\0';
				Message = FString(UTF8_TO_TCHAR(MessageBuffer.GetData()));
				return true;
			}
			else
			{
				UE_LOG(LogCrowdyNet, Warning, TEXT("FTextMessageNotification::DecodePayload - MessageBuffer deserialization failed"));
				 return false;
			}
		}
		else
		{
			UE_LOG(LogCrowdyNet, Warning, TEXT("FTextMessageNotification::DecodePayload - MessageLength deserialization failed"));
			 return false;
		}
	}
	
};
