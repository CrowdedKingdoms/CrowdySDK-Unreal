#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"

struct FRemoveTeamMemberResponse : ICrowdyQueryResponse
{
	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::RemoveTeamMember; }
	virtual FName GetOperationName() const override { return "Remove Team Member Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid()) { bIsValid = false; return; }
		const TSharedPtr<FJsonObject>* DataObj;
		bIsValid = JsonObject->TryGetObjectField(TEXT("data"), DataObj);
	}
};
