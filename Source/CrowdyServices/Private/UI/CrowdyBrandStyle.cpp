// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/CrowdyBrandStyle.h"

#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Framework/Application/SlateApplication.h"
#include "Styling/CoreStyle.h"
#include "Styling/ISlateStyle.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateTypes.h"
#include "Styling/StarshipCoreStyle.h"

TSharedPtr<FSlateStyleSet> FCrowdyBrandStyle::Instance = nullptr;

namespace
{
	// Parse a 0xRRGGBB literal as an sRGB colour and convert to linear (Slate renders linear).
	FLinearColor Hex(uint32 RGB, float A = 1.0f)
	{
		const FColor C((RGB >> 16) & 0xFF, (RGB >> 8) & 0xFF, RGB & 0xFF, 255);
		FLinearColor L = FLinearColor::FromSRGBColor(C);
		L.A = A;
		return L;
	}
}

// Palette (Crowded Kingdoms brand, adapted to a dark host).
// Brand: bold / minimal / premium. Kingdom Black surfaces, Pure White ink, Warm Gold earned
// (selected nav, focus, the single key action per page), Stone Gray captions.
FLinearColor FCrowdyBrandStyle::Panel()        { return Hex(0x0B0B0C); } // window background (near Kingdom Black)
FLinearColor FCrowdyBrandStyle::Rail()         { return Hex(0x070708); } // navigation rail (Kingdom Black)
FLinearColor FCrowdyBrandStyle::Surface()      { return Hex(0x161617); } // card / platter (Soft Black)
FLinearColor FCrowdyBrandStyle::SurfaceHover() { return Hex(0x202022); } // raised / hovered surface
FLinearColor FCrowdyBrandStyle::Line()         { return Hex(0x2B2B2D); } // hairline borders
FLinearColor FCrowdyBrandStyle::TextPrimary()  { return Hex(0xFAFAFA); } // Pure White ink
FLinearColor FCrowdyBrandStyle::TextSecondary(){ return Hex(0xA1A1A4); }
FLinearColor FCrowdyBrandStyle::TextSubtle()   { return Hex(0x6E6E73); } // Stone Gray
FLinearColor FCrowdyBrandStyle::Gold()         { return Hex(0xD6A928); } // Warm Gold (brand accent)
FLinearColor FCrowdyBrandStyle::GoldBright()   { return Hex(0xE8BE40); }
FLinearColor FCrowdyBrandStyle::Success()      { return Hex(0x4FB477); }
FLinearColor FCrowdyBrandStyle::Warning()      { return Hex(0xCC8A33); }
FLinearColor FCrowdyBrandStyle::Danger()       { return Hex(0xE05260); }
FLinearColor FCrowdyBrandStyle::Info()         { return Hex(0x6E92E8); }

