#include "CrowdyK2Node_ServerObjectBase.h"

#include "Algo/Transform.h"
#include "BlueprintActionDatabaseRegistrar.h"
#include "BlueprintNodeSpawner.h"
#include "CrowdyK2Node_CallServerFunction.h"
#include "CrowdyK2Node_GetServerState.h"
#include "CrowdyK2Node_GetServerVariable.h"
#include "CrowdyK2Node_ServerVariableChanged.h"
#include "CrowdyServerObject.h"
#include "CrowdyServerObjectComponent.h"
#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectLibrary.h"
#include "CrowdyServerObjectLink.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Self.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "KismetCompiler.h"
#include "ScopedTransaction.h"
#include "ToolMenu.h"
#include "ToolMenuSection.h"
#include "UObject/UObjectIterator.h"

#define LOCTEXT_NAMESPACE "CrowdyK2Node_ServerObjectBase"

const FName CrowdyServerObjectNodePins::InstanceId(TEXT("Instance Id"));
const FName CrowdyServerObjectNodePins::TeamId(TEXT("Team Id"));
const FName CrowdyServerObjectNodePins::Changed(TEXT("Value Changed"));
const FName CrowdyServerObjectNodePins::HasValues(TEXT("Has Values"));

UCrowdyServerObjectDefinition* UCrowdyK2Node_ServerObjectBase::GetDefinition() const
{
	CrowdyServerObjectPins::PreloadDefinition(Definition);
	return Definition;
}

bool UCrowdyK2Node_ServerObjectBase::UsesFind() const
{
	const UCrowdyServerObjectDefinition* Current = GetDefinition();
	return bFind || (Current && Current->bOnlyOneInstance);
}

bool UCrowdyK2Node_ServerObjectBase::IsTargetClass(const UClass* Class)
{
	return Class && (Class->IsChildOf<UCrowdyServerObject>() || Class->IsChildOf<UCrowdyServerObjectComponent>() || Class->IsChildOf<AActor>()
		|| Class->IsChildOf<UCrowdyServerObjectLink>());
}

void UCrowdyK2Node_ServerObjectBase::GatherFunctionNames(const UCrowdyServerObjectDefinition* Definition, TArray<FName>& Out)
{
	Out.Reset();
	if (!Definition)
	{
		return;
	}
	Algo::Transform(Definition->Functions, Out, &FCrowdyServerFunction::Name);
	if (Definition->MembersFrom == ECrowdyServerMembersSource::ThisObject)
	{
		Algo::Transform(UCrowdyServerObjectDefinition::GetMemberFunctions(), Out, &FCrowdyServerFunction::Name);
	}
}

void UCrowdyK2Node_ServerObjectBase::GatherMenuEntries(const UCrowdyServerObjectDefinition* Definition, TArray<FCrowdyServerObjectMenuEntry>& Out)
{
	Out.Reset();
	if (!Definition)
	{
		return;
	}
	const FText Asset = FText::FromString(Definition->GetName());

	// A node is offered only when every value it shows has a pin, since a value without one fails its compile.
	TArray<FCrowdyServerValuePin> Variables;
	CrowdyServerObjectPins::GatherVariables(Definition, Variables);
	bool bStateHasPins = true;
	for (const FCrowdyServerValuePin& Variable : Variables)
	{
		if (!Variable.bVisible)
		{
			continue;
		}
		bStateHasPins &= Variable.HasPin();
		if (!Variable.HasPin())
		{
			continue;
		}
		Out.Add({UCrowdyK2Node_GetServerVariable::StaticClass(), Variable.Name, Variable.Id, UCrowdyK2Node_GetServerVariable::MakeTitle(Variable.Name, Asset)});
		Out.Add({UCrowdyK2Node_ServerVariableChanged::StaticClass(), Variable.Name, Variable.Id, UCrowdyK2Node_ServerVariableChanged::MakeTitle(Variable.Name, Asset)});
	}

	TArray<FName> Functions;
	GatherFunctionNames(Definition, Functions);
	for (const FName Function : Functions)
	{
		if (UCrowdyK2Node_CallServerFunction::HasEveryPin(Definition, Function))
		{
			Out.Add({UCrowdyK2Node_CallServerFunction::StaticClass(), Function, FGuid(), UCrowdyK2Node_CallServerFunction::MakeTitle(Function, Asset)});
		}
	}

	if (bStateHasPins)
	{
		Out.Add({UCrowdyK2Node_GetServerState::StaticClass(), NAME_None, FGuid(), UCrowdyK2Node_GetServerState::MakeTitle(Asset)});
	}
}

