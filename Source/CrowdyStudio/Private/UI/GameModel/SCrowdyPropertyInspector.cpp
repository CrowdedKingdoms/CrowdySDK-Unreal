// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/GameModel/SCrowdyPropertyInspector.h"

#include "Dom/JsonObject.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Model/FCrowdyStudioController.h"
#include "Serialization/CrowdyJsonSafety.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Style/CrowdyStudioStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/ISlateStyle.h"
#include "UI/CrowdyStudioWidgets.h"
#include "UI/GameModel/SCrowdyModelSectionTable.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CrowdyStudio"

namespace
{
	// The values as they would be read rather than as they crossed the wire: one field per line, indented. The
	// clipboard is where a reader takes a value to paste it somewhere else, and a condensed object is one long line.
	FString CrowdyPropertiesForClipboard(const FString& PropertiesJson)
	{
		const FString Trimmed = PropertiesJson.TrimStartAndEnd();
		if (Trimmed.IsEmpty() || !CrowdyJsonSafety::IsNestingWithinLimit(Trimmed))
		{
			return Trimmed;
		}

		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			// Exactly what arrived. A payload this editor cannot lay out as rows is the one a reader most needs to
			// get out of here whole.
			return Trimmed;
		}

		FString Pretty;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Pretty);
		FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
		return Pretty;
	}
}

TArray<FCrowdyModelColumn> SCrowdyPropertyInspector::PropertyColumns()
{
	TArray<FCrowdyModelColumn> Columns;
	Columns.Reserve(4);

	FCrowdyModelColumn& AttributeColumn = Columns.AddDefaulted_GetRef();
	AttributeColumn.ColumnId = CrowdyModelTableColumns::Name;
	AttributeColumn.Label = LOCTEXT("PropertyColumnAttribute", "Attribute");
	AttributeColumn.FillWidth = 0.26f;
	AttributeColumn.TextStyle = TEXT("Crowdy.Text.BodyStrong");
	AttributeColumn.Cell = [](const FCrowdyModelRow& Row) -> const FString& { return Row.DisplayLabel(); };

	// The widest column, and the reason the panel exists. Fixed width because this column is mostly numbers,
	// yes/no and short JSON, which line up under each other where prose would not.
	FCrowdyModelColumn& ValueColumn = Columns.AddDefaulted_GetRef();
	ValueColumn.ColumnId = CrowdyModelTableColumns::Value;
	ValueColumn.Label = LOCTEXT("PropertyColumnValue", "Value");
	ValueColumn.FillWidth = 0.44f;
	ValueColumn.bMonospace = true;
	ValueColumn.TextStyle = TEXT("Crowdy.Text.Body");
	ValueColumn.Cell = [](const FCrowdyModelRow& Row) -> const FString& { return Row.Value; };

	FCrowdyModelColumn& TypeColumn = Columns.AddDefaulted_GetRef();
	TypeColumn.ColumnId = CrowdyModelTableColumns::Holds;
	TypeColumn.Label = LOCTEXT("PropertyColumnHolds", "Holds");
	TypeColumn.FillWidth = 0.12f;
	TypeColumn.bDropWhenNarrow = true;
	TypeColumn.DropBelowTableWidth = 620.0f;
	TypeColumn.TextStyle = TEXT("Crowdy.Text.Subtle");
	TypeColumn.Cell = [](const FCrowdyModelRow& Row) -> const FString& { return Row.Secondary; };

	// The first to give way: what an attribute is for is worth reading when there is room, and never at the cost
	// of the value beside it.
	FCrowdyModelColumn& NotesColumn = Columns.AddDefaulted_GetRef();
	NotesColumn.ColumnId = CrowdyModelTableColumns::Description;
	NotesColumn.Label = LOCTEXT("PropertyColumnNotes", "Notes");
	NotesColumn.FillWidth = 0.18f;
	NotesColumn.bDropWhenNarrow = true;
	NotesColumn.DropBelowTableWidth = 900.0f;
	NotesColumn.TextStyle = TEXT("Crowdy.Text.Subtle");
	NotesColumn.Cell = [](const FCrowdyModelRow& Row) -> const FString& { return Row.Detail; };

	return Columns;
}

