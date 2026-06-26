// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/SCrowdyGameModelView.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Model/FCrowdyStudioController.h"
#include "Serialization/JsonSerializer.h"
#include "Style/CrowdyStudioStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyle.h"
#include "Styling/StyleDefaults.h"
#include "UI/CrowdyStudioWidgets.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "CrowdyStudio"

namespace
{
	TSharedRef<SWidget> LabeledField(TSharedPtr<SEditableTextBox>& Member, const FText& Label, const TCHAR* Hint)
	{
		const ISlateStyle& Style = FCrowdyStudioStyle::Get();
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 10.0f, 0.0f)
			[ SNew(SBox).WidthOverride(140.0f)[ SNew(STextBlock).Text(Label).TextStyle(&Style, "Crowdy.Text.Body") ] ]
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[ SAssignNew(Member, SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(FText::FromString(Hint)) ];
	}

	// A label-on-the-left row whose input is a segmented single-choice control, matching
	// LabeledField's layout. Values are the stored choices, Labels their display text.
	TSharedRef<SWidget> LabeledChoice(const FText& Label, const TArray<FString>& Values, const TArray<FText>& Labels,
		TAttribute<FString> Current, TFunction<void(const FString&)> OnSelected)
	{
		const ISlateStyle& Style = FCrowdyStudioStyle::Get();
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 10.0f, 0.0f)
			[ SNew(SBox).WidthOverride(140.0f)[ SNew(STextBlock).Text(Label).TextStyle(&Style, "Crowdy.Text.Body") ] ]
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[ CrowdyStudioWidgets::SegmentedEnum(Values, Labels, Current, OnSelected) ];
	}

	FString ParamsToJson(const TArray<FStudioFunctionParam>& Params)
	{
		if (Params.Num() == 0)
		{
			return FString();
		}

		TArray<TSharedPtr<FJsonValue>> Items;
		for (const FStudioFunctionParam& Param : Params)
		{
			const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("name"), Param.Name);
			Object->SetStringField(TEXT("valueType"), Param.ValueType);
			Object->SetBoolField(TEXT("required"), Param.bRequired);
			if (!Param.DefaultValueJson.IsEmpty())
			{
				Object->SetStringField(TEXT("defaultValueJson"), Param.DefaultValueJson);
			}
			if (!Param.Description.IsEmpty())
			{
				Object->SetStringField(TEXT("description"), Param.Description);
			}
			Object->SetNumberField(TEXT("sortOrder"), Param.SortOrder);
			Items.Add(MakeShared<FJsonValueObject>(Object));
		}

		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Items, Writer);
		return Out;
	}

	FString MutationsToJson(const TArray<FStudioFunctionMutation>& Mutations)
	{
		if (Mutations.Num() == 0)
		{
			return FString();
		}

		TArray<TSharedPtr<FJsonValue>> Items;
		for (const FStudioFunctionMutation& Mutation : Mutations)
		{
			const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("target"), Mutation.Target);
			Object->SetStringField(TEXT("property"), Mutation.Property);
			Object->SetStringField(TEXT("expression"), Mutation.Expression);
			Items.Add(MakeShared<FJsonValueObject>(Object));
		}

		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Items, Writer);
		return Out;
	}

	TSharedRef<SWidget> ListCard(const TSharedRef<SWidget>& List, float Height)
	{
		return SNew(SBox).HeightOverride(Height)
			[ CrowdyStudioWidgets::Card(List, FMargin(4.0f), /*bFlat*/ true) ];
	}

	// The eight invoke-policy leaf kinds, in plain language for the rule-type dropdown.
	FText PolicyTypeLabel(const FString& Type)
	{
		if (Type == TEXT("owner_of_self")) { return LOCTEXT("PolOwner", "Owns the target (self)"); }
		if (Type == TEXT("is_current_turn")) { return LOCTEXT("PolTurn", "It's their turn"); }
		if (Type == TEXT("is_host")) { return LOCTEXT("PolHost", "Is the host"); }
		if (Type == TEXT("is_participant")) { return LOCTEXT("PolParticipant", "Is in the session"); }
		if (Type == TEXT("tier_feature")) { return LOCTEXT("PolTier", "Has tier feature"); }
		if (Type == TEXT("group_permission")) { return LOCTEXT("PolGroup", "Has team permission"); }
		if (Type == TEXT("grid_permission")) { return LOCTEXT("PolGrid", "Has grid permission"); }
		if (Type == TEXT("condition")) { return LOCTEXT("PolCond", "Custom condition"); }
		return LOCTEXT("PolPick", "Choose a requirement...");
	}

	bool IsPolicyLeafType(const FString& Type)
	{
		return Type == TEXT("owner_of_self") || Type == TEXT("is_current_turn") || Type == TEXT("is_host")
			|| Type == TEXT("is_participant") || Type == TEXT("tier_feature") || Type == TEXT("group_permission")
			|| Type == TEXT("grid_permission") || Type == TEXT("condition");
	}

	// Read one leaf node's fields. Ids are BigInt-as-string on the wire, but tolerate a raw number so a
	// hand-written policy with numeric ids still loads into the builder rather than silently losing them.
	TSharedPtr<FStudioPolicyRule> ReadPolicyLeaf(const TSharedPtr<FJsonObject>& Node)
	{
		auto ReadId = [&Node](const TCHAR* Field, FString& Out)
		{
			if (!Node->TryGetStringField(Field, Out))
			{
				double Num = 0.0;
				if (Node->TryGetNumberField(Field, Num))
				{
					Out = FString::Printf(TEXT("%lld"), static_cast<int64>(Num));
				}
			}
		};

		TSharedPtr<FStudioPolicyRule> Rule = MakeShared<FStudioPolicyRule>();
		Node->TryGetStringField(TEXT("type"), Rule->Type);
		Node->TryGetStringField(TEXT("feature"), Rule->Feature);
		ReadId(TEXT("groupId"), Rule->GroupId);
		Node->TryGetStringField(TEXT("permission"), Rule->Permission);
		Node->TryGetStringField(TEXT("key"), Rule->Key);
		ReadId(TEXT("gridId"), Rule->GridId);
		Node->TryGetStringField(TEXT("expression"), Rule->Expression);
		return Rule;
	}

	// Parse a stored invoke policy into a flat rule list + top-level connector. Returns true when the
	// builder can represent it: an empty policy, a single bare leaf, or one and/or of leaves. Returns
	// false for a nested group, a "not", or any unknown type, which stays as raw JSON.
	bool ParsePolicyJson(const FString& Json, TArray<TSharedPtr<FStudioPolicyRule>>& OutRules, FString& OutConnector)
	{
		OutRules.Reset();
		OutConnector = TEXT("and");

		const FString Trimmed = Json.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			return true;
		}

		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			return false;
		}

		FString Type;
		if (!Root->TryGetStringField(TEXT("type"), Type))
		{
			return false;
		}

		if (Type == TEXT("and") || Type == TEXT("or"))
		{
			OutConnector = Type;
			const TArray<TSharedPtr<FJsonValue>>* Rules = nullptr;
			if (!Root->TryGetArrayField(TEXT("rules"), Rules))
			{
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Entry : *Rules)
			{
				const TSharedPtr<FJsonObject>* Node = nullptr;
				if (!Entry->TryGetObject(Node) || !Node->IsValid())
				{
					return false;
				}
				FString LeafType;
				(*Node)->TryGetStringField(TEXT("type"), LeafType);
				if (!IsPolicyLeafType(LeafType))
				{
					return false;
				}
				OutRules.Add(ReadPolicyLeaf(*Node));
			}
			return true;
		}

		if (IsPolicyLeafType(Type))
		{
			OutRules.Add(ReadPolicyLeaf(Root));
			return true;
		}

		return false;
	}

	// Serialize the flat builder state back to the stored policy JSON. No rules -> empty string (no
	// policy). A single rule is still wrapped in the connector for one stable shape. Only the fields a
	// rule kind uses are emitted; optional ids/permissions are omitted when blank.
	FString PolicyToJson(const FString& Connector, const TArray<TSharedPtr<FStudioPolicyRule>>& Rules)
	{
		TArray<TSharedPtr<FStudioPolicyRule>> Valid;
		for (const TSharedPtr<FStudioPolicyRule>& Rule : Rules)
		{
			if (Rule.IsValid() && !Rule->Type.IsEmpty())
			{
				Valid.Add(Rule);
			}
		}
		if (Valid.Num() == 0)
		{
			return FString();
		}

		auto LeafObject = [](const TSharedPtr<FStudioPolicyRule>& Rule) -> TSharedPtr<FJsonObject>
		{
			TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("type"), Rule->Type);
			if (Rule->Type == TEXT("tier_feature"))
			{
				Object->SetStringField(TEXT("feature"), Rule->Feature);
			}
			else if (Rule->Type == TEXT("group_permission"))
			{
				Object->SetStringField(TEXT("groupId"), Rule->GroupId);
				if (!Rule->Permission.IsEmpty())
				{
					Object->SetStringField(TEXT("permission"), Rule->Permission);
				}
			}
			else if (Rule->Type == TEXT("grid_permission"))
			{
				Object->SetStringField(TEXT("key"), Rule->Key);
				if (!Rule->GridId.IsEmpty())
				{
					Object->SetStringField(TEXT("gridId"), Rule->GridId);
				}
			}
			else if (Rule->Type == TEXT("condition"))
			{
				Object->SetStringField(TEXT("expression"), Rule->Expression);
			}
			return Object;
		};

		const TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("type"), Connector.IsEmpty() ? FString(TEXT("and")) : Connector);
		TArray<TSharedPtr<FJsonValue>> RuleValues;
		for (const TSharedPtr<FStudioPolicyRule>& Rule : Valid)
		{
			RuleValues.Add(MakeShared<FJsonValueObject>(LeafObject(Rule)));
		}
		Root->SetArrayField(TEXT("rules"), RuleValues);

		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
		return Out;
	}

	// Join the server's static-analysis warnings into one bulleted block for the function editor.
	FString JoinWarnings(const TArray<FString>& Warnings)
	{
		FString Out;
		for (const FString& Warning : Warnings)
		{
			if (!Out.IsEmpty())
			{
				Out += LINE_TERMINATOR;
			}
			Out += TEXT("- ") + Warning;
		}
		return Out;
	}
}

