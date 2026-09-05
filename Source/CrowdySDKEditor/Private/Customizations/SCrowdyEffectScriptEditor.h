// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Customizations/CrowdyExpressionEditorModel.h"
#include "Customizations/SCrowdyExpressionEditor.h"
#include "Styling/SlateColor.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class FCrowdyEffectScriptHighlightMarshaller;
class ITableRow;
class SBorder;
class SMenuAnchor;
class SMultiLineEditableTextBox;
class STableViewBase;

/**
 * A multi-line EffectScript body editor: syntax highlighting, a line-number gutter, inline error underlines, and a
 * caret-anchored as-you-type completion popup. It is the Text-mode counterpart of the single-line
 * SCrowdyExpressionEditor: both share the pure CrowdyExpressionEditorModel, but this one drives the whole-body
 * highlighter (multi-statement diagnostics mapped across lines) and grows with its content rather than being one line.
 *
 * The completion popup shows the ranked suggestions the completion source produces for the live caret, with Up/Down
 * to move, Enter/Tab to accept, Escape to dismiss, and Ctrl+Space to force it open. It opens without stealing focus,
 * so typing continues while it is shown. The gutter and the status line under the editor recompute on every edit; the
 * editor writes the committed body straight through the OnTextCommitted delegate (undo/redo via the real handle) and
 * fires OnTextChanged live so an embedding Details panel can refresh a compile preview as the author types.
 */
class SCrowdyEffectScriptEditor : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyEffectScriptEditor)
		: _HintText(FText::GetEmpty())
	{}
		// The body text shown when the widget is first constructed.
		SLATE_ARGUMENT(FString, InitialText)

		// Resolves the attribute names for completion (optional; unbound means no attribute suggestions).
		SLATE_EVENT(FCrowdyExpressionAttributeProvider, AttributeProvider)

		// Resolves the attributes offered after `source.`, for an effect that declares a source container type of its
		// own. Consulted only when SourceSchemaDeclared is true, so leaving both at their defaults keeps source.<attr>
		// completing from the target's attributes, which is what an effect with no declared source type means. Both are
		// re-read per completion, so changing the effect's Source Container Type takes effect without reopening.
		SLATE_EVENT(FCrowdyExpressionAttributeProvider, SourceAttributeProvider)

		// Whether the effect declares a source container type at all. Kept separate from the list because a declared
		// type that resolves to no class must offer NOTHING rather than fall back to the target's attributes.
		SLATE_ATTRIBUTE(bool, SourceSchemaDeclared)

		// Produces the diagnostics only a real compile can find for a given body: anything needing the container's
		// attributes, which the parser knows nothing about. Optional; unbound leaves the editor reporting syntax
		// alone. Asked once per edit, for the body being shown, so an underline always describes the text under it.
		SLATE_EVENT(FCrowdyEffectBodyDiagnosticProvider, SemanticDiagnosticProvider)

		// The declared magnitude ($param) names, without the sigil.
		SLATE_ARGUMENT(TArray<FString>, Magnitudes)

		// The callable function names (builtins are always offered in addition to these).
		SLATE_ARGUMENT(TArray<FString>, Functions)

		// Placeholder text shown when the body is empty.
		SLATE_ATTRIBUTE(FText, HintText)

		// Fired on commit (focus loss) with the committed body.
		SLATE_EVENT(FCrowdyExpressionTextEvent, OnTextCommitted)

		// Fired on every text change with the in-progress body.
		SLATE_EVENT(FCrowdyExpressionTextEvent, OnTextChanged)
	SLATE_END_ARGS()

	virtual ~SCrowdyEffectScriptEditor() override;

	void Construct(const FArguments& InArgs);

	// The current in-progress body text.
	FString GetText() const { return CurrentText; }

	// Replace the body programmatically (does not fire the change/commit delegates).
	void SetText(const FString& NewText);

	virtual FReply OnPreviewKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;

