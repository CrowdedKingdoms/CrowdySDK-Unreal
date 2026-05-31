#pragma once
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
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
	
	virtual bool Deserialize(const TArray<uint8>& Data) override
	{
		int32 Offset = 0;
		
		if (!DeserializeMetadata(Data, Offset))
		{
			UE_LOG(LogTemp, Warning, TEXT("FTextMessageNotification::Deserialize - Metadata deserialization failed"));
			return false;
		}
		
		if (!USerializationFunctionLibrary::DeserializeValue(Data, UserID, Offset))
		{
			UE_LOG(LogTemp, Warning, TEXT("FTextMessageNotification::Deserialize - UserID deserialization failed"));
			 return false;
		}
		Offset += sizeof(UserID);
		
		if (Offset + sizeof(int32) <= Data.Num())
		{
			int32 UsernameLength;
			if (!USerializationFunctionLibrary::DeserializeValue(Data, UsernameLength, Offset))
			{
				UE_LOG(LogTemp, Warning, TEXT("FTextMessageNotification::Deserialize - UsernameLength deserialization failed"));
				 return false;
			}
			
			Offset += sizeof(int32);
			
			if (UsernameLength > 0 && Offset + UsernameLength <= Data.Num())
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
				UE_LOG(LogTemp, Warning, TEXT("FTextMessageNotification::Deserialize - UsernameBuffer deserialization failed"));
				 return false;
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("FTextMessageNotification::Deserialize - UsernameLength deserialization failed"));
			 return false;
		}
		
		if (Offset + sizeof(int32) <= Data.Num())
		{
			int32 MessageLength;
			FMemory::Memcpy(&MessageLength, Data.GetData() + Offset, sizeof(int32));
			Offset += sizeof(int32);
			
			if (MessageLength > 0 && Offset + MessageLength <= Data.Num())
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
				UE_LOG(LogTemp, Warning, TEXT("FTextMessageNotification::Deserialize - MessageBuffer deserialization failed"));
				 return false;
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("FTextMessageNotification::Deserialize - MessageLength deserialization failed"));
			 return false;
		}
	}
	
	virtual uint32 GetMessageSize() const override
	{
		return DataCopy.Num();
	}
	
private:
	TArray<uint8> DataCopy; 	
};
