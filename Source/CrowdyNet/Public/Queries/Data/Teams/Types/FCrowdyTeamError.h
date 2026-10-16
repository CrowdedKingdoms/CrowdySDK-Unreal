#pragma once
#include "CoreMinimal.h"
#include "FCrowdyTeamError.generated.h"

UENUM(BlueprintType)
enum class ECrowdyTeamErrorCode : uint8
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

USTRUCT(BlueprintType)
struct CROWDYNET_API FCrowdyTeamError
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) ECrowdyTeamErrorCode Code = ECrowdyTeamErrorCode::Unknown;
	UPROPERTY(BlueprintReadOnly) FString Message;

	static FCrowdyTeamError FromMessage(const FString& Msg)
	{
		FCrowdyTeamError Err;
		Err.Message = Msg;

		const FString Lower = Msg.ToLower();
		if (Lower.Contains(TEXT("not found")))             Err.Code = ECrowdyTeamErrorCode::NotFound;
		else if (Lower.Contains(TEXT("forbidden"))
			  || Lower.Contains(TEXT("unauthorized"))
			  || Lower.Contains(TEXT("permission")))       Err.Code = ECrowdyTeamErrorCode::Forbidden;
		else if (Lower.Contains(TEXT("already member"))
			  || Lower.Contains(TEXT("already joined")))   Err.Code = ECrowdyTeamErrorCode::AlreadyMember;
		else if (Lower.Contains(TEXT("not a member")))     Err.Code = ECrowdyTeamErrorCode::NotMember;
		else if (Lower.Contains(TEXT("policy")))           Err.Code = ECrowdyTeamErrorCode::PolicyViolation;
		else if (Lower.Contains(TEXT("http")))             Err.Code = ECrowdyTeamErrorCode::NetworkError;
		else if (!Msg.IsEmpty())                           Err.Code = ECrowdyTeamErrorCode::ServerError;

		return Err;
	}

	/** FromMessage, then the server's stable error code where it names a failure the message text cannot. */
	static FCrowdyTeamError FromFailure(const FString& Msg, const FString& ServerCode)
	{
		FCrowdyTeamError Err = FromMessage(Msg);
		if (ServerCode == TEXT("GROUP_MEMBER_BANNED"))
			Err.Code = ECrowdyTeamErrorCode::Banned;

		return Err;
	}
};
