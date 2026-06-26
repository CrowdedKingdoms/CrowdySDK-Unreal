#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"
#include "Queries/Data/Teams/Types/FCrowdyGroupMember.h"

struct FTeamMembersResponse : ICrowdyQueryResponse
{
	TArray<FCrowdyGroupMember> Members;

	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::TeamMembers; }
	virtual FName GetOperationName() const override { return "Team Members Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid()) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* DataObj;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObj)) { bIsValid = false; return; }

		const TArray<TSharedPtr<FJsonValue>>* Arr;
		if (!(*DataObj)->TryGetArrayField(TEXT("teamMembers"), Arr)) { bIsValid = false; return; }

		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject>* Obj;
			if (V->TryGetObject(Obj))
			{
				FCrowdyGroupMember M;
				FCrowdyGroupMember::ParseFromJson(*Obj, M);
				Members.Add(M);
			}
		}
		bIsValid = true;
	}
};
