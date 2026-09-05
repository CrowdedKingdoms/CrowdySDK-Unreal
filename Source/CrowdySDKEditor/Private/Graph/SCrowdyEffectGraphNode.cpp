// Fill out your copyright notice in the Description page of Project Settings.

#include "Graph/SCrowdyEffectGraphNode.h"

#include "EdGraph/EdGraph.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Graph/CrowdyEffectGraphNodeOptions.h"
#include "Graph/CrowdyEffectGraphNodes.h"
#include "SGraphPin.h"
#include "ScopedTransaction.h"
#include "TimerManager.h"
#include "UObject/Class.h"
#include "UObject/ReflectedTypeAccessors.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SCrowdyEffectGraphNode"

void SCrowdyEffectGraphNode::Construct(const FArguments& InArgs, UCrowdyEffectGraphNode* InNode)
{
	EffectNode = InNode;
	GraphNode = InNode;
	SetCursor(EMouseCursor::CardinalCross);
	UpdateGraphNode();

	if (GraphNode)
	{
		bCachedHasError = GraphNode->bHasCompilerMessage;
		CachedErrorType = GraphNode->ErrorType;
		CachedErrorMsg = GraphNode->ErrorMsg;
	}
}

void SCrowdyEffectGraphNode::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SGraphNode::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

	if (!GraphNode)
	{
		return;
	}

	// A compile stamps or clears this node's error badge on the UEdGraphNode. Rebuild the node widget when that state
	// changes so the badge appears or disappears, the same way the Blueprint node widgets refresh after a compile.
	if (GraphNode->bHasCompilerMessage != bCachedHasError
		|| GraphNode->ErrorType != CachedErrorType
		|| !GraphNode->ErrorMsg.Equals(CachedErrorMsg))
	{
		bCachedHasError = GraphNode->bHasCompilerMessage;
		CachedErrorType = GraphNode->ErrorType;
		CachedErrorMsg = GraphNode->ErrorMsg;
		UpdateGraphNode();
	}
}

void SCrowdyEffectGraphNode::CreateBelowPinControls(TSharedPtr<SVerticalBox> MainBox)
{
	if (!MainBox.IsValid() || !EffectNode.IsValid())
	{
		return;
	}

	MainBox->AddSlot()
	.AutoHeight()
	.Padding(6.f, 2.f, 6.f, 6.f)
	[
		BuildInlineBody()
	];
}

void SCrowdyEffectGraphNode::AddPin(const TSharedRef<SGraphPin>& PinToAdd)
{
	// The output column stretches to the node's height, so one leading filler slot pushes the single result pin to the
	// bottom, level with the last argument instead of the first.
	const UEdGraphPin* Pin = PinToAdd->GetPinObj();
	if (Pin && Pin->Direction == EGPD_Output
		&& RightNodeBox.IsValid() && RightNodeBox->NumSlots() == 0
		&& EffectNode.IsValid() && EffectNode->IsA<UCrowdyEffectGraphNode_Call>())
	{
		RightNodeBox->AddSlot()
		.FillHeight(1.f)
		[
			SNullWidget::NullWidget
		];
	}

	SGraphNode::AddPin(PinToAdd);
}

void SCrowdyEffectGraphNode::CommitNodeChange(bool bReconstructPins)
{
	// Defer the pin reconstruct and the graph-changed broadcast (which can rebuild this very widget) to the next tick,
	// so the current Slate callback unwinds first. The editor's timer manager owns the deferred work, so it is safe
	// even if this widget is torn down by the reconstruct; only a weak node pointer is captured.
	TWeakObjectPtr<UCrowdyEffectGraphNode> WeakNode = EffectNode;
	auto Apply = [WeakNode, bReconstructPins]()
	{
		if (UCrowdyEffectGraphNode* Node = WeakNode.Get())
		{
			if (bReconstructPins)
			{
				Node->Modify();
				Node->ReconstructNode();
			}
			if (UEdGraph* Graph = Node->GetGraph())
			{
				Graph->NotifyGraphChanged();
			}
		}
	};

	if (GEditor)
	{
		GEditor->GetTimerManager()->SetTimerForNextTick(FTimerDelegate::CreateLambda(Apply));
	}
	else
	{
		Apply();
	}
}

