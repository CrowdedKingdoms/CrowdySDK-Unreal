#include "CrowdyK2Node_CallServerFunction.h"

#include "BlueprintActionDatabaseRegistrar.h"
#include "BlueprintNodeSpawner.h"
#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectLibrary.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "K2Node_AsyncAction.h"
#include "K2Node_CallFunction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "KismetCompiler.h"
#include "ScopedTransaction.h"
#include "StructUtils/InstancedStruct.h"
#include "TimerManager.h"
#include "UObject/PropertyOptional.h"

#define LOCTEXT_NAMESPACE "CrowdyK2Node_CallServerFunction"

const FName CrowdyServerCallPins::FunctionPicker(TEXT("Server Function"));
const FName CrowdyServerCallPins::OnSuccess(TEXT("On Success"));
const FName CrowdyServerCallPins::OnFailed(TEXT("On Failed"));
const FName CrowdyServerCallPins::Outcome(TEXT("Call Outcome"));
const FName CrowdyServerCallPins::Reason(TEXT("Call Reason"));
const FName CrowdyServerCallPins::Retryable(TEXT("Call Retryable"));

FText UCrowdyK2Node_CallServerFunction::MakeTitle(FName Function, const FText& Asset)
{
	return FText::Format(LOCTEXT("Title", "Call {0} ({1})"), FText::FromName(Function), Asset);
}

const FCrowdyServerFunction* UCrowdyK2Node_CallServerFunction::GetFunction() const
{
	const UCrowdyServerObjectDefinition* Current = GetDefinition();
	return Current && !Member.IsNone() ? Current->FindFunction(Member) : nullptr;
}

void UCrowdyK2Node_CallServerFunction::GatherFunctionValues(TArray<FCrowdyServerValuePin>& OutInputs, TArray<FCrowdyServerValuePin>& OutOutputs) const
{
	GatherFunctionValues(GetFunction(), OutInputs, OutOutputs);
}

void UCrowdyK2Node_CallServerFunction::GatherFunctionValues(const FCrowdyServerFunction* Function, TArray<FCrowdyServerValuePin>& OutInputs, TArray<FCrowdyServerValuePin>& OutOutputs)
{
	CrowdyServerObjectPins::GatherValues(Function ? Function->GetParamsStruct() : nullptr, OutInputs);
	CrowdyServerObjectPins::GatherValues(Function ? Function->GetReplyStruct() : nullptr, OutOutputs);
}

bool UCrowdyK2Node_CallServerFunction::HasEveryPin(const UCrowdyServerObjectDefinition* InDefinition, FName Function)
{
	TArray<FCrowdyServerValuePin> Values;
	TArray<FCrowdyServerValuePin> Outputs;
	GatherFunctionValues(InDefinition ? InDefinition->FindFunction(Function) : nullptr, Values, Outputs);
	Values.Append(Outputs);
	return !Values.ContainsByPredicate([](const FCrowdyServerValuePin& Value) { return !Value.HasPin(); });
}

void UCrowdyK2Node_CallServerFunction::SetPickedFunction(UCrowdyServerObjectDefinition* InDefinition, FName Function)
{
	if (!GEditor)
	{
		PickFunction(InDefinition, Function);
		return;
	}
	TWeakObjectPtr<UCrowdyServerObjectDefinition> WeakDefinition = InDefinition;
	GEditor->GetTimerManager()->SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this, WeakDefinition, Function]()
	{
		PickFunction(WeakDefinition.Get(), Function);
	}));
}

void UCrowdyK2Node_CallServerFunction::PickFunction(UCrowdyServerObjectDefinition* InDefinition, FName Function)
{
	if (!InDefinition || (Definition == InDefinition && Member == Function))
	{
		return;
	}
	const FScopedTransaction Transaction(LOCTEXT("PickFunction", "Pick Server Function"));
	Modify();
	Definition = InDefinition;
	Member = Function;
	ReconstructAndMarkModified();
}

FText UCrowdyK2Node_CallServerFunction::GetPickedFunctionText() const
{
	const UCrowdyServerObjectDefinition* Current = GetDefinition();
	if (!Current || Member.IsNone())
	{
		return LOCTEXT("NoFunction", "(Pick a function)");
	}
	return FText::Format(LOCTEXT("PickedFunction", "{0}: {1}"), FText::FromString(Current->GetName()), FText::FromName(Member));
}

