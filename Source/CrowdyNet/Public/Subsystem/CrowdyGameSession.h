#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Math/MathFwd.h"
#include "Utils/SerializationFunctionLibrary.h"
#include <atomic>
#include "CrowdyGameSession.generated.h"


DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnOwnerUUIDUpdated, FString, NewOwnerUUID);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnHostIDUpdated, const FGuid&, NewHostID, const FGuid&, PreviousHostID);

/**
 * 
 */

USTRUCT(BlueprintType)
struct FGameSessionInfo
{
	GENERATED_BODY()
	
	UPROPERTY(BlueprintReadWrite, Category="Crowdy SDK|Game Session")
	int64 AppID = 1;
	
	/** Identity SESSION token (management-plane). Mints/refreshes app tokens; NOT
	 *  accepted for gameplay. Persisted (securely) across sessions. */
	UPROPERTY(BlueprintReadWrite, Category="Crowdy SDK|Game Session")
	FString SessionToken = "";

	UPROPERTY(BlueprintReadWrite, Category="Crowdy SDK|Game Session")
	int64 SessionGameTokenID = 0;

	/** App-scoped GAMEPLAY token. Bearer for the Game API, HMAC key for UDP, and
	 *  carried (as GameTokenID) in the UDP spatial message tail. Short-lived
	 *  (~30 min); kept in memory only, never persisted. */
	UPROPERTY(BlueprintReadWrite, Category="Crowdy SDK|Game Session")
	FString GameToken = "";
	
	UPROPERTY(BlueprintReadWrite, Category="Crowdy SDK|Game Session")
	int64 GameTokenID = 0;
	
	/** ISO-8601 expiry of the app token (from mint/refresh). Drives proactive refresh. */
	UPROPERTY(BlueprintReadWrite, Category="Crowdy SDK|Game Session")
	FString AppTokenExpiresAt = "";

	/** The replication server this client is currently assigned to. Empty until the first assignment succeeds. */
	UPROPERTY(BlueprintReadWrite, Category="Crowdy SDK|Game Session")
	FString ReplicationServerIp4 = "";

	UPROPERTY(BlueprintReadWrite, Category="Crowdy SDK|Game Session")
	int32 ReplicationServerClientPort = 0;

	/** Where the Game API installed the GameToken above, when the rotation that produced it named a server. Empty
	 *  means nowhere, and a connection signing with that token has to re-assign before its datagrams are accepted. */
	UPROPERTY(BlueprintReadWrite, Category="Crowdy SDK|Game Session")
	FString AppTokenAuthorizedServerIp4 = "";

	UPROPERTY(BlueprintReadWrite, Category="Crowdy SDK|Game Session")
	int32 AppTokenAuthorizedServerClientPort = 0;

	/** Per-app Game API endpoints returned by mintAppToken/refreshAppToken. */
	UPROPERTY(BlueprintReadWrite, Category="Crowdy SDK|Game Session")
	FString GameApiUrl = "";

	UPROPERTY(BlueprintReadWrite, Category="Crowdy SDK|Game Session")
	FString GameApiWsUrl = "";

	UPROPERTY(BlueprintReadWrite, Category="Crowdy SDK|Game Session")
	FString LaunchUrl = "";

	UPROPERTY(BlueprintReadWrite, Category="Crowdy SDK|Game Session")
	int64 UserID = 0;
	
	UPROPERTY(BlueprintReadWrite, Category="Crowdy SDK|Game Session")
	FString UUID = "";
	
	UPROPERTY(BlueprintReadWrite, Category="Crowdy SDK|Game Session")
	FGuid ID;
	
	UPROPERTY(BlueprintReadWrite, Category="Crowdy SDK|Game Session")
	double ChunkSize = 1600.0f;
	
	UPROPERTY()
	FInt64Vector CurrentPlayerChunkCoordinates = {0, 0, 0};
	
	UPROPERTY()
	FInt64Vector LastMinigameChunkCoordinates = {0, 0, 0};
	
	UPROPERTY()
	FInt32Vector LastMinigameVoxelCoordinates = {0, 0, 0};
	
	UPROPERTY(BlueprintReadWrite, Category="Crowdy SDK|Game Session")
	bool bWasInMinigame = false;
	
