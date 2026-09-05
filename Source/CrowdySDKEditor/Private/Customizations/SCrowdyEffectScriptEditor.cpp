// Fill out your copyright notice in the Description page of Project Settings.

#include "Customizations/SCrowdyEffectScriptEditor.h"

#include "Customizations/CrowdyEffectScriptHighlightMarshaller.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Text/TextLayout.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SMenuAnchor.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "SCrowdyEffectScriptEditor"

namespace
{
	// The editable text hands back LINE_TERMINATOR ("\r\n" on Windows) between lines; the parser, the highlighter, and
	// the stored EffectScript all use '\n', so normalise as the text leaves the box. Line/column caret math and the
	// gutter therefore all operate on a '\n'-only string.
	FString NormalizeNewlines(FString Text)
	{
		Text.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
		Text.ReplaceInline(TEXT("\r"), TEXT("\n"));
		return Text;
	}

	// Convert the text box's (line, column) caret into an absolute offset into the whole body, so the completion
	// source (which works on a flat string + offset) sees the caret the same way the parser does.
	int32 CaretToAbsolute(const FString& Text, const FTextLocation& Location)
	{
		const int32 Line = Location.GetLineIndex();
		const int32 Column = Location.GetOffset();
		if (Line <= 0)
		{
			return FMath::Clamp(Column, 0, Text.Len());
		}

		int32 Absolute = 0;
		int32 CurrentLine = 0;
		while (Absolute < Text.Len() && CurrentLine < Line)
		{
			if (Text[Absolute] == TEXT('\n'))
			{
				++CurrentLine;
			}
			++Absolute;
		}
		return FMath::Clamp(Absolute + Column, 0, Text.Len());
	}

	// The inverse: an absolute offset back to a (line, column) location the text box can place its caret at.
	FTextLocation AbsoluteToCaret(const FString& Text, int32 Absolute)
	{
		Absolute = FMath::Clamp(Absolute, 0, Text.Len());
		int32 Line = 0;
		int32 LineStart = 0;
		for (int32 Index = 0; Index < Absolute; ++Index)
		{
			if (Text[Index] == TEXT('\n'))
			{
				++Line;
				LineStart = Index + 1;
			}
		}
		return FTextLocation(Line, Absolute - LineStart);
	}
}

SCrowdyEffectScriptEditor::~SCrowdyEffectScriptEditor()
{
	// If the widget is torn down while the popup is still open (a panel rebuild that never routed a focus-loss commit),
	// dismiss the menu so it does not linger. Guard on the app so editor shutdown, when Slate is gone, is a no-op.
	if (FSlateApplication::IsInitialized() && CompletionAnchor.IsValid() && CompletionAnchor->IsOpen())
	{
		CompletionAnchor->SetIsOpen(false, /*bFocusMenu*/ false);
	}
}

