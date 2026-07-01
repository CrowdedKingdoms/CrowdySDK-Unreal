#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"
#include "Queries/Authentication/FCrowdyUserIdentity.h"

/** Response for the myIdentities query: the user's linked sign-in identities. */
struct FMyIdentitiesResponse : ICrowdyQueryResponse
{
	TArray<FCrowdyUserIdentity> Identities;

	virtual EQueryResponseType GetResponseType() const override
	{
		return EQueryResponseType::MyIdentities;
	}

	virtual FName GetOperationName() const override
	{
		return TEXT("My Identities Response");
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

		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!(*DataObject)->TryGetArrayField(TEXT("myIdentities"), Array))
		{
			MarkInvalid(TEXT("Missing root field 'myIdentities'"));
			return;
		}

		Identities.Reset();
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (Value.IsValid() && Value->TryGetObject(Obj) && Obj->IsValid())
			{
				Identities.Add(FCrowdyUserIdentity::FromJson(*Obj));
			}
		}
	}
};
