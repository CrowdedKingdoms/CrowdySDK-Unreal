#pragma once

#include "CoreMinimal.h"
#include "CrowdyK2Node_ServerObjectBase.h"
#include "CrowdyK2Node_ServerVariableChanged.generated.h"

/** "On <Variable> Changed": after Bind, Changed runs with the value once the object is ready and after every change. */
UCLASS()
class UCrowdyK2Node_ServerVariableChanged : public UCrowdyK2Node_ServerObjectBase
{
	GENERATED_BODY()

public:
	static FText MakeTitle(FName Variable, const FText& Asset);

	//~ UEdGraphNode
	virtual void AllocateDefaultPins() override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;
	virtual bool IsCompatibleWithGraph(const UEdGraph* TargetGraph) const override;
	//~ End UEdGraphNode

	//~ UK2Node
	virtual void EarlyValidation(FCompilerResultsLog& MessageLog) const override;
	virtual void ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph) override;
	//~ End UK2Node
};
