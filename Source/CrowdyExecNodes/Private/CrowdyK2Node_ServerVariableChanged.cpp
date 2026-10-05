#include "CrowdyK2Node_ServerVariableChanged.h"

#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectLibrary.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Self.h"
#include "KismetCompiler.h"

#define LOCTEXT_NAMESPACE "CrowdyK2Node_ServerVariableChanged"

FText UCrowdyK2Node_ServerVariableChanged::MakeTitle(FName Variable, const FText& Asset)
{
	return FText::Format(LOCTEXT("Title", "On {0} Changed ({1})"), FText::FromName(Variable), Asset);
}

void UCrowdyK2Node_ServerVariableChanged::AllocateDefaultPins()
{
	RefreshMemberVariable();
	UEdGraphPin* Bind = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Execute);
	Bind->PinFriendlyName = LOCTEXT("BindPin", "Bind");
	CreateSourcePins();
	CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Then);
	UEdGraphPin* Changed = CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, CrowdyServerObjectNodePins::Changed);
	Changed->PinFriendlyName = LOCTEXT("ChangedPin", "Changed");
	Changed->PinToolTip = LOCTEXT("ChangedTooltip", "Runs with the current value once the object is ready, and after every change of the variable.").ToString();
	FCrowdyServerValuePin Variable;
	if (FindMemberVariable(Variable))
	{
		CreateValuePins(EGPD_Output, Variable);
	}
	Super::AllocateDefaultPins();
}

FText UCrowdyK2Node_ServerVariableChanged::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	return MakeTitle(Member, GetAssetName());
}

FText UCrowdyK2Node_ServerVariableChanged::GetTooltipText() const
{
	return FText::Format(LOCTEXT("Tooltip", "Runs Changed with the value of {0} once the Server Object is ready, then after every change of {0}. It stops by itself when this object is destroyed."),
		FText::FromName(Member));
}

bool UCrowdyK2Node_ServerVariableChanged::IsCompatibleWithGraph(const UEdGraph* TargetGraph) const
{
	const UEdGraphSchema* Schema = TargetGraph ? TargetGraph->GetSchema() : nullptr;
	return Super::IsCompatibleWithGraph(TargetGraph) && Schema && Schema->GetGraphType(TargetGraph) == GT_Ubergraph;
}

void UCrowdyK2Node_ServerVariableChanged::EarlyValidation(FCompilerResultsLog& MessageLog) const
{
	Super::EarlyValidation(MessageLog);
	const UEdGraphPin* Bind = FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
	if (IsNodeEnabled() && Bind && Bind->LinkedTo.Num() == 0)
	{
		MessageLog.Warning(*LOCTEXT("NeverBound", "@@ never runs: connect Bind.").ToString(), this);
	}
}

void UCrowdyK2Node_ServerVariableChanged::ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph)
{
	Super::ExpandNode(CompilerContext, SourceGraph);

	FCrowdyServerValuePin Variable;
	if (!CheckVariable(CompilerContext, Variable))
	{
		BreakAllNodeLinks();
		return;
	}

	// The handler is a custom event named after this node's GUID, so two nodes on one variable stay two bindings.
	UK2Node_CustomEvent* Handler = CompilerContext.SpawnIntermediateNode<UK2Node_CustomEvent>(this, SourceGraph);
	Handler->CustomFunctionName = *FString::Printf(TEXT("OnServerVariableChanged_%s"), *CompilerContext.GetGuid(this));
	Handler->AllocateDefaultPins();
	const FName ValueName(TEXT("Value"));
	const FName HasValueName(TEXT("HasValue"));
	UEdGraphPin* HandlerValue = Handler->CreateUserDefinedPin(ValueName, Variable.Type, EGPD_Output);
	FEdGraphPinType BoolType;
	BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	UEdGraphPin* HandlerHasValue = Variable.bOptional ? Handler->CreateUserDefinedPin(HasValueName, BoolType, EGPD_Output) : nullptr;

	UK2Node_CallFunction* BindCall = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
	BindCall->FunctionReference.SetExternalMember(GET_FUNCTION_NAME_CHECKED(UCrowdyServerObjectLibrary, BindServerVariableChanged), UCrowdyServerObjectLibrary::StaticClass());
	BindCall->AllocateDefaultPins();
	UEdGraphPin* Target = BindCall->FindPin(TEXT("Target"));
	UEdGraphPin* DefinitionPin = BindCall->FindPin(TEXT("Definition"));
	UEdGraphPin* VariablePin = BindCall->FindPin(TEXT("Variable"));
	UEdGraphPin* HandlerPin = BindCall->FindPin(TEXT("Handler"));
	UEdGraphPin* FunctionPin = BindCall->FindPin(TEXT("HandlerFunction"));
	UEdGraphPin* Value = FindPin(Variable.Name, EGPD_Output);
	UEdGraphPin* HasValue = FindPin(CrowdyServerObjectPins::HasValuePinName(Variable.Name), EGPD_Output);
	const bool bHandlerShaped = HandlerValue && HandlerValue->PinName == ValueName && (!Variable.bOptional || (HandlerHasValue && HandlerHasValue->PinName == HasValueName && HasValue));
	if (!bHandlerShaped || !Target || !DefinitionPin || !VariablePin || !HandlerPin || !FunctionPin || !Value)
	{
		CompilerContext.MessageLog.Error(*LOCTEXT("BindChanged", "@@ could not be expanded: refresh the node.").ToString(), this);
		BreakAllNodeLinks();
		return;
	}

	MoveValueLinks(CompilerContext, *Value, *HandlerValue);
	if (Variable.bOptional)
	{
		MoveValueLinks(CompilerContext, *HasValue, *HandlerHasValue);
	}
	CompilerContext.MovePinLinksToIntermediate(*FindPinChecked(CrowdyServerObjectNodePins::Changed), *Handler->FindPinChecked(UEdGraphSchema_K2::PN_Then));

	UK2Node_Self* Self = CompilerContext.SpawnIntermediateNode<UK2Node_Self>(this, SourceGraph);
	Self->AllocateDefaultPins();
	CompilerContext.GetSchema()->TryCreateConnection(Self->FindPinChecked(UEdGraphSchema_K2::PN_Self), HandlerPin);
	FunctionPin->DefaultValue = Handler->CustomFunctionName.ToString();
	DefinitionPin->DefaultObject = GetDefinition();
	VariablePin->DefaultValue = Variable.Name.ToString();
	ExpandSource(CompilerContext, SourceGraph, {Target});

	CompilerContext.MovePinLinksToIntermediate(*FindPinChecked(UEdGraphSchema_K2::PN_Execute), *BindCall->GetExecPin());
	CompilerContext.MovePinLinksToIntermediate(*FindPinChecked(UEdGraphSchema_K2::PN_Then), *BindCall->GetThenPin());
	BreakAllNodeLinks();
}

#undef LOCTEXT_NAMESPACE
