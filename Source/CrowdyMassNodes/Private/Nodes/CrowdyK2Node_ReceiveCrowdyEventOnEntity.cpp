#include "Nodes/CrowdyK2Node_ReceiveCrowdyEventOnEntity.h"

#include "BlueprintActionDatabaseRegistrar.h"
#include "BlueprintNodeSpawner.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_Self.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "KismetCompiler.h"
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

#define LOCTEXT_NAMESPACE "CrowdyK2Node_ReceiveCrowdyEventOnEntity"

namespace CrowdyMassReceiveEventPins
{
	const FName Bind(TEXT("execute"));
	const FName Unbind(TEXT("Crowdy Mass Unbind"));
	const FName Then(TEXT("Crowdy Mass Then"));
	const FName OnEvent(TEXT("Crowdy Mass On Event"));

	// No space: this one becomes a parameter of the intermediate event, so it has to be spelled the way a
	// parameter is. IsReservedParameterName is what keeps an event from declaring one by the same name.
	const FName Target(TEXT("CrowdyMassTarget"));

	const FName EventPicker(TEXT("Crowdy Mass Event"));
}

namespace CrowdyMassReceiveEventNodePrivate
{
	// The runtime entries the node compiles to. Looked up by name in the CrowdyMass module so the editor
	// module does not have to name the library class that declares them.
	const TCHAR* BindFunctionName = TEXT("CrowdyMassBindEventOnEntity");
	const TCHAR* UnbindFunctionName = TEXT("CrowdyMassUnbindEventOnEntity");
	const TCHAR* MassModulePackage = TEXT("/Script/CrowdyMass");
	const TCHAR* EntityTargetStructName = TEXT("CrowdyMassEntityTarget");

	// The parameters of the runtime entries, by the names they are declared with.
	const TCHAR* HandlerObjectParam = TEXT("HandlerObject");
	const TCHAR* HandlerFunctionNameParam = TEXT("HandlerFunctionName");
	const TCHAR* EventClassParam = TEXT("EventClass");
	const TCHAR* EventNameParam = TEXT("EventName");
	const TCHAR* ExpectedSignatureParam = TEXT("ExpectedSignature");

	// True for a parameter that travels with the call, and therefore has a value to hand a receiver. Uses
	// the transport's own predicate so the node's pin set can never disagree with the parameters the send
	// path actually serializes.
	bool IsCallInputParameter(const FProperty* Property)
	{
		return Property
			&& Property->HasAnyPropertyFlags(CPF_Parm)
			&& !FCrowdyRPC::IsTrueOutputParam(Property);
	}

	// A static entry of the given name declared by some class in the CrowdyMass module.
	UFunction* ResolveMassModuleFunction(const TCHAR* FunctionName)
	{
		UPackage* MassPackage = FindPackage(nullptr, MassModulePackage);
		if (!MassPackage)
		{
			return nullptr;
		}

		const FName WantedName(FunctionName);
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
				return Function;
			}
		}

		return nullptr;
	}

	// The pin type a receiver gets for one of the event's parameters: the parameter's own type, by value.
	// A reference pin on an output would be a promise this node cannot keep, since the value it hands over
	// lives only for the length of the call.
	bool BuildParameterPinType(const UEdGraphSchema_K2* Schema, const FProperty* Param, FEdGraphPinType& OutPinType)
	{
		if (!Schema || !Schema->ConvertPropertyToPinType(Param, OutPinType))
		{
			return false;
		}

		OutPinType.bIsReference = false;
		OutPinType.bIsConst = false;
		return true;
	}

	FText MakeParameterFriendlyName(const FProperty* Param)
	{
		const FString& DisplayNameOverride = Param->GetMetaData(TEXT("DisplayName"));
		return DisplayNameOverride.IsEmpty()
			? FText::FromString(FName::NameToDisplayString(Param->GetName(), CastField<FBoolProperty>(Param) != nullptr))
			: FText::AsCultureInvariant(DisplayNameOverride);
	}
}

UFunction* UCrowdyK2Node_ReceiveCrowdyEventOnEntity::ResolveBindFunction()
{
	using namespace CrowdyMassReceiveEventNodePrivate;

	static TWeakObjectPtr<UFunction> Cached;
	if (UFunction* Live = Cached.Get())
	{
		return Live;
	}

	UFunction* Found = ResolveMassModuleFunction(BindFunctionName);
	Cached = Found;
	return Found;
}

