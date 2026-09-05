// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Customizations/CrowdyExpressionEditorModel.h"
#include "Styling/SlateColor.h"
#include "Widgets/SCompoundWidget.h"

class FCrowdyEffectScriptHighlightMarshaller;
class SBorder;
class SMultiLineEditableTextBox;
class SWidget;

// Resolves the attribute names offered for completion. A provider lambda (rather than a fixed list) lets the same
// widget serve different containers: the embedding picker resolves attributes from the live container class each
// time the menu opens.
DECLARE_DELEGATE_RetVal(TArray<FString>, FCrowdyExpressionAttributeProvider);

// Fired with the full expression text on commit (enter / focus loss) and, separately, on every keystroke change.
DECLARE_DELEGATE_OneParam(FCrowdyExpressionTextEvent, const FString&);

/**
 * A single-line editor for one EffectScript expression, with live error feedback and context-aware completion.
 * Thin Slate glue over the pure CrowdyExpressionEditorModel: on every change it re-runs the parser to reflect an
 * error state (a red border plus a tooltip naming the first diagnostic), and a companion "Insert" menu offers the
 * ranked completions the completion source produces for the caret at the end of the text. Pill-style chip visuals
 * are deliberately left to a later pass; v1 is a styled text box.
 *
 * The public API is stable so the sentence-style effect picker can embed one editor per comparison side, formula
 * body, and step term: it injects the magnitude / function lists directly and the attributes via a provider so a
 * container-class change is reflected without rebuilding the widget.
 *
 * The text is edited in a multi-line box driven by a syntax-highlight marshaller (token colours plus error
 * underlines), but stays a single logical line: newline input is disabled, so Enter still commits the expression.
 */
class SCrowdyExpressionEditor : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyExpressionEditor)
		: _HintText(FText::GetEmpty())
	{}
		// The expression shown when the widget is first constructed.
		SLATE_ARGUMENT(FString, InitialExpression)

		// Resolves the attribute names for completion (optional; unbound means no attribute suggestions).
		SLATE_EVENT(FCrowdyExpressionAttributeProvider, AttributeProvider)

		// The declared magnitude ($param) names, without the sigil.
		SLATE_ARGUMENT(TArray<FString>, Magnitudes)

		// The callable function names (builtins are always offered in addition to these).
		SLATE_ARGUMENT(TArray<FString>, Functions)

		// Placeholder text shown when the expression is empty.
		SLATE_ATTRIBUTE(FText, HintText)

		// Fired on commit (enter / focus loss) with the committed expression.
		SLATE_EVENT(FCrowdyExpressionTextEvent, OnExpressionCommitted)

		// Fired on every text change with the in-progress expression.
		SLATE_EVENT(FCrowdyExpressionTextEvent, OnExpressionChanged)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	// The current in-progress expression text.
	FString GetExpression() const { return CurrentText; }

	// Replace the expression programmatically (does not fire the change/commit delegates).
	void SetExpression(const FString& NewExpression);

private:
	FCrowdyExpressionVocabulary BuildVocabulary() const;

	void OnTextChanged(const FText& NewText);
	void OnTextCommitted(const FText& NewText, ETextCommit::Type CommitType);
	void RefreshDiagnostics();

	TSharedRef<SWidget> BuildCompletionMenu();
	void ApplyCompletionInsert(int32 ReplaceStart, int32 ReplaceLen, FString Insert);

	FSlateColor GetBorderColor() const;
	FText GetDiagnosticTooltip() const;

	TSharedPtr<SMultiLineEditableTextBox> TextBox;
	TSharedPtr<SBorder> ErrorBorder;
	TSharedPtr<FCrowdyEffectScriptHighlightMarshaller> Highlighter;

	FCrowdyExpressionAttributeProvider AttributeProvider;
	TArray<FString> Magnitudes;
	TArray<FString> Functions;

	FCrowdyExpressionTextEvent OnExpressionCommittedEvent;
	FCrowdyExpressionTextEvent OnExpressionChangedEvent;

	FString CurrentText;

	// Recomputed on every change: whether any error diagnostic exists and the message shown in the tooltip.
	bool bHasError = false;
	FText DiagnosticTooltip;
};
