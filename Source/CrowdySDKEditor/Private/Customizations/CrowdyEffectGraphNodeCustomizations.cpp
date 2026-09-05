// Fill out your copyright notice in the Description page of Project Settings.

#include "Customizations/CrowdyEffectGraphNodeCustomizations.h"

#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Graph/CrowdyEffectGraphNodeOptions.h"
#include "Graph/CrowdyEffectGraphNodes.h"
#include "IDetailChildrenBuilder.h"
#include "IDetailPropertyRow.h"
#include "PropertyHandle.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"
#include "ScopedTransaction.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CrowdyEffectGraphNodeCustomizations"

namespace
{
	// The UCrowdyEffect owning the node(s) a property handle edits. Every handle here belongs to a graph node whose
	// outer is the effect (the graph is the effect's editor-only subobject), so the effect is the node's typed outer.
	const UCrowdyEffect* EffectFromHandle(const TSharedRef<IPropertyHandle>& Handle)
	{
		TArray<UObject*> Outers;
		Handle->GetOuterObjects(Outers);
		for (UObject* Outer : Outers)
		{
			if (Outer)
			{
				if (const UCrowdyEffect* Effect = Outer->GetTypedOuter<UCrowdyEffect>())
				{
					return Effect;
				}
			}
		}
		return nullptr;
	}

	// A free-text box plus a "Pick" combo of suggested values. Typing commits any string (the picker never restricts
	// to the list); the dropdown just lists the options the source supplies, or a disabled note when there are none.
	// LabelFor renders an option for display without changing what gets stored, so a builtin can list as "To String"
	// while the effect language still receives "to_string".
	TSharedRef<SWidget> MakeNamePickerWidget(
		TSharedRef<IPropertyHandle> StringHandle, TFunction<TArray<FString>()> OptionSource,
		FText PickTooltip, FText EmptyMessage, TFunction<FString(const FString&)> LabelFor = nullptr)
	{
		return SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.FillWidth(1.f)
		.VAlign(VAlign_Center)
		[
			StringHandle->CreatePropertyValueWidget()
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(4.f, 0.f, 0.f, 0.f)
		[
			SNew(SComboButton)
			.ToolTipText(PickTooltip)
			.OnGetMenuContent_Lambda([StringHandle, OptionSource, EmptyMessage, LabelFor]()
			{
				FMenuBuilder MenuBuilder(/*bCloseAfterSelection*/ true, nullptr);

				const TArray<FString> Options = OptionSource ? OptionSource() : TArray<FString>();
				if (Options.IsEmpty())
				{
					MenuBuilder.AddMenuEntry(EmptyMessage, FText::GetEmpty(), FSlateIcon(),
						FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([] { return false; })));
					return MenuBuilder.MakeWidget();
				}

				for (const FString& Option : Options)
				{
					const FString Label = LabelFor ? LabelFor(Option) : Option;
					MenuBuilder.AddMenuEntry(FText::FromString(Label), FText::GetEmpty(), FSlateIcon(),
						FUIAction(FExecuteAction::CreateLambda([StringHandle, Option]()
						{
							StringHandle->SetValue(Option);
						})));
				}
				return MenuBuilder.MakeWidget();
			})
			.ButtonContent()
			[
				SNew(STextBlock).Text(LOCTEXT("Pick", "Pick"))
			]
		];
	}

	// Replace one node property's default row with a name-picker (free text + "Pick"), keeping its position and label.
	void ReplaceStringField(IDetailLayoutBuilder& DetailBuilder, TSharedRef<IPropertyHandle> Handle,
		TFunction<TArray<FString>()> Options, FText Tip, FText Empty,
		TFunction<FString(const FString&)> LabelFor = nullptr)
	{
		if (!Handle->IsValidHandle())
		{
			return;
		}
		IDetailPropertyRow* Row = DetailBuilder.EditDefaultProperty(Handle);
		if (!Row)
		{
			return;
		}
		Row->CustomWidget()
		.NameContent()
		[
			Handle->CreatePropertyNameWidget()
		]
		.ValueContent()
		.MinDesiredWidth(250.f)
		[
			MakeNamePickerWidget(Handle, MoveTemp(Options), Tip, Empty, MoveTemp(LabelFor))
		];
	}

