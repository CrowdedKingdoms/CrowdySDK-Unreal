#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Subsystem/CrowdySafety.h"
#include "CrowdySafetyActions.generated.h"

// One signature serves On Success and On Error, because a Blueprint async node builds its pins from its delegates.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSafetyCommandAsyncResult, const FCrowdySafetyError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FCommunicationSettingsAsyncResult, const FCrowdyCommunicationSettings&, Settings, const FCrowdySafetyError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FConsentRequestAsyncResult, ECrowdyConsentStatus, Status, const FCrowdySafetyError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FContactRequestAsyncResult, const FCrowdyPlayerContact&, Contact, const FCrowdySafetyError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FPlayerContactsAsyncResult, const TArray<FCrowdyPlayerContact>&, Contacts, const FCrowdySafetyError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FContactRequestsAsyncResult, const TArray<FCrowdyContactRequest>&, Requests, const FCrowdySafetyError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FPlayerBlocksAsyncResult, const TArray<FCrowdyPlayerBlock>&, Blocks, const FCrowdySafetyError&, Error);

/** Every safety call that answers only that it worked. */
UCLASS()
class CROWDYSERVICES_API UCrowdySafety_Command : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FSafetyCommandAsyncResult OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FSafetyCommandAsyncResult OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Accept Contact"), Category="Crowdy SDK|Safety")
	static UCrowdySafety_Command* AcceptContact(UObject* WorldContextObject, int64 UserId);

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Remove Contact"), Category="Crowdy SDK|Safety")
	static UCrowdySafety_Command* RemoveContact(UObject* WorldContextObject, int64 UserId);

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Block Player"), Category="Crowdy SDK|Safety")
	static UCrowdySafety_Command* BlockPlayer(UObject* WorldContextObject, int64 UserId);

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Unblock Player"), Category="Crowdy SDK|Safety")
	static UCrowdySafety_Command* UnblockPlayer(UObject* WorldContextObject, int64 UserId);

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="File Report", AdvancedDisplay="ReportedActorUuid,OccurredAt"), Category="Crowdy SDK|Safety")
	static UCrowdySafety_Command* FileReport(UObject* WorldContextObject, int64 ReportedUserId, ECrowdySafetyReportReason Reason, const FString& Details,
		const FString& ReportedActorUuid, FDateTime OccurredAt);

	virtual void Activate() override;

private:
	static UCrowdySafety_Command* Make(UObject* WorldContextObject, TFunction<void(UCrowdySafety&, FOnSafetyDone, FOnSafetyError)> Call);

	TWeakObjectPtr<UObject> WorldContextObject;
	TFunction<void(UCrowdySafety&, FOnSafetyDone, FOnSafetyError)> Call;

	UFUNCTION() void HandleSuccess();
	UFUNCTION() void HandleError(const FCrowdySafetyError& Error);
};

UCLASS()
class CROWDYSERVICES_API UCrowdySafety_CommunicationSettings : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FCommunicationSettingsAsyncResult OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FCommunicationSettingsAsyncResult OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Get Communication Settings"), Category="Crowdy SDK|Safety")
	static UCrowdySafety_CommunicationSettings* GetCommunicationSettings(UObject* WorldContextObject);

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Set Communication Setting"), Category="Crowdy SDK|Safety")
	static UCrowdySafety_CommunicationSettings* SetCommunicationSetting(UObject* WorldContextObject, ECrowdyCommunicationSetting Setting, ECrowdyCommunicationScope Scope);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	bool bSet = false;
	ECrowdyCommunicationSetting Setting = ECrowdyCommunicationSetting::VoiceSend;
	ECrowdyCommunicationScope Scope = ECrowdyCommunicationScope::Off;

	UFUNCTION() void HandleSuccess(const FCrowdyCommunicationSettings& Settings);
	UFUNCTION() void HandleError(const FCrowdySafetyError& Error);
};

UCLASS()
class CROWDYSERVICES_API UCrowdySafety_RequestParentalConsent : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FConsentRequestAsyncResult OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FConsentRequestAsyncResult OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Request Parental Consent"), Category="Crowdy SDK|Safety")
	static UCrowdySafety_RequestParentalConsent* RequestParentalConsent(UObject* WorldContextObject, ECrowdyConsentFeature Feature, const FString& ParentEmail);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	ECrowdyConsentFeature Feature = ECrowdyConsentFeature::Voice;
	FString ParentEmail;

	UFUNCTION() void HandleSuccess(ECrowdyConsentStatus Status);
	UFUNCTION() void HandleError(const FCrowdySafetyError& Error);
};

UCLASS()
class CROWDYSERVICES_API UCrowdySafety_RequestContact : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FContactRequestAsyncResult OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FContactRequestAsyncResult OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Request Contact"), Category="Crowdy SDK|Safety")
	static UCrowdySafety_RequestContact* RequestContact(UObject* WorldContextObject, int64 UserId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 UserId = 0;

	UFUNCTION() void HandleSuccess(const FCrowdyPlayerContact& Contact);
	UFUNCTION() void HandleError(const FCrowdySafetyError& Error);
};

UCLASS()
class CROWDYSERVICES_API UCrowdySafety_GetContacts : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FPlayerContactsAsyncResult OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FPlayerContactsAsyncResult OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Get Contacts"), Category="Crowdy SDK|Safety")
	static UCrowdySafety_GetContacts* GetContacts(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;

	UFUNCTION() void HandleSuccess(const TArray<FCrowdyPlayerContact>& Contacts);
	UFUNCTION() void HandleError(const FCrowdySafetyError& Error);
};

UCLASS()
class CROWDYSERVICES_API UCrowdySafety_GetContactRequests : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FContactRequestsAsyncResult OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FContactRequestsAsyncResult OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Get Contact Requests"), Category="Crowdy SDK|Safety")
	static UCrowdySafety_GetContactRequests* GetContactRequests(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;

	UFUNCTION() void HandleSuccess(const TArray<FCrowdyContactRequest>& Requests);
	UFUNCTION() void HandleError(const FCrowdySafetyError& Error);
};

UCLASS()
class CROWDYSERVICES_API UCrowdySafety_GetBlocks : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FPlayerBlocksAsyncResult OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FPlayerBlocksAsyncResult OnError;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Get Blocks"), Category="Crowdy SDK|Safety")
	static UCrowdySafety_GetBlocks* GetBlocks(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;

	UFUNCTION() void HandleSuccess(const TArray<FCrowdyPlayerBlock>& Blocks);
	UFUNCTION() void HandleError(const FCrowdySafetyError& Error);
};
