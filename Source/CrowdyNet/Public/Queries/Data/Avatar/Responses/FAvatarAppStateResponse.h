#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"
#include "Queries/Data/Avatar/Types/FCrowdyAppAvatarState.h"

struct FAvatarAppStateResponse : ICrowdyQueryResponse
{
	FCrowdyAppAvatarState AppState;

	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::GetAvatarAppState; }
	virtual FName GetOperationName() const override { return "Avatar App State Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		bIsValid = false;

		if (!JsonObject.IsValid()) { ErrorMessage = TEXT("AvatarAppState: Invalid JSON"); return; }

		const TSharedPtr<FJsonObject>* DataObj;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObj)) { ErrorMessage = TEXT("AvatarAppState: No 'data' field"); return; }

		const TSharedPtr<FJsonObject>* StateObj;
		if (!(*DataObj)->TryGetObjectField(TEXT("avatarAppState"), StateObj)) { ErrorMessage = TEXT("AvatarAppState: No 'avatarAppState' field"); return; }

		if (!FCrowdyAppAvatarState::ParseFromJson(*StateObj, AppState)) { ErrorMessage = TEXT("AvatarAppState: Failed to parse state"); return; }

		bIsValid = true;
	}
};
