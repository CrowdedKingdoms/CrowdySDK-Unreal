#include "Replication/GameModel/CrowdyContainerManifestActions.h"

#include "Data/CrowdyContainerManifest.h"
#include "Replication/GameModel/CrowdyGameModelActionSupport.h"

UCrowdyApplyContainerManifestAction* UCrowdyApplyContainerManifestAction::ApplyContainerManifest(UObject* WorldContext,
	const UCrowdyContainerManifest* InManifest, const FString& InSessionId, bool bInAppScope)
{
	UCrowdyApplyContainerManifestAction* Action = NewObject<UCrowdyApplyContainerManifestAction>();
	Action->WorldContextObject = WorldContext;
	Action->Manifest = InManifest;
	Action->SessionId = InSessionId;
	Action->bAppScope = bInAppScope;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyApplyContainerManifestAction::Activate()
{
	UCrowdyGameModelSubsystem* Model = GetGameModelSubsystem(WorldContextObject);
	if (!Model)
	{
		FCrowdyApplyManifestResult Result;
		Result.Failed = 1;
		FCrowdyManifestRowFailure& Failure = Result.Failures.AddDefaulted_GetRef();
		Failure.Code = TEXT("no_subsystem");
		Failure.Message = TEXT("No Game Model subsystem in this world (not a play world).");
		Failed.Broadcast(Result);
		SetReadyToDestroy();
		return;
	}

	TWeakObjectPtr<UCrowdyApplyContainerManifestAction> WeakThis(this);
	Model->ApplyContainerManifest(Manifest.Get(), SessionId, bAppScope, [WeakThis](const FCrowdyApplyManifestResult& Result)
	{
		UCrowdyApplyContainerManifestAction* Action = WeakThis.Get();
		if (!Action)
		{
			return;
		}
		(Result.IsComplete() ? Action->Succeeded : Action->Failed).Broadcast(Result);
		Action->SetReadyToDestroy();
	});
}
