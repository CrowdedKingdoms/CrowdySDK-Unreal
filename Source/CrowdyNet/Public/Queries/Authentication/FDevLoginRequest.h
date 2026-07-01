#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryRequest.h"
#include "Core/GraphQL/Enums/EGraphQLQuery.h"

/**
 * Dev/test bypass sign-in: returns a SESSION token for an email with no email or
 * social verification. Active only when the server runs DEV_AUTH_BYPASS; throws
 * FORBIDDEN in production. Public. Uses an inline query body.
 */
struct FDevLoginRequest : ICrowdyQueryRequest
{
	FString Email;

	virtual FName GetOperationName() const override
	{
		return TEXT("Dev Login");
	}

	virtual EGraphQLQuery GetQueryType() const override
	{
		return EGraphQLQuery::DevLogin;
	}

	virtual void PrepareQuery() override
	{
		RuntimeVariables.Empty();
		RuntimeVariables.Add(TEXT("email"), Email);

		InlineQueryBody  = GetStaticQueryBody();
		bIncludeAuthToken = false;
		bUseNestedJson    = false;
		bIsValid          = true;
	}

	virtual bool IsValid() const override
	{
		return !Email.IsEmpty();
	}

private:
	static const FString& GetStaticQueryBody()
	{
		static const FString Body =
			TEXT("mutation DevLogin($email: String!) {")
			TEXT("  devLogin(input: { email: $email }) {")
			TEXT("    token")
			TEXT("    gameTokenId")
			TEXT("    user { userId email }")
			TEXT("  }")
			TEXT("}");
		return Body;
	}
};
