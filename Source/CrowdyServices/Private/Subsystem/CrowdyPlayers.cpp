#include "Subsystem/CrowdyPlayers.h"
#include "CrowdyServiceApiSupport.h"

void UCrowdyPlayers::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	LiveToken = MakeShared<uint8>(0);
}

void UCrowdyPlayers::Deinitialize()
{
	LiveToken.Reset();
	Super::Deinitialize();
}

#if WITH_DEV_AUTOMATION_TESTS
void UCrowdyPlayers::InitializeForTest(const TSharedPtr<FCrowdyCppClient>& Client)
{
	LiveToken = MakeShared<uint8>(0);
	ClientForTest = Client;
}
#endif

FCrowdyCppClient* UCrowdyPlayers::ResolveClient() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (const TSharedPtr<FCrowdyCppClient> Override = ClientForTest.Pin())
	{
		return Override.Get();
	}
#endif
	return CrowdyServiceApi::ResolveApiClient(GetGameInstance(), TEXT("CrowdyPlayers"));
}

void UCrowdyPlayers::GetPlayerName(int64 UserID, FOnPlayerProfileRead OnResult, FOnPlayersError OnError)
{
	FCrowdyCppClient* Client = ResolveClient();
	if (!Client)
	{
		const FCrowdyAccountError Error = CrowdyServiceApi::ClientUnavailableError<FCrowdyAccountError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("userId"), CrowdyServiceApi::BigInt(UserID));
	Client->RunOp(ECrowdyCppApiDomain::Users, TEXT("PlayerProfile"), Variables,
		CrowdyServiceApi::GuardLifetime<FCrowdyCppJsonResult>(LiveToken, [OnResult, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyAccountError Error;
			const TSharedPtr<FJsonObject>* Payload = CrowdyServiceApi::ReadPayload(Result, Error);
			if (!Payload)
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			const TSharedPtr<FJsonObject>* Row = nullptr;
			FCrowdyPlayerProfile Profile;
			if (!(*Payload)->TryGetObjectField(TEXT("playerProfile"), Row) || !FCrowdyPlayerProfile::ParseFromJson(*Row, Profile))
			{
				Error = FCrowdyAccountError::Make(ECrowdyAccountErrorCode::NotFound, TEXT("No player has that User ID."));
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnResult.ExecuteIfBound(Profile);
		}),
		ECrowdyCppTokenPlane::Game);
}

void UCrowdyPlayers::GetPlayerNames(const TArray<int64>& UserIDs, FOnPlayerProfilesRead OnResult, FOnPlayersError OnError)
{
	if (UserIDs.Num() > MaxProfilesPerCall)
	{
		const FCrowdyAccountError Error = FCrowdyAccountError::Make(ECrowdyAccountErrorCode::Unknown,
			FString::Printf(TEXT("Get Player Names takes at most %d User IDs; %d were given, so nothing was sent."),
				MaxProfilesPerCall, UserIDs.Num()));
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}
	if (UserIDs.IsEmpty())
	{
		OnResult.ExecuteIfBound(TArray<FCrowdyPlayerProfile>());
		return;
	}

	FCrowdyCppClient* Client = ResolveClient();
	if (!Client)
	{
		const FCrowdyAccountError Error = CrowdyServiceApi::ClientUnavailableError<FCrowdyAccountError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TArray<TSharedPtr<FJsonValue>> Ids;
	Ids.Reserve(UserIDs.Num());
	for (const int64 UserID : UserIDs)
	{
		Ids.Add(MakeShared<FJsonValueString>(CrowdyServiceApi::BigInt(UserID)));
	}
	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetArrayField(TEXT("userIds"), Ids);
	Client->RunOp(ECrowdyCppApiDomain::Users, TEXT("PlayerProfiles"), Variables,
		CrowdyServiceApi::GuardLifetime<FCrowdyCppJsonResult>(LiveToken, [OnResult, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyAccountError Error;
			TArray<FCrowdyPlayerProfile> Profiles;
			if (!CrowdyServiceApi::ReadArray(Result, TEXT("playerProfiles"), Profiles, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnResult.ExecuteIfBound(Profiles);
		}),
		ECrowdyCppTokenPlane::Game);
}

void UCrowdyPlayers::SetPlayerName(const FString& Gamertag, const FString& Disambiguation, FOnPlayerProfileRead OnResult,
	FOnPlayersError OnError)
{
	FCrowdyCppClient* Client = ResolveClient();
	if (!Client)
	{
		const FCrowdyAccountError Error = CrowdyServiceApi::ClientUnavailableError<FCrowdyAccountError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	Client->UpdateGamertag(Gamertag, Disambiguation,
		CrowdyServiceApi::GuardLifetime<FCrowdyCppJsonValueResult>(LiveToken, [OnResult, OnError](FCrowdyCppJsonValueResult Result)
		{
			FCrowdyAccountError Error;
			const TSharedPtr<FJsonObject>* User = nullptr;
			FCrowdyPlayerProfile Profile;
			if (!Result.bOk)
			{
				Error = Result.ErrorMessage == FCrowdyCppClient::CanceledErrorMessage()
					? FCrowdyAccountError::Make(ECrowdyAccountErrorCode::NetworkError, TEXT("The request was interrupted before it completed."))
					: FCrowdyAccountError::FromFailure(Result.ErrorMessage, Result.ErrorCode);
			}
			else if (!Result.Value.IsValid() || !Result.Value->TryGetObject(User) || !FCrowdyPlayerProfile::ParseFromJson(*User, Profile))
			{
				Error = CrowdyServiceApi::UnreadableResponseError<FCrowdyAccountError>();
			}
			if (!Error.Message.IsEmpty())
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnResult.ExecuteIfBound(Profile);
		}));
}

void UCrowdyPlayers::GetMyAppState(FOnPlayerAppStateRead OnResult, FOnPlayersError OnError)
{
	FCrowdyCppClient* Client = ResolveClient();
	if (!Client)
	{
		const FCrowdyAccountError Error = CrowdyServiceApi::ClientUnavailableError<FCrowdyAccountError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), CrowdyServiceApi::BigInt(CrowdyServiceApi::ResolveAppId(GetGameInstance())));
	Client->RunOp(ECrowdyCppApiDomain::State, TEXT("UserAppState"), Variables,
		CrowdyServiceApi::GuardLifetime<FCrowdyCppJsonResult>(LiveToken, [OnResult, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyAccountError Error;
			const TSharedPtr<FJsonObject>* Payload = CrowdyServiceApi::ReadPayload(Result, Error);
			if (!Payload)
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			// Null means never saved and a null state means cleared; a missing field is an answer that cannot be read, never empty.
			const TSharedPtr<FJsonValue> Row = (*Payload)->TryGetField(TEXT("userAppState"));
			if (Row.IsValid() && Row->IsNull())
			{
				OnResult.ExecuteIfBound(FString());
				return;
			}
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!Row.IsValid() || !Row->TryGetObject(Object))
			{
				Error = CrowdyServiceApi::UnreadableResponseError<FCrowdyAccountError>();
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			const TSharedPtr<FJsonValue> StateValue = (*Object)->TryGetField(TEXT("state"));
			FString State;
			if (!StateValue.IsValid() || (!StateValue->IsNull() && StateValue->Type != EJson::String))
			{
				Error = CrowdyServiceApi::UnreadableResponseError<FCrowdyAccountError>();
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			if (!StateValue->IsNull())
			{
				State = StateValue->AsString();
			}
			OnResult.ExecuteIfBound(State);
		}),
		ECrowdyCppTokenPlane::Game);
}

void UCrowdyPlayers::SetMyAppState(const FString& State, FOnPlayerAppStateWritten OnDone, FOnPlayersError OnError)
{
	FCrowdyCppClient* Client = ResolveClient();
	if (!Client)
	{
		const FCrowdyAccountError Error = CrowdyServiceApi::ClientUnavailableError<FCrowdyAccountError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("appId"), CrowdyServiceApi::BigInt(CrowdyServiceApi::ResolveAppId(GetGameInstance())));
	if (State.IsEmpty())
	{
		Input->SetField(TEXT("state"), MakeShared<FJsonValueNull>());
	}
	else
	{
		Input->SetStringField(TEXT("state"), State);
	}
	Client->RunOp(ECrowdyCppApiDomain::State, TEXT("UpdateUserAppState"), CrowdyServiceApi::WrapInput(Input),
		CrowdyServiceApi::GuardLifetime<FCrowdyCppJsonResult>(LiveToken, [OnDone, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyAccountError Error;
			const TSharedPtr<FJsonObject>* Payload = CrowdyServiceApi::ReadPayload(Result, Error);
			if (!Payload)
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			if (!(*Payload)->HasTypedField<EJson::Object>(TEXT("updateUserAppState")))
			{
				Error = CrowdyServiceApi::UnreadableResponseError<FCrowdyAccountError>();
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnDone.ExecuteIfBound();
		}),
		ECrowdyCppTokenPlane::Game);
}
