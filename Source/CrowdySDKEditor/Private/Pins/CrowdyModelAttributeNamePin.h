#pragma once

#include "CoreMinimal.h"
#include "SGraphPin.h"

class UClass;
class UEdGraphPin;
class SEditableTextBox;

/**
 * Custom default-value widget for the "Key" pin of UCrowdyModel::GetInt/GetFloat/GetBool/GetString.
 * Pairs a free-text box with a dropdown of the target container class's discovered Server Owned attribute keys,
 * filtered to the getter's value type (Get Integer lists only "int" keys, and so on). The class is resolved from
 * the call node's Entity pin: its connected reference's class, or the owning Blueprint's own class when Entity is
 * left unconnected. Free text stays allowed, so a key the picker doesn't list can still be typed. Editor-only; no
 * cooked-build footprint.
 */
class SCrowdyModelAttributeNamePin : public SGraphPin
{
public:
	SLATE_BEGIN_ARGS(SCrowdyModelAttributeNamePin) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, UEdGraphPin* InPin);

	// The Game Model value type a UCrowdyModel getter reads: GetInt -> "int", GetFloat -> "float",
	// GetBool -> "bool", GetString -> "string". Empty for any other function name.
	static FString ValueTypeForGetter(FName GetterFunctionName);

	// The sorted server-attribute keys on Class whose value type equals WantedValueType. Empty when Class is null,
	// WantedValueType is empty, or nothing matches. Pure (no Slate, no graph): the testable core of the picker.
	static TArray<FName> ComputeMatchingKeys(const UClass* Class, const FString& WantedValueType);

protected:
	//~ SGraphPin
	virtual TSharedRef<SWidget> GetDefaultValueWidget() override;

private:
	// The container class whose attribute keys populate the dropdown, from the Entity pin (connected reference's
	// class, else the owning Blueprint's own class when Entity is unconnected). Null when unresolvable.
	UClass* ResolveTargetClass() const;

	// The value type this pin's getter reads, resolved from the owning call node's target function. Empty when the
	// node is not one of the four typed getters.
	FString ResolveWantedValueType() const;

	TSharedRef<SWidget> BuildPickerMenu();
	FText GetCurrentValueText() const;
	void OnKeySelected(FName Key);
	void OnKeyTextCommitted(const FText& NewText, ETextCommit::Type CommitType);
	void SetPinValue(FName Key);
};