void UCrowdyK2Node_CallServerFunction::AllocateDefaultPins()
{
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Execute);
	CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Then);
	if (bPickFunction)
	{
		UEdGraphPin* Picker = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Name, CrowdyServerCallPins::FunctionPicker);
		Picker->PinFriendlyName = LOCTEXT("FunctionPin", "Function");
		Picker->bNotConnectable = true;
	}
	if (GetDefinition())
	{
		CreateSourcePins();
	}

	UEdGraphPin* Success = CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, CrowdyServerCallPins::OnSuccess);
	Success->PinToolTip = LOCTEXT("SuccessTooltip", "The function ran; its outputs are set.").ToString();
	UEdGraphPin* Failed = CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, CrowdyServerCallPins::OnFailed);
	Failed->PinToolTip = LOCTEXT("FailedTooltip", "The call did not succeed; Reason says why.").ToString();

	TArray<FCrowdyServerValuePin> Inputs;
	TArray<FCrowdyServerValuePin> Outputs;
	GatherFunctionValues(Inputs, Outputs);
	for (const FCrowdyServerValuePin& Input : Inputs)
	{
		CreateValuePins(EGPD_Input, Input);
	}
	for (const FCrowdyServerValuePin& Output : Outputs)
	{
		CreateValuePins(EGPD_Output, Output);
	}

	UEdGraphPin* Outcome = CreateAdvancedOutput(UEdGraphSchema_K2::PC_Byte, StaticEnum<ECrowdyServerCallOutcome>(), CrowdyServerCallPins::Outcome, LOCTEXT("OutcomeTooltip", "How the call ended."));
	Outcome->PinFriendlyName = LOCTEXT("OutcomePin", "Outcome");
	UEdGraphPin* Reason = CreateAdvancedOutput(UEdGraphSchema_K2::PC_String, nullptr, CrowdyServerCallPins::Reason, LOCTEXT("ReasonTooltip", "Why the call failed; empty on success."));
	Reason->PinFriendlyName = LOCTEXT("ReasonPin", "Reason");
	UEdGraphPin* Retryable = CreateAdvancedOutput(UEdGraphSchema_K2::PC_Boolean, nullptr, CrowdyServerCallPins::Retryable, LOCTEXT("RetryableTooltip", "Whether calling again may succeed."));
	Retryable->PinFriendlyName = LOCTEXT("RetryablePin", "Retryable");
	PrefillInputDefaults(Inputs);
	Super::AllocateDefaultPins();
}

void UCrowdyK2Node_CallServerFunction::PrefillInputDefaults(TConstArrayView<FCrowdyServerValuePin> Inputs)
{
	const FCrowdyServerFunction* Function = GetFunction();
	const UScriptStruct* Params = Function ? Function->GetParamsStruct() : nullptr;
	if (!Params)
	{
		return;
	}
	FInstancedStruct Values;
	Function->InitializeParams(Values);
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	for (const FCrowdyServerValuePin& Input : Inputs)
	{
		UEdGraphPin* Pin = Input.HasPin() ? FindPin(Input.Name, EGPD_Input) : nullptr;
		if (!Pin || Pin->PinType.IsContainer() || !Values.IsValid())
		{
			continue;
		}
		const FOptionalProperty* Optional = CastField<FOptionalProperty>(Input.Property);
		const FProperty* ValueProperty = Optional ? Optional->GetValueProperty() : Input.Property;
		const void* Value = Input.Property->ContainerPtrToValuePtr<void>(Values.GetMemory());
		if (UEdGraphPin* HasValue = Optional ? FindPin(CrowdyServerObjectPins::HasValuePinName(Input.Name), EGPD_Input) : nullptr)
		{
			Schema->SetPinAutogeneratedDefaultValue(HasValue, Optional->IsSet(Value) ? TEXT("true") : TEXT("false"));
			Value = Optional->GetValuePointerForReadIfSet(Value);
		}
		FString Text;
		if (Value && FBlueprintEditorUtils::PropertyValueToString_Direct(ValueProperty, static_cast<const uint8*>(Value), Text))
		{
			Schema->SetPinAutogeneratedDefaultValue(Pin, Text);
		}
	}
}

