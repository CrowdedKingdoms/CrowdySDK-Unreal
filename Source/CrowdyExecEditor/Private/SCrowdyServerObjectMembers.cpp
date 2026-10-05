#include "SCrowdyServerObjectMembers.h"

#include "CrowdyExecInternal.h"
#include "CrowdyServerCodeFiles.h"
#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectDefinitionCustomization.h"
#include "CrowdyServerObjectSelection.h"
#include "DetailLayoutBuilder.h"
#include "EdGraphSchema_K2.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "PropertyBagDetails.h"
#include "ScopedTransaction.h"
#include "InputCoreTypes.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "SCrowdyServerObjectMembers"

namespace SCrowdyServerObjectMembersDetail
{
	using FItem = SCrowdyServerObjectMembers::FItem;
	using FItemPtr = SCrowdyServerObjectMembers::FItemPtr;

	FName UniqueName(const TCHAR* Base, TFunctionRef<bool(FName)> IsTaken)
	{
		FName Name(Base);
		for (int32 Suffix = 1; IsTaken(Name); ++Suffix)
		{
			Name = FName(*FString::Printf(TEXT("%s_%d"), Base, Suffix));
		}
		return Name;
	}

	/** Every bake problem that starts with one of Prefixes, one per line. */
	FText ProblemsFor(const TArray<FString>& Errors, TConstArrayView<FString> Prefixes)
	{
		TArray<FString> Found;
		for (const FString& Error : Errors)
		{
			if (Prefixes.ContainsByPredicate([&Error](const FString& Prefix) { return !Prefix.IsEmpty() && Error.StartsWith(Prefix, ESearchCase::CaseSensitive); }))
			{
				Found.Add(Error);
			}
		}
		return FText::FromString(FString::Join(Found, TEXT("\n")));
	}

	FSlateColor PinColor(const FEdGraphPinType& PinType)
	{
		return FSlateColor(GetDefault<UEdGraphSchema_K2>()->GetPinTypeColor(PinType));
	}

	FItemPtr MakeSection(FItem::EType Type, const FText& Label)
	{
		FItemPtr Section = MakeShared<FItem>();
		Section->Type = Type;
		Section->Label = Label;
		return Section;
	}

	FText CallerTag(ECrowdyServerFunctionCaller Caller)
	{
		switch (Caller)
		{
		case ECrowdyServerFunctionCaller::Members: return LOCTEXT("MembersTag", "Members");
		case ECrowdyServerFunctionCaller::Leader: return LOCTEXT("LeaderTag", "Leader");
		case ECrowdyServerFunctionCaller::ServerOnly: return LOCTEXT("ServerOnlyTag", "Server Only");
		default: return LOCTEXT("PlayersTag", "Players");
		}
	}

	FText BuiltInToolTip(FName Name)
	{
		static const TMap<FName, FText> ToolTips = {
			{TEXT("Join"), LOCTEXT("JoinToolTip", "Join: adds you")},
			{TEXT("Leave"), LOCTEXT("LeaveToolTip", "Leave: removes you")},
			{TEXT("AddMember"), LOCTEXT("AddMemberToolTip", "Add Member: adds a player (server only)")},
			{TEXT("RemoveMember"), LOCTEXT("RemoveMemberToolTip", "Remove Member: removes a player (leader)")},
			{TEXT("MakeLeader"), LOCTEXT("MakeLeaderToolTip", "Make Leader: hands leadership on (leader)")},
			{TEXT("SetOpenForJoining"), LOCTEXT("SetOpenForJoiningToolTip", "Set Open for Joining: opens or closes joining (leader)")}};
		const FText* Found = ToolTips.Find(Name);
		return Found ? *Found : FText::GetEmpty();
	}
}

SCrowdyServerObjectMembers::~SCrowdyServerObjectMembers()
{
	FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PropertyChangedHandle);
	FTSTicker::RemoveTicker(RebuildTicker);
	if (Selection.IsValid())
	{
		Selection->OnChanged.Remove(SelectionChangedHandle);
	}
}