	// True when the selected node is a Constant currently in Bool mode, so the Literal shows a true/false picker.
	bool IsBoolConstant(const TWeakObjectPtr<UObject>& WeakNode)
	{
		const UCrowdyEffectGraphNode_Constant* Constant = Cast<UCrowdyEffectGraphNode_Constant>(WeakNode.Get());
		return Constant && Constant->ConstantType == ECrowdyEffectGraphConstantType::Bool;
	}

	// A strict true/false chooser for a Bool constant's Literal, so it can only ever hold "true" or "false".
	TSharedRef<SWidget> MakeBoolLiteralWidget(TSharedRef<IPropertyHandle> LiteralHandle)
	{
		return SNew(SComboButton)
		.ToolTipText(LOCTEXT("BoolLiteralTip", "This constant's boolean value"))
		.OnGetMenuContent_Lambda([LiteralHandle]()
		{
			FMenuBuilder MenuBuilder(/*bCloseAfterSelection*/ true, nullptr);

			// The stored literal stays the language's lowercase "true" / "false"; only the label follows Unreal's casing.
			for (const TCHAR* Choice : { TEXT("true"), TEXT("false") })
			{
				const FString Value = Choice;
				MenuBuilder.AddMenuEntry(
					FText::FromString(CrowdyEffectGraphNodeOptions::BoolLiteralDisplayName(Value)),
					FText::GetEmpty(), FSlateIcon(),
					FUIAction(FExecuteAction::CreateLambda([LiteralHandle, Value]() { LiteralHandle->SetValue(Value); })));
			}
			return MenuBuilder.MakeWidget();
		})
		.ButtonContent()
		[
			SNew(STextBlock)
			.Text_Lambda([LiteralHandle]()
			{
				FString Value;
				LiteralHandle->GetValue(Value);
				return Value.TrimStartAndEnd().IsEmpty()
					? LOCTEXT("BoolChoose", "(choose True or False)")
					: FText::FromString(CrowdyEffectGraphNodeOptions::BoolLiteralDisplayName(Value));
			})
		];
	}

