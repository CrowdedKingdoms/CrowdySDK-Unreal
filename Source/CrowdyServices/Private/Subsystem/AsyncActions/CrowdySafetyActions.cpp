#include "Subsystem/AsyncActions/CrowdySafetyActions.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

namespace
{
	UCrowdySafety* FindSafety(const TWeakObjectPtr<UObject>& WorldContextObject)
	{
		const UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject.Get(), EGetWorldErrorMode::ReturnNull) : nullptr;
		UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		return GameInstance ? GameInstance->GetSubsystem<UCrowdySafety>() : nullptr;
	}

	FCrowdySafetyError MissingSubsystem()
	{
		FCrowdySafetyError Error;
		Error.Code = ECrowdySafetyErrorCode::NetworkError;
		Error.Message = TEXT("UCrowdySafety subsystem not found");
		return Error;
	}
}

UCrowdySafety_Command* UCrowdySafety_Command::Make(UObject* WorldContextObject, TFunction<void(UCrowdySafety&, FOnSafetyDone, FOnSafetyError)> InCall)
{
	UCrowdySafety_Command* Action = NewObject<UCrowdySafety_Command>();
	Action->WorldContextObject = WorldContextObject;
	Action->Call = MoveTemp(InCall);
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

UCrowdySafety_Command* UCrowdySafety_Command::AcceptContact(UObject* WorldContextObject, int64 UserId)
{
	return Make(WorldContextObject, [UserId](UCrowdySafety& Safety, FOnSafetyDone S, FOnSafetyError E) { Safety.AcceptContact(UserId, S, E); });
}

UCrowdySafety_Command* UCrowdySafety_Command::RemoveContact(UObject* WorldContextObject, int64 UserId)
{
	return Make(WorldContextObject, [UserId](UCrowdySafety& Safety, FOnSafetyDone S, FOnSafetyError E) { Safety.RemoveContact(UserId, S, E); });
}

UCrowdySafety_Command* UCrowdySafety_Command::BlockPlayer(UObject* WorldContextObject, int64 UserId)
{
	return Make(WorldContextObject, [UserId](UCrowdySafety& Safety, FOnSafetyDone S, FOnSafetyError E) { Safety.BlockPlayer(UserId, S, E); });
}

UCrowdySafety_Command* UCrowdySafety_Command::UnblockPlayer(UObject* WorldContextObject, int64 UserId)
{
	return Make(WorldContextObject, [UserId](UCrowdySafety& Safety, FOnSafetyDone S, FOnSafetyError E) { Safety.UnblockPlayer(UserId, S, E); });
}

UCrowdySafety_Command* UCrowdySafety_Command::FileReport(UObject* WorldContextObject, int64 ReportedUserId, ECrowdySafetyReportReason Reason, const FString& Details,
	const FString& ReportedActorUuid, FDateTime OccurredAt)
{
	return Make(WorldContextObject, [ReportedUserId, Reason, Details, ReportedActorUuid, OccurredAt](UCrowdySafety& Safety, FOnSafetyDone S, FOnSafetyError E)
	{
		Safety.FileReport(ReportedUserId, Reason, Details, ReportedActorUuid, OccurredAt, S, E);
	});
}

void UCrowdySafety_Command::Activate()
{
	UCrowdySafety* Safety = FindSafety(WorldContextObject);
	if (!Safety)
	{
		HandleError(MissingSubsystem());
		return;
	}

	FOnSafetyDone S;
	S.BindDynamic(this, &UCrowdySafety_Command::HandleSuccess);
	FOnSafetyError E;
	E.BindDynamic(this, &UCrowdySafety_Command::HandleError);
	Call(*Safety, S, E);
}

void UCrowdySafety_Command::HandleSuccess()
{
	OnSuccess.Broadcast(FCrowdySafetyError());
	SetReadyToDestroy();
}

void UCrowdySafety_Command::HandleError(const FCrowdySafetyError& Error)
{
	OnError.Broadcast(Error);
	SetReadyToDestroy();
}

UCrowdySafety_CommunicationSettings* UCrowdySafety_CommunicationSettings::GetCommunicationSettings(UObject* WorldContextObject)
{
	UCrowdySafety_CommunicationSettings* Action = NewObject<UCrowdySafety_CommunicationSettings>();
	Action->WorldContextObject = WorldContextObject;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

UCrowdySafety_CommunicationSettings* UCrowdySafety_CommunicationSettings::SetCommunicationSetting(UObject* WorldContextObject, ECrowdyCommunicationSetting Setting, ECrowdyCommunicationScope Scope)
{
	UCrowdySafety_CommunicationSettings* Action = GetCommunicationSettings(WorldContextObject);
	Action->bSet = true;
	Action->Setting = Setting;
	Action->Scope = Scope;
	return Action;
}

void UCrowdySafety_CommunicationSettings::Activate()
{
	UCrowdySafety* Safety = FindSafety(WorldContextObject);
	if (!Safety)
	{
		HandleError(MissingSubsystem());
		return;
	}

	FOnCommunicationSettings S;
	S.BindDynamic(this, &UCrowdySafety_CommunicationSettings::HandleSuccess);
	FOnSafetyError E;
	E.BindDynamic(this, &UCrowdySafety_CommunicationSettings::HandleError);
	if (bSet)
	{
		Safety->SetCommunicationSetting(Setting, Scope, S, E);
		return;
	}
	Safety->GetCommunicationSettings(S, E);
}

void UCrowdySafety_CommunicationSettings::HandleSuccess(const FCrowdyCommunicationSettings& Settings)
{
	OnSuccess.Broadcast(Settings, FCrowdySafetyError());
	SetReadyToDestroy();
}

void UCrowdySafety_CommunicationSettings::HandleError(const FCrowdySafetyError& Error)
{
	OnError.Broadcast(FCrowdyCommunicationSettings(), Error);
	SetReadyToDestroy();
}

UCrowdySafety_RequestParentalConsent* UCrowdySafety_RequestParentalConsent::RequestParentalConsent(UObject* WorldContextObject, ECrowdyConsentFeature Feature, const FString& ParentEmail)
{
	UCrowdySafety_RequestParentalConsent* Action = NewObject<UCrowdySafety_RequestParentalConsent>();
	Action->WorldContextObject = WorldContextObject;
	Action->Feature = Feature;
	Action->ParentEmail = ParentEmail;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdySafety_RequestParentalConsent::Activate()
{
	UCrowdySafety* Safety = FindSafety(WorldContextObject);
	if (!Safety)
	{
		HandleError(MissingSubsystem());
		return;
	}

	FOnConsentRequested S;
	S.BindDynamic(this, &UCrowdySafety_RequestParentalConsent::HandleSuccess);
	FOnSafetyError E;
	E.BindDynamic(this, &UCrowdySafety_RequestParentalConsent::HandleError);
	Safety->RequestParentalConsent(Feature, ParentEmail, S, E);
}

void UCrowdySafety_RequestParentalConsent::HandleSuccess(ECrowdyConsentStatus Status)
{
	OnSuccess.Broadcast(Status, FCrowdySafetyError());
	SetReadyToDestroy();
}

void UCrowdySafety_RequestParentalConsent::HandleError(const FCrowdySafetyError& Error)
{
	OnError.Broadcast(ECrowdyConsentStatus::NotRequired, Error);
	SetReadyToDestroy();
}

UCrowdySafety_RequestContact* UCrowdySafety_RequestContact::RequestContact(UObject* WorldContextObject, int64 UserId)
{
	UCrowdySafety_RequestContact* Action = NewObject<UCrowdySafety_RequestContact>();
	Action->WorldContextObject = WorldContextObject;
	Action->UserId = UserId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdySafety_RequestContact::Activate()
{
	UCrowdySafety* Safety = FindSafety(WorldContextObject);
	if (!Safety)
	{
		HandleError(MissingSubsystem());
		return;
	}

	FOnPlayerContact S;
	S.BindDynamic(this, &UCrowdySafety_RequestContact::HandleSuccess);
	FOnSafetyError E;
	E.BindDynamic(this, &UCrowdySafety_RequestContact::HandleError);
	Safety->RequestContact(UserId, S, E);
}

void UCrowdySafety_RequestContact::HandleSuccess(const FCrowdyPlayerContact& Contact)
{
	OnSuccess.Broadcast(Contact, FCrowdySafetyError());
	SetReadyToDestroy();
}

void UCrowdySafety_RequestContact::HandleError(const FCrowdySafetyError& Error)
{
	OnError.Broadcast(FCrowdyPlayerContact(), Error);
	SetReadyToDestroy();
}

UCrowdySafety_GetContacts* UCrowdySafety_GetContacts::GetContacts(UObject* WorldContextObject)
{
	UCrowdySafety_GetContacts* Action = NewObject<UCrowdySafety_GetContacts>();
	Action->WorldContextObject = WorldContextObject;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdySafety_GetContacts::Activate()
{
	UCrowdySafety* Safety = FindSafety(WorldContextObject);
	if (!Safety)
	{
		HandleError(MissingSubsystem());
		return;
	}

	FOnPlayerContacts S;
	S.BindDynamic(this, &UCrowdySafety_GetContacts::HandleSuccess);
	FOnSafetyError E;
	E.BindDynamic(this, &UCrowdySafety_GetContacts::HandleError);
	Safety->GetContacts(S, E);
}

void UCrowdySafety_GetContacts::HandleSuccess(const TArray<FCrowdyPlayerContact>& Contacts)
{
	OnSuccess.Broadcast(Contacts, FCrowdySafetyError());
	SetReadyToDestroy();
}

void UCrowdySafety_GetContacts::HandleError(const FCrowdySafetyError& Error)
{
	OnError.Broadcast(TArray<FCrowdyPlayerContact>(), Error);
	SetReadyToDestroy();
}

UCrowdySafety_GetContactRequests* UCrowdySafety_GetContactRequests::GetContactRequests(UObject* WorldContextObject)
{
	UCrowdySafety_GetContactRequests* Action = NewObject<UCrowdySafety_GetContactRequests>();
	Action->WorldContextObject = WorldContextObject;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdySafety_GetContactRequests::Activate()
{
	UCrowdySafety* Safety = FindSafety(WorldContextObject);
	if (!Safety)
	{
		HandleError(MissingSubsystem());
		return;
	}

	FOnContactRequests S;
	S.BindDynamic(this, &UCrowdySafety_GetContactRequests::HandleSuccess);
	FOnSafetyError E;
	E.BindDynamic(this, &UCrowdySafety_GetContactRequests::HandleError);
	Safety->GetContactRequests(S, E);
}

void UCrowdySafety_GetContactRequests::HandleSuccess(const TArray<FCrowdyContactRequest>& Requests)
{
	OnSuccess.Broadcast(Requests, FCrowdySafetyError());
	SetReadyToDestroy();
}

void UCrowdySafety_GetContactRequests::HandleError(const FCrowdySafetyError& Error)
{
	OnError.Broadcast(TArray<FCrowdyContactRequest>(), Error);
	SetReadyToDestroy();
}

UCrowdySafety_GetBlocks* UCrowdySafety_GetBlocks::GetBlocks(UObject* WorldContextObject)
{
	UCrowdySafety_GetBlocks* Action = NewObject<UCrowdySafety_GetBlocks>();
	Action->WorldContextObject = WorldContextObject;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdySafety_GetBlocks::Activate()
{
	UCrowdySafety* Safety = FindSafety(WorldContextObject);
	if (!Safety)
	{
		HandleError(MissingSubsystem());
		return;
	}

	FOnPlayerBlocks S;
	S.BindDynamic(this, &UCrowdySafety_GetBlocks::HandleSuccess);
	FOnSafetyError E;
	E.BindDynamic(this, &UCrowdySafety_GetBlocks::HandleError);
	Safety->GetBlocks(S, E);
}

void UCrowdySafety_GetBlocks::HandleSuccess(const TArray<FCrowdyPlayerBlock>& Blocks)
{
	OnSuccess.Broadcast(Blocks, FCrowdySafetyError());
	SetReadyToDestroy();
}

void UCrowdySafety_GetBlocks::HandleError(const FCrowdySafetyError& Error)
{
	OnError.Broadcast(TArray<FCrowdyPlayerBlock>(), Error);
	SetReadyToDestroy();
}
