#include "CrowdyServerObjectDefinitionCustomization.h"

#include "CrowdyExecInternal.h"
#include "CrowdyServerCodeFiles.h"
#include "CrowdyServerComputeService.h"
#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectSelection.h"
#include "CrowdyServerTimerCustomization.h"
#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "EdGraphSchema_K2.h"
#include "IDetailGroup.h"
#include "IDetailPropertyRow.h"
#include "IPropertyUtilities.h"
#include "InstancedPropertyBagStructureDataProvider.h"
#include "PropertyBagDetails.h"
#include "PropertyCustomizationHelpers.h"
#include "PropertyHandle.h"
#include "SPinTypeSelector.h"
#include "ScopedTransaction.h"
#include "Math/UnitConversion.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/NumericUnitTypeInterface.inl"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SHyperlink.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CrowdyServerObjectDefinitionCustomization"

namespace CrowdyServerObjectDefinitionCustomizationDetail
{
	const FName ServerObjectCategory(TEXT("Server Object"));
	const FName ServerFunctionsCategory(TEXT("Server Functions"));
	const FName ServerCodeCategory(TEXT("Server Code"));
	const FName ServerNamesCategory(TEXT("Server Names"));
	const FName BakedCategory(TEXT("Baked"));
	const FName AdvancedCategory(TEXT("Advanced"));
	const FName AccessCategory(TEXT("Access"));
	const FName MembersCategory(TEXT("Members"));
	const FName TimersCategory(TEXT("Timers & Events"));
	const FName CanCallCategory(TEXT("Can Call"));
	constexpr float ValueMinWidth = 250.0f;
	/** The widths Details gives a plain text or number property. */
	constexpr float TextMinWidth = 125.0f;
	constexpr float TextMaxWidth = 600.0f;

	TSharedRef<SWidget> Note(const FText& Text)
	{
		return SNew(STextBlock)
			.Text(Text)
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.ColorAndOpacity(FSlateColor::UseSubduedForeground());
	}

	TSharedRef<SWidget> WarningIcon()
	{
		return SNew(SImage)
			.Image(FAppStyle::GetBrush("Icons.Warning"))
			.ColorAndOpacity(FStyleColors::Warning)
			.DesiredSizeOverride(FVector2D(16.0f, 16.0f));
	}

	/** An input or output row: the Blueprint type pill, rename, delete and drag to reorder. */
	constexpr EPropertyBagChildRowFeatures PinRowFeatures = EPropertyBagChildRowFeatures::Renaming | EPropertyBagChildRowFeatures::Deletion
		| EPropertyBagChildRowFeatures::DragAndDrop | EPropertyBagChildRowFeatures::CompactTypeSelector | EPropertyBagChildRowFeatures::DropDownMenuButton
		| EPropertyBagChildRowFeatures::Menu_TypeSelector | EPropertyBagChildRowFeatures::Menu_Rename | EPropertyBagChildRowFeatures::Menu_Delete;

	/** Blueprint's variable type picker, offering only what a Server Object can carry, for one of the definition's variables. */
	TSharedRef<SWidget> MakeTypePicker(TWeakObjectPtr<UCrowdyServerObjectDefinition> Weak, TSharedPtr<IPropertyHandle> Variables, FName Name)
	{
		const FGetPinTypeTree Tree = FGetPinTypeTree::CreateLambda([Weak](TArray<FPinTypeTreeItem>& OutTree, ETypeTreeFilter Filter)
		{
			GetDefault<UEdGraphSchema_K2>()->GetVariableTypeTree(OutTree, Filter);
			const UCrowdyServerObjectDefinition* Current = Weak.Get();
			if (!Current)
			{
				return;
			}
			OutTree.RemoveAll([Current](const FPinTypeTreeItem& Item) { return !Item.IsValid() || !Current->IsServerValueTypeAccepted(Item->GetPinType(false), false); });
			for (const FPinTypeTreeItem& Item : OutTree)
			{
				Item->Children.RemoveAll([Current](const FPinTypeTreeItem& Child) { return !Child.IsValid() || !Current->IsServerValueTypeAccepted(Child->GetPinType(false), true); });
			}
		});
		auto FindDesc = [Weak, Name]() -> const FPropertyBagPropertyDesc*
		{
			const UCrowdyServerObjectDefinition* Current = Weak.Get();
			const UPropertyBag* Bag = Current ? Current->StateList.GetPropertyBagStruct() : nullptr;
			return Bag ? Bag->FindPropertyDescByName(Name) : nullptr;
		};
		return SNew(SPinTypeSelector, Tree)
			.Schema(GetDefault<UEdGraphSchema_K2>())
			.TypeTreeFilter(ETypeTreeFilter::None)
			.SelectorType(SPinTypeSelector::ESelectorType::Full)
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.TargetPinType_Lambda([FindDesc]()
			{
				const FPropertyBagPropertyDesc* Desc = FindDesc();
				return Desc ? UE::StructUtils::GetPropertyDescAsPin(*Desc) : FEdGraphPinType();
			})
			.OnPinTypeChanged_Lambda([FindDesc, Variables](const FEdGraphPinType& NewType)
			{
				const FPropertyBagPropertyDesc* Desc = FindDesc();
				if (!Desc)
				{
					return;
				}
				UE::StructUtils::ApplyChangesToSinglePropertyDesc(LOCTEXT("ChangeVariableType", "Change Variable Type"), FPropertyBagPropertyDesc(*Desc), Variables,
					[&NewType](FPropertyBagPropertyDesc& Edited) { UE::StructUtils::SetPropertyDescFromPin(Edited, NewType); });
			});
	}

	/** The function as its Call node reads: Tip (Amount) returns Total. */
	FText NodeLine(const FCrowdyServerFunction& Function)
	{
		return FText::Format(LOCTEXT("NodeLine", "{0} {1}"), FText::FromName(Function.Name), CrowdyServerObjectText::Signature(Function, false));
	}

