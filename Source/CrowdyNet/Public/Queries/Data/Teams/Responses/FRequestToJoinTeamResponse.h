#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"
#include "Queries/Data/Teams/Types/FCrowdyGroupMember.h"

struct FRequestToJoinTeamResponse : ICrowdyQueryResponse
{
	FCrowdyGroupMember Member;

	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::RequestToJoinTeam; }
	virtual FName GetOperationName() const override { return "Request To Join Team Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid()) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* DataObj;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObj)) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* Obj;
		if (!(*DataObj)->TryGetObjectField(TEXT("requestToJoinTeam"), Obj)) { bIsValid = false; return; }

		bIsValid = FCrowdyGroupMember::ParseFromJson(*Obj, Member);
	}
};
