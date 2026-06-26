#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"

struct FUDPAddressNotify : public ICrowdyQueryResponse
{
	bool bGateKeep = false;
	FString IPv6Address;
	FString IPv4Address;
	uint16 Port;
	
	
	virtual EQueryResponseType GetResponseType() const override
	{
		return EQueryResponseType::UDP_Info;
	}
	
	virtual FName GetOperationName() const override
	{
		return "UDP Address Notification";
	}
	
	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (bGateKeep)
		{
			bIsValid = true;
			return;
		}
		
		if (!JsonObject.IsValid())
		{
			ErrorMessage = TEXT("Json Object is invalid.");
			bIsValid = false;
			return;
		}
		
		const TSharedPtr<FJsonObject> DataObject = JsonObject->GetObjectField(TEXT("data"));

		if (!DataObject.IsValid())
		{
			ErrorMessage = TEXT("Data object is invalid.");
			bIsValid = false;
			return;
		}

		const TSharedPtr<FJsonObject> ServerObject = DataObject->GetObjectField(TEXT("serverWithLeastClients"));
		
		if (!ServerObject.IsValid())
		{
			ErrorMessage = TEXT("Server object is invalid.");
			bIsValid = false;
			return;
		}
		
		if (!ServerObject->TryGetStringField(TEXT("ip6"), IPv6Address))
		{
			ErrorMessage = TEXT("IPv6 address is invalid. Trying v4");
		}

		if (!ServerObject->TryGetStringField(TEXT("ip4"), IPv4Address))
		{
			ErrorMessage = TEXT("IPv4 and IPv6 addresses are invalid.");
			bIsValid = false;
			return;
		}

		if (!ServerObject->TryGetNumberField(TEXT("clientPort"), Port))
		{
			ErrorMessage = TEXT("Port is invalid.");
			bIsValid = false;
		}
	}
	
};