	// Replaces a Constant node's Literal row with a value editor that suits the constant's type: a true/false picker
	// in Bool mode, the plain text box otherwise (Null hides the Literal already, via the property's EditCondition).
	void CustomizeConstantLiteral(IDetailLayoutBuilder& DetailBuilder, TWeakObjectPtr<UObject> WeakNode)
	{
		const TSharedRef<IPropertyHandle> LiteralHandle = DetailBuilder.GetProperty(
			GET_MEMBER_NAME_CHECKED(UCrowdyEffectGraphNode_Constant, Literal), UCrowdyEffectGraphNode_Constant::StaticClass());
		if (!LiteralHandle->IsValidHandle())
		{
			return;
		}
		IDetailPropertyRow* Row = DetailBuilder.EditDefaultProperty(LiteralHandle);
		if (!Row)
		{
			return;
		}
		Row->CustomWidget()
		.NameContent()
		[
			LiteralHandle->CreatePropertyNameWidget()
		]
		.ValueContent()
		.MinDesiredWidth(250.f)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.FillWidth(1.f)
			[
				SNew(SBox)
				.Visibility_Lambda([WeakNode]()
				{
					return IsBoolConstant(WeakNode) ? EVisibility::Visible : EVisibility::Collapsed;
				})
				[
					MakeBoolLiteralWidget(LiteralHandle)
				]
			]

			+ SHorizontalBox::Slot()
			.FillWidth(1.f)
			[
				SNew(SBox)
				.Visibility_Lambda([WeakNode]()
				{
					return IsBoolConstant(WeakNode) ? EVisibility::Collapsed : EVisibility::Visible;
				})
				[
					LiteralHandle->CreatePropertyValueWidget()
				]
			]
		];
	}

	// The Tuning node's trimmed parameter name, or empty.
	FString TuningParamName(const TWeakObjectPtr<UObject>& WeakNode)
	{
		const UCrowdyEffectGraphNode_Tuning* Tuning = Cast<UCrowdyEffectGraphNode_Tuning>(WeakNode.Get());
		return Tuning ? Tuning->ParamName.TrimStartAndEnd() : FString();
	}

	bool MagnitudeDeclared(const UCrowdyEffect* Effect, const FString& Name)
	{
		return Effect && Effect->Magnitudes.ContainsByPredicate([&Name](const FCrowdyEffectMagnitude& Magnitude)
		{
			return Magnitude.Name.TrimStartAndEnd().Equals(Name, ESearchCase::IgnoreCase);
		});
	}

	// Declares the Tuning node's $param as a magnitude on the owning effect (if it is not already), then nudges the
	// graph so the effect's own Details panel (its Magnitudes list) refreshes through the toolkit's graph-changed hook.
	void CreateMagnitudeFromTuning(TWeakObjectPtr<UObject> WeakNode)
	{
		UCrowdyEffectGraphNode_Tuning* Tuning = Cast<UCrowdyEffectGraphNode_Tuning>(WeakNode.Get());
		if (!Tuning)
		{
			return;
		}
		const FString Name = Tuning->ParamName.TrimStartAndEnd();
		UCrowdyEffect* Effect = Tuning->GetTypedOuter<UCrowdyEffect>();
		if (Name.IsEmpty() || !Effect || MagnitudeDeclared(Effect, Name))
		{
			return;
		}

		const FScopedTransaction Transaction(LOCTEXT("CreateMagnitudeTx", "Create Tuning Parameter"));
		Effect->Modify();
		FCrowdyEffectMagnitude Magnitude;
		Magnitude.Name = Name;
		Effect->Magnitudes.Add(MoveTemp(Magnitude));
		Effect->PostEditChange();

		if (UEdGraph* Graph = Tuning->GetGraph())
		{
			Graph->NotifyGraphChanged();
		}
	}

	// Adds a "Return" button to the Result node's details, so the common "answer with what I just wrote" shape is one
	// click instead of an attribute node placed and wired by hand.
	void AddReturnDefaultRow(IDetailLayoutBuilder& DetailBuilder, TWeakObjectPtr<UObject> WeakNode)
	{
		auto PlanFor = [WeakNode]()
		{
			return CrowdyEffectReturnDefault::Plan(Cast<UCrowdyEffectGraphNode_Result>(WeakNode.Get()));
		};

		IDetailCategoryBuilder& Category = DetailBuilder.EditCategory("Result");
		Category.AddCustomRow(LOCTEXT("ReturnDefaultFilter", "Return the new value"))
		.NameContent()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("ReturnDefaultName", "Shortcut"))
			.Font(DetailBuilder.GetDetailFont())
		]
		.ValueContent()
		.MinDesiredWidth(300.f)
		[
			SNew(SButton)
			.HAlign(HAlign_Center)
			.IsEnabled_Lambda([PlanFor]() { return PlanFor().bAvailable; })
			.ToolTipText_Lambda([PlanFor]()
			{
				const CrowdyEffectReturnDefault::FPlan Planned = PlanFor();
				return Planned.bAvailable
					? LOCTEXT("ReturnDefaultTip",
						"Reads the attribute back after the write and wires it into the Return pin, so the caller is "
						"answered with the value this effect just produced.")
					: Planned.Message;
			})
			.OnClicked_Lambda([WeakNode]()
			{
				CrowdyEffectReturnDefault::Apply(Cast<UCrowdyEffectGraphNode_Result>(WeakNode.Get()));
				return FReply::Handled();
			})
			[
				SNew(STextBlock).Text_Lambda([PlanFor]() { return PlanFor().Message; })
			]
		];
	}

	// Adds a "Declare" button to the Tuning node's details, so naming a parameter on the node also exposes it on the
	// effect without hunting for the Tuning Parameters list.
	void AddCreateMagnitudeRow(IDetailLayoutBuilder& DetailBuilder, TWeakObjectPtr<UObject> WeakNode)
	{
		IDetailCategoryBuilder& Category = DetailBuilder.EditCategory("Tuning");
		Category.AddCustomRow(LOCTEXT("CreateMagnitudeFilter", "Create Tuning Parameter"))
		.NameContent()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("CreateMagnitudeName", "Declare"))
			.Font(DetailBuilder.GetDetailFont())
		]
		.ValueContent()
		.MinDesiredWidth(250.f)
		[
			SNew(SButton)
			.HAlign(HAlign_Center)
			.ToolTipText(LOCTEXT("CreateMagnitudeTip",
				"Expose this parameter on the effect, so it can be balanced without editing the graph"))
			.IsEnabled_Lambda([WeakNode]()
			{
				const FString Name = TuningParamName(WeakNode);
				const UCrowdyEffect* Effect = WeakNode.IsValid() ? WeakNode->GetTypedOuter<UCrowdyEffect>() : nullptr;
				return !Name.IsEmpty() && Effect && !MagnitudeDeclared(Effect, Name);
			})
			.OnClicked_Lambda([WeakNode]() { CreateMagnitudeFromTuning(WeakNode); return FReply::Handled(); })
			[
				SNew(STextBlock)
				.Text_Lambda([WeakNode]()
				{
					const FString Name = TuningParamName(WeakNode);
					if (Name.IsEmpty())
					{
						return LOCTEXT("CreateMagnitudeEmpty", "Enter a parameter name first");
					}
					const UCrowdyEffect* Effect = WeakNode.IsValid() ? WeakNode->GetTypedOuter<UCrowdyEffect>() : nullptr;
					if (MagnitudeDeclared(Effect, Name))
					{
						return FText::Format(LOCTEXT("CreateMagnitudeDone", "'{0}' is already a tuning parameter"),
							FText::FromString(Name));
					}
					return FText::Format(LOCTEXT("CreateMagnitudeAdd", "Declare tuning parameter '{0}'"),
						FText::FromString(Name));
				})
			]
		];
	}
}