	/** One side of a signature: a List's values or a struct's travelling fields, labelled as the Call node shows its pins, or the struct's own name. */
	FString SignatureSide(ECrowdyServerValuesForm Form, const UScriptStruct* Struct, const FInstancedPropertyBag& List, bool bFieldNames)
	{
		TArray<FString> Names;
		if (Form == ECrowdyServerValuesForm::List)
		{
			const UPropertyBag* Bag = List.GetPropertyBagStruct();
			for (const FPropertyBagPropertyDesc& Desc : Bag ? Bag->GetPropertyDescs() : TConstArrayView<FPropertyBagPropertyDesc>())
			{
				Names.Add(FName::NameToDisplayString(Desc.Name.ToString(), Desc.ValueType == EPropertyBagPropertyType::Bool));
			}
			return FString::Join(Names, TEXT(", "));
		}
		if (!Struct || !bFieldNames)
		{
			return Struct ? CrowdyExec::DisplayName(Struct) : FString();
		}
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			if (CrowdyExec::IsTravellingProperty(*It))
			{
				Names.Add(FName::NameToDisplayString(It->GetAuthoredName(), It->IsA<FBoolProperty>()));
			}
		}
		return FString::Join(Names, TEXT(", "));
	}

	/** The fractional digits a Value Range box shows and saves, so a stored bound is never rounded. */
	constexpr int32 RangeFractionalDigits = 15;

	/** A Value Range bound as it is saved: what SetBound accepts. */
	FString RangeBoundText(int64 Value)
	{
		return LexToString(Value);
	}

	FString RangeBoundText(double Value)
	{
		// Fixed-point with RangeFractionalDigits digits, as the bake refuses an exponent.
		FString Text = FString::Printf(TEXT("%.15f"), Value);
		while (Text.EndsWith(TEXT("0"), ESearchCase::CaseSensitive))
		{
			Text.LeftChopInline(1);
		}
		if (Text.EndsWith(TEXT("."), ESearchCase::CaseSensitive))
		{
			Text.LeftChopInline(1);
		}
		return Text;
	}

	/** Adds an input or output named like Blueprint's, NewParam, then NewParam_1 and on. */
	FReply AddPin(TSharedPtr<IPropertyHandle> List)
	{
		const UPropertyBag* Bag = UE::StructUtils::GetCommonBagStruct(List);
		FName Name(TEXT("NewParam"));
		for (int32 Suffix = 1; Bag && Bag->FindPropertyDescByName(Name); ++Suffix)
		{
			Name = FName(*FString::Printf(TEXT("NewParam_%d"), Suffix));
		}
		UE::StructUtils::ApplyChangesToPropertyDescs(LOCTEXT("AddPin", "Add Pin"), List,
			[Name](TArray<FPropertyBagPropertyDesc>& Descs) { Descs.Emplace(Name, EPropertyBagPropertyType::Int32); });
		return FReply::Handled();
	}

	/** Members and Leader callers are refused by the bake while the object has no Members From. */
	bool CallerNeedsMembers(const UCrowdyServerObjectDefinition* Current, int32 Index)
	{
		if (!Current || !Current->Functions.IsValidIndex(Index) || Current->MembersFrom != ECrowdyServerMembersSource::None)
		{
			return false;
		}
		const ECrowdyServerFunctionCaller Caller = Current->Functions[Index].WhoCanCall;
		return Caller == ECrowdyServerFunctionCaller::Members || Caller == ECrowdyServerFunctionCaller::Leader;
	}

	/** One bound of a List input's Value Range, a whole number or not as the input is; the input is found by its id, so a rename keeps the box working. */
	template <typename NumericType>
	TSharedRef<SWidget> MakeRangeBox(TWeakObjectPtr<UCrowdyServerObjectDefinition> Weak, int32 Index, FGuid Input, TSharedPtr<IPropertyHandle> List, FName Key, const FText& Label)
	{
		auto FindDesc = [Weak, Index, Input]() -> const FPropertyBagPropertyDesc*
		{
			const UCrowdyServerObjectDefinition* Current = Weak.Get();
			const UPropertyBag* Bag = Current && Current->Functions.IsValidIndex(Index) ? Current->Functions[Index].ParamsList.GetPropertyBagStruct() : nullptr;
			return Bag ? Bag->FindPropertyDescByID(Input) : nullptr;
		};
		auto GetBound = [FindDesc, Key]() -> TOptional<NumericType>
		{
			const FPropertyBagPropertyDesc* Desc = FindDesc();
			NumericType Bound{};
			if (!Desc || !Desc->HasMetaData(Key) || !LexTryParseString(Bound, *Desc->GetMetaData(Key)))
			{
				return TOptional<NumericType>();
			}
			return Bound;
		};
		// An empty Typed clears the bound.
		auto Commit = [FindDesc, List, Key](const FString& Typed)
		{
			const FPropertyBagPropertyDesc* Desc = FindDesc();
			if (!Desc)
			{
				return;
			}
			FPropertyBagPropertyDesc Edited = *Desc;
			if (!CrowdyServerValueRange::SetBound(Edited, Key, Typed))
			{
				return;
			}
			UE::StructUtils::ApplyChangesToSinglePropertyDesc(LOCTEXT("ChangeValueRange", "Change Value Range"), FPropertyBagPropertyDesc(*Desc), List,
				[Key, &Typed](FPropertyBagPropertyDesc& Changed) { CrowdyServerValueRange::SetBound(Changed, Key, Typed); });
		};
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(SNumericEntryBox<NumericType>)
				.Font(IDetailLayoutBuilder::GetDetailFont())
				.AllowSpin(false)
				.MinFractionalDigits(TOptional<int32>(0))
				.MaxFractionalDigits(TOptional<int32>(RangeFractionalDigits))
				.UndeterminedString(FText::GetEmpty())
				.LabelVAlign(VAlign_Center)
				.Label()
				[
					SNew(STextBlock)
					.Text(Label)
					.Font(IDetailLayoutBuilder::GetDetailFont())
				]
				.Value(TAttribute<TOptional<NumericType>>::CreateLambda(GetBound))
				.OnValueCommitted_Lambda([Commit, GetBound](NumericType NewValue, ETextCommit::Type)
				{
					// Leaving the box commits too; an unchanged value must not rewrite the stored text.
					const TOptional<NumericType> Stored = GetBound();
					if (Stored.IsSet() && Stored.GetValue() == NewValue)
					{
						return;
					}
					Commit(RangeBoundText(NewValue));
				})
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				PropertyCustomizationHelpers::MakeClearButton(FSimpleDelegate::CreateLambda([Commit]() { Commit(FString()); }),
					LOCTEXT("ClearValueRangeBound", "No limit"), TAttribute<bool>::CreateLambda([GetBound]() { return GetBound().IsSet(); }))
			];
	}
}

bool CrowdyServerValueRange::CanHaveRange(const FPropertyBagPropertyDesc& Desc)
{
	return Desc.ContainerTypes.IsEmpty() && (Desc.IsNumericIntegralType() || Desc.IsNumericFloatType());
}

bool CrowdyServerValueRange::SetBound(FPropertyBagPropertyDesc& Desc, FName Key, const FString& Text)
{
	const FString Bound = Text.TrimStartAndEnd();
	if (Bound.IsEmpty())
	{
		if (!Desc.HasMetaData(Key))
		{
			return false;
		}
		Desc.RemoveMetadata(Key);
		return true;
	}
	// Held to the form the bake accepts: a whole number for a whole-number input, so the server code compares like with like.
	double Real = 0.0;
	int64 Whole = 0;
	const bool bParsed = Desc.IsNumericIntegralType() ? CrowdyExec::ParseRangeWhole(Bound, Whole) : CrowdyExec::ParseRangeNumber(Bound, Real);
	if (!CanHaveRange(Desc) || !bParsed || Desc.GetMetaData(Key).Equals(Bound, ESearchCase::CaseSensitive))
	{
		return false;
	}
	Desc.SetMetaData(Key, Bound);
	return true;
}

TSharedRef<INumericTypeInterface<float>> CrowdyServerObjectText::MakeSecondsInterface()
{
	const TSharedRef<TNumericUnitTypeInterface<float>> Seconds = MakeShared<TNumericUnitTypeInterface<float>>(EUnit::Seconds);
	Seconds->FixedDisplayUnits = EUnit::Seconds;
	Seconds->SetMinFractionalDigits(TOptional<int32>(0));
	return Seconds;
}

