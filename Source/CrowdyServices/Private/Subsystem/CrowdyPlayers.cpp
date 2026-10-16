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

	// Both fields are required: the pair is the player's unique handle.
	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("gamertag"), Gamertag);
	Input->SetStringField(TEXT("disambiguation"), Disambiguation);
	Client->RunOp(ECrowdyCppApiDomain::Users, TEXT("UpdateGamertag"), CrowdyServiceApi::WrapInput(Input),
		CrowdyServiceApi::GuardLifetime<FCrowdyCppJsonResult>(LiveToken, [OnResult, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyAccountError Error;
			FCrowdyPlayerProfile Profile;
			if (!CrowdyServiceApi::ReadObject(Result, TEXT("updateGamertag"), Profile, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnResult.ExecuteIfBound(Profile);
		}),
		// A rename changes the account, so the platform takes it only under the session token.
		ECrowdyCppTokenPlane::Management);
}
