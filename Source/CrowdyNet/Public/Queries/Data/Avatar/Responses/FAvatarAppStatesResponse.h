#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"
#include "Queries/Data/Avatar/Types/FCrowdyAppAvatarState.h"

struct FAvatarAppStatesResponse : ICrowdyQueryResponse
{
	TArray<FCrowdyAppAvatarState> AppStates;

	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::GetAvatarAppStates; }
	virtual FName GetOperationName() const override { return "Avatar App States Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		bIsValid = false;

		if (!JsonObject.IsValid()) { ErrorMessage = TEXT("AvatarAppStates: Invalid JSON"); return; }

		const TSharedPtr<FJsonObject>* DataObj;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObj)) { ErrorMessage = TEXT("AvatarAppStates: No 'data' field"); return; }

		const TArray<TSharedPtr<FJsonValue>>* Arr;
		if (!(*DataObj)->TryGetArrayField(TEXT("avatarAppStates"), Arr)) { ErrorMessage = TEXT("AvatarAppStates: No 'avatarAppStates' array"); return; }

		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject>* Obj;
			if (V->TryGetObject(Obj))
			{
				FCrowdyAppAvatarState S;
				FCrowdyAppAvatarState::ParseFromJson(*Obj, S);
				AppStates.Add(S);
			}
		}

		bIsValid = true;
	}
};