FText CrowdyServerObjectText::Signature(const FCrowdyServerFunction& Function, bool bFieldNames)
{
	using namespace CrowdyServerObjectDefinitionCustomizationDetail;
	const FText Inputs = FText::FromString(TEXT("(") + SignatureSide(Function.ParamsForm, Function.Params, Function.ParamsList, bFieldNames) + TEXT(")"));
	const FString Outputs = SignatureSide(Function.ReplyForm, Function.Reply, Function.ReplyList, bFieldNames);
	return Outputs.IsEmpty() ? Inputs : FText::Format(LOCTEXT("SignatureReturns", "{0} returns {1}"), Inputs, FText::FromString(Outputs));
}

FText CrowdyServerObjectText::TimerTitle(const FCrowdyServerTimer& Timer)
{
	const FText Runs = StaticEnum<ECrowdyServerTimerRepeat>()->GetDisplayNameTextByValue(static_cast<int64>(Timer.Repeat));
	const FText Name = Timer.Name.IsNone() ? LOCTEXT("UnnamedTimer", "New Timer") : FText::FromName(Timer.Name);
	return FText::Format(LOCTEXT("TimerTitle", "{0}: {1} {2}"), Name, Runs, FText::FromString(MakeSecondsInterface()->ToString(Timer.Seconds)));
}

void CrowdyServerObjectRows::ShowSeconds(IDetailPropertyRow& Row, const TSharedRef<IPropertyHandle>& Seconds)
{
	const float Min = Seconds->HasMetaData(TEXT("ClampMin")) ? Seconds->GetFloatMetaData(TEXT("ClampMin")) : TNumericLimits<float>::Lowest();
	Row.CustomWidget()
		.NameContent()
		[
			Seconds->CreatePropertyNameWidget()
		]
		.ValueContent()
		.MinDesiredWidth(CrowdyServerObjectDefinitionCustomizationDetail::TextMinWidth)
		.MaxDesiredWidth(CrowdyServerObjectDefinitionCustomizationDetail::TextMinWidth)
		[
			SNew(SNumericEntryBox<float>)
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.AllowSpin(false)
			.MinFractionalDigits(TOptional<int32>(0))
			.TypeInterface(CrowdyServerObjectText::MakeSecondsInterface())
			.Value_Lambda([Seconds]()
			{
				float Value = 0.0f;
				return Seconds->GetValue(Value) == FPropertyAccess::Success ? TOptional<float>(Value) : TOptional<float>();
			})
			.OnValueCommitted_Lambda([Seconds, Min](float NewValue, ETextCommit::Type) { Seconds->SetValue(FMath::Max(NewValue, Min)); })
		];
}

void CrowdyServerObjectRows::ShowUseStruct(IDetailPropertyRow& Row, const TSharedRef<IPropertyHandle>& Form, const FText& Name)
{
	Row.CustomWidget()
		.FilterString(Name)
		.NameContent()
		[
			Form->CreatePropertyNameWidget(Name)
		]
		.ValueContent()
		[
			SNew(SCheckBox)
			.IsChecked_Lambda([Form]()
			{
				uint8 Value = 0;
				if (Form->GetValue(Value) != FPropertyAccess::Success)
				{
					return ECheckBoxState::Undetermined;
				}
				return Value == static_cast<uint8>(ECrowdyServerValuesForm::Struct) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			})
			.OnCheckStateChanged_Lambda([Form](ECheckBoxState State)
			{
				Form->SetValue(static_cast<uint8>(State == ECheckBoxState::Checked ? ECrowdyServerValuesForm::Struct : ECrowdyServerValuesForm::List));
			})
		];
}

FCrowdyServerObjectDefinitionCustomization::FCrowdyServerObjectDefinitionCustomization(TSharedPtr<FCrowdyServerObjectSelection> InSelection)
	: Selection(MoveTemp(InSelection))
{
}

TSharedRef<IDetailCustomization> FCrowdyServerObjectDefinitionCustomization::MakeInstance()
{
	return MakeShared<FCrowdyServerObjectDefinitionCustomization>();
}

TSharedRef<IDetailCustomization> FCrowdyServerObjectDefinitionCustomization::MakeForEditor(TSharedPtr<FCrowdyServerObjectSelection> InSelection)
{
	return MakeShared<FCrowdyServerObjectDefinitionCustomization>(MoveTemp(InSelection));
}

FCrowdyServerObjectDefinitionCustomization::~FCrowdyServerObjectDefinitionCustomization()
{
	Disconnect();
}

void FCrowdyServerObjectDefinitionCustomization::PendingDelete()
{
	Disconnect();
}

void FCrowdyServerObjectDefinitionCustomization::Disconnect()
{
	FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PropertyChangedHandle);
	PropertyChangedHandle.Reset();
	FTSTicker::RemoveTicker(RefreshTicker);
	RefreshTicker.Reset();
	if (const TSharedPtr<FCrowdyServerComputeService> Pinned = Service.Pin())
	{
		Pinned->OnChanged().Remove(ServiceChangedHandle);
	}
	ServiceChangedHandle.Reset();
}

