// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/GameModel/SCrowdyProvenanceLegend.h"

#include "GameModel/CrowdyModelVocabulary.h"
#include "Style/CrowdyStudioStyle.h"
#include "Styling/ISlateStyle.h"
#include "UI/GameModel/SCrowdyProvenanceGlyph.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CrowdyProvenanceLegend"

namespace
{
	// Bigger again than the gutter mark. Here the shape IS the subject: this is where a reader learns the difference
	// between the ring and the filled dot, so it is drawn large enough that the difference is obvious rather than
	// merely present.
	constexpr float CrowdyLegendGlyphExtent = 22.0f;

	// The column the glyphs sit in, wide enough that the shapes line up under each other and the words after them
	// start at one x. Derived from the glyph so it cannot cap the shape it is supposed to hold.
	constexpr float CrowdyLegendGlyphColumn = CrowdyLegendGlyphExtent + 10.0f;

	// How much room the word gets before its meaning. "code-not-pushed" is the longest of them and sets this.
	constexpr float CrowdyLegendWordColumn = 104.0f;
}

TSharedRef<SWidget> SCrowdyProvenanceLegend::MakeHeading(const FText& Text)
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	return SNew(STextBlock)
		.Text(Text)
		.TextStyle(&Style, "Crowdy.Text.Subtle")
		.Font(FCoreStyle::GetDefaultFontStyle("Bold", 8));
}

TSharedRef<SWidget> SCrowdyProvenanceLegend::MakeProvenanceRow(ECrowdyModelProvenance Provenance)
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	// Unknown paints no shape, so its own row would be a blank cell followed by a sentence about a blank cell. The
	// dash stands in for the absence and keeps the column from reading as a widget that failed to draw.
	const bool bHasShape = Provenance != ECrowdyModelProvenance::Unknown;
	const FString Word = CrowdyModelVocabulary::ProvenanceLabel(Provenance);

	return SNew(SHorizontalBox)

		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SBox)
			.WidthOverride(CrowdyLegendGlyphColumn)
			.HAlign(HAlign_Center)
			[
				bHasShape
					? StaticCastSharedRef<SWidget>(SNew(SCrowdyProvenanceGlyph)
						.Provenance(Provenance)
						.Extent(CrowdyLegendGlyphExtent))
					: StaticCastSharedRef<SWidget>(SNew(STextBlock)
						.Text(LOCTEXT("LegendNoMark", "-"))
						.TextStyle(&Style, "Crowdy.Text.Subtle"))
			]
		]

		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SBox)
			.WidthOverride(CrowdyLegendWordColumn)
			[
				SNew(STextBlock)
				.Text(Word.IsEmpty()
					? LOCTEXT("LegendNoWord", "no mark")
					: FText::FromString(Word))
				.TextStyle(&Style, "Crowdy.Text.Subtle")
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
			]
		]

		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(FText::FromString(CrowdyModelVocabulary::ProvenanceMeaning(Provenance)))
			.TextStyle(&Style, "Crowdy.Text.Subtle")
			.AutoWrapText(true)
		];
}

TSharedRef<SWidget> SCrowdyProvenanceLegend::MakeDriftRow(ECrowdyModelDrift Drift)
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();
	const FString Word = CrowdyModelVocabulary::DriftLabel(Drift);

	return SNew(SHorizontalBox)

		// The empty gutter keeps the Status rows in the same two columns as the Source rows above them, so the two
		// halves read as one table rather than two lists that happen to be stacked.
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SBox).WidthOverride(CrowdyLegendGlyphColumn)
		]

		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SBox)
			.WidthOverride(CrowdyLegendWordColumn)
			[
				SNew(STextBlock)
				.Text(Word.IsEmpty()
					? LOCTEXT("LegendBlankStatus", "(blank)")
					: FText::FromString(Word))
				.TextStyle(&Style, "Crowdy.Text.Subtle")
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
				.AutoWrapText(true)
			]
		]

		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(FText::FromString(CrowdyModelVocabulary::DriftMeaning(Drift)))
			.TextStyle(&Style, "Crowdy.Text.Subtle")
			.AutoWrapText(true)
		];
}

void SCrowdyProvenanceLegend::Construct(const FArguments& InArgs)
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);

	Rows->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 3.0f)
	[
		MakeHeading(LOCTEXT("LegendSourceHeading", "SOURCE"))
	];

	for (const ECrowdyModelProvenance Provenance : CrowdyModelVocabulary::AllProvenances())
	{
		Rows->AddSlot().AutoHeight().Padding(0.0f, 2.0f)
		[
			MakeProvenanceRow(Provenance)
		];
	}

	Rows->AddSlot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 3.0f)
	[
		MakeHeading(LOCTEXT("LegendStatusHeading", "STATUS"))
	];

	for (const ECrowdyModelDrift Drift : CrowdyModelVocabulary::AllDrifts())
	{
		Rows->AddSlot().AutoHeight().Padding(0.0f, 2.0f)
		[
			MakeDriftRow(Drift)
		];
	}

	// Scrolls rather than names a height, so whatever shows this decides how much room it gets. A panel that sizes
	// itself is how the card it used to live in ended up taller than the space it had and clipped its last row.
	ChildSlot
	[
		SNew(SScrollBox)
		+ SScrollBox::Slot()
		[
			Rows
		]
	];
}

#undef LOCTEXT_NAMESPACE
