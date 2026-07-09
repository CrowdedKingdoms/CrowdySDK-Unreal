#pragma once
#include "Core/GraphQL/Enums/EQueryResponseType.h"
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"

// Response for `actor(uuid: String!) { uuid userId }`. Resolves the server-side owner
// user id of an actor by its 32-char wire uuid. Used to answer "is this (arbitrary)
// actor the host" by comparing its owner userId to the elected gameHost hostUserId.
// The `uuid` is echoed back so a caller can correlate the answer to the request it made.
struct FActorOwnerResponse : ICrowdyQueryResponse
{
	FString Uuid;
	int64   UserId = 0;

	virtual EQueryResponseType GetResponseType() const override
	{
		return EQueryResponseType::ActorOwner;
	}

	virtual FName GetOperationName() const override
	{
		return TEXT("Actor Owner Response");
	}

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		bIsValid = false;

		if (!JsonObject.IsValid())
		{
			ErrorMessage = TEXT("ActorOwner: Invalid JSON object.");
			return;
		}

		const TSharedPtr<FJsonObject>* DataObject;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObject) || !DataObject->IsValid())
		{
			ErrorMessage = TEXT("ActorOwner: Missing 'data' field.");
			return;
		}

		const TSharedPtr<FJsonObject>* ActorObject;
		if (!(*DataObject)->TryGetObjectField(TEXT("actor"), ActorObject) || !ActorObject->IsValid())
		{
			ErrorMessage = TEXT("ActorOwner: Missing 'actor' field in data.");
			return;
		}

		(*ActorObject)->TryGetStringField(TEXT("uuid"), Uuid);

		// userId comes back as a BigInt decimal string.
		FString UserIdStr;
		if (!(*ActorObject)->TryGetStringField(TEXT("userId"), UserIdStr))
		{
			ErrorMessage = TEXT("ActorOwner: Missing 'userId' field in actor.");
			return;
		}
		UserId = FCString::Atoi64(*UserIdStr);

		bIsValid = true;
	}
};
