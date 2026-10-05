#pragma once

#include "CoreMinimal.h"
#include "CrowdyK2Node_ServerObjectBase.h"
#include "CrowdyK2Node_GetServerVariable.generated.h"

/** "Get <Variable>": reads one variable of a Server Object in place, with a pin of the variable's own type. */
UCLASS()
class UCrowdyK2Node_GetServerVariable : public UCrowdyK2Node_ServerObjectBase
{
	GENERATED_BODY()

public:
	static FText MakeTitle(FName Variable, const FText& Asset);

	//~ UEdGraphNode
	virtual void AllocateDefaultPins() override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;
	//~ End UEdGraphNode

	//~ UK2Node
	virtual bool IsNodePure() const override { return true; }
	virtual void ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph) override;
	//~ End UK2Node
};
