#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "FCrowdyUserIdentity.generated.h"

/**
 * A federated / passwordless sign-in identity linked to a user account (a social
 * provider, an emailed magic link, or the dev bypass). Mirrors the Management API
 * UserIdentity type. BlueprintType so account-management UI can list and act on it.
 *
 * IDs are kept as strings: identityId / userId are GraphQL ID! (opaque), and the
 * timestamps are ISO-8601 DateTime strings (empty when the field is absent/null).
 */
USTRUCT(BlueprintType)
struct FCrowdyUserIdentity
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Authentication")
	FString IdentityId;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Authentication")
	FString UserId;

	/** The identity provider: 'google' | 'apple' | 'discord' | 'email' (magic link) | 'dev' (dev bypass). */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Authentication")
	FString Provider;

	/** The provider's stable subject id ('sub'). For 'email' / 'dev' this is the lowercased email. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Authentication")
	FString Subject;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Authentication")
	FString Email;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Authentication")
	bool bEmailVerified = false;

	/** ISO-8601 creation timestamp. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Authentication")
	FString CreatedAt;

	/** ISO-8601 last-login timestamp; empty if never. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Authentication")
	FString LastLoginAt;

	/** Build one from a UserIdentity JSON object — the element shape shared by the
	 *  myIdentities query and the linkIdentity mutation. */
	static FCrowdyUserIdentity FromJson(const TSharedPtr<FJsonObject>& Obj)
	{
		FCrowdyUserIdentity Identity;
		if (!Obj.IsValid())
		{
			return Identity;
		}
		Obj->TryGetStringField(TEXT("identityId"),    Identity.IdentityId);
		Obj->TryGetStringField(TEXT("userId"),        Identity.UserId);
		Obj->TryGetStringField(TEXT("provider"),      Identity.Provider);
		Obj->TryGetStringField(TEXT("subject"),       Identity.Subject);
		Obj->TryGetStringField(TEXT("email"),         Identity.Email);
		Obj->TryGetBoolField(TEXT("emailVerified"),   Identity.bEmailVerified);
		Obj->TryGetStringField(TEXT("createdAt"),     Identity.CreatedAt);
		Obj->TryGetStringField(TEXT("lastLoginAt"),   Identity.LastLoginAt);
		return Identity;
	}
};
