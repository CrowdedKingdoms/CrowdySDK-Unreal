#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"
#include "Queries/Data/Teams/Types/FCrowdyAppGroupPolicy.h"

struct FSetTeamPolicyResponse : ICrowdyQueryResponse
{
	FCrowdyAppGroupPolicy Policy;

	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::SetTeamPolicy; }
	virtual FName GetOperationName() const override { return "Set Team Policy Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid()) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* DataObj;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObj)) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* Obj;
		if (!(*DataObj)->TryGetObjectField(TEXT("setTeamPolicy"), Obj)) { bIsValid = false; return; }

		bIsValid = FCrowdyAppGroupPolicy::ParseFromJson(*Obj, Policy);
	}
};
