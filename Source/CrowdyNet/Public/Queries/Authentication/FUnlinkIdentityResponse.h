#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"

/** Response for the unlinkIdentity mutation: whether the identity was removed. */
struct FUnlinkIdentityResponse : ICrowdyQueryResponse
{
	bool bRemoved = false;

	virtual EQueryResponseType GetResponseType() const override
	{
		return EQueryResponseType::UnlinkIdentity;
	}

	virtual FName GetOperationName() const override
	{
		return TEXT("Unlink Identity Response");
	}

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid())
		{
			MarkInvalid(TEXT("Invalid JSON Object"));
			return;
		}

		const TSharedPtr<FJsonObject>* DataObject = nullptr;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObject) || !DataObject->IsValid())
		{
			MarkInvalid(TEXT("Invalid Data JSON Object"));
			return;
		}

		if (!(*DataObject)->TryGetBoolField(TEXT("unlinkIdentity"), bRemoved))
		{
			MarkInvalid(TEXT("Missing root field 'unlinkIdentity'"));
			return;
		}
	}
};
