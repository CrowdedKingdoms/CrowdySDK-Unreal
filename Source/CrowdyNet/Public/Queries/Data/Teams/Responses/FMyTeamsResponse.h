#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"
#include "Queries/Data/Teams/Types/FCrowdyGroupMembership.h"

struct FMyTeamsResponse : ICrowdyQueryResponse
{
	TArray<FCrowdyGroupMembership> Memberships;

	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::MyTeams; }
	virtual FName GetOperationName() const override { return "My Teams Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid()) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* DataObj;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObj)) { bIsValid = false; return; }

		const TArray<TSharedPtr<FJsonValue>>* Arr;
		if (!(*DataObj)->TryGetArrayField(TEXT("myTeams"), Arr)) { bIsValid = false; return; }

		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject>* Obj;
			if (V->TryGetObject(Obj))
			{
				FCrowdyGroupMembership M;
				FCrowdyGroupMembership::ParseFromJson(*Obj, M);
				Memberships.Add(M);
			}
		}
		bIsValid = true;
	}
};
