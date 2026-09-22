// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/GameModel/SCrowdyModelSectionTable.h"

#include "Style/CrowdyStudioStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/ISlateStyle.h"
#include "Styling/SlateTypes.h"
#include "UI/CrowdyStudioWidgets.h"
#include "UI/GameModel/SCrowdyProvenanceGlyph.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "CrowdyStudio"

namespace CrowdyModelTableColumns
{
	const FName Provenance("Provenance");
	const FName Name("Name");
	const FName Detail("Detail");
	const FName Description("Description");
	const FName Source("Source");
	const FName Status("Status");

	const FName Title("Title");
	const FName Id("Id");
	const FName Owner("Owner");
	const FName Session("Session");
	const FName Binding("Binding");

	const FName Value("Value");
	const FName Holds("Holds");

	TArray<FCrowdyModelColumn> ModelSectionColumns(const FText& DetailLabel)
	{
		TArray<FCrowdyModelColumn> Columns;
		Columns.Reserve(6);

		// The gutter. No caption: a heading over a column of marks would be wider than the marks and would say
		// nothing the marks' own tooltips do not.
		FCrowdyModelColumn& ProvenanceColumn = Columns.AddDefaulted_GetRef();
		ProvenanceColumn.ColumnId = CrowdyModelTableColumns::Provenance;
		// Derived, never a number of its own. A hardcoded width here silently caps the mark: the column keeps the
		// size it was written for while the glyph grows, and the shape is squeezed by a file that never mentions it.
		ProvenanceColumn.FixedWidth = SCrowdyProvenanceGlyph::GlyphExtent() + 8.0f;
		ProvenanceColumn.bProvenanceGlyph = true;

		FCrowdyModelColumn& NameColumn = Columns.AddDefaulted_GetRef();
		NameColumn.ColumnId = CrowdyModelTableColumns::Name;
		NameColumn.Label = LOCTEXT("ModelTableColumnName", "Name");
		NameColumn.FillWidth = 0.34f;
		NameColumn.TextStyle = TEXT("Crowdy.Text.BodyStrong");
		// The readable form of the name where the row has one, which is what an attribute's lowercased server key
		// would otherwise cost the reader. The cell's tooltip repeats whatever this returns, so both follow it.
		NameColumn.Cell = [](const FCrowdyModelRow& Row) -> const FString& { return Row.DisplayLabel(); };

		FCrowdyModelColumn& DetailColumn = Columns.AddDefaulted_GetRef();
		DetailColumn.ColumnId = CrowdyModelTableColumns::Detail;
		DetailColumn.Label = DetailLabel;
		DetailColumn.FillWidth = 0.24f;
		DetailColumn.TextStyle = TEXT("Crowdy.Text.Body");
		DetailColumn.Cell = [](const FCrowdyModelRow& Row) -> const FString& { return Row.Secondary; };

		// The first to give way, and the widest, so dropping it buys the most room back.
		FCrowdyModelColumn& DescriptionColumn = Columns.AddDefaulted_GetRef();
		DescriptionColumn.ColumnId = CrowdyModelTableColumns::Description;
		DescriptionColumn.Label = LOCTEXT("ModelTableColumnDescription", "Description");
		DescriptionColumn.FillWidth = 0.30f;
		DescriptionColumn.bDropWhenNarrow = true;
		DescriptionColumn.DropBelowTableWidth = 780.0f;
		DescriptionColumn.TextStyle = TEXT("Crowdy.Text.Subtle");
		DescriptionColumn.Cell = [](const FCrowdyModelRow& Row) -> const FString& { return Row.Detail; };

		// Where the row came from, in the same five words the Crowdy Effect asset's own toolbar uses. It gives way
		// next, because the gutter mark beside the name says the same thing in the space of a character.
		FCrowdyModelColumn& SourceColumn = Columns.AddDefaulted_GetRef();
		SourceColumn.ColumnId = CrowdyModelTableColumns::Source;
		SourceColumn.Label = LOCTEXT("ModelTableColumnSource", "Source");
		SourceColumn.FixedWidth = 112.0f;
		SourceColumn.bDropWhenNarrow = true;
		SourceColumn.TextStyle = TEXT("Crowdy.Text.Subtle");
		SourceColumn.Cell = [](const FCrowdyModelRow& Row) -> const FString&
		{
			// A row that is on the server and matches the project says nothing here. Its word exists, and the mark
			// in the gutter carries it in a tooltip, but printing it would put a reassuring label on every row of a
			// healthy app and leave the rows that need attention indistinguishable from the rest.
			//
			// A static rather than a temporary on purpose: this returns a reference the cell outlives, and a
			// reference to a local would already be dead by the time that cell was built.
			static const FString Silent;
			return Row.Provenance == ECrowdyModelProvenance::CodeSynced ? Silent : Row.ProvenanceText;
		};

		// How the row differs from the project. This one never drops: it is the only column that says something is
		// wrong, and it is empty on most rows, so it costs nothing to keep.
		FCrowdyModelColumn& StatusColumn = Columns.AddDefaulted_GetRef();
		StatusColumn.ColumnId = CrowdyModelTableColumns::Status;
		StatusColumn.Label = LOCTEXT("ModelTableColumnStatus", "Status");
		StatusColumn.FixedWidth = 122.0f;
		StatusColumn.TextStyle = TEXT("Crowdy.Text.Body");
		StatusColumn.Cell = [](const FCrowdyModelRow& Row) -> const FString& { return Row.DriftText; };

		return Columns;
	}
}