void FCrowdyServerObjectDefinitionCustomization::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	using namespace CrowdyServerObjectDefinitionCustomizationDetail;
	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailBuilder.GetObjectsBeingCustomized(Objects);
	Definition = Objects.Num() == 1 ? Cast<UCrowdyServerObjectDefinition>(Objects[0].Get()) : nullptr;
	TypeNameHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UCrowdyServerObjectDefinition, TypeName));
	WatchedHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UCrowdyServerObjectDefinition, WatchedFields));
	DetailBuilder.HideCategory(ServerCodeCategory);
	DetailBuilder.RegisterInstancedCustomPropertyTypeLayout(FCrowdyServerTimer::StaticStruct()->GetFName(),
		FOnGetPropertyTypeCustomizationInstance::CreateStatic(&FCrowdyServerTimerCustomization::MakeInstance));

	// In the asset editor the Server Object panel lists the variables and functions, and Details shows the one selected.
	const bool bFollowSelection = Selection.IsValid() && Definition.IsValid();
	if (bFollowSelection && Selection->Kind != FCrowdyServerObjectSelection::EKind::None)
	{
		DetailBuilder.HideCategory(ServerObjectCategory);
		DetailBuilder.HideCategory(ServerFunctionsCategory);
		DetailBuilder.HideCategory(ServerNamesCategory);
		DetailBuilder.HideCategory(BakedCategory);
		DetailBuilder.HideCategory(MembersCategory);
		DetailBuilder.HideCategory(TimersCategory);
		DetailBuilder.HideCategory(CanCallCategory);
		if (Selection->Kind == FCrowdyServerObjectSelection::EKind::Function)
		{
			CustomizeFunction(DetailBuilder, Selection->Function);
			return;
		}
		CustomizeVariable(DetailBuilder, Selection->Variable);
		return;
	}

	IDetailCategoryBuilder& ServerObject = DetailBuilder.EditCategory(ServerObjectCategory, FText::GetEmpty(), ECategoryPriority::Important);
	IDetailPropertyRow& TypeNameRow = ServerObject.AddProperty(TypeNameHandle);
	ServerObject.AddProperty(DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UCrowdyServerObjectDefinition, Description)));
	if (bFollowSelection)
	{
		DetailBuilder.HideProperty(DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UCrowdyServerObjectDefinition, StateForm)));
		DetailBuilder.HideProperty(DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UCrowdyServerObjectDefinition, StateList)));
		DetailBuilder.HideProperty(WatchedHandle);
		DetailBuilder.HideCategory(ServerFunctionsCategory);
	}
	else
	{
		const TSharedRef<IPropertyHandle> StateForm = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UCrowdyServerObjectDefinition, StateForm));
		CrowdyServerObjectRows::ShowUseStruct(ServerObject.AddProperty(StateForm), StateForm, LOCTEXT("VariablesUseStruct", "Use Struct for Variables"));
	}
	ServerObject.AddProperty(DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UCrowdyServerObjectDefinition, State)));
	if (!bFollowSelection)
	{
		ServerObject.AddProperty(DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UCrowdyServerObjectDefinition, StateList)));
	}
	IDetailPropertyRow* WatchedRow = bFollowSelection ? nullptr : &ServerObject.AddProperty(WatchedHandle);
	ServerObject.AddProperty(DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UCrowdyServerObjectDefinition, SaveIntervalSeconds)));
	ServerObject.AddProperty(DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UCrowdyServerObjectDefinition, IdleTimeoutSeconds)));
	if (!bFollowSelection)
	{
		DetailBuilder.EditCategory(ServerFunctionsCategory, FText::GetEmpty(), ECategoryPriority::TypeSpecific);
	}
	AddObjectCategories(DetailBuilder);
	AddAdvancedCategory(DetailBuilder);

	// Several definitions selected at once keep the plain rows.
	if (!Definition.IsValid())
	{
		return;
	}
	TypeNameRow.CustomWidget()
		.NameContent()
		[
			TypeNameHandle->CreatePropertyNameWidget()
		]
		.ValueContent()
		.MinDesiredWidth(TextMinWidth)
		.MaxDesiredWidth(TextMaxWidth)
		[
			MakeTypeNameValue()
		];
	if (WatchedRow)
	{
		WatchedRow->CustomWidget()
			.NameContent()
			[
				WatchedHandle->CreatePropertyNameWidget()
			]
			.ValueContent()
			.MinDesiredWidth(ValueMinWidth)
			[
				SAssignNew(PlayersSeeBox, SWrapBox)
				.UseAllottedSize(true)
				.InnerSlotPadding(FVector2D(10.0f, 2.0f))
			];
	}

	PropertyChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddSP(this, &FCrowdyServerObjectDefinitionCustomization::HandleObjectPropertyChanged);
	FCrowdyServerComputeService& Compute = FCrowdyServerComputeService::Get();
	Service = Compute.AsShared();
	ServiceChangedHandle = Compute.OnChanged().AddSP(this, &FCrowdyServerObjectDefinitionCustomization::HandleServiceChanged);
	SeenGeneration = Compute.GetDataGeneration();
	RefreshTypeName();
	RebuildPlayersSee();
	RefreshServiceTypes();
}

void FCrowdyServerObjectDefinitionCustomization::CustomizeFunction(IDetailLayoutBuilder& DetailBuilder, int32 Index)
{
	using namespace CrowdyServerObjectDefinitionCustomizationDetail;
	const UCrowdyServerObjectDefinition* Current = Definition.Get();
	const TSharedPtr<IPropertyHandleArray> Functions = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UCrowdyServerObjectDefinition, Functions))->AsArray();
	uint32 Count = 0;
	if (!Current || !Functions.IsValid() || Functions->GetNumElements(Count) != FPropertyAccess::Success || !Current->Functions.IsValidIndex(Index) || Index >= static_cast<int32>(Count))
	{
		return;
	}
	const TSharedRef<IPropertyHandle> Function = Functions->GetElement(Index);
	IDetailCategoryBuilder& Category = DetailBuilder.EditCategory(TEXT("Function"), LOCTEXT("FunctionCategory", "Function"), ECategoryPriority::Important);
	Category.HeaderContent(
		SNew(SBox)
		.HAlign(HAlign_Right)
		.VAlign(VAlign_Center)
		.Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(NodeLine(Current->Functions[Index]))
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		]);
	Category.AddProperty(Function->GetChildHandle(GET_MEMBER_NAME_CHECKED(FCrowdyServerFunction, Name)));
	Category.AddProperty(Function->GetChildHandle(GET_MEMBER_NAME_CHECKED(FCrowdyServerFunction, WhoCanCall)));
	const TWeakObjectPtr<const UCrowdyServerObjectDefinition> Weak = Current;
	Category.AddCustomRow(LOCTEXT("CallerNeedsMembersSearch", "Callable By Members From"))
		.Visibility(TAttribute<EVisibility>::CreateLambda([Weak, Index]()
		{
			return CallerNeedsMembers(Weak.Get(), Index) ? EVisibility::Visible : EVisibility::Collapsed;
		}))
		.ValueContent()
		.MinDesiredWidth(ValueMinWidth)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[
				WarningIcon()
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				Note(LOCTEXT("CallerNeedsMembers", "Members and Leader need Members From, set in the object's Members."))
			]
		];
	const TSharedPtr<IPropertyHandle> Cooldown = Function->GetChildHandle(GET_MEMBER_NAME_CHECKED(FCrowdyServerFunction, CooldownSeconds));
	if (Cooldown.IsValid())
	{
		CrowdyServerObjectRows::ShowSeconds(Category.AddProperty(Cooldown), Cooldown.ToSharedRef());
	}
	AddPins(DetailBuilder, Function, Index, false);
	AddPins(DetailBuilder, Function, Index, true);
	IDetailCategoryBuilder& Advanced = DetailBuilder.EditCategory(TEXT("FunctionAdvanced"), LOCTEXT("FunctionAdvanced", "Advanced"), ECategoryPriority::Uncommon);
	Advanced.InitiallyCollapsed(true);
	Advanced.AddProperty(Function->GetChildHandle(GET_MEMBER_NAME_CHECKED(FCrowdyServerFunction, ServerName)));
}