void SCrowdyServerObjectMembers::Construct(const FArguments& InArgs)
{
	Definition = InArgs._Definition;
	Selection = InArgs._Selection;
	OnOpenFunction = InArgs._OnOpenFunction;
	check(Selection.IsValid());
	TypeSettings = SCrowdyServerObjectMembersDetail::MakeSection(FItem::EType::TypeSettings, LOCTEXT("TypeSettings", "Type Settings"));

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
		.Padding(0.0f)
		[
			SAssignNew(Tree, STreeView<FItemPtr>)
			.TreeItemsSource(&Roots)
			.SelectionMode(ESelectionMode::Single)
			.OnGenerateRow(this, &SCrowdyServerObjectMembers::MakeRow)
			.OnGetChildren_Lambda([](FItemPtr Item, TArray<FItemPtr>& OutChildren) { OutChildren = Item->Children; })
			.OnIsSelectableOrNavigable_Lambda([](FItemPtr Item) { return Item.IsValid() && Item->Type != FItem::EType::BuiltIn; })
			.OnSelectionChanged(this, &SCrowdyServerObjectMembers::HandleSelectionChanged)
			.OnMouseButtonDoubleClick(this, &SCrowdyServerObjectMembers::HandleDoubleClick)
			.OnContextMenuOpening(this, &SCrowdyServerObjectMembers::MakeContextMenu)
		]
	];

	PropertyChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddSP(this, &SCrowdyServerObjectMembers::HandleObjectPropertyChanged);
	SelectionChangedHandle = Selection->OnChanged.AddSP(this, &SCrowdyServerObjectMembers::HandleExternalSelection);
	Rebuild();
}

void SCrowdyServerObjectMembers::PostUndo(bool bSuccess)
{
	Rebuild();
}

void SCrowdyServerObjectMembers::PostRedo(bool bSuccess)
{
	Rebuild();
}

FReply SCrowdyServerObjectMembers::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	const TArray<FItemPtr> Selected = Tree.IsValid() ? Tree->GetSelectedItems() : TArray<FItemPtr>();
	const UCrowdyServerObjectDefinition* Current = Definition.Get();
	if (InKeyEvent.GetKey() != EKeys::Delete || Selected.IsEmpty() || !Current)
	{
		return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
	}
	const FItemPtr Item = Selected[0];
	if (Item->Type == FItem::EType::Function)
	{
		RemoveFunction(Item->Index);
		return FReply::Handled();
	}
	if (Item->Type == FItem::EType::Variable && (Current->StateForm == ECrowdyServerValuesForm::List || Item->bStale))
	{
		RemoveVariable(Item->Name);
		return FReply::Handled();
	}
	return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

void SCrowdyServerObjectMembers::ShowTypeSettings()
{
	Selection->SelectNone();
	RestoreSelection(false);
}

