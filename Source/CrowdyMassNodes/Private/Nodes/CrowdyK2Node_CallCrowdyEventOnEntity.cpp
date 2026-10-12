#include "Nodes/CrowdyK2Node_CallCrowdyEventOnEntity.h"

#include "BlueprintActionDatabaseRegistrar.h"
#include "BlueprintNodeSpawner.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphUtilities.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "KismetCompiler.h"
#include "KismetCompilerMisc.h"
#include "Nodes/CrowdyMassEventCatalog.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Styling/AppStyle.h"
#include "Textures/SlateIcon.h"
#include "TimerManager.h"
#include "UObject/Package.h"
#include "UObject/TopLevelAssetPath.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

#define LOCTEXT_NAMESPACE "CrowdyK2Node_CallCrowdyEventOnEntity"

namespace CrowdyMassCallEventPins
{
	const FName Target(TEXT("Crowdy Mass Target"));
	const FName EventClass(TEXT("Crowdy Mass Event Class"));
	const FName EventName(TEXT("Crowdy Mass Event Name"));
	const FName EventSignature(TEXT("Crowdy Mass Event Signature"));
	const FName EventPicker(TEXT("Crowdy Mass Event"));
}

namespace CrowdyMassCallEventNodePrivate
{
	// The runtime entry the node compiles to. Looked up by name in the CrowdyMass module so the editor
	// module does not have to name the library class that declares it.
	const TCHAR* SendFunctionName = TEXT("CrowdyMassCallEventOnEntity");
	const TCHAR* MassModulePackage = TEXT("/Script/CrowdyMass");
	const TCHAR* EntityTargetStructName = TEXT("CrowdyMassEntityTarget");

	// True for a parameter that travels with the call. Uses the transport's own predicate so the node's
	// pin set can never disagree with the parameters the send path actually serializes.
	bool IsCallInputParameter(const FProperty* Property)
	{
		return Property
			&& Property->HasAnyPropertyFlags(CPF_Parm)
			&& !FCrowdyRPC::IsTrueOutputParam(Property);
	}
}

UFunction* UCrowdyK2Node_CallCrowdyEventOnEntity::ResolveSendFunction()
{
	using namespace CrowdyMassCallEventNodePrivate;

	static TWeakObjectPtr<UFunction> Cached;
	if (UFunction* Live = Cached.Get())
	{
		return Live;
	}

	UPackage* MassPackage = FindPackage(nullptr, MassModulePackage);
	if (!MassPackage)
	{
		return nullptr;
	}

	const FName WantedName(SendFunctionName);
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		if (Class->GetOuter() != MassPackage)
		{
			continue;
		}

		UFunction* Function = Class->FindFunctionByName(WantedName, EIncludeSuperFlag::ExcludeSuper);
		if (Function && Function->HasAnyFunctionFlags(FUNC_Static))
		{
			Cached = Function;
			return Function;
		}
	}

	return nullptr;
}

UScriptStruct* UCrowdyK2Node_CallCrowdyEventOnEntity::ResolveEntityTargetStruct()
{
	static TWeakObjectPtr<UScriptStruct> Cached;
	if (UScriptStruct* Live = Cached.Get())
	{
		return Live;
	}

	// Braced: parenthesised, both arguments read as declarations and the whole line parses as a function
	// declaration rather than a variable.
	const FTopLevelAssetPath StructPath{
		FName(CrowdyMassCallEventNodePrivate::MassModulePackage),
		FName(CrowdyMassCallEventNodePrivate::EntityTargetStructName) };

	UScriptStruct* Found = FindObject<UScriptStruct>(StructPath);
	Cached = Found;
	return Found;
}

UFunction* UCrowdyK2Node_CallCrowdyEventOnEntity::GetPickedEvent() const
{
	if (EventReference.GetMemberName().IsNone())
	{
		return nullptr;
	}
	UFunction* Resolved = EventReference.ResolveMember<UFunction>(GetBlueprintClassFromNode());
	if (!Resolved)
	{
		return nullptr;
	}

	// ResolveMember answers from the most up-to-date class, which for a Blueprint is the SKELETON. The
	// event markers are stamped on the generated class only, and ComputeFunctionID hashes the owner's
	// path name, so a skeleton function both fails the "is this a Crowdy event" check and produces an id
	// no receiver can match. Read the same function off the authoritative class instead.
	UClass* Owner = Resolved->GetOwnerClass();
	UClass* Authoritative = Owner ? Owner->GetAuthoritativeClass() : nullptr;
	if (Authoritative && Authoritative != Owner)
	{
		if (UFunction* OnAuthoritative = Authoritative->FindFunctionByName(Resolved->GetFName()))
		{
			return OnAuthoritative;
		}
	}

	return Resolved;
}

