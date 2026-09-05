// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/GameModel/SCrowdyProvenanceGlyph.h"

#include "GameModel/CrowdyModelVocabulary.h"
#include "Layout/Clipping.h"
#include "Rendering/DrawElements.h"
#include "Style/CrowdyStudioStyle.h"
#include "Styling/ISlateStyle.h"
#include "Styling/SlateBrush.h"
#include "Styling/WidgetStyle.h"

namespace
{
	// The box the glyph is laid out in. These shapes are told apart by silhouette, and a silhouette needs size: at
	// the original nine pixels the ring's hole closed up on a normal display and it painted as the filled dot,
	// collapsing two verdicts into one picture. Fifteen keeps the ring open while still reading as a mark beside a
	// row rather than as a control in its own right, which is what the size above it started to look like.
	// Anything sizing a column around it must ask GlyphExtent rather than repeat a number, or the column caps it.
	constexpr float CrowdyProvenanceGlyphExtent = 15.0f;

	// The size the shape dimensions below were drawn against. It is a unit, not a size: every literal in OnPaint is
	// a fraction of this, and the paint scales by Extent / this. It must therefore NEVER be changed to follow the
	// default above, or every shape silently keeps the proportions it had while its box grows around it.
	constexpr float CrowdyProvenanceGlyphBase = 9.0f;

	// A rect of the given size, centred in the space the glyph was given. The gutter column is wider than the glyph
	// so that the shapes sit in a straight line whatever the column ends up being.
	FPaintGeometry CrowdyProvenanceGlyphRect(const FGeometry& AllottedGeometry, float Width, float Height)
	{
		const FVector2f Available = FVector2f(AllottedGeometry.GetLocalSize());
		const FVector2f Offset(
			FMath::RoundToFloat((Available.X - Width) * 0.5f),
			FMath::RoundToFloat((Available.Y - Height) * 0.5f));
		return AllottedGeometry.ToPaintGeometry(FVector2f(Width, Height), FSlateLayoutTransform(Offset));
	}

	// The colour one of these shapes is filled with. A draw element does not keep the brush it was made from, so the
	// brush's own colour is dropped unless it is multiplied into the tint here: passing the widget's tint alone fills
	// every shape with opaque white, which turns the hollow ring into a solid disc and makes the gutter the brightest
	// ink on a page whose healthy state is meant to be silent. An outline colour is the one thing a rounded-box brush
	// does carry through on its own, so a ring keeps its edge whatever this returns.
	FLinearColor CrowdyProvenanceGlyphTint(const FSlateBrush* Brush, const FWidgetStyle& InWidgetStyle)
	{
		const FLinearColor WidgetTint = InWidgetStyle.GetColorAndOpacityTint();
		return Brush ? WidgetTint * Brush->GetTint(InWidgetStyle) : WidgetTint;
	}
}

void SCrowdyProvenanceGlyph::Construct(const FArguments& InArgs)
{
	Provenance = InArgs._Provenance;
	Extent = InArgs._Extent > 0.0f ? InArgs._Extent : CrowdyProvenanceGlyphExtent;

	// Nothing about a glyph changes once it is built, and a row's glyph is rebuilt with the row.
	SetCanTick(false);

	// The word the shape stands for. Without it the shape is a cipher the first time anyone meets it, and the page
	// deliberately spends no other ink on saying that a row is fine.
	const FString Word = CrowdyModelVocabulary::ProvenanceLabel(Provenance);
	if (!Word.IsEmpty())
	{
		SetToolTipText(FText::FromString(Word));
	}
}

float SCrowdyProvenanceGlyph::GlyphExtent()
{
	return CrowdyProvenanceGlyphExtent;
}

float SCrowdyProvenanceGlyph::ShapeScale(float InExtent)
{
	return (InExtent > 0.0f ? InExtent : CrowdyProvenanceGlyphExtent) / CrowdyProvenanceGlyphBase;
}

FVector2D SCrowdyProvenanceGlyph::ComputeDesiredSize(float) const
{
	// The same size whatever the shape, including the shape that paints nothing, so a column of glyphs stays a
	// column and rows do not shift as their verdicts change.
	return FVector2D(Extent, Extent);
}