void SCrowdyGameModelView::Construct(const FArguments& InArgs)
{
	Controller = InArgs._Controller;

	if (Controller.IsValid())
	{
		Controller->OnContainerTypesChanged.AddSP(this, &SCrowdyGameModelView::HandleContainerTypesChanged);
		Controller->OnPropertyDefsChanged.AddSP(this, &SCrowdyGameModelView::HandlePropertyDefsChanged);
		Controller->OnFunctionsChanged.AddSP(this, &SCrowdyGameModelView::HandleFunctionsChanged);
		Controller->OnFeaturesChanged.AddSP(this, &SCrowdyGameModelView::HandleFeaturesChanged);
		Controller->OnTierFeaturesChanged.AddSP(this, &SCrowdyGameModelView::HandleTierFeaturesChanged);
		Controller->OnAccessTiersChanged.AddSP(this, &SCrowdyGameModelView::HandleAccessTiersChanged);
		Controller->OnRuntimePermissionsChanged.AddSP(this, &SCrowdyGameModelView::HandleRuntimePermissionsChanged);
		Controller->OnContainersChanged.AddSP(this, &SCrowdyGameModelView::HandleContainersChanged);
		Controller->OnContainerStateChanged.AddSP(this, &SCrowdyGameModelView::HandleContainerStateChanged);
	}

	// Options for the per-row value-type combo in the parameters editor.
	for (const TCHAR* ValueType : { TEXT("int"), TEXT("float"), TEXT("string"), TEXT("bool"), TEXT("array"), TEXT("object"), TEXT("container_ref") })
	{
		ValueTypeOptions.Add(MakeShared<FString>(ValueType));
	}

	// Options for the per-row rule-type combo in the invoke-policy builder (slice 4).
	for (const TCHAR* RuleType : { TEXT("owner_of_self"), TEXT("is_current_turn"), TEXT("is_host"), TEXT("is_participant"), TEXT("tier_feature"), TEXT("group_permission"), TEXT("grid_permission"), TEXT("condition") })
	{
		PolicyTypeOptions.Add(MakeShared<FString>(RuleType));
	}

	RebuildTargetOptions();

	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	auto Btn = [&Style](const FText& Label, bool bPrimary, FOnClicked OnClick) -> TSharedRef<SWidget>
	{
		return SNew(SButton)
			.ButtonStyle(&Style, bPrimary ? "Crowdy.Button.Primary" : "Crowdy.Button.Secondary")
			.ContentPadding(FMargin(13.0f, 7.0f))
			.OnClicked(OnClick)
			[ SNew(STextBlock).Text(Label).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(FSlateColor::UseForeground()) ];
	};

	TSharedRef<SWidget> RefreshButton = SNew(SButton)
		.ButtonStyle(&Style, "Crowdy.Button.Secondary").ContentPadding(FMargin(9.0f, 5.0f))
		.OnClicked(this, &SCrowdyGameModelView::OnRefreshClicked)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[ CrowdyStudioWidgets::Icon(TEXT("refresh"), 14.0f, FSlateColor(FCrowdyStudioStyle::TextSecondary())) ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[ SNew(STextBlock).Text(LOCTEXT("GameModelRefresh", "Refresh")).Font(FCoreStyle::GetDefaultFontStyle("Regular", 9)).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSecondary())) ]
		];

	ChildSlot
	[
		SNew(SScrollBox)
		+ SScrollBox::Slot().Padding(2.0f)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[ SNew(STextBlock).Text(LOCTEXT("GameModelHeader", "Game Model")).TextStyle(&Style, "Crowdy.Text.Title") ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ RefreshButton ]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle").Text(LOCTEXT("GameModelPlane", "The design-time schema the runtime consumes. Game plane — sign in with email and password.")) ]

			// Container types.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[
				CrowdyStudioWidgets::Card(
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("ContainerTypes", "Container types"), TEXT("cube")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[
						ListCard(
							SAssignNew(TypeListView, SListView<TSharedPtr<FStudioContainerType>>)
							.ListItemsSource(Controller.IsValid() ? &Controller->GetContainerTypes() : nullptr)
							.OnGenerateRow(this, &SCrowdyGameModelView::MakeTypeRow)
							.OnSelectionChanged(this, &SCrowdyGameModelView::OnTypeSelectionChanged)
							.SelectionMode(ESelectionMode::Single), 120.0f)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[ LabeledField(TypeNameBox, LOCTEXT("TypeName", "Type name"), TEXT("stable key, e.g. weapon")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[ LabeledField(DisplayNameBox, LOCTEXT("DisplayName", "Display name"), TEXT("human-friendly name")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[ LabeledField(TypeDescBox, LOCTEXT("TypeDesc", "Description"), TEXT("optional")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[ LabeledChoice(LOCTEXT("InstantiableBy", "Instantiable by"), { TEXT("admin"), TEXT("member"), TEXT("owner") }, { LOCTEXT("InstAdmin", "Admin"), LOCTEXT("InstMember", "Member"), LOCTEXT("InstOwner", "Owner") }, TAttribute<FString>::CreateLambda([this]() { return TypeInstantiableBy; }), [this](const FString& V) { TypeInstantiableBy = V; }) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[ LabeledChoice(LOCTEXT("DefaultVis", "Default visibility"), { TEXT("public"), TEXT("owner"), TEXT("hidden") }, { LOCTEXT("DvPublic", "Public"), LOCTEXT("DvOwner", "Owner"), LOCTEXT("DvHidden", "Hidden") }, TAttribute<FString>::CreateLambda([this]() { return TypeDefaultVis; }), [this](const FString& V) { TypeDefaultVis = V; }) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f).HAlign(HAlign_Right)
					[ Btn(LOCTEXT("SaveType", "Save Container Type"), false, FOnClicked::CreateSP(this, &SCrowdyGameModelView::OnSaveTypeClicked)) ],
					FMargin(16.0f, 14.0f))
			]

			// Properties.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[
				CrowdyStudioWidgets::Card(
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
					[ SNew(STextBlock).Text(this, &SCrowdyGameModelView::GetSelectedTypeLabel).TextStyle(&Style, "Crowdy.Text.Heading") ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[
						ListCard(
							SAssignNew(PropertyListView, SListView<TSharedPtr<FStudioPropertyDef>>)
							.ListItemsSource(Controller.IsValid() ? &Controller->GetPropertyDefs() : nullptr)
							.OnGenerateRow(this, &SCrowdyGameModelView::MakePropertyRow)
							.SelectionMode(ESelectionMode::None), 110.0f)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[ LabeledField(PropKeyBox, LOCTEXT("PropKey", "Property key"), TEXT("unique within the type")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[ LabeledChoice(LOCTEXT("PropValueType", "Value type"), { TEXT("int"), TEXT("float"), TEXT("string"), TEXT("bool"), TEXT("array"), TEXT("object"), TEXT("container_ref") }, { LOCTEXT("VtInt", "Int"), LOCTEXT("VtFloat", "Float"), LOCTEXT("VtString", "String"), LOCTEXT("VtBool", "Bool"), LOCTEXT("VtArray", "Array"), LOCTEXT("VtObject", "Object"), LOCTEXT("VtRef", "Ref") }, TAttribute<FString>::CreateLambda([this]() { return PropValueType; }), [this](const FString& V) { PropValueType = V; }) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[ LabeledField(PropDefaultBox, LOCTEXT("PropDefault", "Default (JSON)"), TEXT("optional, JSON-encoded")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[ LabeledChoice(LOCTEXT("PropVis", "Visibility"), { TEXT("public"), TEXT("owner"), TEXT("hidden") }, { LOCTEXT("PvPublic", "Public"), LOCTEXT("PvOwner", "Owner"), LOCTEXT("PvHidden", "Hidden") }, TAttribute<FString>::CreateLambda([this]() { return PropVis; }), [this](const FString& V) { PropVis = V; }) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[ LabeledChoice(LOCTEXT("PropWritable", "Writable"), { TEXT("function"), TEXT("owner"), TEXT("admin") }, { LOCTEXT("WrFunction", "Function"), LOCTEXT("WrOwner", "Owner"), LOCTEXT("WrAdmin", "Admin") }, TAttribute<FString>::CreateLambda([this]() { return PropWritable; }), [this](const FString& V) { PropWritable = V; }) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[ LabeledField(PropDescBox, LOCTEXT("PropDesc", "Description"), TEXT("optional")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f).HAlign(HAlign_Right)
					[ Btn(LOCTEXT("SaveProperty", "Save Property"), false, FOnClicked::CreateSP(this, &SCrowdyGameModelView::OnSavePropertyClicked)) ],
					FMargin(16.0f, 14.0f))
			]

			// Functions.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[
				CrowdyStudioWidgets::Card(
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("Functions", "Functions"), TEXT("wand")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[
						ListCard(
							SAssignNew(FunctionListView, SListView<TSharedPtr<FStudioFunction>>)
							.ListItemsSource(Controller.IsValid() ? &Controller->GetFunctions() : nullptr)
							.OnGenerateRow(this, &SCrowdyGameModelView::MakeFunctionRow)
							.OnSelectionChanged(this, &SCrowdyGameModelView::OnFunctionSelectionChanged)
							.SelectionMode(ESelectionMode::Single), 110.0f)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[ LabeledField(FnNameBox, LOCTEXT("FnName", "Function name"), TEXT("unique per app")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[ LabeledField(FnTypeBox, LOCTEXT("FnType", "Bound type"), TEXT("container type, or empty for global")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[ LabeledField(FnDescBox, LOCTEXT("FnDesc", "Description"), TEXT("optional")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[ LabeledField(FnReturnTypeBox, LOCTEXT("FnReturnType", "Return type"), TEXT("optional")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[ LabeledChoice(LOCTEXT("FnInvokeScope", "Invoke scope"), { TEXT("player"), TEXT("server"), TEXT("internal") }, { LOCTEXT("ScPlayer", "Player"), LOCTEXT("ScServer", "Server"), LOCTEXT("ScInternal", "Internal") }, TAttribute<FString>::CreateLambda([this]() { return FnInvokeScope; }), [this](const FString& V) { FnInvokeScope = V; }) ]
					// Return expression with an inline expression-help cheat sheet (slice 5).
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 10.0f, 0.0f)
						[ SNew(SBox).WidthOverride(140.0f)[ SNew(STextBlock).Text(LOCTEXT("FnReturnExpr", "Return expression")).TextStyle(&Style, "Crowdy.Text.Body") ] ]
						+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 6.0f, 0.0f)
						[ SAssignNew(FnReturnExprBox, SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("FnReturnExprHint", "optional, e.g. self.hp")) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[
							SNew(SComboButton)
							.ContentPadding(FMargin(8.0f, 3.0f))
							.ToolTipText(LOCTEXT("ExprHelpTip", "Operators, builtins, and properties you can use in expressions"))
							.OnGetMenuContent(FOnGetContent::CreateSP(this, &SCrowdyGameModelView::MakeExpressionCheatSheet))
							.ButtonContent()
							[ SNew(STextBlock).Text(LOCTEXT("ExprHelp", "Expression help")).TextStyle(&Style, "Crowdy.Text.Subtle") ]
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
						[ SNew(STextBlock).Text(LOCTEXT("FnParams", "Parameters")).TextStyle(&Style, "Crowdy.Text.Body") ]
						+ SVerticalBox::Slot().AutoHeight()[ SAssignNew(ParamsRows, SVerticalBox) ]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f).HAlign(HAlign_Left)
						[ Btn(LOCTEXT("AddParam", "+ Add parameter"), false, FOnClicked::CreateSP(this, &SCrowdyGameModelView::OnAddParamClicked)) ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
						[ SNew(STextBlock).Text(LOCTEXT("FnMutations", "Mutations")).TextStyle(&Style, "Crowdy.Text.Body") ]
						+ SVerticalBox::Slot().AutoHeight()[ SAssignNew(MutationRows, SVerticalBox) ]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f).HAlign(HAlign_Left)
						[ Btn(LOCTEXT("AddMutation", "+ Add mutation"), false, FOnClicked::CreateSP(this, &SCrowdyGameModelView::OnAddMutationClicked)) ]
					]
					// Invoke policy (slice 4): a guided requirement list, with a raw-JSON escape hatch.
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
							[ SNew(STextBlock).Text(LOCTEXT("FnPolicy", "Invoke policy")).TextStyle(&Style, "Crowdy.Text.Body") ]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
							[
								SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Secondary").ContentPadding(FMargin(10.0f, 4.0f))
								.ToolTipText(LOCTEXT("PolToggleTip", "Switch between the guided builder and the raw rule tree"))
								.OnClicked(FOnClicked::CreateSP(this, &SCrowdyGameModelView::OnTogglePolicyJsonClicked))
								[ SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Bold", 9)).ColorAndOpacity(FSlateColor::UseForeground())
									.Text_Lambda([this]() { return bPolicyRawMode ? LOCTEXT("PolUseBuilder", "Use builder") : LOCTEXT("PolEditJson", "Edit as JSON"); }) ]
							]
						]

						// Guided builder (hidden in raw mode).
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(SVerticalBox)
							.Visibility_Lambda([this]() { return bPolicyRawMode ? EVisibility::Collapsed : EVisibility::Visible; })
							+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
							[
								SNew(SHorizontalBox)
								+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
								[ SNew(STextBlock).Text(LOCTEXT("PolMustSatisfy", "Caller must satisfy")).TextStyle(&Style, "Crowdy.Text.Subtle") ]
								+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
								[ SNew(SBox).WidthOverride(150.0f)[ CrowdyStudioWidgets::SegmentedEnum({ TEXT("and"), TEXT("or") }, { LOCTEXT("PolAll", "All"), LOCTEXT("PolAny", "Any") }, TAttribute<FString>::CreateLambda([this]() { return PolicyConnector; }), [this](const FString& V) { PolicyConnector = V; }) ] ]
								+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
								[ SNew(STextBlock).Text(LOCTEXT("PolOfThese", "of these requirements")).TextStyle(&Style, "Crowdy.Text.Subtle") ]
							]
							+ SVerticalBox::Slot().AutoHeight()[ SAssignNew(PolicyRows, SVerticalBox) ]
							+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f).HAlign(HAlign_Left)
							[ Btn(LOCTEXT("AddPolicyRule", "+ Add requirement"), false, FOnClicked::CreateSP(this, &SCrowdyGameModelView::OnAddPolicyRuleClicked)) ]
							+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)
							[ SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Subtle").Text(LOCTEXT("PolNote", "Admins always bypass. No requirements = anyone with app access can call.")) ]
						]

						// Raw JSON escape hatch (shown in raw mode).
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(SVerticalBox)
							.Visibility_Lambda([this]() { return bPolicyRawMode ? EVisibility::Visible : EVisibility::Collapsed; })
							+ SVerticalBox::Slot().AutoHeight()
							[
								SNew(SBox).HeightOverride(64.0f)
								[ SNew(SBorder).BorderImage(Style.GetBrush("Crowdy.Inset")).Padding(FMargin(2.0f))[ SAssignNew(FnPolicyBox, SMultiLineEditableTextBox).HintText(LOCTEXT("FnPolicyRawHint", "authority rule tree as JSON")) ] ]
							]
							+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
							[ SNew(STextBlock).AutoWrapText(true)
								.Visibility_Lambda([this]() { return bPolicyNotRepresentable ? EVisibility::Visible : EVisibility::Collapsed; })
								.ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::Danger()))
								.Text(LOCTEXT("PolTooAdvanced", "This policy is too advanced for the builder (nested or unknown rules). Keep editing as JSON.")) ]
						]
					]
					// Static-analysis warnings the server returned for this function (slice 5). Shown after a
					// save or when a function with warnings is selected; hidden when there are none.
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
					[
						SNew(SVerticalBox)
						.Visibility_Lambda([this]() { return FnWarningsText.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
						+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 3.0f)
						[ SNew(STextBlock).Text(LOCTEXT("FnWarnings", "Static-analysis warnings")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 9)).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::Warning())) ]
						+ SVerticalBox::Slot().AutoHeight()
						[ SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::Warning())).Text_Lambda([this]() { return FText::FromString(FnWarningsText); }) ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f).HAlign(HAlign_Right)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)[ Btn(LOCTEXT("SaveFunction", "Save Function"), false, FOnClicked::CreateSP(this, &SCrowdyGameModelView::OnSaveFunctionClicked)) ]
						+ SHorizontalBox::Slot().AutoWidth()[ Btn(LOCTEXT("DeleteFunction", "Delete"), false, FOnClicked::CreateSP(this, &SCrowdyGameModelView::OnDeleteFunctionClicked)) ]
					],
					FMargin(16.0f, 14.0f))
			]

			// Features.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[
				CrowdyStudioWidgets::Card(
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("Features", "Features"), TEXT("check")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[
						ListCard(
							SAssignNew(FeatureListView, SListView<TSharedPtr<FStudioAppFeature>>)
							.ListItemsSource(Controller.IsValid() ? &Controller->GetFeatures() : nullptr)
							.OnGenerateRow(this, &SCrowdyGameModelView::MakeFeatureRow)
							.SelectionMode(ESelectionMode::None), 90.0f)
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)[ SAssignNew(FeatureKeyBox, SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("FeatureKeyHint", "featureKey")).MinDesiredWidth(160.0f) ]
						+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 6.0f, 0.0f)[ SAssignNew(FeatureDescBox, SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("FeatureDescHint", "description (optional)")) ]
						+ SHorizontalBox::Slot().AutoWidth()[ Btn(LOCTEXT("DefineFeature", "Define"), false, FOnClicked::CreateSP(this, &SCrowdyGameModelView::OnDefineFeatureClicked)) ]
					],
					FMargin(16.0f, 14.0f))
			]

			// Tier-feature grants.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[
				CrowdyStudioWidgets::Card(
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("TierFeatures", "Tier-feature grants"), TEXT("apps")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[
						ListCard(
							SAssignNew(TierFeatureListView, SListView<TSharedPtr<FStudioTierFeature>>)
							.ListItemsSource(Controller.IsValid() ? &Controller->GetTierFeatures() : nullptr)
							.OnGenerateRow(this, &SCrowdyGameModelView::MakeTierFeatureRow)
							.SelectionMode(ESelectionMode::None), 90.0f)
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
						[
							SNew(SBox).WidthOverride(170.0f)
							[
								SAssignNew(TierComboBox, SComboBox<TSharedPtr<FStudioAccessTier>>)
								.OptionsSource(Controller.IsValid() ? &Controller->GetAccessTiers() : nullptr)
								.OnGenerateWidget_Lambda([](TSharedPtr<FStudioAccessTier> Tier) { return SNew(STextBlock).Text(FText::FromString(Tier.IsValid() ? FString::Printf(TEXT("%s  (#%lld)"), *Tier->Name, Tier->TierId) : FString())); })
								.OnSelectionChanged_Lambda([this](TSharedPtr<FStudioAccessTier> Tier, ESelectInfo::Type) { SelectedTier = Tier; })
								[ SNew(STextBlock).Text_Lambda([this]() { return SelectedTier.IsValid() ? FText::FromString(SelectedTier->Name) : LOCTEXT("PickTier", "Select tier"); }) ]
							]
						]
						+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 6.0f, 0.0f)[ SAssignNew(TierFeatureKeyBox, SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("TierFeatureKeyHint", "featureKey")) ]
						+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 4.0f, 0.0f)[ Btn(LOCTEXT("GrantTierFeature", "Grant"), false, FOnClicked::CreateSP(this, &SCrowdyGameModelView::OnGrantTierFeatureClicked)) ]
						+ SHorizontalBox::Slot().AutoWidth()[ Btn(LOCTEXT("RevokeTierFeature", "Revoke"), false, FOnClicked::CreateSP(this, &SCrowdyGameModelView::OnRevokeTierFeatureClicked)) ]
					],
					FMargin(16.0f, 14.0f))
			]

			// Session policy.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[
				CrowdyStudioWidgets::Card(
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("GameModelPolicy", "Session policy"), TEXT("config")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle").Text(this, &SCrowdyGameModelView::GetPolicyLabel) ]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 6.0f, 0.0f)[ CrowdyStudioWidgets::SegmentedEnum({ TEXT("admin"), TEXT("member"), TEXT("anyone") }, { LOCTEXT("ScpAdmin", "Admin"), LOCTEXT("ScpMember", "Member"), LOCTEXT("ScpAnyone", "Anyone") }, TAttribute<FString>::CreateLambda([this]() { return SessionCreationPolicy; }), [this](const FString& V) { SessionCreationPolicy = V; }) ]
						+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 6.0f, 0.0f)[ SAssignNew(ParticipantRoleBox, SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("ParticipantRoleHint", "defaultParticipantRole")) ]
						+ SHorizontalBox::Slot().AutoWidth()[ Btn(LOCTEXT("SetGmPolicy", "Set Policy"), false, FOnClicked::CreateSP(this, &SCrowdyGameModelView::OnSetPolicyClicked)) ]
					],
					FMargin(16.0f, 14.0f))
			]

			// B3-2: live runtime container browser (read-only). Reads the server's instantiated
			// containers, so it shows live state even outside Play.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[
				CrowdyStudioWidgets::Card(
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
					[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("LiveContainers", "Live containers"), TEXT("inspector")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[ SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Subtle").Text(LOCTEXT("LiveContainersHint", "Read-only view of the containers the runtime has instantiated for this app. It reads the server, so it shows live state even outside Play. Filters are optional.")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
						[ SAssignNew(ContainerTypeFilterBox, SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("ContainerTypeFilter", "type name (optional)")).MinDesiredWidth(150.0f) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
						[ SAssignNew(ContainerSessionFilterBox, SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("ContainerSessionFilter", "session id (optional)")).MinDesiredWidth(150.0f) ]
						+ SHorizontalBox::Slot().FillWidth(1.0f).HAlign(HAlign_Right).VAlign(VAlign_Center)
						[ Btn(LOCTEXT("RefreshContainers", "Refresh containers"), false, FOnClicked::CreateSP(this, &SCrowdyGameModelView::OnRefreshContainersClicked)) ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
					[
						SNew(SBox).HeightOverride(150.0f)
						[
							CrowdyStudioWidgets::Card(
								SNew(SOverlay)
								+ SOverlay::Slot()
								[
									SAssignNew(ContainerListView, SListView<TSharedPtr<FStudioContainer>>)
									.ListItemsSource(Controller.IsValid() ? &Controller->GetContainers() : nullptr)
									.OnGenerateRow(this, &SCrowdyGameModelView::MakeContainerRow)
									.OnSelectionChanged(this, &SCrowdyGameModelView::OnContainerSelectionChanged)
									.SelectionMode(ESelectionMode::Single)
								]
								+ SOverlay::Slot()
								[
									SNew(SBox).Visibility_Lambda([this]() { return (Controller.IsValid() && Controller->GetContainers().Num() == 0) ? EVisibility::Visible : EVisibility::Collapsed; })
									[ CrowdyStudioWidgets::EmptyState(TEXT("inspector"), LOCTEXT("NoContainers", "No live containers.\nPress Refresh containers.")) ]
								],
								FMargin(4.0f), /*bFlat*/ true)
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
					[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle").Text(LOCTEXT("ContainerStateLabel", "Selected container properties (JSON):")) ]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SBox).HeightOverride(140.0f)
						[ SNew(SBorder).BorderImage(Style.GetBrush("Crowdy.Inset")).Padding(FMargin(2.0f))[ SAssignNew(ContainerStateBox, SMultiLineEditableTextBox).IsReadOnly(true).HintText(LOCTEXT("ContainerStateHint", "Select a container to view its visible properties.")) ] ]
					],
					FMargin(16.0f, 14.0f))
			]

			// Bulk seed.
			+ SVerticalBox::Slot().AutoHeight()
			[
				CrowdyStudioWidgets::Card(
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("Seed", "Bulk seed"), TEXT("server")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[
						SNew(SBox).HeightOverride(120.0f)
						[ SNew(SBorder).BorderImage(Style.GetBrush("Crowdy.Inset")).Padding(FMargin(2.0f))[ SAssignNew(SeedBox, SMultiLineEditableTextBox).HintText(LOCTEXT("SeedHint", "SeedGameModelInput JSON (appId is added for you): { \"containerTypes\": [...], \"propertyDefinitions\": [...], \"functions\": [...] }")) ] ]
					]
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
					[ Btn(LOCTEXT("SeedButton", "Seed Definitions"), false, FOnClicked::CreateSP(this, &SCrowdyGameModelView::OnSeedClicked)) ],
					FMargin(16.0f, 14.0f))
			]
		]
	];
}

SCrowdyGameModelView::~SCrowdyGameModelView()
{
	if (Controller.IsValid())
	{
		Controller->OnContainerTypesChanged.RemoveAll(this);
		Controller->OnPropertyDefsChanged.RemoveAll(this);
		Controller->OnFunctionsChanged.RemoveAll(this);
		Controller->OnFeaturesChanged.RemoveAll(this);
		Controller->OnTierFeaturesChanged.RemoveAll(this);
		Controller->OnAccessTiersChanged.RemoveAll(this);
		Controller->OnRuntimePermissionsChanged.RemoveAll(this);
		Controller->OnContainersChanged.RemoveAll(this);
		Controller->OnContainerStateChanged.RemoveAll(this);
	}
}

FReply SCrowdyGameModelView::OnRefreshClicked()
{
	if (Controller.IsValid())
	{
		Controller->FetchContainerTypes();
		Controller->FetchFunctions(FString());
		Controller->FetchFeatures();
		Controller->FetchTierFeatures();
		Controller->FetchAppAccessTiers();
		Controller->FetchRuntimePermissions();
		Controller->FetchGameModelPolicy();
	}
	return FReply::Handled();
}

FReply SCrowdyGameModelView::OnSaveTypeClicked()
{
	if (Controller.IsValid() && TypeNameBox.IsValid())
	{
		Controller->UpsertContainerType(
			TypeNameBox->GetText().ToString(),
			DisplayNameBox->GetText().ToString(),
			TypeDescBox->GetText().ToString(),
			TypeInstantiableBy,
			TypeDefaultVis);
	}
	return FReply::Handled();
}

FReply SCrowdyGameModelView::OnSavePropertyClicked()
{
	if (Controller.IsValid() && PropKeyBox.IsValid())
	{
		Controller->UpsertPropertyDef(
			SelectedTypeName(),
			PropKeyBox->GetText().ToString(),
			PropValueType,
			PropDefaultBox->GetText().ToString(),
			PropVis,
			PropWritable,
			PropDescBox->GetText().ToString());
	}
	return FReply::Handled();
}

FReply SCrowdyGameModelView::OnSaveFunctionClicked()
{
	if (Controller.IsValid() && FnNameBox.IsValid())
	{
		// Serialize the parameter rows back to the JSON the server expects.
		TArray<FStudioFunctionParam> Params;
		for (int32 Index = 0; Index < EditParams.Num(); ++Index)
		{
			FStudioFunctionParam P = *EditParams[Index];
			P.SortOrder = Index;
			Params.Add(P);
		}

		TArray<FStudioFunctionMutation> Mutations;
		for (const TSharedPtr<FStudioFunctionMutation>& M : EditMutations)
		{
			if (M.IsValid())
			{
				Mutations.Add(*M);
			}
		}

		// In raw mode the user owns the JSON verbatim; otherwise serialize the guided builder.
		const FString PolicyJson = (bPolicyRawMode && FnPolicyBox.IsValid())
			? FnPolicyBox->GetText().ToString()
			: PolicyToJson(PolicyConnector, EditPolicy);

		Controller->UpsertFunction(
			FnNameBox->GetText().ToString(),
			FnTypeBox->GetText().ToString(),
			FnDescBox->GetText().ToString(),
			FnReturnTypeBox->GetText().ToString(),
			FnInvokeScope,
			FnReturnExprBox->GetText().ToString(),
			ParamsToJson(Params),
			MutationsToJson(Mutations),
			PolicyJson);
	}
	return FReply::Handled();
}

FReply SCrowdyGameModelView::OnDeleteFunctionClicked()
{
	if (Controller.IsValid() && FnNameBox.IsValid())
	{
		Controller->DeleteFunction(FnNameBox->GetText().ToString());
	}
	return FReply::Handled();
}

FReply SCrowdyGameModelView::OnDefineFeatureClicked()
{
	if (Controller.IsValid() && FeatureKeyBox.IsValid())
	{
		Controller->DefineFeature(FeatureKeyBox->GetText().ToString(), FeatureDescBox->GetText().ToString());
	}
	return FReply::Handled();
}

FReply SCrowdyGameModelView::OnGrantTierFeatureClicked()
{
	if (Controller.IsValid() && SelectedTier.IsValid() && TierFeatureKeyBox.IsValid())
	{
		Controller->GrantTierFeature(SelectedTier->TierId, TierFeatureKeyBox->GetText().ToString());
	}
	return FReply::Handled();
}

FReply SCrowdyGameModelView::OnRevokeTierFeatureClicked()
{
	if (Controller.IsValid() && SelectedTier.IsValid() && TierFeatureKeyBox.IsValid())
	{
		Controller->RevokeTierFeature(SelectedTier->TierId, TierFeatureKeyBox->GetText().ToString());
	}
	return FReply::Handled();
}

FReply SCrowdyGameModelView::OnSetPolicyClicked()
{
	if (Controller.IsValid() && ParticipantRoleBox.IsValid())
	{
		Controller->SetGameModelPolicy(SessionCreationPolicy, ParticipantRoleBox->GetText().ToString());
	}
	return FReply::Handled();
}

FReply SCrowdyGameModelView::OnSeedClicked()
{
	if (Controller.IsValid() && SeedBox.IsValid())
	{
		Controller->SeedGameModel(SeedBox->GetText().ToString());
	}
	return FReply::Handled();
}

FReply SCrowdyGameModelView::OnAddParamClicked()
{
	const TSharedPtr<FStudioFunctionParam> Param = MakeShared<FStudioFunctionParam>();
	Param->ValueType = TEXT("int");
	Param->bRequired = true;
	EditParams.Add(Param);
	RebuildParamsRows();
	return FReply::Handled();
}

void SCrowdyGameModelView::RebuildParamsRows()
{
	if (!ParamsRows.IsValid())
	{
		return;
	}

	ParamsRows->ClearChildren();
	for (const TSharedPtr<FStudioFunctionParam>& Param : EditParams)
	{
		ParamsRows->AddSlot().AutoHeight().Padding(0.0f, 2.0f)[ MakeParamRow(Param) ];
	}
}

TSharedRef<SWidget> SCrowdyGameModelView::MakeParamRow(TSharedPtr<FStudioFunctionParam> Param)
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	// Each widget edits the parameter in place through the shared pointer, so the row stays correct
	// even as rows are added or removed above it.
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			SNew(SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("ParamName", "name"))
			.Text(FText::FromString(Param->Name))
			.OnTextChanged_Lambda([Param](const FText& NewText) { Param->Name = NewText.ToString(); })
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			SNew(SBox).WidthOverride(120.0f)
			[
				SNew(SComboBox<TSharedPtr<FString>>)
				.OptionsSource(&ValueTypeOptions)
				.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) { return SNew(STextBlock).Text(FText::FromString(Item.IsValid() ? *Item : FString())); })
				.OnSelectionChanged_Lambda([Param](TSharedPtr<FString> Item, ESelectInfo::Type) { if (Item.IsValid()) { Param->ValueType = *Item; } })
				[ SNew(STextBlock).Text_Lambda([Param]() { return FText::FromString(Param->ValueType); }) ]
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			SNew(SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("ParamDefault", "default (JSON, optional)"))
			.Text(FText::FromString(Param->DefaultValueJson))
			.OnTextChanged_Lambda([Param](const FText& NewText) { Param->DefaultValueJson = NewText.ToString(); })
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			SNew(SCheckBox)
			.IsChecked(Param->bRequired ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
			.OnCheckStateChanged_Lambda([Param](ECheckBoxState State) { Param->bRequired = (State == ECheckBoxState::Checked); })
			[ SNew(STextBlock).Text(LOCTEXT("ParamRequired", "required")).TextStyle(&Style, "Crowdy.Text.Subtle") ]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Secondary").ContentPadding(FMargin(8.0f, 3.0f))
			.ToolTipText(LOCTEXT("ParamRemoveTip", "Remove this parameter"))
			.OnClicked_Lambda([this, Param]() { EditParams.Remove(Param); RebuildParamsRows(); return FReply::Handled(); })
			[ SNew(STextBlock).Text(LOCTEXT("ParamRemove", "Remove")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 9)).ColorAndOpacity(FSlateColor::UseForeground()) ]
		];
}

FReply SCrowdyGameModelView::OnAddMutationClicked()
{
	const TSharedPtr<FStudioFunctionMutation> Mutation = MakeShared<FStudioFunctionMutation>();
	Mutation->Target = TEXT("self");
	EditMutations.Add(Mutation);
	RebuildMutationRows();
	return FReply::Handled();
}

void SCrowdyGameModelView::RebuildTargetOptions()
{
	TargetOptions.Reset();
	TargetOptions.Add(MakeShared<FString>(TEXT("self")));
	if (Controller.IsValid())
	{
		for (const TSharedPtr<FStudioContainerType>& Type : Controller->GetContainerTypes())
		{
			if (Type.IsValid() && !Type->TypeName.IsEmpty())
			{
				TargetOptions.Add(MakeShared<FString>(Type->TypeName));
			}
		}
	}
}

void SCrowdyGameModelView::RebuildMutationRows()
{
	if (!MutationRows.IsValid())
	{
		return;
	}

	MutationRows->ClearChildren();
	for (const TSharedPtr<FStudioFunctionMutation>& Mutation : EditMutations)
	{
		MutationRows->AddSlot().AutoHeight().Padding(0.0f, 2.0f)[ MakeMutationRow(Mutation) ];
	}
}

TSharedRef<SWidget> SCrowdyGameModelView::MakeMutationRow(TSharedPtr<FStudioFunctionMutation> Mutation)
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	// Target is "self" (the function's own container) or another container type; property is a key
	// suggested from the loaded container type. Each widget edits the mutation through the shared
	// pointer, so rows stay correct as others are added or removed.
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			SNew(SBox).WidthOverride(110.0f)
			[
				SNew(SComboBox<TSharedPtr<FString>>)
				.OptionsSource(&TargetOptions)
				.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) { return SNew(STextBlock).Text(FText::FromString(Item.IsValid() ? *Item : FString())); })
				.OnSelectionChanged_Lambda([Mutation](TSharedPtr<FString> Item, ESelectInfo::Type) { if (Item.IsValid()) { Mutation->Target = *Item; } })
				[ SNew(STextBlock).Text_Lambda([Mutation]() { return FText::FromString(Mutation->Target.IsEmpty() ? FString(TEXT("self")) : Mutation->Target); }) ]
			]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			SNew(SBox).WidthOverride(140.0f)
			[
				SNew(SComboBox<TSharedPtr<FString>>)
				.OptionsSource(&PropertyOptions)
				.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) { return SNew(STextBlock).Text(FText::FromString(Item.IsValid() ? *Item : FString())); })
				.OnSelectionChanged_Lambda([Mutation](TSharedPtr<FString> Item, ESelectInfo::Type) { if (Item.IsValid()) { Mutation->Property = *Item; } })
				[ SNew(STextBlock).Text_Lambda([Mutation]() { return FText::FromString(Mutation->Property.IsEmpty() ? FString(TEXT("property")) : Mutation->Property); }) ]
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			SNew(SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("MutExpr", "expression, e.g. hp - $amount"))
			.Text(FText::FromString(Mutation->Expression))
			.OnTextChanged_Lambda([Mutation](const FText& NewText) { Mutation->Expression = NewText.ToString(); })
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Secondary").ContentPadding(FMargin(8.0f, 3.0f))
			.ToolTipText(LOCTEXT("MutRemoveTip", "Remove this mutation"))
			.OnClicked_Lambda([this, Mutation]() { EditMutations.Remove(Mutation); RebuildMutationRows(); return FReply::Handled(); })
			[ SNew(STextBlock).Text(LOCTEXT("MutRemove", "Remove")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 9)).ColorAndOpacity(FSlateColor::UseForeground()) ]
		];
}

