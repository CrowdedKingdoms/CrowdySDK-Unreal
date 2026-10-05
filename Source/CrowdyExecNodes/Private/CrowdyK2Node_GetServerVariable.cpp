#include "CrowdyK2Node_GetServerVariable.h"

#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectLibrary.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "KismetCompiler.h"

#define LOCTEXT_NAMESPACE "CrowdyK2Node_GetServerVariable"

FText UCrowdyK2Node_GetServerVariable::MakeTitle(FName Variable, const FText& Asset)
{
	return FText::Format(LOCTEXT("Title", "Get {0} ({1})"), FText::FromName(Variable), Asset);
}

void UCrowdyK2Node_GetServerVariable::AllocateDefaultPins()
{
	RefreshMemberVariable();
	CreateSourcePins();
	FCrowdyServerValuePin Variable;
	if (FindMemberVariable(Variable) && CreateValuePins(EGPD_Output, Variable) && !Variable.bOptional)
	{
		CreateReadyPin(CrowdyServerObjectPins::HasValuePinName(Variable.Name), CrowdyServerObjectPins::HasValuePinId(Variable.Id));
	}
	Super::AllocateDefaultPins();
}

FText UCrowdyK2Node_GetServerVariable::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	return MakeTitle(Member, GetAssetName());
}

FText UCrowdyK2Node_GetServerVariable::GetTooltipText() const
{
	return FText::Format(LOCTEXT("Tooltip", "The value of {0} on the Server Object, read where it is kept. Before the object is ready it reads the default."), FText::FromName(Member));
}

void UCrowdyK2Node_GetServerVariable::ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph)
{
	Super::ExpandNode(CompilerContext, SourceGraph);

	FCrowdyServerValuePin Variable;
	if (!CheckVariable(CompilerContext, Variable))
	{
		BreakAllNodeLinks();
		return;
	}

	UK2Node_CallFunction* Read = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
	Read->FunctionReference.SetExternalMember(GET_FUNCTION_NAME_CHECKED(UCrowdyServerObjectLibrary, ReadServerVariable), UCrowdyServerObjectLibrary::StaticClass());
	Read->AllocateDefaultPins();
	UEdGraphPin* Target = Read->FindPin(TEXT("Target"));
	UEdGraphPin* DefinitionPin = Read->FindPin(TEXT("Definition"));
	UEdGraphPin* VariablePin = Read->FindPin(TEXT("Variable"));
	UEdGraphPin* Value = Read->FindPin(TEXT("Value"));
	UEdGraphPin* Found = Read->GetReturnValuePin();
	UEdGraphPin* Pin = FindPin(Variable.Name, EGPD_Output);
	if (!Target || !DefinitionPin || !VariablePin || !Value || !Found || !Pin)
	{
		CompilerContext.MessageLog.Error(*LOCTEXT("ReadChanged", "@@ could not be expanded: refresh the node.").ToString(), this);
		BreakAllNodeLinks();
		return;
	}

	DefinitionPin->DefaultObject = GetDefinition();
	VariablePin->DefaultValue = Variable.Name.ToString();
	Value->PinType = Variable.Type;
	ExpandSource(CompilerContext, SourceGraph, {Target});
	MoveValueLinks(CompilerContext, *Pin, *Value);
	if (UEdGraphPin* HasValue = FindPin(CrowdyServerObjectPins::HasValuePinName(Variable.Name), EGPD_Output))
	{
		MoveValueLinks(CompilerContext, *HasValue, *Found);
	}
	BreakAllNodeLinks();
}

#undef LOCTEXT_NAMESPACE