void UCrowdyK2Node_ServerObjectBase::GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const
{
	// Only loaded definitions are offered; the module loads every definition asset, and the action database refreshes
	// an asset's entries itself when one is loaded, added or removed.
	for (TObjectIterator<UCrowdyServerObjectDefinition> It; It; ++It)
	{
		UCrowdyServerObjectDefinition* Asset = *It;
		if (!IsValid(Asset) || !Asset->IsAsset() || !ActionRegistrar.IsOpenForRegistration(Asset))
		{
			continue;
		}

		TArray<FCrowdyServerObjectMenuEntry> Entries;
		GatherMenuEntries(Asset, Entries);
		const FText Category = FText::Format(LOCTEXT("MenuCategory", "Server Objects|{0}"), FText::FromString(Asset->GetName()));
		for (const FCrowdyServerObjectMenuEntry& Entry : Entries)
		{
			if (Entry.NodeClass != GetClass())
			{
				continue;
			}
			UBlueprintNodeSpawner* Spawner = UBlueprintNodeSpawner::Create(Entry.NodeClass);
			Spawner->DefaultMenuSignature.MenuName = Entry.Title;
			Spawner->DefaultMenuSignature.Category = Category;
			Spawner->DefaultMenuSignature.Keywords = FText::FromName(Entry.Member);
			TWeakObjectPtr<UCrowdyServerObjectDefinition> WeakAsset = Asset;
			Spawner->CustomizeNodeDelegate = UBlueprintNodeSpawner::FCustomizeNodeDelegate::CreateLambda(
				[WeakAsset, Member = Entry.Member, MemberId = Entry.MemberId](UEdGraphNode* NewNode, bool /*bIsTemplateNode*/)
				{
					UCrowdyK2Node_ServerObjectBase* Node = CastChecked<UCrowdyK2Node_ServerObjectBase>(NewNode);
					Node->Definition = WeakAsset.Get();
					Node->Member = Member;
					Node->MemberId = MemberId;
				});
			ActionRegistrar.AddBlueprintAction(Asset, Spawner);
		}
	}
}

FText UCrowdyK2Node_ServerObjectBase::GetMenuCategory() const
{
	return FText::Format(LOCTEXT("NodeCategory", "Server Objects|{0}"), GetAssetName());
}

FText UCrowdyK2Node_ServerObjectBase::GetAssetName() const
{
	const UCrowdyServerObjectDefinition* Current = GetDefinition();
	return Current ? FText::FromString(Current->GetName()) : LOCTEXT("NoDefinition", "None");
}

bool UCrowdyK2Node_ServerObjectBase::HasExternalDependencies(TArray<UStruct*>* OptionalOutput) const
{
	const UCrowdyServerObjectDefinition* Current = GetDefinition();
	if (!Current)
	{
		return Super::HasExternalDependencies(OptionalOutput);
	}
	if (OptionalOutput)
	{
		OptionalOutput->AddUnique(const_cast<UScriptStruct*>(Current->GetStateStruct()));
		for (const FCrowdyServerFunction& Function : Current->Functions)
		{
			OptionalOutput->AddUnique(const_cast<UScriptStruct*>(Function.GetParamsStruct()));
			OptionalOutput->AddUnique(const_cast<UScriptStruct*>(Function.GetReplyStruct()));
		}
		OptionalOutput->Remove(nullptr);
	}
	Super::HasExternalDependencies(OptionalOutput);
	return true;
}

UK2Node::ERedirectType UCrowdyK2Node_ServerObjectBase::DoPinsMatchForReconstruction(const UEdGraphPin* NewPin, int32 NewPinIndex, const UEdGraphPin* OldPin, int32 OldPinIndex) const
{
	// A renamed List entry or Blueprint struct field keeps its id, so its pin keeps its wires under the new name.
	if (NewPin->PersistentGuid.IsValid() && NewPin->PersistentGuid == OldPin->PersistentGuid && NewPin->Direction == OldPin->Direction)
	{
		return ERedirectType_Name;
	}
	return Super::DoPinsMatchForReconstruction(NewPin, NewPinIndex, OldPin, OldPinIndex);
}