FReply SCrowdyGameModelView::OnAddPolicyRuleClicked()
{
	const TSharedPtr<FStudioPolicyRule> Rule = MakeShared<FStudioPolicyRule>();
	Rule->Type = TEXT("owner_of_self");
	EditPolicy.Add(Rule);
	RebuildPolicyRows();
	return FReply::Handled();
}

FReply SCrowdyGameModelView::OnTogglePolicyJsonClicked()
{
	if (bPolicyRawMode)
	{
		// Raw -> builder: only switch if the JSON is something the builder can represent.
		TArray<TSharedPtr<FStudioPolicyRule>> ParsedRules;
		FString ParsedConnector;
		if (FnPolicyBox.IsValid() && ParsePolicyJson(FnPolicyBox->GetText().ToString(), ParsedRules, ParsedConnector))
		{
			EditPolicy = ParsedRules;
			PolicyConnector = ParsedConnector;
			bPolicyRawMode = false;
			bPolicyNotRepresentable = false;
			RebuildPolicyRows();
		}
		else
		{
			bPolicyNotRepresentable = true;
		}
	}
	else
	{
		// Builder -> raw: serialize the current builder state into the box for hand-editing.
		if (FnPolicyBox.IsValid())
		{
			FnPolicyBox->SetText(FText::FromString(PolicyToJson(PolicyConnector, EditPolicy)));
		}
		bPolicyRawMode = true;
		bPolicyNotRepresentable = false;
	}
	return FReply::Handled();
}

