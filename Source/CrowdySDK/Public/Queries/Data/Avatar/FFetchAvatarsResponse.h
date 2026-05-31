#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"

struct FFetchAvatarsResponse : ICrowdyQueryResponse
{
	struct FRawAvatar
	{
		int64 ID = 0;
		FString Name;
		FString PublicStateString;
	};
	
	TArray<FRawAvatar> Avatars;
	
	virtual EQueryResponseType GetResponseType() const override
	{
		return EQueryResponseType::MyAvatars;
	}
	
	virtual FName GetOperationName() const override
	{
		return "FetchAvatarsResponse";
	}
	
	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		bIsValid = false;
		
		if (!JsonObject.IsValid())
		{
			ErrorMessage = TEXT("My Avatars: Json Object is invalid");
			return;
		}
		
		const TSharedPtr<FJsonObject>* DataObject;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObject) || !DataObject || !DataObject->IsValid())
		{
			ErrorMessage =  TEXT("FetchAvatars: No 'data' field in response");
			return;
		}
		
		const TArray<TSharedPtr<FJsonValue>>* MyAvatarsArray = nullptr;
		
		if (!(*DataObject)->TryGetArrayField(TEXT("myAvatars"), MyAvatarsArray) || !MyAvatarsArray)
		{
			ErrorMessage =  TEXT("FetchAvatars: No 'myAvatars' field in data");
			return;
		}
		
		for (const TSharedPtr<FJsonValue>& AvatarValue : *MyAvatarsArray)
		{
			const TSharedPtr<FJsonObject>* AvatarObject = nullptr;
			if (!AvatarValue.IsValid() || !AvatarValue->TryGetObject(AvatarObject) || !AvatarObject || !AvatarObject->IsValid())
			{
				ErrorMessage =  TEXT("FetchAvatars: Invalid avatar object in array");
				continue;
			}

			FRawAvatar Raw;

			// avatarId (string) -> int64
			FString AvatarIdString;
			if ((*AvatarObject)->TryGetStringField(TEXT("avatarId"), AvatarIdString))
			{
				Raw.ID = FCString::Atoi64(*AvatarIdString);
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("FetchAvatars: Missing avatarId field"));
				continue;
			}

			// name
			if (!(*AvatarObject)->TryGetStringField(TEXT("name"), Raw.Name))
			{
				UE_LOG(LogTemp, Warning, TEXT("FetchAvatars: Missing name field for avatar %lld"), Raw.ID);
				Raw.Name = TEXT("");
			}

			// publicState as raw string (can be null/missing/empty)
			(*AvatarObject)->TryGetStringField(TEXT("publicState"), Raw.PublicStateString);

			Avatars.Add(MoveTemp(Raw));

			UE_LOG(LogTemp, Log, TEXT("FetchAvatars: Parsed raw avatar - ID: %lld, Name: %s"),
				Avatars.Last().ID, *Avatars.Last().Name);
		}
		
		bIsValid = true;
	}
};
