#pragma once
#include "CoreMinimal.h"
#include "Core/GraphQL/Enums/EQueryResponseType.h"
#include "Dom/JsonObject.h"

class CROWDYSDK_API ICrowdyQueryResponse
{
public:
	
	FString ErrorMessage;
	
	virtual ~ICrowdyQueryResponse() = default;
	virtual EQueryResponseType GetResponseType() const = 0;
	virtual FName GetOperationName() const = 0;
	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) = 0;
	
	FString GetError() const
	{
		return ErrorMessage;
	}
	
	bool IsValid() const
	{
		return bIsValid;
	}

	/** Mark this response as failed. Safe to call from any thread before dispatch. */
	void MarkInvalid(const FString& Error)
	{
		ErrorMessage = Error;
		bIsValid = false;
	}

protected:

	bool bIsValid = true;
};