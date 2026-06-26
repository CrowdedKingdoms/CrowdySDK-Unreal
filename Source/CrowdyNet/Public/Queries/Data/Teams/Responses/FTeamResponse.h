#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"
#include "Queries/Data/Teams/Types/FCrowdyGroup.h"

struct FTeamResponse : ICrowdyQueryResponse
{
	FCrowdyGroup Group;

	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::Team; }
	virtual FName GetOperationName() const override { return "Team Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid()) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* DataObj;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObj)) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* Obj;
		if (!(*DataObj)->TryGetObjectField(TEXT("team"), Obj)) { bIsValid = false; return; }

		bIsValid = FCrowdyGroup::ParseFromJson(*Obj, Group);
	}
};