UClass* UCrowdyK2Node_CallCrowdyEventOnEntity::GetPickedEventClass() const
{
	const UFunction* Event = GetPickedEvent();
	UClass* Owner = Event ? Event->GetOwnerClass() : nullptr;
	return Owner ? Owner->GetAuthoritativeClass() : nullptr;
}

FString UCrowdyK2Node_CallCrowdyEventOnEntity::GetPickedEventDisplayName() const
{
	const FName MemberName = EventReference.GetMemberName();
	return MemberName.IsNone() ? FString() : CrowdyMassEventCatalog::MakeDisplayName(MemberName);
}

void UCrowdyK2Node_CallCrowdyEventOnEntity::SetPickedEvent(UClass* DeclaringClass, FName FunctionName)
{
	if (!DeclaringClass || FunctionName.IsNone())
	{
		return;
	}

	if (EventReference.GetMemberName() == FunctionName && GetPickedEventClass() == DeclaringClass)
	{
		return;
	}

	Modify();

	// Always an external reference, never a self reference, even when the picked event happens to be
	// declared on the Blueprint being edited: the call addresses another entity, so the node has to keep
	// meaning that class after it is copied into a different Blueprint. The GUID is what makes a rename
	// redirect; without it the stored name would simply stop matching anything.
	FGuid MemberGuid;
	UBlueprint::GetGuidFromClassByFieldName<UFunction>(DeclaringClass, FunctionName, MemberGuid);
	EventReference.SetExternalMember(FunctionName, DeclaringClass, MemberGuid);
	RefreshEventDerivedPins();

	// The pin set is grown from the picked event's signature, so it has to be rebuilt. Rebuilding here
	// would free the pins the dropdown widget is still holding, so it waits for the next tick.
	ScheduleReconstruct();
}

void UCrowdyK2Node_CallCrowdyEventOnEntity::ClearPickedEvent()
{
	if (EventReference.GetMemberName().IsNone())
	{
		return;
	}

	Modify();
	EventReference = FMemberReference();
	RefreshEventDerivedPins();

	ScheduleReconstruct();
}

void UCrowdyK2Node_CallCrowdyEventOnEntity::ScheduleReconstruct()
{
	if (bReconstructPending || !GEditor)
	{
		return;
	}
	bReconstructPending = true;

	GEditor->GetTimerManager()->SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this]()
	{
		bReconstructPending = false;
		ReconstructNode();
		if (UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForNode(this))
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		}
	}));
}