void SCrowdyGameModelView::RebuildPolicyRows()
{
	if (!PolicyRows.IsValid())
	{
		return;
	}

	PolicyRows->ClearChildren();
	for (const TSharedPtr<FStudioPolicyRule>& Rule : EditPolicy)
	{
		PolicyRows->AddSlot().AutoHeight().Padding(0.0f, 2.0f)[ MakePolicyRow(Rule) ];
	}
}

TSharedRef<SWidget> SCrowdyGameModelView::MakePolicyRow(TSharedPtr<FStudioPolicyRule> Rule)
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	// The contextual field lives in its own host so changing the rule type swaps just the field, without
	// destroying the type combo mid-callback. The field and Remove button edit the rule in place.
	const TSharedRef<SBox> FieldHost = SNew(SBox)[ MakePolicyRuleField(Rule) ];

	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			SNew(SBox).WidthOverride(170.0f)
			[
				SNew(SComboBox<TSharedPtr<FString>>)
				.OptionsSource(&PolicyTypeOptions)
				.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) { return SNew(STextBlock).Text(PolicyTypeLabel(Item.IsValid() ? *Item : FString())); })
				.OnSelectionChanged_Lambda([this, Rule, FieldHost](TSharedPtr<FString> Item, ESelectInfo::Type) { if (Item.IsValid()) { Rule->Type = *Item; FieldHost->SetContent(MakePolicyRuleField(Rule)); } })
				[ SNew(STextBlock).Text_Lambda([Rule]() { return PolicyTypeLabel(Rule->Type); }) ]
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[ FieldHost ]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Secondary").ContentPadding(FMargin(8.0f, 3.0f))
			.ToolTipText(LOCTEXT("PolRemoveTip", "Remove this requirement"))
			.OnClicked_Lambda([this, Rule]() { EditPolicy.Remove(Rule); RebuildPolicyRows(); return FReply::Handled(); })
			[ SNew(STextBlock).Text(LOCTEXT("PolRemove", "Remove")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 9)).ColorAndOpacity(FSlateColor::UseForeground()) ]
		];
}