void UCrowdyK2Node_ServerObjectBase::CreateSourcePins()
{
	if (!UsesFind())
	{
		UEdGraphPin* Target = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Object, UObject::StaticClass(), UEdGraphSchema_K2::PN_Self);
		Target->PinFriendlyName = LOCTEXT("TargetPin", "Target");
		Target->PinToolTip = LOCTEXT("TargetTooltip", "A Server Object, a Crowdy Server Object component, an actor with one, or a Server Object Link. Empty uses self.").ToString();
		return;
	}
	const UCrowdyServerObjectDefinition* Current = GetDefinition();
	if (Current && Current->bOnlyOneInstance)
	{
		return;
	}
	if (Instance == ECrowdyServerObjectFind::InstanceId)
	{
		CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_String, CrowdyServerObjectNodePins::InstanceId);
	}
	if (Instance == ECrowdyServerObjectFind::PlayersTeam)
	{
		UEdGraphPin* Team = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Int64, CrowdyServerObjectNodePins::TeamId);
		Team->PinToolTip = LOCTEXT("TeamTooltip", "Which team, when the player is in several; 0 for their first.").ToString();
		GetDefault<UEdGraphSchema_K2>()->SetPinAutogeneratedDefaultValueBasedOnType(Team);
	}
}

UEdGraphPin* UCrowdyK2Node_ServerObjectBase::CreateValuePins(EEdGraphPinDirection Direction, const FCrowdyServerValuePin& Value)
{
	if (!Value.HasPin())
	{
		return nullptr;
	}
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	UEdGraphPin* Pin = CreatePin(Direction, Value.Type, Value.Name);
	Pin->PersistentGuid = Value.Id;
	Schema->SetPinAutogeneratedDefaultValueBasedOnType(Pin);
	if (Value.bOptional)
	{
		UEdGraphPin* HasValue = CreatePin(Direction, UEdGraphSchema_K2::PC_Boolean, CrowdyServerObjectPins::HasValuePinName(Value.Name));
		HasValue->PersistentGuid = CrowdyServerObjectPins::HasValuePinId(Value.Id);
		Schema->SetPinAutogeneratedDefaultValueBasedOnType(HasValue);
	}
	return Pin;
}

UEdGraphPin* UCrowdyK2Node_ServerObjectBase::CreateReadyPin(FName Name, const FGuid& Id)
{
	UEdGraphPin* Pin = CreateAdvancedOutput(UEdGraphSchema_K2::PC_Boolean, nullptr, Name, LOCTEXT("ReadyTooltip", "False until the server has sent this Server Object's values"));
	Pin->PersistentGuid = Id;
	return Pin;
}

UEdGraphPin* UCrowdyK2Node_ServerObjectBase::CreateAdvancedOutput(FName Category, UObject* SubCategoryObject, FName Name, const FText& Tooltip)
{
	UEdGraphPin* Pin = CreatePin(EGPD_Output, Category, SubCategoryObject, Name);
	Pin->bAdvancedView = true;
	Pin->PinToolTip = Tooltip.ToString();
	if (AdvancedPinDisplay == ENodeAdvancedPins::NoPins)
	{
		AdvancedPinDisplay = ENodeAdvancedPins::Hidden;
	}
	return Pin;
}

bool UCrowdyK2Node_ServerObjectBase::FindMemberVariable(FCrowdyServerValuePin& Out) const
{
	TArray<FCrowdyServerValuePin> Variables;
	CrowdyServerObjectPins::GatherVariables(GetDefinition(), Variables);
	const FCrowdyServerValuePin* Found = CrowdyServerObjectPins::FindValue(Variables, MemberId, Member);
	if (!Found)
	{
		return false;
	}
	Out = *Found;
	return true;
}

void UCrowdyK2Node_ServerObjectBase::RefreshMemberVariable()
{
	FCrowdyServerValuePin Variable;
	if (!FindMemberVariable(Variable))
	{
		return;
	}
	Member = Variable.Name;
	MemberId = Variable.Id;
}

