#include "Subsystem/CrowdyAuthentication.h"
#include "CrowdyServicesLog.h"
#include "Subsystem/Data/CrowdyAuthSaveGame.h"
#include "Async/Async.h"
#include "Core/CrowdySDKBridgeSubsystem.h"
#include "Kismet/GameplayStatics.h"
#include "Network/GraphQL/CrowdyQuerySubsystem.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Core/GraphQL/Enums/EQueryResponseType.h"
#include "Core/GraphQL/Enums/EGraphQLQuery.h"
#include "Queries/Authentication/FLoginResponse.h"
#include "Queries/Authentication/FRegisterResponse.h"

static const FString AuthSaveSlot  = TEXT("CrowdyAuth");
static const int32   AuthUserIndex = 0;

//Lifecycle 

void UCrowdyAuthentication::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

}

void UCrowdyAuthentication::Deinitialize()
{
	FScopeLock Lock(&CallbackMutex);
	PendingCallbacks.Empty();
	Super::Deinitialize();
}

void UCrowdyAuthentication::InjectDependencies(FCrowdyDataRegistry* InRegistry,
                                               UCrowdyQuerySubsystem* InQuerySubsystem,
                                               UCrowdyGameSession* InGameSession)
{
	if (InRegistry) InRegistry->RegisterLayer(this);
	QuerySubsystem = InQuerySubsystem;
	GameSession    = InGameSession;
}

// Reception layer

TArray<EQueryResponseType> UCrowdyAuthentication::GetSupportedResponseType() const
{
	return { EQueryResponseType::Login, EQueryResponseType::Register };
}

void UCrowdyAuthentication::OnResponseReceived(TSharedPtr<ICrowdyQueryResponse> Response)
{
	if (!Response.IsValid()) return;
	FireCallback(Response);
}

// Internal helpers

void UCrowdyAuthentication::ApplySessionAndKickUDP(const FString& GameToken, int64 GameTokenID, int64 UserID)
{
	if (GameSession)
	{
		GameSession->SetUserID(UserID);
		GameSession->SetGameTokenID(GameTokenID);
		GameSession->SetGameToken(GameToken);
	}
	if (QuerySubsystem)
	{
		QuerySubsystem->SetAuthToken(GameToken);
		QuerySubsystem->ExecuteQueryByID(EGraphQLQuery::UDP_Access, TMap<FString, FString>(), true, false);
	}
}

void UCrowdyAuthentication::SaveCredentials(const FString& GameToken, int64 GameTokenID, int64 UserID) const
{
	UCrowdyAuthSaveGame* Save = Cast<UCrowdyAuthSaveGame>(
		UGameplayStatics::CreateSaveGameObject(UCrowdyAuthSaveGame::StaticClass()));
	if (!Save) return;

	Save->GameToken   = GameToken;
	Save->GameTokenID = GameTokenID;
	Save->UserID      = UserID;
	UGameplayStatics::SaveGameToSlot(Save, AuthSaveSlot, AuthUserIndex);
}

// Login 

void UCrowdyAuthentication::Login(const FString& Email, const FString& Password,
                                  FOnAuthSuccess OnSuccess, FOnAuthError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::Login, [this, OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
		{
			const FLoginResponse& R = static_cast<FLoginResponse&>(*Resp);

			UE_CLOG(CrowdyServicesTrace::Services(), LogCrowdyServices, Log, TEXT("[CrowdyAuth] Login successful. UserID=%lld GameTokenID=%lld"),
				R.UserID, R.GameTokenID);

			SaveCredentials(R.GameToken, R.GameTokenID, R.UserID);
			ApplySessionAndKickUDP(R.GameToken, R.GameTokenID, R.UserID);

			FCrowdyAuthResult Result;
			Result.GameToken = R.GameToken;
			Result.UserID    = R.UserID;

			OnSuccess.ExecuteIfBound(Result);
			OnLogin.Broadcast(Result);
		}
		else
		{
			const FString Msg = Resp->GetError();
			OnError.ExecuteIfBound(Msg);
			OnLoginFailed.Broadcast(Msg);
		}
	});

	TMap<FString, FString> Vars;
	Vars.Add(TEXT("email"),    Email);
	Vars.Add(TEXT("password"), Password);
	QuerySubsystem->ExecuteQueryByID(EGraphQLQuery::Login, Vars, false, false);
}

//Register 