void UCrowdyK2Node_CallCrowdyEventOnEntity::AllocateDefaultPins()
{
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();

	CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Execute);
	CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Then);

	// The authoring surface. It carries the picked event's name for display only; the member reference
	// on the node is what the call is built from, so a rename never has to be typed back in here.
	UEdGraphPin* PickerPin = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Name, CrowdyMassCallEventPins::EventPicker);
	PickerPin->PinFriendlyName = LOCTEXT("EventPinLabel", "Event");
	PickerPin->PinToolTip = LOCTEXT("EventPinTooltip",
		"The Crowdy event to send. The list holds every event of every entity class the Mass rendering backend is configured for, plus the SDK's own events under \"Any Crowd Entity\", which can be sent to a crowd entity of any class.").ToString();
	PickerPin->bNotConnectable = true;

	if (UScriptStruct* TargetStruct = ResolveEntityTargetStruct())
	{
		UEdGraphPin* TargetPin =
			CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Struct, TargetStruct, CrowdyMassCallEventPins::Target);
		TargetPin->PinFriendlyName = LOCTEXT("TargetPinLabel", "Target");
		TargetPin->PinToolTip = LOCTEXT("TargetPinTooltip",
			"The entity to send to. A stored target does not survive the entity being respawned.").ToString();
	}

	// The picked event travels as its declaring class plus its name, which is how the receiving client
	// resolves it. Both are written from the member reference, never read back out of these pins.
	UEdGraphPin* ClassPin =
		CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Class, UObject::StaticClass(), CrowdyMassCallEventPins::EventClass);
	ClassPin->bHidden = true;
	ClassPin->bNotConnectable = true;

	UEdGraphPin* NamePin = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Name, CrowdyMassCallEventPins::EventName);
	NamePin->bHidden = true;
	NamePin->bNotConnectable = true;

	// The signature the parameter terms below were compiled against. The class and the name alone let the
	// receiving side look up a DIFFERENT function than this graph was compiled for, if the picked event is
	// later reshaped and this graph is not recompiled. The terms in the bytecode would still be the old
	// ones, so reading them back against the new signature reads the wrong number of them, or the right
	// number at the wrong sizes. Sending the signature makes that disagreement detectable instead.
	UEdGraphPin* SignaturePin =
		CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Int64, CrowdyMassCallEventPins::EventSignature);
	SignaturePin->bHidden = true;
	SignaturePin->bNotConnectable = true;

	if (const UFunction* Event = GetPickedEvent())
	{
		for (TFieldIterator<FProperty> It(Event); It; ++It)
		{
			const FProperty* Param = *It;
			if (!CrowdyMassCallEventNodePrivate::IsCallInputParameter(Param))
			{
				continue;
			}

			FEdGraphPinType PinType;
			if (!Schema || !Schema->ConvertPropertyToPinType(Param, PinType))
			{
				continue;
			}

			// Grown as a by-value input even where the parameter is declared as a const reference: the
			// call passes the value, and a reference pin left unwired has no value to pass.
			PinType.bIsReference = false;
			PinType.bIsConst = false;

			UEdGraphPin* ParamPin = CreatePin(EGPD_Input, PinType, Param->GetFName());

			const FString& DisplayNameOverride = Param->GetMetaData(TEXT("DisplayName"));
			ParamPin->PinFriendlyName = DisplayNameOverride.IsEmpty()
				? FText::FromString(FName::NameToDisplayString(Param->GetName(), CastField<FBoolProperty>(Param) != nullptr))
				: FText::AsCultureInvariant(DisplayNameOverride);

			FString ParamDefault;
			if (UEdGraphSchema_K2::FindFunctionParameterDefaultValue(Event, Param, ParamDefault))
			{
				Schema->SetPinAutogeneratedDefaultValue(ParamPin, ParamDefault);
			}
			else
			{
				Schema->SetPinAutogeneratedDefaultValueBasedOnType(ParamPin);
			}

			// An array, set or map has no literal form the call can carry, so it has to come from a wire.
			// Marking the default ignored keeps the node from offering an entry field that cannot compile.
			ParamPin->bDefaultValueIsIgnored = PinType.IsContainer();
		}
	}

	RefreshEventDerivedPins();

	Super::AllocateDefaultPins();
}

void UCrowdyK2Node_CallCrowdyEventOnEntity::PostReconstructNode()
{
	Super::PostReconstructNode();

	// A rebuild carries each old pin's value onto the new pin of the same name, which would put back the
	// name the event had before it was renamed. Re-deriving here, after that copy, is what keeps the call
	// addressed at the event the member reference now resolves to.
	RefreshEventDerivedPins();
}

void UCrowdyK2Node_CallCrowdyEventOnEntity::RefreshEventDerivedPins()
{
	const UFunction* Event = GetPickedEvent();
	UClass* DeclaringClass = GetPickedEventClass();

	if (UEdGraphPin* PickerPin = FindPin(CrowdyMassCallEventPins::EventPicker, EGPD_Input))
	{
		PickerPin->DefaultValue = GetPickedEventDisplayName();
	}

	if (UEdGraphPin* ClassPin = FindPin(CrowdyMassCallEventPins::EventClass, EGPD_Input))
	{
		ClassPin->DefaultObject = DeclaringClass;
	}

	if (UEdGraphPin* NamePin = FindPin(CrowdyMassCallEventPins::EventName, EGPD_Input))
	{
		NamePin->DefaultValue = Event ? Event->GetName() : FString();
	}

	if (UEdGraphPin* SignaturePin = FindPin(CrowdyMassCallEventPins::EventSignature, EGPD_Input))
	{
		SignaturePin->DefaultValue = LexToString(Event ? FCrowdyRPC::ComputeFunctionID(Event) : 0);
	}
}

