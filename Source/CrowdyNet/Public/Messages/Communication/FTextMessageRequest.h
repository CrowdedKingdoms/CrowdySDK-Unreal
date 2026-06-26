#pragma once
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"

struct FTextMessageRequest : ICrowdyMessage
{
	int64 UserID;
	FString Username;
	FString Message;
	
	virtual ECrowdyMessageType GetType() const override
	{
		return ECrowdyMessageType::CLIENT_TEXT_PACKET;
	}
	
	virtual FName GetTypeName() const override
	{
		return "Text Message Request";
	}
	
	virtual TArray<uint8> Serialize() const override
	{
		TArray<uint8> Data = SerializeMetadata();
		
		// Write UserID
		Data.Append(reinterpret_cast<const uint8*>(&UserID), sizeof(int64));
    
		// Write Username
		const FTCHARToUTF8 ConvertedUsername(*Username);
		const int32 UsernameLen = ConvertedUsername.Length();  // Get actual length
		Data.Append(reinterpret_cast<const uint8*>(&UsernameLen), sizeof(int32));
		Data.Append(reinterpret_cast<const uint8*>(ConvertedUsername.Get()), UsernameLen);
    
		// Write Message
		const FTCHARToUTF8 ConvertedMessage(*Message);
		const int32 MessageLen = ConvertedMessage.Length();  // Get actual length
		Data.Append(reinterpret_cast<const uint8*>(&MessageLen), sizeof(int32));
		Data.Append(reinterpret_cast<const uint8*>(ConvertedMessage.Get()), MessageLen);
		
		return Data;
	}
	
	virtual bool Deserialize(const TArray<uint8>& Data) override
	{
		return false;
	}
	
	virtual uint32 GetMessageSize() const override
	{
		return sizeof(AppID) + sizeof(int64)*3 + 32 + sizeof(int32)*2 + Message.Len();
	}
	
};