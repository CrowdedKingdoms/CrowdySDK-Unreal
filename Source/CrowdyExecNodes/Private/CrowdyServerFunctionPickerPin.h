#pragma once

#include "CoreMinimal.h"
#include "EdGraphUtilities.h"
#include "SGraphPin.h"

class UCrowdyServerObjectDefinition;

/** The Function dropdown of Call Server Function: every definition's functions, grouped by definition asset. */
class SCrowdyServerFunctionPickerPin : public SGraphPin
{
public:
	SLATE_BEGIN_ARGS(SCrowdyServerFunctionPickerPin) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, UEdGraphPin* InPin);

protected:
	//~ SGraphPin
	virtual TSharedRef<SWidget> GetDefaultValueWidget() override;
	//~ End SGraphPin

private:
	TSharedRef<SWidget> BuildMenu();
	FText GetCurrentText() const;
	void OnPicked(TWeakObjectPtr<UCrowdyServerObjectDefinition> Definition, FName Function);
};

/** Draws SCrowdyServerFunctionPickerPin for Call Server Function's Function pin, and defers for every other pin. */
class FCrowdyServerFunctionPickerPinFactory : public FGraphPanelPinFactory
{
public:
	virtual TSharedPtr<SGraphPin> CreatePin(UEdGraphPin* InPin) const override;
};