template <typename TEnum>
TSharedRef<SWidget> SCrowdyEffectGraphNode::MakeEnumCombo(
	TFunction<TEnum()> Get, TFunction<void(TEnum)> Set, bool bReconstructOnChange, const FText& TransactionLabel)
{
	return SNew(SComboButton)
	.OnGetMenuContent_Lambda([this, Set, bReconstructOnChange, TransactionLabel]()
	{
		FMenuBuilder MenuBuilder(/*bCloseAfterSelection*/ true, nullptr);
		const UEnum* EnumType = StaticEnum<TEnum>();
		if (EnumType)
		{
			// NumEnums includes the generated _MAX sentinel, which is hidden; skip it and any Hidden-marked entry.
			for (int32 Index = 0; Index < EnumType->NumEnums() - 1; ++Index)
			{
				if (EnumType->HasMetaData(TEXT("Hidden"), Index))
				{
					continue;
				}
				const TEnum Value = static_cast<TEnum>(EnumType->GetValueByIndex(Index));
				MenuBuilder.AddMenuEntry(
					EnumType->GetDisplayNameTextByIndex(Index), FText::GetEmpty(), FSlateIcon(),
					FUIAction(FExecuteAction::CreateLambda([this, Set, Value, bReconstructOnChange, TransactionLabel]()
					{
						if (UCrowdyEffectGraphNode* Node = EffectNode.Get())
						{
							const FScopedTransaction Transaction(TransactionLabel);
							Node->Modify();
							Set(Value);
							CommitNodeChange(bReconstructOnChange);
						}
					})));
			}
		}
		return MenuBuilder.MakeWidget();
	})
	.ButtonContent()
	[
		SNew(STextBlock)
		.Text_Lambda([Get]()
		{
			const UEnum* EnumType = StaticEnum<TEnum>();
			return EnumType ? EnumType->GetDisplayNameTextByValue(static_cast<int64>(Get())) : FText::GetEmpty();
		})
	];
}

TSharedRef<SWidget> SCrowdyEffectGraphNode::MakeTextField(
	TFunction<FString()> Get, TFunction<void(const FString&)> Set, const FText& HintText, const FText& TransactionLabel)
{
	return SNew(SEditableTextBox)
	.HintText(HintText)
	.MinDesiredWidth(120.f)
	.Text_Lambda([Get]() { return FText::FromString(Get()); })
	.OnTextCommitted_Lambda([this, Get, Set, TransactionLabel](const FText& NewText, ETextCommit::Type)
	{
		const FString NewValue = NewText.ToString();
		if (NewValue.Equals(Get()))
		{
			return;
		}
		if (UCrowdyEffectGraphNode* Node = EffectNode.Get())
		{
			const FScopedTransaction Transaction(TransactionLabel);
			Node->Modify();
			Set(NewValue);

			// A Result write's / condition's pin label follows the attribute name, but a plain value node's text does not
			// reshape pins; either way the graph changed, so refresh the preview without a pin reconstruct.
			CommitNodeChange(false);
		}
	});
}

