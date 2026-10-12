#pragma once

#include "CoreMinimal.h"
#include "K2Node.h"
#include "CrowdyK2Node_ReceiveServerValuesChangedOnEntity.generated.h"

class FBlueprintActionDatabaseRegistrar;
class FKismetCompilerContext;
class UEdGraph;

/** "Receive Server Values Changed on Entity": runs part of a graph when the server values of a crowd entity of Entity Class change. */
UCLASS()
class UCrowdyK2Node_ReceiveServerValuesChangedOnEntity : public UK2Node
{
	GENERATED_BODY()

public:
	//~ UEdGraphNode
	virtual void AllocateDefaultPins() override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;
	virtual FSlateIcon GetIconAndTint(FLinearColor& OutColor) const override;
	virtual bool CanPasteHere(const UEdGraph* TargetGraph) const override;	//~ End UEdGraphNode

	//~ UK2Node
	virtual void GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const override;
	virtual FText GetMenuCategory() const override;
	virtual bool IsCompatibleWithGraph(const UEdGraph* TargetGraph) const override;
	virtual void EarlyValidation(class FCompilerResultsLog& MessageLog) const override;
	virtual void ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph) override;
	//~ End UK2Node

	UEdGraphPin* GetClassPin() const;
};