private:
	// One offered completion, as a shared item the popup list view owns.
	struct FCompletionRow
	{
		FString Insert;
		FString Label;
	};

	FCrowdyExpressionVocabulary BuildVocabulary() const;

	void OnEditableTextChanged(const FText& NewText);
	void OnEditableTextCommitted(const FText& NewText, ETextCommit::Type CommitType);
	void RefreshDiagnostics();

	FText GetLineNumberText() const;
	FText GetStatusText() const;
	FSlateColor GetStatusColor() const;
	FSlateColor GetBorderColor() const;

	// The absolute caret offset in the multi-line body (the text box reports a line/column pair).
	int32 GetCaretOffset() const;

	// Recompute the completion set for the live caret and open or close the popup accordingly. bForce opens even with
	// no partial word (the Ctrl+Space path).
	void UpdateCompletions(bool bForce);
	void CloseCompletions();
	void AcceptSelectedCompletion();

	// Splice one completion's insertion into the body over the cached replace range and re-place the caret. Shared by
	// the keyboard-accept and mouse-click paths; the mouse path cannot read the selection, because clicking a row moves
	// focus off the text box, which commits and clears the item list before the click handler runs.
	void CommitCompletion(const FString& Insert);
	void MoveSelection(int32 Delta);

	// Text mutations (completion inserts) and menu open/close are deferred out of the text box's own input callbacks
	// (typing, key events) to the next tick. Mutating the editable text from inside its input callstack corrupts the
	// Windows text store, so a later TSF GetText reads past the changed string and crashes; RefreshDiagnostics stays
	// inline because it is read-only.
	void RequestCompletionRefresh(bool bForce);
	void RequestCommit(const FString& Insert);
	void EnsureDeferredTimer();
	EActiveTimerReturnType ProcessDeferredWork(double InCurrentTime, float InDeltaTime);

	TSharedRef<ITableRow> GenerateCompletionRow(
		TSharedPtr<FCompletionRow> Item, const TSharedRef<STableViewBase>& OwnerTable);
	void OnCompletionRowClicked(TSharedPtr<FCompletionRow> Item);

	TSharedPtr<SMultiLineEditableTextBox> TextBox;
	TSharedPtr<SBorder> EditorBorder;
	TSharedPtr<FCrowdyEffectScriptHighlightMarshaller> Highlighter;
	TSharedPtr<SMenuAnchor> CompletionAnchor;
	TSharedPtr<SListView<TSharedPtr<FCompletionRow>>> CompletionListView;

	FCrowdyExpressionAttributeProvider AttributeProvider;
	FCrowdyExpressionAttributeProvider SourceAttributeProvider;
	TAttribute<bool> SourceSchemaDeclared;
	FCrowdyEffectBodyDiagnosticProvider SemanticDiagnosticProvider;
	TArray<FString> Magnitudes;
	TArray<FString> Functions;

	FCrowdyExpressionTextEvent OnTextCommittedEvent;
	FCrowdyExpressionTextEvent OnTextChangedEvent;

	FString CurrentText;

	// Recomputed on every change: whether any error diagnostic exists, the message shown in the status line, and the
	// gutter's line-number column (cached so the bound gutter attribute is not rebuilt on every paint).
	bool bHasError = false;
	FText StatusMessage;
	FText CachedLineNumbers;

	// The live completion popup state.
	TArray<TSharedPtr<FCompletionRow>> CompletionItems;
	int32 SelectedCompletionIndex = INDEX_NONE;
	int32 CompletionReplaceStart = 0;
	int32 CompletionReplaceLen = 0;

	// Set when the user moves the popup selection with Up/Down; Enter accepts only after an explicit navigation, so a
	// plain Enter in the body inserts a newline instead of committing an auto-highlighted suggestion.
	bool bUserNavigatedCompletions = false;

	// Deferred-work flags (see RequestCompletionRefresh / RequestCommit). One active timer drains both.
	bool bDeferredTimerActive = false;
	bool bRefreshPending = false;
	bool bRefreshForce = false;
	bool bCommitPending = false;
	FString PendingCommitInsert;
};