FText UCrowdyK2Node_CallServerFunction::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	if (!GetDefinition() || Member.IsNone())
	{
		return LOCTEXT("GenericTitle", "Call Server Function");
	}
	return MakeTitle(Member, GetAssetName());
}

FText UCrowdyK2Node_CallServerFunction::GetTooltipText() const
{
	return LOCTEXT("Tooltip", "Calls a Server Function with its inputs. On Success runs with its outputs; On Failed runs with Reason, and Retryable when calling again later may succeed.\n\nLatent: it completes later, and can only be placed in event graphs.");
}

bool UCrowdyK2Node_CallServerFunction::IsCompatibleWithGraph(const UEdGraph* TargetGraph) const
{
	const UEdGraphSchema* Schema = TargetGraph ? TargetGraph->GetSchema() : nullptr;
	const EGraphType Type = Schema ? Schema->GetGraphType(TargetGraph) : GT_Function;
	return Super::IsCompatibleWithGraph(TargetGraph) && (Type == GT_Ubergraph || Type == GT_Macro);
}

FText UCrowdyK2Node_CallServerFunction::GetMenuCategory() const
{
	return GetDefinition() ? Super::GetMenuCategory() : LOCTEXT("GenericCategory", "Server Objects");
}

FName UCrowdyK2Node_CallServerFunction::GetCornerIcon() const
{
	return TEXT("Graph.Latent.LatentIcon");
}

void UCrowdyK2Node_CallServerFunction::GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const
{
	Super::GetMenuActions(ActionRegistrar);
	if (!ActionRegistrar.IsOpenForRegistration(GetClass()))
	{
		return;
	}
	UBlueprintNodeSpawner* Spawner = UBlueprintNodeSpawner::Create(GetClass());
	Spawner->DefaultMenuSignature.MenuName = LOCTEXT("GenericTitle", "Call Server Function");
	Spawner->DefaultMenuSignature.Category = LOCTEXT("GenericCategory", "Server Objects");
	Spawner->CustomizeNodeDelegate = UBlueprintNodeSpawner::FCustomizeNodeDelegate::CreateLambda([](UEdGraphNode* NewNode, bool /*bIsTemplateNode*/)
	{
		CastChecked<UCrowdyK2Node_CallServerFunction>(NewNode)->bPickFunction = true;
	});
	ActionRegistrar.AddBlueprintAction(GetClass(), Spawner);
}

UEdGraphPin* UCrowdyK2Node_CallServerFunction::ChainInputError(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, UEdGraphPin* Errors, UEdGraphPin* Set, FName Input)
{
	UK2Node_CallFunction* Note = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
	Note->FunctionReference.SetExternalMember(GET_FUNCTION_NAME_CHECKED(UCrowdyServerObjectLibrary, NoteInputError), UCrowdyServerObjectLibrary::StaticClass());
	Note->AllocateDefaultPins();
	UEdGraphPin* NoteErrors = Note->FindPin(TEXT("Errors"));
	UEdGraphPin* NoteSet = Note->FindPin(TEXT("bSet"));
	UEdGraphPin* NoteInput = Note->FindPin(TEXT("Input"));
	if (!Set || !NoteErrors || !NoteSet || !NoteInput || !Note->GetReturnValuePin())
	{
		CompilerContext.MessageLog.Error(*LOCTEXT("NoteChanged", "@@ could not be expanded: Note Input Error has changed its parameters.").ToString(), this);
		return nullptr;
	}
	const UEdGraphSchema_K2* Schema = CompilerContext.GetSchema();
	if (Errors)
	{
		Schema->TryCreateConnection(Errors, NoteErrors);
	}
	Schema->TryCreateConnection(Set, NoteSet);
	NoteInput->DefaultValue = Input.ToString();
	return Note->GetReturnValuePin();
}