bool UCrowdyK2Node_CallCrowdyEventOnEntity::CollectCallPins(TArray<UEdGraphPin*>& OutPins, FString& OutProblem) const
{
	OutPins.Reset();
	OutProblem.Reset();

	UEdGraphPin* TargetPin = GetTargetPin();
	UEdGraphPin* ClassPin = FindPin(CrowdyMassCallEventPins::EventClass, EGPD_Input);
	UEdGraphPin* NamePin = FindPin(CrowdyMassCallEventPins::EventName, EGPD_Input);
	UEdGraphPin* SignaturePin = FindPin(CrowdyMassCallEventPins::EventSignature, EGPD_Input);

	if (!TargetPin || !ClassPin || !NamePin || !SignaturePin)
	{
		OutProblem = TEXT("the node is missing one of its own pins; refresh the node");
		return false;
	}

	OutPins.Add(TargetPin);
	OutPins.Add(ClassPin);
	OutPins.Add(NamePin);
	OutPins.Add(SignaturePin);

	const UFunction* Event = GetPickedEvent();
	if (!Event)
	{
		OutProblem = TEXT("no event is picked");
		return false;
	}

	// Declaration order, because that is the order the terms are read back in.
	for (TFieldIterator<FProperty> It(Event); It; ++It)
	{
		const FProperty* Param = *It;
		if (!CrowdyMassCallEventNodePrivate::IsCallInputParameter(Param))
		{
			continue;
		}

		UEdGraphPin* ParamPin = FindPin(Param->GetFName(), EGPD_Input);
		if (!ParamPin)
		{
			OutProblem = FString::Printf(
				TEXT("the pin for parameter '%s' is missing; refresh the node"), *Param->GetName());
			return false;
		}
		OutPins.Add(ParamPin);
	}

	return true;
}

UEdGraphPin* UCrowdyK2Node_CallCrowdyEventOnEntity::GetThenPin() const
{
	return FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
}

UEdGraphPin* UCrowdyK2Node_CallCrowdyEventOnEntity::GetTargetPin() const
{
	return FindPin(CrowdyMassCallEventPins::Target, EGPD_Input);
}

FText UCrowdyK2Node_CallCrowdyEventOnEntity::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	const FString Picked = GetPickedEventDisplayName();
	if (TitleType == ENodeTitleType::MenuTitle || TitleType == ENodeTitleType::ListView || Picked.IsEmpty())
	{
		return LOCTEXT("NodeTitle", "Call Crowdy Event on Entity");
	}

	const UClass* DeclaringClass = GetPickedEventClass();
	if (!DeclaringClass)
	{
		return FText::Format(LOCTEXT("NodeTitleUnresolved", "Call {0} on Entity (missing)"), FText::FromString(Picked));
	}

	FString ClassName = DeclaringClass->GetName();
	ClassName.RemoveFromEnd(TEXT("_C"), ESearchCase::CaseSensitive);

	return FText::Format(LOCTEXT("NodeTitlePicked", "Call {0} on Entity\n{1}"),
		FText::FromString(Picked), FText::FromString(ClassName));
}

FText UCrowdyK2Node_CallCrowdyEventOnEntity::GetTooltipText() const
{
	return LOCTEXT("NodeTooltip",
		"Sends a Crowdy event to one Mass entity.\n\n"
		"Pick the event on the node: the list holds every Crowdy event of every entity class the Mass rendering "
		"backend is configured for, plus the SDK's own events under \"Any Crowd Entity\", and picking one picks "
		"its class too. The node then grows a pin per parameter of that event.\n\n"
		"For a plain one-shot animation there is a shorter road that needs no event at all: Send Crowdy Action, "
		"which takes the action by name.\n\n"
		"The call is addressed by the picked class; whether the entity plugged into Target can receive it is "
		"decided when it is sent, from the entity itself.");
}

FSlateIcon UCrowdyK2Node_CallCrowdyEventOnEntity::GetIconAndTint(FLinearColor& OutColor) const
{
	OutColor = GetNodeTitleColor();
	static FSlateIcon Icon(FAppStyle::GetAppStyleSetName(), "GraphEditor.Event_16x");
	return Icon;
}

FText UCrowdyK2Node_CallCrowdyEventOnEntity::GetMenuCategory() const
{
	return LOCTEXT("MenuCategory", "Crowdy Mass|Events");
}

void UCrowdyK2Node_CallCrowdyEventOnEntity::GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const
{
	UClass* ActionKey = GetClass();
	if (!ActionRegistrar.IsOpenForRegistration(ActionKey))
	{
		return;
	}

	// One action, not one per event: the event is picked on the placed node, so the menu offers the node
	// itself and stays the same size however many entity classes a project configures.
	UBlueprintNodeSpawner* Spawner = UBlueprintNodeSpawner::Create(GetClass());
	check(Spawner);

	ActionRegistrar.AddBlueprintAction(ActionKey, Spawner);
}