TSharedRef<SWidget> SCrowdyGameModelView::MakePolicyRuleField(TSharedPtr<FStudioPolicyRule> Rule)
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	if (Rule->Type == TEXT("tier_feature"))
	{
		return SNew(SComboBox<TSharedPtr<FString>>)
			.OptionsSource(&PolicyFeatureOptions)
			.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) { return SNew(STextBlock).Text(FText::FromString(Item.IsValid() ? *Item : FString())); })
			.OnSelectionChanged_Lambda([Rule](TSharedPtr<FString> Item, ESelectInfo::Type) { if (Item.IsValid()) { Rule->Feature = *Item; } })
			[ SNew(STextBlock).Text_Lambda([Rule]() { return FText::FromString(Rule->Feature.IsEmpty() ? FString(TEXT("feature...")) : Rule->Feature); }) ];
	}

	if (Rule->Type == TEXT("grid_permission"))
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SNew(SComboBox<TSharedPtr<FString>>)
				.OptionsSource(&PolicyGridKeyOptions)
				.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) { return SNew(STextBlock).Text(FText::FromString(Item.IsValid() ? *Item : FString())); })
				.OnSelectionChanged_Lambda([Rule](TSharedPtr<FString> Item, ESelectInfo::Type) { if (Item.IsValid()) { Rule->Key = *Item; } })
				[ SNew(STextBlock).Text_Lambda([Rule]() { return FText::FromString(Rule->Key.IsEmpty() ? FString(TEXT("permission key...")) : Rule->Key); }) ]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[ SNew(SBox).WidthOverride(120.0f)[ SNew(SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("PolGridId", "gridId (optional)")).Text(FText::FromString(Rule->GridId)).OnTextChanged_Lambda([Rule](const FText& T) { Rule->GridId = T.ToString(); }) ] ];
	}

	if (Rule->Type == TEXT("group_permission"))
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[ SNew(SBox).WidthOverride(110.0f)[ SNew(SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("PolGroupId", "groupId")).Text(FText::FromString(Rule->GroupId)).OnTextChanged_Lambda([Rule](const FText& T) { Rule->GroupId = T.ToString(); }) ] ]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[ SNew(SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("PolPermission", "permission (optional)")).Text(FText::FromString(Rule->Permission)).OnTextChanged_Lambda([Rule](const FText& T) { Rule->Permission = T.ToString(); }) ];
	}

	if (Rule->Type == TEXT("condition"))
	{
		return SNew(SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("PolExpr", "expression, e.g. self.level >= 5"))
			.Text(FText::FromString(Rule->Expression))
			.OnTextChanged_Lambda([Rule](const FText& T) { Rule->Expression = T.ToString(); });
	}

	// owner_of_self / is_current_turn / is_host / is_participant carry no extra fields.
	return SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle").Text(LOCTEXT("PolNoFields", "no extra settings"));
}