namespace
{
	// Below this width a droppable column cannot hold a readable phrase and it squeezes the columns that identify
	// the row, so it is dropped instead. A column that needs more room than this says so for itself.
	constexpr float CrowdyModelNarrowColumnMinTableWidth = 560.0f;

	const TCHAR* const CrowdyModelDefaultCellTextStyle = TEXT("Crowdy.Text.Body");

	float CrowdyModelColumnDropWidth(const FCrowdyModelColumn& Column)
	{
		return Column.DropBelowTableWidth > 0.0f ? Column.DropBelowTableWidth : CrowdyModelNarrowColumnMinTableWidth;
	}
}

// One line of a section table. A multi-column table row has to produce a widget per column: a row that lays its
// own cells out in a horizontal box lines up with the header above it at exactly one width and drifts at every
// other one.
class SCrowdyModelSectionRow : public SMultiColumnTableRow<TSharedPtr<FCrowdyModelRow>>
{
public:
	SLATE_BEGIN_ARGS(SCrowdyModelSectionRow) {}
		SLATE_ARGUMENT(TSharedPtr<FCrowdyModelRow>, Row)
		SLATE_ARGUMENT(TSharedPtr<const TArray<FCrowdyModelColumn>>, Columns)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& OwnerTable)
	{
		Row = InArgs._Row;
		Columns = InArgs._Columns;

		FSuperRowType::Construct(
			FSuperRowType::FArguments()
				.Style(&FCrowdyStudioStyle::Get(), "Crowdy.TableRow")
				.Padding(FMargin(0.0f, 1.0f)),
			OwnerTable);
	}

	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override
	{
		if (!Row.IsValid() || !Columns.IsValid())
		{
			return SNullWidget::NullWidget;
		}

		for (const FCrowdyModelColumn& Column : *Columns)
		{
			if (Column.ColumnId != ColumnName)
			{
				continue;
			}
			if (Column.bProvenanceGlyph)
			{
				return SNew(SBox)
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					[
						SNew(SCrowdyProvenanceGlyph).Provenance(Row->Provenance)
					];
			}
			if (Column.Cell)
			{
				return Cell(Column.Cell(*Row), Column);
			}
		}

		return SNullWidget::NullWidget;
	}