void SCrowdyServerObjectMembers::Rebuild()
{
	using namespace SCrowdyServerObjectMembersDetail;
	Roots.Reset();
	const UCrowdyServerObjectDefinition* Current = Definition.Get();
	if (!Current)
	{
		Tree->RequestTreeRefresh();
		return;
	}
	TArray<FCrowdyExecBakedStruct> Structs;
	TArray<FCrowdyExecBakedEnum> Enums;
	TArray<FString> Errors;
	Current->BuildTables(Structs, Enums, Errors);

	FItemPtr Variables = MakeSection(FItem::EType::Variables, LOCTEXT("Variables", "Variables"));
	const UScriptStruct* StateStruct = Current->GetStateStruct();
	const FString ListName = Current->FindListName(StateStruct);
	const FString StateName = ListName.IsEmpty() ? CrowdyExec::DisplayName(StateStruct) : ListName;
	auto AddVariable = [&](FName Name, const FEdGraphPinType& PinType)
	{
		FItemPtr Item = MakeShared<FItem>();
		Item->Type = FItem::EType::Variable;
		Item->Name = Name;
		Item->Label = FText::FromName(Name);
		Item->TypeColor = PinColor(PinType);
		Item->Detail = UEdGraphSchema_K2::TypeToText(PinType);
		Item->bVisibleToPlayers = Current->WatchedFields.Contains(Name);
		Item->Problem = ProblemsFor(Errors, {StateName + TEXT(".") + Name.ToString() + TEXT(":")});
		Variables->Children.Add(Item);
	};
	const UPropertyBag* Bag = Current->StateList.GetPropertyBagStruct();
	if (Current->StateForm == ECrowdyServerValuesForm::List && Bag)
	{
		for (const FPropertyBagPropertyDesc& Desc : Bag->GetPropertyDescs())
		{
			AddVariable(Desc.Name, UE::StructUtils::GetPropertyDescAsPin(Desc));
		}
	}
	else if (Current->StateForm == ECrowdyServerValuesForm::Struct && StateStruct)
	{
		for (TFieldIterator<FProperty> It(StateStruct); It; ++It)
		{
			FEdGraphPinType PinType;
			GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(*It, PinType);
			AddVariable(FName(*It->GetAuthoredName()), PinType);
		}
	}

	// A variable marked visible that no longer exists stays listed, so it can be seen and deleted.
	TArray<FName> Known;
	for (const FItemPtr& Item : Variables->Children)
	{
		Known.Add(Item->Name);
	}
	for (const FName Missing : CrowdyServerCodeFiles::FindMissingFields(Current->WatchedFields, Known))
	{
		FItemPtr Item = MakeShared<FItem>();
		Item->Type = FItem::EType::Variable;
		Item->Name = Missing;
		Item->Label = FText::FromName(Missing);
		Item->TypeColor = FSlateColor::UseSubduedForeground();
		Item->bVisibleToPlayers = true;
		Item->bStale = true;
		Item->Problem = FText::Format(LOCTEXT("StaleVariable", "{0} is no longer a variable, so players cannot see it. Delete it, or add the variable back."), Item->Label);
		Variables->Children.Add(Item);
	}

	FItemPtr Functions = MakeSection(FItem::EType::Functions, LOCTEXT("Functions", "Functions"));
	for (int32 Index = 0; Index < Current->Functions.Num(); ++Index)
	{
		const FCrowdyServerFunction& Function = Current->Functions[Index];
		FItemPtr Item = MakeShared<FItem>();
		Item->Type = FItem::EType::Function;
		Item->Name = Function.Name;
		Item->Index = Index;
		Item->Label = FText::FromName(Function.Name);
		Item->Detail = CrowdyServerObjectText::Signature(Function, false);
		Item->Tag = Function.WhoCanCall == ECrowdyServerFunctionCaller::Players ? FText::GetEmpty() : CallerTag(Function.WhoCanCall);
		const FString ParamsList = Function.GetParamsListName();
		const FString ReplyList = Function.GetReplyListName();
		Item->Problem = ProblemsFor(Errors, {FString::Printf(TEXT("Function %s:"), *Function.Name.ToString()),
			ParamsList.IsEmpty() ? FString() : ParamsList + TEXT("."), ReplyList.IsEmpty() ? FString() : ReplyList + TEXT(".")});
		Functions->Children.Add(Item);
	}

	Roots = {TypeSettings, Variables, Functions};
	if (Current->MembersFrom == ECrowdyServerMembersSource::ThisObject)
	{
		FItemPtr BuiltIns = MakeSection(FItem::EType::BuiltIns, LOCTEXT("BuiltIns", "Built-in"));
		for (const FCrowdyServerFunction& Function : UCrowdyServerObjectDefinition::GetMemberFunctions())
		{
			FItemPtr Item = MakeShared<FItem>();
			Item->Type = FItem::EType::BuiltIn;
			Item->Name = Function.Name;
			Item->Label = FText::FromString(FName::NameToDisplayString(Function.Name.ToString(), false));
			Item->Detail = CrowdyServerObjectText::Signature(Function, true);
			Item->Tag = CallerTag(Function.WhoCanCall);
			BuiltIns->Children.Add(Item);
		}
		Roots.Add(BuiltIns);
	}
	Tree->RequestTreeRefresh();
	for (const FItemPtr& Section : Roots)
	{
		Tree->SetItemExpansion(Section, true);
	}
	RestoreSelection(true);
}

SCrowdyServerObjectMembers::FItemPtr SCrowdyServerObjectMembers::FindRow(TConstArrayView<FItemPtr> Roots, const FCrowdyServerObjectSelection& Selection)
{
	using EKind = FCrowdyServerObjectSelection::EKind;
	auto Shows = [&Selection](const FItemPtr& Item)
	{
		switch (Item->Type)
		{
		case FItem::EType::TypeSettings: return Selection.Kind == EKind::None;
		case FItem::EType::Variable: return Selection.Kind == EKind::Variable && Item->Name == Selection.Variable;
		case FItem::EType::Function: return Selection.Kind == EKind::Function && Item->Index == Selection.Function;
		default: return false;
		}
	};
	for (const FItemPtr& Root : Roots)
	{
		const FItemPtr* Found = Shows(Root) ? &Root : Root->Children.FindByPredicate(Shows);
		if (Found)
		{
			return *Found;
		}
	}
	return nullptr;
}