void SCrowdyEffectScriptEditor::Construct(const FArguments& InArgs)
{
	AttributeProvider = InArgs._AttributeProvider;
	SourceAttributeProvider = InArgs._SourceAttributeProvider;
	SourceSchemaDeclared = InArgs._SourceSchemaDeclared;
	SemanticDiagnosticProvider = InArgs._SemanticDiagnosticProvider;
	Magnitudes = InArgs._Magnitudes;
	Functions = InArgs._Functions;
	OnTextCommittedEvent = InArgs._OnTextCommitted;
	OnTextChangedEvent = InArgs._OnTextChanged;
	CurrentText = InArgs._InitialText;

	Highlighter = FCrowdyEffectScriptHighlightMarshaller::CreateForBody();
	Highlighter->SetDiagnosticProvider(SemanticDiagnosticProvider);

	const FSlateFontInfo GutterFont = FCoreStyle::GetDefaultFontStyle("Mono", 10);

	// The completion popup: a bounded, single-selection list rebuilt on each keystroke and shown by the menu anchor
	// without stealing focus, so the author keeps typing while it is open.
	const TSharedRef<SWidget> CompletionMenu =
		SNew(SBorder)
		.BorderImage(FAppStyle::Get().GetBrush("Menu.Background"))
		.Padding(1.f)
		[
			SNew(SBox)
			.MinDesiredWidth(220.f)
			.MaxDesiredHeight(200.f)
			[
				SAssignNew(CompletionListView, SListView<TSharedPtr<FCompletionRow>>)
				.ListItemsSource(&CompletionItems)
				.SelectionMode(ESelectionMode::Single)
				.OnGenerateRow(this, &SCrowdyEffectScriptEditor::GenerateCompletionRow)
				.OnMouseButtonClick(this, &SCrowdyEffectScriptEditor::OnCompletionRowClicked)
			]
		];

	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SAssignNew(CompletionAnchor, SMenuAnchor)
			.Placement(MenuPlacement_BelowAnchor)
			.MenuContent(CompletionMenu)
			[
				SAssignNew(EditorBorder, SBorder)
				.BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
				.BorderBackgroundColor(this, &SCrowdyEffectScriptEditor::GetBorderColor)
				.Padding(1.f)
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(2.f, 2.f, 4.f, 2.f)
					[
						SNew(STextBlock)
						.Font(GutterFont)
						.ColorAndOpacity(FSlateColor(FLinearColor(0.45f, 0.45f, 0.48f)))
						.Justification(ETextJustify::Right)
						.Text(this, &SCrowdyEffectScriptEditor::GetLineNumberText)
					]

					+ SHorizontalBox::Slot()
					.FillWidth(1.f)
					[
						SNew(SBox)
						.MinDesiredHeight(96.f)
						[
							SAssignNew(TextBox, SMultiLineEditableTextBox)
							.Text(FText::FromString(CurrentText))
							.Marshaller(Highlighter)
							.HintText(InArgs._HintText)
							.AllowMultiLine(true)
							.AutoWrapText(false)
							.OnTextChanged(this, &SCrowdyEffectScriptEditor::OnEditableTextChanged)
							.OnTextCommitted(this, &SCrowdyEffectScriptEditor::OnEditableTextCommitted)
						]
					]
				]
			]
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(2.f, 2.f, 0.f, 0.f)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(this, &SCrowdyEffectScriptEditor::GetStatusColor)
			.Text(this, &SCrowdyEffectScriptEditor::GetStatusText)
		]
	];

	RefreshDiagnostics();
}

void SCrowdyEffectScriptEditor::SetText(const FString& NewText)
{
	CurrentText = NewText;
	if (TextBox.IsValid())
	{
		TextBox->SetText(FText::FromString(CurrentText));
	}
	CloseCompletions();
	RefreshDiagnostics();
}

FCrowdyExpressionVocabulary SCrowdyEffectScriptEditor::BuildVocabulary() const
{
	FCrowdyExpressionVocabulary Vocabulary;
	if (AttributeProvider.IsBound())
	{
		Vocabulary.Attributes = AttributeProvider.Execute();
	}
	// Read every completion, not cached, so declaring or changing a source container type is reflected in the next
	// suggestion rather than at the next asset open.
	Vocabulary.bSourceSchemaDeclared = SourceSchemaDeclared.Get(false);
	if (Vocabulary.bSourceSchemaDeclared && SourceAttributeProvider.IsBound())
	{
		Vocabulary.SourceAttributes = SourceAttributeProvider.Execute();
	}
	Vocabulary.Magnitudes = Magnitudes;
	Vocabulary.Functions = Functions;
	return Vocabulary;
}

void SCrowdyEffectScriptEditor::OnEditableTextChanged(const FText& NewText)
{
	CurrentText = NormalizeNewlines(NewText.ToString());
	RefreshDiagnostics();
	OnTextChangedEvent.ExecuteIfBound(CurrentText);
	// Recompute + open/close the popup next tick, off this input callback (opening a menu window from inside the text
	// box's change notification is part of what desyncs the Windows text store).
	RequestCompletionRefresh(/*bForce*/ false);
}