CrowdyEffectReturnDefault::FPlan CrowdyEffectReturnDefault::Plan(const UCrowdyEffectGraphNode_Result* Result)
{
	FPlan Planned;
	if (!Result)
	{
		Planned.Message = LOCTEXT("ReturnDefaultNoNode", "Select the Result node to return a value");
		return Planned;
	}
	// Turning the return on and leaving the pin empty is the state an author lands in first, since the checkbox is
	// the only control that mentions returning. That state does not return anything yet, so the shortcut has to
	// stay available: refusing on the flag alone would switch the shortcut off exactly where it helps most, while
	// the graph is failing to compile for want of a wire.
	const UEdGraphPin* ExistingReturn =
		Result->FindPin(UCrowdyEffectGraphNode_Result::ReturnPinName(), EGPD_Input);
	if (Result->bReturnsValue && ExistingReturn && ExistingReturn->LinkedTo.Num() > 0)
	{
		Planned.Message = LOCTEXT("ReturnDefaultAlready", "This effect already returns a value");
		return Planned;
	}
	if (Result->Writes.Num() == 0)
	{
		Planned.Message = LOCTEXT("ReturnDefaultNoWrites",
			"Add a write first, or turn on 'Returns a value' and wire the Return pin yourself");
		return Planned;
	}
	if (Result->Writes.Num() > 1)
	{
		// Which of several writes the caller wants back is the author's call, not a guess worth making for them.
		Planned.Message = LOCTEXT("ReturnDefaultManyWrites",
			"This effect writes more than one attribute, so wire the Return pin yourself");
		return Planned;
	}

	const FCrowdyEffectGraphWrite& Write = Result->Writes[0];
	const FString Attribute = Write.Attribute.TrimStartAndEnd();
	if (Attribute.IsEmpty())
	{
		Planned.Message = LOCTEXT("ReturnDefaultNoAttribute", "Name the write's attribute first");
		return Planned;
	}

	// Reads the same way the write's own pin does, so the button and the pin describe one attribute identically.
	const FString Phrase = Write.TargetRole == ECrowdyEffectRole::Source
		? FString::Printf(TEXT("Source's %s"), *Attribute)
		: Attribute;

	Planned.bAvailable = true;
	Planned.Role = Write.TargetRole;
	Planned.Attribute = Attribute;
	Planned.Message = FText::Format(
		LOCTEXT("ReturnDefaultAvailable", "Return the new value of {0}"), FText::FromString(Phrase));
	return Planned;
}

