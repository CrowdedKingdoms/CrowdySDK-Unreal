#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"

/** Response for the availableLoginProviders query: the enabled provider ids. */
struct FAvailableLoginProvidersResponse : ICrowdyQueryResponse
{
	TArray<FString> Providers;

	virtual EQueryResponseType GetResponseType() const override
	{
		return EQueryResponseType::AvailableLoginProviders;
	}

	virtual FName GetOperationName() const override
	{
		return TEXT("Available Login Providers Response");
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
		if (!(*DataObject)->TryGetArrayField(TEXT("availableLoginProviders"), Array))
		{
			MarkInvalid(TEXT("Missing root field 'availableLoginProviders'"));
			return;
		}

		Providers.Reset();
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			FString Provider;
			if (Value.IsValid() && Value->TryGetString(Provider) && !Provider.IsEmpty())
			{
				Providers.Add(Provider);
			}
		}
	}
};