TSharedRef<SWidget> SCrowdyGameModelView::MakeExpressionCheatSheet()
{
	auto Section = [](const FText& Title, const FString& Body) -> TSharedRef<SWidget>
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 2.0f)
			[ SNew(STextBlock).Text(Title).Font(FCoreStyle::GetDefaultFontStyle("Bold", 9)).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSecondary())) ]
			+ SVerticalBox::Slot().AutoHeight()
			[ SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(Body)).Font(FCoreStyle::GetDefaultFontStyle("Mono", 9)) ];
	};

	// Properties come from whatever container type is loaded in the list above (the same source the
	// mutation property dropdowns use); it may differ from the function's bound type until that type is
	// selected, so the label is honest about where they come from.
	FString PropsText;
	for (const TSharedPtr<FString>& Key : PropertyOptions)
	{
		if (Key.IsValid() && !Key->IsEmpty())
		{
			PropsText += (PropsText.IsEmpty() ? TEXT("") : TEXT("   ")) + *Key;
		}
	}
	if (PropsText.IsEmpty())
	{
		PropsText = TEXT("(select a type in the list above to see its properties)");
	}

	return SNew(SBox).WidthOverride(380.0f)
	[
		CrowdyStudioWidgets::Card(
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[ Section(LOCTEXT("ExprReads", "Reads"), TEXT("self.key    $param    ref($param).key    ref(\"<uuid>\").key")) ]
			+ SVerticalBox::Slot().AutoHeight()[ Section(LOCTEXT("ExprOps", "Operators"), TEXT("+  -  *  /  %      ==  !=  <  >  <=  >=      &&  ||  !")) ]
			+ SVerticalBox::Slot().AutoHeight()[ Section(LOCTEXT("ExprCond", "Conditional"), TEXT("if(condition, then, else)")) ]
			+ SVerticalBox::Slot().AutoHeight()[ Section(LOCTEXT("ExprBuiltins", "Builtins"), TEXT("max min abs floor ceil round clamp pow sqrt len concat to_int to_float to_string rand rand_int not is_null coalesce")) ]
			+ SVerticalBox::Slot().AutoHeight()[ Section(LOCTEXT("ExprCall", "Call another function (read-only)"), TEXT("fn:other_function(args)")) ]
			+ SVerticalBox::Slot().AutoHeight()[ Section(LOCTEXT("ExprProps", "Bound-type properties"), PropsText) ],
			FMargin(14.0f, 12.0f))
	];
}

