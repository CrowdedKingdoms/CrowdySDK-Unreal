#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Subsystem/CrowdyPlayers.h"
#include "CrowdyPlayersActions.generated.h"

// One signature serves On Success and On Error, because a Blueprint async node builds its data pins from the first delegate only.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FPlayerProfileAsyncResult, FCrowdyPlayerProfile, Profile, FCrowdyAccountError, Error, FString, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FPlayerProfilesAsyncResult, const TArray<FCrowdyPlayerProfile>&, Profiles, FCrowdyAccountError, Error, FString, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FPlayerAppStateAsyncResult, const FString&, State, FCrowdyAccountError, Error, FString, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FPlayerAppStateWriteAsyncResult, FCrowdyAccountError, Error, FString, Message);

UCLASS()
class CROWDYSERVICES_API UCrowdyPlayers_GetMyAppState : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FPlayerAppStateAsyncResult OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FPlayerAppStateAsyncResult OnError;

	/** Reads the signed-in player's private state for this app, base64; empty when none was saved. Only that player can read it, from any machine. Deserialize Avatar State to Struct turns it back into a struct. */
	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Players|State", DisplayName="Get My App State")
	static UCrowdyPlayers_GetMyAppState* GetMyAppState(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;

	UFUNCTION() void HandleRead(const FString& State);
	UFUNCTION() void HandleError(FCrowdyAccountError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyPlayers_SetMyAppState : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FPlayerAppStateWriteAsyncResult OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FPlayerAppStateWriteAsyncResult OnError;

	/** Replaces the signed-in player's private state for this app with State, base64 (Serialize Struct to Avatar State makes it from a struct); empty clears it. Kept per account, not per avatar or machine. */
	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Players|State", DisplayName="Set My App State")
	static UCrowdyPlayers_SetMyAppState* SetMyAppState(UObject* WorldContextObject, const FString& State);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString State;

	UFUNCTION() void HandleWritten();
	UFUNCTION() void HandleError(FCrowdyAccountError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyPlayers_GetPlayerName : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FPlayerProfileAsyncResult OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FPlayerProfileAsyncResult OnError;

	/** Reads one player's name, for a nametag. A player reads their own with their own User ID. */
	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Players", DisplayName="Get Player Name")
	static UCrowdyPlayers_GetPlayerName* GetPlayerName(UObject* WorldContextObject, int64 UserID);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 UserID = 0;

	UFUNCTION() void HandleRead(FCrowdyPlayerProfile Profile);
	UFUNCTION() void HandleError(FCrowdyAccountError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyPlayers_GetPlayerNames : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FPlayerProfilesAsyncResult OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FPlayerProfilesAsyncResult OnError;

	/** Reads up to 100 players' names in one call, for a roster. Unknown User IDs are left out. */
	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Players", DisplayName="Get Player Names")
	static UCrowdyPlayers_GetPlayerNames* GetPlayerNames(UObject* WorldContextObject, const TArray<int64>& UserIDs);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	TArray<int64> UserIDs;

	UFUNCTION() void HandleRead(const TArray<FCrowdyPlayerProfile>& Profiles);
	UFUNCTION() void HandleError(FCrowdyAccountError Error, FString Message);
};

UCLASS()
class CROWDYSERVICES_API UCrowdyPlayers_SetPlayerName : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FPlayerProfileAsyncResult OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FPlayerProfileAsyncResult OnError;

	/** Renames the signed-in player. Gamertag and Disambiguation together must be unused, or it fails as Name Taken. */
	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Players", DisplayName="Set Player Name")
	static UCrowdyPlayers_SetPlayerName* SetPlayerName(UObject* WorldContextObject, const FString& Gamertag,
		const FString& Disambiguation);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString Gamertag;
	FString Disambiguation;

	UFUNCTION() void HandleRead(FCrowdyPlayerProfile Profile);
	UFUNCTION() void HandleError(FCrowdyAccountError Error, FString Message);
};
