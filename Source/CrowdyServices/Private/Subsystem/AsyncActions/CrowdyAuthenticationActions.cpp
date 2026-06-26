#include "Subsystem/AsyncActions/CrowdyAuthenticationActions.h"
#include "CrowdyServicesLog.h"
#include "Subsystem/CrowdyAuthentication.h"

static UCrowdyAuthentication* GetAuthSubsystem(const TWeakObjectPtr<UObject>& Ctx)
{
	if (!Ctx.IsValid()) return nullptr;
	UGameInstance* GI = Ctx->GetWorld() ? Ctx->GetWorld()->GetGameInstance() : nullptr;
	return GI ? GI->GetSubsystem<UCrowdyAuthentication>() : nullptr;
}

// ---- UCrowdyAuth_Login -------------------------------------------------------

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

// ---- UCrowdyAuth_Register ----------------------------------------------------

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

// ---- UCrowdyAuth_RestoreSession ----------------------------------------------

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
	Auth->RestoreSession(S, E);
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
