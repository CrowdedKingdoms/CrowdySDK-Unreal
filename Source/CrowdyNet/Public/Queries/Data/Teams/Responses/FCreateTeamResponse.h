#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"
#include "Queries/Data/Teams/Types/FCrowdyGroup.h"

struct FCreateTeamResponse : ICrowdyQueryResponse
{
	FCrowdyGroup Group;

	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::CreateTeam; }
	virtual FName GetOperationName() const override { return "Create Team Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid()) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* DataObj;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObj)) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* Obj;
		if (!(*DataObj)->TryGetObjectField(TEXT("createTeam"), Obj)) { bIsValid = false; return; }

		bIsValid = FCrowdyGroup::ParseFromJson(*Obj, Group);
	}
};
