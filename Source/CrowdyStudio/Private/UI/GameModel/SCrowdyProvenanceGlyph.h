// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameModel/CrowdyModelLedger.h" // ECrowdyModelProvenance
#include "Widgets/SLeafWidget.h"

// A few pixels of ink in the gutter beside a row, saying where that row came from. Provenance is carried by the
// SHAPE and never by a colour: every shape is drawn in the same neutral ink, so nothing on this page is told apart
// by hue alone, and the shape is backed by a tooltip carrying the word so it never has to be decoded from memory.
//
//   code-synced      a filled dot     it is on the server and it matches the project
//   code-not-pushed  a half dot       the project has it and the server does not yet
//   code-drifted     a hollow ring    both have it and they differ
//   server-only      a small square   nothing in the project declares it
//   kit-owned        a short bar      a game kit deployed it and owns it
//   unknown          nothing at all   no check has run, or this entity could not be checked
//
// Drawn rather than loaded: five shapes at nine pixels are cheaper to paint than to ship, and adding an icon file
// per shape would put the vocabulary in two places.
class SCrowdyProvenanceGlyph : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyProvenanceGlyph)
		: _Provenance(ECrowdyModelProvenance::Unknown)
		, _Extent(0.0f)
	{}
		SLATE_ARGUMENT(ECrowdyModelProvenance, Provenance)
		// The side of the square to paint into, for a caller that is not a table row. Zero means the row size.
		// A legend is read rather than glanced at, so it asks for a larger one: at the row size these shapes are
		// tuned to sit quietly beside nine-point text, which is the opposite of what is wanted when the shape
		// itself is the thing being explained. Every shape scales together from the one set of proportions, so a
		// bigger glyph stays the same drawing rather than becoming a second set of numbers to keep in step.
		SLATE_ARGUMENT(float, Extent)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	// The side of the square this glyph occupies in a table row, in device-independent pixels. Public so a caller
	// sizing a gutter column budgets the same number this paints into.
	static float GlyphExtent();

	// How much the painted shape is multiplied by at a given extent. Public only so it can be tested: the shapes
	// are fractions of a fixed base unit, and if this ever returns one at the default extent then the glyph paints
	// at its original size inside whatever box it was given, which looks exactly like a size setting being ignored.
	static float ShapeScale(float InExtent);

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle,
		bool bParentEnabled) const override;

	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override;

private:
	ECrowdyModelProvenance Provenance = ECrowdyModelProvenance::Unknown;

	// The side of the square this instance paints into. Set from the argument, falling back to the row size.
	float Extent = 0.0f;
};