TSharedRef<FSlateStyleSet> FCrowdyBrandStyle::Create()
{
	TSharedRef<FSlateStyleSet> Style = MakeShareable(new FSlateStyleSet(StyleName()));

	const FLinearColor Ink = Hex(0x0A0A0A); // black text on gold
	const FLinearColor Field = Hex(0x101011);

	Style->Set("Crowdy.Card",     new FSlateRoundedBoxBrush(Surface(), 8.0f, Line(), 1.0f));
	Style->Set("Crowdy.Inset",    new FSlateRoundedBoxBrush(Field, 8.0f, Line(), 1.0f));
	Style->Set("Crowdy.Backdrop", new FSlateColorBrush(FLinearColor(0.0f, 0.0f, 0.0f, 0.62f)));

	// Gold is the single key action; everything else is a neutral solid. Solid fills with no outline
	// avoid the rounded-corner halo.
	auto MakeButton = [](const FSlateBrush& N, const FSlateBrush& H, const FSlateBrush& P, const FSlateBrush& D,
		const FSlateColor& Fg, const FSlateColor& HoverFg, const FMargin& Padding)
	{
		return FButtonStyle()
			.SetNormal(N).SetHovered(H).SetPressed(P).SetDisabled(D)
			.SetNormalForeground(Fg).SetHoveredForeground(HoverFg).SetPressedForeground(Fg)
			.SetDisabledForeground(FCrowdyBrandStyle::TextSubtle())
			.SetNormalPadding(Padding).SetPressedPadding(Padding);
	};

	const FSlateRoundedBoxBrush Dormant(Hex(0x1C1C1E), 6.0f);
	const FMargin ButtonPadding(13.0f, 7.0f);

	Style->Set("Crowdy.Button.Primary", MakeButton(
		FSlateRoundedBoxBrush(Gold(), 6.0f),
		FSlateRoundedBoxBrush(GoldBright(), 6.0f),
		FSlateRoundedBoxBrush(Hex(0xB8901F), 6.0f),
		Dormant,
		FSlateColor(Ink), FSlateColor(Ink), ButtonPadding));

	Style->Set("Crowdy.Button.Secondary", MakeButton(
		FSlateRoundedBoxBrush(Hex(0x242427), 6.0f),
		FSlateRoundedBoxBrush(Hex(0x2E2E32), 6.0f),
		FSlateRoundedBoxBrush(Hex(0x1C1C1E), 6.0f),
		Dormant,
		FSlateColor(TextPrimary()), FSlateColor(TextPrimary()), ButtonPadding));

	Style->Set("Crowdy.Button.Link", MakeButton(
		FSlateColorBrush(FLinearColor::Transparent),
		FSlateColorBrush(FLinearColor::Transparent),
		FSlateColorBrush(FLinearColor::Transparent),
		FSlateColorBrush(FLinearColor::Transparent),
		FSlateColor(Gold()), FSlateColor(GoldBright()), FMargin(0.0f)));

	// The tick is the engine checkbox's own image, which ships with every game; the box and colours are ours.
	const FVector2f BoxSize(18.0f, 18.0f);
	FCheckBoxStyle CheckBox = FStarshipCoreStyle::GetCoreStyle().GetWidgetStyle<FCheckBoxStyle>("Checkbox");
	auto Tick = [&CheckBox](const FLinearColor& Tint)
	{
		FSlateBrush Brush = CheckBox.CheckedImage;
		Brush.TintColor = Tint;
		return Brush;
	};
	CheckBox
		.SetCheckedImage(Tick(Gold()))
		.SetCheckedHoveredImage(Tick(GoldBright()))
		.SetCheckedPressedImage(Tick(Hex(0xB8901F)))
		.SetBackgroundImage(FSlateRoundedBoxBrush(Field, 4.0f, Line(), 1.0f, BoxSize))
		.SetBackgroundHoveredImage(FSlateRoundedBoxBrush(Field, 4.0f, Hex(0x44444A), 1.0f, BoxSize))
		.SetBackgroundPressedImage(FSlateRoundedBoxBrush(Field, 4.0f, Gold(), 1.0f, BoxSize))
		.SetPadding(FMargin(10.0f, 0.0f, 0.0f, 0.0f));
	Style->Set("Crowdy.CheckBox", CheckBox);

	const FName Bold = "Bold";
	const FName Reg = "Regular";
	Style->Set("Crowdy.Text.Title",        FTextBlockStyle().SetFont(FCoreStyle::GetDefaultFontStyle(Bold, 17)).SetColorAndOpacity(TextPrimary()));
	Style->Set("Crowdy.Text.Body",         FTextBlockStyle().SetFont(FCoreStyle::GetDefaultFontStyle(Reg, 10)).SetColorAndOpacity(TextSecondary()));
	Style->Set("Crowdy.Text.BodyStrong",   FTextBlockStyle().SetFont(FCoreStyle::GetDefaultFontStyle(Bold, 10)).SetColorAndOpacity(TextPrimary()));
	Style->Set("Crowdy.Text.Error",        FTextBlockStyle().SetFont(FCoreStyle::GetDefaultFontStyle(Reg, 10)).SetColorAndOpacity(Danger()));
	Style->Set("Crowdy.Text.SectionLabel", FTextBlockStyle().SetFont(FCoreStyle::GetDefaultFontStyle(Bold, 8)).SetColorAndOpacity(TextSubtle()));

	return Style;
}

const ISlateStyle& FCrowdyBrandStyle::Get()
{
	check(FSlateApplication::IsInitialized());
	if (!Instance.IsValid())
	{
		Instance = Create();
	}
	return *Instance;
}

void FCrowdyBrandStyle::Shutdown()
{
	Instance.Reset();
}

FName FCrowdyBrandStyle::StyleName()
{
	static const FName Name(TEXT("CrowdyBrandStyle"));
	return Name;
}
