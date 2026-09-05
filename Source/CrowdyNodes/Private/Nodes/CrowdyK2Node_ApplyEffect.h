// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "K2Node_BaseAsyncTask.h"
#include "CrowdyK2Node_ApplyEffect.generated.h"

class UCrowdyEffect;
class FBlueprintActionDatabaseRegistrar;
class FKismetCompilerContext;
class UEdGraph;

/**
 * The designer-facing "Apply Crowdy Effect" node. It wraps the UCrowdyApplyEffectAction latent action but replaces
 * the raw Overrides map, Level, and Source pins with a typed pin per tuning magnitude the referenced effect
 * declares: a designer sets base_power on an int pin, not by hand-typing a JSON map. The effect must be a literal
 * on the Effect pin; a connected (variable) Effect can't be read at author time, so the node degrades to the plain
 * async pin set and a compile warning.
 *
 * At compile time ExpandNode assembles the Overrides map from the connected / changed magnitude pins (each encoded
 * with UCrowdyEffects::JsonFrom* or, for a container_ref magnitude, GetContainerIdFor) and feeds it into the
 * wrapped factory call; an untouched pin is omitted so the effect's server default (or a sampled curve) stays
 * authoritative. Level appears only for a curve-driven effect and Source only when the effect reads source.<attr>.
 *
 * When the effect declares a return type, the node also grows a single typed Return Value output pin beside the raw
 * Return Value Json pin, which stays visible: the typed pin is a decode of the raw one, spliced in at compile time,
 * not a replacement for it. An absent, malformed, or wrong-typed result decodes to the type's zero, so a server that
 * answered nothing and a server that answered zero read alike on the typed pin; the raw pin is what tells them
 * apart. The pin follows the declared type alone. An effect that declares a type but whose body authors no return
 * still gets the pin, and a compile warning saying the pin will only ever read its type's default.
 */
UCLASS()
class UCrowdyK2Node_ApplyEffect : public UK2Node_BaseAsyncTask
{
	GENERATED_BODY()

public:
	UCrowdyK2Node_ApplyEffect(const FObjectInitializer& ObjectInitializer);

	//~ UEdGraphNode
	virtual void AllocateDefaultPins() override;
	virtual void PinDefaultValueChanged(UEdGraphPin* Pin) override;
	virtual void PinConnectionListChanged(UEdGraphPin* Pin) override;
	virtual void PostPlacedNewNode() override;
	virtual void PostLoad() override;
	virtual void BeginDestroy() override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;
	virtual void ValidateNodeDuringCompilation(class FCompilerResultsLog& MessageLog) const override;
	//~ End UEdGraphNode

	//~ UK2Node
	virtual void GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const override;
	virtual FText GetMenuCategory() const override;
	virtual void ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph) override;
	virtual void ReallocatePinsDuringReconstruction(TArray<UEdGraphPin*>& OldPins) override;
	//~ End UK2Node

private:
	// The effect referenced as a LITERAL on the Effect pin; null when the pin is connected or empty (degraded mode).
	// During a reconstruction pass (see ReallocatePinsDuringReconstruction) this returns the pending value read off
	// the outgoing pin instead, since the freshly-allocated Effect pin has no value yet at that point.
	UCrowdyEffect* GetLiteralEffect() const;

	// Hide the Overrides pin, drop the Level / Source pins the effect doesn't need, and add one typed pin per
	// magnitude. A no-op (plain async pins) when the Effect pin has no literal.
	void SynchronizeEffectPins();

	// Queue a ReconstructNode for the next editor tick. Used from PinDefaultValueChanged, where reconstructing
	// synchronously would free a pin the asset-picker caller is still using.
	void ScheduleReconstruct();

	// Subscribe to asset property changes so editing the referenced effect's magnitudes reconstructs the node.
	void RegisterEffectAssetListener();
	void HandleObjectPropertyChanged(UObject* Object, struct FPropertyChangedEvent& PropertyChangedEvent);

	FDelegateHandle OnPropertyChangedHandle;

	// Set only while a reconstruction pass is in flight (see ReallocatePinsDuringReconstruction); GetLiteralEffect
	// prefers this over the live Effect pin, which is briefly empty during that window. Never serialized.
	UCrowdyEffect* PendingReconstructionEffect = nullptr;

	// True between scheduling a deferred reconstruct and it firing, so a burst of default-value changes coalesces
	// into a single rebuild. Transient editor state, never serialized.
	bool bReconstructPending = false;
};
