#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "CrowdyOwnershipTransfer.generated.h"

/**
 * Static Blueprint entry points for explicit ownership transfer, so a request or grant can be issued from
 * anywhere without first getting the target actor's Crowdy Entity Component by hand. Ownership lives on the
 * client-authoritative view plane: it is a coordination convention, NOT an enforcement boundary — cheat-sensitive
 * ownership belongs in a Game Model. A transfer is also transient (a client that spawns the entity after a grant
 * sees the spawn-time owner).
 *
 * Flow: a client calls RequestOwnershipTransfer for an entity it does not own; the entity's current authority
 * (its owning client, or the host for a host-owned world entity) receives the request and either auto-grants
 * (the component's bAutoApproveOwnershipRequests) or surfaces UCrowdyEntitySubsystem::OnOwnershipRequested for
 * game code to approve (by calling a Grant node) or ignore (rejection by silence). A grant is announced reliably
 * to every client, surfacing as UCrowdyEntitySubsystem::OnEntityOwnershipChanged.
 *
 * Each node resolves the target actor's UCrowdyEntityComponent and its entity subsystem; every one is a safe
 * no-op (never a crash) when the target is null or is not a registered Crowdy entity.
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyOwnershipTransfer : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:

	/**
	 * Requests that ownership of TargetEntity be transferred to the local player. Sends a reliable request to the
	 * entity's current authority; that authority then grants or ignores it. No-op if TargetEntity is not a
	 * registered Crowdy entity, or the local player already owns it.
	 */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Ownership",
		meta = (WorldContext = "WorldContextObject", DisplayName = "Request Ownership Transfer"))
	static void RequestOwnershipTransfer(UObject* WorldContextObject, AActor* TargetEntity);

	/**
	 * Grants ownership of TargetEntity to the player who owns NewOwner (pass that player's avatar, or any entity
	 * they own). Only the entity's current authority may grant; call this from an OnOwnershipRequested handler to
	 * approve a request. No-op if NewOwner is not a client-owned entity — use GrantOwnershipToHost to make an
	 * entity host-owned.
	 */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Ownership",
		meta = (WorldContext = "WorldContextObject", DisplayName = "Grant Ownership Transfer"))
	static void GrantOwnershipTransfer(UObject* WorldContextObject, AActor* TargetEntity, AActor* NewOwner);

	/**
	 * Grants ownership of TargetEntity to a specific player by id. The approval flow hands you the requester's id
	 * directly (OnOwnershipRequested), so this avoids resolving it back to an actor that may not exist on this
	 * client. Only the entity's current authority may grant.
	 */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Ownership",
		meta = (WorldContext = "WorldContextObject", DisplayName = "Grant Ownership Transfer To Player"))
	static void GrantOwnershipTransferToPlayer(UObject* WorldContextObject, AActor* TargetEntity, FGuid NewOwnerPlayerID);

	/**
	 * Makes TargetEntity host-owned (a world entity owned by whichever client is host). Only the entity's current
	 * authority may do this.
	 */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Ownership",
		meta = (WorldContext = "WorldContextObject", DisplayName = "Grant Ownership To Host"))
	static void GrantOwnershipToHost(UObject* WorldContextObject, AActor* TargetEntity);
};