UFunction* UCrowdyK2Node_ReceiveCrowdyEventOnEntity::ResolveUnbindFunction()
{
	using namespace CrowdyMassReceiveEventNodePrivate;

	static TWeakObjectPtr<UFunction> Cached;
	if (UFunction* Live = Cached.Get())
	{
		return Live;
	}

	UFunction* Found = ResolveMassModuleFunction(UnbindFunctionName);
	Cached = Found;
	return Found;
}

UScriptStruct* UCrowdyK2Node_ReceiveCrowdyEventOnEntity::ResolveEntityTargetStruct()
{
	static TWeakObjectPtr<UScriptStruct> Cached;
	if (UScriptStruct* Live = Cached.Get())
	{
		return Live;
	}

	// Braced: parenthesised, both arguments read as declarations and the whole line parses as a function
	// declaration rather than a variable.
	const FTopLevelAssetPath StructPath{
		FName(CrowdyMassReceiveEventNodePrivate::MassModulePackage),
		FName(CrowdyMassReceiveEventNodePrivate::EntityTargetStructName) };

	UScriptStruct* Found = FindObject<UScriptStruct>(StructPath);
	Cached = Found;
	return Found;
}

bool UCrowdyK2Node_ReceiveCrowdyEventOnEntity::IsReservedParameterName(const FName ParameterName)
{
	return ParameterName == CrowdyMassReceiveEventPins::Target
		|| ParameterName == UEdGraphSchema_K2::PN_Then
		|| ParameterName == UK2Node_Event::DelegateOutputName;
}

UFunction* UCrowdyK2Node_ReceiveCrowdyEventOnEntity::GetPickedEvent() const
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
	// no sender can match. Read the same function off the authoritative class instead.
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

UClass* UCrowdyK2Node_ReceiveCrowdyEventOnEntity::GetPickedEventClass() const
{
	const UFunction* Event = GetPickedEvent();
	UClass* Owner = Event ? Event->GetOwnerClass() : nullptr;
	return Owner ? Owner->GetAuthoritativeClass() : nullptr;
}

FString UCrowdyK2Node_ReceiveCrowdyEventOnEntity::GetPickedEventDisplayName() const
{
	const FName MemberName = EventReference.GetMemberName();
	return MemberName.IsNone() ? FString() : CrowdyMassEventCatalog::MakeDisplayName(MemberName);
}

void UCrowdyK2Node_ReceiveCrowdyEventOnEntity::SetPickedEvent(UClass* DeclaringClass, const FName FunctionName)
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
	// declared on the Blueprint being edited: the event is declared by the class the observed entity is a
	// representation of, so the node has to keep meaning that class after it is copied into a different
	// Blueprint. The GUID is what makes a rename redirect; without it the stored name would simply stop
	// matching anything.
	FGuid MemberGuid;
	UBlueprint::GetGuidFromClassByFieldName<UFunction>(DeclaringClass, FunctionName, MemberGuid);
	EventReference.SetExternalMember(FunctionName, DeclaringClass, MemberGuid);
	RefreshEventDerivedPins();

	// The pin set is grown from the picked event's signature, so it has to be rebuilt. Rebuilding here
	// would free the pins the dropdown widget is still holding, so it waits for the next tick.
	ScheduleReconstruct();
}

void UCrowdyK2Node_ReceiveCrowdyEventOnEntity::ClearPickedEvent()
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

void UCrowdyK2Node_ReceiveCrowdyEventOnEntity::ScheduleReconstruct()
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

