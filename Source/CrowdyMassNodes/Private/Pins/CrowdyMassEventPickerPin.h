#pragma once

#include "CoreMinimal.h"
#include "EdGraphUtilities.h"
#include "SGraphPin.h"

class UClass;
class UEdGraphPin;

/**
 * The Event dropdown, shared by "Call Crowdy Event on Entity" and "Receive Crowdy Event on Entity".
 *
 * One widget, holding every Crowdy event of every entity class the Mass rendering backend is configured
 * for, grouped under the class it belongs to. Picking an event picks its class with it, which is why
 * neither node needs a class pin. The class headers appear only once there is more than one group, so a
 * list with nothing to tell apart is flat.
 *
 * The two nodes get nearly the same list and deliberately not the same one. The call node additionally
 * offers the events the SDK declares on the stand-in base every crowd entity is given, which can be sent
 * to an entity of any class; the receive node does not, because the crowd plane runs those events itself
 * and there is nothing for a graph to be handed. So the call node's list always has the SDK group and its
 * headers appear as soon as a project contributes one class of its own, while the receive node's list is
 * flat with one configured class and empty with none.
 *
 * The list is rebuilt each time the dropdown opens, so a class bound or an event added since the last
 * time appears without reopening the graph. The choice is written onto the node as a member reference,
 * never into the pin, so the pin's text is a label rather than the record of what was picked.
 */
class SCrowdyMassEventPickerPin : public SGraphPin
{
public:
	SLATE_BEGIN_ARGS(SCrowdyMassEventPickerPin) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, UEdGraphPin* InPin);

protected:
	//~ SGraphPin
	virtual TSharedRef<SWidget> GetDefaultValueWidget() override;
	//~ End SGraphPin

private:
	TSharedRef<SWidget> BuildPickerMenu();
	FText GetCurrentValueText() const;
	void OnEventPicked(UClass* DeclaringClass, FName FunctionName);
	void OnEventCleared();
};

/**
 * Supplies SCrowdyMassEventPickerPin for the Event pin of either Crowdy Mass event node and defers
 * (returns null) for every other pin, so the default factory draws the rest. Registered by the module.
 */
class FCrowdyMassEventPickerPinFactory : public FGraphPanelPinFactory
{
public:
	virtual TSharedPtr<SGraphPin> CreatePin(UEdGraphPin* InPin) const override;
};
