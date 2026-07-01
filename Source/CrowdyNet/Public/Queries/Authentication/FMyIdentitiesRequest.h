#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryRequest.h"
#include "Core/GraphQL/Enums/EGraphQLQuery.h"

/**
 * Query the signed-in user's linked sign-in identities. Requires the SESSION token
 * (Management plane; attached by the descriptor's Session scope). Inline body, no
 * variables.
 */
struct FMyIdentitiesRequest : ICrowdyQueryRequest
{
	virtual FName GetOperationName() const override
	{
		return TEXT("My Identities");
	}

	virtual EGraphQLQuery GetQueryType() const override
	{
		return EGraphQLQuery::MyIdentities;
	}

	virtual void PrepareQuery() override
	{
		RuntimeVariables.Empty();

		InlineQueryBody   = GetStaticQueryBody();
		bIncludeAuthToken = true; // session token, attached by descriptor scope
		bUseNestedJson    = false;
		bIsValid          = true;
	}

	virtual bool IsValid() const override
	{
		return true;
	}

private:
	static const FString& GetStaticQueryBody()
	{
		static const FString Body =
			TEXT("query MyIdentities {")
			TEXT("  myIdentities {")
			TEXT("    identityId")
			TEXT("    userId")
			TEXT("    provider")
			TEXT("    subject")
			TEXT("    email")
			TEXT("    emailVerified")
			TEXT("    createdAt")
			TEXT("    lastLoginAt")
			TEXT("  }")
			TEXT("}");
		return Body;
	}
};