void FCrowdyServerObjectDefinitionCustomization::AddPins(IDetailLayoutBuilder& DetailBuilder, const TSharedRef<IPropertyHandle>& Function, int32 Index, bool bOutputs)
{
	using namespace CrowdyServerObjectDefinitionCustomizationDetail;
	const UCrowdyServerObjectDefinition* Current = Definition.Get();
	const TSharedPtr<IPropertyHandle> Form = Function->GetChildHandle(bOutputs ? GET_MEMBER_NAME_CHECKED(FCrowdyServerFunction, ReplyForm) : GET_MEMBER_NAME_CHECKED(FCrowdyServerFunction, ParamsForm));
	const TSharedPtr<IPropertyHandle> Struct = Function->GetChildHandle(bOutputs ? GET_MEMBER_NAME_CHECKED(FCrowdyServerFunction, Reply) : GET_MEMBER_NAME_CHECKED(FCrowdyServerFunction, Params));
	const TSharedPtr<IPropertyHandle> List = Function->GetChildHandle(bOutputs ? GET_MEMBER_NAME_CHECKED(FCrowdyServerFunction, ReplyList) : GET_MEMBER_NAME_CHECKED(FCrowdyServerFunction, ParamsList));
	if (!Current || !Form.IsValid() || !Struct.IsValid() || !List.IsValid())
	{
		return;
	}
	const FCrowdyServerFunction& Data = Current->Functions[Index];
	const bool bUseStruct = (bOutputs ? Data.ReplyForm : Data.ParamsForm) == ECrowdyServerValuesForm::Struct;
	const FInstancedPropertyBag& Values = bOutputs ? Data.ReplyList : Data.ParamsList;
	const TSharedPtr<IPropertyUtilities> PropUtils = DetailBuilder.GetPropertyUtilities();

	IDetailCategoryBuilder& Category = DetailBuilder.EditCategory(bOutputs ? TEXT("Outputs") : TEXT("Inputs"),
		bOutputs ? LOCTEXT("Outputs", "Outputs") : LOCTEXT("Inputs", "Inputs"), ECategoryPriority::Default);
	const TSharedRef<SHorizontalBox> Header = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		[
			SNullWidget::NullWidget
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0.0f, 0.0f, 8.0f, 0.0f)
		[
			SNew(SCheckBox)
			.IsChecked(bUseStruct ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
			.ToolTipText(bOutputs
				? LOCTEXT("OutputsUseStructToolTip", "Reply with a C++ or Blueprint struct of your own instead of the outputs listed here")
				: LOCTEXT("InputsUseStructToolTip", "Take a C++ or Blueprint struct of your own instead of the inputs listed here"))
			.OnCheckStateChanged_Lambda([Form](ECheckBoxState State)
			{
				Form->SetValue(static_cast<uint8>(State == ECheckBoxState::Checked ? ECrowdyServerValuesForm::Struct : ECrowdyServerValuesForm::List));
			})
			[
				SNew(STextBlock)
				.Text(LOCTEXT("UseStruct", "Use Struct"))
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		];
	if (!bUseStruct)
	{
		// Only an add button: the engine's list header also offers to remove every pin in one click.
		Header->AddSlot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ToolTipText(bOutputs ? LOCTEXT("AddOutput", "Add an output") : LOCTEXT("AddInput", "Add an input"))
				.OnClicked_Lambda([List]() { return AddPin(List); })
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush("Icons.PlusCircle"))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
			];
	}
	Category.HeaderContent(Header);

	if (bUseStruct)
	{
		Category.AddProperty(Struct);
		return;
	}
	const UPropertyBag* Bag = Values.GetPropertyBagStruct();
	if (!Bag || Bag->GetPropertyDescs().IsEmpty())
	{
		Category.AddCustomRow(bOutputs ? LOCTEXT("NoOutputs", "No outputs") : LOCTEXT("NoInputs", "No inputs"))
			.WholeRowContent()
			[
				Note(bOutputs ? LOCTEXT("NoOutputsNote", "No outputs: the function replies nothing. Add one with the + button.")
					: LOCTEXT("NoInputsNote", "No inputs: the caller sends nothing. Add one with the + button."))
			];
		return;
	}
	FPropertyBagInstanceDataDetails::FConstructParams Params;
	Params.BagStructProperty = List;
	Params.PropUtils = PropUtils;
	Params.ChildRowFeatures = PinRowFeatures;
	Category.AddCustomBuilder(MakeShared<FPropertyBagInstanceDataDetails>(Params));
	if (!bOutputs)
	{
		AddValueRanges(Category, Index, List);
	}
}

void FCrowdyServerObjectDefinitionCustomization::AddValueRanges(IDetailCategoryBuilder& Category, int32 Index, const TSharedPtr<IPropertyHandle>& List)
{
	using namespace CrowdyServerObjectDefinitionCustomizationDetail;
	UCrowdyServerObjectDefinition* Current = Definition.Get();
	const UPropertyBag* Bag = Current && Current->Functions.IsValidIndex(Index) ? Current->Functions[Index].ParamsList.GetPropertyBagStruct() : nullptr;
	if (!Bag)
	{
		return;
	}
	IDetailGroup* Group = nullptr;
	for (const FPropertyBagPropertyDesc& Desc : Bag->GetPropertyDescs())
	{
		// A range left on an input that is no longer a number stays listed, so it can be cleared.
		const bool bHasRange = Desc.HasMetaData(CrowdyServerValueRange::MinKey) || Desc.HasMetaData(CrowdyServerValueRange::MaxKey);
		if (!bHasRange && !CrowdyServerValueRange::CanHaveRange(Desc))
		{
			continue;
		}
		if (!Group)
		{
			Group = &Category.AddGroup(TEXT("ValueRange"), LOCTEXT("ValueRange", "Value Range"), false, true);
		}
		const FText MinLabel = LOCTEXT("ValueRangeMin", "Min");
		const FText MaxLabel = LOCTEXT("ValueRangeMax", "Max");
		const bool bWhole = Desc.IsNumericIntegralType();
		Group->AddWidgetRow()
			.FilterString(FText::FromName(Desc.Name))
			.NameContent()
			[
				SNew(STextBlock)
				.Text(FText::FromName(Desc.Name))
				.Font(IDetailLayoutBuilder::GetDetailFont())
				.ToolTipText(LOCTEXT("ValueRangeToolTip", "The values a caller may send; the server refuses a call outside them. Clear a bound for no limit."))
			]
			.ValueContent()
			.MinDesiredWidth(2.0f * TextMinWidth)
			.MaxDesiredWidth(2.0f * TextMinWidth)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.Padding(0.0f, 0.0f, 4.0f, 0.0f)
				[
					bWhole ? MakeRangeBox<int64>(Current, Index, Desc.ID, List, CrowdyServerValueRange::MinKey, MinLabel)
						: MakeRangeBox<double>(Current, Index, Desc.ID, List, CrowdyServerValueRange::MinKey, MinLabel)
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					bWhole ? MakeRangeBox<int64>(Current, Index, Desc.ID, List, CrowdyServerValueRange::MaxKey, MaxLabel)
						: MakeRangeBox<double>(Current, Index, Desc.ID, List, CrowdyServerValueRange::MaxKey, MaxLabel)
				]
			];
	}
}

void FCrowdyServerObjectDefinitionCustomization::AddObjectCategories(IDetailLayoutBuilder& DetailBuilder)
{
	using namespace CrowdyServerObjectDefinitionCustomizationDetail;
	auto AddCategory = [&DetailBuilder](FName Name, const FText& Label, std::initializer_list<FName> Properties)
	{
		IDetailCategoryBuilder& Category = DetailBuilder.EditCategory(Name, Label, ECategoryPriority::TypeSpecific);
		for (const FName Property : Properties)
		{
			Category.AddProperty(DetailBuilder.GetProperty(Property));
		}
	};
	// Max Members, Show Members to Players and Remove Members Who Leave hide themselves unless Members From is This Object.
	AddCategory(AccessCategory, LOCTEXT("AccessCategory", "Access"), {GET_MEMBER_NAME_CHECKED(UCrowdyServerObjectDefinition, Visibility),
		GET_MEMBER_NAME_CHECKED(UCrowdyServerObjectDefinition, bOnlyOneInstance)});
	AddCategory(MembersCategory, LOCTEXT("MembersCategory", "Members"), {GET_MEMBER_NAME_CHECKED(UCrowdyServerObjectDefinition, MembersFrom),
		GET_MEMBER_NAME_CHECKED(UCrowdyServerObjectDefinition, MaxMembers), GET_MEMBER_NAME_CHECKED(UCrowdyServerObjectDefinition, bShowMembers),
		GET_MEMBER_NAME_CHECKED(UCrowdyServerObjectDefinition, bRemoveMembersWhoLeave)});
	AddCategory(TimersCategory, LOCTEXT("TimersCategory", "Timers & Events"), {GET_MEMBER_NAME_CHECKED(UCrowdyServerObjectDefinition, Timers),
		GET_MEMBER_NAME_CHECKED(UCrowdyServerObjectDefinition, bOnPlayerJoined), GET_MEMBER_NAME_CHECKED(UCrowdyServerObjectDefinition, bOnPlayerLeft)});
	AddCategory(CanCallCategory, LOCTEXT("CanCallCategory", "Can Call"), {GET_MEMBER_NAME_CHECKED(UCrowdyServerObjectDefinition, CanCall)});
}

