#include "CrowdyK2Node_ServerMessageReceived.h"

#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectLibrary.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Self.h"
#include "KismetCompiler.h"
#include "StructUtils/InstancedStruct.h"

#define LOCTEXT_NAMESPACE "CrowdyK2Node_ServerMessageReceived"

FText UCrowdyK2Node_ServerMessageReceived::MakeTitle(FName Message, const FText& Asset)
{
	return FText::Format(LOCTEXT("Title", "On {0} Received ({1})"), FText::FromName(Message), Asset);
}

const FCrowdyServerMessage* UCrowdyK2Node_ServerMessageReceived::GetServerMessage() const
{
	const UCrowdyServerObjectDefinition* Current = GetDefinition();
	return Current && !Member.IsNone() ? Current->FindMessage(Member) : nullptr;
}

void UCrowdyK2Node_ServerMessageReceived::GatherPayloadValues(TArray<FCrowdyServerValuePin>& OutValues) const
{
	const FCrowdyServerMessage* Message = GetServerMessage();
	CrowdyServerObjectPins::GatherValues(Message ? Message->GetPayloadStruct() : nullptr, OutValues);
}

bool UCrowdyK2Node_ServerMessageReceived::HasEveryPin(const UCrowdyServerObjectDefinition* InDefinition, FName Message)
{
	const FCrowdyServerMessage* Found = InDefinition ? InDefinition->FindMessage(Message) : nullptr;
	TArray<FCrowdyServerValuePin> Values;
	CrowdyServerObjectPins::GatherValues(Found ? Found->GetPayloadStruct() : nullptr, Values);
	return !Values.ContainsByPredicate([](const FCrowdyServerValuePin& Value) { return !Value.HasPin(); });
}

void UCrowdyK2Node_ServerMessageReceived::AllocateDefaultPins()
{
	UEdGraphPin* Bind = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Execute);
	Bind->PinFriendlyName = LOCTEXT("BindPin", "Bind");
	CreateSourcePins();
	CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Then);
	UEdGraphPin* Received = CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, CrowdyServerObjectNodePins::Received);
	Received->PinFriendlyName = LOCTEXT("ReceivedPin", "Received");
	Received->PinToolTip = LOCTEXT("ReceivedTooltip", "Runs with the message's payload each time the server sends it while the Server Object is Ready or reading; a message that arrives during a read runs once the values are current. A message sent while the connection is closed is lost.").ToString();
	TArray<FCrowdyServerValuePin> Payload;
	GatherPayloadValues(Payload);
	for (const FCrowdyServerValuePin& Value : Payload)
	{
		CreateValuePins(EGPD_Output, Value);
	}
	Super::AllocateDefaultPins();
}

FText UCrowdyK2Node_ServerMessageReceived::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	return MakeTitle(Member, GetAssetName());
}

FText UCrowdyK2Node_ServerMessageReceived::GetTooltipText() const
{
	return FText::Format(LOCTEXT("Tooltip", "Runs Received with the payload of {0} each time the Server Object's server code sends it, while the object is Ready or reading (a message that arrives during a read runs once the values are current). One sent while the connection is closed is lost. It stops running when this object is destroyed."),
		FText::FromName(Member));
}

bool UCrowdyK2Node_ServerMessageReceived::IsCompatibleWithGraph(const UEdGraph* TargetGraph) const
{
	const UEdGraphSchema* Schema = TargetGraph ? TargetGraph->GetSchema() : nullptr;
	return Super::IsCompatibleWithGraph(TargetGraph) && Schema && Schema->GetGraphType(TargetGraph) == GT_Ubergraph;
}

void UCrowdyK2Node_ServerMessageReceived::EarlyValidation(FCompilerResultsLog& MessageLog) const
{
	Super::EarlyValidation(MessageLog);
	const UEdGraphPin* Bind = FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
	if (IsNodeEnabled() && Bind && Bind->LinkedTo.Num() == 0)
	{
		MessageLog.Warning(*LOCTEXT("NeverBound", "@@ never runs: connect Bind.").ToString(), this);
	}
}

