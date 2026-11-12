#include "Subsystem/AsyncActions/CrowdyActorOwnersActions.h"
#include "Subsystem/AsyncActions/CrowdyServicesActionSupport.h"
#include "Containers/Ticker.h"

UCrowdyActorOwners_ResolveActorOwner* UCrowdyActorOwners_ResolveActorOwner::ResolveActorOwner(UObject* WorldContextObject, FGuid ActorId)
{
	UCrowdyActorOwners_ResolveActorOwner* Action = NewObject<UCrowdyActorOwners_ResolveActorOwner>();
	Action->WorldContextObject = WorldContextObject;
	Action->ActorId = ActorId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyActorOwners_ResolveActorOwner::Activate()
{
	UCrowdyActorOwners* Owners = GetServiceSubsystem<UCrowdyActorOwners>(WorldContextObject);
	if (!Owners)
	{
		FCrowdyActorOwner Missing;
		Missing.Result = ECrowdyActorOwnerResult::Failed;
		Missing.Message = TEXT("UCrowdyActorOwners subsystem not found");
		HandleResolved(Missing);
		return;
	}
	FOnActorOwnerResolved Resolved;
	Resolved.BindDynamic(this, &UCrowdyActorOwners_ResolveActorOwner::HandleResolved);
	Owners->ResolveActorOwner(ActorId, Resolved);
}

void UCrowdyActorOwners_ResolveActorOwner::HandleResolved(const FCrowdyActorOwner& Owner)
{
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateWeakLambda(this, [this, Owner](float)
	{
		Deliver(Owner);
		return false;
	}));
}

void UCrowdyActorOwners_ResolveActorOwner::Deliver(const FCrowdyActorOwner& Owner)
{
	switch (Owner.Result)
	{
	case ECrowdyActorOwnerResult::Confirmed: OnConfirmed.Broadcast(Owner); break;
	case ECrowdyActorOwnerResult::Unknown: OnUnknown.Broadcast(Owner); break;
	default: OnFailed.Broadcast(Owner); break;
	}
	SetReadyToDestroy();
}