bool CrowdyEffectReturnDefault::Apply(UCrowdyEffectGraphNode_Result* Result)
{
	const FPlan Planned = Plan(Result);
	if (!Planned.bAvailable)
	{
		return false;
	}

	UEdGraph* Graph = Result->GetGraph();
	if (!Graph)
	{
		return false;
	}

	const FScopedTransaction Transaction(LOCTEXT("ReturnDefaultTx", "Return The New Value"));
	Graph->Modify();
	Result->Modify();

	// The author may already have turned the return on and left the pin empty, so restore what they had rather than
	// assuming this call is what switched it on.
	const bool bReturnedBefore = Result->bReturnsValue;
	auto Rollback = [Result, bReturnedBefore, Graph](UEdGraphNode* PartialNode)
	{
		if (PartialNode)
		{
			Graph->RemoveNode(PartialNode);
		}
		Result->bReturnsValue = bReturnedBefore;
		Result->ReconstructNode();
		Graph->NotifyGraphChanged();
	};

	// Turning the flag on does not by itself create the Return pin: pins come from the node's pin allocation, so the
	// node has to be rebuilt before there is anything to wire into. The pin's stable identity is stamped first, since
	// the rebuild matches pins by that identity and it is what carries the new wire through every later edit.
	Result->bReturnsValue = true;
	if (!Result->ReturnPinId.IsValid())
	{
		Result->ReturnPinId = FGuid::NewGuid();
	}
	Result->ReconstructNode();

	UEdGraphPin* ReturnPin = Result->FindPin(UCrowdyEffectGraphNode_Result::ReturnPinName(), EGPD_Input);
	if (!ReturnPin)
	{
		// Nothing to wire into, so leave the node as it was rather than half-applied.
		Rollback(nullptr);
		return false;
	}

	UCrowdyEffectGraphNode_Attribute* Read =
		NewObject<UCrowdyEffectGraphNode_Attribute>(Graph, NAME_None, RF_Transactional);
	Read->Role = Planned.Role;
	Read->Attribute = Planned.Attribute;
	Graph->AddNode(Read, /*bFromUI*/ true, /*bSelectNewNode*/ false);
	Read->CreateNewGuid();
	Read->PostPlacedNewNode();
	Read->AllocateDefaultPins();

	// Left of the Result node and level with the Return pin, which sits below every write and condition, so the new
	// wire runs the same left-to-right way as the rest of the graph instead of doubling back over it.
	Read->NodePosX = Result->NodePosX - 320;
	Read->NodePosY = Result->NodePosY + 64 * FMath::Max(0, Result->Pins.Num() - 1);

	UEdGraphPin* ValuePin = Read->FindPin(UCrowdyEffectGraphNode_Attribute::OutputPinName(), EGPD_Output);
	if (!ValuePin)
	{
		// The read cannot carry a value, so take it back out instead of leaving it stranded in the graph.
		Rollback(Read);
		return false;
	}

	ReturnPin->Modify();
	ValuePin->MakeLinkTo(ReturnPin);

	Graph->NotifyGraphChanged();
	return true;
}

