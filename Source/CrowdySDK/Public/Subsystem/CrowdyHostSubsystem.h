// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "CrowdyGameSession.h"
#include "Replication/Interfaces/CrowdyEventReceiver.h"
#include <atomic>
#include "Subsystems/WorldSubsystem.h"
#include "CrowdyHostSubsystem.generated.h"


struct FCrowdySelectIDAsHost;
class UCrowdySDKSubsystem;
struct FInstancedStruct;
class UCrowdyActorTracker;
class UCrowdyGameSession;


DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnCrowdyHostElected, const FGuid&, HostID, const FGuid&, PreviousHostID);

/**
 * 
 */
UCLASS(BlueprintType, meta=(DisplayName="Crowdy Host Subsystem"))
class CROWDYSDK_API UCrowdyHostSubsystem : public UWorldSubsystem, public ICrowdyEventReceiver
{
	GENERATED_BODY()

public:
	
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	
	
	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Host Subsystem|Events")
	FOnCrowdyHostElected OnHostElected;
	
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy SDK|Host Subsystem|Data")
	FGuid GetHostID() const;
	
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy SDK|Host Subsystem|Data")
	bool IsHostSet() const {return HostID.IsValid();}

	// ── Host tracking ──────────────────────────────────────────────────────
	/** Called by the SDK when a GameHost response arrives. Thread-safe.
	 *  Broadcasts OnHostSet on the game thread whenever the host changes. */
	void SetHostUserID(const int64 InHostUserID);

	/** Returns the user ID of the current game host (0 if not yet known). */
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Session",
		meta=(DisplayName="Get Host User ID"))
	int64 GetHostUserID() const
	{
		return HostUserID.load(std::memory_order_relaxed);
	}
	
	/**
	 * Returns true when the local user IS the current game host.
	 * Safe to call from any thread and from Blueprint.
	 */
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Session",
		meta=(DisplayName="Is Host"))
	bool IsHost() const
	{
		const int64 LocalID = GameSession->GetUserID();
		const int64 CurrentHostID = HostUserID.load(std::memory_order_relaxed);
		return LocalID != 0 && LocalID == CurrentHostID;
	}
	
	bool IsReady() const;
	
private:
	
	std::atomic<int64> HostUserID { 0 };
	
	UPROPERTY()
	TObjectPtr<UCrowdyGameSession> GameSession;
	
	UPROPERTY()
	TObjectPtr<UCrowdySDKSubsystem> CrowdySDK;
	
	UPROPERTY()
	FGuid HostID;
	
	UPROPERTY()
	FGuid PreviousHostID;
	
	UPROPERTY()
	FGuid LocalPlayerID;
	
	bool bIsReady = false;
	
private:
	
	UFUNCTION()
	void OnOwnerPlayerIDSet(FString OwnerID);

	bool LoadConfig() const;
};
