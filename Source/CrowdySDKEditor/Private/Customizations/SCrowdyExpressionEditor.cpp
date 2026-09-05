// Fill out your copyright notice in the Description page of Project Settings.

#include "Customizations/SCrowdyExpressionEditor.h"

#include "Customizations/CrowdyEffectScriptHighlightMarshaller.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/Text/TextLayout.h"
#include "Styling/AppStyle.h"
#include "Textures/SlateIcon.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SCrowdyExpressionEditor"

void SCrowdyExpressionEditor::Construct(const FArguments& InArgs)
{
	AttributeProvider = InArgs._AttributeProvider;
	Magnitudes = InArgs._Magnitudes;
	Functions = InArgs._Functions;
	OnExpressionCommittedEvent = InArgs._OnExpressionCommitted;
	OnExpressionChangedEvent = InArgs._OnExpressionChanged;
	CurrentText = InArgs._InitialExpression;

	Highlighter = FCrowdyEffectScriptHighlightMarshaller::Create();

	ChildSlot
	[
		SAssignNew(ErrorBorder, SBorder)
		.BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
		.BorderBackgroundColor(this, &SCrowdyExpressionEditor::GetBorderColor)
		.Padding(1.f)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.FillWidth(1.f)
			[
				SAssignNew(TextBox, SMultiLineEditableTextBox)
				.Text(FText::FromString(CurrentText))
				.Marshaller(Highlighter)
				.HintText(InArgs._HintText)
				.ToolTipText(this, &SCrowdyExpressionEditor::GetDiagnosticTooltip)
				.AllowMultiLine(false)
				.OnTextChanged(this, &SCrowdyExpressionEditor::OnTextChanged)
				.OnTextCommitted(this, &SCrowdyExpressionEditor::OnTextCommitted)
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(2.f, 0.f, 0.f, 0.f)
			[
				SNew(SComboButton)
				.ToolTipText(LOCTEXT("InsertTip", "Insert an attribute, magnitude, function, or builtin."))
				.OnGetMenuContent(this, &SCrowdyExpressionEditor::BuildCompletionMenu)
				.ButtonContent()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("InsertLabel", "Insert"))
				]
			]
		]
	];

	RefreshDiagnostics();
}

void SCrowdyExpressionEditor::SetExpression(const FString& NewExpression)
{
	CurrentText = NewExpression;
	if (TextBox.IsValid())
	{
		TextBox->SetText(FText::FromString(CurrentText));
	}
	RefreshDiagnostics();
}

FCrowdyExpressionVocabulary SCrowdyExpressionEditor::BuildVocabulary() const
{
	FCrowdyExpressionVocabulary Vocabulary;
	if (AttributeProvider.IsBound())
	{
		Vocabulary.Attributes = AttributeProvider.Execute();
	}
	Vocabulary.Magnitudes = Magnitudes;
	Vocabulary.Functions = Functions;
	return Vocabulary;
}

void SCrowdyExpressionEditor::OnTextChanged(const FText& NewText)
{
	CurrentText = NewText.ToString();
	RefreshDiagnostics();
	OnExpressionChangedEvent.ExecuteIfBound(CurrentText);
}

void SCrowdyExpressionEditor::OnTextCommitted(const FText& NewText, ETextCommit::Type CommitType)
{
	CurrentText = NewText.ToString();
	RefreshDiagnostics();
	OnExpressionCommittedEvent.ExecuteIfBound(CurrentText);
}

void SCrowdyExpressionEditor::RefreshDiagnostics()
{
	const TArray<FCrowdyExpressionDiagnosticRange> Ranges = CrowdyExpressionEditorModel::DiagnoseRanges(CurrentText);

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

	DiagnosticTooltip = bHasError
		? FText::FromString(FirstError)
		: LOCTEXT("NoError", "Enter an EffectScript expression.");
}

TSharedRef<SWidget> SCrowdyExpressionEditor::BuildCompletionMenu()
{
	const FCrowdyExpressionCompletionSource Source(BuildVocabulary());

	// Anchor completion at the live caret, falling back to end-of-text when the cursor location is unavailable (the
	// expression is a single logical line, so the cursor offset is its column).
	int32 Caret = CurrentText.Len();
	if (TextBox.IsValid())
	{
		const FTextLocation CursorLocation = TextBox->GetCursorLocation();
		if (CursorLocation.GetLineIndex() >= 0)
		{
			Caret = FMath::Clamp(CursorLocation.GetOffset(), 0, CurrentText.Len());
		}
	}

	int32 ReplaceStart = 0;
	int32 ReplaceLen = 0;
	const TArray<FCrowdyExpressionCompletion> Completions =
		Source.GetCompletions(CurrentText, Caret, ReplaceStart, ReplaceLen);

	FMenuBuilder MenuBuilder(true, nullptr);
	if (Completions.Num() == 0)
	{
		MenuBuilder.AddWidget(
			SNew(STextBlock).Text(LOCTEXT("NoCompletions", "No suggestions")),
			FText::GetEmpty());
		return MenuBuilder.MakeWidget();
	}

	// Cap the menu so a large vocabulary cannot build an unbounded list.
	constexpr int32 MaxEntries = 40;
	const int32 Count = FMath::Min(Completions.Num(), MaxEntries);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FString Insert = Completions[Index].Insert;
		MenuBuilder.AddMenuEntry(
			FText::FromString(Completions[Index].Label),
			FText::GetEmpty(),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(
				this, &SCrowdyExpressionEditor::ApplyCompletionInsert, ReplaceStart, ReplaceLen, Insert)));
	}
	return MenuBuilder.MakeWidget();
}

void SCrowdyExpressionEditor::ApplyCompletionInsert(int32 ReplaceStart, int32 ReplaceLen, FString Insert)
{
	int32 NewCaret = 0;
	CurrentText = FCrowdyExpressionCompletionSource::ApplyCompletion(CurrentText, ReplaceStart, ReplaceLen, Insert, NewCaret);
	if (TextBox.IsValid())
	{
		TextBox->SetText(FText::FromString(CurrentText));
		// Place the caret just past the inserted text so the author can keep typing where they left off.
		TextBox->GoTo(FTextLocation(0, NewCaret));
	}
	RefreshDiagnostics();
	OnExpressionChangedEvent.ExecuteIfBound(CurrentText);
}

FSlateColor SCrowdyExpressionEditor::GetBorderColor() const
{
	return bHasError ? FSlateColor(FLinearColor::Red) : FSlateColor(FLinearColor::White);
}

FText SCrowdyExpressionEditor::GetDiagnosticTooltip() const
{
	return DiagnosticTooltip;
}

#undef LOCTEXT_NAMESPACE
