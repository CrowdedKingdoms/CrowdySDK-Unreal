#pragma once

#include "CoreMinimal.h"
#include "CrowdyK2Node_ServerObjectBase.h"
#include "CrowdyK2Node_CallServerFunction.generated.h"

struct FCrowdyServerFunction;

/** "Call <Function>": calls a Server Function with a typed pin per input and output; the generic one picks it from a dropdown. */
UCLASS()
class UCrowdyK2Node_CallServerFunction : public UCrowdyK2Node_ServerObjectBase
{
	GENERATED_BODY()

public:
	/** Shows the function dropdown, as the generic Call Server Function does. */
	UPROPERTY()
	bool bPickFunction = false;

	static FText MakeTitle(FName Function, const FText& Asset);

	/** The function Member names, or null. */
	const FCrowdyServerFunction* GetFunction() const;

	/** The function's inputs and outputs as pins. */
	void GatherFunctionValues(TArray<FCrowdyServerValuePin>& OutInputs, TArray<FCrowdyServerValuePin>& OutOutputs) const;
	static void GatherFunctionValues(const FCrowdyServerFunction* Function, TArray<FCrowdyServerValuePin>& OutInputs, TArray<FCrowdyServerValuePin>& OutOutputs);

	/** Whether every input and output of InDefinition's Function has a pin. */
	static bool HasEveryPin(const UCrowdyServerObjectDefinition* InDefinition, FName Function);

	/** Picks Function of InDefinition from the dropdown on the next tick, since the dropdown's pin is rebuilt. */
	void SetPickedFunction(UCrowdyServerObjectDefinition* InDefinition, FName Function);

	/** Picks Function of InDefinition now, as one undoable step. */
	void PickFunction(UCrowdyServerObjectDefinition* InDefinition, FName Function);

	/** What the dropdown shows. */
	FText GetPickedFunctionText() const;

	//~ UEdGraphNode
	virtual void AllocateDefaultPins() override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;
	virtual bool IsCompatibleWithGraph(const UEdGraph* TargetGraph) const override;
	//~ End UEdGraphNode

	//~ UK2Node
	virtual void GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const override;
	virtual FText GetMenuCategory() const override;
	virtual bool IsLatentForMacros() const override { return true; }
	virtual FName GetCornerIcon() const override;
	virtual void ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph) override;
	//~ End UK2Node

private:
	void PrefillInputDefaults(TConstArrayView<FCrowdyServerValuePin> Inputs);
	UEdGraphPin* ChainInputError(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, UEdGraphPin* Errors, UEdGraphPin* Set, FName Input);
	UEdGraphPin* ExpandInputs(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, TConstArrayView<FCrowdyServerValuePin> Inputs, UEdGraphPin* InputsPin,
		UEdGraphPin* InputErrorPin);
	UEdGraphPin* ExpandOutputs(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, TConstArrayView<FCrowdyServerValuePin> Outputs, UEdGraphPin* SuccessPin, UEdGraphPin* OutputsPin);
};
