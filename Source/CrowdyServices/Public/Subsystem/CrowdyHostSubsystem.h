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
	
	
	UE_DEPRECATED(5.8, "Bind OnLeaderChanged on UCrowdyLeaderSubsystem, which never fires under Leader Source Elected Host; logic only one machine should run belongs in a Server Object.")
	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Host|Events",
		meta=(DeprecatedProperty, DeprecationMessage="Bind On Leader Changed on the Crowdy Leader Subsystem, which never fires under Leader Source Elected Host; logic only one machine should run belongs in a Server Object."))
	FOnCrowdyHostElected OnHostElected;

	UE_DEPRECATED(5.8, "Use GetLeaderUserID on UCrowdyLeaderSubsystem, which is 0 under Leader Source Elected Host; logic only one machine should run belongs in a Server Object.")
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy SDK|Host",
		meta=(DeprecatedFunction, DeprecationMessage="Use Get Leader User ID on the Crowdy Leader Subsystem, which is 0 under Leader Source Elected Host; logic only one machine should run belongs in a Server Object."))
	FGuid GetHostID() const;

	UE_DEPRECATED(5.8, "Use GetLeaderUserID on UCrowdyLeaderSubsystem; 0 means there is no Leader, which is always so under Leader Source Elected Host.")
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy SDK|Host",
		meta=(DeprecatedFunction, DeprecationMessage="Use Get Leader User ID on the Crowdy Leader Subsystem; 0 means there is no Leader, which is always so under Leader Source Elected Host."))
	bool IsHostSet() const {return IsValid(GameSession) && GameSession->GetAppWriterID().IsValid();}

	/** Called by the SDK on the game thread when a GameHost response arrives, and with 0 on sign-out.
	 *  Broadcasts OnHostElected on the game thread whenever the host changes. */
	void SetHostUserID(const int64 InHostUserID);

	/** The host id for a user id; user id 0 means no host, which is the empty id rather than a hash of zero. */
	static FGuid HostIDForUser(const int64 UserID);

	/** Returns the user ID of the current game host (0 if not yet known). */
	UE_DEPRECATED(5.8, "Use GetLeaderUserID on UCrowdyLeaderSubsystem, which is 0 under Leader Source Elected Host; logic only one machine should run belongs in a Server Object.")
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Host",
		meta=(DisplayName="Get Host User ID", DeprecatedFunction,
		      DeprecationMessage="Use Get Leader User ID on the Crowdy Leader Subsystem, which is 0 under Leader Source Elected Host; logic only one machine should run belongs in a Server Object."))
	int64 GetHostUserID() const
	{
		return HostUserID.load(std::memory_order_relaxed);
	}
	
	/**
	 * Returns true when the local user IS the current game host.
	 * Safe to call from any thread and from Blueprint.
	 */
	UE_DEPRECATED(5.8, "Use IsLocalLeader on UCrowdyLeaderSubsystem, which is false under Leader Source Elected Host; logic only one machine should run belongs in a Server Object.")
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Host",
		meta=(DisplayName="Is Host", DeprecatedFunction,
		      DeprecationMessage="Use Is Local Leader on the Crowdy Leader Subsystem, which is false under Leader Source Elected Host; logic only one machine should run belongs in a Server Object."))
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
	 *  - For the LOCAL player's own entity we ask amIGameHost: the authoritative,
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
	UE_DEPRECATED(5.8, "Use IsLocalLeader on UCrowdyLeaderSubsystem for a one-machine gate, which is false under Leader Source Elected Host. IsCrowdyEntityLocallyOwned is a different question: it is true on every client for its own avatar. A decision that must be enforced belongs in a Server Object.")
	void CheckEntityIsHost(const AActor* Entity, TFunction<void(bool bSuccess, bool bIsHost)> Callback);

#if WITH_DEV_AUTOMATION_TESTS
	void SetGameSessionForTest(UCrowdyGameSession* InGameSession) { GameSession = InGameSession; }
#endif

private:

	/**
	 * Ask the server the two questions CheckEntityIsHost is built from. Each call owns its own completion, so
	 * concurrent checks cannot answer each other, and every completion runs: this subsystem is world-scoped while
	 * the Blueprint node waiting on it is not, so a completion that is dropped when the world ends leaves that
	 * node's pins unfired.
	 */
	void RequestAmIGameHost(TFunction<void(bool bSuccess, bool bAmHost)> Callback);
	void RequestActorOwner(const FString& Uuid, TFunction<void(bool bSuccess, int64 UserId)> Callback);

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
