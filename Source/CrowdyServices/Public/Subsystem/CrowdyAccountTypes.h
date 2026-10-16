#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "CrowdyAccountTypes.generated.h"

UENUM(BlueprintType)
enum class ECrowdyAccountErrorCode : uint8
{
	Unknown,
	NotSignedIn,
	WrongPassword,
	PasswordAlreadySet,
	NoPassword,
	NameTaken,
	NotFound,
	RateLimited,
	NetworkError,
	ServerError,
};

/** Why an account, player name or access call failed. Message is the server's own wording, fit to show the player. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyAccountError
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Account")
	ECrowdyAccountErrorCode Code = ECrowdyAccountErrorCode::Unknown;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Account")
	FString Message;

	static FCrowdyAccountError Make(ECrowdyAccountErrorCode Code, const FString& Message)
	{
		FCrowdyAccountError Error;
		Error.Code = Code;
		Error.Message = Message;
		return Error;
	}

	/** Classifies by the server's stable code; a failure that carried none never reached a verdict. */
	static FCrowdyAccountError FromFailure(const FString& Msg, const FString& ServerCode)
	{
		if (ServerCode.IsEmpty())
			return Make(ECrowdyAccountErrorCode::NetworkError, Msg);
		if (ServerCode == TEXT("UNAUTHENTICATED"))
			return Make(ECrowdyAccountErrorCode::NotSignedIn, Msg);
		if (ServerCode == TEXT("INVALID_CURRENT_PASSWORD"))
			return Make(ECrowdyAccountErrorCode::WrongPassword, Msg);
		if (ServerCode == TEXT("PASSWORD_ALREADY_SET"))
			return Make(ECrowdyAccountErrorCode::PasswordAlreadySet, Msg);
		if (ServerCode == TEXT("PASSWORD_NOT_SET"))
			return Make(ECrowdyAccountErrorCode::NoPassword, Msg);
		if (ServerCode == TEXT("RATE_LIMITED"))
			return Make(ECrowdyAccountErrorCode::RateLimited, Msg);
		if (ServerCode == TEXT("GAMERTAG_ALREADY_EXISTS") || ServerCode == TEXT("CONFLICT"))
			return Make(ECrowdyAccountErrorCode::NameTaken, Msg);
		return Make(ECrowdyAccountErrorCode::ServerError, Msg);
	}
};

UENUM(BlueprintType)
enum class ECrowdyAccessState : uint8
{
	/** The player has never held access to this app. */
	None,
	Active,
	/** Until Suspended Until, then it returns by itself. */
	Suspended,
	Expired,
	Revoked,
};

/** The signed-in player's own access to this app. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyAppAccess
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Account")
	ECrowdyAccessState State = ECrowdyAccessState::None;

	/** The access tier that grants it, 0 when none does. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Account")
	int64 TierID = 0;

	/** When the grant ends; zero when it does not expire. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Account")
	FDateTime ExpiresAt;

	/** When a suspension ends; zero when the player is not suspended. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Account")
	FDateTime SuspendedUntil;

	/** True when the access came from the app's free tier rather than from an administrator. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Account")
	bool bFreeGrant = false;

	/** Reads an AppUserAccess row as of Now; a missing row is State None. */
	static FCrowdyAppAccess FromJson(const TSharedPtr<FJsonObject>& Row, const FDateTime& Now)
	{
		FCrowdyAppAccess Access;
		if (!Row.IsValid())
			return Access;

		FString Text;
		if (Row->TryGetStringField(TEXT("tierId"), Text))
			Access.TierID = FCString::Atoi64(*Text);
		if (Row->TryGetStringField(TEXT("expiresAt"), Text))
			FDateTime::ParseIso8601(*Text, Access.ExpiresAt);
		if (Row->TryGetStringField(TEXT("suspendedUntil"), Text))
			FDateTime::ParseIso8601(*Text, Access.SuspendedUntil);
		Access.bFreeGrant = Row->TryGetStringField(TEXT("grantedBy"), Text) && Text == TEXT("system");

		FString Status;
		Row->TryGetStringField(TEXT("status"), Status);
		if (Status != TEXT("active"))
			Access.State = ECrowdyAccessState::Revoked;
		else if (Access.SuspendedUntil.GetTicks() > 0 && Access.SuspendedUntil > Now)
			Access.State = ECrowdyAccessState::Suspended;
		else if (Access.ExpiresAt.GetTicks() > 0 && Access.ExpiresAt <= Now)
			Access.State = ECrowdyAccessState::Expired;
		else
			Access.State = ECrowdyAccessState::Active;
		return Access;
	}
};

/** A player's public name. A player reads their own with their own User ID. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyPlayerProfile
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Players")
	int64 UserID = 0;

	/** Empty when the player has not chosen a name. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Players")
	FString Gamertag;

	/** Makes the gamertag unique: no two players share the pair. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Players")
	FString Disambiguation;

	static bool ParseFromJson(const TSharedPtr<FJsonObject>& Json, FCrowdyPlayerProfile& Out)
	{
		FString UserId;
		if (!Json.IsValid() || !Json->TryGetStringField(TEXT("userId"), UserId))
			return false;

		Out.UserID = FCString::Atoi64(*UserId);
		Json->TryGetStringField(TEXT("gamertag"), Out.Gamertag);
		Json->TryGetStringField(TEXT("disambiguation"), Out.Disambiguation);
		return Out.UserID != 0;
	}
};