void SCrowdyServerObjectMembers::SelectItem(const FItemPtr& Item, FCrowdyServerObjectSelection& Selection)
{
	const FItem::EType Type = Item.IsValid() ? Item->Type : FItem::EType::TypeSettings;
	if (Type == FItem::EType::Variable)
	{
		Selection.SelectVariable(Item->Name);
		return;
	}
	if (Type == FItem::EType::Function)
	{
		Selection.SelectFunction(Item->Index);
		return;
	}
	Selection.SelectNone();
}

void SCrowdyServerObjectMembers::RestoreSelection(bool bDropMissing)
{
	TGuardValue<bool> Guard(bSelecting, true);
	if (const FItemPtr Match = FindRow(Roots, *Selection))
	{
		Tree->SetSelection(Match, ESelectInfo::Direct);
		return;
	}
	Tree->ClearSelection();
	// A member added a moment ago has no row until the next rebuild; only a rebuild knows one is gone.
	if (!bDropMissing)
	{
		return;
	}
	Selection->SelectNone();
	if (const FItemPtr Shown = FindRow(Roots, *Selection))
	{
		Tree->SetSelection(Shown, ESelectInfo::Direct);
	}
}

TSharedRef<ITableRow> SCrowdyServerObjectMembers::MakeRow(FItemPtr Item, const TSharedRef<STableViewBase>& Owner)
{
	const bool bSection = Item->Type == FItem::EType::Variables || Item->Type == FItem::EType::Functions || Item->Type == FItem::EType::BuiltIns;
	const bool bTypeSettings = Item->Type == FItem::EType::TypeSettings;
	return SNew(STableRow<FItemPtr>, Owner)
		.Padding(FMargin(2.0f, bSection || bTypeSettings ? 4.0f : 2.0f))
		[
			bTypeSettings ? MakeTypeSettingsRow(Item) : bSection ? MakeSectionRow(Item) : MakeMemberRow(Item)
		];
}

TSharedRef<SWidget> SCrowdyServerObjectMembers::MakeTypeSettingsRow(const FItemPtr& Item)
{
	return SNew(SHorizontalBox)
		.ToolTipText(LOCTEXT("TypeSettingsRowToolTip", "Edit the type's own settings: access, members, timers, saving and Server Names"))
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			SNew(SImage)
			.Image(FAppStyle::GetBrush("Icons.Settings"))
			.ColorAndOpacity(FSlateColor::UseForeground())
		]
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(Item->Label)
			.Font(IDetailLayoutBuilder::GetDetailFontBold())
		];
}

TSharedRef<SWidget> SCrowdyServerObjectMembers::MakeSectionRow(const FItemPtr& Item)
{
	const bool bVariables = Item->Type == FItem::EType::Variables;
	TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(Item->Label)
			.Font(IDetailLayoutBuilder::GetDetailFontBold())
		];
	if (Item->Type == FItem::EType::BuiltIns)
	{
		Row->SetToolTipText(LOCTEXT("BuiltInsToolTip", "Added by Members From This Object; they cannot be edited or deleted"));
		return Row;
	}
	if (bVariables)
	{
		Row->AddSlot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f)
			[
				SNew(SCheckBox)
				.ToolTipText(LOCTEXT("UseStructToolTip", "Take the variables from a C++ or Blueprint struct of your own, chosen in Details, instead of adding them here"))
				.IsChecked_Lambda([this]()
				{
					const UCrowdyServerObjectDefinition* Current = Definition.Get();
					return Current && Current->StateForm == ECrowdyServerValuesForm::Struct ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
				})
				.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { SetVariablesUseStruct(State == ECheckBoxState::Checked); })
				[
					SNew(STextBlock)
					.Text(LOCTEXT("UseStruct", "Use Struct"))
					.Font(IDetailLayoutBuilder::GetDetailFont())
				]
			];
	}
	Row->AddSlot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ToolTipText(bVariables ? LOCTEXT("AddVariableToolTip", "Add a variable") : LOCTEXT("AddFunctionToolTip", "Add a function"))
			.Visibility_Lambda([this, bVariables]()
			{
				// A new definition starts on a struct with none chosen; adding a variable is how it moves to variables.
				const UCrowdyServerObjectDefinition* Current = Definition.Get();
				const bool bUsesStruct = bVariables && Current && Current->StateForm == ECrowdyServerValuesForm::Struct && Current->State;
				return bUsesStruct ? EVisibility::Hidden : EVisibility::Visible;
			})
			.OnClicked_Lambda([this, bVariables]()
			{
				bVariables ? AddVariable() : AddFunction();
				return FReply::Handled();
			})
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("Icons.Plus"))
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
		];
	return Row;
}

