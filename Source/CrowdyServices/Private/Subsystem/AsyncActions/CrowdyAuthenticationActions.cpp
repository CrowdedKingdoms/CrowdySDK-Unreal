#include "Subsystem/AsyncActions/CrowdyAuthenticationActions.h"
#include "CrowdyServicesLog.h"
#include "Subsystem/CrowdyAuthentication.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"


static UCrowdyAuthentication* GetAuthSubsystem(const TWeakObjectPtr<UObject>& Ctx)
{
	if (!Ctx.IsValid()) return nullptr;
	UGameInstance* GI = Ctx->GetWorld() ? Ctx->GetWorld()->GetGameInstance() : nullptr;
	return GI ? GI->GetSubsystem<UCrowdyAuthentication>() : nullptr;
}


UCrowdyAuth_Login* UCrowdyAuth_Login::Login(UObject* WorldContextObject, const FString& Email, const FString& Password)
{
	UCrowdyAuth_Login* Action = NewObject<UCrowdyAuth_Login>();
	Action->WorldContextObject = WorldContextObject;
	Action->Email = Email;
	Action->Password = Password;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyAuth_Login::Activate()
{
	UCrowdyAuthentication* Auth = GetAuthSubsystem(WorldContextObject);
	if (!Auth)
	{
		HandleError(TEXT("UCrowdyAuthentication subsystem not found"));
		return;
	}

	FOnAuthSuccess S;
	S.BindDynamic(this, &UCrowdyAuth_Login::HandleSuccess);
	FOnAuthError E;
	E.BindDynamic(this, &UCrowdyAuth_Login::HandleError);
	Auth->Login(Email, Password, S, E);
}

void UCrowdyAuth_Login::HandleSuccess(FCrowdyAuthResult Result)
{
	OnSuccess.Broadcast(Result);
	SetReadyToDestroy();
}

void UCrowdyAuth_Login::HandleError(FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("[CrowdyAuth] Login failed: %s"), *Message);
	OnError.Broadcast(Message);
	SetReadyToDestroy();
}


UCrowdyAuth_Register* UCrowdyAuth_Register::Register(UObject* WorldContextObject, const FString& Email,
                                                      const FString& Password)
{
	UCrowdyAuth_Register* Action = NewObject<UCrowdyAuth_Register>();
	Action->WorldContextObject = WorldContextObject;
	Action->Email = Email;
	Action->Password = Password;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyAuth_Register::Activate()
{
	UCrowdyAuthentication* Auth = GetAuthSubsystem(WorldContextObject);
	if (!Auth)
	{
		HandleError(TEXT("UCrowdyAuthentication subsystem not found"));
		return;
	}

	FOnAuthSuccess S;
	S.BindDynamic(this, &UCrowdyAuth_Register::HandleSuccess);
	FOnAuthError E;
	E.BindDynamic(this, &UCrowdyAuth_Register::HandleError);
	Auth->Register(Email, Password, S, E);
}

void UCrowdyAuth_Register::HandleSuccess(FCrowdyAuthResult Result)
{
	OnSuccess.Broadcast(Result);
	SetReadyToDestroy();
}

void UCrowdyAuth_Register::HandleError(FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("[CrowdyAuth] Register failed: %s"), *Message);
	OnError.Broadcast(Message);
	SetReadyToDestroy();
}

