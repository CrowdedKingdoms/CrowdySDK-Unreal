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

	TArray<FString> AccessEndedReasons;
	TArray<FString> AccessEndedBlockedUntils;

	UFUNCTION()
	void HandleAccessEnded(const FString& Reason, const FString& BlockedUntil)
	{
		AccessEndedReasons.Add(Reason);
		AccessEndedBlockedUntils.Add(BlockedUntil);
	}
};

/** Records what the account and player calls hand their delegates. */
UCLASS(Transient)
class UCrowdyAccountTestListener : public UObject
{
	GENERATED_BODY()

public:
	int32 Dones = 0;
	TArray<FCrowdyAccountError> Errors;
	TArray<FCrowdyAppAccess> Accesses;
	TArray<FCrowdyPlayerProfile> Profiles;
	TArray<TArray<FCrowdyPlayerProfile>> ProfileLists;

	UFUNCTION()
	void HandleDone() { ++Dones; }

	UFUNCTION()
	void HandleError(FCrowdyAccountError Error, FString Message) { Errors.Add(Error); }

	UFUNCTION()
	void HandleAccess(FCrowdyAppAccess Access) { Accesses.Add(Access); }

	UFUNCTION()
	void HandleProfile(FCrowdyPlayerProfile Profile) { Profiles.Add(Profile); }

	UFUNCTION()
	void HandleProfiles(const TArray<FCrowdyPlayerProfile>& List) { ProfileLists.Add(List); }
};
