// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Customizations/CrowdyExpressionEditorModel.h"
#include "Framework/Text/BaseTextLayoutMarshaller.h"
#include "Styling/SlateBrush.h"
#include "Styling/SlateTypes.h"

class FTextLayout;

/**
 * Paints one EffectScript expression with syntax colours, driving a multi-line editable text box. It is a thin
 * Slate adapter over the pure CrowdyExpressionEditorModel::BuildHighlightRuns: on every edit it re-partitions the
 * text into coloured runs (keywords, attributes, magnitudes, numbers, strings, operators, calls) and underlines
 * any span the parser flagged as an error. All colour choices live here in one palette; the run boundaries and
 * error spans are decided by the model, so the highlighting matches exactly what the effect compiles to.
 */
class FCrowdyEffectScriptHighlightMarshaller : public FBaseTextLayoutMarshaller
{
public:
	// Single-expression mode: diagnostics come from the single-expression parser (used by the picker's per-operand
	// editor, which edits one line).
	static TSharedRef<FCrowdyEffectScriptHighlightMarshaller> Create();

	// Whole-body mode: diagnostics come from the full-body parser and are mapped across lines, so a multi-statement
	// EffectScript body underlines the right token on the right line (used by the Text-mode body editor).
	static TSharedRef<FCrowdyEffectScriptHighlightMarshaller> CreateForBody();

	virtual ~FCrowdyEffectScriptHighlightMarshaller() override = default;

	virtual void SetText(const FString& SourceString, FTextLayout& TargetTextLayout) override;
	virtual void GetText(FString& TargetString, const FTextLayout& SourceTextLayout) override;

	// Syntax highlighting mutates the layout from the source text, so the box must re-run SetText on every change.
	virtual bool RequiresLiveUpdate() const override { return true; }

	// Supplies the diagnostics the parser cannot produce, underlined alongside the ones it can. An unknown attribute
	// or one attribute spelled two ways needs the container's vocabulary, which this marshaller has no way to know.
	// It is a provider rather than a stored list because the answer belongs to one exact body: asked here, at the
	// moment a body is painted, an underline can never land on text the diagnostic was not computed from. Whole-body
	// mode only; ignored for a single expression.
	void SetDiagnosticProvider(FCrowdyEffectBodyDiagnosticProvider InProvider)
	{
		DiagnosticProvider = MoveTemp(InProvider);
	}

private:
	explicit FCrowdyEffectScriptHighlightMarshaller(bool bInBodyMode);

	// Whether diagnostics are produced by the whole-body parser (true) or the single-expression parser (false).
	bool bBodyMode = false;

	FCrowdyEffectBodyDiagnosticProvider DiagnosticProvider;

	const FTextBlockStyle& StyleForColor(ECrowdyExpressionHighlightColor Color) const;

	FTextBlockStyle BaseStyle;
	TMap<ECrowdyExpressionHighlightColor, FTextBlockStyle> StylesByColor;

	// A thin white brush tinted red by the error underline highlighter.
	FSlateBrush ErrorUnderlineBrush;
	FLinearColor ErrorColor;
};