TSharedRef<SWidget> SCrowdyServerObjectMembers::MakeMemberRow(const FItemPtr& Item)
{
	const bool bVariable = Item->Type == FItem::EType::Variable;
	const bool bBuiltIn = Item->Type == FItem::EType::BuiltIn;
	TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			SNew(SImage)
			.Image(FAppStyle::GetBrush(bVariable ? "Kismet.VariableList.TypeIcon" : "GraphEditor.Function_16x"))
			.ColorAndOpacity(bVariable ? Item->TypeColor : FSlateColor::UseForeground())
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(Item->Label)
			.Font(IDetailLayoutBuilder::GetDetailFont())
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(6.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SImage)
			.Image(FAppStyle::GetBrush("Icons.Warning"))
			.ColorAndOpacity(FStyleColors::Warning)
			.DesiredSizeOverride(FVector2D(16.0f, 16.0f))
			.ToolTipText(Item->Problem)
			.Visibility(Item->Problem.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
		]
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		.Padding(10.0f, 0.0f, 6.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(Item->Detail)
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			.ToolTipText(bBuiltIn ? FText::GetEmpty() : Item->Detail)
		];
	if (!bVariable)
	{
		Row->SetToolTipText(bBuiltIn ? SCrowdyServerObjectMembersDetail::BuiltInToolTip(Item->Name) : LOCTEXT("FunctionRowToolTip", "Double-click to open it in the server code"));
		Row->SetEnabled(!bBuiltIn);
		Row->AddSlot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
				.Padding(FMargin(5.0f, 1.0f))
				.Visibility(Item->Tag.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
				.ToolTipText(bBuiltIn ? FText::GetEmpty() : FText::Format(LOCTEXT("CallerTagToolTip", "Callable By {0}"), Item->Tag))
				[
					SNew(STextBlock)
					.Text(Item->Tag)
					.Font(IDetailLayoutBuilder::GetDetailFont())
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			];
		return Row;
	}
	const FName Name = Item->Name;
	const bool bVisible = Item->bVisibleToPlayers;
	Row->AddSlot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ToolTipText(bVisible
				? LOCTEXT("VisibleToolTip", "Visible to Players: players can read and watch it. Click to keep it on the server only.")
				: LOCTEXT("HiddenToolTip", "Server Only: players never see it. Click to make it visible to players."))
			.OnClicked_Lambda([this, Name, bVisible]()
			{
				SetVisibleToPlayers(Name, !bVisible);
				return FReply::Handled();
			})
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush(bVisible ? "Kismet.VariableList.ExposeForInstance" : "Kismet.VariableList.HideForInstance"))
				.ColorAndOpacity(bVisible ? FSlateColor::UseForeground() : FSlateColor::UseSubduedForeground())
			]
		];
	return Row;
}