void UCrowdyK2Node_CallCrowdyEventOnEntity::ValidateNodeDuringCompilation(FCompilerResultsLog& MessageLog) const
{
	Super::ValidateNodeDuringCompilation(MessageLog);

	if (!ResolveEntityTargetStruct())
	{
		MessageLog.Error(*LOCTEXT("MissingTargetStruct",
			"@@ cannot compile: the Crowdy Mass entity target type is not available in this build.").ToString(), this);
		return;
	}

	if (!ResolveSendFunction())
	{
		MessageLog.Error(*LOCTEXT("MissingSendEntry",
			"@@ cannot compile: the Crowdy Mass runtime does not provide the entry this node calls.").ToString(), this);
		return;
	}

	const FName MemberName = EventReference.GetMemberName();
	if (MemberName.IsNone())
	{
		MessageLog.Error(*LOCTEXT("NoEventPicked",
			"@@ has no event picked. Choose one from the node's Event dropdown.").ToString(), this);
		return;
	}

	const UClass* StoredClass = EventReference.GetMemberParentClass();
	if (!StoredClass)
	{
		MessageLog.Error(*FText::Format(LOCTEXT("MissingEventClass",
			"@@ refers to event '{0}', but the class that declared it no longer exists. Pick the event again."),
			FText::FromName(MemberName)).ToString(), this);
		return;
	}

	const UFunction* Event = GetPickedEvent();
	if (!Event)
	{
		MessageLog.Error(*FText::Format(LOCTEXT("MissingEventFunction",
			"@@ refers to event '{0}' on '{1}', which no longer exists. Pick the event again."),
			FText::FromName(MemberName), FText::FromString(StoredClass->GetName())).ToString(), this);
		return;
	}

	// The event can stop being sendable without disappearing: its marker can be removed, or a parameter
	// changed to a type the transport cannot carry. Say which, rather than failing on a missing pin later.
	const FString CallProblem = CrowdyMassEventCatalog::DescribeCallProblem(Event);
	if (!CallProblem.IsEmpty())
	{
		MessageLog.Error(*FText::Format(LOCTEXT("EventNotSendable",
			"@@ refers to '{0}', which {1}."),
			FText::FromString(Event->GetName()), FText::FromString(CallProblem)).ToString(), this);
		return;
	}

	const UEdGraphPin* TargetPin = GetTargetPin();
	if (!TargetPin || TargetPin->LinkedTo.Num() == 0)
	{
		MessageLog.Error(*LOCTEXT("TargetUnwired",
			"@@ has nothing wired into Target. A Mass entity target has no default; it must come from a trace or a stored target.").ToString(), this);
	}

	TArray<UEdGraphPin*> CallPins;
	FString PinProblem;
	if (!CollectCallPins(CallPins, PinProblem))
	{
		MessageLog.Error(*FText::Format(LOCTEXT("PinsOutOfSync", "@@ cannot be called: {0}."),
			FText::FromString(PinProblem)).ToString(), this);
		return;
	}

	for (const UEdGraphPin* Pin : CallPins)
	{
		if (Pin && Pin->PinType.IsContainer() && Pin->LinkedTo.Num() == 0)
		{
			MessageLog.Error(*FText::Format(LOCTEXT("ContainerPinUnwired",
				"@@ needs a connection on '{0}'. An array, set or map input has no literal form the call can carry."),
				Pin->PinFriendlyName.IsEmpty() ? FText::FromName(Pin->PinName) : Pin->PinFriendlyName).ToString(), this);
		}
	}
}

void UCrowdyK2Node_CallCrowdyEventOnEntity::ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph)
{
	Super::ExpandNode(CompilerContext, SourceGraph);

	// Re-derived here rather than trusted from the last rebuild, so a rename the member reference
	// redirected since then reaches the call instead of the name that was current when it was picked.
	RefreshEventDerivedPins();

	// The Event dropdown is this node's own authoring surface and matches no argument of the call, so it
	// is dropped before the call is emitted.
	if (UEdGraphPin* PickerPin = FindPin(CrowdyMassCallEventPins::EventPicker, EGPD_Input))
	{
		RemovePin(PickerPin);
	}
}