void UCrowdyK2Node_ReceiveCrowdyEventOnEntity::AllocateDefaultPins()
{
	using namespace CrowdyMassReceiveEventNodePrivate;

	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();

	UEdGraphPin* BindPin = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Exec, CrowdyMassReceiveEventPins::Bind);
	BindPin->PinFriendlyName = LOCTEXT("BindPinLabel", "Bind");
	BindPin->PinToolTip = LOCTEXT("BindPinTooltip",
		"Start listening for this event on crowd entities. The binding belongs to this object and stops when it is destroyed.").ToString();

	UEdGraphPin* UnbindPin = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Exec, CrowdyMassReceiveEventPins::Unbind);
	UnbindPin->PinFriendlyName = LOCTEXT("UnbindPinLabel", "Unbind");
	UnbindPin->PinToolTip = LOCTEXT("UnbindPinTooltip",
		"Stop listening. Doing nothing is also fine: the binding is dropped when this object or its world goes.").ToString();

	UEdGraphPin* ThenPin = CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, CrowdyMassReceiveEventPins::Then);
	ThenPin->PinFriendlyName = LOCTEXT("ThenPinLabel", "Then");
	ThenPin->PinToolTip = LOCTEXT("ThenPinTooltip",
		"Continues as soon as the bind or unbind is done, so several of these can be chained.").ToString();

	UEdGraphPin* OnEventPin = CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, CrowdyMassReceiveEventPins::OnEvent);
	OnEventPin->PinFriendlyName = LOCTEXT("OnEventPinLabel", "On Event");
	OnEventPin->PinToolTip = LOCTEXT("OnEventPinTooltip",
		"Runs once for every arriving event, on the game thread. Every value it carries came from the observed client and can be anything, so use it to decide what is drawn and never what is true.").ToString();

	// The authoring surface. It carries the picked event's name for display only; the member reference on
	// the node is what the binding is built from, so a rename never has to be typed back in here.
	UEdGraphPin* PickerPin = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Name, CrowdyMassReceiveEventPins::EventPicker);
	PickerPin->PinFriendlyName = LOCTEXT("EventPinLabel", "Event");
	PickerPin->PinToolTip = LOCTEXT("EventPinTooltip",
		"The Crowdy event to listen for. The list holds every event of every entity class the Mass rendering backend is configured for. It does not hold the events the crowd plane applies itself, such as Play Crowdy Action: those run their own body on every observer and hand nothing to a graph.").ToString();
	PickerPin->bNotConnectable = true;

	if (UScriptStruct* TargetStruct = ResolveEntityTargetStruct())
	{
		UEdGraphPin* TargetPin =
			CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Struct, TargetStruct, CrowdyMassReceiveEventPins::Target);
		TargetPin->PinFriendlyName = LOCTEXT("TargetPinLabel", "Target");
		TargetPin->PinToolTip = LOCTEXT("TargetPinTooltip",
			"The entity the event arrived for. Valid for this call; it does not survive that entity being respawned.").ToString();
	}

	if (const UFunction* Event = GetPickedEvent())
	{
		// Declaration order, matching the pin order the call node grows for the same event, so the two read
		// as two halves of one thing.
		for (TFieldIterator<FProperty> It(Event); It; ++It)
		{
			const FProperty* Param = *It;
			if (!IsCallInputParameter(Param))
			{
				continue;
			}

			FEdGraphPinType PinType;
			if (!BuildParameterPinType(Schema, Param, PinType))
			{
				continue;
			}

			UEdGraphPin* ParamPin = CreatePin(EGPD_Output, PinType, Param->GetFName());
			ParamPin->PinFriendlyName = MakeParameterFriendlyName(Param);
		}
	}

	RefreshEventDerivedPins();

	Super::AllocateDefaultPins();
}

void UCrowdyK2Node_ReceiveCrowdyEventOnEntity::PostReconstructNode()
{
	Super::PostReconstructNode();

	// A rebuild carries each old pin's value onto the new pin of the same name, which would put back the
	// name the event had before it was renamed. Re-deriving here, after that copy, is what keeps the label
	// showing the event the member reference now resolves to.
	RefreshEventDerivedPins();
}

void UCrowdyK2Node_ReceiveCrowdyEventOnEntity::RefreshEventDerivedPins()
{
	if (UEdGraphPin* PickerPin = FindPin(CrowdyMassReceiveEventPins::EventPicker, EGPD_Input))
	{
		PickerPin->DefaultValue = GetPickedEventDisplayName();
	}
}

UEdGraphPin* UCrowdyK2Node_ReceiveCrowdyEventOnEntity::GetBindPin() const
{
	return FindPin(CrowdyMassReceiveEventPins::Bind, EGPD_Input);
}

UEdGraphPin* UCrowdyK2Node_ReceiveCrowdyEventOnEntity::GetUnbindPin() const
{
	return FindPin(CrowdyMassReceiveEventPins::Unbind, EGPD_Input);
}

