#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"

struct FDeleteTeamResponse : ICrowdyQueryResponse
{
	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::DeleteTeam; }
	virtual FName GetOperationName() const override { return "Delete Team Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid()) { bIsValid = false; return; }
		const TSharedPtr<FJsonObject>* DataObj;
		bIsValid = JsonObject->TryGetObjectField(TEXT("data"), DataObj);
	}
};