bool UCrowdyK2Node_ServerObjectBase::CheckDefinition(FKismetCompilerContext& CompilerContext) const
{
	if (GetDefinition())
	{
		return true;
	}
	CompilerContext.MessageLog.Error(*LOCTEXT("NoDefinitionError", "@@ has no Server Object definition. Delete it and place it again from the menu.").ToString(), this);
	return false;
}

bool UCrowdyK2Node_ServerObjectBase::CheckVariable(FKismetCompilerContext& CompilerContext, FCrowdyServerValuePin& Out) const
{
	if (!CheckDefinition(CompilerContext))
	{
		return false;
	}
	if (!FindMemberVariable(Out))
	{
		CompilerContext.MessageLog.Error(*FText::Format(LOCTEXT("NoVariable", "@@: {0} has no variable {1} any more."), GetAssetName(), FText::FromName(Member)).ToString(), this);
		return false;
	}
	if (!Out.bVisible)
	{
		CompilerContext.MessageLog.Error(*FText::Format(LOCTEXT("HiddenVariable", "@@: {0} is not Visible to Players, so players never receive it. Tick it under Visible to Players in {1}, or remove this node."),
			FText::FromName(Out.Name), GetAssetName()).ToString(), this);
		return false;
	}
	return CheckValue(CompilerContext, Out);
}

bool UCrowdyK2Node_ServerObjectBase::CheckValue(FKismetCompilerContext& CompilerContext, const FCrowdyServerValuePin& Value) const
{
	if (Value.HasPin())
	{
		return true;
	}
	CompilerContext.MessageLog.Error(*FText::Format(LOCTEXT("RefusedValue", "@@: {0} has no pin, because {1}."), FText::FromName(Value.Name), Value.Refusal).ToString(), this);
	return false;
}

bool UCrowdyK2Node_ServerObjectBase::MoveValueLinks(FKismetCompilerContext& CompilerContext, UEdGraphPin& Pin, UEdGraphPin& To)
{
	bool bFits = true;
	for (UEdGraphPin* Linked : Pin.LinkedTo)
	{
		if (CompilerContext.GetSchema()->CanCreateConnection(&Pin, Linked).Response != CONNECT_RESPONSE_DISALLOW)
		{
			continue;
		}
		CompilerContext.MessageLog.Error(*FText::Format(LOCTEXT("StaleWire", "@@: the wire on {0} no longer fits, because {0} is now {1}. Reconnect or remove it."),
			Pin.GetDisplayName(), UEdGraphSchema_K2::TypeToText(Pin.PinType)).ToString(), this);
		bFits = false;
	}
	if (!bFits)
	{
		return false;
	}
	if (!CompilerContext.MovePinLinksToIntermediate(Pin, To).CanSafeConnect())
	{
		CompilerContext.MessageLog.Error(*FText::Format(LOCTEXT("MoveFailed", "@@: could not connect {0}."), Pin.GetDisplayName()).ToString(), this);
		return false;
	}
	return true;
}

bool UCrowdyK2Node_ServerObjectBase::IsConnectionDisallowed(const UEdGraphPin* MyPin, const UEdGraphPin* OtherPin, FString& OutReason) const
{
	if (!MyPin || !OtherPin || MyPin->PinName != UEdGraphSchema_K2::PN_Self || MyPin->Direction != EGPD_Input)
	{
		return Super::IsConnectionDisallowed(MyPin, OtherPin, OutReason);
	}
	const bool bSelf = OtherPin->PinType.PinSubCategory == UEdGraphSchema_K2::PSC_Self;
	const UClass* Class = bSelf ? GetBlueprintClassFromNode() : Cast<UClass>(OtherPin->PinType.PinSubCategoryObject.Get());
	if (OtherPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object && IsTargetClass(Class))
	{
		return false;
	}
	OutReason = LOCTEXT("TargetRefused", "Target takes a Server Object, a Crowdy Server Object component, an actor with one, or a Server Object Link").ToString();
	return true;
}

