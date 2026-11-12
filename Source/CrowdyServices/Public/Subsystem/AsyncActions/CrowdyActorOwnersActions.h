#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Subsystem/CrowdyActorOwners.h"
#include "CrowdyActorOwnersActions.generated.h"

// One signature serves every pin, because a Blueprint async node builds its data pins from the first delegate only.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FActorOwnerAsyncResult, const FCrowdyActorOwner&, Owner);

UCLASS()
class CROWDYSERVICES_API UCrowdyActorOwners_ResolveActorOwner : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FActorOwnerAsyncResult OnConfirmed;

	/** No record ties the id to a user. Name no one for it. */
	UPROPERTY(BlueprintAssignable)
	FActorOwnerAsyncResult OnUnknown;

	UPROPERTY(BlueprintAssignable)
	FActorOwnerAsyncResult OnFailed;

	/**
	 * Asks the platform which user a remote actor id is registered to. A realtime sender can still use an id it does not
	 * own. Pins fire on a later tick, never inside this node, so asking again from a pin does not recurse.
	 */
	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Players", DisplayName="Resolve Actor Owner")
	static UCrowdyActorOwners_ResolveActorOwner* ResolveActorOwner(UObject* WorldContextObject, FGuid ActorId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FGuid ActorId;

	UFUNCTION() void HandleResolved(const FCrowdyActorOwner& Owner);
	void Deliver(const FCrowdyActorOwner& Owner);
};
