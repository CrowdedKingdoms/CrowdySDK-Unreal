#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Messages/Actor/FCrowdyKitActorState.h"
#include "CrowdyKitActorComponent.generated.h"

class UCrowdyKitActorComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnCrowdyKitActorChanged, UCrowdyKitActorComponent*, Component, const FCrowdyKitActorState&, KitActor);

/**
 * What a Game Kit starter's actor drawn by this actor is: its kind, its id in its hub (for that hub's calls, such as Attack), its name,
 * health, mode and owner. The Crowdy SDK fills it when it spawns the actor for a kind in a starter's Kit Actors, and adds one when the
 * class has none. View state: the actor's truth is in its hub.
 */
UCLASS(ClassGroup = (Crowdy), meta = (BlueprintSpawnableComponent, DisplayName = "Crowdy Kit Actor"))
class CROWDYREPLICATION_API UCrowdyKitActorComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UCrowdyKitActorComponent();

	/** The actor as its hub last showed it; Location and Rotation are that update's, while the actor itself moves smoothly between them. */
	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Kit")
	FCrowdyKitActorState KitActor;

	/** Runs when this actor starts drawing a kit actor and whenever anything but where it is changes, such as its health or mode. */
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Kit")
	FOnCrowdyKitActorChanged OnKitActorChanged;

	/** The key the SDK knows this kit actor by. */
	const FGuid& GetKitKey() const { return KitKey; }

	// Called by the SDK: draws Key's actor with State, telling listeners, and answering true, when it starts drawing it, it is another actor or its facts changed.
	bool ApplyKitActor(const FGuid& Key, const FCrowdyKitActorState& State, bool bStartsDrawing = false);

private:
	FGuid KitKey;
};
