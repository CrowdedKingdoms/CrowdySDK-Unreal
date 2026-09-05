// Fill out your copyright notice in the Description page of Project Settings.

#include "Customizations/CrowdyEffectScriptHighlightMarshaller.h"

#include "Framework/Text/IRun.h"
#include "Framework/Text/SlateTextRun.h"
#include "Framework/Text/SlateTextUnderlineLineHighlighter.h"
#include "Framework/Text/TextLayout.h"
#include "Framework/Text/TextLineHighlight.h"
#include "Brushes/SlateColorBrush.h"
#include "Styling/CoreStyle.h"

namespace
{
	// Parse a 0xRRGGBB literal as an sRGB colour and convert to the linear space Slate renders in. Mirrors the brand
	// palette convention used by the Studio style so the two editor surfaces read consistently.
	FLinearColor HighlightHex(uint32 RGB)
	{
		const FColor C((RGB >> 16) & 0xFF, (RGB >> 8) & 0xFF, RGB & 0xFF, 255);
		return FLinearColor::FromSRGBColor(C);
	}

	// The syntax palette, tuned for the dark editor host and aligned to the Crowded Kingdoms brand (warm gold for
	// keywords, blue for reads, a soft violet for tuning values). Centralised here so every colour lives in one place.
	FLinearColor DefaultInk()   { return HighlightHex(0xE4E4E6); }
	FLinearColor KeywordInk()   { return HighlightHex(0xD6A928); }
	FLinearColor AttributeInk() { return HighlightHex(0x6E92E8); }
	FLinearColor MagnitudeInk() { return HighlightHex(0xC08CE0); }
	FLinearColor NumberInk()    { return HighlightHex(0xE0A15C); }
	FLinearColor StringInk()    { return HighlightHex(0x8FCA6B); }
	FLinearColor OperatorInk()  { return HighlightHex(0xB6B6BA); }
	FLinearColor PunctInk()     { return HighlightHex(0x8A8A90); }
	FLinearColor FunctionInk()  { return HighlightHex(0x5FC9C0); }
	FLinearColor ErrorInk()     { return HighlightHex(0xE05260); }
}

TSharedRef<FCrowdyEffectScriptHighlightMarshaller> FCrowdyEffectScriptHighlightMarshaller::Create()
{
	return MakeShareable(new FCrowdyEffectScriptHighlightMarshaller(/*bInBodyMode*/ false));
}

TSharedRef<FCrowdyEffectScriptHighlightMarshaller> FCrowdyEffectScriptHighlightMarshaller::CreateForBody()
{
	return MakeShareable(new FCrowdyEffectScriptHighlightMarshaller(/*bInBodyMode*/ true));
}

