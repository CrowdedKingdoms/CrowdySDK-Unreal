// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameModel/CrowdyModelLedger.h"
#include "Templates/Function.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class FCrowdyStudioController;
class ITableRow;
class SHeaderRow;
class STableViewBase;
class STextBlock;

// One column of a model table: what it is called, how much width it takes, how its text reads, and how to get
// its value out of a row. A table is described by an array of these, so a table with a different set of columns
// is a different array rather than a second widget written alongside this one.
struct FCrowdyModelColumn
{
	FName ColumnId;

	FText Label;

	// This column's share of the table's width, relative to the other columns' shares. Ignored when FixedWidth is
	// set.
	float FillWidth = 0.25f;

	// A width in pixels this column keeps whatever the table's is. Zero shares the width out by FillWidth instead.
	// A gutter holding one small mark is the case for it: sharing out a proportion of the table would make that
	// mark's column grow with the window for no reason.
	float FixedWidth = 0.0f;

	// Ids, session keys and binding keys are opaque strings that get read character by character and compared by
	// eye, so they are set in a fixed-width face where a transposed pair is visible.
	bool bMonospace = false;

	// Dropped once the table is too narrow to give every column a readable width. A header column has no
	// visibility attribute, so whether its widget is generated at all is the only control there is.
	bool bDropWhenNarrow = false;

	// The table width below which this column is dropped. Zero takes the shared default. Columns give way one at a
	// time rather than all together, so a table narrowing past one threshold keeps everything the next one allows.
	float DropBelowTableWidth = 0.0f;

	// Draws the row's provenance as a shape instead of text. The one column that is not a string: provenance is a
	// mark in the gutter, and Cell is left unset for it.
	bool bProvenanceGlyph = false;

	// Name of the text style the cell is drawn in, in the Studio style set. Null takes the body style.
	const TCHAR* TextStyle = nullptr;

	// Reads this column's value out of a row. It must return a reference to a member of the row it is handed:
	// a reference to a temporary or to a local dangles before the cell it feeds is even built, and the read that
	// follows is silent far more often than it crashes.
	TFunction<const FString&(const FCrowdyModelRow&)> Cell;
};

// Wraps built entries into the shared pointers a list view holds its items by. A list view compares its
// selection by pointer identity, so every rebuild necessarily produces a fresh set and whatever was highlighted
// has to be found again by name rather than by pointer.
template <typename ElementType>
TArray<TSharedPtr<ElementType>> CrowdyModelListItems(TArray<ElementType>&& Built)
{
	TArray<TSharedPtr<ElementType>> Items;
	Items.Reserve(Built.Num());
	for (ElementType& Element : Built)
	{
		Items.Add(MakeShared<ElementType>(MoveTemp(Element)));
	}
	return Items;
}

// The column ids the model tables and their rows agree on. Spelling them once keeps a header column and the cell
// that fills it from drifting apart on a typo, which shows up as a permanently blank column rather than as an
// error.
namespace CrowdyModelTableColumns
{
	extern const FName Provenance;
	extern const FName Name;
	extern const FName Detail;
	extern const FName Description;
	extern const FName Source;
	extern const FName Status;

	extern const FName Title;
	extern const FName Id;
	extern const FName Owner;
	extern const FName Session;
	extern const FName Binding;

	// The columns every section of the Models detail pane shows: a gutter mark for where the row came from, its
	// name, one value that differs per section (an attribute's type, a function's result, an automation's
	// schedule) whose caption comes from the caller, its description, that same source in words, and how it
	// differs from the project.
	//
	// The last two are empty on a row with nothing wrong and on every row of an app nobody has checked, which is
	// deliberate: ink appears here in proportion to what needs attention, so a healthy app is plain grey.
	TArray<FCrowdyModelColumn> ModelSectionColumns(const FText& DetailLabel);
}