/**
 * Emits the call directly instead of leaning on the stock call-function handler.
 *
 * The runtime entry takes the target, the event's class and the event's name, and then every parameter
 * of the picked event as extra variadic arguments. The stock handler rejects a variadic argument that is
 * not connected, which would make an authored literal on a parameter pin fail to compile; the base
 * functor's net registration already turns such a pin into a literal term, and that is exactly the term
 * list the bytecode writer expects, so the statement is assembled here from the node's own pin order.
 */
class FCrowdyMassCallEventOnEntityHandler : public FNodeHandlingFunctor
{
public:
	explicit FCrowdyMassCallEventOnEntityHandler(FKismetCompilerContext& InCompilerContext)
		: FNodeHandlingFunctor(InCompilerContext)
	{
	}

	/**
	 * Nets are registered for every node before any node is compiled, and in graph order rather than in
	 * the order values flow, so a wired input can be reached here before the node that produces it has
	 * registered its output. The base implementation treats that as an internal compiler error; a local
	 * term is created instead, exactly as an ordinary call node does, and the producer's own term is the
	 * one the call ends up reading once every node has been registered.
	 */
	virtual void RegisterNet(FKismetFunctionContext& Context, UEdGraphPin* Net) override
	{
		FBPTerminal* Term =
			Context.CreateLocalTerminalFromPinAutoChooseScope(Net, Context.NetNameMap->MakeValidName(Net));
		Context.NetMap.Add(Net, Term);
	}

	virtual void Compile(FKismetFunctionContext& Context, UEdGraphNode* Node) override
	{
		UCrowdyK2Node_CallCrowdyEventOnEntity* EventNode = CastChecked<UCrowdyK2Node_CallCrowdyEventOnEntity>(Node);

		UFunction* SendFunction = UCrowdyK2Node_CallCrowdyEventOnEntity::ResolveSendFunction();
		if (!SendFunction)
		{
			CompilerContext.MessageLog.Error(
				*LOCTEXT("HandlerMissingEntry", "@@ cannot compile: the Crowdy Mass runtime entry it calls is missing.").ToString(),
				Node);
			return;
		}

		TArray<UEdGraphPin*> CallPins;
		FString PinProblem;
		if (!EventNode->CollectCallPins(CallPins, PinProblem))
		{
			CompilerContext.MessageLog.Error(
				*FText::Format(LOCTEXT("HandlerPinProblem", "@@ cannot compile: {0}."),
					FText::FromString(PinProblem)).ToString(),
				Node);
			return;
		}

		TArray<FBPTerminal*> Arguments;
		Arguments.Reserve(CallPins.Num());

		const UFunction* PickedEvent = EventNode->GetPickedEvent();

		for (UEdGraphPin* Pin : CallPins)
		{
			// Computed here, from the same function the parameter pins were grown from, rather than read
			// back from the pin. A default written at some earlier reconstruct can disagree with what this
			// call actually carries, and the receiving guard would then refuse every call.
			if (Pin->PinName == CrowdyMassCallEventPins::EventSignature && PickedEvent)
			{
				FBPTerminal* SignatureTerm = Context.RegisterLiteral(Pin);
				SignatureTerm->Name = LexToString(FCrowdyRPC::ComputeFunctionID(PickedEvent));
				Arguments.Add(SignatureTerm);
				continue;
			}

			UEdGraphPin* Net = FEdGraphUtilities::GetNetFromPin(Pin);
			FBPTerminal** Term = Net ? Context.NetMap.Find(Net) : nullptr;
			if (!Term || !*Term)
			{
				CompilerContext.MessageLog.Error(
					*LOCTEXT("HandlerUnresolvedTerm", "@@ could not resolve the value passed into @@.").ToString(),
					Node, Pin);
				return;
			}
			Arguments.Add(*Term);
		}

		FBlueprintCompiledStatement& Statement = Context.AppendStatementForNode(Node);
		Statement.Type = KCST_CallFunction;
		Statement.FunctionToCall = SendFunction;

		// Static entry, so there is no calling context and no destination: the entry reports whether the
		// send was routed, which this node does not expose.
		Statement.FunctionContext = nullptr;
		Statement.bIsParentContext = false;
		Statement.LHS = nullptr;
		Statement.RHS = Arguments;

		GenerateSimpleThenGoto(Context, *Node, EventNode->GetThenPin());
	}
};

FNodeHandlingFunctor* UCrowdyK2Node_CallCrowdyEventOnEntity::CreateNodeHandler(
	FKismetCompilerContext& CompilerContext) const
{
	return new FCrowdyMassCallEventOnEntityHandler(CompilerContext);
}

#undef LOCTEXT_NAMESPACE