TSharedRef<IDetailCustomization> FCrowdyEffectGraphNodeCustomization::MakeInstance()
{
	return MakeShared<FCrowdyEffectGraphNodeCustomization>();
}

void FCrowdyEffectGraphNodeCustomization::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailBuilder.GetObjectsBeingCustomized(Objects);
	if (Objects.Num() != 1 || !Objects[0].IsValid())
	{
		return;
	}

	const TWeakObjectPtr<UObject> WeakNode = Objects[0];
	UObject* NodeObject = WeakNode.Get();

	// Resolve the owning effect fresh each time an option list is built, so nothing captures a raw effect pointer that
	// could outlive the asset.
	auto EffectOf = [WeakNode]() -> const UCrowdyEffect*
	{
		return WeakNode.IsValid() ? WeakNode->GetTypedOuter<UCrowdyEffect>() : nullptr;
	};

	// Re-reads the Attribute node's Role each time the option list is built (the picker asks for it every time its
	// dropdown opens, not once here), so flipping Role between Target and Source afterwards changes which
	// container's keys the menu offers on its next open.
	auto AttributeRoleOf = [WeakNode]() -> ECrowdyEffectRole
	{
		const UCrowdyEffectGraphNode_Attribute* AttrNode = Cast<UCrowdyEffectGraphNode_Attribute>(WeakNode.Get());
		return AttrNode ? AttrNode->Role : ECrowdyEffectRole::Target;
	};

	if (NodeObject->IsA<UCrowdyEffectGraphNode_Attribute>())
	{
		ReplaceStringField(DetailBuilder,
			DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UCrowdyEffectGraphNode_Attribute, Attribute),
				UCrowdyEffectGraphNode_Attribute::StaticClass()),
			[EffectOf, AttributeRoleOf]() { return CrowdyEffectGraphNodeOptions::AttributeOptions(EffectOf(), AttributeRoleOf()); },
			LOCTEXT("AttrPickTip", "Pick a Server Owned attribute of the effect's container class"),
			LOCTEXT("AttrPickEmpty", "Set the effect's container class first"));
	}
	else if (NodeObject->IsA<UCrowdyEffectGraphNode_ReadRef>())
	{
		ReplaceStringField(DetailBuilder,
			DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UCrowdyEffectGraphNode_ReadRef, Attribute),
				UCrowdyEffectGraphNode_ReadRef::StaticClass()),
			[EffectOf]() { return CrowdyEffectGraphNodeOptions::AttributeOptions(EffectOf()); },
			LOCTEXT("RefAttrPickTip",
				"Suggested from the effect's container class; the referenced container's type may differ, so any name is allowed"),
			LOCTEXT("RefAttrPickEmpty", "No attributes to suggest"));
	}
	else if (NodeObject->IsA<UCrowdyEffectGraphNode_Tuning>())
	{
		ReplaceStringField(DetailBuilder,
			DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UCrowdyEffectGraphNode_Tuning, ParamName),
				UCrowdyEffectGraphNode_Tuning::StaticClass()),
			[EffectOf]() { return CrowdyEffectGraphNodeOptions::MagnitudeOptions(EffectOf()); },
			LOCTEXT("ParamPickTip", "Pick one of this effect's declared tuning parameters"),
			LOCTEXT("ParamPickEmpty", "No tuning parameters declared on this effect"));
		AddCreateMagnitudeRow(DetailBuilder, WeakNode);
	}
	else if (NodeObject->IsA<UCrowdyEffectGraphNode_Constant>())
	{
		CustomizeConstantLiteral(DetailBuilder, WeakNode);
	}
	else if (NodeObject->IsA<UCrowdyEffectGraphNode_Result>())
	{
		AddReturnDefaultRow(DetailBuilder, WeakNode);
	}
	else if (const UCrowdyEffectGraphNode_Call* Call = Cast<UCrowdyEffectGraphNode_Call>(NodeObject))
	{
		if (Call->bIsFnCall)
		{
			// Every other effect on this container type that returns a value. Only a returning function can answer
			// a fn: call, so a function that runs but returns nothing is deliberately left off. A formula is never
			// offered here: the graph compiler never expands an effect's own formulas into what it lowers, so a
			// graph-authored fn: call naming one would name a function that does not exist on the server.
			ReplaceStringField(DetailBuilder,
				DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UCrowdyEffectGraphNode_Call, Callee),
					UCrowdyEffectGraphNode_Call::StaticClass()),
				[EffectOf]() { return CrowdyEffectGraphNodeOptions::FnCallOptions(EffectOf()); },
				LOCTEXT("CallPickTip", "Pick a function that returns a value: another effect on this container type"),
				LOCTEXT("CallPickEmpty",
					"No function is offered here yet. If one should be, its index may still be catching up; "
					"otherwise, give an effect on this container type a return, then try again."));
		}
		else if (const FCrowdyEffectBuiltinCall* Builtin = CrowdyEffectGraphNodeOptions::FindBuiltin(Call->Callee))
		{
			// A builtin with one legal arity has nothing to configure, so its argument count is not offered at all
			// rather than offered and then silently corrected.
			if (Builtin->IsFixedArity())
			{
				DetailBuilder.HideProperty(
					DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UCrowdyEffectGraphNode_Call, ArgCount),
						UCrowdyEffectGraphNode_Call::StaticClass()));
			}
		}
	}
}