void FCrowdyServerObjectDefinitionCustomization::CustomizeVariable(IDetailLayoutBuilder& DetailBuilder, FName Name)
{
	using namespace CrowdyServerObjectDefinitionCustomizationDetail;
	UCrowdyServerObjectDefinition* Current = Definition.Get();
	if (!Current)
	{
		return;
	}
	IDetailCategoryBuilder& Category = DetailBuilder.EditCategory(TEXT("Variable"), LOCTEXT("VariableCategory", "Variable"), ECategoryPriority::Important);
	const TSharedPtr<IPropertyHandle> Variables = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UCrowdyServerObjectDefinition, StateList));
	const bool bList = Current->StateForm == ECrowdyServerValuesForm::List;
	const UPropertyBag* Bag = Current->StateList.GetPropertyBagStruct();
	TSharedPtr<SWidget> NameWidget;
	TSharedPtr<SWidget> TypeWidget;
	if (bList && Bag && Bag->FindPropertyDescByName(Name))
	{
		IDetailCategoryBuilder& Default = DetailBuilder.EditCategory(TEXT("DefaultValue"), LOCTEXT("DefaultValueCategory", "Default Value"), ECategoryPriority::Default);
		IDetailPropertyRow* ValueRow = Default.AddExternalStructureProperty(MakeShared<FInstancePropertyBagStructureDataProvider>(Current->StateList), Name);
		const TSharedPtr<IPropertyHandle> Value = ValueRow ? ValueRow->GetPropertyHandle() : nullptr;
		if (Value.IsValid())
		{
			// The value lives in the definition, which the view of the List's own memory does not know to record for undo.
			const TWeakObjectPtr<UCrowdyServerObjectDefinition> Weak = Current;
			const FSimpleDelegate Modify = FSimpleDelegate::CreateLambda([Weak]() { if (UCrowdyServerObjectDefinition* Edited = Weak.Get()) { Edited->Modify(); } });
			const FSimpleDelegate Dirty = FSimpleDelegate::CreateLambda([Weak]() { if (UCrowdyServerObjectDefinition* Edited = Weak.Get()) { Edited->MarkPackageDirty(); } });
			Value->SetOnPropertyValuePreChange(Modify);
			Value->SetOnChildPropertyValuePreChange(Modify);
			Value->SetOnPropertyValueChanged(Dirty);
			Value->SetOnChildPropertyValueChanged(Dirty);
			TypeWidget = MakeTypePicker(Current, Variables, Name);
		}
		NameWidget = SNew(SEditableTextBox)
			.Text(FText::FromName(Name))
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.OnTextCommitted_Lambda([this, Variables, Name](const FText& NewText, ETextCommit::Type)
			{
				RenameVariable(Variables, Name, NewText);
			});
	}
	else if (const FProperty* Field = Current->GetStateStruct() ? FindFProperty<FProperty>(Current->GetStateStruct(), Name) : nullptr)
	{
		// The struct sets both, so they show as the List's boxes do, read-only.
		const FText FromStruct = LOCTEXT("VariableFromStruct", "Set by the State Struct; change it there.");
		FEdGraphPinType PinType;
		GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Field, PinType);
		NameWidget = SNew(SEditableTextBox).Text(FText::FromName(Name)).Font(IDetailLayoutBuilder::GetDetailFont()).IsReadOnly(true).ToolTipText(FromStruct);
		TypeWidget = SNew(SPinTypeSelector, FGetPinTypeTree::CreateUObject(GetDefault<UEdGraphSchema_K2>(), &UEdGraphSchema_K2::GetVariableTypeTree))
			.Schema(GetDefault<UEdGraphSchema_K2>())
			.TargetPinType(PinType)
			.SelectorType(SPinTypeSelector::ESelectorType::Full)
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.IsEnabled(false)
			.ToolTipText(FromStruct);
	}
	if (!NameWidget.IsValid())
	{
		return;
	}
	const FText NameLabel = LOCTEXT("VariableName", "Variable Name");
	const FText TypeLabel = LOCTEXT("VariableType", "Variable Type");
	Category.AddCustomRow(NameLabel)
		.NameContent()
		[
			SNew(STextBlock)
			.Text(NameLabel)
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.ToolTipText(LOCTEXT("VariableNameToolTip", "The variable's name, in Unreal and in server code"))
		]
		.ValueContent()
		.MinDesiredWidth(TextMinWidth)
		.MaxDesiredWidth(TextMaxWidth)
		[
			NameWidget.ToSharedRef()
		];
	if (TypeWidget.IsValid())
	{
		Category.AddCustomRow(TypeLabel)
			.NameContent()
			[
				SNew(STextBlock)
				.Text(TypeLabel)
				.Font(IDetailLayoutBuilder::GetDetailFont())
				.ToolTipText(LOCTEXT("VariableTypeToolTip", "What the variable holds, chosen from the types a Server Object can carry"))
			]
			.ValueContent()
			[
				TypeWidget.ToSharedRef()
			];
	}
	Category.AddCustomRow(LOCTEXT("VisibleToPlayers", "Visible to Players"))
		.NameContent()
		[
			WatchedHandle->CreatePropertyNameWidget()
		]
		.ValueContent()
		[
			SNew(SCheckBox)
			.ToolTipText(LOCTEXT("VisibleToPlayersToolTip", "Players can read and watch this variable. Off, it stays on the server."))
			.IsChecked(Current->WatchedFields.Contains(Name) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
			.OnCheckStateChanged_Lambda([this, Name](ECheckBoxState State) { SetVisibleToPlayers(Name, State == ECheckBoxState::Checked); })
		];
}

