// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/SCrowdyLogoIntro.h"

#include "Math/TransformCalculus2D.h"
#include "Rendering/SlateRenderTransform.h"
#include "Style/CrowdyStudioStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyle.h"
#include "UI/CrowdyStudioWidgets.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CrowdyStudio"

namespace
{
	constexpr float IntroIn = 0.75f;    // scale + fade in
	constexpr float IntroHold = 0.55f;  // hold at full
	constexpr float IntroOut = 0.70f;   // dissolve the overlay away
}

void SCrowdyLogoIntro::Construct(const FArguments& InArgs)
{
	OnFinished = InArgs._OnFinished;

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FCrowdyStudioStyle::Get().GetBrush("Crowdy.Panel"))
		.HAlign(HAlign_Fill)
		.VAlign(VAlign_Fill)
		.Padding(0.0f)
		[
			SNew(SBox).HAlign(HAlign_Center).VAlign(VAlign_Center)
			[
				SAssignNew(MarkBox, SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
				[
					CrowdyStudioWidgets::BrandMark(36, /*bWithCrown*/ true, HAlign_Center)
				]
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 16.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("IntroTagline", "STUDIO  CONSOLE"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
					.ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSubtle()))
				]
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 22.0f, 0.0f, 0.0f)
				[
					SNew(SCircularThrobber).Radius(11.0f)
				]
			]
		]
	];

	if (MarkBox.IsValid())
	{
		MarkBox->SetRenderTransformPivot(FVector2D(0.5f, 0.5f));
		MarkBox->SetRenderOpacity(0.0f);
	}

	RegisterActiveTimer(0.0f, FWidgetActiveTimerDelegate::CreateSP(this, &SCrowdyLogoIntro::OnIntroTick));
}

EActiveTimerReturnType SCrowdyLogoIntro::OnIntroTick(double /*InCurrentTime*/, float InDeltaTime)
{
	if (bDone)
	{
		return EActiveTimerReturnType::Stop;
	}

	Elapsed += InDeltaTime;
	const float Total = IntroIn + IntroHold + IntroOut;

	float Scale = 1.0f;
	float MarkOpacity = 1.0f;
	float OverlayOpacity = 1.0f;

	if (Elapsed < IntroIn)
	{
		const float T = FMath::Clamp(Elapsed / IntroIn, 0.0f, 1.0f);
		const float E = 1.0f - FMath::Pow(1.0f - T, 3.0f); // ease-out cubic
		Scale = FMath::Lerp(0.82f, 1.0f, E);
		MarkOpacity = E;
	}
	else if (Elapsed < IntroIn + IntroHold)
	{
		Scale = 1.0f;
		MarkOpacity = 1.0f;
	}
	else if (Elapsed < Total)
	{
		const float T = FMath::Clamp((Elapsed - IntroIn - IntroHold) / IntroOut, 0.0f, 1.0f);
		const float E = T * T * T; // ease-in cubic
		Scale = FMath::Lerp(1.0f, 1.08f, E);
		MarkOpacity = 1.0f - E;
		OverlayOpacity = 1.0f - E;
	}
	else
	{
		Finish();
		return EActiveTimerReturnType::Stop;
	}

	if (MarkBox.IsValid())
	{
		MarkBox->SetRenderTransform(TOptional<FSlateRenderTransform>(FSlateRenderTransform(FScale2D(Scale))));
		MarkBox->SetRenderOpacity(MarkOpacity);
	}
	SetRenderOpacity(OverlayOpacity);

	return EActiveTimerReturnType::Continue;
}

void SCrowdyLogoIntro::Finish()
{
	if (bDone)
	{
		return;
	}
	bDone = true;
	SetVisibility(EVisibility::Collapsed);
	SetRenderOpacity(1.0f);
	OnFinished.ExecuteIfBound();
}

FReply SCrowdyLogoIntro::OnMouseButtonDown(const FGeometry& /*MyGeometry*/, const FPointerEvent& /*MouseEvent*/)
{
	if (!bDone)
	{
		// Skip ahead to the dissolve so the reveal still feels intentional.
		Elapsed = FMath::Max(Elapsed, IntroIn + IntroHold);
	}
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