	void Reset()
	{
		// AppID is configuration (loaded from developer settings), not per-session
		// state, so keep it: a sign-in after logout can still mint for the app.
		SessionToken = "";
		SessionGameTokenID = 0;
		GameToken = "";
		GameTokenID = 0;
		AppTokenExpiresAt = "";
		ReplicationServerIp4 = "";
		ReplicationServerClientPort = 0;
		AppTokenAuthorizedServerIp4 = "";
		AppTokenAuthorizedServerClientPort = 0;
		GameApiUrl = "";
		GameApiWsUrl = "";
		LaunchUrl = "";
		UserID = 0;
		UUID = "";
		CurrentPlayerChunkCoordinates = {0, 0, 0};
		LastMinigameChunkCoordinates = {0, 0, 0};
		LastMinigameVoxelCoordinates = {0, 0, 0};
		bWasInMinigame = false;
	}
};

UCLASS(BlueprintType)
class CROWDYNET_API UCrowdyGameSession : public UGameInstanceSubsystem
{
	GENERATED_BODY()
	
public:
	
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Session")
	FOnOwnerUUIDUpdated OnOwnerUUIDUpdated;

	/** Fired on the game thread whenever the elected host changes.
	 *  UCrowdyHostSubsystem (CrowdyServices) writes the host here so that lower
	 *  modules (CrowdyReplication) can read it without a dependency cycle. */
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Session")
	FOnHostIDUpdated OnHostIDUpdated;
	
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Session")
	void SetAppID(const int64 InAppID) {GameSessionInfo.AppID = InAppID;}
	
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Session")
	void SetUserID(const int64 InUserID){GameSessionInfo.UserID = InUserID;}
	
	/**
	 * Install the app-scoped gameplay token.
	 *
	 * A replacement token is authorized on no replication server until something says otherwise, so this clears
	 * the authorized-server pair rather than leaving the outgoing token's. Call SetAppTokenAuthorizedServer
	 * afterwards when the answer that carried this token named one.
	 */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Session")
	void SetGameToken(const FString InGameToken)
	{
		GameSessionInfo.GameToken = InGameToken;
		GameSessionInfo.AppTokenAuthorizedServerIp4 = FString();
		GameSessionInfo.AppTokenAuthorizedServerClientPort = 0;
	}

	/** Where the Game API installed the current app token. Both halves together, since neither means anything alone. */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Session")
	void SetAppTokenAuthorizedServer(const FString InIp4, const int32 InClientPort)
	{
		GameSessionInfo.AppTokenAuthorizedServerIp4 = InIp4;
		GameSessionInfo.AppTokenAuthorizedServerClientPort = InClientPort;
	}

	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Session")
	FString GetAppTokenAuthorizedServerIp4() const {return GameSessionInfo.AppTokenAuthorizedServerIp4;}

	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Session")
	int32 GetAppTokenAuthorizedServerClientPort() const {return GameSessionInfo.AppTokenAuthorizedServerClientPort;}

	/** The replication server this client is assigned to, recorded when an assignment succeeds. */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Session")
	void SetReplicationServer(const FString InIp4, const int32 InClientPort)
	{
		GameSessionInfo.ReplicationServerIp4 = InIp4;
		GameSessionInfo.ReplicationServerClientPort = InClientPort;
	}

	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Session")
	FString GetReplicationServerIp4() const {return GameSessionInfo.ReplicationServerIp4;}

	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Session")
	int32 GetReplicationServerClientPort() const {return GameSessionInfo.ReplicationServerClientPort;}
	
	/** Identity SESSION token (management-plane). */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Session")
	void SetSessionToken(const FString InSessionToken){GameSessionInfo.SessionToken = InSessionToken;}

	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Session")
	FString GetSessionToken() const {return GameSessionInfo.SessionToken;}

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Session")
	void SetSessionGameTokenID(const int64 InSessionGameTokenID){GameSessionInfo.SessionGameTokenID = InSessionGameTokenID;}

	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Session")
	int64 GetSessionGameTokenID() const {return GameSessionInfo.SessionGameTokenID;}