TSharedPtr<SWidget> SCrowdyServerObjectMembers::MakeContextMenu()
{
	const TArray<FItemPtr> Selected = Tree->GetSelectedItems();
	const UCrowdyServerObjectDefinition* Current = Definition.Get();
	if (Selected.IsEmpty() || !Current)
	{
		return nullptr;
	}
	const FItemPtr Item = Selected[0];
	if (Item->Type == FItem::EType::TypeSettings || Item->Type == FItem::EType::BuiltIns || Item->Type == FItem::EType::BuiltIn)
	{
		return nullptr;
	}
	FMenuBuilder Menu(true, nullptr);
	switch (Item->Type)
	{
	case FItem::EType::Variables:
		if (Current->StateForm == ECrowdyServerValuesForm::List)
		{
			Menu.AddMenuEntry(LOCTEXT("AddVariable", "Add Variable"), FText::GetEmpty(), FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SCrowdyServerObjectMembers::AddVariable)));
		}
		break;
	case FItem::EType::Functions:
		Menu.AddMenuEntry(LOCTEXT("AddFunction", "Add Function"), FText::GetEmpty(), FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SCrowdyServerObjectMembers::AddFunction)));
		break;
	case FItem::EType::Variable:
		if (Current->StateForm == ECrowdyServerValuesForm::List || Item->bStale)
		{
			Menu.AddMenuEntry(LOCTEXT("DeleteVariable", "Delete"), LOCTEXT("DeleteVariableToolTip", "Removes this variable"), FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Delete"),
				FUIAction(FExecuteAction::CreateSP(this, &SCrowdyServerObjectMembers::RemoveVariable, Item->Name)));
		}
		break;
	case FItem::EType::Function:
		Menu.AddMenuEntry(LOCTEXT("DuplicateFunction", "Duplicate"), FText::GetEmpty(), FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Duplicate"),
			FUIAction(FExecuteAction::CreateSP(this, &SCrowdyServerObjectMembers::DuplicateFunction, Item->Index)));
		Menu.AddMenuEntry(LOCTEXT("DeleteFunction", "Delete"), LOCTEXT("DeleteFunctionToolTip", "Removes this function"), FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Delete"),
			FUIAction(FExecuteAction::CreateSP(this, &SCrowdyServerObjectMembers::RemoveFunction, Item->Index)));
		break;
	default:
		break;
	}
	return Menu.MakeWidget();
}

void SCrowdyServerObjectMembers::EditDefinition(const FText& Description, TFunctionRef<void(UCrowdyServerObjectDefinition&)> Edit)
{
	UCrowdyServerObjectDefinition* Current = Definition.Get();
	if (!Current)
	{
		return;
	}
	const FScopedTransaction Transaction(Description);
	Current->Modify();
	Edit(*Current);
	Current->PostEditChange();
}

void SCrowdyServerObjectMembers::AddVariable()
{
	const UCrowdyServerObjectDefinition* Current = Definition.Get();
	if (!Current)
	{
		return;
	}
	const FName Name = SCrowdyServerObjectMembersDetail::UniqueName(TEXT("NewVar"), [Current](FName Candidate)
	{
		return Current->StateList.FindPropertyDescByName(Candidate) != nullptr;
	});
	EditDefinition(LOCTEXT("AddVariableTransaction", "Add Variable"), [Name](UCrowdyServerObjectDefinition& Edited)
	{
		Edited.StateForm = ECrowdyServerValuesForm::List;
		Edited.StateList.AddProperty(Name, EPropertyBagPropertyType::Int32);
	});
	Selection->SelectVariable(Name);
}

void SCrowdyServerObjectMembers::RemoveVariable(FName Name)
{
	EditDefinition(LOCTEXT("DeleteVariableTransaction", "Delete Variable"), [Name](UCrowdyServerObjectDefinition& Edited)
	{
		Edited.StateList.RemovePropertyByName(Name);
		Edited.WatchedFields.Remove(Name);
	});
	Selection->SelectNone();
}

void SCrowdyServerObjectMembers::SetVisibleToPlayers(FName Name, bool bVisible)
{
	EditDefinition(LOCTEXT("VisibleTransaction", "Change Visible to Players"), [Name, bVisible](UCrowdyServerObjectDefinition& Edited)
	{
		CrowdyServerCodeFiles::SetWatchedField(Edited.WatchedFields, Name, bVisible);
	});
}

void SCrowdyServerObjectMembers::SetVariablesUseStruct(bool bUseStruct)
{
	EditDefinition(LOCTEXT("UseStructTransaction", "Change Use Struct for Variables"), [bUseStruct](UCrowdyServerObjectDefinition& Edited)
	{
		Edited.StateForm = bUseStruct ? ECrowdyServerValuesForm::Struct : ECrowdyServerValuesForm::List;
	});
	Selection->SelectNone();
}

