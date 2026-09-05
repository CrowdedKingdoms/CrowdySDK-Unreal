// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Replication/GameModel/CrowdyGameModelSubsystem.h" // FCrowdyModelAttributeChanged (the shared change delegate)
#include "CrowdyModelChangeActions.generated.h"

class UWorld;

namespace CrowdyModelListen
{
	// Pure filter decision for a model-change listener, extracted so it is headless-testable without a world.
	// bHasTargetFilter records whether the node was created with a Target, so a Target that was later destroyed
	// (TargetFilter now null) matches NOTHING rather than silently widening to every change. Precedence: an
	// explicit Target wins, then a non-empty ModelId, else the listener is global (every change passes).
	CROWDYREPLICATION_API bool PassesFilter(bool bHasTargetFilter, const UObject* TargetFilter,
		const FString& ModelIdFilter, const UObject* InTarget, const FString& InModelId);
}

/**
 * Persistent listener for Game Model attribute changes: fires OnChanged every time a matching container's
 * attribute changes. The zero-setup way to react to server-truth changes from anywhere - a HUD, a scoreboard, an
 * inventory panel - without wiring a per-attribute CrowdyOnRep on the affected object.
 *
 * Lifetime: it never auto-completes on a change; it reaps itself when its bound world tears down (so it stops with
 * the map rather than lingering rooted on the GameInstance) or, for a Target-filtered listener, when the watched
 * Target is destroyed. After level travel a Blueprint re-creates the listener on the new map.
 *
 * Filtering: pass Target to watch one actor/object, or ModelId to watch one container by id, or leave both empty
 * to observe every model change (a scoreboard). Target takes precedence over ModelId. OnChanged carries the
 * changed Target, ModelId, Attribute (the server key), and the old/new canonical JSON values.
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyListenForModelChangesAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Attributes", meta = (DisplayName = "On Game Model Changed"))
	FCrowdyModelAttributeChanged OnChanged;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Attributes", DisplayName = "Listen for Model Changes")
	static UCrowdyListenForModelChangesAction* ListenForModelChanges(UObject* WorldContext, UObject* Target,
		const FString& ModelId);

	virtual void Activate() override;
	virtual void BeginDestroy() override;

private:
	// Bound to the subsystem's OnModelAttributeChanged; applies the filter and re-broadcasts OnChanged. Must be a
	// UFUNCTION to bind a dynamic multicast delegate.
	UFUNCTION()
	void HandleModelAttributeChanged(UObject* InTarget, const FString& InModelId, FName Attribute,
		const FString& OldValueJson, const FString& NewValueJson);

	// Reap when the bound world tears down (level travel / PIE stop) so the node does not outlive its change source
	// as an inert, rooted object.
	void HandleWorldBeginTearDown(UWorld* World);

	// Unbind everything and mark ready to destroy, once. Called on world teardown or when a Target-filtered watch
	// loses its Target.
	void Shutdown();

	TWeakObjectPtr<UObject> WorldContextObject;
	TWeakObjectPtr<UObject> TargetFilter;
	FString ModelIdFilter;
	bool bHasTargetFilter = false;
	bool bShutDown = false;

	// The subsystem we bound to, so we can unbind if it outlives us. Weak: it is a world subsystem and may be torn
	// down first, in which case the dynamic binding is already gone.
	TWeakObjectPtr<UCrowdyGameModelSubsystem> BoundModel;
	// The world whose teardown reaps this node.
	TWeakObjectPtr<UWorld> BoundWorld;
	FDelegateHandle WorldTearDownHandle;
};
