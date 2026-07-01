#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"

/**
 * Shared parsing for app-token responses (mintAppToken, refreshAppToken). The
 * returned token is the app-scoped GAMEPLAY credential: the Bearer for the app's
 * Game API + realtime surface and the HMAC key for its UDP traffic (the 64
 * characters as-is; do not hex-decode). It is short-lived (~30 min); rotates
 * before expiresAt. gameApiUrl / gameApiWsUrl give the per-app endpoints to use.
 *
 * appId / gameTokenId are returned as JSON strings (BigInt). gameTokenId is what
 * rides the UDP spatial message tail for this session.
 */
struct FAppTokenResponseBase : ICrowdyQueryResponse
{
	FString AppToken;
	int64   AppGameTokenID = 0;
	FString AppID;
	FString ExpiresAt;
	FString GameApiUrl;
	FString GameApiWsUrl;
	FString LaunchUrl;

protected:
	void ParseAppToken(const TSharedPtr<FJsonObject>& JsonObject, const TCHAR* RootField)
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

		if (!(*RootObject)->TryGetStringField(TEXT("token"), AppToken) || AppToken.IsEmpty())
		{
			MarkInvalid(TEXT("Missing app token"));
			return;
		}

		FString GameTokenIDStr;
		if ((*RootObject)->TryGetStringField(TEXT("gameTokenId"), GameTokenIDStr) && !GameTokenIDStr.IsEmpty())
		{
			AppGameTokenID = FCString::Atoi64(*GameTokenIDStr);
		}

		(*RootObject)->TryGetStringField(TEXT("appId"), AppID);
		(*RootObject)->TryGetStringField(TEXT("expiresAt"), ExpiresAt);
		(*RootObject)->TryGetStringField(TEXT("gameApiUrl"), GameApiUrl);
		(*RootObject)->TryGetStringField(TEXT("gameApiWsUrl"), GameApiWsUrl);
		(*RootObject)->TryGetStringField(TEXT("launchUrl"), LaunchUrl);
	}
};