void UCrowdyAuthentication::Register(const FString& Email, const FString& Password,
                                     FOnAuthSuccess OnSuccess, FOnAuthError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::Register, [this, OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
		{
			const FRegisterResponse& R = static_cast<FRegisterResponse&>(*Resp);

			// FRegisterResponse has no GameTokenID field; use 0 as placeholder.
			SaveCredentials(R.GameToken, 0, R.UserID);

			if (GameSession)
			{
				GameSession->SetUserID(R.UserID);
				GameSession->SetGameToken(R.GameToken);
			}
			if (QuerySubsystem) QuerySubsystem->SetAuthToken(R.GameToken);

			FCrowdyAuthResult Result;
			Result.GameToken = R.GameToken;
			Result.UserID    = R.UserID;

			OnSuccess.ExecuteIfBound(Result);
			OnRegister.Broadcast(Result);
		}
		else
		{
			const FString Msg = Resp->GetError();
			OnError.ExecuteIfBound(Msg);
			OnRegisterFailed.Broadcast(Msg);
		}
	});

	TMap<FString, FString> Vars;
	Vars.Add(TEXT("email"),    Email);
	Vars.Add(TEXT("password"), Password);
	QuerySubsystem->ExecuteQueryByID(EGraphQLQuery::Register, Vars, false, false);
}

// Session restore

bool UCrowdyAuthentication::HasSavedSession() const
{
	if (!UGameplayStatics::DoesSaveGameExist(AuthSaveSlot, AuthUserIndex)) return false;

	const UCrowdyAuthSaveGame* Save = Cast<UCrowdyAuthSaveGame>(
		UGameplayStatics::LoadGameFromSlot(AuthSaveSlot, AuthUserIndex));
	return Save && !Save->GameToken.IsEmpty();
}

bool UCrowdyAuthentication::RestoreSession(FOnAuthSuccess OnSuccess, FOnAuthError OnError)
{
	if (!UGameplayStatics::DoesSaveGameExist(AuthSaveSlot, AuthUserIndex))
	{
		const FString Msg = TEXT("No saved session found");
		OnError.ExecuteIfBound(Msg);
		OnSessionRestoreFailed.Broadcast(Msg);
		return false;
	}

	const UCrowdyAuthSaveGame* Save = Cast<UCrowdyAuthSaveGame>(
		UGameplayStatics::LoadGameFromSlot(AuthSaveSlot, AuthUserIndex));

	if (!Save || Save->GameToken.IsEmpty())
	{
		const FString Msg = TEXT("Saved session is empty or corrupt");
		OnError.ExecuteIfBound(Msg);
		OnSessionRestoreFailed.Broadcast(Msg);
		return false;
	}

	UE_CLOG(CrowdyServicesTrace::Services(), LogCrowdyServices, Log, TEXT("[CrowdyAuth] Restoring session. UserID=%lld GameTokenID=%lld"),
		Save->UserID, Save->GameTokenID);

	ApplySessionAndKickUDP(Save->GameToken, Save->GameTokenID, Save->UserID);

	FCrowdyAuthResult Result;
	Result.GameToken = Save->GameToken;
	Result.UserID    = Save->UserID;

	OnSuccess.ExecuteIfBound(Result);
	OnSessionRestored.Broadcast(Result);
	return true;
}

void UCrowdyAuthentication::ClearSavedSession()
{
	if (UGameplayStatics::DoesSaveGameExist(AuthSaveSlot, AuthUserIndex))
		UGameplayStatics::DeleteGameInSlot(AuthSaveSlot, AuthUserIndex);
}

// Callback queue

void UCrowdyAuthentication::PushCallback(EQueryResponseType Type,
                                         TFunction<void(TSharedPtr<ICrowdyQueryResponse>)> Callback)
{
	FScopeLock Lock(&CallbackMutex);
	PendingCallbacks.FindOrAdd(Type).Add(MoveTemp(Callback));
}

void UCrowdyAuthentication::FireCallback(TSharedPtr<ICrowdyQueryResponse> Response)
{
	TFunction<void(TSharedPtr<ICrowdyQueryResponse>)> Callback;
	{
		FScopeLock Lock(&CallbackMutex);
		TArray<TFunction<void(TSharedPtr<ICrowdyQueryResponse>)>>* Queue =
			PendingCallbacks.Find(Response->GetResponseType());
		if (Queue && Queue->Num() > 0)
		{
			Callback = MoveTemp((*Queue)[0]);
			Queue->RemoveAt(0, 1, EAllowShrinking::No);
		}
	}

	if (Callback)
	{
		TSharedPtr<ICrowdyQueryResponse> Copy = Response;
		AsyncTask(ENamedThreads::GameThread, [Callback = MoveTemp(Callback), Copy]()
		{
			Callback(Copy);
		});
	}
}