void FCrowdyServerObjectDefinitionCustomization::RenameVariable(TSharedPtr<IPropertyHandle> Variables, FName Name, const FText& NewText)
{
	UCrowdyServerObjectDefinition* Current = Definition.Get();
	const UPropertyBag* Bag = Current ? Current->StateList.GetPropertyBagStruct() : nullptr;
	const FPropertyBagPropertyDesc* Desc = Bag ? Bag->FindPropertyDescByName(Name) : nullptr;
	const FName NewName(*NewText.ToString().TrimStartAndEnd());
	if (!Desc || !Variables.IsValid() || NewName.IsNone() || NewName == Name || Bag->FindPropertyDescByName(NewName))
	{
		return;
	}
	const FPropertyBagPropertyDesc Renamed = *Desc;
	const FScopedTransaction Transaction(LOCTEXT("RenameVariable", "Rename Variable"));
	Current->Modify();
	UE::StructUtils::ApplyChangesToSinglePropertyDesc(LOCTEXT("RenameVariable", "Rename Variable"), Renamed, Variables,
		[NewName](FPropertyBagPropertyDesc& Edited) { Edited.Name = NewName; });
	const int32 Watched = Current->WatchedFields.IndexOfByKey(Name);
	if (Watched != INDEX_NONE)
	{
		Current->WatchedFields[Watched] = NewName;
		Current->PostEditChange();
	}
	if (Selection.IsValid())
	{
		Selection->SelectVariable(NewName);
	}
}

void FCrowdyServerObjectDefinitionCustomization::SetVisibleToPlayers(FName Name, bool bVisible)
{
	UCrowdyServerObjectDefinition* Current = Definition.Get();
	if (!Current)
	{
		return;
	}
	TArray<FName> Watched = Current->WatchedFields;
	if (!CrowdyServerCodeFiles::SetWatchedField(Watched, Name, bVisible))
	{
		return;
	}
	const FScopedTransaction Transaction(LOCTEXT("ChangeVisibleToPlayers", "Change Visible to Players"));
	Current->Modify();
	Current->WatchedFields = MoveTemp(Watched);
	Current->PostEditChange();
}

void FCrowdyServerObjectDefinitionCustomization::AddAdvancedCategory(IDetailLayoutBuilder& DetailBuilder)
{
	using namespace CrowdyServerObjectDefinitionCustomizationDetail;
	DetailBuilder.HideCategory(ServerNamesCategory);
	DetailBuilder.HideCategory(BakedCategory);
	IDetailCategoryBuilder& Advanced = DetailBuilder.EditCategory(AdvancedCategory, FText::GetEmpty(), ECategoryPriority::Uncommon);
	Advanced.InitiallyCollapsed(true);
	IDetailGroup& Names = Advanced.AddGroup(ServerNamesCategory, LOCTEXT("ServerNamesGroup", "Server Names"), false, true);
	Names.AddPropertyRow(DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UCrowdyServerObjectDefinition, FieldNames)));
	Names.AddPropertyRow(DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UCrowdyServerObjectDefinition, EnumValueNames)));
	IDetailGroup& Baked = Advanced.AddGroup(BakedCategory, LOCTEXT("BakedGroup", "Baked"), false, true);
	Baked.AddPropertyRow(DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UCrowdyServerObjectDefinition, BakedStructs)));
	Baked.AddPropertyRow(DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UCrowdyServerObjectDefinition, BakedEnums)));
}

TSharedRef<SWidget> FCrowdyServerObjectDefinitionCustomization::MakeTypeNameValue()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SEditableTextBox)
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.Text(this, &FCrowdyServerObjectDefinitionCustomization::GetTypeNameText)
			.HintText(LOCTEXT("TypeNameHint", "boss_fight"))
			.ToolTipText(TypeNameHandle->GetToolTipText())
			.OnTextChanged(this, &FCrowdyServerObjectDefinitionCustomization::HandleTypeNameTyped)
			.OnTextCommitted(this, &FCrowdyServerObjectDefinitionCustomization::HandleTypeNameCommitted)
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 2.0f, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)
			.Visibility(this, &FCrowdyServerObjectDefinitionCustomization::GetProblemVisibility)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[
				CrowdyServerObjectDefinitionCustomizationDetail::WarningIcon()
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(this, &FCrowdyServerObjectDefinitionCustomization::GetTypeNameProblem)
				.Font(IDetailLayoutBuilder::GetDetailFont())
				.AutoWrapText(true)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SHyperlink)
				.Text(this, &FCrowdyServerObjectDefinitionCustomization::GetSuggestionText)
				.ToolTipText(LOCTEXT("UseSuggestionToolTip", "Sets the Type Name from the asset's name"))
				.Visibility(this, &FCrowdyServerObjectDefinitionCustomization::GetSuggestionVisibility)
				.OnNavigate(this, &FCrowdyServerObjectDefinitionCustomization::HandleUseSuggestion)
			]
		];
}