void SCrowdyEffectScriptEditor::OnEditableTextCommitted(const FText& NewText, ETextCommit::Type CommitType)
{
	CurrentText = NormalizeNewlines(NewText.ToString());
	CloseCompletions();
	RefreshDiagnostics();
	OnTextCommittedEvent.ExecuteIfBound(CurrentText);
}

void SCrowdyEffectScriptEditor::RefreshDiagnostics()
{
	TArray<FCrowdyExpressionDiagnosticRange> Ranges =
		CrowdyExpressionEditorModel::DiagnoseBodyRanges(CurrentText);

	// Asked for this exact body, the same way the highlighter asks, so the status line and the underlines can never
	// describe different text.
	if (SemanticDiagnosticProvider.IsBound())
	{
		Ranges.Append(CrowdyExpressionEditorModel::MapBodyDiagnosticsToRanges(
			CurrentText, SemanticDiagnosticProvider.Execute(CurrentText)));
	}

	bHasError = false;
	FString FirstError;
	for (const FCrowdyExpressionDiagnosticRange& Range : Ranges)
	{
		if (Range.bIsError)
		{
			bHasError = true;
			FirstError = Range.Message;
			break;
		}
	}

	if (bHasError)
	{
		StatusMessage = FText::FromString(FirstError);
	}
	else if (SemanticDiagnosticProvider.IsBound())
	{
		StatusMessage = LOCTEXT("NoErrors", "No errors.");
	}
	else
	{
		// Without a provider only the parser has run, so saying "No errors." would claim the attribute checks
		// passed when they never happened, which is how a body that does not compile reads as clean.
		StatusMessage = LOCTEXT("NoSyntaxErrors", "No syntax errors. Compile for the full check.");
	}

	// Rebuild the gutter column here (on change) rather than in the bound attribute (on every paint).
	const int32 Lines = CrowdyExpressionEditorModel::CountLines(CurrentText);
	TArray<FString> Numbers;
	Numbers.Reserve(Lines);
	for (int32 Line = 1; Line <= Lines; ++Line)
	{
		Numbers.Add(FString::FromInt(Line));
	}
	CachedLineNumbers = FText::FromString(FString::Join(Numbers, TEXT("\n")));
}

FText SCrowdyEffectScriptEditor::GetLineNumberText() const
{
	return CachedLineNumbers;
}

FText SCrowdyEffectScriptEditor::GetStatusText() const
{
	return StatusMessage;
}

FSlateColor SCrowdyEffectScriptEditor::GetStatusColor() const
{
	return bHasError ? FSlateColor(FLinearColor(0.88f, 0.32f, 0.38f)) : FSlateColor(FLinearColor(0.45f, 0.62f, 0.42f));
}

FSlateColor SCrowdyEffectScriptEditor::GetBorderColor() const
{
	return bHasError ? FSlateColor(FLinearColor::Red) : FSlateColor(FLinearColor::White);
}

int32 SCrowdyEffectScriptEditor::GetCaretOffset() const
{
	if (!TextBox.IsValid())
	{
		return CurrentText.Len();
	}
	return CaretToAbsolute(CurrentText, TextBox->GetCursorLocation());
}