bool UCrowdyK2Node_ServerObjectBase::ExpandSource(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, TConstArrayView<UEdGraphPin*> Inputs)
{
	const UEdGraphSchema_K2* Schema = CompilerContext.GetSchema();
	UK2Node_Self* Self = CompilerContext.SpawnIntermediateNode<UK2Node_Self>(this, SourceGraph);
	Self->AllocateDefaultPins();
	UEdGraphPin* SelfPin = Self->FindPinChecked(UEdGraphSchema_K2::PN_Self);

	if (!UsesFind())
	{
		UEdGraphPin* Target = FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input);
		const bool bWired = Target && Target->LinkedTo.Num() > 0;
		const UEdGraphPin* Source = bWired ? Target->LinkedTo[0] : SelfPin;
		FString Reason;
		if (!Target || IsConnectionDisallowed(Target, Source, Reason))
		{
			const UClass* Class = Source->PinType.PinSubCategory == UEdGraphSchema_K2::PSC_Self ? GetBlueprintClassFromNode() : Cast<UClass>(Source->PinType.PinSubCategoryObject.Get());
			const FText ClassName = Class ? Class->GetDisplayNameText() : UEdGraphSchema_K2::TypeToText(Source->PinType);
			const FText Message = bWired
				? FText::Format(LOCTEXT("WrongTarget", "@@: Target must be a Server Object, a Crowdy Server Object component, an actor with one, or a Server Object Link, not {0}."), ClassName)
				: FText::Format(LOCTEXT("NoTarget", "@@ has no Target, and {0} is not a Server Object, a Crowdy Server Object component, an actor or a Server Object Link. Connect a Target, or use Find By Asset."), ClassName);
			CompilerContext.MessageLog.Error(*Message.ToString(), this);
			return false;
		}
		bool bConnected = true;
		for (UEdGraphPin* Input : Inputs)
		{
			bConnected &= bWired ? CompilerContext.CopyPinLinksToIntermediate(*Target, *Input).CanSafeConnect() : Schema->TryCreateConnection(SelfPin, Input);
		}
		return ReportSourceConnected(CompilerContext, bConnected);
	}

	UK2Node_CallFunction* Find = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
	Find->FunctionReference.SetExternalMember(GET_FUNCTION_NAME_CHECKED(UCrowdyServerObjectLibrary, FindServerObjectLink), UCrowdyServerObjectLibrary::StaticClass());
	Find->AllocateDefaultPins();
	UEdGraphPin* Owner = Find->FindPin(TEXT("Owner"));
	UEdGraphPin* DefinitionPin = Find->FindPin(TEXT("Definition"));
	UEdGraphPin* InstancePin = Find->FindPin(TEXT("Instance"));
	UEdGraphPin* InstanceIdPin = Find->FindPin(TEXT("InstanceId"));
	UEdGraphPin* TeamIdPin = Find->FindPin(TEXT("TeamId"));
	UEdGraphPin* Link = Find->GetReturnValuePin();
	if (!Owner || !DefinitionPin || !InstancePin || !InstanceIdPin || !TeamIdPin || !Link)
	{
		CompilerContext.MessageLog.Error(*LOCTEXT("FindChanged", "@@ could not be expanded: Find Server Object Link has changed its parameters.").ToString(), this);
		return false;
	}

	bool bConnected = Schema->TryCreateConnection(SelfPin, Owner);
	DefinitionPin->DefaultObject = GetDefinition();
	InstancePin->DefaultValue = StaticEnum<ECrowdyServerObjectFind>()->GetNameStringByValue(static_cast<int64>(Instance));
	const TPair<FName, UEdGraphPin*> Arguments[] = {{CrowdyServerObjectNodePins::InstanceId, InstanceIdPin}, {CrowdyServerObjectNodePins::TeamId, TeamIdPin}};
	for (const TPair<FName, UEdGraphPin*>& Argument : Arguments)
	{
		UEdGraphPin* Pin = FindPin(Argument.Key, EGPD_Input);
		if (!Pin)
		{
			continue;
		}
		Argument.Value->DefaultValue = Pin->DefaultValue;
		bConnected &= CompilerContext.MovePinLinksToIntermediate(*Pin, *Argument.Value).CanSafeConnect();
	}
	for (UEdGraphPin* Input : Inputs)
	{
		bConnected &= Schema->TryCreateConnection(Link, Input);
	}
	return ReportSourceConnected(CompilerContext, bConnected);
}

