#include "CrowdyK2Node_GetServerState.h"

#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectLibrary.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "KismetCompiler.h"

#define LOCTEXT_NAMESPACE "CrowdyK2Node_GetServerState"

FText UCrowdyK2Node_GetServerState::MakeTitle(const FText& Asset)
{
	return FText::Format(LOCTEXT("Title", "Get Server State ({0})"), Asset);
}

void UCrowdyK2Node_GetServerState::AllocateDefaultPins()
{
	CreateSourcePins();
	TArray<FCrowdyServerValuePin> Variables;
	GatherVisibleVariables(Variables);
	for (const FCrowdyServerValuePin& Variable : Variables)
	{
		CreateValuePins(EGPD_Output, Variable);
	}
	CreateReadyPin(CrowdyServerObjectNodePins::HasValues, FGuid());
	Super::AllocateDefaultPins();
}

void UCrowdyK2Node_GetServerState::GatherVisibleVariables(TArray<FCrowdyServerValuePin>& Out) const
{
	CrowdyServerObjectPins::GatherVariables(GetDefinition(), Out);
	Out.RemoveAll([](const FCrowdyServerValuePin& Variable) { return !Variable.bVisible; });
}

FText UCrowdyK2Node_GetServerState::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	return MakeTitle(GetAssetName());
}

FText UCrowdyK2Node_GetServerState::GetTooltipText() const
{
	return LOCTEXT("Tooltip", "Every variable of the Server Object, read where it is kept. Before the object is ready they read their defaults.");
}

void UCrowdyK2Node_GetServerState::ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph)
{
	Super::ExpandNode(CompilerContext, SourceGraph);

	if (!CheckDefinition(CompilerContext))
	{
		BreakAllNodeLinks();
		return;
	}
	TArray<FCrowdyServerValuePin> Variables;
	GatherVisibleVariables(Variables);
	bool bAllPins = true;
	for (const FCrowdyServerValuePin& Variable : Variables)
	{
		bAllPins &= CheckValue(CompilerContext, Variable);
	}
	if (!bAllPins)
	{
		BreakAllNodeLinks();
		return;
	}

	// Has Values rides one read, of a variable that is not optional where there is one, since an empty optional also reads false.
	UEdGraphPin* HasValues = FindPin(CrowdyServerObjectNodePins::HasValues, EGPD_Output);
	const FCrowdyServerValuePin* ReadyRead = Variables.FindByPredicate([](const FCrowdyServerValuePin& Variable) { return !Variable.bOptional; });
	if (!ReadyRead && !Variables.IsEmpty())
	{
		ReadyRead = &Variables[0];
	}
	TArray<UEdGraphPin*> Targets;
	for (const FCrowdyServerValuePin& Variable : Variables)
	{
		UEdGraphPin* Pin = FindPin(Variable.Name, EGPD_Output);
		UEdGraphPin* HasValue = Variable.bOptional ? FindPin(CrowdyServerObjectPins::HasValuePinName(Variable.Name), EGPD_Output) : nullptr;
		const bool bReady = HasValues && HasValues->LinkedTo.Num() > 0 && &Variable == ReadyRead;
		if ((!Pin || Pin->LinkedTo.Num() == 0) && (!HasValue || HasValue->LinkedTo.Num() == 0) && !bReady)
		{
			continue;
		}

		UK2Node_CallFunction* Read = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
		Read->FunctionReference.SetExternalMember(GET_FUNCTION_NAME_CHECKED(UCrowdyServerObjectLibrary, ReadServerVariable), UCrowdyServerObjectLibrary::StaticClass());
		Read->AllocateDefaultPins();
		UEdGraphPin* Target = Read->FindPin(TEXT("Target"));
		UEdGraphPin* DefinitionPin = Read->FindPin(TEXT("Definition"));
		UEdGraphPin* VariablePin = Read->FindPin(TEXT("Variable"));
		UEdGraphPin* Value = Read->FindPin(TEXT("Value"));
		UEdGraphPin* Found = Read->GetReturnValuePin();
		if (!Pin || !Target || !DefinitionPin || !VariablePin || !Value || !Found)
		{
			CompilerContext.MessageLog.Error(*LOCTEXT("ReadChanged", "@@ could not be expanded: refresh the node.").ToString(), this);
			BreakAllNodeLinks();
			return;
		}

		DefinitionPin->DefaultObject = GetDefinition();
		VariablePin->DefaultValue = Variable.Name.ToString();
		Value->PinType = Variable.Type;
		MoveValueLinks(CompilerContext, *Pin, *Value);
		if (HasValue)
		{
			MoveValueLinks(CompilerContext, *HasValue, *Found);
		}
		if (bReady)
		{
			MoveValueLinks(CompilerContext, *HasValues, *Found);
		}
		Targets.Add(Target);
	}
	if (Targets.Num() > 0)
	{
		ExpandSource(CompilerContext, SourceGraph, Targets);
	}
	BreakAllNodeLinks();
}

#undef LOCTEXT_NAMESPACE