void SCrowdyPropertyInspector::Construct(const FArguments& InArgs)
{
	Controller = InArgs._Controller;

	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	ChildSlot
	[
		CrowdyStudioWidgets::Card(
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(SHorizontalBox)

				// One line: what this is, then the live model it is of. The id belongs to the line under the rows,
				// where it is still readable and costs the attributes no height.
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("PropertyHeadingLabel", "Values"))
					.TextStyle(&Style, "Crowdy.Text.SectionLabel")
				]

				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SAssignNew(TitleText, STextBlock)
					.TextStyle(&Style, "Crowdy.Text.BodyStrong")
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SBox).WidthOverride(190.0f)
					[
						SAssignNew(SearchBox, SSearchBox)
						.Style(&Style, "Crowdy.SearchBox")
						.HintText(LOCTEXT("PropertySearchHint", "Search attributes and values"))
						.DelayChangeNotificationsWhileTyping(true)
						.OnTextChanged_Lambda([this](const FText&) { Rebuild(); })
					]
				]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SCheckBox)
					.ToolTipText(LOCTEXT("PropertyInternalTip",
						"Also show the keys the runtime keeps for itself, such as the revision counter."))
					.IsChecked_Lambda([this]() { return bShowInternal ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
					{
						bShowInternal = State == ECheckBoxState::Checked;
						Rebuild();
					})
					[
						SNew(SBox).Padding(FMargin(4.0f, 0.0f, 0.0f, 0.0f))
						[
							SNew(STextBlock)
							.Text(LOCTEXT("PropertyInternal", "Internal"))
							.TextStyle(&Style, "Crowdy.Text.Subtle")
						]
					]
				]
			]

			// The rows get whatever the header and the line below them do not. No card of its own around the
			// table: a second border and its padding inside a panel this size is a row and a half of attributes.
			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				SAssignNew(Table, SCrowdyModelSectionTable)
				.Controller(Controller)
				.Columns(PropertyColumns())
				.Placeholder(FText::FromString(
					CrowdyModelEmptyState::PropertyTable(ECrowdyModelLoadState::NeverRequested, false)))
				.PlaceholderIcon(TEXT("inspector"))
				.OnSelectionChanged_Lambda([this](TSharedPtr<FCrowdyModelRow>, ESelectInfo::Type) { UpdateFooter(); })
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 16.0f, 0.0f)
				[
					SAssignNew(FooterText, STextBlock)
					.TextStyle(&Style, "Crowdy.Text.Subtle")
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				]

				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					// The id, on the line under the rows: a name is a label and two live models may carry the same
					// one, but nobody reads an id first, so it sits where it costs the attributes no height.
					SAssignNew(SubtitleText, STextBlock)
					.TextStyle(&Style, "Crowdy.Text.Subtle")
					.Font(FCoreStyle::GetDefaultFontStyle("Mono", 8))
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.ButtonStyle(&Style, "Crowdy.Button.Secondary")
					.ContentPadding(FMargin(11.0f, 5.0f))
					.ToolTipText(LOCTEXT("PropertyCopyValueTip",
						"Copy the highlighted attribute's value to the clipboard, whole and unshortened."))
					.IsEnabled_Lambda([this]()
					{
						const TSharedPtr<FCrowdyModelRow> Row = Table.IsValid() ? Table->GetSelectedRow() : nullptr;
						return Row.IsValid() && Row->bHasValue;
					})
					.OnClicked(FOnClicked::CreateSP(this, &SCrowdyPropertyInspector::OnCopyValueClicked))
					[
						SNew(STextBlock)
						.Text(LOCTEXT("PropertyCopyValue", "Copy value"))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
						.ColorAndOpacity(FSlateColor::UseForeground())
					]
				]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SButton)
					.ButtonStyle(&Style, "Crowdy.Button.Secondary")
					.ContentPadding(FMargin(11.0f, 5.0f))
					.ToolTipText(LOCTEXT("PropertyCopyAllTip",
						"Copy every value this live model holds, as they arrived from the server."))
					.IsEnabled_Lambda([this]() { return !HeldState.PropertiesJson.TrimStartAndEnd().IsEmpty(); })
					.OnClicked(FOnClicked::CreateSP(this, &SCrowdyPropertyInspector::OnCopyAllClicked))
					[
						SNew(STextBlock)
						.Text(LOCTEXT("PropertyCopyAll", "Copy values"))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
						.ColorAndOpacity(FSlateColor::UseForeground())
					]
				]
			],
			FMargin(10.0f, 8.0f))
	];

	Clear();
}

void SCrowdyPropertyInspector::Show(
	ECrowdyModelLoadState InValuesState, const FStudioContainerState& State, const FString& InstanceLabel)
{
	// A copy from the last instance's read would go on being filtered and counted under the new one's heading.
	CopyNotice = FText::GetEmpty();
	ValuesState = InValuesState;
	HeldState = State;
	HeldLabel = InstanceLabel;
	Rebuild();
}

void SCrowdyPropertyInspector::Refresh()
{
	Rebuild();
}

void SCrowdyPropertyInspector::Clear()
{
	ValuesState = ECrowdyModelLoadState::NeverRequested;
	HeldState = FStudioContainerState();
	HeldLabel.Reset();
	CopyNotice = FText::GetEmpty();
	if (SearchBox.IsValid())
	{
		SearchBox->SetText(FText::GetEmpty());
	}
	Rebuild();
}

FString SCrowdyPropertyInspector::SearchQuery() const
{
	return SearchBox.IsValid() ? SearchBox->GetText().ToString().TrimStartAndEnd() : FString();
}

