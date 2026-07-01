#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryRequest.h"
#include "Core/GraphQL/Enums/EGraphQLQuery.h"

/**
 * Unlink a federated identity from the signed-in account by identityId. Requires the
 * SESSION token; the server refuses to remove the last remaining sign-in method.
 * identityId is a flat String! argument (not an input object). Inline body.
 */
struct FUnlinkIdentityRequest : ICrowdyQueryRequest
{
	FString IdentityId;

	virtual FName GetOperationName() const override
	{
		return TEXT("Unlink Identity");
	}

	virtual EGraphQLQuery GetQueryType() const override
	{
		return EGraphQLQuery::UnlinkIdentity;
	}

	virtual void PrepareQuery() override
	{
		RuntimeVariables.Empty();
		RuntimeVariables.Add(TEXT("identityId"), IdentityId);

		InlineQueryBody   = GetStaticQueryBody();
		bIncludeAuthToken = true; // session token, attached by descriptor scope
		bUseNestedJson    = false;
		bIsValid          = true;
	}

	virtual bool IsValid() const override
	{
		return !IdentityId.IsEmpty();
	}

private:
	static const FString& GetStaticQueryBody()
	{
		static const FString Body =
			TEXT("mutation UnlinkIdentity($identityId: String!) {")
			TEXT("  unlinkIdentity(identityId: $identityId)")
			TEXT("}");
		return Body;
	}
};