void SCrowdyEffectScriptEditor::UpdateCompletions(bool bForce)
{
	if (!CompletionAnchor.IsValid())
	{
		return;
	}

	const int32 Caret = GetCaretOffset();
	const FCrowdyExpressionCompletionSource Source(BuildVocabulary());

	int32 ReplaceStart = 0;
	int32 ReplaceLen = 0;
	const TArray<FCrowdyExpressionCompletion> Completions =
		Source.GetCompletions(CurrentText, Caret, ReplaceStart, ReplaceLen);

	// Show as the author types an identifier, right after a '.' or '$' trigger, or when Ctrl+Space forces it. Anything
	// else (empty candidate set, a caret in whitespace with no partial word) closes the popup so it never nags.
	const bool bTriggerChar = Caret > 0
		&& (CurrentText[Caret - 1] == TEXT('.') || CurrentText[Caret - 1] == TEXT('$'));
	const bool bShow = Completions.Num() > 0 && (bForce || ReplaceLen > 0 || bTriggerChar);
	if (!bShow)
	{
		CloseCompletions();
		return;
	}

	CompletionReplaceStart = ReplaceStart;
	CompletionReplaceLen = ReplaceLen;

	constexpr int32 MaxEntries = 40;
	const int32 Count = FMath::Min(Completions.Num(), MaxEntries);
	CompletionItems.Reset(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		CompletionItems.Add(MakeShared<FCompletionRow>(FCompletionRow{ Completions[Index].Insert, Completions[Index].Label }));
	}
	SelectedCompletionIndex = 0;
	// A freshly (re)built list starts unnavigated, so a plain Enter falls through to a newline until the author moves
	// the selection with Up/Down.
	bUserNavigatedCompletions = false;

	if (CompletionListView.IsValid())
	{
		CompletionListView->RequestListRefresh();
		CompletionListView->SetSelection(CompletionItems[0]);
	}
	CompletionAnchor->SetIsOpen(true, /*bFocusMenu*/ false);
}

void SCrowdyEffectScriptEditor::CloseCompletions()
{
	CompletionItems.Reset();
	SelectedCompletionIndex = INDEX_NONE;
	if (CompletionAnchor.IsValid() && CompletionAnchor->IsOpen())
	{
		CompletionAnchor->SetIsOpen(false, /*bFocusMenu*/ false);
	}
}

void SCrowdyEffectScriptEditor::AcceptSelectedCompletion()
{
	if (!CompletionItems.IsValidIndex(SelectedCompletionIndex))
	{
		CloseCompletions();
		return;
	}
	RequestCommit(CompletionItems[SelectedCompletionIndex]->Insert);
}

void SCrowdyEffectScriptEditor::RequestCompletionRefresh(bool bForce)
{
	bRefreshPending = true;
	bRefreshForce = bRefreshForce || bForce;
	EnsureDeferredTimer();
}

void SCrowdyEffectScriptEditor::RequestCommit(const FString& Insert)
{
	PendingCommitInsert = Insert;
	bCommitPending = true;
	EnsureDeferredTimer();
}

void SCrowdyEffectScriptEditor::EnsureDeferredTimer()
{
	if (!bDeferredTimerActive)
	{
		bDeferredTimerActive = true;
		RegisterActiveTimer(0.f,
			FWidgetActiveTimerDelegate::CreateSP(this, &SCrowdyEffectScriptEditor::ProcessDeferredWork));
	}
}

EActiveTimerReturnType SCrowdyEffectScriptEditor::ProcessDeferredWork(double, float)
{
	bDeferredTimerActive = false;

	// A pending commit supersedes a pending refresh: the insert replaces the partial word and closes the popup, so
	// reopening it would be wrong.
	if (bCommitPending)
	{
		bCommitPending = false;
		bRefreshPending = false;
		bRefreshForce = false;
		const FString Insert = PendingCommitInsert;
		PendingCommitInsert.Reset();
		CommitCompletion(Insert);
	}
	else if (bRefreshPending)
	{
		bRefreshPending = false;
		const bool bForce = bRefreshForce;
		bRefreshForce = false;
		UpdateCompletions(bForce);
	}

	return EActiveTimerReturnType::Stop;
}

