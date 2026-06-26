#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"
#include "Queries/Data/Teams/Types/FCrowdyGroup.h"

struct FTeamsResponse : ICrowdyQueryResponse
{
	TArray<FCrowdyGroup> Groups;

	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::Teams; }
	virtual FName GetOperationName() const override { return "Teams Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid()) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* DataObj;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObj)) { bIsValid = false; return; }

		const TArray<TSharedPtr<FJsonValue>>* Arr;
		if (!(*DataObj)->TryGetArrayField(TEXT("teams"), Arr)) { bIsValid = false; return; }

		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject>* Obj;
			if (V->TryGetObject(Obj))
			{
				FCrowdyGroup G;
				FCrowdyGroup::ParseFromJson(*Obj, G);
				Groups.Add(G);
			}
		}
		bIsValid = true;
	}
};