UEdGraphPin* UCrowdyK2Node_ReceiveCrowdyEventOnEntity::GetThenPin() const
{
	return FindPin(CrowdyMassReceiveEventPins::Then, EGPD_Output);
}

UEdGraphPin* UCrowdyK2Node_ReceiveCrowdyEventOnEntity::GetOnEventPin() const
{
	return FindPin(CrowdyMassReceiveEventPins::OnEvent, EGPD_Output);
}

UEdGraphPin* UCrowdyK2Node_ReceiveCrowdyEventOnEntity::GetTargetPin() const
{
	return FindPin(CrowdyMassReceiveEventPins::Target, EGPD_Output);
}

FText UCrowdyK2Node_ReceiveCrowdyEventOnEntity::GetNodeTitle(const ENodeTitleType::Type TitleType) const
{
	const FString Picked = GetPickedEventDisplayName();
	if (TitleType == ENodeTitleType::MenuTitle || TitleType == ENodeTitleType::ListView || Picked.IsEmpty())
	{
		return LOCTEXT("NodeTitle", "Receive Crowdy Event on Entity");
	}

	const UClass* DeclaringClass = GetPickedEventClass();
	if (!DeclaringClass)
	{
		return FText::Format(LOCTEXT("NodeTitleUnresolved", "Receive {0} on Entity (missing)"), FText::FromString(Picked));
	}

	FString ClassName = DeclaringClass->GetName();
	ClassName.RemoveFromEnd(TEXT("_C"), ESearchCase::CaseSensitive);

	return FText::Format(LOCTEXT("NodeTitlePicked", "Receive {0} on Entity\n{1}"),
		FText::FromString(Picked), FText::FromString(ClassName));
}

FText UCrowdyK2Node_ReceiveCrowdyEventOnEntity::GetTooltipText() const
{
	return LOCTEXT("NodeTooltip",
		"Runs part of this graph whenever a Crowdy event arrives for a crowd entity: the observer's half of "
		"the event that \"Call Crowdy Event on Entity\" sends.\n\n"
		"Pick the event on the node and it grows an output pin per parameter, plus a Target naming the entity "
		"the call arrived for. Bind starts listening and Unbind stops; the binding also ends when this object "
		"or its world does.\n\n"
		"Everything On Event carries was chosen by the client being observed and can be anything at all. Use "
		"it to decide what is drawn, never what is true.");
}

FSlateIcon UCrowdyK2Node_ReceiveCrowdyEventOnEntity::GetIconAndTint(FLinearColor& OutColor) const
{
	OutColor = GetNodeTitleColor();
	static FSlateIcon Icon(FAppStyle::GetAppStyleSetName(), "GraphEditor.Event_16x");
	return Icon;
}

FText UCrowdyK2Node_ReceiveCrowdyEventOnEntity::GetMenuCategory() const
{
	return LOCTEXT("MenuCategory", "Crowdy Mass|Events");
}

void UCrowdyK2Node_ReceiveCrowdyEventOnEntity::GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const
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

bool UCrowdyK2Node_ReceiveCrowdyEventOnEntity::IsCompatibleWithGraph(const UEdGraph* TargetGraph) const
{
	if (!Super::IsCompatibleWithGraph(TargetGraph))
	{
		return false;
	}

	// The node compiles to an event, and an event can only live in an event graph. Refusing it here is what
	// keeps it out of a function or macro graph, where it would fail with an error about a node the author
	// never placed.
	const UEdGraphSchema* GraphSchema = TargetGraph ? TargetGraph->GetSchema() : nullptr;
	return GraphSchema && GraphSchema->GetGraphType(TargetGraph) == EGraphType::GT_Ubergraph;
}

bool UCrowdyK2Node_ReceiveCrowdyEventOnEntity::CanPasteHere(const UEdGraph* TargetGraph) const
{
	// Asked separately from placement, and it has to be: the base answer is about the schema alone, so
	// without this a node copied out of an event graph would paste happily into a function graph.
	return Super::CanPasteHere(TargetGraph) && IsCompatibleWithGraph(TargetGraph);
}