void SCrowdyPropertyInspector::Rebuild()
{
	// The model's attributes and the plan are asked for here rather than passed in, so a vocabulary that lands
	// after the values reaches the rows on the next rebuild without the caller carrying it.
	const TArray<TSharedPtr<FStudioPropertyDef>>* Defs = nullptr;
	TSharedPtr<const FCrowdyModelSnapshot> Snapshot;
	if (Controller.IsValid() && !HeldState.TypeName.IsEmpty())
	{
		Defs = Controller->GetPropertyDefsForType(HeldState.TypeName);
		Snapshot = Controller->GetModelSnapshot();
	}

	const bool bHaveValues = ValuesState == ECrowdyModelLoadState::Loaded && HeldState.bValid;

	AllRows = bHaveValues
		? CrowdyModelLedger::BuildPropertyRows(HeldState.PropertiesJson, Defs, Snapshot.Get(), bShowInternal)
		: TArray<FCrowdyModelRow>();

	const FString Query = SearchQuery();
	TArray<FCrowdyModelRow> Shown = Query.IsEmpty() ? AllRows : CrowdyModelLedger::FilterRows(AllRows, Query);

	if (Table.IsValid())
	{
		// Which of the five empty answers this is cannot be read off an empty list, so it is decided here where
		// all five are still distinguishable.
		FString Placeholder;
		if (!bHaveValues)
		{
			Placeholder = ValuesState == ECrowdyModelLoadState::Loaded
				? CrowdyModelEmptyState::PropertyTableMissing()
				: CrowdyModelEmptyState::PropertyTable(ValuesState, Defs != nullptr);
		}
		else if (CrowdyModelLedger::ClassifyPropertiesJson(HeldState.PropertiesJson)
			== ECrowdyPropertyReadState::Unreadable)
		{
			Placeholder = CrowdyModelEmptyState::PropertyTableUnreadable();
		}
		else if (AllRows.Num() > 0 && Shown.Num() == 0)
		{
			Placeholder = CrowdyModelEmptyState::PropertyTableFilteredBySearch(Query);
		}
		else
		{
			Placeholder = CrowdyModelEmptyState::PropertyTable(ECrowdyModelLoadState::Loaded, Defs != nullptr);
		}

		Table->SetPlaceholder(FText::FromString(Placeholder));
		Table->SetRows(CrowdyModelListItems(MoveTemp(Shown)));
	}

	// Only the live model's name, beside the panel's own label. With none selected the heading stays empty and
	// the table's own placeholder is the one sentence on screen, rather than two saying the same thing.
	const FText Heading = FText::FromString(HeldLabel);
	if (TitleText.IsValid())
	{
		TitleText->SetText(Heading);
		TitleText->SetToolTipText(Heading);
	}
	if (SubtitleText.IsValid())
	{
		SubtitleText->SetText(FText::FromString(HeldState.ContainerId));
		SubtitleText->SetToolTipText(FText::FromString(HeldState.ContainerId));
	}

	UpdateFooter();
}

void SCrowdyPropertyInspector::UpdateFooter()
{
	if (!FooterText.IsValid())
	{
		return;
	}

	if (!CopyNotice.IsEmpty())
	{
		FooterText->SetText(CopyNotice);
		return;
	}

	if (AllRows.Num() == 0)
	{
		FooterText->SetText(FText::GetEmpty());
		return;
	}

	int32 WithValues = 0;
	for (const FCrowdyModelRow& Row : AllRows)
	{
		WithValues += Row.bHasValue ? 1 : 0;
	}
	const int32 NotSet = AllRows.Num() - WithValues;

	// Never a claim about the model as a whole: these count the rows built from one instance's read, and the
	// not-set tail exists only where the model's attributes were known when they were built.
	const int32 Shown = Table.IsValid() ? Table->NumRows() : AllRows.Num();
	if (Shown != AllRows.Num())
	{
		FooterText->SetText(FText::Format(
			LOCTEXT("PropertyFooterFiltered", "Showing {0} of {1} attributes."),
			FText::AsNumber(Shown), FText::AsNumber(AllRows.Num())));
		return;
	}

	FooterText->SetText(NotSet > 0
		? FText::Format(
			LOCTEXT("PropertyFooterWithUnset", "{0} with a value, {1} not set."),
			FText::AsNumber(WithValues), FText::AsNumber(NotSet))
		: FText::Format(LOCTEXT("PropertyFooter", "{0} with a value."), FText::AsNumber(WithValues)));
}

FReply SCrowdyPropertyInspector::OnCopyValueClicked()
{
	const TSharedPtr<FCrowdyModelRow> Row = Table.IsValid() ? Table->GetSelectedRow() : nullptr;
	if (Row.IsValid() && Row->bHasValue)
	{
		// The value as the server holds it, not the shortened form the cell shows.
		FPlatformApplicationMisc::ClipboardCopy(*Row->RawValue);
		CopyNotice = FText::Format(
			LOCTEXT("PropertyCopiedValue", "Copied the value of {0}."), FText::FromString(Row->DisplayLabel()));
		UpdateFooter();
	}
	return FReply::Handled();
}

FReply SCrowdyPropertyInspector::OnCopyAllClicked()
{
	const FString Payload = CrowdyPropertiesForClipboard(HeldState.PropertiesJson);
	if (!Payload.IsEmpty())
	{
		FPlatformApplicationMisc::ClipboardCopy(*Payload);
		CopyNotice = LOCTEXT("PropertyCopiedAll", "Copied every value this live model holds.");
		UpdateFooter();
	}
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