private:
	// Every string a cell shows was built once, when the row was built, so nothing here is recomputed while the
	// table is on screen. The tooltip repeats the cell because a cell narrower than its text ends in an ellipsis
	// with no other way to read the rest.
	static TSharedRef<SWidget> Cell(const FString& Text, const FCrowdyModelColumn& Column)
	{
		const FText Value = FText::FromString(Text);
		const TCHAR* TextStyle = Column.TextStyle != nullptr ? Column.TextStyle : CrowdyModelDefaultCellTextStyle;

		TSharedRef<STextBlock> Block = SNew(STextBlock)
			.Text(Value)
			.ToolTipText(Value)
			.TextStyle(&FCrowdyStudioStyle::Get(), TextStyle)
			.OverflowPolicy(ETextOverflowPolicy::Ellipsis);

		if (Column.bMonospace)
		{
			// Overriding the font alone keeps the column's colour and weight, so a mono column still reads as the
			// same kind of text as the one beside it.
			Block->SetFont(FCoreStyle::GetDefaultFontStyle("Mono", 9.0f));
		}

		return SNew(SBox)
			.VAlign(VAlign_Center)
			.Padding(FMargin(10.0f, 6.0f))
			[
				Block
			];
	}

	TSharedPtr<FCrowdyModelRow> Row;
	TSharedPtr<const TArray<FCrowdyModelColumn>> Columns;
};

void SCrowdyModelSectionTable::Construct(const FArguments& InArgs)
{
	Controller = InArgs._Controller;

	const TSharedRef<TArray<FCrowdyModelColumn>> ColumnSet = MakeShared<TArray<FCrowdyModelColumn>>(InArgs._Columns);
	Columns = ColumnSet;

	const ISlateStyle& Style = FCrowdyStudioStyle::Get();
	const TCHAR* PlaceholderIcon = InArgs._PlaceholderIcon != nullptr ? InArgs._PlaceholderIcon : TEXT("cube");

	SAssignNew(HeaderRow, SHeaderRow)
		.Style(&Style, "Crowdy.HeaderRow");

	for (int32 Index = 0; Index < ColumnSet->Num(); ++Index)
	{
		const FCrowdyModelColumn& Column = (*ColumnSet)[Index];

		SHeaderRow::FColumn::FArguments ColumnArgs = SHeaderRow::Column(Column.ColumnId);
		ColumnArgs.DefaultLabel(Column.Label);
		if (Column.FixedWidth > 0.0f)
		{
			ColumnArgs.FixedWidth(Column.FixedWidth);
		}
		else
		{
			ColumnArgs.FillWidth(Column.FillWidth);
		}

		if (Column.bDropWhenNarrow)
		{
			ColumnArgs.ShouldGenerateWidget(
				TAttribute<bool>::CreateSP(this, &SCrowdyModelSectionTable::IsColumnShown, Index));
			bHasDroppableColumns = true;
		}

		HeaderRow->AddColumn(ColumnArgs);
	}

	ChildSlot
	[
		SNew(SOverlay)

		+ SOverlay::Slot()
		[
			SAssignNew(RowListView, SListView<TSharedPtr<FCrowdyModelRow>>)
			.ListViewStyle(&Style, "Crowdy.TableView")
			.ListItemsSource(&Rows)
			.OnGenerateRow(this, &SCrowdyModelSectionTable::MakeRow)
			.OnSelectionChanged(InArgs._OnSelectionChanged)
			.SelectionMode(ESelectionMode::Single)
			.HeaderRow(HeaderRow)
		]

		+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
		[
			SNew(SBox)
			.Padding(20.0f)
			// Hit-test invisible so that the message never swallows a scroll aimed at the list underneath it.
			.Visibility_Lambda([this]() { return Rows.Num() == 0 ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 0.0f, 0.0f, 10.0f)
				[ CrowdyStudioWidgets::Icon(PlaceholderIcon, 26.0f, FSlateColor(FCrowdyStudioStyle::TextSubtle())) ]
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
				[
					SAssignNew(PlaceholderText, STextBlock)
					.Text(InArgs._Placeholder)
					.TextStyle(&Style, "Crowdy.Text.Subtle")
					.Justification(ETextJustify::Center)
					.AutoWrapText(true)
				]
			]
		]
	];
}

bool SCrowdyModelSectionTable::IsSameEntity(const FCrowdyModelRow& A, const FCrowdyModelRow& B)
{
	return A.Kind == B.Kind
		&& A.Name.Equals(B.Name, ESearchCase::CaseSensitive)
		&& A.OwningType.Equals(B.OwningType, ESearchCase::CaseSensitive);
}