FCrowdyEffectScriptHighlightMarshaller::FCrowdyEffectScriptHighlightMarshaller(bool bInBodyMode)
	: bBodyMode(bInBodyMode)
{
	BaseStyle = FTextBlockStyle(FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText"))
		.SetFont(FCoreStyle::GetDefaultFontStyle("Mono", 10))
		.SetColorAndOpacity(FSlateColor(DefaultInk()));

	auto Register = [this](ECrowdyExpressionHighlightColor Color, const FLinearColor& Ink)
	{
		StylesByColor.Add(Color, FTextBlockStyle(BaseStyle).SetColorAndOpacity(FSlateColor(Ink)));
	};

	Register(ECrowdyExpressionHighlightColor::Default,      DefaultInk());
	Register(ECrowdyExpressionHighlightColor::Keyword,      KeywordInk());
	Register(ECrowdyExpressionHighlightColor::Attribute,    AttributeInk());
	Register(ECrowdyExpressionHighlightColor::Magnitude,    MagnitudeInk());
	Register(ECrowdyExpressionHighlightColor::Number,       NumberInk());
	Register(ECrowdyExpressionHighlightColor::String,       StringInk());
	Register(ECrowdyExpressionHighlightColor::Operator,     OperatorInk());
	Register(ECrowdyExpressionHighlightColor::Punctuation,  PunctInk());
	Register(ECrowdyExpressionHighlightColor::FunctionName, FunctionInk());

	ErrorColor = ErrorInk();
	// A solid white box the underline highlighter tints with ErrorColor; a one-pixel height gives a thin underline.
	ErrorUnderlineBrush = FSlateColorBrush(FLinearColor::White);
	ErrorUnderlineBrush.ImageSize = FVector2f(1.0f, 1.0f);
}

const FTextBlockStyle& FCrowdyEffectScriptHighlightMarshaller::StyleForColor(ECrowdyExpressionHighlightColor Color) const
{
	if (const FTextBlockStyle* Found = StylesByColor.Find(Color))
	{
		return *Found;
	}
	return BaseStyle;
}

void FCrowdyEffectScriptHighlightMarshaller::SetText(const FString& SourceString, FTextLayout& TargetTextLayout)
{
	// GetText below emits LINE_TERMINATOR ("\r\n" on Windows) between lines to match the text layout's own offset
	// accounting, so the string handed back here can carry '\r'. Normalise to '\n' before splitting and highlighting,
	// so line models never hold a stray '\r' and the highlight offsets line up with the parser (which is fed the same
	// '\n' body).
	FString Body = SourceString;
	Body.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
	Body.ReplaceInline(TEXT("\r"), TEXT("\n"));

	const TArray<FCrowdyExpressionHighlightRun> HighlightRuns = bBodyMode
		? CrowdyExpressionEditorModel::BuildBodyHighlightRuns(Body,
			DiagnosticProvider.IsBound() ? DiagnosticProvider.Execute(Body) : TArray<FCrowdyEffectDiagnostic>())
		: CrowdyExpressionEditorModel::BuildHighlightRuns(Body);

	TArray<FTextLayout::FNewLineData> LinesToAdd;
	TArray<FTextLineHighlight> LineHighlights;
	TSharedPtr<FSlateTextUnderlineLineHighlighter> ErrorUnderline;

	const int32 SourceLen = Body.Len();
	int32 LineIndex = 0;

	// A single-expression editor is one line; a body editor is many. Either way, split on '\n' and emit one layout
	// line per source line: each line's text is the absolute [LineStart, LineEnd) slice, excluding the separator,
	// and the global runs are clipped onto the line and rebased to line-local ranges.
	auto EmitLine = [&](int32 LineStart, int32 LineEnd)
	{
		TSharedRef<FString> ModelString = MakeShareable(new FString(Body.Mid(LineStart, LineEnd - LineStart)));
		TArray<TSharedRef<IRun>> Runs;

		for (const FCrowdyExpressionHighlightRun& HighlightRun : HighlightRuns)
		{
			const int32 Start = FMath::Max(HighlightRun.Start, LineStart);
			const int32 End = FMath::Min(HighlightRun.Start + HighlightRun.Len, LineEnd);
			if (End <= Start)
			{
				continue;
			}

			const FTextRange LocalRange(Start - LineStart, End - LineStart);
			Runs.Add(FSlateTextRun::Create(FRunInfo(), ModelString, StyleForColor(HighlightRun.Color), LocalRange));

			if (HighlightRun.bError)
			{
				if (!ErrorUnderline.IsValid())
				{
					ErrorUnderline = FSlateTextUnderlineLineHighlighter::Create(
						ErrorUnderlineBrush, BaseStyle.Font, FSlateColor(ErrorColor),
						BaseStyle.ShadowOffset, BaseStyle.ShadowColorAndOpacity);
				}
				LineHighlights.Add(FTextLineHighlight(
					LineIndex, LocalRange, FSlateTextUnderlineLineHighlighter::DefaultZIndex, ErrorUnderline.ToSharedRef()));
			}
		}

		if (Runs.Num() == 0)
		{
			// A line with no clipped runs (an empty line, or one entirely past the classified text) still needs a run
			// so the layout renders a valid, editable line.
			Runs.Add(FSlateTextRun::Create(FRunInfo(), ModelString, BaseStyle, FTextRange(0, ModelString->Len())));
		}

		LinesToAdd.Emplace(MoveTemp(ModelString), MoveTemp(Runs));
		++LineIndex;
	};

	int32 CurrentLineStart = 0;
	for (int32 Position = 0; Position <= SourceLen; ++Position)
	{
		if (Position == SourceLen || Body[Position] == TEXT('\n'))
		{
			EmitLine(CurrentLineStart, Position);
			CurrentLineStart = Position + 1;
		}
	}

	TargetTextLayout.AddLines(LinesToAdd);
	TargetTextLayout.SetLineHighlights(LineHighlights);
}

void FCrowdyEffectScriptHighlightMarshaller::GetText(FString& TargetString, const FTextLayout& SourceTextLayout)
{
	// Delegate to the layout so the reconstructed text uses LINE_TERMINATOR between lines exactly as the layout's own
	// length accounting does (FTextLayout::GetAsTextAndOffsets). Hand-joining with '\n' undercounts each line break by
	// one character on Windows ("\r\n"), so the IME text store would think the document is longer than this string and
	// read past its end (a crash in FTextStoreACP::GetText on the first multi-line edit).
	SourceTextLayout.GetAsText(TargetString);
}