void UCrowdyK2Node_ReceiveCrowdyEventOnEntity::ValidateNodeDuringCompilation(FCompilerResultsLog& MessageLog) const
{
	using namespace CrowdyMassReceiveEventNodePrivate;

	Super::ValidateNodeDuringCompilation(MessageLog);

	if (!ResolveEntityTargetStruct())
	{
		MessageLog.Error(*LOCTEXT("MissingTargetStruct",
			"@@ cannot compile: the Crowdy Mass entity target type is not available in this build.").ToString(), this);
		return;
	}

	if (!ResolveBindFunction() || !ResolveUnbindFunction())
	{
		MessageLog.Error(*LOCTEXT("MissingBindEntry",
			"@@ cannot compile: the Crowdy Mass runtime does not provide the entries this node calls.").ToString(), this);
		return;
	}

	// Deliberately no check that this is an event graph, even though the node compiles to an event and only
	// an event graph can hold one. Placement and paste both refuse anything else, and by the time this runs
	// the graph is the compiler's merged copy, which no schema reports as an event graph: a check here would
	// fail every legitimate compile instead of the case it was written for.

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

	// The SAME predicate the call node uses, deliberately: whether a call can ride the transport at all is
	// one question, and the two nodes must never disagree about it.
	const FString CallProblem = CrowdyMassEventCatalog::DescribeCallProblem(Event);
	if (!CallProblem.IsEmpty())
	{
		MessageLog.Error(*FText::Format(LOCTEXT("EventNotReceivable",
			"@@ refers to '{0}', which {1}."),
			FText::FromString(Event->GetName()), FText::FromString(CallProblem)).ToString(), this);
		return;
	}

	// Where the two nodes' lists DO differ, and the one refusal only this node makes. An event declared on
	// the crowd stand-in base is applied by the crowd plane itself: it resolves a stand-in on every machine
	// that receives it and runs the event's own body there, so the call is never offered to anything
	// registered and a graph bound to it would compile clean and never run. Refused here as well as being
	// kept out of this node's dropdown, because a node saved before the dropdown stopped offering it is
	// exactly the case that would otherwise stay silently dead.
	if (CrowdyMassEventCatalog::IsAppliedByTheCrowdPlane(Event))
	{
		MessageLog.Error(*FText::Format(LOCTEXT("EventAppliedByThePlane",
			"@@ refers to '{0}', which the crowd plane applies itself, so nothing is ever handed to a graph for it. Send it with Call Crowdy Event on Entity, or receive an event declared on the entity's own class instead."),
			FText::FromString(Event->GetName())).ToString(), this);
		return;
	}

	// A parameter by one of these names would be renamed by the compiler to keep the intermediate event's
	// own pins unique, and the binding fills parameters by name, so that one would silently stop arriving.
	for (TFieldIterator<FProperty> It(Event); It; ++It)
	{
		const FProperty* Param = *It;
		if (IsCallInputParameter(Param) && IsReservedParameterName(Param->GetFName()))
		{
			MessageLog.Error(*FText::Format(LOCTEXT("ReservedParameterName",
				"@@ cannot receive '{0}': its parameter '{1}' uses a name this node needs for itself. Rename the parameter on the event."),
				FText::FromString(Event->GetName()), FText::FromString(Param->GetName())).ToString(), this);
			return;
		}
	}
}

// Checked on the source graph: the compiler validates its merged copy only after expansion has moved Bind's links away.
void UCrowdyK2Node_ReceiveCrowdyEventOnEntity::EarlyValidation(FCompilerResultsLog& MessageLog) const
{
	Super::EarlyValidation(MessageLog);

	if (!IsNodeEnabled())
	{
		return;
	}
	const UEdGraphPin* BindPin = GetBindPin();
	const UEdGraphPin* UnbindPin = GetUnbindPin();
	if (BindPin && BindPin->LinkedTo.Num() == 0 && UnbindPin && UnbindPin->LinkedTo.Num() == 0)
	{
		MessageLog.Warning(*LOCTEXT("NeverBound",
			"@@ is never reached: nothing runs into Bind, so it will not listen for anything. Wire Bind to Begin Play or wherever this object starts observing.").ToString(), this);
	}
}