TSharedRef<IPropertyTypeCustomization> FCrowdyEffectGraphWriteCustomization::MakeInstance()
{
	return MakeShared<FCrowdyEffectGraphWriteCustomization>();
}

void FCrowdyEffectGraphWriteCustomization::CustomizeHeader(TSharedRef<IPropertyHandle> PropertyHandle,
	FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	const TSharedPtr<IPropertyHandle> RoleHandle =
		PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FCrowdyEffectGraphWrite, TargetRole));
	const TSharedPtr<IPropertyHandle> AttributeHandle =
		PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FCrowdyEffectGraphWrite, Attribute));
	const TSharedPtr<IPropertyHandle> OpHandle =
		PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FCrowdyEffectGraphWrite, Op));
	if (!RoleHandle.IsValid() || !AttributeHandle.IsValid() || !OpHandle.IsValid())
	{
		HeaderRow.NameContent()[ PropertyHandle->CreatePropertyNameWidget() ];
		return;
	}

	const TSharedRef<IPropertyHandle> AttributeRef = AttributeHandle.ToSharedRef();
	const TSharedRef<IPropertyHandle> RoleRef = RoleHandle.ToSharedRef();

	HeaderRow
	.NameContent()
	[
		PropertyHandle->CreatePropertyNameWidget()
	]
	.ValueContent()
	.MinDesiredWidth(360.f)
	.MaxDesiredWidth(640.f)
	[
		SNew(SHorizontalBox)

		// Whose attribute the write targets (Target / Source).
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(0.f, 0.f, 4.f, 0.f)
		[
			RoleHandle->CreatePropertyValueWidget()
		]

		// The attribute, free text plus a "Pick" dropdown of the container class's Server Owned attributes.
		+ SHorizontalBox::Slot()
		.FillWidth(1.f)
		.Padding(0.f, 0.f, 4.f, 0.f)
		[
			MakeNamePickerWidget(AttributeRef,
				[AttributeRef, RoleRef]()
				{
					// Read TargetRole live rather than once at row-build time, so switching Target/Source and then
					// reopening the dropdown offers the newly-relevant container's attributes.
					uint8 RoleByte = static_cast<uint8>(ECrowdyEffectRole::Target);
					RoleRef->GetValue(RoleByte);
					return CrowdyEffectGraphNodeOptions::AttributeOptions(
						EffectFromHandle(AttributeRef), static_cast<ECrowdyEffectRole>(RoleByte));
				},
				LOCTEXT("WriteAttrTip", "Pick a Server Owned attribute of the effect's container class"),
				LOCTEXT("WriteAttrEmpty", "Set the effect's container class first"))
		]

		// The assignment operator (Set / Add / ...).
		+ SHorizontalBox::Slot()
		.AutoWidth()
		[
			OpHandle->CreatePropertyValueWidget()
		]
	];
}

