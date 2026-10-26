#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "CrowdySDKAuthForwardingTestTypes.generated.h"

/** Records what the SDK subsystem's OnLogin hands the delegates bound to it. */
UCLASS(Transient)
class UCrowdySDKLoginRecorder : public UObject
{
	GENERATED_BODY()

public:
	TArray<bool> Successes;
	TArray<FString> Messages;

	UFUNCTION()
	void HandleLogin(bool bSuccess, FString Message)
	{
		Successes.Add(bSuccess);
		Messages.Add(Message);
	}
};
