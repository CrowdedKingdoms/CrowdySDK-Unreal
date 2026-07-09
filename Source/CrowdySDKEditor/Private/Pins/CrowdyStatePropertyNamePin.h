#pragma once

#include "CoreMinimal.h"
#include "EdGraphUtilities.h"
#include "SGraphPin.h"

class UClass;
class UEdGraphPin;

/**
 * Custom default-value widget for the "PropertyName" pin of UCrowdyStateBlueprintLibrary::MarkCrowdyStateDirty.
 * Replaces the plain text box with a dropdown of the target actor class's CrowdyState + CrowdyManualDirty
 * property names. The class is resolved from the call node's Target pin: its connected reference's class, or —
 * when Target is left unconnected (DefaultToSelf) — the Blueprint's own class. Options are recomputed each time
 * the dropdown opens, so they always reflect the current Target. Editor-only; no cooked-build footprint.
 */
class SCrowdyStatePropertyNamePin : public SGraphPin
{
public:
	SLATE_BEGIN_ARGS(SCrowdyStatePropertyNamePin) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, UEdGraphPin* InPin);

protected:
	//~ SGraphPin
	virtual TSharedRef<SWidget> GetDefaultValueWidget() override;

private:
	// The actor class whose CrowdyManualDirty properties populate the dropdown, from the Target pin (connected
	// class, else the owning Blueprint's own class for the DefaultToSelf case). Null when unresolvable.
	UClass* ResolveTargetClass() const;

	// Appends the CrowdyState + CrowdyManualDirty property names declared on (or inherited by) Class.
	static void GatherManualDirtyPropertyNames(const UClass* Class, TArray<FName>& Out);

	TSharedRef<SWidget> BuildPickerMenu();
	FText GetCurrentValueText() const;
	void OnPropertySelected(FName PropertyName);
};

/**
 * Supplies SCrowdyStatePropertyNamePin for the PropertyName pin of MarkCrowdyStateDirty and defers (returns
 * null) for every other pin, so the default pin factory renders the rest. Registered in the module's
 * StartupModule via FEdGraphUtilities::RegisterVisualPinFactory.
 */
class FCrowdyStatePropertyPinFactory : public FGraphPanelPinFactory
{
public:
	virtual TSharedPtr<SGraphPin> CreatePin(UEdGraphPin* InPin) const override;
};