UCrowdyAuth_RestoreSession* UCrowdyAuth_RestoreSession::RestoreSession(UObject* WorldContextObject)
{
	UCrowdyAuth_RestoreSession* Action = NewObject<UCrowdyAuth_RestoreSession>();
	Action->WorldContextObject = WorldContextObject;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyAuth_RestoreSession::Activate()
{
	UCrowdyAuthentication* Auth = GetAuthSubsystem(WorldContextObject);
	if (!Auth)
	{
		HandleError(TEXT("UCrowdyAuthentication subsystem not found"));
		return;
	}

	FOnAuthSuccess S;
	S.BindDynamic(this, &UCrowdyAuth_RestoreSession::HandleSuccess);
	FOnAuthError E;
	E.BindDynamic(this, &UCrowdyAuth_RestoreSession::HandleError);

	// The bool return (true = a saved session was found and restore is proceeding async) is not needed
	// here: on false (nothing saved) RestoreSession already calls E synchronously, which HandleError
	// above catches either way.
	(void)Auth->RestoreSession(S, E);
}

void UCrowdyAuth_RestoreSession::HandleSuccess(FCrowdyAuthResult Result)
{
	OnSuccess.Broadcast(Result);
	SetReadyToDestroy();
}

void UCrowdyAuth_RestoreSession::HandleError(FString Message)
{
	UE_CLOG(CrowdyServicesTrace::Services(), LogCrowdyServices, Log, TEXT("[CrowdyAuth] Session restore: %s"), *Message);
	OnError.Broadcast(Message);
	SetReadyToDestroy();
}

UCrowdyAuth_DevLogin* UCrowdyAuth_DevLogin::DevLogin(UObject* WorldContextObject, const FString& Email)
{
	UCrowdyAuth_DevLogin* Action = NewObject<UCrowdyAuth_DevLogin>();
	Action->WorldContextObject = WorldContextObject;
	Action->Email = Email;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyAuth_DevLogin::Activate()
{
	UCrowdyAuthentication* Auth = GetAuthSubsystem(WorldContextObject);
	if (!Auth)
	{
		HandleError(TEXT("UCrowdyAuthentication subsystem not found"));
		return;
	}

	FOnAuthSuccess S;
	S.BindDynamic(this, &UCrowdyAuth_DevLogin::HandleSuccess);
	FOnAuthError E;
	E.BindDynamic(this, &UCrowdyAuth_DevLogin::HandleError);
	Auth->DevLogin(Email, S, E);
}

void UCrowdyAuth_DevLogin::HandleSuccess(FCrowdyAuthResult Result)
{
	OnSuccess.Broadcast(Result);
	SetReadyToDestroy();
}

void UCrowdyAuth_DevLogin::HandleError(FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("[CrowdyAuth] Dev login failed: %s"), *Message);
	OnError.Broadcast(Message);
	SetReadyToDestroy();
}

UCrowdyAuth_BeginMagicLinkSignIn* UCrowdyAuth_BeginMagicLinkSignIn::BeginMagicLinkSignIn(UObject* WorldContextObject, const FString& Email)
{
	UCrowdyAuth_BeginMagicLinkSignIn* Action = NewObject<UCrowdyAuth_BeginMagicLinkSignIn>();
	Action->WorldContextObject = WorldContextObject;
	Action->Email = Email;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyAuth_BeginMagicLinkSignIn::Activate()
{
	UCrowdyAuthentication* Auth = GetAuthSubsystem(WorldContextObject);
	if (!Auth)
	{
		HandleError(TEXT("UCrowdyAuthentication subsystem not found"));
		return;
	}

	FOnAuthSuccess S;
	S.BindDynamic(this, &UCrowdyAuth_BeginMagicLinkSignIn::HandleSuccess);
	FOnAuthError E;
	E.BindDynamic(this, &UCrowdyAuth_BeginMagicLinkSignIn::HandleError);
	Auth->BeginMagicLinkSignIn(Email, S, E);
}

void UCrowdyAuth_BeginMagicLinkSignIn::HandleSuccess(FCrowdyAuthResult Result)
{
	OnSuccess.Broadcast(Result);
	SetReadyToDestroy();
}

void UCrowdyAuth_BeginMagicLinkSignIn::HandleError(FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("[CrowdyAuth] Magic-link sign-in failed: %s"), *Message);
	OnError.Broadcast(Message);
	SetReadyToDestroy();
}

UCrowdyAuth_BeginSocialSignIn* UCrowdyAuth_BeginSocialSignIn::BeginSocialSignIn(UObject* WorldContextObject, const FString& Provider)
{
	UCrowdyAuth_BeginSocialSignIn* Action = NewObject<UCrowdyAuth_BeginSocialSignIn>();
	Action->WorldContextObject = WorldContextObject;
	Action->Provider = Provider;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyAuth_BeginSocialSignIn::Activate()
{
	UCrowdyAuthentication* Auth = GetAuthSubsystem(WorldContextObject);
	if (!Auth)
	{
		HandleError(TEXT("UCrowdyAuthentication subsystem not found"));
		return;
	}

	FOnAuthSuccess S;
	S.BindDynamic(this, &UCrowdyAuth_BeginSocialSignIn::HandleSuccess);
	FOnAuthError E;
	E.BindDynamic(this, &UCrowdyAuth_BeginSocialSignIn::HandleError);
	Auth->BeginSocialSignIn(Provider, S, E);
}

void UCrowdyAuth_BeginSocialSignIn::HandleSuccess(FCrowdyAuthResult Result)
{
	OnSuccess.Broadcast(Result);
	SetReadyToDestroy();
}

void UCrowdyAuth_BeginSocialSignIn::HandleError(FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("[CrowdyAuth] Social sign-in failed: %s"), *Message);
	OnError.Broadcast(Message);
	SetReadyToDestroy();
}
