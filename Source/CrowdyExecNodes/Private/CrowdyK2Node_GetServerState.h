#pragma once

#include "CoreMinimal.h"
#include "CrowdyK2Node_ServerObjectBase.h"
#include "CrowdyK2Node_GetServerState.generated.h"

/** "Get Server State": every variable players can see, each on a pin of its own type, read in place. */
UCLASS()
class UCrowdyK2Node_GetServerState : public UCrowdyK2Node_ServerObjectBase
{
	GENERATED_BODY()

public:
	static FText MakeTitle(const FText& Asset);

	//~ UEdGraphNode
	virtual void AllocateDefaultPins() override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;
	//~ End UEdGraphNode

	//~ UK2Node
	virtual bool IsNodePure() const override { return true; }
	virtual void ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph) override;
	//~ End UK2Node

private:
	/** The variables in Visible to Players; players never receive the others. */
	void GatherVisibleVariables(TArray<FCrowdyServerValuePin>& Out) const;
};
