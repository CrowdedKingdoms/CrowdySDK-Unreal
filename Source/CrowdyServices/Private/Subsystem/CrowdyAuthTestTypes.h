#pragma once

#include "CoreMinimal.h"
#include "Subsystem/CrowdyAuthentication.h"
#include "UObject/Object.h"
#include "CrowdyAuthTestTypes.generated.h"

/** Records what the authentication subsystem hands the delegates bound to it, as a Blueprint graph would receive it. */
UCLASS(Transient)
class UCrowdyAuthTestListener : public UObject
{
	GENERATED_BODY()

public:
	int32 LegalAcceptanceRequests = 0;
	int32 LegalAcceptanceEnds = 0;
	int32 FlowSuccesses = 0;
	TArray<FString> FlowErrors;
	int32 Logins = 0;
	TArray<FString> LoginFailures;
	int32 Refreshes = 0;
	int32 ConsentsRecorded = 0;
	TArray<FString> ConsentErrors;

	UFUNCTION()
	void HandleLegalAcceptanceRequired() { ++LegalAcceptanceRequests; }

	UFUNCTION()
	void HandleLegalAcceptanceEnded() { ++LegalAcceptanceEnds; }

	UFUNCTION()
	void HandleFlowSuccess(FCrowdyAuthResult Result) { ++FlowSuccesses; }

	UFUNCTION()
	void HandleFlowError(FString Message) { FlowErrors.Add(Message); }

	UFUNCTION()
	void HandleLogin(FCrowdyAuthResult Result) { ++Logins; }

	UFUNCTION()
	void HandleLoginFailed(FString Message) { LoginFailures.Add(Message); }

	UFUNCTION()
	void HandleRefreshed() { ++Refreshes; }

	UFUNCTION()
	void HandleConsentRecorded() { ++ConsentsRecorded; }

	UFUNCTION()
	void HandleConsentError(FString Message) { ConsentErrors.Add(Message); }
};
