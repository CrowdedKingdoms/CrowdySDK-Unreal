#pragma once
#include "CoreMinimal.h"
#include "FCrowdyChannelError.generated.h"

UENUM(BlueprintType)
enum class ECrowdyChannelErrorCode : uint8
{
	Unknown,
	NotFound,
	Forbidden,
	PolicyViolation,
	AlreadyMember,
	NotMember,
	NetworkError,
	ServerError,
	Banned,
};

/**
 * Why a channel call failed. The code is a best-effort classification of the server's message, for callers that
 * want to branch; Message is what to show. Banned is the exception: it comes from the server's own error code, never
 * from the message text.
 */
USTRUCT(BlueprintType)
struct CROWDYNET_API FCrowdyChannelError
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) ECrowdyChannelErrorCode Code = ECrowdyChannelErrorCode::Unknown;
	UPROPERTY(BlueprintReadOnly) FString Message;

	static FCrowdyChannelError FromMessage(const FString& Msg)
	{
		FCrowdyChannelError Error;
		Error.Message = Msg;

		const FString Lower = Msg.ToLower();
		if (Lower.Contains(TEXT("not found")))             Error.Code = ECrowdyChannelErrorCode::NotFound;
		else if (Lower.Contains(TEXT("forbidden"))
			  || Lower.Contains(TEXT("unauthorized"))
			  || Lower.Contains(TEXT("permission")))       Error.Code = ECrowdyChannelErrorCode::Forbidden;
		else if (Lower.Contains(TEXT("already member"))
			  || Lower.Contains(TEXT("already joined")))   Error.Code = ECrowdyChannelErrorCode::AlreadyMember;
		else if (Lower.Contains(TEXT("not a member")))     Error.Code = ECrowdyChannelErrorCode::NotMember;
		else if (Lower.Contains(TEXT("policy")))           Error.Code = ECrowdyChannelErrorCode::PolicyViolation;
		else if (Lower.Contains(TEXT("http")))             Error.Code = ECrowdyChannelErrorCode::NetworkError;
		else if (!Msg.IsEmpty())                           Error.Code = ECrowdyChannelErrorCode::ServerError;

		return Error;
	}

	/** FromMessage, then the server's stable error code where it names a failure the message text cannot. */
	static FCrowdyChannelError FromFailure(const FString& Msg, const FString& ServerCode)
	{
		FCrowdyChannelError Error = FromMessage(Msg);
		if (ServerCode == TEXT("GROUP_MEMBER_BANNED"))
			Error.Code = ECrowdyChannelErrorCode::Banned;

		return Error;
	}
};