bool UCrowdyK2Node_ServerMessageReceived::CheckMessage(FKismetCompilerContext& CompilerContext, TArray<FCrowdyServerValuePin>& Out) const
{
	if (!CheckDefinition(CompilerContext))
	{
		return false;
	}
	const FCrowdyServerMessage* Message = GetServerMessage();
	if (!Message)
	{
		CompilerContext.MessageLog.Error(*FText::Format(LOCTEXT("NoMessage", "@@: {0} has no message {1} any more. Place it again from the menu, or give the message its old name back."),
			GetAssetName(), FText::FromName(Member)).ToString(), this);
		return false;
	}
	if (!Message->bPlayersCanListen)
	{
		CompilerContext.MessageLog.Error(*FText::Format(LOCTEXT("NotHeardByPlayers", "@@: {0} is heard only by other Server Object types; turn on Players Can Listen, a setting of message {0} in the definition asset {1}"),
			FText::FromName(Member), GetAssetName()).ToString(), this);
		return false;
	}
	GatherPayloadValues(Out);
	bool bAllPins = true;
	for (const FCrowdyServerValuePin& Value : Out)
	{
		bAllPins &= CheckValue(CompilerContext, Value);
	}
	return bAllPins;
}

void UCrowdyK2Node_ServerMessageReceived::ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph)
{
	Super::ExpandNode(CompilerContext, SourceGraph);

	TArray<FCrowdyServerValuePin> Payload;
	if (!CheckMessage(CompilerContext, Payload))
	{
		BreakAllNodeLinks();
		return;
	}

	// The handler is a custom event named after this node's GUID, so two nodes on one message stay two bindings.
	UK2Node_CustomEvent* Handler = CompilerContext.SpawnIntermediateNode<UK2Node_CustomEvent>(this, SourceGraph);
	Handler->CustomFunctionName = *FString::Printf(TEXT("OnServerMessage_%s"), *CompilerContext.GetGuid(this));
	Handler->AllocateDefaultPins();
	FEdGraphPinType PayloadType;
	PayloadType.PinCategory = UEdGraphSchema_K2::PC_Struct;
	PayloadType.PinSubCategoryObject = FInstancedStruct::StaticStruct();
	UEdGraphPin* HandlerPayload = Handler->CreateUserDefinedPin(TEXT("Payload"), PayloadType, EGPD_Output);

	UK2Node_CallFunction* BindCall = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
	BindCall->FunctionReference.SetExternalMember(GET_FUNCTION_NAME_CHECKED(UCrowdyServerObjectLibrary, BindServerMessage), UCrowdyServerObjectLibrary::StaticClass());
	BindCall->AllocateDefaultPins();
	UEdGraphPin* Target = BindCall->FindPin(TEXT("Target"));
	UEdGraphPin* DefinitionPin = BindCall->FindPin(TEXT("Definition"));
	UEdGraphPin* MessagePin = BindCall->FindPin(TEXT("Message"));
	UEdGraphPin* HandlerPin = BindCall->FindPin(TEXT("Handler"));
	UEdGraphPin* FunctionPin = BindCall->FindPin(TEXT("HandlerFunction"));
	if (!HandlerPayload || !Target || !DefinitionPin || !MessagePin || !HandlerPin || !FunctionPin)
	{
		CompilerContext.MessageLog.Error(*LOCTEXT("BindChanged", "@@ could not be expanded: refresh the node.").ToString(), this);
		BreakAllNodeLinks();
		return;
	}

	UEdGraphPin* Last = ExpandOutputs(CompilerContext, SourceGraph, Payload, Handler->FindPinChecked(UEdGraphSchema_K2::PN_Then), HandlerPayload);
	if (!Last)
	{
		BreakAllNodeLinks();
		return;
	}
	CompilerContext.MovePinLinksToIntermediate(*FindPinChecked(CrowdyServerObjectNodePins::Received), *Last);

	UK2Node_Self* Self = CompilerContext.SpawnIntermediateNode<UK2Node_Self>(this, SourceGraph);
	Self->AllocateDefaultPins();
	CompilerContext.GetSchema()->TryCreateConnection(Self->FindPinChecked(UEdGraphSchema_K2::PN_Self), HandlerPin);
	FunctionPin->DefaultValue = Handler->CustomFunctionName.ToString();
	DefinitionPin->DefaultObject = GetDefinition();
	MessagePin->DefaultValue = Member.ToString();
	ExpandSource(CompilerContext, SourceGraph, {Target});

	CompilerContext.MovePinLinksToIntermediate(*FindPinChecked(UEdGraphSchema_K2::PN_Execute), *BindCall->GetExecPin());
	CompilerContext.MovePinLinksToIntermediate(*FindPinChecked(UEdGraphSchema_K2::PN_Then), *BindCall->GetThenPin());
	BreakAllNodeLinks();
}

#undef LOCTEXT_NAMESPACE