void SCrowdyGameModelView::OnTypeSelectionChanged(TSharedPtr<FStudioContainerType> Type, ESelectInfo::Type)
{
	if (!Type.IsValid() || !Controller.IsValid())
	{
		return;
	}

	TypeNameBox->SetText(FText::FromString(Type->TypeName));
	DisplayNameBox->SetText(FText::FromString(Type->DisplayName));
	TypeDescBox->SetText(FText::FromString(Type->Description));
	TypeInstantiableBy = Type->InstantiableBy;
	TypeDefaultVis = Type->DefaultPropertyVisibility;

	Controller->FetchPropertyDefs(Type->TypeName);
	Controller->FetchFunctions(Type->TypeName);
}

void SCrowdyGameModelView::OnFunctionSelectionChanged(TSharedPtr<FStudioFunction> Function, ESelectInfo::Type)
{
	if (!Function.IsValid())
	{
		return;
	}

	FnNameBox->SetText(FText::FromString(Function->Name));
	FnTypeBox->SetText(FText::FromString(Function->ContainerTypeName));
	FnDescBox->SetText(FText::FromString(Function->Description));
	FnReturnTypeBox->SetText(FText::FromString(Function->ReturnType));
	FnInvokeScope = Function->InvokeScope;
	FnReturnExprBox->SetText(FText::FromString(Function->ReturnExpression));
	FnWarningsText = JoinWarnings(Function->Warnings);
	EditParams.Reset();
	for (const FStudioFunctionParam& Param : Function->Parameters)
	{
		EditParams.Add(MakeShared<FStudioFunctionParam>(Param));
	}
	RebuildParamsRows();
	EditMutations.Reset();
	for (const FStudioFunctionMutation& Mutation : Function->Mutations)
	{
		EditMutations.Add(MakeShared<FStudioFunctionMutation>(Mutation));
	}
	RebuildMutationRows();

	// Invoke policy: try the guided builder; if the stored tree is nested or unrecognized, fall back to
	// the raw JSON box (with a note) rather than lose it. The raw box always holds the original so the
	// escape hatch shows it verbatim.
	TArray<TSharedPtr<FStudioPolicyRule>> ParsedRules;
	FString ParsedConnector;
	const bool bRepresentable = ParsePolicyJson(Function->InvokePolicyJson, ParsedRules, ParsedConnector);
	if (bRepresentable)
	{
		EditPolicy = ParsedRules;
		PolicyConnector = ParsedConnector;
	}
	else
	{
		EditPolicy.Reset();
	}
	bPolicyRawMode = !bRepresentable;
	bPolicyNotRepresentable = !bRepresentable;
	RebuildPolicyRows();
	if (FnPolicyBox.IsValid())
	{
		FnPolicyBox->SetText(FText::FromString(Function->InvokePolicyJson));
	}
}

void SCrowdyGameModelView::HandleContainerTypesChanged()
{
	if (TypeListView.IsValid())
	{
		TypeListView->RequestListRefresh();
	}

	// The mutation target dropdown lists the container types, so refresh it when they change.
	RebuildTargetOptions();
	RebuildMutationRows();
}

void SCrowdyGameModelView::HandlePropertyDefsChanged()
{
	if (PropertyListView.IsValid())
	{
		PropertyListView->RequestListRefresh();
	}

	// Offer the loaded container type's property keys as suggestions in the mutation property
	// dropdowns.
	PropertyOptions.Reset();
	if (Controller.IsValid())
	{
		for (const TSharedPtr<FStudioPropertyDef>& Def : Controller->GetPropertyDefs())
		{
			if (Def.IsValid())
			{
				PropertyOptions.Add(MakeShared<FString>(Def->Key));
			}
		}
	}
	RebuildMutationRows();
}

void SCrowdyGameModelView::HandleFunctionsChanged()
{
	if (FunctionListView.IsValid())
	{
		FunctionListView->RequestListRefresh();
	}

	// Keep the warnings panel in sync with the freshly fetched function (e.g. right after a save, whose
	// response carries the new static-analysis warnings). Match the function being edited by name.
	FnWarningsText.Reset();
	if (Controller.IsValid() && FnNameBox.IsValid())
	{
		const FString Name = FnNameBox->GetText().ToString();
		if (!Name.IsEmpty())
		{
			for (const TSharedPtr<FStudioFunction>& Fn : Controller->GetFunctions())
			{
				if (Fn.IsValid() && Fn->Name == Name)
				{
					FnWarningsText = JoinWarnings(Fn->Warnings);
					break;
				}
			}
		}
	}
}

void SCrowdyGameModelView::HandleFeaturesChanged()
{
	if (FeatureListView.IsValid())
	{
		FeatureListView->RequestListRefresh();
	}

	// Feed the tier_feature rule's dropdown in the policy builder.
	PolicyFeatureOptions.Reset();
	if (Controller.IsValid())
	{
		for (const TSharedPtr<FStudioAppFeature>& Feature : Controller->GetFeatures())
		{
			if (Feature.IsValid() && !Feature->FeatureKey.IsEmpty())
			{
				PolicyFeatureOptions.Add(MakeShared<FString>(Feature->FeatureKey));
			}
		}
	}
	RebuildPolicyRows();
}

void SCrowdyGameModelView::HandleTierFeaturesChanged()
{
	if (TierFeatureListView.IsValid())
	{
		TierFeatureListView->RequestListRefresh();
	}
}

void SCrowdyGameModelView::HandleAccessTiersChanged()
{
	if (TierComboBox.IsValid())
	{
		TierComboBox->RefreshOptions();
	}
}

void SCrowdyGameModelView::HandleRuntimePermissionsChanged()
{
	// Feed the grid_permission rule's key dropdown in the policy builder (the slice 6b catalog).
	PolicyGridKeyOptions.Reset();
	if (Controller.IsValid())
	{
		for (const FString& Key : Controller->GetRuntimePermissions())
		{
			PolicyGridKeyOptions.Add(MakeShared<FString>(Key));
		}
	}
	RebuildPolicyRows();
}

FString SCrowdyGameModelView::SelectedTypeName() const
{
	if (TypeNameBox.IsValid())
	{
		return TypeNameBox->GetText().ToString();
	}
	return FString();
}

FText SCrowdyGameModelView::GetSelectedTypeLabel() const
{
	const FString TypeName = SelectedTypeName();
	if (TypeName.IsEmpty())
	{
		return LOCTEXT("PropsForType", "Properties (pick a container type above)");
	}
	return FText::Format(LOCTEXT("PropsOfFmt", "Properties of '{0}'"), FText::FromString(TypeName));
}