TSharedRef<SWidget> FCrowdyServerObjectDefinitionCustomization::MakeFieldCheckBox(FName Field, const FText& Label, bool bWatched)
{
	const FText ToolTip = bWatched
		? FText::Format(LOCTEXT("WatchedOn", "Players can read and watch {0}"), Label)
		: FText::Format(LOCTEXT("WatchedOff", "Only the server sees {0}"), Label);
	return SNew(SCheckBox)
		.IsChecked(bWatched ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
		.ToolTipText(ToolTip)
		.OnCheckStateChanged(this, &FCrowdyServerObjectDefinitionCustomization::HandleWatchedChanged, Field)
		[
			SNew(STextBlock)
			.Text(Label)
			.Font(IDetailLayoutBuilder::GetDetailFont())
		];
}

TSharedRef<SWidget> FCrowdyServerObjectDefinitionCustomization::MakeMissingField(FName Field)
{
	return SNew(SHorizontalBox)
		.ToolTipText(FText::Format(LOCTEXT("MissingWatched", "{0} is no longer a field of State, so players cannot see it. Remove it, or add the field back."), FText::FromName(Field)))
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0.0f, 0.0f, 4.0f, 0.0f)
		[
			CrowdyServerObjectDefinitionCustomizationDetail::WarningIcon()
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(FText::FromName(Field))
			.Font(IDetailLayoutBuilder::GetDetailFont())
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(2.0f, 0.0f, 0.0f, 0.0f)
		[
			PropertyCustomizationHelpers::MakeClearButton(FSimpleDelegate::CreateSP(this, &FCrowdyServerObjectDefinitionCustomization::HandleRemoveWatched, Field),
				LOCTEXT("RemoveWatched", "Stops watching it"))
		];
}

void FCrowdyServerObjectDefinitionCustomization::RefreshTypeName()
{
	const UCrowdyServerObjectDefinition* Current = Definition.Get();
	if (!Current)
	{
		return;
	}
	StoredTypeName = Current->TypeName;
	TypeNameText = FText::FromString(StoredTypeName);
	SuggestedTypeName = CrowdyServerCodeFiles::SuggestTypeName(Current->GetName());
	SuggestionText = FText::Format(LOCTEXT("UseSuggestion", "Use {0}"), FText::FromString(SuggestedTypeName));
	const TSharedPtr<FCrowdyServerComputeService> Pinned = Service.Pin();
	const bool bValid = CrowdyServerCodeFiles::CheckTypeName(StoredTypeName) == CrowdyServerCodeFiles::ETypeNameProblem::None;
	bSharedTypeName = bValid && Pinned.IsValid() && Pinned->IsSharedTypeName(StoredTypeName);
	TypeNameProblem = ProblemFor(StoredTypeName);
}

void FCrowdyServerObjectDefinitionCustomization::RefreshServiceTypes()
{
	// The service's list of definitions is what a shared Type Name is found in.
	const TSharedPtr<FCrowdyServerComputeService> Pinned = Service.Pin();
	if (Pinned.IsValid() && !Pinned->IsBusy())
	{
		Pinned->RequestRefreshTypes();
	}
}

void FCrowdyServerObjectDefinitionCustomization::RebuildPlayersSee()
{
	using namespace CrowdyServerObjectDefinitionCustomizationDetail;
	const UCrowdyServerObjectDefinition* Current = Definition.Get();
	if (!Current || !PlayersSeeBox.IsValid())
	{
		return;
	}
	PlayersSeeBox->ClearChildren();
	const UScriptStruct* StateStruct = Current->GetStateStruct();
	if (!StateStruct)
	{
		PlayersSeeBox->AddSlot()[Note(Current->StateForm == ECrowdyServerValuesForm::List
			? LOCTEXT("NoStateValues", "Add variables to pick which players can see.")
			: LOCTEXT("NoState", "Choose a State Struct to pick which variables players can see."))];
		return;
	}
	TArray<FName> Fields;
	for (TFieldIterator<FProperty> It(StateStruct); It; ++It)
	{
		const FName Field(*It->GetAuthoredName());
		Fields.Add(Field);
		PlayersSeeBox->AddSlot()[MakeFieldCheckBox(Field, It->GetDisplayNameText(), Current->WatchedFields.Contains(Field))];
	}
	for (const FName Missing : CrowdyServerCodeFiles::FindMissingFields(Current->WatchedFields, Fields))
	{
		PlayersSeeBox->AddSlot()[MakeMissingField(Missing)];
	}
	if (Fields.IsEmpty())
	{
		PlayersSeeBox->AddSlot()[Note(LOCTEXT("StateEmpty", "State has no fields yet."))];
	}
}

void FCrowdyServerObjectDefinitionCustomization::SetWatched(FName Field, bool bWatched)
{
	UCrowdyServerObjectDefinition* Current = Definition.Get();
	if (!Current || !WatchedHandle.IsValid())
	{
		return;
	}
	TArray<FName> Watched = Current->WatchedFields;
	if (!CrowdyServerCodeFiles::SetWatchedField(Watched, Field, bWatched))
	{
		return;
	}
	const FScopedTransaction Transaction(LOCTEXT("ChangePlayersSee", "Change Visible to Players"));
	WatchedHandle->NotifyPreChange();
	Current->Modify();
	Current->WatchedFields = MoveTemp(Watched);
	WatchedHandle->NotifyPostChange(EPropertyChangeType::ValueSet);
	WatchedHandle->NotifyFinishedChangingProperties();
}

FText FCrowdyServerObjectDefinitionCustomization::ProblemFor(const FString& TypeName) const
{
	switch (CrowdyServerCodeFiles::CheckTypeName(TypeName))
	{
	case CrowdyServerCodeFiles::ETypeNameProblem::Empty: return LOCTEXT("TypeNameEmpty", "Needed: the type's name on the server.");
	case CrowdyServerCodeFiles::ETypeNameProblem::TooLong: return LOCTEXT("TypeNameTooLong", "At most 48 characters.");
	case CrowdyServerCodeFiles::ETypeNameProblem::FirstNotLetter: return LOCTEXT("TypeNameFirst", "Start with a lowercase letter.");
	case CrowdyServerCodeFiles::ETypeNameProblem::BadCharacter: return LOCTEXT("TypeNameCharacters", "Use only lowercase letters, digits and underscores.");
	default: break;
	}
	// Only the stored name is known to the service, so a name still being typed is never reported as shared.
	if (!bSharedTypeName || !TypeName.Equals(StoredTypeName, ESearchCase::CaseSensitive))
	{
		return FText::GetEmpty();
	}
	return LOCTEXT("TypeNameShared", "Another definition already uses this Type Name; give this one its own.");
}

void FCrowdyServerObjectDefinitionCustomization::HandleObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& Event)
{
	if (Object != Definition.Get() || (Event.ChangeType & EPropertyChangeType::Interactive) != 0 || RefreshTicker.IsValid())
	{
		return;
	}
	// On the next tick, so a checkbox is never rebuilt while it handles its own click.
	RefreshTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateSP(this, &FCrowdyServerObjectDefinitionCustomization::HandleDeferredRefresh));
}

bool FCrowdyServerObjectDefinitionCustomization::HandleDeferredRefresh(float DeltaTime)
{
	RefreshTicker.Reset();
	const UCrowdyServerObjectDefinition* Current = Definition.Get();
	if (!Current)
	{
		return false;
	}
	const bool bTypeNameChanged = !Current->TypeName.Equals(StoredTypeName, ESearchCase::CaseSensitive);
	RefreshTypeName();
	RebuildPlayersSee();
	if (bTypeNameChanged)
	{
		RefreshServiceTypes();
	}
	return false;
}

void FCrowdyServerObjectDefinitionCustomization::HandleServiceChanged()
{
	const TSharedPtr<FCrowdyServerComputeService> Pinned = Service.Pin();
	if (!Pinned.IsValid() || Pinned->GetDataGeneration() == SeenGeneration)
	{
		return;
	}
	SeenGeneration = Pinned->GetDataGeneration();
	RefreshTypeName();
}

void FCrowdyServerObjectDefinitionCustomization::HandleTypeNameTyped(const FText& NewText)
{
	TypeNameProblem = ProblemFor(NewText.ToString().TrimStartAndEnd());
}

void FCrowdyServerObjectDefinitionCustomization::HandleTypeNameCommitted(const FText& NewText, ETextCommit::Type CommitType)
{
	const FString Typed = NewText.ToString().TrimStartAndEnd();
	if (!TypeNameHandle.IsValid() || Typed.Equals(StoredTypeName, ESearchCase::CaseSensitive))
	{
		TypeNameProblem = ProblemFor(StoredTypeName);
		return;
	}
	TypeNameHandle->SetValue(Typed);
	RefreshTypeName();
	RefreshServiceTypes();
}

void FCrowdyServerObjectDefinitionCustomization::HandleUseSuggestion()
{
	if (!TypeNameHandle.IsValid() || SuggestedTypeName.IsEmpty())
	{
		return;
	}
	TypeNameHandle->SetValue(SuggestedTypeName);
	RefreshTypeName();
	RefreshServiceTypes();
}

void FCrowdyServerObjectDefinitionCustomization::HandleWatchedChanged(ECheckBoxState NewState, FName Field)
{
	SetWatched(Field, NewState == ECheckBoxState::Checked);
}

void FCrowdyServerObjectDefinitionCustomization::HandleRemoveWatched(FName Field)
{
	SetWatched(Field, false);
}

EVisibility FCrowdyServerObjectDefinitionCustomization::GetProblemVisibility() const
{
	return TypeNameProblem.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
}

EVisibility FCrowdyServerObjectDefinitionCustomization::GetSuggestionVisibility() const
{
	const bool bUseful = !SuggestedTypeName.IsEmpty() && !SuggestedTypeName.Equals(StoredTypeName, ESearchCase::CaseSensitive);
	return !TypeNameProblem.IsEmpty() && bUseful ? EVisibility::Visible : EVisibility::Collapsed;
}

#undef LOCTEXT_NAMESPACE