UEdGraphPin* UCrowdyK2Node_CallServerFunction::ExpandInputs(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, TConstArrayView<FCrowdyServerValuePin> Inputs, UEdGraphPin* InputsPin,
	UEdGraphPin* InputErrorPin)
{
	const UEdGraphSchema_K2* Schema = CompilerContext.GetSchema();
	UEdGraphPin* Then = InputsPin->GetOwningNode()->FindPinChecked(UEdGraphSchema_K2::PN_Then);
	// Each Set's result is noted, so a value its input cannot hold fails the call instead of sending the default.
	UEdGraphPin* Errors = nullptr;
	for (const FCrowdyServerValuePin& Input : Inputs)
	{
		UEdGraphPin* Pin = FindPin(Input.Name, EGPD_Input);
		UEdGraphPin* HasValue = Input.bOptional ? FindPin(CrowdyServerObjectPins::HasValuePinName(Input.Name), EGPD_Input) : nullptr;
		const bool bValueSet = Pin && (Pin->LinkedTo.Num() > 0 || !Pin->DoesDefaultValueMatchAutogenerated());
		const bool bHasValueSet = HasValue && (HasValue->LinkedTo.Num() > 0 || !HasValue->DoesDefaultValueMatchAutogenerated());
		if (!bValueSet && !bHasValueSet)
		{
			continue;
		}

		UK2Node_CallFunction* Set = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
		const FName SetFunction = HasValue ? GET_FUNCTION_NAME_CHECKED(UCrowdyServerObjectLibrary, SetServerValueOrUnset) : GET_FUNCTION_NAME_CHECKED(UCrowdyServerObjectLibrary, SetServerValue);
		Set->FunctionReference.SetExternalMember(SetFunction, UCrowdyServerObjectLibrary::StaticClass());
		Set->AllocateDefaultPins();
		UEdGraphPin* Values = Set->FindPin(TEXT("Values"));
		UEdGraphPin* Name = Set->FindPin(TEXT("Name"));
		UEdGraphPin* Value = Set->FindPin(TEXT("Value"));
		UEdGraphPin* SetHasValue = HasValue ? Set->FindPin(TEXT("bHasValue")) : nullptr;
		if (!Pin || !Values || !Name || !Value || (HasValue && !SetHasValue))
		{
			CompilerContext.MessageLog.Error(*LOCTEXT("SetChanged", "@@ could not be expanded: refresh the node.").ToString(), this);
			return nullptr;
		}

		Schema->TryCreateConnection(InputsPin, Values);
		Name->DefaultValue = Input.Name.ToString();
		const bool bReference = Value->PinType.bIsReference;
		const bool bConst = Value->PinType.bIsConst;
		Value->PinType = Input.Type;
		Value->PinType.bIsReference = bReference;
		Value->PinType.bIsConst = bConst;
		if (Pin->LinkedTo.Num() > 0)
		{
			MoveValueLinks(CompilerContext, *Pin, *Value);
		}
		else if (UEdGraphPin* Literal = UK2Node_CallFunction::InnerHandleAutoCreateRef(this, Value, CompilerContext, SourceGraph, true))
		{
			// A literal cannot be passed by reference, so it is held in a local first.
			Literal->DefaultValue = Pin->DefaultValue;
			Literal->DefaultObject = Pin->DefaultObject;
			Literal->DefaultTextValue = Pin->DefaultTextValue;
		}
		if (SetHasValue)
		{
			SetHasValue->DefaultValue = HasValue->DefaultValue;
			MoveValueLinks(CompilerContext, *HasValue, *SetHasValue);
		}
		Schema->TryCreateConnection(Then, Set->GetExecPin());
		Then = Set->GetThenPin();
		Errors = ChainInputError(CompilerContext, SourceGraph, Errors, Set->GetReturnValuePin(), Input.Name);
		if (!Errors)
		{
			return nullptr;
		}
	}
	if (Errors)
	{
		Schema->TryCreateConnection(Errors, InputErrorPin);
	}
	return Then;
}

