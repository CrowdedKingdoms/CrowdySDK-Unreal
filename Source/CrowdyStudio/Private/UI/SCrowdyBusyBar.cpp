// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/SCrowdyBusyBar.h"

#include "Style/CrowdyStudioStyle.h"
#include "Styling/ISlateStyle.h"
#include "Types/WidgetActiveTimerDelegate.h"

namespace
{
	// The pill's share of the track. Long enough to read as a moving object rather than a dot, short enough that the
	// gap either side of it is obvious.
	constexpr float CrowdyBusyBarPillFraction = 0.32f;

}

float SCrowdyBusyBar::Ease(float InPhase)
{
	const float Clamped = FMath::Clamp(InPhase, 0.0f, 1.0f);
	return Clamped * Clamped * (3.0f - 2.0f * Clamped);
}

float SCrowdyBusyBar::PhaseAtTime(double InCurrentTime, float InSecondsPerSweep)
{
	const double Sweep = FMath::Max(static_cast<double>(InSecondsPerSweep), 0.05);
	const double Cycle = Sweep * 2.0;

	// Fmod is negative for negative input, so a clock that has not started yet would otherwise fold to a phase above
	// one and put the pill off the end of the track.
	double Position = FMath::Fmod(InCurrentTime, Cycle) / Cycle;
	if (Position < 0.0)
	{
		Position += 1.0;
	}

	// The second half is the first half backwards, which is what makes the turn continuous: at the halfway point both
	// expressions give one, so the pill arrives at the end and returns rather than jumping back to the start.
	return Position <= 0.5
		? static_cast<float>(Position * 2.0)
		: static_cast<float>((1.0 - Position) * 2.0);
}

float SCrowdyBusyBar::PillLeftForPhase(float InTrackWidth, float InPillWidth, float InEasedPhase)
{
	// Travel is the track minus the pill, so at phase one the pill's right edge lands exactly on the track's.
	const float Travel = FMath::Max(InTrackWidth - InPillWidth, 0.0f);
	return Travel * FMath::Clamp(InEasedPhase, 0.0f, 1.0f);
}

void SCrowdyBusyBar::Construct(const FArguments& InArgs)
{
	Thickness = FMath::Max(InArgs._Thickness, 1.0f);
	SecondsPerSweep = FMath::Max(InArgs._SecondsPerSweep, 0.05f);

	// An active timer only runs while its widget is painted, so a collapsed bar (or one on a page that is not the
	// open one) animates nothing and costs nothing. Nothing unregisters it for that reason.
	AnimationTimerHandle = RegisterActiveTimer(0.0f,
		FWidgetActiveTimerDelegate::CreateSP(this, &SCrowdyBusyBar::HandleAnimationTick));
}

EActiveTimerReturnType SCrowdyBusyBar::HandleAnimationTick(double InCurrentTime, float InDeltaTime)
{
	// Driven from absolute time rather than accumulated deltas, so a frame the editor skipped (a compile, a load)
	// leaves the sweep where it should be by then instead of behind by however long the hitch lasted.
	Phase = PhaseAtTime(InCurrentTime, SecondsPerSweep);

	Invalidate(EInvalidateWidgetReason::Paint);
	return EActiveTimerReturnType::Continue;
}

FVector2D SCrowdyBusyBar::ComputeDesiredSize(float) const
{
	// No width of its own: it takes what the slot gives it. The height is the whole of its size.
	return FVector2D(0.0f, Thickness);
}

int32 SCrowdyBusyBar::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle,
	bool bParentEnabled) const
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();
	const FSlateBrush* Track = Style.GetBrush("Crowdy.BusyBar.Track");
	const FSlateBrush* Pill = Style.GetBrush("Crowdy.BusyBar.Pill");
	if (Track == nullptr || Pill == nullptr)
	{
		return LayerId;
	}

	const FVector2f Size = FVector2f(AllottedGeometry.GetLocalSize());
	if (Size.X <= 0.0f || Size.Y <= 0.0f)
	{
		return LayerId;
	}

	// MakeBox does not carry a brush's own tint: FSlateBoxPayload::SetBrush copies layout fields only, and the fill
	// comes from the colour passed here. Both colours are therefore multiplied in by hand, or every shape paints
	// opaque white.
	FSlateDrawElement::MakeBox(
		OutDrawElements,
		LayerId,
		AllottedGeometry.ToPaintGeometry(),
		Track,
		ESlateDrawEffect::None,
		InWidgetStyle.GetColorAndOpacityTint() * Track->GetTint(InWidgetStyle));

	const float PillWidth = FMath::Min(FMath::Max(Size.X * CrowdyBusyBarPillFraction, 8.0f), Size.X);
	const float PillLeft = PillLeftForPhase(Size.X, PillWidth, Ease(Phase));

	FSlateDrawElement::MakeBox(
		OutDrawElements,
		LayerId + 1,
		AllottedGeometry.ToPaintGeometry(FVector2f(PillWidth, Size.Y), FSlateLayoutTransform(FVector2f(PillLeft, 0.0f))),
		Pill,
		ESlateDrawEffect::None,
		InWidgetStyle.GetColorAndOpacityTint() * Pill->GetTint(InWidgetStyle));

	return LayerId + 1;
}