void FCrowdyEffectGraphWriteCustomization::CustomizeChildren(TSharedRef<IPropertyHandle> PropertyHandle,
	IDetailChildrenBuilder& ChildBuilder, IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	// The whole write reads on the header row; nothing expands below it (the write's PinId is internal, never shown).
}

TSharedRef<IPropertyTypeCustomization> FCrowdyEffectGraphRequireCustomization::MakeInstance()
{
	return MakeShared<FCrowdyEffectGraphRequireCustomization>();
}

void FCrowdyEffectGraphRequireCustomization::CustomizeHeader(TSharedRef<IPropertyHandle> PropertyHandle,
	FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	const TSharedPtr<IPropertyHandle> NoteHandle =
		PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FCrowdyEffectGraphRequire, Note));
	if (!NoteHandle.IsValid())
	{
		HeaderRow.NameContent()[ PropertyHandle->CreatePropertyNameWidget() ];
		return;
	}

	const int32 Index = PropertyHandle->GetIndexInArray();
	const TSharedRef<IPropertyHandle> NoteRef = NoteHandle.ToSharedRef();

	// The note is optional, so the row reads from whatever is wired into the condition's pin when it is blank. A bare
	// "Index [0]" beside an empty box gave no hint that the condition was already driven by the graph.
	auto WiredText = [PropertyHandle, Index]() -> FString
	{
		TArray<UObject*> Outers;
		PropertyHandle->GetOuterObjects(Outers);
		for (UObject* Outer : Outers)
		{
			if (const UCrowdyEffectGraphNode_Result* Result = Cast<UCrowdyEffectGraphNode_Result>(Outer))
			{
				return Result->DescribeWiredCondition(Index);
			}
		}
		return FString();
	};

	HeaderRow
	.NameContent()
	[
		SNew(STextBlock)
		.Text(FText::Format(LOCTEXT("ConditionRowName", "Condition {0}"), FText::AsNumber(Index + 1)))
		.Font(CustomizationUtils.GetRegularFont())
	]
	.ValueContent()
	.MinDesiredWidth(300.f)
	[
		SNew(SEditableTextBox)
		.ToolTipText(LOCTEXT("ConditionNoteTip",
			"An optional note describing what this condition checks. The condition itself is the boolean wired into "
			"this row's pin on the Result node."))
		.HintText_Lambda([WiredText]()
		{
			const FString Wired = WiredText();
			return Wired.IsEmpty()
				? LOCTEXT("ConditionNoteHintUnwired", "wire a boolean into this condition's pin")
				: FText::Format(LOCTEXT("ConditionNoteHintWired", "{0}"), FText::FromString(Wired));
		})
		.Text_Lambda([NoteRef]()
		{
			FString Value;
			NoteRef->GetValue(Value);
			return FText::FromString(Value);
		})
		.OnTextCommitted_Lambda([NoteRef](const FText& NewText, ETextCommit::Type)
		{
			FString Current;
			NoteRef->GetValue(Current);
			const FString Committed = NewText.ToString();
			if (!Current.Equals(Committed))
			{
				NoteRef->SetValue(Committed);
			}
		})
	];
}

void FCrowdyEffectGraphRequireCustomization::CustomizeChildren(TSharedRef<IPropertyHandle> PropertyHandle,
	IDetailChildrenBuilder& ChildBuilder, IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	// The condition's only authored field is its Note, shown on the header row; the boolean it gates is wired into
	// the node's condition pin, not authored here.
}

#undef LOCTEXT_NAMESPACE