TSharedRef<SWidget> SCrowdyEffectGraphNode::MakeBoolLiteralField()
{
	return SNew(SComboButton)
	.OnGetMenuContent_Lambda([this]()
	{
		FMenuBuilder MenuBuilder(/*bCloseAfterSelection*/ true, nullptr);

		// The stored literal stays the language's lowercase "true" / "false"; only the label follows Unreal's casing.
		for (const TCHAR* Choice : { TEXT("true"), TEXT("false") })
		{
			const FString Value = Choice;
			MenuBuilder.AddMenuEntry(
				FText::FromString(CrowdyEffectGraphNodeOptions::BoolLiteralDisplayName(Value)),
				FText::GetEmpty(), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([this, Value]()
				{
					if (UCrowdyEffectGraphNode_Constant* Constant = Cast<UCrowdyEffectGraphNode_Constant>(EffectNode.Get()))
					{
						const FScopedTransaction Transaction(LOCTEXT("SetBoolLiteralTx", "Set Constant Value"));
						Constant->Modify();
						Constant->Literal = Value;
						CommitNodeChange(false);
					}
				})));
		}
		return MenuBuilder.MakeWidget();
	})
	.ButtonContent()
	[
		SNew(STextBlock)
		.Text_Lambda([this]()
		{
			const UCrowdyEffectGraphNode_Constant* Constant = Cast<UCrowdyEffectGraphNode_Constant>(EffectNode.Get());
			const FString Value = Constant ? Constant->Literal.TrimStartAndEnd() : FString();
			return Value.IsEmpty()
				? LOCTEXT("BoolChoose", "(choose)")
				: FText::FromString(CrowdyEffectGraphNodeOptions::BoolLiteralDisplayName(Value));
		})
	];
}

TSharedRef<SWidget> SCrowdyEffectGraphNode::MakeAttributePicker(
	TFunction<FString()> Get, TFunction<void(const FString&)> Set, const FText& TransactionLabel,
	TFunction<ECrowdyEffectRole()> GetRole)
{
	auto Commit = [this, Set, TransactionLabel](const FString& NewValue)
	{
		if (UCrowdyEffectGraphNode* Node = EffectNode.Get())
		{
			const FScopedTransaction Transaction(TransactionLabel);
			Node->Modify();
			Set(NewValue);
			CommitNodeChange(false);
		}
	};

	return SNew(SComboButton)
	.ToolTipText(LOCTEXT("AttrPickerTip", "Pick an attribute of the effect's container class, or type any name"))
	.OnGetMenuContent_Lambda([this, Get, Commit, GetRole]()
	{
		FMenuBuilder MenuBuilder(/*bCloseAfterSelection*/ true, nullptr);

		MenuBuilder.AddWidget(
			SNew(SBox)
			.WidthOverride(180.f)
			[
				SNew(SEditableTextBox)
				.HintText(LOCTEXT("AttrCustomHint", "type an attribute name"))
				.Text(FText::FromString(Get()))
				.OnTextCommitted_Lambda([Commit](const FText& NewText, ETextCommit::Type CommitType)
				{
					if (CommitType == ETextCommit::OnEnter)
					{
						Commit(NewText.ToString());
						FSlateApplication::Get().DismissAllMenus();
					}
				})
			],
			FText::GetEmpty());

		const UCrowdyEffect* Effect = CrowdyEffectGraphNodeOptions::OwningEffect(EffectNode.Get());
		const ECrowdyEffectRole Role = GetRole ? GetRole() : ECrowdyEffectRole::Target;
		const TArray<FString> Options = CrowdyEffectGraphNodeOptions::AttributeOptions(Effect, Role);
		if (Options.IsEmpty())
		{
			MenuBuilder.AddMenuEntry(
				LOCTEXT("AttrPickerEmpty", "Set the effect's container class to list attributes"),
				FText::GetEmpty(), FSlateIcon(),
				FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([] { return false; })));
			return MenuBuilder.MakeWidget();
		}

		MenuBuilder.BeginSection(NAME_None, LOCTEXT("AttrPickerSection", "Attributes"));
		for (const FString& Option : Options)
		{
			MenuBuilder.AddMenuEntry(FText::FromString(Option), FText::GetEmpty(), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([Commit, Option]() { Commit(Option); })));
		}
		MenuBuilder.EndSection();
		return MenuBuilder.MakeWidget();
	})
	.ButtonContent()
	[
		SNew(STextBlock)
		.Text_Lambda([Get]()
		{
			const FString Value = Get().TrimStartAndEnd();
			return Value.IsEmpty() ? LOCTEXT("AttrPickerEmptyLabel", "(attribute)") : FText::FromString(Value);
		})
	];
}