	/** App-token metadata from mintAppToken / refreshAppToken. */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Session")
	void SetAppTokenExpiresAt(const FString InExpiresAt){GameSessionInfo.AppTokenExpiresAt = InExpiresAt;}

	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Session")
	FString GetAppTokenExpiresAt() const {return GameSessionInfo.AppTokenExpiresAt;}

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Session")
	void SetGameApiUrl(const FString InGameApiUrl){GameSessionInfo.GameApiUrl = InGameApiUrl;}

	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Session")
	FString GetGameApiUrl() const {return GameSessionInfo.GameApiUrl;}

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Session")
	void SetGameApiWsUrl(const FString InGameApiWsUrl){GameSessionInfo.GameApiWsUrl = InGameApiWsUrl;}

	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Session")
	FString GetGameApiWsUrl() const {return GameSessionInfo.GameApiWsUrl;}

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Session")
	void SetLaunchUrl(const FString InLaunchUrl){GameSessionInfo.LaunchUrl = InLaunchUrl;}

	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Session")
	FString GetLaunchUrl() const {return GameSessionInfo.LaunchUrl;}
	
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Session")
	void SetUUID(FString InUUID)
	{
		GameSessionInfo.UUID = InUUID;
		GameSessionInfo.ID = USerializationFunctionLibrary::ToGuid(InUUID);
		OnOwnerUUIDUpdated.Broadcast(InUUID);
	}
	
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Session")
	void SetGameTokenID(const int64 InGameTokenID) {GameSessionInfo.GameTokenID = InGameTokenID;}
	
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Session")
	void SetPlayerCurrentChunkCoordinates(const int64 X, const int64 Y, const int64 Z) {GameSessionInfo.CurrentPlayerChunkCoordinates = FInt64Vector(X, Y, Z);}
	
	UFUNCTION()
	FInt64Vector GetPlayerCurrentChunkCoordinates() const {return GameSessionInfo.CurrentPlayerChunkCoordinates;}
	
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Session")
	void SetChunkSize(const double ChunkSize)
	{
		GameSessionInfo.ChunkSize = ChunkSize;
	}
	
	UFUNCTION(BlueprintPure, Category="Crowdy SDK|Game Session")
	double GetChunkSize() const
	{
		return GameSessionInfo.ChunkSize;
	}
	
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Session")
	int64 GetAppID() const { return GameSessionInfo.AppID;}
	
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Session")
	FString GetGameToken() const {return GameSessionInfo.GameToken;}
	
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Session")
	FString GetUUID() const {return GameSessionInfo.UUID;}
	
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Session")
	FGuid GetID() const {return GameSessionInfo.ID;}

	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Session")
	FGuid GetHostID() const {return HostID;}

	/** Game thread only. Called by UCrowdyHostSubsystem when a host is elected. */
	void SetHostID(const FGuid& InHostID)
	{
		if (HostID == InHostID) return;
		const FGuid PreviousHostID = HostID;
		HostID = InHostID;
		OnHostIDUpdated.Broadcast(HostID, PreviousHostID);
	}
	
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Session")
	int64 GetUserID() const {return GameSessionInfo.UserID;}
	
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Session")
	int64 GetGameTokenID() const {return GameSessionInfo.GameTokenID;}

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Session")
	void ClearCurrentSessionData() {GameSessionInfo.Reset();}
	
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Session")
	FGameSessionInfo GetCurrentGameSessionInfo() const {return GameSessionInfo;}
	
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Session")
	void SetLastMinigameCoordinates(int64 ChunkX, int64 ChunkY, int64 ChunkZ, int32 VoxelX, int32 VoxelY, int32 VoxelZ)
	{
		GameSessionInfo.LastMinigameChunkCoordinates = {ChunkX, ChunkY, ChunkZ};
		GameSessionInfo.LastMinigameVoxelCoordinates = {VoxelX, VoxelY, VoxelZ};
	};
	
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Session")
	void GetLastMinigameCoordinates(int64& ChunkX, int64& ChunkY, int64& ChunkZ, int32& VoxelX, int32& VoxelY, int32& VoxelZ) const
	{
		ChunkX = GameSessionInfo.LastMinigameChunkCoordinates.X;
		ChunkY = GameSessionInfo.LastMinigameChunkCoordinates.Y;
		ChunkZ = GameSessionInfo.LastMinigameChunkCoordinates.Z;
		VoxelX = GameSessionInfo.LastMinigameVoxelCoordinates.X;
		VoxelY = GameSessionInfo.LastMinigameVoxelCoordinates.Y;
		VoxelZ = GameSessionInfo.LastMinigameVoxelCoordinates.Z;
	}
	
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Session")
	void SetWasInMinigame(const bool bWasInMinigame) {GameSessionInfo.bWasInMinigame = bWasInMinigame;}
	
private:

	UPROPERTY()
	FGameSessionInfo GameSessionInfo;

	UPROPERTY()
	FGuid HostID;
	
	/** Atomic so it can be written from a background response thread and read
	 *  from the game thread (or any other thread) without a lock. */
	std::atomic<int64> HostUserID { 0 };
};
