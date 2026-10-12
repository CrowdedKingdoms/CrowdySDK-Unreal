#include "Nodes/CrowdyK2Node_ReceiveServerValuesChangedOnEntity.h"

#include "BlueprintActionDatabaseRegistrar.h"
#include "BlueprintNodeSpawner.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Self.h"
#include "Kismet2/CompilerResultsLog.h"
#include "KismetCompiler.h"
#include "Styling/AppStyle.h"
#include "Target/CrowdyMassEntityTarget.h"
#include "Target/CrowdyMassServerValuesLibrary.h"
#include "Textures/SlateIcon.h"

#define LOCTEXT_NAMESPACE "CrowdyK2Node_ReceiveServerValuesChangedOnEntity"

namespace CrowdyMassServerValuesPins
{
	const FName Unbind(TEXT("Crowdy Mass Unbind"));
	const FName Then(TEXT("Crowdy Mass Then"));
	const FName OnChanged(TEXT("Crowdy Mass On Changed"));
	const FName EntityClass(TEXT("Crowdy Mass Entity Class"));
	// These two become parameters of the intermediate event, so they are spelled the way a parameter is.
	const FName Target(TEXT("CrowdyMassTarget"));
	const FName Changed(TEXT("Changed"));

	FEdGraphPinType ChangedPinType()
	{
		FEdGraphPinType PinType;
		PinType.PinCategory = UEdGraphSchema_K2::PC_String;
		PinType.ContainerType = EPinContainerType::Array;
		return PinType;
	}

	FEdGraphPinType TargetPinType()
	{
		FEdGraphPinType PinType;
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = FCrowdyMassEntityTarget::StaticStruct();
		return PinType;
	}
}

void UCrowdyK2Node_ReceiveServerValuesChangedOnEntity::AllocateDefaultPins()
{
	using namespace CrowdyMassServerValuesPins;

	UEdGraphPin* BindPin = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Execute);
	BindPin->PinFriendlyName = LOCTEXT("BindPinLabel", "Bind");
	UEdGraphPin* UnbindPin = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Exec, Unbind);
	UnbindPin->PinFriendlyName = LOCTEXT("UnbindPinLabel", "Unbind");

	UEdGraphPin* ClassPin = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Class, AActor::StaticClass(), EntityClass);
	ClassPin->PinFriendlyName = LOCTEXT("ClassPinLabel", "Entity Class");
	ClassPin->PinToolTip = LOCTEXT("ClassPinTooltip", "The actor class whose crowd rows are listened to, with its subclasses.").ToString();

	UEdGraphPin* ThenPin = CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, Then);
	ThenPin->PinFriendlyName = LOCTEXT("ThenPinLabel", "Then");
	UEdGraphPin* OnChangedPin = CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, OnChanged);
	OnChangedPin->PinFriendlyName = LOCTEXT("OnChangedPinLabel", "On Changed");

	UEdGraphPin* TargetPin = CreatePin(EGPD_Output, TargetPinType(), Target);
	TargetPin->PinFriendlyName = LOCTEXT("TargetPinLabel", "Target");
	UEdGraphPin* ChangedPin = CreatePin(EGPD_Output, ChangedPinType(), Changed);
	ChangedPin->PinToolTip = LOCTEXT("ChangedPinTooltip", "The variables that changed, as Get Server Value takes them. Empty when the entity's record was removed.").ToString();

	Super::AllocateDefaultPins();
}

UEdGraphPin* UCrowdyK2Node_ReceiveServerValuesChangedOnEntity::GetClassPin() const
{
	return FindPin(CrowdyMassServerValuesPins::EntityClass, EGPD_Input);
}

FText UCrowdyK2Node_ReceiveServerValuesChangedOnEntity::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	const UEdGraphPin* ClassPin = GetClassPin();
	const UClass* Picked = ClassPin && ClassPin->LinkedTo.IsEmpty() ? Cast<UClass>(ClassPin->DefaultObject) : nullptr;
	if (TitleType == ENodeTitleType::MenuTitle || !Picked)
	{
		return LOCTEXT("NodeTitle", "Receive Server Values Changed on Entity");
	}
	FString ClassName = Picked->GetName();
	ClassName.RemoveFromEnd(TEXT("_C"), ESearchCase::CaseSensitive);
	return FText::Format(LOCTEXT("NodeTitlePicked", "Receive Server Values Changed on Entity\n{0}"), FText::FromString(ClassName));
}

