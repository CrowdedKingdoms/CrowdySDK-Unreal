#include "Subsystem/AsyncActions/CrowdyHostQueryActions.h"
#include "CrowdyServicesLog.h"
#include "Subsystem/CrowdyHostSubsystem.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

static UCrowdyHostSubsystem* GetHostSubsystem(const TWeakObjectPtr<UObject>& Ctx)
{
	if (!Ctx.IsValid()) return nullptr;
	const UWorld* World = Ctx->GetWorld();
	return World ? World->GetSubsystem<UCrowdyHostSubsystem>() : nullptr;
}

UCrowdyIsEntityHostServer* UCrowdyIsEntityHostServer::IsCrowdyEntityHostServer(UObject* WorldContextObject, AActor* Entity)
{
	UCrowdyIsEntityHostServer* Action = NewObject<UCrowdyIsEntityHostServer>();
	Action->WorldContextObject = WorldContextObject;
	Action->Entity = Entity;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyIsEntityHostServer::Activate()
{
	UCrowdyHostSubsystem* HostSubsystem = GetHostSubsystem(WorldContextObject);
	if (!IsValid(HostSubsystem))
	{
		UE_LOG(LogCrowdyServices, Warning, TEXT("[IsCrowdyEntityHostServer]: HostSubsystem not found."));
		Finish(false, false);
		return;
	}

	TWeakObjectPtr<UCrowdyIsEntityHostServer> WeakThis(this);
	HostSubsystem->CheckEntityIsHost(Entity.Get(), [WeakThis](bool bSuccess, bool bIsHost)
	{
		if (UCrowdyIsEntityHostServer* Self = WeakThis.Get())
			Self->Finish(bSuccess, bIsHost);
	});
}

void UCrowdyIsEntityHostServer::Finish(bool bSuccess, bool bIsHost)
{
	if (!bSuccess)
		OnFailed.Broadcast();
	else if (bIsHost)
		OnIsHost.Broadcast();
	else
		OnIsNotHost.Broadcast();

	SetReadyToDestroy();
}