void SCrowdyServerObjectMembers::AddFunction()
{
	const UCrowdyServerObjectDefinition* Current = Definition.Get();
	if (!Current)
	{
		return;
	}
	const FName Name = SCrowdyServerObjectMembersDetail::UniqueName(TEXT("NewFunction"), [Current](FName Candidate)
	{
		return Current->Functions.ContainsByPredicate([Candidate](const FCrowdyServerFunction& Function) { return Function.Name == Candidate; });
	});
	EditDefinition(LOCTEXT("AddFunctionTransaction", "Add Function"), [Name](UCrowdyServerObjectDefinition& Edited)
	{
		FCrowdyServerFunction& Added = Edited.Functions.AddDefaulted_GetRef();
		Added.Name = Name;
		Added.ParamsForm = ECrowdyServerValuesForm::List;
		Added.ReplyForm = ECrowdyServerValuesForm::List;
	});
	Selection->SelectFunction(Current->Functions.Num() - 1);
}

void SCrowdyServerObjectMembers::DuplicateFunction(int32 Index)
{
	const UCrowdyServerObjectDefinition* Current = Definition.Get();
	if (!Current || !Current->Functions.IsValidIndex(Index))
	{
		return;
	}
	const FString Base = Current->Functions[Index].Name.ToString() + TEXT("Copy");
	const FName Name = SCrowdyServerObjectMembersDetail::UniqueName(*Base, [Current](FName Candidate)
	{
		return Current->Functions.ContainsByPredicate([Candidate](const FCrowdyServerFunction& Function) { return Function.Name == Candidate; });
	});
	EditDefinition(LOCTEXT("DuplicateFunctionTransaction", "Duplicate Function"), [Index, Name](UCrowdyServerObjectDefinition& Edited)
	{
		FCrowdyServerFunction Copy = Edited.Functions[Index];
		Copy.Name = Name;
		Copy.ServerName.Reset();
		Edited.Functions.Insert(MoveTemp(Copy), Index + 1);
	});
	Selection->SelectFunction(Index + 1);
}

void SCrowdyServerObjectMembers::RemoveFunction(int32 Index)
{
	EditDefinition(LOCTEXT("DeleteFunctionTransaction", "Delete Function"), [Index](UCrowdyServerObjectDefinition& Edited)
	{
		if (Edited.Functions.IsValidIndex(Index))
		{
			Edited.Functions.RemoveAt(Index);
		}
	});
	Selection->SelectNone();
}

void SCrowdyServerObjectMembers::HandleSelectionChanged(FItemPtr Item, ESelectInfo::Type SelectInfo)
{
	if (bSelecting)
	{
		return;
	}
	TGuardValue<bool> Guard(bSelecting, true);
	SelectItem(Item, *Selection);
	// A click on empty space shows the type's settings; a section heading stays selected for its context menu.
	const FItemPtr Shown = Item.IsValid() ? FItemPtr() : FindRow(Roots, *Selection);
	if (Shown.IsValid())
	{
		Tree->SetSelection(Shown, ESelectInfo::Direct);
	}
}

void SCrowdyServerObjectMembers::HandleDoubleClick(FItemPtr Item)
{
	const UCrowdyServerObjectDefinition* Current = Definition.Get();
	if (!Item.IsValid() || Item->Type != FItem::EType::Function || !Current || !Current->Functions.IsValidIndex(Item->Index))
	{
		return;
	}
	OnOpenFunction.ExecuteIfBound(Current->Functions[Item->Index].GetMethodName());
}

void SCrowdyServerObjectMembers::HandleObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& Event)
{
	if (Object != Definition.Get() || (Event.ChangeType & EPropertyChangeType::Interactive) != 0 || RebuildTicker.IsValid())
	{
		return;
	}
	// On the next tick, so a row is never rebuilt while it handles its own click.
	RebuildTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateSP(this, &SCrowdyServerObjectMembers::HandleDeferredRebuild));
}

bool SCrowdyServerObjectMembers::HandleDeferredRebuild(float DeltaTime)
{
	RebuildTicker.Reset();
	Rebuild();
	return false;
}

void SCrowdyServerObjectMembers::HandleExternalSelection()
{
	if (bSelecting)
	{
		return;
	}
	RestoreSelection(false);
}

#undef LOCTEXT_NAMESPACE