FText UCrowdyK2Node_ReceiveServerValuesChangedOnEntity::GetTooltipText() const
{
	return LOCTEXT("NodeTooltip",
		"Runs part of this graph whenever the server values of a crowd entity of Entity Class change: the values its Crowdy "
		"Server Object component reads from a record per entity. Target is the entity and Changed names the variables.\n\n"
		"Bind starts listening and Unbind stops; the binding also ends when this object or its world does.");
}

FSlateIcon UCrowdyK2Node_ReceiveServerValuesChangedOnEntity::GetIconAndTint(FLinearColor& OutColor) const
{
	OutColor = GetNodeTitleColor();
	static FSlateIcon Icon(FAppStyle::GetAppStyleSetName(), "GraphEditor.Event_16x");
	return Icon;
}

FText UCrowdyK2Node_ReceiveServerValuesChangedOnEntity::GetMenuCategory() const
{
	return LOCTEXT("MenuCategory", "Crowdy Mass|Server Values");
}

void UCrowdyK2Node_ReceiveServerValuesChangedOnEntity::GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const
{
	UClass* ActionKey = GetClass();
	if (!ActionRegistrar.IsOpenForRegistration(ActionKey))
	{
		return;
	}
	UBlueprintNodeSpawner* Spawner = UBlueprintNodeSpawner::Create(GetClass());
	check(Spawner);
	ActionRegistrar.AddBlueprintAction(ActionKey, Spawner);
}

bool UCrowdyK2Node_ReceiveServerValuesChangedOnEntity::IsCompatibleWithGraph(const UEdGraph* TargetGraph) const
{
	// The node compiles to an event, which only an event graph can hold.
	const UEdGraphSchema* GraphSchema = TargetGraph ? TargetGraph->GetSchema() : nullptr;
	return Super::IsCompatibleWithGraph(TargetGraph) && GraphSchema && GraphSchema->GetGraphType(TargetGraph) == EGraphType::GT_Ubergraph;
}

bool UCrowdyK2Node_ReceiveServerValuesChangedOnEntity::CanPasteHere(const UEdGraph* TargetGraph) const
{
	return Super::CanPasteHere(TargetGraph) && IsCompatibleWithGraph(TargetGraph);
}

// Checked on the source graph: the compiler validates its merged copy only after expansion has moved this node's links away.
void UCrowdyK2Node_ReceiveServerValuesChangedOnEntity::EarlyValidation(FCompilerResultsLog& MessageLog) const
{
	Super::EarlyValidation(MessageLog);

	if (!IsNodeEnabled())
	{
		return;
	}
	const UEdGraphPin* BindPin = FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
	if (BindPin && BindPin->LinkedTo.IsEmpty())
	{
		MessageLog.Warning(*LOCTEXT("NeverBound", "@@ is never reached: nothing runs into Bind, so it listens for nothing.").ToString(), this);
	}
	const UEdGraphPin* ClassPin = GetClassPin();
	if (ClassPin && ClassPin->LinkedTo.IsEmpty() && !ClassPin->DefaultObject)
	{
		MessageLog.Error(*LOCTEXT("NoClassPicked", "@@ has no Entity Class. Pick the actor class whose crowd rows it listens to.").ToString(), this);
		return;
	}
	const UClass* Picked = ClassPin && ClassPin->LinkedTo.IsEmpty() ? Cast<UClass>(ClassPin->DefaultObject) : nullptr;
	if (Picked && Picked->GetDefaultObject() && !UCrowdyMassServerValuesLibrary::ClassGetsServerValues(Picked))
	{
		MessageLog.Warning(*LOCTEXT("NoRecordsComponent",
			"@@ listens to an Entity Class with no Crowdy Server Object component that reads a record per entity (Only One Instance, Instance Id or This Actor's Area), so it never runs.").ToString(), this);
	}
}

