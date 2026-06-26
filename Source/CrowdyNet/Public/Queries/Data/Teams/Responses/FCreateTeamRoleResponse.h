#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"
#include "Queries/Data/Teams/Types/FCrowdyGroupRole.h"

struct FCreateTeamRoleResponse : ICrowdyQueryResponse
{
	FCrowdyGroupRole Role;

	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::CreateTeamRole; }
	virtual FName GetOperationName() const override { return "Create Team Role Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid()) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* DataObj;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObj)) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* Obj;
		if (!(*DataObj)->TryGetObjectField(TEXT("createTeamRole"), Obj)) { bIsValid = false; return; }

		bIsValid = FCrowdyGroupRole::ParseFromJson(*Obj, Role);
	}
};
