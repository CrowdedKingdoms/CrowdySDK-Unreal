#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Queries/Authentication/FCrowdyUserIdentity.h"

/**
 * Readers for the authentication payloads that have no typed counterpart, shared by the runtime sign-in and the
 * editor console's. Both drive the same operations against the same server, so a difference between them would be a
 * bug rather than a feature, and each rule below exists because getting it wrong has a specific consequence.
 *
 * Every reader takes the value of the operation's single selected field, already unwrapped, and reports false when
 * the shape cannot be used. Server payloads are untrusted, so each one type-checks rather than coercing:
 * FJsonValue::AsObject() answers with a shared empty object on a type mismatch and logs at Error, so a caller that
 * tests the returned pointer for validity never detects one.
 */
namespace CrowdyAuthPayloads
{
	// The mint response's gameApiUrl is a bare host, but the Game API GraphQL is served at /graphql, so using it raw
	// makes every Game-API POST 404. Append the path when missing (idempotent; trims a trailing slash). Path-append
	// only, no host derivation.
	inline FString EnsureGameApiGraphqlPath(const FString& Url)
	{
		FString Normalized = Url;
		Normalized.RemoveFromEnd(TEXT("/"));
		if (!Normalized.IsEmpty() && !Normalized.EndsWith(TEXT("/graphql")))
		{
			Normalized += TEXT("/graphql");
		}
		return Normalized;
	}

	// requestLoginLink: { sent }. The one-time token arrives only by email; the server used to hand
	// it back here as devToken under a bypass, and that field no longer exists on any tier.
	inline bool ReadLoginLinkPayload(const TSharedPtr<FJsonValue>& Value, bool& OutSent)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(Obj) || !Obj || !Obj->IsValid())
		{
			return false;
		}
		(*Obj)->TryGetBoolField(TEXT("sent"), OutSent);
		return true;
	}

	// socialLoginStart: { authorizeUrl, state }.
	//
	// Both fields are required. The authorize URL is where the player is sent, and the state is the CSRF material the
	// provider round-trips: the loopback listener treats an empty expected state as "accept any callback", so a
	// payload without one would leave the listener open to a code injected by anything else on the machine.
	inline bool ReadSocialStartPayload(const TSharedPtr<FJsonValue>& Value, FString& OutAuthorizeUrl, FString& OutState)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(Obj) || !Obj || !Obj->IsValid())
		{
			return false;
		}
		(*Obj)->TryGetStringField(TEXT("authorizeUrl"), OutAuthorizeUrl);
		(*Obj)->TryGetStringField(TEXT("state"), OutState);
		return !OutAuthorizeUrl.IsEmpty() && !OutState.IsEmpty();
	}

	// Whether a URL is safe to hand to the platform's URL opener. On Windows that reaches the shell, which honours
	// far more than web schemes, so a browser-bound URL from the server is required to actually be one.
	inline bool IsBrowserNavigableUrl(const FString& Url)
	{
		return Url.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase)
			|| Url.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase);
	}

	// One linked identity. The operation does not select the identity's userId, so it is filled from the signed-in
	// user: both the list and the link are scoped to that account, so there is no other account it could belong to.
	// An identity with no id cannot be acted on later, so it is rejected rather than surfaced blank.
	inline bool ReadIdentity(const TSharedPtr<FJsonValue>& Value, int64 SignedInUserId, FCrowdyUserIdentity& OutIdentity)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(Obj) || !Obj || !Obj->IsValid())
		{
			return false;
		}
		OutIdentity = FCrowdyUserIdentity::FromJson(*Obj);
		if (OutIdentity.IdentityId.IsEmpty())
		{
			return false;
		}
		if (OutIdentity.UserId.IsEmpty() && SignedInUserId != 0)
		{
			OutIdentity.UserId = LexToString(SignedInUserId);
		}
		return true;
	}
}
