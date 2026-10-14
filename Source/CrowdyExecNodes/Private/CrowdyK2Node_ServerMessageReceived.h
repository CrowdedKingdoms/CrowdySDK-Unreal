#pragma once

#include "CoreMinimal.h"
#include "CrowdyK2Node_ServerObjectBase.h"
#include "CrowdyK2Node_ServerMessageReceived.generated.h"

struct FCrowdyServerMessage;

/** "On <Message> Received": after Bind, Received runs with the payload's values each time the server sends the message. */
UCLASS()
class UCrowdyK2Node_ServerMessageReceived : public UCrowdyK2Node_ServerObjectBase
{
	GENERATED_BODY()

public:
	static FText MakeTitle(FName Message, const FText& Asset);

	/** The message Member names, or null. */
	const FCrowdyServerMessage* GetServerMessage() const;

	/** The values of the message's payload as pins; none for a message without one. */
	void GatherPayloadValues(TArray<FCrowdyServerValuePin>& OutValues) const;

	/** Whether every value of InDefinition's Message payload has a pin. */
	static bool HasEveryPin(const UCrowdyServerObjectDefinition* InDefinition, FName Message);

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

private:
	/** The payload's values into Out, or false after reporting a missing definition, a missing message or a refusal. */
	bool CheckMessage(FKismetCompilerContext& CompilerContext, TArray<FCrowdyServerValuePin>& Out) const;
};
