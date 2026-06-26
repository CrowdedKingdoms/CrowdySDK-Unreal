#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"
#include "Queries/Data/Teams/Types/FCrowdyGroupMember.h"

struct FAddTeamMemberResponse : ICrowdyQueryResponse
{
	FCrowdyGroupMember Member;

	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::AddTeamMember; }
	virtual FName GetOperationName() const override { return "Add Team Member Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid()) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* DataObj;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObj)) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* Obj;
		if (!(*DataObj)->TryGetObjectField(TEXT("addTeamMember"), Obj)) { bIsValid = false; return; }

		bIsValid = FCrowdyGroupMember::ParseFromJson(*Obj, Member);
	}
};
