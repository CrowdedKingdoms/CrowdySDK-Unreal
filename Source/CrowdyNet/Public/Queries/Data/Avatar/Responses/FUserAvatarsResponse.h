#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"
#include "Queries/Data/Avatar/Types/FCrowdyAvatar.h"

struct FUserAvatarsResponse : ICrowdyQueryResponse
{
	TArray<FCrowdyAvatar> Avatars;

	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::GetUserAvatars; }
	virtual FName GetOperationName() const override { return "User Avatars Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		bIsValid = false;

		if (!JsonObject.IsValid()) { ErrorMessage = TEXT("UserAvatars: Invalid JSON"); return; }

		const TSharedPtr<FJsonObject>* DataObj;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObj)) { ErrorMessage = TEXT("UserAvatars: No 'data' field"); return; }

		const TArray<TSharedPtr<FJsonValue>>* Arr;
		if (!(*DataObj)->TryGetArrayField(TEXT("userAvatars"), Arr)) { ErrorMessage = TEXT("UserAvatars: No 'userAvatars' array"); return; }

		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject>* Obj;
			if (V->TryGetObject(Obj))
			{
				FCrowdyAvatar A;
				FCrowdyAvatar::ParseFromJson(*Obj, A);
				Avatars.Add(A);
			}
		}

		bIsValid = true;
	}
};
