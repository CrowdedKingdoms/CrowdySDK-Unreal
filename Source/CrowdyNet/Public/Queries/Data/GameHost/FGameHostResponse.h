#pragma once
#include "Core/GraphQL/Enums/EQueryResponseType.h"
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"

struct FGameHostResponse : ICrowdyQueryResponse
{
	int64   HostUserId             = 0;
	int32   ActorCount             = 0;
	FString EarliestActorJoinedAt;

	virtual EQueryResponseType GetResponseType() const override
	{
		return EQueryResponseType::GameHost;
	}

	virtual FName GetOperationName() const override
	{
		return TEXT("Game Host Response");
	}

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		bIsValid = false;

		if (!JsonObject.IsValid())
		{
			ErrorMessage = TEXT("GameHost: Invalid JSON object.");
			return;
		}

		const TSharedPtr<FJsonObject>* DataObject;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObject) || !DataObject->IsValid())
		{
			ErrorMessage = TEXT("GameHost: Missing 'data' field.");
			return;
		}

		const TSharedPtr<FJsonObject>* GameHostObject;
		if (!(*DataObject)->TryGetObjectField(TEXT("gameHost"), GameHostObject) || !GameHostObject->IsValid())
		{
			ErrorMessage = TEXT("GameHost: Missing 'gameHost' field in data.");
			return;
		}

		// hostUserId comes back as a BigInt string
		FString HostUserIdStr;
		if ((*GameHostObject)->TryGetStringField(TEXT("hostUserId"), HostUserIdStr))
			HostUserId = FCString::Atoi64(*HostUserIdStr);

		double ActorCountVal = 0.0;
		if ((*GameHostObject)->TryGetNumberField(TEXT("actorCount"), ActorCountVal))
			ActorCount = static_cast<int32>(ActorCountVal);

		(*GameHostObject)->TryGetStringField(TEXT("earliestActorJoinedAt"), EarliestActorJoinedAt);

		bIsValid = true;
	}
};
