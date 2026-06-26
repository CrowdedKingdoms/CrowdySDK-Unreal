#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"
#include "Queries/Data/Teams/Types/FCrowdyGroupRole.h"

struct FTeamRolesResponse : ICrowdyQueryResponse
{
	TArray<FCrowdyGroupRole> Roles;

	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::TeamRoles; }
	virtual FName GetOperationName() const override { return "Team Roles Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid()) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* DataObj;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObj)) { bIsValid = false; return; }

		const TArray<TSharedPtr<FJsonValue>>* Arr;
		if (!(*DataObj)->TryGetArrayField(TEXT("teamRoles"), Arr)) { bIsValid = false; return; }

		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject>* Obj;
			if (V->TryGetObject(Obj))
			{
				FCrowdyGroupRole R;
				FCrowdyGroupRole::ParseFromJson(*Obj, R);
				Roles.Add(R);
			}
		}
		Roles.Sort([](const FCrowdyGroupRole& A, const FCrowdyGroupRole& B) { return A.Rank < B.Rank; });
		bIsValid = true;
	}
};
