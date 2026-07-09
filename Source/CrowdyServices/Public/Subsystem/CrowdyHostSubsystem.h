// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Subsystem/CrowdyGameSession.h"
#include <atomic>
#include "Subsystems/WorldSubsystem.h"
#include "Templates/Function.h"
#include "CrowdyHostSubsystem.generated.h"


struct FCrowdySelectIDAsHost;
struct FInstancedStruct;
class UCrowdyActorTracker;
class UCrowdyGameSession;
class UCrowdyQuerySubsystem;


DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnCrowdyHostElected, const FGuid&, HostID, const FGuid&, PreviousHostID);

/**
 * 
 */
UCLASS(BlueprintType, meta=(DisplayName="Crowdy Host Subsystem"))
class CROWDYSERVICES_API UCrowdyHostSubsystem : public UWorldSubsystem
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
	bool IsHostSet() const {return IsValid(GameSession) && GameSession->GetHostID().IsValid();}

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
		if (!IsValid(GameSession)) return false;
		const int64 LocalID = GameSession->GetUserID();
		const int64 CurrentHostID = HostUserID.load(std::memory_order_relaxed);
		return LocalID != 0 && LocalID == CurrentHostID;
	}
	
	bool IsReady() const;

	/**
	 * Server-validated "is this actor the host" check for an arbitrary Crowdy entity.
	 *
	 * The Game API elects a host per USER (there is no is-host-by-actor query), so:
	 *  - For the LOCAL player's own entity we ask amIGameHost — the authoritative,
	 *    server-side yes/no for this client.
	 *  - For any OTHER actor we resolve its owner userId server-side (actor(uuid)) and
	 *    compare it to the currently elected host userId (from the gameHost poll).
	 *
	 * Callback fires on the game thread:
	 *   bSuccess == false -> the answer could not be determined (unresolved entity,
	 *                        network/GraphQL error, or no host yet). bIsHost is false.
	 *   bSuccess == true  -> bIsHost is the authoritative answer.
	 *
	 * This is the server-validated sibling of UCrowdyUtilities::IsCrowdyEntityHost
	 * (which only compares client-derived deterministic GUIDs). Call from the game thread.
	 */
	void CheckEntityIsHost(const AActor* Entity, TFunction<void(bool bSuccess, bool bIsHost)> Callback);

	/** Called by UCrowdySDKSubsystem on the game thread when the matching response lands. */
	void HandleAmIGameHostResponse(bool bSuccess, bool bAmHost);
	void HandleActorOwnerResponse(bool bSuccess, const FString& Uuid, int64 UserId);

private:

	/** Dispatch amIGameHost / actor(uuid) via the shared query subsystem and queue the callback. */
	void RequestAmIGameHost(TFunction<void(bool bSuccess, bool bAmHost)> Callback);
	void RequestActorOwner(const FString& Uuid, TFunction<void(bool bSuccess, int64 UserId)> Callback);

	UCrowdyQuerySubsystem* ResolveQuerySubsystem() const;

	// Pending server-check callbacks. Touched only on the game thread (dispatch from
	// CheckEntityIsHost, resolution from the game-thread-marshaled Handle* forwards),
	// so no lock is required. amIGameHost carries no correlation id (a global boolean),
	// so it resolves FIFO; actor(uuid) echoes its uuid so it resolves by match.
	TArray<TFunction<void(bool, bool)>> PendingAmIHostCallbacks;

	struct FPendingActorOwnerCallback
	{
		FString Uuid;
		TFunction<void(bool, int64)> Callback;
	};
	TArray<FPendingActorOwnerCallback> PendingActorOwnerCallbacks;

	std::atomic<int64> HostUserID { 0 };
	
	/** Host identity itself lives on the game session (single store, readable
	 *  from lower modules); this subsystem only elects and broadcasts. */
	UPROPERTY()
	TObjectPtr<UCrowdyGameSession> GameSession;

	UPROPERTY()
	FGuid LocalPlayerID;
	
	bool bIsReady = false;
	
private:
	
	UFUNCTION()
	void OnOwnerPlayerIDSet(FString OwnerID);

	bool LoadConfig() const;
};
