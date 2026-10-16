#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "CrowdyServerObjectPingTestTypes.generated.h"

/** Records the outcomes a Ping Server Objects node would hand its Blueprint graph. */
UCLASS(Transient)
class UCrowdyServerPingTestListener : public UObject
{
	GENERATED_BODY()

public:
	TArray<float> SuccessRoundTrips;
	TArray<FString> SuccessReasons;
	TArray<float> FailedRoundTrips;
	TArray<FString> FailedReasons;

	UFUNCTION()
	void HandleSuccess(float RoundTripMs, const FString& Reason)
	{
		SuccessRoundTrips.Add(RoundTripMs);
		SuccessReasons.Add(Reason);
	}

	UFUNCTION()
	void HandleFailed(float RoundTripMs, const FString& Reason)
	{
		FailedRoundTrips.Add(RoundTripMs);
		FailedReasons.Add(Reason);
	}
};