UEdGraphPin* UCrowdyK2Node_CallServerFunction::ExpandOutputs(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, TConstArrayView<FCrowdyServerValuePin> Outputs, UEdGraphPin* SuccessPin, UEdGraphPin* OutputsPin)
{
	const UEdGraphSchema_K2* Schema = CompilerContext.GetSchema();
	UEdGraphPin* Then = SuccessPin;
	for (const FCrowdyServerValuePin& Output : Outputs)
	{
		UEdGraphPin* Pin = FindPin(Output.Name, EGPD_Output);
		UEdGraphPin* HasValue = Output.bOptional ? FindPin(CrowdyServerObjectPins::HasValuePinName(Output.Name), EGPD_Output) : nullptr;
		if ((!Pin || Pin->LinkedTo.Num() == 0) && (!HasValue || HasValue->LinkedTo.Num() == 0))
		{
			continue;
		}

		UK2Node_CallFunction* Get = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
		Get->FunctionReference.SetExternalMember(GET_FUNCTION_NAME_CHECKED(UCrowdyServerObjectLibrary, GetServerValue), UCrowdyServerObjectLibrary::StaticClass());
		Get->AllocateDefaultPins();
		UEdGraphPin* Values = Get->FindPin(TEXT("Values"));
		UEdGraphPin* Name = Get->FindPin(TEXT("Name"));
		UEdGraphPin* Value = Get->FindPin(TEXT("Value"));
		UEdGraphPin* Found = Get->GetReturnValuePin();
		if (!Pin || !Values || !Name || !Value || !Found)
		{
			CompilerContext.MessageLog.Error(*LOCTEXT("GetChanged", "@@ could not be expanded: refresh the node.").ToString(), this);
			return nullptr;
		}

		// The call's Outputs becomes a local the delegate writes before On Success runs, so each read sees this call's reply.
		Schema->TryCreateConnection(OutputsPin, Values);
		Name->DefaultValue = Output.Name.ToString();
		Value->PinType = Output.Type;
		MoveValueLinks(CompilerContext, *Pin, *Value);
		if (HasValue)
		{
			MoveValueLinks(CompilerContext, *HasValue, *Found);
		}
		Schema->TryCreateConnection(Then, Get->GetExecPin());
		Then = Get->GetThenPin();
	}
	return Then;
}

