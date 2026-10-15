#pragma once

#include "CoreMinimal.h"
#include "Queries/Data/Channels/Types/FCrowdyChannelError.h"
#include "Queries/Data/Channels/Types/FCrowdyChannelMember.h"
#include "UObject/Object.h"
#include "CrowdyChannelsTestTypes.generated.h"

/** Records what the channels subsystem hands the delegates bound to it, as a Blueprint graph would receive it. */
UCLASS(Transient)
class UCrowdyChannelsTestListener : public UObject
{
	GENERATED_BODY()

public:
	TArray<ECrowdyChannelErrorCode> ErrorCodes;
	TArray<FString> Errors;
	int32 Successes = 0;

	UFUNCTION()
	void HandleMember(FCrowdyChannelMember Member) { ++Successes; }

	UFUNCTION()
	void HandleError(FCrowdyChannelError Error, FString Message)
	{
		ErrorCodes.Add(Error.Code);
		Errors.Add(Message);
	}
};
