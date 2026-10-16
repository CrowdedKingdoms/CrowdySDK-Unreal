#include "Subsystem/AsyncActions/CrowdyAccountActions.h"
#include "CrowdyServicesLog.h"
#include "Subsystem/AsyncActions/CrowdyServicesActionSupport.h"

void UCrowdyAccountAction::Activate()
{
	UCrowdyAuthentication* Auth = GetServiceSubsystem<UCrowdyAuthentication>(WorldContextObject);
	if (!Auth)
	{
		HandleError(FCrowdyAccountError::Make(ECrowdyAccountErrorCode::Unknown, TEXT("UCrowdyAuthentication subsystem not found")),
			TEXT("UCrowdyAuthentication subsystem not found"));
		return;
	}

	FOnAccountDone Done;
	Done.BindDynamic(this, &UCrowdyAccountAction::HandleDone);
	FOnAccountError Failed;
	Failed.BindDynamic(this, &UCrowdyAccountAction::HandleError);
	Send(*Auth, Done, Failed);
}

void UCrowdyAccountAction::HandleDone()
{
	OnSuccess.Broadcast(FCrowdyAccountError(), FString());
	SetReadyToDestroy();
}

void UCrowdyAccountAction::HandleError(FCrowdyAccountError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("[CrowdyAccount] %s failed: %s"), *GetClass()->GetName(), *Message);
	OnError.Broadcast(Error, Message);
	SetReadyToDestroy();
}