void SCrowdyEffectScriptEditor::CommitCompletion(const FString& Insert)
{
	// The replace range was anchored to the partial word at UpdateCompletions time; the body text has not changed
	// since (every edit re-runs UpdateCompletions), so splicing there is still correct even on the mouse path where a
	// focus-loss commit already cleared the item list.
	int32 NewCaret = 0;
	CurrentText = FCrowdyExpressionCompletionSource::ApplyCompletion(
		CurrentText, CompletionReplaceStart, CompletionReplaceLen, Insert, NewCaret);

	CloseCompletions();

	if (TextBox.IsValid())
	{
		TextBox->SetText(FText::FromString(CurrentText));
		TextBox->GoTo(AbsoluteToCaret(CurrentText, NewCaret));
		// Clicking a row moved focus off the text box; return it so the author keeps typing where they left off.
		FSlateApplication::Get().SetKeyboardFocus(TextBox);
	}
	RefreshDiagnostics();
	OnTextChangedEvent.ExecuteIfBound(CurrentText);
}

void SCrowdyEffectScriptEditor::MoveSelection(int32 Delta)
{
	if (CompletionItems.Num() == 0)
	{
		return;
	}
	SelectedCompletionIndex = FMath::Clamp(SelectedCompletionIndex + Delta, 0, CompletionItems.Num() - 1);
	bUserNavigatedCompletions = true;
	if (CompletionListView.IsValid())
	{
		CompletionListView->SetSelection(CompletionItems[SelectedCompletionIndex]);
		CompletionListView->RequestScrollIntoView(CompletionItems[SelectedCompletionIndex]);
	}
}

FReply SCrowdyEffectScriptEditor::OnPreviewKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();

	// Ctrl+Space opens the popup at the caret, even mid-word or in an empty position (deferred off this key event).
	if (Key == EKeys::SpaceBar && InKeyEvent.IsControlDown())
	{
		RequestCompletionRefresh(/*bForce*/ true);
		return FReply::Handled();
	}

	// While the popup is open it owns the navigation / accept / dismiss keys.
	if (CompletionAnchor.IsValid() && CompletionAnchor->IsOpen())
	{
		if (Key == EKeys::Down)
		{
			MoveSelection(1);
			return FReply::Handled();
		}
		if (Key == EKeys::Up)
		{
			MoveSelection(-1);
			return FReply::Handled();
		}
		if (Key == EKeys::Tab)
		{
			AcceptSelectedCompletion();
			return FReply::Handled();
		}
		if (Key == EKeys::Enter)
		{
			// Enter accepts only after the author explicitly moved the selection; otherwise it falls through to insert a
			// newline (so pressing Enter to end a line never commits an auto-highlighted suggestion by surprise).
			if (bUserNavigatedCompletions)
			{
				AcceptSelectedCompletion();
				return FReply::Handled();
			}
			CloseCompletions();
			return SCompoundWidget::OnPreviewKeyDown(MyGeometry, InKeyEvent);
		}
		if (Key == EKeys::Escape)
		{
			CloseCompletions();
			return FReply::Handled();
		}
	}

	return SCompoundWidget::OnPreviewKeyDown(MyGeometry, InKeyEvent);
}

TSharedRef<ITableRow> SCrowdyEffectScriptEditor::GenerateCompletionRow(
	TSharedPtr<FCompletionRow> Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(STableRow<TSharedPtr<FCompletionRow>>, OwnerTable)
	[
		SNew(STextBlock)
		.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
		.Text(FText::FromString(Item.IsValid() ? Item->Label : FString()))
	];
}

void SCrowdyEffectScriptEditor::OnCompletionRowClicked(TSharedPtr<FCompletionRow> Item)
{
	// Apply the clicked item directly (its Insert, not a list lookup): clicking a row moves focus off the text box,
	// whose focus-loss commit already ran CloseCompletions and emptied CompletionItems. Deferred so the text mutation
	// lands off any input callback.
	if (Item.IsValid())
	{
		RequestCommit(Item->Insert);
	}
}

#undef LOCTEXT_NAMESPACE
