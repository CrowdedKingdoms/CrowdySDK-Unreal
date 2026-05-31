#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Containers/Queue.h"
#include "HAL/CriticalSection.h"
#include "Math/MathFwd.h"
#include "Utils/SerializationFunctionLibrary.h"
#include <atomic>
#include "CrowdyGameSession.generated.h"


DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnOwnerUUIDUpdated, FString, NewOwnerUUID);

/**
 * 
 */

USTRUCT(BlueprintType)
struct FGameSessionInfo
{
	GENERATED_BODY()
	
	UPROPERTY(BlueprintReadWrite, Category="Crowdy SDK|Game Session")
	int64 AppID = 1;
	
	UPROPERTY(BlueprintReadWrite, Category="Crowdy SDK|Game Session")
	FString GameToken = "";
	
	UPROPERTY(BlueprintReadWrite, Category="Crowdy SDK|Game Session")
	int64 GameTokenID = 0;
	
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
		AppID = 0;
		GameToken = "";
		GameTokenID = 0;
		UserID = 0;
		UUID = "";
		CurrentPlayerChunkCoordinates = {0, 0, 0};
		LastMinigameChunkCoordinates = {0, 0, 0};
		LastMinigameVoxelCoordinates = {0, 0, 0};
		bWasInMinigame = false;
	}
};

UCLASS(BlueprintType)
class CROWDYSDK_API UCrowdyGameSession : public UGameInstanceSubsystem
{
	GENERATED_BODY()
	
public:
	
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Session")
	FOnOwnerUUIDUpdated OnOwnerUUIDUpdated;
	
	[[nodiscard]] bool EnqueueMessageToSend(TArray<uint8>&& Message);

	bool DequeueMessageToSend(TArray<uint8>& OutMessage);

	void EnqueueMessageToReceive(const TArray<uint8>& Message);
	
	[[nodiscard]] bool DequeueMessageToReceive(TArray<uint8>& OutMessage);
	
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Session")
	void SetAppID(const int64 InAppID) {GameSessionInfo.AppID = InAppID;}
	
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Session")
	void SetUserID(const int64 InUserID){GameSessionInfo.UserID = InUserID;}
	
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Session")
	void SetGameToken(const FString InGameToken){GameSessionInfo.GameToken = InGameToken;}
	
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
	int64 GetUserID() const {return GameSessionInfo.UserID;}
	
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Session")
	int64 GetGameTokenID() const {return GameSessionInfo.GameTokenID;}

	// ──────────────────────────────────────────────────────────────────────

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
	
	[[nodiscard]] bool HasPendingIncomingMessages() const; 
	
	[[nodiscard]] bool HasPendingOutgoingMessages() const;
	
	FEvent* GetSendEvent() const;
	FEvent* GetReceiveEvent() const;
private:

	UPROPERTY()
	FGameSessionInfo GameSessionInfo;

	UPROPERTY()
	FGuid HostID;
	
	/** Atomic so it can be written from a background response thread and read
	 *  from the game thread (or any other thread) without a lock. */
	std::atomic<int64> HostUserID { 0 };

	TQueue<TArray<uint8>, EQueueMode::Mpsc> SendQueue;
	TQueue<TArray<uint8>, EQueueMode::Spsc> ReceiveQueue;
	
	std::atomic<int32> SendCounter = 0;
	std::atomic<int32> ReceiveCounter = 0;
	
	FCriticalSection SendQueueMutex;
	FCriticalSection ReceiveQueueMutex;
	
	FEvent* SendEvent;
	FEvent* ReceiveEvent;
};