int32 SCrowdyProvenanceGlyph::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
	const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId,
	const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	if (Provenance == ECrowdyModelProvenance::Unknown)
	{
		// Nothing is known about this row, so nothing is drawn. An "unknown" mark would be a claim of its own, and
		// on an app nobody has checked that mark would be on every row at once.
		return LayerId;
	}

	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	// Every shape below is a fraction of the base unit, multiplied up to whatever size this instance was given, so
	// one set of proportions serves the gutter and the legend alike. Scaling against the DEFAULT size instead of the
	// base is why raising that default did nothing for a table row: there Extent is the default, so the ratio came
	// out at exactly one and every shape kept the size it had while its box grew around it.
	const float S = ShapeScale(Extent);

	// Each brush carries the one neutral ink these shapes are drawn in, and the widget's own tint carries a disabled
	// or fading row's opacity, so every shape is drawn in both multiplied together.
	switch (Provenance)
	{
	case ECrowdyModelProvenance::CodeSynced:
	{
		const FSlateBrush* Brush = Style.GetBrush("Crowdy.Glyph.Dot");
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId,
			CrowdyProvenanceGlyphRect(AllottedGeometry, 8.0f * S, 8.0f * S),
			Brush, ESlateDrawEffect::None, CrowdyProvenanceGlyphTint(Brush, InWidgetStyle));
		break;
	}

	case ECrowdyModelProvenance::CodeNotPushed:
	{
		// Half a dot: the whole disc, drawn with its right half clipped away. Masking a full circle is what keeps the
		// curved edge a true half circle, which is what tells this apart from the filled dot at this size.
		const FVector2f Available = FVector2f(AllottedGeometry.GetLocalSize());
		const FVector2f Offset(
			FMath::RoundToFloat((Available.X - 8.0f * S) * 0.5f),
			FMath::RoundToFloat((Available.Y - 8.0f * S) * 0.5f));

		const FSlateBrush* Brush = Style.GetBrush("Crowdy.Glyph.Dot");
		OutDrawElements.PushClip(FSlateClippingZone(
			AllottedGeometry.ToPaintGeometry(FVector2f(4.0f * S, 8.0f * S), FSlateLayoutTransform(Offset))));
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId,
			AllottedGeometry.ToPaintGeometry(FVector2f(8.0f * S, 8.0f * S), FSlateLayoutTransform(Offset)),
			Brush, ESlateDrawEffect::None, CrowdyProvenanceGlyphTint(Brush, InWidgetStyle));
		OutDrawElements.PopClip();
		break;
	}

	case ECrowdyModelProvenance::CodeDrifted:
	{
		// The one shape whose fill is transparent: what is drawn is its outline, which a rounded-box brush carries
		// through by itself.
		const FSlateBrush* Brush = Style.GetBrush("Crowdy.Glyph.Ring");
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId,
			CrowdyProvenanceGlyphRect(AllottedGeometry, 9.0f * S, 9.0f * S),
			Brush, ESlateDrawEffect::None, CrowdyProvenanceGlyphTint(Brush, InWidgetStyle));
		break;
	}

	case ECrowdyModelProvenance::ServerOnly:
	{
		const FSlateBrush* Brush = Style.GetBrush("Crowdy.Glyph.Square");
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId,
			CrowdyProvenanceGlyphRect(AllottedGeometry, 7.0f * S, 7.0f * S),
			Brush, ESlateDrawEffect::None, CrowdyProvenanceGlyphTint(Brush, InWidgetStyle));
		break;
	}

	case ECrowdyModelProvenance::KitOwned:
	{
		// A short bar. It shares no silhouette with the dot, the ring or the square, so the five shapes stay
		// distinguishable from each other and not merely from nothing.
		const FSlateBrush* Brush = Style.GetBrush("Crowdy.Glyph.Square");
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId,
			CrowdyProvenanceGlyphRect(AllottedGeometry, 9.0f * S, 2.5f * S),
			Brush, ESlateDrawEffect::None, CrowdyProvenanceGlyphTint(Brush, InWidgetStyle));
		break;
	}

	default:
		break;
	}

	return LayerId;
}