// A multi-column table over rows built elsewhere, with the columns supplied by the caller. It renders what it is
// handed and reads nothing itself, so moving between sections costs no server traffic. Rows are recycled by the
// list view rather than laid out one widget per entry, so a model with hundreds of attributes costs what a model
// with ten costs.
class SCrowdyModelSectionTable : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyModelSectionTable)
		: _PlaceholderIcon(nullptr)
	{}
		SLATE_ARGUMENT(TSharedPtr<FCrowdyStudioController>, Controller)
		// The columns, in the order they are shown. Fixed for the life of the table.
		SLATE_ARGUMENT(TArray<FCrowdyModelColumn>, Columns)
		// Stands in for the rows while the table holds none.
		SLATE_ARGUMENT(FText, Placeholder)
		SLATE_ARGUMENT(const TCHAR*, PlaceholderIcon)
		// Fires when the highlighted row changes. A change reported as direct is the table replacing its rows
		// rather than anyone choosing anything, which is why the two are told apart at the callback.
		SLATE_EVENT(SListView<TSharedPtr<FCrowdyModelRow>>::FOnSelectionChanged, OnSelectionChanged)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	// Replace everything the table shows. The rows are taken by value, so the caller is free to rebuild its own
	// array afterwards.
	//
	// A highlighted row survives the replacement when the new set still holds the entity it named. Most replacements
	// are a repaint of the same entities after an unrelated read landed, and the highlight is what arms the controls
	// that act on a row, so dropping it there disarms them under the reader with nothing on screen to say why.
	void SetRows(TArray<TSharedPtr<FCrowdyModelRow>> InRows);

	// Whether two rows name the same entity: the same kind of thing, with the same name, on the same model. Entity
	// names and model names are server keys, and the default string comparison folds case, so both compare
	// case-sensitively. Public and static so the rule can be exercised without a table to put rows in.
	static bool IsSameEntity(const FCrowdyModelRow& A, const FCrowdyModelRow& B);

	// The message shown while the table is empty. "Not read yet" and "there are genuinely none" are different
	// answers to look at, and only the caller knows which one applies, so the message comes from outside.
	void SetPlaceholder(const FText& Message);

	int32 NumRows() const { return Rows.Num(); }

	// How many rows are highlighted at this instant. A control that acts on the selection asks this every paint
	// rather than remembering an answer, so it can never stay armed while the table shows nothing highlighted.
	int32 NumSelectedRows() const;

	// The highlighted row, read from the table at the moment of the call and deliberately never cached.
	// Replacing the rows builds a whole new set of objects and drops the vanished one silently, so a remembered
	// pointer outlives the row it names and keeps pointing at something no longer on screen.
	TSharedPtr<FCrowdyModelRow> GetSelectedRow() const;

	// Highlight the row naming this entity and scroll it into view. The entity is matched by IsSameEntity, which is
	// already the rule a rebuild uses to carry a highlight across a replaced row set, so a link and a refresh agree
	// about what "the same row" means; nothing is matched by position, because a rebuild replaces every row object.
	//
	// Returns false when no such row is present, so a caller can say so rather than silently doing nothing. That is
	// the answer for a model whose attributes have not been read yet: there is no row to land on and the caller has
	// to wait for the read rather than treat the entity as absent.
	bool SelectRowForEntity(const FCrowdyModelRow& Entity);

	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

private:
	TSharedRef<ITableRow> MakeRow(TSharedPtr<FCrowdyModelRow> Row, const TSharedRef<STableViewBase>& OwnerTable);

	// Whether the column at this index currently has the width it asked for. Bound per column rather than shared,
	// because columns give way at different widths and a single answer would drop them all at once.
	bool IsColumnShown(int32 ColumnIndex) const;

	// Which columns fit, one bit per column index, recomputed only when the table's width crosses one of their
	// thresholds. Every column shows until the first Tick measures anything.
	uint32 ShownColumnMask = MAX_uint32;

	// The table issues no reads of its own. The controller is held so that every widget on this page is
	// constructed the same way and a later section that does need it costs no change at the call sites.
	TSharedPtr<FCrowdyStudioController> Controller;

	// Shared with every row, rather than pointed at from them, so that a row still holding on after the table
	// went away reads a column set that is still there.
	TSharedPtr<const TArray<FCrowdyModelColumn>> Columns;

	TSharedPtr<SHeaderRow> HeaderRow;
	TSharedPtr<SListView<TSharedPtr<FCrowdyModelRow>>> RowListView;
	TSharedPtr<STextBlock> PlaceholderText;

	TArray<TSharedPtr<FCrowdyModelRow>> Rows;

	// True when at least one column can drop. With none, the width is nothing this table has to watch.
	bool bHasDroppableColumns = false;
};