TSharedRef<SWidget> SCrowdyEffectGraphNode::BuildInlineBody()
{
	UCrowdyEffectGraphNode* Node = EffectNode.Get();
	if (!Node)
	{
		return SNullWidget::NullWidget;
	}

	if (UCrowdyEffectGraphNode_Constant* Constant = Cast<UCrowdyEffectGraphNode_Constant>(Node))
	{
		TWeakObjectPtr<UCrowdyEffectGraphNode_Constant> WeakConstant = Constant;
		auto IsBool = [WeakConstant]()
		{
			const UCrowdyEffectGraphNode_Constant* C = WeakConstant.Get();
			return C && C->ConstantType == ECrowdyEffectGraphConstantType::Bool;
		};
		auto IsNull = [WeakConstant]()
		{
			const UCrowdyEffectGraphNode_Constant* C = WeakConstant.Get();
			return C && C->ConstantType == ECrowdyEffectGraphConstantType::Null;
		};

		return SNew(SVerticalBox)

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.f, 0.f, 0.f, 2.f)
		[
			// Changing the type flips whether the output pin is boolean, so reconstruct the pins on change.
			MakeEnumCombo<ECrowdyEffectGraphConstantType>(
				[WeakConstant]() { return WeakConstant.IsValid() ? WeakConstant->ConstantType : ECrowdyEffectGraphConstantType::Number; },
				[WeakConstant](ECrowdyEffectGraphConstantType V) { if (WeakConstant.IsValid()) { WeakConstant->ConstantType = V; } },
				/*bReconstructOnChange*/ true, LOCTEXT("ConstTypeTx", "Change Constant Type"))
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBox)
			.Visibility_Lambda([IsBool]() { return IsBool() ? EVisibility::Visible : EVisibility::Collapsed; })
			[
				MakeBoolLiteralField()
			]
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBox)
			.Visibility_Lambda([IsBool, IsNull]()
			{
				return (IsBool() || IsNull()) ? EVisibility::Collapsed : EVisibility::Visible;
			})
			[
				MakeTextField(
					[WeakConstant]() { return WeakConstant.IsValid() ? WeakConstant->Literal : FString(); },
					[WeakConstant](const FString& V) { if (WeakConstant.IsValid()) { WeakConstant->Literal = V; } },
					LOCTEXT("ConstValueHint", "value"), LOCTEXT("ConstValueTx", "Set Constant Value"))
			]
		];
	}

	if (UCrowdyEffectGraphNode_Attribute* Attribute = Cast<UCrowdyEffectGraphNode_Attribute>(Node))
	{
		TWeakObjectPtr<UCrowdyEffectGraphNode_Attribute> WeakAttr = Attribute;
		return SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(0.f, 0.f, 4.f, 0.f)
		[
			MakeEnumCombo<ECrowdyEffectRole>(
				[WeakAttr]() { return WeakAttr.IsValid() ? WeakAttr->Role : ECrowdyEffectRole::Target; },
				[WeakAttr](ECrowdyEffectRole V) { if (WeakAttr.IsValid()) { WeakAttr->Role = V; } },
				/*bReconstructOnChange*/ false, LOCTEXT("AttrRoleTx", "Change Attribute Role"))
		]

		+ SHorizontalBox::Slot()
		.FillWidth(1.f)
		[
			MakeAttributePicker(
				[WeakAttr]() { return WeakAttr.IsValid() ? WeakAttr->Attribute : FString(); },
				[WeakAttr](const FString& V) { if (WeakAttr.IsValid()) { WeakAttr->Attribute = V; } },
				LOCTEXT("AttrNameTx", "Set Attribute Name"),
				[WeakAttr]() { return WeakAttr.IsValid() ? WeakAttr->Role : ECrowdyEffectRole::Target; })
		];
	}

	if (UCrowdyEffectGraphNode_ReadRef* ReadRef = Cast<UCrowdyEffectGraphNode_ReadRef>(Node))
	{
		TWeakObjectPtr<UCrowdyEffectGraphNode_ReadRef> WeakRef = ReadRef;
		return MakeAttributePicker(
			[WeakRef]() { return WeakRef.IsValid() ? WeakRef->Attribute : FString(); },
			[WeakRef](const FString& V) { if (WeakRef.IsValid()) { WeakRef->Attribute = V; } },
			LOCTEXT("RefAttrNameTx", "Set Attribute Name"),
			// A Read Ref names an arbitrary container resolved at runtime, not the effect's Target or Source, so it
			// always suggests the target's attributes as a best-effort hint.
			[]() { return ECrowdyEffectRole::Target; });
	}

	if (UCrowdyEffectGraphNode_BinaryOp* Binary = Cast<UCrowdyEffectGraphNode_BinaryOp>(Node))
	{
		TWeakObjectPtr<UCrowdyEffectGraphNode_BinaryOp> WeakOp = Binary;
		return MakeEnumCombo<ECrowdyEffectGraphArithOp>(
			[WeakOp]() { return WeakOp.IsValid() ? WeakOp->Op : ECrowdyEffectGraphArithOp::Add; },
			[WeakOp](ECrowdyEffectGraphArithOp V) { if (WeakOp.IsValid()) { WeakOp->Op = V; } },
			/*bReconstructOnChange*/ false, LOCTEXT("ArithOpTx", "Change Operator"));
	}

	if (UCrowdyEffectGraphNode_Compare* Compare = Cast<UCrowdyEffectGraphNode_Compare>(Node))
	{
		TWeakObjectPtr<UCrowdyEffectGraphNode_Compare> WeakCmp = Compare;
		return MakeEnumCombo<ECrowdyEffectComparator>(
			[WeakCmp]() { return WeakCmp.IsValid() ? WeakCmp->Comparator : ECrowdyEffectComparator::GreaterOrEqual; },
			[WeakCmp](ECrowdyEffectComparator V) { if (WeakCmp.IsValid()) { WeakCmp->Comparator = V; } },
			/*bReconstructOnChange*/ false, LOCTEXT("CmpTx", "Change Comparator"));
	}

	if (UCrowdyEffectGraphNode_Logic* Logic = Cast<UCrowdyEffectGraphNode_Logic>(Node))
	{
		TWeakObjectPtr<UCrowdyEffectGraphNode_Logic> WeakLogic = Logic;
		return MakeEnumCombo<ECrowdyEffectGraphLogicOp>(
			[WeakLogic]() { return WeakLogic.IsValid() ? WeakLogic->Op : ECrowdyEffectGraphLogicOp::And; },
			[WeakLogic](ECrowdyEffectGraphLogicOp V) { if (WeakLogic.IsValid()) { WeakLogic->Op = V; } },
			/*bReconstructOnChange*/ false, LOCTEXT("LogicOpTx", "Change Operator"));
	}

	if (UCrowdyEffectGraphNode_Unary* Unary = Cast<UCrowdyEffectGraphNode_Unary>(Node))
	{
		TWeakObjectPtr<UCrowdyEffectGraphNode_Unary> WeakUnary = Unary;
		// Not and Negate carry different pin categories (boolean vs value), so reconstruct on change.
		return MakeEnumCombo<ECrowdyEffectGraphUnaryOp>(
			[WeakUnary]() { return WeakUnary.IsValid() ? WeakUnary->Op : ECrowdyEffectGraphUnaryOp::Not; },
			[WeakUnary](ECrowdyEffectGraphUnaryOp V) { if (WeakUnary.IsValid()) { WeakUnary->Op = V; } },
			/*bReconstructOnChange*/ true, LOCTEXT("UnaryOpTx", "Change Operator"));
	}

	if (UCrowdyEffectGraphNode_Tuning* Tuning = Cast<UCrowdyEffectGraphNode_Tuning>(Node))
	{
		TWeakObjectPtr<UCrowdyEffectGraphNode_Tuning> WeakTuning = Tuning;
		return MakeTextField(
			[WeakTuning]() { return WeakTuning.IsValid() ? WeakTuning->ParamName : FString(); },
			[WeakTuning](const FString& V) { if (WeakTuning.IsValid()) { WeakTuning->ParamName = V; } },
			LOCTEXT("ParamHint", "parameter name"), LOCTEXT("ParamTx", "Set Parameter Name"));
	}

	if (UCrowdyEffectGraphNode_Call* Call = Cast<UCrowdyEffectGraphNode_Call>(Node))
	{
		TWeakObjectPtr<UCrowdyEffectGraphNode_Call> WeakCall = Call;

		// A builtin's name comes from the palette entry that placed it and its arity is fixed by the language, so its
		// node body carries neither field: everything about it is already decided. A server call is the opposite, since
		// only the author knows the name and how many arguments their function takes.
		const FCrowdyEffectBuiltinCall* Builtin = Call->bIsFnCall
			? nullptr
			: CrowdyEffectGraphNodeOptions::FindBuiltin(Call->Callee);
		const bool bShowNameField = Call->bIsFnCall;
		const bool bShowArgCount = Call->bIsFnCall || !Builtin || !Builtin->IsFixedArity();
		if (!bShowNameField && !bShowArgCount)
		{
			return SNullWidget::NullWidget;
		}

		const int32 MinArgs = Builtin ? Builtin->MinArgs : 0;
		const int32 MaxArgs = Builtin ? Builtin->MaxArgs : 16;

		TSharedRef<SVerticalBox> Body = SNew(SVerticalBox);

		if (bShowNameField)
		{
			Body->AddSlot()
			.AutoHeight()
			.Padding(0.f, 0.f, 0.f, 2.f)
			[
				MakeTextField(
					[WeakCall]() { return WeakCall.IsValid() ? WeakCall->Callee : FString(); },
					[WeakCall](const FString& V) { if (WeakCall.IsValid()) { WeakCall->Callee = V; } },
					LOCTEXT("ServerCalleeHint", "server function name"),
					LOCTEXT("CalleeTx", "Set Server Function Name"))
			];
		}

		if (bShowArgCount)
		{
			Body->AddSlot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.f, 0.f, 4.f, 0.f)
				[
					SNew(STextBlock).Text(LOCTEXT("ArgCountLabel", "Arguments"))
				]

				+ SHorizontalBox::Slot()
				.FillWidth(1.f)
				[
					SNew(SSpinBox<int32>)
					.MinValue(MinArgs)
					.MaxValue(MaxArgs)
					.MinSliderValue(MinArgs)
					.MaxSliderValue(MaxArgs)
					.Value_Lambda([WeakCall]() { return WeakCall.IsValid() ? WeakCall->ArgCount : 0; })
					.OnValueCommitted_Lambda([this, WeakCall, MinArgs, MaxArgs](int32 NewValue, ETextCommit::Type)
					{
						UCrowdyEffectGraphNode_Call* C = WeakCall.Get();
						const int32 Clamped = FMath::Clamp(NewValue, MinArgs, MaxArgs);
						if (!C || C->ArgCount == Clamped)
						{
							return;
						}
						const FScopedTransaction Transaction(LOCTEXT("ArgCountTx", "Change Argument Count"));
						C->Modify();
						C->ArgCount = Clamped;

						// The argument count reshapes the node's input pins, so reconstruct after the commit unwinds.
						CommitNodeChange(true);
					})
				]
			];
		}

		return Body;
	}

	// Result and Select have no inline body; they render as plain nodes (and still show error badges).
	return SNullWidget::NullWidget;
}

TSharedPtr<SGraphNode> FCrowdyEffectGraphNodeFactory::CreateNode(UEdGraphNode* Node) const
{
	if (UCrowdyEffectGraphNode* EffectNode = Cast<UCrowdyEffectGraphNode>(Node))
	{
		return SNew(SCrowdyEffectGraphNode, EffectNode);
	}
	return nullptr;
}

#undef LOCTEXT_NAMESPACE
