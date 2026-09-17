#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Replication/GameModel/CrowdyContainerManifestApply.h"
#include "CrowdyContainerManifestActions.generated.h"

class UCrowdyContainerManifest;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FCrowdyApplyManifestOutcome, FCrowdyApplyManifestResult, Result);

/**
 * Creates the Game Model containers a map's manifest names, so its placed entities find their rows instead of
 * creating them one by one. Call it once per session from the client that created the session (or the host), with
 * the manifest the editor saved beside the map. Rows that already exist are left alone; the apply stops at the
 * first refusal, so Failed carries one failure and the count of rows never sent.
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyApplyContainerManifestAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Containers")
	FCrowdyApplyManifestOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Containers")
	FCrowdyApplyManifestOutcome Failed;

	/**
	 * Session Id empty: the active session, or the app itself when no session is active; App Scope makes an empty
	 * Session Id mean the app itself even while a session is active. Every row counts against the shared allowance.
	 */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext", AdvancedDisplay = "bAppScope"),
		Category = "Crowdy SDK|Game Model|Containers", DisplayName = "Apply Container Manifest")
	static UCrowdyApplyContainerManifestAction* ApplyContainerManifest(UObject* WorldContext,
		const UCrowdyContainerManifest* Manifest, const FString& SessionId = FString(), bool bAppScope = false);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	TWeakObjectPtr<const UCrowdyContainerManifest> Manifest;
	FString SessionId;
	bool bAppScope = false;
};
