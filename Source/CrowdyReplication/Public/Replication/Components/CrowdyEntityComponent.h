#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Core/UDP/Enums/ECrowdyTarget.h"
#include "Data/CrowdyEntityTypes.h"
#include "Replication/Executor/ActorUpdateExecutor.h"
#include "Replication/Interfaces/CrowdyReplicationSource.h"
#include "StructUtils/InstancedStruct.h"
#include "CrowdyEntityComponent.generated.h"

class UCrowdyAutoReplicator;
class UCrowdyEntitySubsystem;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnCrowdyEntitySpawned, const FInstancedStruct&, InitialState, bool, bIsLocallyOwned);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnCrowdyEntityDestroyed, bool, bIsLocallyOwned);

UENUM(BlueprintType)
enum class ECrowdyEntityMode : uint8
{
	// Continuous state channel — the AutoReplicator sends the StateExecutor's
	// snapshot every replication interval (movement, anything high-frequency).
	Dynamic UMETA(DisplayName="Dynamic"),
	// Event-only — state travels through SendEvent and CrowdyEvent handlers.
	Static UMETA(DisplayName="Static"),
};

UENUM(BlueprintType)
enum class ECrowdyIdentityPolicy : uint8
{
	// Deterministic from the logged-in UserID. Only valid on the locally
	// controlled player pawn; anything else falls back to Random with a warning.
	PlayerDerived UMETA(DisplayName="Player Derived"),
	// Hashed from the owner's level path — every client computes the same NetID
	// for the same level-placed actor without hand-typed seeds.
	Stable UMETA(DisplayName="Stable"),
	Random UMETA(DisplayName="Random"),
};

/**
 * The single replication component for everything the SDK tracks as an entity.
 * Registers the owner in UCrowdyEntitySubsystem, optionally feeds the continuous
 * state channel (Dynamic mode), and receives the entity lifecycle callbacks.
 */
UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent, DisplayName="Crowdy Entity Component"))
class CROWDYREPLICATION_API UCrowdyEntityComponent : public UActorComponent, public ICrowdyReplicationSource
{
	GENERATED_BODY()

	// Configures Mode when adding the component dynamically to remote spawns
	friend class UCrowdyEntitySubsystem;

public:

	UCrowdyEntityComponent();

	/**
	 * Injects identity before BeginPlay so the component skips ID minting.
	 * Used by UCrowdyEntitySubsystem for deferred spawn, the original class is
	 * spawned on every client, with role/ownership decided by the spawn event.
	 */
	void InitIdentity(const FGuid& InNetID, const FGuid& InOwnerID, ECrowdyRole InRole, uint32 InClassID);

	/**
	 * Assigns identity to an actor that has already begun play — for pooled proxies.
	 * The actor pool pre-warms and reuses actors, so InitIdentity's pre-BeginPlay
	 * injection can't be used. Only syncs the component's own fields; the rendering
	 * backend owns the EntitySubsystem record for pooled proxies. Pair with
	 * ClearIdentity() when the actor returns to the pool.
	 */
	void AssignPooledIdentity(const FGuid& InNetID, const FGuid& InOwnerID, ECrowdyRole InRole, uint32 InClassID);

	/**
	 * Clears identity and unregisters this component's record, returning a pooled actor
	 * to an inert, unassigned state. Also removes the throwaway Owner record a pre-warmed
	 * pool actor mints in BeginPlay before it is assigned a real proxy id.
	 */
	void ClearIdentity();

	// Continuous channel (Dynamic mode)

	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Entity Component")
	void StartReplication();

	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Entity Component")
	void StopReplication();

	// Event channel

	/**
	 * Sends a game event addressed at this entity. Everyone: the entity's
	 * handlers run on every client; OwnerOnly: only on the owning client.
	 * State changes are plain events now — handle them with meta=(CrowdyEvent)
	 * functions on the owner.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Entity Component")
	void SendEvent(const FInstancedStruct& Payload, ECrowdyEventScope Scope = ECrowdyEventScope::Everyone);

	/** Broadcasts an FCrowdyEntityDestroyEvent, then destroys the owner (after DestroyDelay). */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Entity Component")
	void DestroyEntity();

	// Lifecycle events

	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Entity Component")
	FOnCrowdyEntitySpawned OnCrowdySpawned;

	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Entity Component")
	FOnCrowdyEntityDestroyed OnCrowdyDestroyed;

	/** Seconds between OnCrowdyDestroyed firing and the actor being destroyed (dissolve effects etc.); 0 = immediate. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Entity Component")
	float DestroyDelay = 0.f;

	// Accessors
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy SDK|Entity Component")
	FGuid GetNetID() const { return NetID; }

	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy SDK|Entity Component")
	FGuid GetOwnerID() const { return OwnerID; }

	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy SDK|Entity Component")
	ECrowdyRole GetRole() const { return Role; }

	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy SDK|Entity Component")
	bool IsLocallyOwned() const { return Role == ECrowdyRole::Owner; }

	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy SDK|Entity Component")
	ECrowdyEntityMode GetMode() const { return Mode; }

	uint32 GetClassID() const { return ClassID; }

	// ICrowdyReplicationSource
	virtual AActor* GetReplicatedActor() const override { return CachedOwner; }
	virtual const FString& GetReplicationUUID() const override { return UUIDString; }
	virtual const FInstancedStruct& GetReplicatedState() const override;

	// Component config — settable from an owning actor's constructor;
	// EditAnywhere keeps them tweakable on instances and Blueprint defaults.
	UPROPERTY(EditAnywhere, Category="Crowdy SDK|Entity Component")
	ECrowdyEntityMode Mode = ECrowdyEntityMode::Dynamic;

	UPROPERTY(EditAnywhere, Category="Crowdy SDK|Entity Component")
	ECrowdyIdentityPolicy IdentityPolicy = ECrowdyIdentityPolicy::Stable;

	// Dynamic mode only produces the snapshot the AutoReplicator sends
	UPROPERTY(EditAnywhere, Instanced, Category="Crowdy SDK|Entity Component",
		meta=(EditCondition="Mode == ECrowdyEntityMode::Dynamic", EditConditionHides))
	TObjectPtr<UActorUpdateExecutor> StateExecutor;

	// Dynamic mode: join the AutoReplicator on BeginPlay (StartReplication otherwise)
	UPROPERTY(EditAnywhere, Category="Crowdy SDK|Entity Component",
		meta=(EditCondition="Mode == ECrowdyEntityMode::Dynamic", EditConditionHides))
	bool bAutoRegister = true;

protected:

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:

	UPROPERTY()
	FGuid NetID;

	UPROPERTY()
	FGuid OwnerID;

	UPROPERTY()
	ECrowdyRole Role = ECrowdyRole::None;

	// Hash of the owner's class path, carried on spawn events
	UPROPERTY()
	uint32 ClassID = 0;

	// Derived from NetID once; exists only for the actor-update wire format
	UPROPERTY()
	FString UUIDString;

	UPROPERTY()
	mutable FInstancedStruct CachedState;

	UPROPERTY()
	TObjectPtr<AActor> CachedOwner;

	UPROPERTY()
	TObjectPtr<UCrowdyAutoReplicator> AutoReplicator;

	UPROPERTY()
	TObjectPtr<UCrowdyEntitySubsystem> EntitySubsystem;

	bool bIdentityInjected = false;

	void ResolveIdentity();
};
