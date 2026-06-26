#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"
#include "Queries/Data/Avatar/Types/FCrowdyAppAvatarState.h"

struct FUpdateAvatarAppStateResponse : ICrowdyQueryResponse
{
	FCrowdyAppAvatarState AppState;

	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::UpdateAvatarAppState; }
	virtual FName GetOperationName() const override { return "Update Avatar App State Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		bIsValid = false;

		if (!JsonObject.IsValid()) { ErrorMessage = TEXT("UpdateAvatarAppState: Invalid JSON"); return; }

		const TSharedPtr<FJsonObject>* DataObj;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObj)) { ErrorMessage = TEXT("UpdateAvatarAppState: No 'data' field"); return; }

		const TSharedPtr<FJsonObject>* StateObj;
		if (!(*DataObj)->TryGetObjectField(TEXT("updateAvatarAppState"), StateObj)) { ErrorMessage = TEXT("UpdateAvatarAppState: No 'updateAvatarAppState' field"); return; }

		if (!FCrowdyAppAvatarState::ParseFromJson(*StateObj, AppState)) { ErrorMessage = TEXT("UpdateAvatarAppState: Failed to parse state"); return; }

		bIsValid = true;
	}
};
