#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"
#include "Queries/Data/Avatar/Types/FCrowdyAvatar.h"

struct FMyAvatarsResponse : ICrowdyQueryResponse
{
	TArray<FCrowdyAvatar> Avatars;

	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::MyAvatars; }
	virtual FName GetOperationName() const override { return "My Avatars Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		bIsValid = false;

		if (!JsonObject.IsValid()) { ErrorMessage = TEXT("MyAvatars: Invalid JSON"); return; }

		const TSharedPtr<FJsonObject>* DataObj;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObj)) { ErrorMessage = TEXT("MyAvatars: No 'data' field"); return; }

		const TArray<TSharedPtr<FJsonValue>>* Arr;
		if (!(*DataObj)->TryGetArrayField(TEXT("myAvatars"), Arr)) { ErrorMessage = TEXT("MyAvatars: No 'myAvatars' array"); return; }

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
