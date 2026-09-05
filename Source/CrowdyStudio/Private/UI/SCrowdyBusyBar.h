// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SLeafWidget.h"

class FActiveTimerHandle;

/**
 * A thin bar that says work is happening without claiming how much of it is done: a short pill sweeps back and forth
 * along a track, easing at each end.
 *
 * Why not SProgressBar with no Percent set. Its marquee draws a block the width of the bar PLUS the marquee image and
 * slides that, which only reads as motion when the brush is a repeating pattern (the engine's stock marquee is a
 * striped tile). Given a solid brush it paints a uniform block that looks identical at every offset, which is a
 * static filled bar. Supplying a striped tile would mean adding an image asset for four pixels of animation, so the
 * sweep is painted here instead.
 *
 * The animation runs from an active timer, so it costs nothing while the widget is collapsed or on a page that is not
 * being painted.
 */
class SCrowdyBusyBar : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyBusyBar)
		: _Thickness(4.0f)
		, _SecondsPerSweep(1.15f)
	{}
		// Height of the track. The pill is drawn the same height.
		SLATE_ARGUMENT(float, Thickness)
		// One end-to-end pass. Slow enough not to nag, quick enough to read as active.
		SLATE_ARGUMENT(float, SecondsPerSweep)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle,
		bool bParentEnabled) const override;

	virtual FVector2D ComputeDesiredSize(float) const override;

	// Where the pill sits along its there-and-back sweep at a given moment, 0 at the left end and 1 at the right.
	// Pure and static so the motion is exercised without a widget: continuity at the turn is the property that
	// matters, and a fold-back that is off by a factor jumps rather than returns, which no test of the paint could
	// tell apart from a hitch.
	static float PhaseAtTime(double InCurrentTime, float InSecondsPerSweep);

	// Smoothstep, so the pill slows into each end instead of reversing hard.
	static float Ease(float InPhase);

	// Where the pill's left edge goes, given the track and pill widths and an eased phase. Kept here so the rule that
	// the pill never overhangs the track is stated in one place and can be checked: the sweep is deliberately drawn
	// without a clip, so an overhang would paint outside the widget rather than being trimmed.
	static float PillLeftForPhase(float InTrackWidth, float InPillWidth, float InEasedPhase);

private:
	EActiveTimerReturnType HandleAnimationTick(double InCurrentTime, float InDeltaTime);

	float Thickness = 4.0f;
	float SecondsPerSweep = 1.15f;

	// Where the pill is along its sweep, 0 to 1 and back. Written by the timer, read by paint.
	float Phase = 0.0f;

	TWeakPtr<FActiveTimerHandle> AnimationTimerHandle;
};
