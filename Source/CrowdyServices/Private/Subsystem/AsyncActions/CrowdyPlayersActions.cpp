#include "Subsystem/AsyncActions/CrowdyPlayersActions.h"
#include "CrowdyServicesLog.h"
#include "Subsystem/AsyncActions/CrowdyServicesActionSupport.h"

static const TCHAR* const PlayersMissingMessage = TEXT("UCrowdyPlayers subsystem not found");

UCrowdyPlayers_GetPlayerName* UCrowdyPlayers_GetPlayerName::GetPlayerName(UObject* WorldContextObject, int64 UserID)
{
	UCrowdyPlayers_GetPlayerName* Action = NewObject<UCrowdyPlayers_GetPlayerName>();
	Action->WorldContextObject = WorldContextObject;
	Action->UserID = UserID;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyPlayers_GetPlayerName::Activate()
{
	UCrowdyPlayers* Players = GetServiceSubsystem<UCrowdyPlayers>(WorldContextObject);
	if (!Players)
	{
		HandleError(FCrowdyAccountError::Make(ECrowdyAccountErrorCode::Unknown, PlayersMissingMessage), PlayersMissingMessage);
		return;
	}
	FOnPlayerProfileRead Read;
	Read.BindDynamic(this, &UCrowdyPlayers_GetPlayerName::HandleRead);
	FOnPlayersError Failed;
	Failed.BindDynamic(this, &UCrowdyPlayers_GetPlayerName::HandleError);
	Players->GetPlayerName(UserID, Read, Failed);
}

void UCrowdyPlayers_GetPlayerName::HandleRead(FCrowdyPlayerProfile Profile)
{
	OnSuccess.Broadcast(Profile, FCrowdyAccountError(), FString());
	SetReadyToDestroy();
}

void UCrowdyPlayers_GetPlayerName::HandleError(FCrowdyAccountError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("[CrowdyPlayers] Get Player Name failed: %s"), *Message);
	OnError.Broadcast(FCrowdyPlayerProfile(), Error, Message);
	SetReadyToDestroy();
}

UCrowdyPlayers_GetPlayerNames* UCrowdyPlayers_GetPlayerNames::GetPlayerNames(UObject* WorldContextObject,
	const TArray<int64>& UserIDs)
{
	UCrowdyPlayers_GetPlayerNames* Action = NewObject<UCrowdyPlayers_GetPlayerNames>();
	Action->WorldContextObject = WorldContextObject;
	Action->UserIDs = UserIDs;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyPlayers_GetPlayerNames::Activate()
{
	UCrowdyPlayers* Players = GetServiceSubsystem<UCrowdyPlayers>(WorldContextObject);
	if (!Players)
	{
		HandleError(FCrowdyAccountError::Make(ECrowdyAccountErrorCode::Unknown, PlayersMissingMessage), PlayersMissingMessage);
		return;
	}
	FOnPlayerProfilesRead Read;
	Read.BindDynamic(this, &UCrowdyPlayers_GetPlayerNames::HandleRead);
	FOnPlayersError Failed;
	Failed.BindDynamic(this, &UCrowdyPlayers_GetPlayerNames::HandleError);
	Players->GetPlayerNames(UserIDs, Read, Failed);
}

void UCrowdyPlayers_GetPlayerNames::HandleRead(const TArray<FCrowdyPlayerProfile>& Profiles)
{
	OnSuccess.Broadcast(Profiles, FCrowdyAccountError(), FString());
	SetReadyToDestroy();
}

void UCrowdyPlayers_GetPlayerNames::HandleError(FCrowdyAccountError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("[CrowdyPlayers] Get Player Names failed: %s"), *Message);
	OnError.Broadcast(TArray<FCrowdyPlayerProfile>(), Error, Message);
	SetReadyToDestroy();
}

UCrowdyPlayers_SetPlayerName* UCrowdyPlayers_SetPlayerName::SetPlayerName(UObject* WorldContextObject,
	const FString& Gamertag, const FString& Disambiguation)
{
	UCrowdyPlayers_SetPlayerName* Action = NewObject<UCrowdyPlayers_SetPlayerName>();
	Action->WorldContextObject = WorldContextObject;
	Action->Gamertag = Gamertag;
	Action->Disambiguation = Disambiguation;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyPlayers_SetPlayerName::Activate()
{
	UCrowdyPlayers* Players = GetServiceSubsystem<UCrowdyPlayers>(WorldContextObject);
	if (!Players)
	{
		HandleError(FCrowdyAccountError::Make(ECrowdyAccountErrorCode::Unknown, PlayersMissingMessage), PlayersMissingMessage);
		return;
	}
	FOnPlayerProfileRead Read;
	Read.BindDynamic(this, &UCrowdyPlayers_SetPlayerName::HandleRead);
	FOnPlayersError Failed;
	Failed.BindDynamic(this, &UCrowdyPlayers_SetPlayerName::HandleError);
	Players->SetPlayerName(Gamertag, Disambiguation, Read, Failed);
}

void UCrowdyPlayers_SetPlayerName::HandleRead(FCrowdyPlayerProfile Profile)
{
	OnSuccess.Broadcast(Profile, FCrowdyAccountError(), FString());
	SetReadyToDestroy();
}

void UCrowdyPlayers_SetPlayerName::HandleError(FCrowdyAccountError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("[CrowdyPlayers] Set Player Name failed: %s"), *Message);
	OnError.Broadcast(FCrowdyPlayerProfile(), Error, Message);
	SetReadyToDestroy();
}
