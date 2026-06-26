#pragma once
#include "CoreMinimal.h"
#include "FCrowdyAvatarError.generated.h"

UENUM(BlueprintType)
enum class ECrowdyAvatarErrorCode : uint8
{
	Unknown,
	NotFound,
	Forbidden,
	NetworkError,
	ServerError,
};

USTRUCT(BlueprintType)
struct CROWDYNET_API FCrowdyAvatarError
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) ECrowdyAvatarErrorCode Code = ECrowdyAvatarErrorCode::Unknown;
	UPROPERTY(BlueprintReadOnly) FString Message;

	static FCrowdyAvatarError FromMessage(const FString& Msg)
	{
		FCrowdyAvatarError Err;
		Err.Message = Msg;

		const FString Lower = Msg.ToLower();
		if (Lower.Contains(TEXT("not found")))             Err.Code = ECrowdyAvatarErrorCode::NotFound;
		else if (Lower.Contains(TEXT("forbidden"))
			  || Lower.Contains(TEXT("unauthorized"))
			  || Lower.Contains(TEXT("permission")))       Err.Code = ECrowdyAvatarErrorCode::Forbidden;
		else if (Lower.Contains(TEXT("http")))             Err.Code = ECrowdyAvatarErrorCode::NetworkError;
		else if (!Msg.IsEmpty())                           Err.Code = ECrowdyAvatarErrorCode::ServerError;

		return Err;
	}
};
