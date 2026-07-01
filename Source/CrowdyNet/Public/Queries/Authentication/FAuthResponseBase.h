#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"

/**
 * Shared parsing for passwordless sign-in responses. Every sign-in path
 * (completeLoginLink, devLogin, socialLoginComplete) returns the same
 * AuthResponse payload { token, gameTokenId, user { userId email } } under a
 * per-operation root field. Subclasses supply the root field name and the
 * EQueryResponseType.
 *
 * NOTE: token here is the identity SESSION token (management-plane only). It is
 * NOT a gameplay credential. An app-scoped token must be minted from it before
 * the Game API / UDP surface will accept the player. See FMintAppTokenResponse.
 */
struct FAuthResponseBase : ICrowdyQueryResponse
{
	FString SessionToken;
	int64   SessionGameTokenID = 0;
	int64   UserID = 0;
	FString Email;

protected:
	void ParseAuthPayload(const TSharedPtr<FJsonObject>& JsonObject, const TCHAR* RootField)
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

		const TSharedPtr<FJsonObject>* RootObject = nullptr;
		if (!(*DataObject)->TryGetObjectField(RootField, RootObject) || !RootObject->IsValid())
		{
			MarkInvalid(FString::Printf(TEXT("Missing root field '%s'"), RootField));
			return;
		}

		if (!(*RootObject)->TryGetStringField(TEXT("token"), SessionToken) || SessionToken.IsEmpty())
		{
			MarkInvalid(TEXT("Missing session token"));
			return;
		}

		FString GameTokenIDStr;
		if ((*RootObject)->TryGetStringField(TEXT("gameTokenId"), GameTokenIDStr) && !GameTokenIDStr.IsEmpty())
		{
			SessionGameTokenID = FCString::Atoi64(*GameTokenIDStr);
		}

		const TSharedPtr<FJsonObject>* UserObject = nullptr;
		if ((*RootObject)->TryGetObjectField(TEXT("user"), UserObject) && UserObject->IsValid())
		{
			FString UserIdStr;
			if ((*UserObject)->TryGetStringField(TEXT("userId"), UserIdStr))
			{
				UserID = FCString::Atoi64(*UserIdStr);
			}
			(*UserObject)->TryGetStringField(TEXT("email"), Email);
		}
	}
};
