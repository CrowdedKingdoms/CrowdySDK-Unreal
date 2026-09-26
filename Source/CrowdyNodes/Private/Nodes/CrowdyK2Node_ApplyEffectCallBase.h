// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "K2Node_CallFunction.h"
#include "CrowdyK2Node_ApplyEffectCallBase.generated.h"

class UCrowdyEffect;
class FBlueprintActionDatabaseRegistrar;
class FKismetCompilerContext;
class UEdGraph;

/**
 * Shared behaviour for the "Apply Crowdy Effect" nodes that lower to a plain library call rather than to a latent
 * action. A subclass names the function to call and the title to show; everything else, the reshaping of the node's
 * own pins around the referenced effect, lives here.
 *
 * The node replaces the raw Overrides map with one typed input pin per tuning magnitude the referenced effect
 * declares, so a designer sets base_power on an int pin instead of hand-typing a JSON map. The effect must be a
 * literal on the Effect pin; a connected (variable) effect cannot be read while the graph is being authored, so the
 * node falls back to the plain pin set and a compile warning. At compile time the magnitude pins that were wired or
 * changed are encoded and assembled into the Overrides argument; an untouched pin is left out so the effect's own
 * default (or a sampled curve) stays authoritative.
 *
 * A parameter the effect does not use is hidden and reset rather than removed: every parameter of the called
 * function must still have a pin when the call is emitted, and any pin that is not a parameter must be gone by then.
 */
UCLASS(Abstract)
class UCrowdyK2Node_ApplyEffectCallBase : public UK2Node_CallFunction
{
	GENERATED_BODY()

public:
	//~ UEdGraphNode
	virtual void AllocateDefaultPins() override;
	virtual void PostReconstructNode() override;
	virtual void PinDefaultValueChanged(UEdGraphPin* Pin) override;
	virtual void PinConnectionListChanged(UEdGraphPin* Pin) override;
	virtual void PostPlacedNewNode() override;
	virtual void PostLoad() override;
	virtual void BeginDestroy() override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual void ValidateNodeDuringCompilation(class FCompilerResultsLog& MessageLog) const override;
	//~ End UEdGraphNode

	//~ UK2Node
	virtual void GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const override;
	virtual FText GetMenuCategory() const override;
	virtual void ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph) override;
	virtual void ReallocatePinsDuringReconstruction(TArray<UEdGraphPin*>& OldPins) override;
	//~ End UK2Node

	// The library function this node places and calls. A subclass must override this; it is resolved by name so it
	// is valid on the class default object too, which is what the Blueprint action menu is built from. Returning
	// null leaves the node out of the menu rather than crashing, which is what the base class does.
	virtual UFunction* GetWrappedFunction() const { return nullptr; }

protected:
	// The node's title with no effect assigned. With one assigned, the effect's name is appended. A subclass must
	// override this to name the specific call it places.
	virtual FText GetBaseNodeTitle() const;

private:
	// The effect referenced as a LITERAL on the Effect pin; null when the pin is connected or empty, which is the
	// degraded mode where the node keeps its plain pin set. During a rebuild this returns the pending value read off
	// the outgoing pin instead, since the freshly allocated Effect pin has no value yet at that point.
	UCrowdyEffect* GetLiteralEffect() const;

	// Hide the pins the referenced effect does not use, then add one typed pin per magnitude.
	void SynchronizeEffectPins();

	// Hide and reset the input pins the referenced effect does not use. Run both when the pins are first created and
	// again after a rebuild, because a rebuild copies a value the designer set while a pin was still visible back
	// onto the now hidden pin, and that value would otherwise reach the call.
	void ApplyParameterPinVisibility();

	// Drop the synthesized magnitude pins. They are this node's own authoring surface and match no parameter of the
	// function being called, so the call cannot be emitted while they are still present.
	void RemoveMagnitudePins();

	// Queue a rebuild for the next editor tick. Used from PinDefaultValueChanged, where rebuilding straight away
	// would free a pin the asset picker is still using.
	void ScheduleReconstruct();

	// Subscribe to asset property changes so editing the referenced effect's magnitudes reshapes the node.
	void RegisterEffectAssetListener();
	void HandleObjectPropertyChanged(UObject* Object, struct FPropertyChangedEvent& PropertyChangedEvent);

	FDelegateHandle OnPropertyChangedHandle;

	// Set only while a rebuild is in flight (see ReallocatePinsDuringReconstruction); GetLiteralEffect prefers this
	// over the live Effect pin, which is briefly empty during that window. Never serialized.
	UCrowdyEffect* PendingReconstructionEffect = nullptr;

	// Magnitude values changed only in letter case, carried across a rebuild that would reset them. Never serialized.
	TMap<FName, FString> PendingCaseOnlyMagnitudeValues;

	// True between scheduling a deferred rebuild and it firing, so a burst of default-value changes coalesces into a
	// single rebuild. Transient editor state, never serialized.
	bool bReconstructPending = false;
};