void SCrowdyModelSectionTable::SetRows(TArray<TSharedPtr<FCrowdyModelRow>> InRows)
{
	// Held by value, so it outlives the row set it came from and can be compared against the new one.
	const TSharedPtr<FCrowdyModelRow> WasHighlighted = GetSelectedRow();

	Rows = MoveTemp(InRows);

	if (RowListView.IsValid())
	{
		// A replaced source is a whole new set of row objects, so the one the list was holding is gone whatever
		// happens next.
		RowListView->ClearSelection();

		// The entity it named usually is not gone: an attribute read landing does not change which functions a model
		// has, and a plan finishing changes what the rows SAY rather than which rows there are. So the highlight is
		// carried over to the new row for the same entity, and dropped only when there is no such row.
		if (WasHighlighted.IsValid())
		{
			for (const TSharedPtr<FCrowdyModelRow>& Row : Rows)
			{
				if (Row.IsValid() && IsSameEntity(*Row, *WasHighlighted))
				{
					// Announced as a direct change, which the selection handlers ignore by design: nobody chose
					// anything here, the same choice is being pointed at the object that now carries it.
					RowListView->SetSelection(Row, ESelectInfo::Direct);
					break;
				}
			}
		}

		RowListView->RequestListRefresh();
	}
}

int32 SCrowdyModelSectionTable::NumSelectedRows() const
{
	return RowListView.IsValid() ? RowListView->GetNumItemsSelected() : 0;
}

TSharedPtr<FCrowdyModelRow> SCrowdyModelSectionTable::GetSelectedRow() const
{
	if (!RowListView.IsValid())
	{
		return nullptr;
	}

	const TArray<TSharedPtr<FCrowdyModelRow>> Selected = RowListView->GetSelectedItems();
	return Selected.Num() > 0 ? Selected[0] : nullptr;
}

bool SCrowdyModelSectionTable::SelectRowForEntity(const FCrowdyModelRow& Entity)
{
	if (!RowListView.IsValid())
	{
		return false;
	}

	for (const TSharedPtr<FCrowdyModelRow>& Row : Rows)
	{
		if (!Row.IsValid() || !IsSameEntity(*Row, Entity))
		{
			continue;
		}

		// Announced as a direct change, exactly as carrying a highlight across a rebuild is: nobody clicked
		// anything here, a choice made elsewhere is being pointed at the object that carries it.
		RowListView->SetSelection(Row, ESelectInfo::Direct);
		RowListView->RequestScrollIntoView(Row);
		return true;
	}

	return false;
}

void SCrowdyModelSectionTable::SetPlaceholder(const FText& Message)
{
	if (PlaceholderText.IsValid())
	{
		PlaceholderText->SetText(Message);
	}
}

bool SCrowdyModelSectionTable::IsColumnShown(int32 ColumnIndex) const
{
	// A column past the width of the mask is never dropped. Thirty-two columns is far more than any table here has,
	// and silently hiding one would be a worse answer than showing it.
	if (ColumnIndex < 0 || ColumnIndex >= 32)
	{
		return true;
	}
	return (ShownColumnMask & (1u << ColumnIndex)) != 0u;
}

void SCrowdyModelSectionTable::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

	if (!bHasDroppableColumns || !Columns.IsValid())
	{
		return;
	}

	const float TableWidth = AllottedGeometry.GetLocalSize().X;

	uint32 Fits = MAX_uint32;
	const int32 Count = FMath::Min(Columns->Num(), 32);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FCrowdyModelColumn& Column = (*Columns)[Index];
		if (Column.bDropWhenNarrow && TableWidth < CrowdyModelColumnDropWidth(Column))
		{
			Fits &= ~(1u << Index);
		}
	}

	if (Fits == ShownColumnMask)
	{
		return;
	}

	ShownColumnMask = Fits;

	if (HeaderRow.IsValid())
	{
		HeaderRow->RefreshColumns();
	}
	if (RowListView.IsValid())
	{
		// Refreshing the header regenerates the header widgets alone. A row decides which cells it has from the
		// header's column set at the moment it is built, so the rows have to be built again to follow it.
		RowListView->RebuildList();
	}
}

TSharedRef<ITableRow> SCrowdyModelSectionTable::MakeRow(TSharedPtr<FCrowdyModelRow> Row, const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(SCrowdyModelSectionRow, OwnerTable)
		.Row(Row)
		.Columns(Columns);
}

#undef LOCTEXT_NAMESPACE