bool UCrowdyK2Node_ServerObjectBase::ReportSourceConnected(FKismetCompilerContext& CompilerContext, bool bConnected) const
{
	if (!bConnected)
	{
		CompilerContext.MessageLog.Error(*LOCTEXT("SourceNotConnected", "@@ could not connect where its Server Object comes from.").ToString(), this);
	}
	return bConnected;
}

void UCrowdyK2Node_ServerObjectBase::GetNodeContextMenuActions(UToolMenu* Menu, UGraphNodeContextMenuContext* Context) const
{
	Super::GetNodeContextMenuActions(Menu, Context);
	const UCrowdyServerObjectDefinition* Current = GetDefinition();
	if (!Menu || !Context || Context->bIsDebugging || !Current || Current->bOnlyOneInstance)
	{
		return;
	}
	FToolMenuSection& Section = Menu->AddSection(TEXT("CrowdyServerObjectNode"), LOCTEXT("ContextSection", "Server Object"));
	UCrowdyK2Node_ServerObjectBase* MutableThis = const_cast<UCrowdyK2Node_ServerObjectBase*>(this);
	Section.AddMenuEntry(TEXT("CrowdyServerObjectToggleFind"),
		bFind ? LOCTEXT("UseTarget", "Use Target") : LOCTEXT("FindByAsset", "Find By Asset"),
		bFind ? LOCTEXT("UseTargetTooltip", "Take the Server Object from a Target pin.") : LOCTEXT("FindByAssetTooltip", "Find the Server Object by this definition and an Instance, with no Target."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateUObject(MutableThis, &UCrowdyK2Node_ServerObjectBase::ToggleFind)));
}

void UCrowdyK2Node_ServerObjectBase::ToggleFind()
{
	const FScopedTransaction Transaction(LOCTEXT("ToggleFind", "Change Server Object Source"));
	Modify();
	bFind = !bFind;
	ReconstructAndMarkModified();
}

void UCrowdyK2Node_ServerObjectBase::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	const FName Changed = PropertyChangedEvent.GetMemberPropertyName();
	if (Changed != GET_MEMBER_NAME_CHECKED(UCrowdyK2Node_ServerObjectBase, bFind) && Changed != GET_MEMBER_NAME_CHECKED(UCrowdyK2Node_ServerObjectBase, Instance))
	{
		return;
	}
	ReconstructAndMarkModified();
}

void UCrowdyK2Node_ServerObjectBase::ReconstructAndMarkModified()
{
	ReconstructNode();
	if (UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForNode(this))
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}
}

void UCrowdyK2Node_ServerObjectBase::PostLoad()
{
	Super::PostLoad();
	RegisterDefinitionListener();
}

void UCrowdyK2Node_ServerObjectBase::PostPlacedNewNode()
{
	Super::PostPlacedNewNode();
	RegisterDefinitionListener();
}

void UCrowdyK2Node_ServerObjectBase::PostPasteNode()
{
	Super::PostPasteNode();
	RegisterDefinitionListener();
}

void UCrowdyK2Node_ServerObjectBase::BeginDestroy()
{
	if (OnPropertyChangedHandle.IsValid())
	{
		FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(OnPropertyChangedHandle);
		OnPropertyChangedHandle.Reset();
	}
	Super::BeginDestroy();
}

void UCrowdyK2Node_ServerObjectBase::RegisterDefinitionListener()
{
	// A transient node is a compile's copy of the graph, which nobody edits and which only garbage collection removes.
	if (!GetGraph() || HasAnyFlags(RF_Transient) || OnPropertyChangedHandle.IsValid())
	{
		return;
	}
	OnPropertyChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddUObject(this, &UCrowdyK2Node_ServerObjectBase::HandleObjectPropertyChanged);
}

void UCrowdyK2Node_ServerObjectBase::HandleObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& PropertyChangedEvent)
{
	if (!Object || Object != Definition || PropertyChangedEvent.ChangeType == EPropertyChangeType::Interactive)
	{
		return;
	}
	ReconstructAndMarkModified();
}

#undef LOCTEXT_NAMESPACE
