#pragma once
#include "CoreMinimal.h"
#include "Core/GraphQL/Enums/EGraphQLQuery.h"

class CROWDYSDK_API ICrowdyQueryRequest
{
public:
	virtual ~ICrowdyQueryRequest() = default;
	
	TMap<FString, FString> RuntimeVariables;
	bool    bIncludeAuthToken = true;
	bool    bUseNestedJson    = false;

	/**
	 * Optional inline query body.  When non-empty the transmission layer uses
	 * this string directly instead of looking up the data asset.
	 * Used by SDK-internal requests that don't need a user-configurable body.
	 */
	FString InlineQueryBody;
	
	virtual FName GetOperationName() const = 0;
	virtual void PrepareQuery() = 0;
	virtual EGraphQLQuery GetQueryType() const = 0;
	virtual bool IsValid() const = 0;
	
protected:
	bool bIsValid = false;
};