void UCrowdyK2Node_CallServerFunction::ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph)
{
	Super::ExpandNode(CompilerContext, SourceGraph);

	if (bPickFunction && !GetDefinition())
	{
		CompilerContext.MessageLog.Error(*LOCTEXT("NothingPicked", "@@ has no function yet. Pick a Server Function in its Function dropdown.").ToString(), this);
		BreakAllNodeLinks();
		return;
	}
	if (!CheckDefinition(CompilerContext))
	{
		BreakAllNodeLinks();
		return;
	}
	const FCrowdyServerFunction* Function = GetFunction();
	if (!Function)
	{
		CompilerContext.MessageLog.Error(*FText::Format(LOCTEXT("NoFunction", "@@: {0} has no Server Function {1}."), GetAssetName(), FText::FromName(Member)).ToString(), this);
		BreakAllNodeLinks();
		return;
	}
	TArray<FCrowdyServerValuePin> Inputs;
	TArray<FCrowdyServerValuePin> Outputs;
	GatherFunctionValues(Inputs, Outputs);
	bool bAllPins = true;
	for (const FCrowdyServerValuePin& Value : Inputs)
	{
		bAllPins &= CheckValue(CompilerContext, Value);
	}
	for (const FCrowdyServerValuePin& Value : Outputs)
	{
		bAllPins &= CheckValue(CompilerContext, Value);
	}
	if (!bAllPins)
	{
		BreakAllNodeLinks();
		return;
	}

	const UEdGraphSchema_K2* Schema = CompilerContext.GetSchema();
	UK2Node_CallFunction* Held = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
	Held->FunctionReference.SetExternalMember(GET_FUNCTION_NAME_CHECKED(UCrowdyServerObjectLibrary, GetHeldServerObject), UCrowdyServerObjectLibrary::StaticClass());
	Held->AllocateDefaultPins();

	UK2Node_AsyncAction* Call = CompilerContext.SpawnIntermediateNode<UK2Node_AsyncAction>(this, SourceGraph);
	Call->InitializeProxyFromFunction(UCrowdyServerTypedCallAction::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UCrowdyServerTypedCallAction, CallServerFunctionWithInputs)));
	Call->AllocateDefaultPins();

	UEdGraphPin* HeldTarget = Held->FindPin(TEXT("Target"));
	UEdGraphPin* HeldDefinition = Held->FindPin(TEXT("Definition"));
	UEdGraphPin* CallObject = Call->FindPin(TEXT("Object"));
	UEdGraphPin* CallFunction = Call->FindPin(TEXT("Function"));
	UEdGraphPin* CallInputs = Call->FindPin(TEXT("Inputs"));
	UEdGraphPin* CallInputError = Call->FindPin(TEXT("InputError"));
	UEdGraphPin* CallOutputs = Call->FindPin(TEXT("Outputs"));
	UEdGraphPin* CallSuccess = Call->FindPin(GET_MEMBER_NAME_CHECKED(UCrowdyServerCallAction, OnSuccess));
	UEdGraphPin* CallFailed = Call->FindPin(GET_MEMBER_NAME_CHECKED(UCrowdyServerCallAction, OnFailed));
	UEdGraphPin* CallOutcome = Call->FindPin(TEXT("Outcome"));
	UEdGraphPin* CallReason = Call->FindPin(TEXT("Reason"));
	UEdGraphPin* CallRetryable = Call->FindPin(TEXT("bRetryable"));
	if (!HeldTarget || !HeldDefinition || !Held->GetReturnValuePin() || !CallObject || !CallFunction || !CallInputs || !CallInputError || !CallOutputs || !CallSuccess
		|| !CallFailed || !CallOutcome || !CallReason || !CallRetryable)
	{
		CompilerContext.MessageLog.Error(*LOCTEXT("CallChanged", "@@ could not be expanded: Call Server Function has changed its parameters.").ToString(), this);
		BreakAllNodeLinks();
		return;
	}

	HeldDefinition->DefaultObject = GetDefinition();
	ExpandSource(CompilerContext, SourceGraph, {HeldTarget});
	Schema->TryCreateConnection(Held->GetReturnValuePin(), CallObject);
	CallFunction->DefaultValue = Member.ToString();

	UEdGraphPin* Execute = FindPinChecked(UEdGraphSchema_K2::PN_Execute);
	if (Function->GetParamsStruct())
	{
		UK2Node_CallFunction* Make = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
		Make->FunctionReference.SetExternalMember(GET_FUNCTION_NAME_CHECKED(UCrowdyServerObjectLibrary, MakeFunctionInputs), UCrowdyServerObjectLibrary::StaticClass());
		Make->AllocateDefaultPins();
		UEdGraphPin* MakeDefinition = Make->FindPin(TEXT("Definition"));
		UEdGraphPin* MakeFunction = Make->FindPin(TEXT("Function"));
		UEdGraphPin* Made = Make->GetReturnValuePin();
		if (!MakeDefinition || !MakeFunction || !Made)
		{
			CompilerContext.MessageLog.Error(*LOCTEXT("MakeChanged", "@@ could not be expanded: Make Function Inputs has changed its parameters.").ToString(), this);
			BreakAllNodeLinks();
			return;
		}
		MakeDefinition->DefaultObject = GetDefinition();
		MakeFunction->DefaultValue = Member.ToString();
		Schema->TryCreateConnection(Made, CallInputs);
		CompilerContext.MovePinLinksToIntermediate(*Execute, *Make->GetExecPin());
		UEdGraphPin* InputsThen = ExpandInputs(CompilerContext, SourceGraph, Inputs, Made, CallInputError);
		if (!InputsThen)
		{
			BreakAllNodeLinks();
			return;
		}
		Schema->TryCreateConnection(InputsThen, Call->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
	}
	else
	{
		CompilerContext.MovePinLinksToIntermediate(*Execute, *Call->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
	}

	CompilerContext.MovePinLinksToIntermediate(*FindPinChecked(UEdGraphSchema_K2::PN_Then), *Call->FindPinChecked(UEdGraphSchema_K2::PN_Then));
	CompilerContext.MovePinLinksToIntermediate(*FindPinChecked(CrowdyServerCallPins::OnFailed), *CallFailed);
	CompilerContext.MovePinLinksToIntermediate(*FindPinChecked(CrowdyServerCallPins::Outcome), *CallOutcome);
	CompilerContext.MovePinLinksToIntermediate(*FindPinChecked(CrowdyServerCallPins::Reason), *CallReason);
	CompilerContext.MovePinLinksToIntermediate(*FindPinChecked(CrowdyServerCallPins::Retryable), *CallRetryable);
	if (UEdGraphPin* SuccessThen = ExpandOutputs(CompilerContext, SourceGraph, Outputs, CallSuccess, CallOutputs))
	{
		CompilerContext.MovePinLinksToIntermediate(*FindPinChecked(CrowdyServerCallPins::OnSuccess), *SuccessThen);
	}
	BreakAllNodeLinks();
}

#undef LOCTEXT_NAMESPACE
