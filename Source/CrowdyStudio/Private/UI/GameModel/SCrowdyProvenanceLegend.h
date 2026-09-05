// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameModel/CrowdyModelLedger.h" // ECrowdyModelProvenance, ECrowdyModelDrift
#include "Widgets/SCompoundWidget.h"

class STextBlock;
class SWidget;

// What the Source marks and the Status words mean, for a reader meeting them for the first time. This is the panel
// body only: whatever shows it owns opening and closing it, so the same key can be put behind a button, in a
// tooltip or beside a table without carrying a disclosure it does not need.
//
// Every row is built by walking the vocabulary's own lists, so a provenance or drift state added there appears here
// without anyone remembering to come back. A legend typed out beside the widget is a second copy of the vocabulary
// and would start lying the first time one of them changed.
class SCrowdyProvenanceLegend : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyProvenanceLegend) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	// The glyph, its word and what the word means, as one line of the Source half.
	static TSharedRef<SWidget> MakeProvenanceRow(ECrowdyModelProvenance Provenance);

	// The Status half has no glyph, so its rows put the word where the glyph would be and stay in the same columns.
	static TSharedRef<SWidget> MakeDriftRow(ECrowdyModelDrift Drift);

	static TSharedRef<SWidget> MakeHeading(const FText& Text);
};