UCrowdyAccount_RequestPasswordReset* UCrowdyAccount_RequestPasswordReset::RequestPasswordReset(UObject* WorldContextObject,
	const FString& Email)
{
	UCrowdyAccount_RequestPasswordReset* Action = NewObject<UCrowdyAccount_RequestPasswordReset>();
	Action->WorldContextObject = WorldContextObject;
	Action->Email = Email;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyAccount_RequestPasswordReset::Send(UCrowdyAuthentication& Auth, const FOnAccountDone& Done,
	const FOnAccountError& Failed)
{
	Auth.RequestPasswordReset(Email, Done, Failed);
}

UCrowdyAccount_ResendConfirmationEmail* UCrowdyAccount_ResendConfirmationEmail::ResendConfirmationEmail(
	UObject* WorldContextObject, const FString& Email)
{
	UCrowdyAccount_ResendConfirmationEmail* Action = NewObject<UCrowdyAccount_ResendConfirmationEmail>();
	Action->WorldContextObject = WorldContextObject;
	Action->Email = Email;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyAccount_ResendConfirmationEmail::Send(UCrowdyAuthentication& Auth, const FOnAccountDone& Done,
	const FOnAccountError& Failed)
{
	Auth.ResendConfirmationEmail(Email, Done, Failed);
}

UCrowdyAccount_ChangePassword* UCrowdyAccount_ChangePassword::ChangePassword(UObject* WorldContextObject,
	const FString& CurrentPassword, const FString& NewPassword)
{
	UCrowdyAccount_ChangePassword* Action = NewObject<UCrowdyAccount_ChangePassword>();
	Action->WorldContextObject = WorldContextObject;
	Action->CurrentPassword = CurrentPassword;
	Action->NewPassword = NewPassword;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyAccount_ChangePassword::Send(UCrowdyAuthentication& Auth, const FOnAccountDone& Done,
	const FOnAccountError& Failed)
{
	Auth.ChangePassword(CurrentPassword, NewPassword, Done, Failed);
}

UCrowdyAccount_AddPassword* UCrowdyAccount_AddPassword::AddPassword(UObject* WorldContextObject, const FString& NewPassword)
{
	UCrowdyAccount_AddPassword* Action = NewObject<UCrowdyAccount_AddPassword>();
	Action->WorldContextObject = WorldContextObject;
	Action->NewPassword = NewPassword;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyAccount_AddPassword::Send(UCrowdyAuthentication& Auth, const FOnAccountDone& Done,
	const FOnAccountError& Failed)
{
	Auth.AddPassword(NewPassword, Done, Failed);
}

UCrowdyAccount_DeleteAccount* UCrowdyAccount_DeleteAccount::DeleteAccount(UObject* WorldContextObject)
{
	UCrowdyAccount_DeleteAccount* Action = NewObject<UCrowdyAccount_DeleteAccount>();
	Action->WorldContextObject = WorldContextObject;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyAccount_DeleteAccount::Send(UCrowdyAuthentication& Auth, const FOnAccountDone& Done,
	const FOnAccountError& Failed)
{
	Auth.DeleteAccount(Done, Failed);
}

UCrowdyAccount_SignOutEverywhere* UCrowdyAccount_SignOutEverywhere::SignOutEverywhere(UObject* WorldContextObject)
{
	UCrowdyAccount_SignOutEverywhere* Action = NewObject<UCrowdyAccount_SignOutEverywhere>();
	Action->WorldContextObject = WorldContextObject;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyAccount_SignOutEverywhere::Send(UCrowdyAuthentication& Auth, const FOnAccountDone& Done,
	const FOnAccountError& Failed)
{
	Auth.SignOutEverywhere(Done, Failed);
}

UCrowdyAccount_GetMyAccess* UCrowdyAccount_GetMyAccess::GetMyAccess(UObject* WorldContextObject)
{
	UCrowdyAccount_GetMyAccess* Action = NewObject<UCrowdyAccount_GetMyAccess>();
	Action->WorldContextObject = WorldContextObject;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyAccount_GetMyAccess::Activate()
{
	UCrowdyAuthentication* Auth = GetServiceSubsystem<UCrowdyAuthentication>(WorldContextObject);
	if (!Auth)
	{
		HandleError(FCrowdyAccountError::Make(ECrowdyAccountErrorCode::Unknown, TEXT("UCrowdyAuthentication subsystem not found")),
			TEXT("UCrowdyAuthentication subsystem not found"));
		return;
	}

	FOnAppAccessRead Read;
	Read.BindDynamic(this, &UCrowdyAccount_GetMyAccess::HandleRead);
	FOnAccountError Failed;
	Failed.BindDynamic(this, &UCrowdyAccount_GetMyAccess::HandleError);
	Auth->GetMyAccess(Read, Failed);
}

void UCrowdyAccount_GetMyAccess::HandleRead(FCrowdyAppAccess Access)
{
	OnSuccess.Broadcast(Access, FCrowdyAccountError(), FString());
	SetReadyToDestroy();
}

void UCrowdyAccount_GetMyAccess::HandleError(FCrowdyAccountError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("[CrowdyAccount] Get My Access failed: %s"), *Message);
	OnError.Broadcast(FCrowdyAppAccess(), Error, Message);
	SetReadyToDestroy();
}

UCrowdyAccount_ClaimFreeAccess* UCrowdyAccount_ClaimFreeAccess::ClaimFreeAccess(UObject* WorldContextObject)
{
	UCrowdyAccount_ClaimFreeAccess* Action = NewObject<UCrowdyAccount_ClaimFreeAccess>();
	Action->WorldContextObject = WorldContextObject;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyAccount_ClaimFreeAccess::Activate()
{
	UCrowdyAuthentication* Auth = GetServiceSubsystem<UCrowdyAuthentication>(WorldContextObject);
	if (!Auth)
	{
		HandleError(TEXT("UCrowdyAuthentication subsystem not found"));
		return;
	}

	FOnAuthSuccess Succeeded;
	Succeeded.BindDynamic(this, &UCrowdyAccount_ClaimFreeAccess::HandleSuccess);
	FOnAuthError Failed;
	Failed.BindDynamic(this, &UCrowdyAccount_ClaimFreeAccess::HandleError);
	Auth->ClaimFreeAccess(Succeeded, Failed);
}

void UCrowdyAccount_ClaimFreeAccess::HandleSuccess(FCrowdyAuthResult Result)
{
	OnSuccess.Broadcast(Result, FString());
	SetReadyToDestroy();
}

void UCrowdyAccount_ClaimFreeAccess::HandleError(FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("[CrowdyAccount] Claim Free Access failed: %s"), *Message);
	OnError.Broadcast(FCrowdyAuthResult(), Message);
	SetReadyToDestroy();
}