void UCrowdyK2Node_ReceiveServerValuesChangedOnEntity::ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph)
{
	using namespace CrowdyMassServerValuesPins;

	Super::ExpandNode(CompilerContext, SourceGraph);

	const UEdGraphSchema_K2* Schema = CompilerContext.GetSchema();
	UFunction* BindFunction = UCrowdyMassServerValuesLibrary::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(UCrowdyMassServerValuesLibrary, CrowdyMassBindServerValuesChangedOnEntity));
	UFunction* UnbindFunction = UCrowdyMassServerValuesLibrary::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(UCrowdyMassServerValuesLibrary, CrowdyMassUnbindServerValuesChangedOnEntity));
	UEdGraphPin* BindPin = FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
	UEdGraphPin* UnbindPin = FindPin(Unbind, EGPD_Input);
	UEdGraphPin* ClassPin = GetClassPin();
	UEdGraphPin* ThenPin = FindPin(Then, EGPD_Output);
	UEdGraphPin* OnChangedPin = FindPin(OnChanged, EGPD_Output);
	UEdGraphPin* TargetPin = FindPin(Target, EGPD_Output);
	UEdGraphPin* ChangedPin = FindPin(Changed, EGPD_Output);
	if (!Schema || !BindFunction || !UnbindFunction || !BindPin || !UnbindPin || !ClassPin || !ThenPin || !OnChangedPin || !TargetPin || !ChangedPin)
	{
		CompilerContext.MessageLog.Error(*LOCTEXT("ExpandMissingParts", "@@ cannot compile: it is missing one of its pins or the runtime entry it binds through. Refresh the node.").ToString(), this);
		BreakAllNodeLinks();
		return;
	}

	// The intermediate event is the handler the runtime calls by name, named from this node's GUID so two nodes stay two bindings.
	UK2Node_CustomEvent* Handler = CompilerContext.SpawnIntermediateNode<UK2Node_CustomEvent>(this, SourceGraph);
	Handler->CustomFunctionName = *FString::Printf(TEXT("CrowdyMassServerValues_%s"), *CompilerContext.GetGuid(this));
	Handler->AllocateDefaultPins();
	UEdGraphPin* HandlerTarget = Handler->CreateUserDefinedPin(Target, TargetPinType(), EGPD_Output);
	UEdGraphPin* HandlerChanged = Handler->CreateUserDefinedPin(Changed, ChangedPinType(), EGPD_Output);
	if (!HandlerTarget || !HandlerChanged || HandlerTarget->PinName != Target || HandlerChanged->PinName != Changed)
	{
		CompilerContext.MessageLog.Error(*LOCTEXT("HandlerPinsRenamed", "@@ cannot compile: the event it compiles to could not take its Target and Changed parameters.").ToString(), this);
		BreakAllNodeLinks();
		return;
	}
	CompilerContext.MovePinLinksToIntermediate(*TargetPin, *HandlerTarget);
	CompilerContext.MovePinLinksToIntermediate(*ChangedPin, *HandlerChanged);
	CompilerContext.MovePinLinksToIntermediate(*OnChangedPin, *Handler->FindPinChecked(UEdGraphSchema_K2::PN_Then));

	UK2Node_Self* SelfNode = CompilerContext.SpawnIntermediateNode<UK2Node_Self>(this, SourceGraph);
	SelfNode->AllocateDefaultPins();
	UEdGraphPin* SelfPin = SelfNode->FindPinChecked(UEdGraphSchema_K2::PN_Self);
	const FString HandlerName = Handler->GetFunctionName().ToString();

	bool bWiredSelf = true;
	for (UFunction* Function : {BindFunction, UnbindFunction})
	{
		UK2Node_CallFunction* Call = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
		Call->SetFromFunction(Function);
		Call->AllocateDefaultPins();
		bWiredSelf &= Schema->TryCreateConnection(SelfPin, Call->FindPinChecked(TEXT("HandlerObject")));
		Call->FindPinChecked(TEXT("HandlerFunctionName"))->DefaultValue = HandlerName;
		CompilerContext.CopyPinLinksToIntermediate(*ClassPin, *Call->FindPinChecked(TEXT("EntityClass")));
		CompilerContext.MovePinLinksToIntermediate(Function == BindFunction ? *BindPin : *UnbindPin, *Call->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
		// Copied to both: Then continues after whichever of Bind and Unbind ran.
		CompilerContext.CopyPinLinksToIntermediate(*ThenPin, *Call->FindPinChecked(UEdGraphSchema_K2::PN_Then));
	}
	if (!bWiredSelf)
	{
		CompilerContext.MessageLog.Error(*LOCTEXT("SelfNotWired", "@@ cannot compile: this Blueprint's own reference could not be passed to the binding.").ToString(), this);
	}

	BreakAllNodeLinks();
}

#undef LOCTEXT_NAMESPACE