FText SCrowdyGameModelView::GetPolicyLabel() const
{
	if (Controller.IsValid())
	{
		const FStudioGameModelPolicy& Policy = Controller->GetGameModelPolicy();
		if (Policy.bValid)
		{
			return FText::Format(LOCTEXT("GmPolicyFmt", "Current: creation = {0},  default role = {1}"),
				FText::FromString(Policy.SessionCreationPolicy), FText::FromString(Policy.DefaultParticipantRole));
		}
	}
	return LOCTEXT("NoGmPolicy", "Current policy not loaded. Refresh to read it.");
}

TSharedRef<ITableRow> SCrowdyGameModelView::MakeTypeRow(TSharedPtr<FStudioContainerType> Type, const TSharedRef<STableViewBase>& OwnerTable)
{
	const FString Display = Type.IsValid() ? Type->DisplayName : FString();
	const FString TypeName = Type.IsValid() ? Type->TypeName : FString();
	const FString By = Type.IsValid() ? Type->InstantiableBy : FString();

	return SNew(STableRow<TSharedPtr<FStudioContainerType>>, OwnerTable)
		.Style(&FCrowdyStudioStyle::Get(), "Crowdy.TableRow")
		.Padding(FMargin(0.0f, 1.0f))
		[
			SNew(SBorder).BorderImage(FStyleDefaults::GetNoBrush()).Padding(FMargin(11.0f, 7.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)[ SNew(STextBlock).Text(FText::FromString(Display)).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.BodyStrong") ]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ CrowdyStudioWidgets::Chip(FText::FromString(TypeName)) ]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[ SNew(STextBlock).Text(FText::FromString(By)).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.Subtle") ]
			]
		];
}

TSharedRef<ITableRow> SCrowdyGameModelView::MakePropertyRow(TSharedPtr<FStudioPropertyDef> Def, const TSharedRef<STableViewBase>& OwnerTable)
{
	const FString Key = Def.IsValid() ? Def->Key : FString();
	const FString Detail = Def.IsValid() ? FString::Printf(TEXT(": %s   ·   %s / %s"), *Def->ValueType, *Def->Visibility, *Def->Writable) : FString();

	return SNew(STableRow<TSharedPtr<FStudioPropertyDef>>, OwnerTable)
		.Style(&FCrowdyStudioStyle::Get(), "Crowdy.TableRow")
		.Padding(FMargin(0.0f, 1.0f))
		[
			SNew(SBorder).BorderImage(FStyleDefaults::GetNoBrush()).Padding(FMargin(11.0f, 7.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)[ CrowdyStudioWidgets::Chip(FText::FromString(Key)) ]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)[ SNew(STextBlock).Text(FText::FromString(Detail)).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.Subtle") ]
			]
		];
}

TSharedRef<ITableRow> SCrowdyGameModelView::MakeFunctionRow(TSharedPtr<FStudioFunction> Function, const TSharedRef<STableViewBase>& OwnerTable)
{
	const FString Name = Function.IsValid() ? Function->Name : FString();
	const FString Bound = Function.IsValid() ? (Function->ContainerTypeName.IsEmpty() ? TEXT("global") : Function->ContainerTypeName) : FString();
	const FString Scope = Function.IsValid() ? Function->InvokeScope : FString();

	return SNew(STableRow<TSharedPtr<FStudioFunction>>, OwnerTable)
		.Style(&FCrowdyStudioStyle::Get(), "Crowdy.TableRow")
		.Padding(FMargin(0.0f, 1.0f))
		[
			SNew(SBorder).BorderImage(FStyleDefaults::GetNoBrush()).Padding(FMargin(11.0f, 7.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)[ SNew(STextBlock).Text(FText::FromString(Name)).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.BodyStrong") ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ CrowdyStudioWidgets::Chip(FText::FromString(Bound)) ]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).HAlign(HAlign_Right)[ SNew(STextBlock).Text(FText::FromString(Scope)).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.Subtle") ]
			]
		];
}

TSharedRef<ITableRow> SCrowdyGameModelView::MakeFeatureRow(TSharedPtr<FStudioAppFeature> Feature, const TSharedRef<STableViewBase>& OwnerTable)
{
	const FString Key = Feature.IsValid() ? Feature->FeatureKey : FString();
	const FString Desc = Feature.IsValid() ? Feature->Description : FString();

	return SNew(STableRow<TSharedPtr<FStudioAppFeature>>, OwnerTable)
		.Style(&FCrowdyStudioStyle::Get(), "Crowdy.TableRow")
		.Padding(FMargin(0.0f, 1.0f))
		[
			SNew(SBorder).BorderImage(FStyleDefaults::GetNoBrush()).Padding(FMargin(11.0f, 7.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)[ CrowdyStudioWidgets::Chip(FText::FromString(Key)) ]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)[ SNew(STextBlock).Text(FText::FromString(Desc)).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.Subtle") ]
			]
		];
}

TSharedRef<ITableRow> SCrowdyGameModelView::MakeTierFeatureRow(TSharedPtr<FStudioTierFeature> Grant, const TSharedRef<STableViewBase>& OwnerTable)
{
	const FString Tier = Grant.IsValid() ? FString::Printf(TEXT("tier %lld"), Grant->TierId) : FString();
	const FString Key = Grant.IsValid() ? Grant->FeatureKey : FString();

	return SNew(STableRow<TSharedPtr<FStudioTierFeature>>, OwnerTable)
		.Style(&FCrowdyStudioStyle::Get(), "Crowdy.TableRow")
		.Padding(FMargin(0.0f, 1.0f))
		[
			SNew(SBorder).BorderImage(FStyleDefaults::GetNoBrush()).Padding(FMargin(11.0f, 7.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)[ CrowdyStudioWidgets::Chip(FText::FromString(Tier)) ]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)[ SNew(STextBlock).Text(FText::FromString(Key)).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.BodyStrong") ]
			]
		];
}

FReply SCrowdyGameModelView::OnRefreshContainersClicked()
{
	if (Controller.IsValid())
	{
		Controller->FetchContainers(
			ContainerTypeFilterBox.IsValid() ? ContainerTypeFilterBox->GetText().ToString().TrimStartAndEnd() : FString(),
			ContainerSessionFilterBox.IsValid() ? ContainerSessionFilterBox->GetText().ToString().TrimStartAndEnd() : FString());
	}
	return FReply::Handled();
}

void SCrowdyGameModelView::HandleContainersChanged()
{
	if (ContainerListView.IsValid())
	{
		ContainerListView->RequestListRefresh();
	}
}

void SCrowdyGameModelView::HandleContainerStateChanged()
{
	if (ContainerStateBox.IsValid() && Controller.IsValid())
	{
		const FStudioContainerState& State = Controller->GetContainerState();
		ContainerStateBox->SetText(FText::FromString(State.bValid ? State.PropertiesJson : FString()));
	}
}

void SCrowdyGameModelView::OnContainerSelectionChanged(TSharedPtr<FStudioContainer> Container, ESelectInfo::Type SelectInfo)
{
	// Ignore the programmatic clear the list emits when its source is rebuilt on refresh.
	if (SelectInfo == ESelectInfo::Direct)
	{
		return;
	}
	if (Controller.IsValid() && Container.IsValid())
	{
		Controller->FetchContainerState(Container->ContainerId);
	}
}

TSharedRef<ITableRow> SCrowdyGameModelView::MakeContainerRow(TSharedPtr<FStudioContainer> Container, const TSharedRef<STableViewBase>& OwnerTable)
{
	const FString Type = Container.IsValid() ? Container->TypeName : FString();
	const FString Display = Container.IsValid() ? Container->DisplayName : FString();
	const FString Id = Container.IsValid() ? Container->ContainerId : FString();
	const FString Owner = (Container.IsValid() && Container->OwnerUserId != 0) ? FString::Printf(TEXT("owner #%lld"), Container->OwnerUserId) : FString(TEXT("unowned"));
	const FString Title = Display.IsEmpty() ? Type : FString::Printf(TEXT("%s  (%s)"), *Display, *Type);

	return SNew(STableRow<TSharedPtr<FStudioContainer>>, OwnerTable)
		.Style(&FCrowdyStudioStyle::Get(), "Crowdy.TableRow")
		.Padding(FMargin(0.0f, 1.0f))
		[
			SNew(SBorder).BorderImage(FStyleDefaults::GetNoBrush()).Padding(FMargin(11.0f, 7.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[ SNew(STextBlock).Text(FText::FromString(Title)).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.BodyStrong") ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
					[ SNew(STextBlock).Text(FText::FromString(Id)).Font(FCoreStyle::GetDefaultFontStyle("Mono", 8)).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSubtle())) ]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[ SNew(STextBlock).Text(FText::FromString(Owner)).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.Subtle") ]
			]
		];
}

#undef LOCTEXT_NAMESPACE