void UCrowdyK2Node_ReceiveCrowdyEventOnEntity::ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph)
{
	using namespace CrowdyMassReceiveEventNodePrivate;

	Super::ExpandNode(CompilerContext, SourceGraph);

	const UEdGraphSchema_K2* Schema = CompilerContext.GetSchema();
	UFunction* Event = GetPickedEvent();
	UClass* DeclaringClass = GetPickedEventClass();
	UFunction* BindFunction = ResolveBindFunction();
	UFunction* UnbindFunction = ResolveUnbindFunction();
	UScriptStruct* TargetStruct = ResolveEntityTargetStruct();

	UEdGraphPin* BindPin = GetBindPin();
	UEdGraphPin* UnbindPin = GetUnbindPin();
	UEdGraphPin* ThenPin = GetThenPin();
	UEdGraphPin* OnEventPin = GetOnEventPin();
	UEdGraphPin* TargetPin = GetTargetPin();

	if (!Schema || !Event || !DeclaringClass || !BindFunction || !UnbindFunction || !TargetStruct
		|| !BindPin || !UnbindPin || !ThenPin || !OnEventPin || !TargetPin)
	{
		CompilerContext.MessageLog.Error(
			*LOCTEXT("ExpandMissingParts", "@@ cannot compile: it is missing one of its own pins, or the runtime entry it binds through. Refresh the node.").ToString(),
			this);
		BreakAllNodeLinks();
		return;
	}

	// The intermediate event IS the handler. It is what the runtime entry is given the name of, and what
	// ProcessEvent runs when a call arrives, which is how the graph's own pins end up carrying the values.
	// Named from this node's GUID so two nodes listening to one event stay two separate bindings, and so
	// the name survives a recompile.
	UK2Node_CustomEvent* Handler = CompilerContext.SpawnIntermediateNode<UK2Node_CustomEvent>(this, SourceGraph);
	Handler->CustomFunctionName =
		*FString::Printf(TEXT("CrowdyMassReceive_%s_%s"), *Event->GetName(), *CompilerContext.GetGuid(this));
	Handler->AllocateDefaultPins();

	// Target first, which is what the runtime side reads it as: the handler's first parameter. Every other
	// parameter is matched by name, so only this one depends on where it sits.
	FEdGraphPinType TargetPinType;
	TargetPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
	TargetPinType.PinSubCategoryObject = TargetStruct;

	UEdGraphPin* HandlerTargetPin =
		Handler->CreateUserDefinedPin(CrowdyMassReceiveEventPins::Target, TargetPinType, EGPD_Output);
	if (!HandlerTargetPin || HandlerTargetPin->PinName != CrowdyMassReceiveEventPins::Target)
	{
		CompilerContext.MessageLog.Error(
			*LOCTEXT("TargetParameterRenamed", "@@ cannot compile: the entity target could not be given its own name on the intermediate event.").ToString(),
			this);
		BreakAllNodeLinks();
		return;
	}

	CompilerContext.MovePinLinksToIntermediate(*TargetPin, *HandlerTargetPin);

	for (TFieldIterator<FProperty> It(Event); It; ++It)
	{
		const FProperty* Param = *It;
		if (!IsCallInputParameter(Param))
		{
			continue;
		}

		UEdGraphPin* NodeParamPin = FindPin(Param->GetFName(), EGPD_Output);
		if (!NodeParamPin)
		{
			// The pin set was grown from a different signature than the one the event has now, so what the
			// graph is wired to no longer describes the call. Recompiling the node is what fixes it.
			CompilerContext.MessageLog.Error(
				*FText::Format(LOCTEXT("MissingParamPin",
					"@@ has no pin for parameter '{0}'. Refresh the node."), FText::FromString(Param->GetName())).ToString(),
				this);
			BreakAllNodeLinks();
			return;
		}

		FEdGraphPinType PinType;
		if (!BuildParameterPinType(Schema, Param, PinType))
		{
			CompilerContext.MessageLog.Error(
				*FText::Format(LOCTEXT("UnsupportedParamType",
					"@@ cannot receive parameter '{0}': its type has no Blueprint pin."), FText::FromString(Param->GetName())).ToString(),
				this);
			BreakAllNodeLinks();
			return;
		}

		UEdGraphPin* HandlerParamPin = Handler->CreateUserDefinedPin(Param->GetFName(), PinType, EGPD_Output);

		// The name is the whole of how a value finds its parameter at runtime, so a pin that had to be
		// renamed to stay unique is a value that would never arrive. Reported rather than left to be
		// noticed as a pin that is always its default.
		if (!HandlerParamPin || HandlerParamPin->PinName != Param->GetFName())
		{
			CompilerContext.MessageLog.Error(
				*FText::Format(LOCTEXT("ParamPinRenamed",
					"@@ cannot receive parameter '{0}': that name is already taken on the event this node compiles to. Rename the parameter on the event."),
					FText::FromString(Param->GetName())).ToString(),
				this);
			BreakAllNodeLinks();
			return;
		}

		CompilerContext.MovePinLinksToIntermediate(*NodeParamPin, *HandlerParamPin);
	}

	CompilerContext.MovePinLinksToIntermediate(*OnEventPin, *Handler->FindPinChecked(UEdGraphSchema_K2::PN_Then));

	// One self node feeding both calls: the object whose graph this is, is both the object the handler runs
	// on and the object whose world the binding belongs to.
	UK2Node_Self* SelfNode = CompilerContext.SpawnIntermediateNode<UK2Node_Self>(this, SourceGraph);
	SelfNode->AllocateDefaultPins();
	UEdGraphPin* SelfPin = SelfNode->FindPinChecked(UEdGraphSchema_K2::PN_Self);

	const FString HandlerName = Handler->GetFunctionName().ToString();
	const FString EventName = Event->GetName();
	const FString SignatureId = LexToString(FCrowdyRPC::ComputeFunctionID(Event));

	UK2Node_CallFunction* BindCall = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
	BindCall->SetFromFunction(BindFunction);
	BindCall->AllocateDefaultPins();

	bool bWiredSelf = Schema->TryCreateConnection(SelfPin, BindCall->FindPinChecked(FName(HandlerObjectParam)));
	BindCall->FindPinChecked(FName(HandlerFunctionNameParam))->DefaultValue = HandlerName;
	BindCall->FindPinChecked(FName(EventClassParam))->DefaultObject = DeclaringClass;
	BindCall->FindPinChecked(FName(EventNameParam))->DefaultValue = EventName;

	// The signature the output pins above were grown from. The class and the name alone would let the
	// binding resolve a DIFFERENT function than this graph was compiled for, if the event is reshaped and
	// this graph is not recompiled; the pins would then quietly stop being filled. Sending the signature
	// makes that disagreement something the bind can refuse and report.
	BindCall->FindPinChecked(FName(ExpectedSignatureParam))->DefaultValue = SignatureId;

	UK2Node_CallFunction* UnbindCall = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
	UnbindCall->SetFromFunction(UnbindFunction);
	UnbindCall->AllocateDefaultPins();

	bWiredSelf &= Schema->TryCreateConnection(SelfPin, UnbindCall->FindPinChecked(FName(HandlerObjectParam)));
	UnbindCall->FindPinChecked(FName(HandlerFunctionNameParam))->DefaultValue = HandlerName;
	UnbindCall->FindPinChecked(FName(EventClassParam))->DefaultObject = DeclaringClass;
	UnbindCall->FindPinChecked(FName(EventNameParam))->DefaultValue = EventName;

	// The object the handler runs on is the whole of what a binding is made against, so a self reference
	// that would not connect leaves a bind that could only ever refuse itself.
	if (!bWiredSelf)
	{
		CompilerContext.MessageLog.Error(
			*LOCTEXT("SelfNotWired", "@@ cannot compile: this Blueprint's own reference could not be passed to the binding.").ToString(),
			this);
		BreakAllNodeLinks();
		return;
	}

	CompilerContext.MovePinLinksToIntermediate(*BindPin, *BindCall->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
	CompilerContext.MovePinLinksToIntermediate(*UnbindPin, *UnbindCall->FindPinChecked(UEdGraphSchema_K2::PN_Execute));

	// Copied to both, not moved to one: Then continues after whichever of the two exec inputs was taken,
	// and moving the links would leave the other with nothing to continue into.
	CompilerContext.CopyPinLinksToIntermediate(*ThenPin, *BindCall->FindPinChecked(UEdGraphSchema_K2::PN_Then));
	CompilerContext.CopyPinLinksToIntermediate(*ThenPin, *UnbindCall->FindPinChecked(UEdGraphSchema_K2::PN_Then));

	BreakAllNodeLinks();
}

#undef LOCTEXT_NAMESPACE
