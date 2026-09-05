// Fill out your copyright notice in the Description page of Project Settings.

#include "Style/CrowdyStudioStyle.h"

#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateImageBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Interfaces/IPluginManager.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Styling/SlateTypes.h"

TSharedPtr<FSlateStyleSet> FCrowdyStudioStyle::Instance = nullptr;

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

	FLinearColor WithAlpha(FLinearColor C, float A)
	{
		C.A = A;
		return C;
	}
}

// Palette (Crowded Kingdoms brand, adapted to the dark editor host).
// Brand: bold / minimal / premium. Kingdom Black surfaces, Pure White ink, Warm Gold earned
// (selected nav, focus, the single key action per page), Stone Gray captions.
FLinearColor FCrowdyStudioStyle::Panel()        { return Hex(0x0B0B0C); } // window background (near Kingdom Black)
FLinearColor FCrowdyStudioStyle::Rail()         { return Hex(0x070708); } // navigation rail (Kingdom Black)
FLinearColor FCrowdyStudioStyle::Surface()      { return Hex(0x161617); } // card / platter (Soft Black)
FLinearColor FCrowdyStudioStyle::SurfaceHover() { return Hex(0x202022); } // raised / hovered surface
FLinearColor FCrowdyStudioStyle::Line()         { return Hex(0x2B2B2D); } // hairline borders
FLinearColor FCrowdyStudioStyle::TextPrimary()  { return Hex(0xFAFAFA); } // Pure White ink
FLinearColor FCrowdyStudioStyle::TextSecondary(){ return Hex(0xA1A1A4); }
FLinearColor FCrowdyStudioStyle::TextSubtle()   { return Hex(0x6E6E73); } // Stone Gray
FLinearColor FCrowdyStudioStyle::Gold()         { return Hex(0xD6A928); } // Warm Gold (brand accent)
FLinearColor FCrowdyStudioStyle::GoldBright()   { return Hex(0xE8BE40); }
FLinearColor FCrowdyStudioStyle::Success()      { return Hex(0x4FB477); }
FLinearColor FCrowdyStudioStyle::Warning()      { return Hex(0xCC8A33); }
FLinearColor FCrowdyStudioStyle::Danger()       { return Hex(0xE05260); }
FLinearColor FCrowdyStudioStyle::Info()         { return Hex(0x6E92E8); }

TSharedRef<FSlateStyleSet> FCrowdyStudioStyle::Create()
{
	TSharedRef<FSlateStyleSet> Style = MakeShareable(new FSlateStyleSet(StyleName()));

	if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("CrowdySDK")))
	{
		Style->SetContentRoot(Plugin->GetBaseDir() / TEXT("Resources"));
	}

	const FVector2D Icon18(18.0, 18.0);
	const FLinearColor Ink = Hex(0x0A0A0A); // black text on gold

	// Surfaces
	Style->Set("Crowdy.Panel",         new FSlateColorBrush(Panel()));
	Style->Set("Crowdy.Rail",          new FSlateColorBrush(Rail()));
	Style->Set("Crowdy.Separator",     new FSlateColorBrush(Line()));
	Style->Set("Crowdy.Card",          new FSlateRoundedBoxBrush(Surface(), 8.0f, Line(), 1.0f));
	Style->Set("Crowdy.Card.Flat",     new FSlateRoundedBoxBrush(Surface(), 8.0f));
	Style->Set("Crowdy.Card.Selected", new FSlateRoundedBoxBrush(SurfaceHover(), 8.0f, Gold(), 1.5f));
	Style->Set("Crowdy.Hero",          new FSlateRoundedBoxBrush(SurfaceHover(), 10.0f, Line(), 1.0f));
	Style->Set("Crowdy.Inset",         new FSlateRoundedBoxBrush(Hex(0x101011), 8.0f, Line(), 1.0f));
	Style->Set("Crowdy.Pill",          new FSlateRoundedBoxBrush(FLinearColor::White, 12.0f)); // tinted per badge/dot
	Style->Set("Crowdy.Chip",          new FSlateRoundedBoxBrush(Hex(0x232325), 5.0f));
	Style->Set("Crowdy.Nav.Active",    new FSlateRoundedBoxBrush(WithAlpha(Gold(), 0.14f), 6.0f, WithAlpha(Gold(), 0.5f), 1.0f));
	Style->Set("Crowdy.Nav.Hover",     new FSlateRoundedBoxBrush(FLinearColor(1, 1, 1, 0.05f), 6.0f));

	// The gutter-glyph shapes, drawn a few pixels wide beside a table row to say where that row came from. All three
	// carry the same neutral ink on purpose: the shape is what distinguishes them, so a reader who cannot tell two
	// colours apart reads exactly what everyone else does. The corner radius is what turns one brush into a disc and
	// another into a square at these sizes, since a rounded box clamps its radius to half the box.
	Style->Set("Crowdy.Glyph.Dot",     new FSlateRoundedBoxBrush(TextSecondary(), 12.0f));
	Style->Set("Crowdy.Glyph.Ring",    new FSlateRoundedBoxBrush(FLinearColor::Transparent, 12.0f, TextSecondary(), 1.5f));
	Style->Set("Crowdy.Glyph.Square",  new FSlateRoundedBoxBrush(TextSecondary(), 1.0f));

	// Input
	{
		FEditableTextBoxStyle EditStyle = FAppStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>("NormalEditableTextBox");
		EditStyle
			.SetBackgroundImageNormal(FSlateRoundedBoxBrush(Hex(0x101011), 6.0f, Line(), 1.0f))
			.SetBackgroundImageHovered(FSlateRoundedBoxBrush(Hex(0x101011), 6.0f, Hex(0x44444A), 1.0f))
			.SetBackgroundImageFocused(FSlateRoundedBoxBrush(Hex(0x101011), 6.0f, Gold(), 1.0f))
			.SetBackgroundImageReadOnly(FSlateRoundedBoxBrush(Hex(0x0C0C0D), 6.0f, Line(), 1.0f))
			.SetForegroundColor(TextPrimary())
			.SetPadding(FMargin(9.0f, 6.0f));
		Style->Set("Crowdy.Input", EditStyle);
	}

	// Buttons.
	// Brand button system: gold (Primary) is the single key action per page; everything else is a
	// neutral solid (Secondary). Solid fills with NO outline avoid the rounded-corner halo and give a
	// clear hierarchy. Ghost is for tertiary/toolbar actions and toggles.
	auto MakeButton = [](const FSlateBrush& N, const FSlateBrush& H, const FSlateBrush& P, const FSlateColor& Fg)
	{
		return FButtonStyle()
			.SetNormal(N).SetHovered(H).SetPressed(P)
			.SetNormalForeground(Fg).SetHoveredForeground(Fg).SetPressedForeground(Fg).SetDisabledForeground(FCrowdyStudioStyle::TextSubtle())
			.SetNormalPadding(FMargin(13.0f, 7.0f)).SetPressedPadding(FMargin(13.0f, 7.0f));
	};

	Style->Set("Crowdy.Button.Primary", MakeButton(
		FSlateRoundedBoxBrush(Gold(), 6.0f),
		FSlateRoundedBoxBrush(GoldBright(), 6.0f),
		FSlateRoundedBoxBrush(Hex(0xB8901F), 6.0f),
		FSlateColor(Ink)));

	Style->Set("Crowdy.Button.Secondary", MakeButton(
		FSlateRoundedBoxBrush(Hex(0x242427), 6.0f),
		FSlateRoundedBoxBrush(Hex(0x2E2E32), 6.0f),
		FSlateRoundedBoxBrush(Hex(0x1C1C1E), 6.0f),
		FSlateColor(TextPrimary())));

	Style->Set("Crowdy.Button.Ghost", MakeButton(
		FSlateColorBrush(FLinearColor::Transparent),
		FSlateRoundedBoxBrush(FLinearColor(1, 1, 1, 0.06f), 6.0f),
		FSlateRoundedBoxBrush(FLinearColor(1, 1, 1, 0.03f), 6.0f),
		FSlateColor(TextSecondary())));

	Style->Set("Crowdy.Button.Nav", MakeButton(
		FSlateColorBrush(FLinearColor::Transparent),
		FSlateRoundedBoxBrush(FLinearColor(1, 1, 1, 0.05f), 6.0f),
		FSlateRoundedBoxBrush(FLinearColor(1, 1, 1, 0.03f), 6.0f),
		FSlateColor(TextSecondary())));

	// List row (custom selection: gold-tinted, faint hover; flat transparent rows).
	{
		FTableRowStyle RowStyle = FAppStyle::Get().GetWidgetStyle<FTableRowStyle>("TableView.Row");
		const FSlateColorBrush Transparent(FLinearColor::Transparent);
		const FSlateColorBrush Hovered(FLinearColor(1, 1, 1, 0.035f));
		const FSlateColorBrush SelectedActive(WithAlpha(Gold(), 0.16f));
		const FSlateColorBrush SelectedInactive(WithAlpha(Gold(), 0.11f));
		RowStyle
			.SetEvenRowBackgroundBrush(Transparent).SetOddRowBackgroundBrush(Transparent)
			.SetEvenRowBackgroundHoveredBrush(Hovered).SetOddRowBackgroundHoveredBrush(Hovered)
			.SetActiveBrush(SelectedActive).SetActiveHoveredBrush(SelectedActive)
			.SetInactiveBrush(SelectedInactive).SetInactiveHoveredBrush(SelectedInactive)
			.SetSelectorFocusedBrush(Transparent)
			.SetTextColor(TextSecondary()).SetSelectedTextColor(TextPrimary());
		Style->Set("Crowdy.TableRow", RowStyle);
	}

	// Splitter (the drag handle between the model list and the detail panel).
	{
		FSplitterStyle SplitterStyle = FAppStyle::Get().GetWidgetStyle<FSplitterStyle>("Splitter");
		SplitterStyle
			.SetHandleNormalBrush(FSlateColorBrush(Line()))
			.SetHandleHighlightBrush(FSlateColorBrush(SurfaceHover()));
		Style->Set("Crowdy.Splitter", SplitterStyle);
	}

	// Header row for the multi-column model tables.
	{
		FTableColumnHeaderStyle ColumnStyle = FAppStyle::Get().GetWidgetStyle<FTableColumnHeaderStyle>("TableView.Header.Column");
		ColumnStyle
			.SetNormalBrush(FSlateColorBrush(Surface()))
			.SetHoveredBrush(FSlateColorBrush(SurfaceHover()));

		FHeaderRowStyle HeaderRowStyle = FAppStyle::Get().GetWidgetStyle<FHeaderRowStyle>("TableView.Header");
		HeaderRowStyle
			.SetColumnStyle(ColumnStyle)
			.SetLastColumnStyle(ColumnStyle)
			.SetBackgroundBrush(FSlateColorBrush(Surface()))
			.SetForegroundColor(TextSubtle())
			.SetHorizontalSeparatorBrush(FSlateColorBrush(Line()))
			.SetHorizontalSeparatorThickness(1.0f);
		Style->Set("Crowdy.HeaderRow", HeaderRowStyle);
	}

	// Search box, built on Crowdy.Input so focus is the same gold hairline as every other field.
	{
		FSearchBoxStyle SearchBoxStyle = FAppStyle::Get().GetWidgetStyle<FSearchBoxStyle>("SearchBox");
		SearchBoxStyle.SetTextBoxStyle(Style->GetWidgetStyle<FEditableTextBoxStyle>("Crowdy.Input"));
		Style->Set("Crowdy.SearchBox", SearchBoxStyle);
	}

	// Table view background. Rows stay on the shared Crowdy.TableRow; only the backing brush is new.
	Style->Set("Crowdy.TableView", FTableViewStyle().SetBackgroundBrush(FSlateColorBrush(Panel())));

	// The two pieces SCrowdyBusyBar paints: a recessed track and the pill that sweeps along it. Neutral, like the rest
	// of the Game Model page, because this reports activity rather than a result and so spends no colour on meaning.
	// Plain brushes rather than an FProgressBarStyle: SProgressBar's marquee needs a repeating tile to look like it is
	// moving, and a solid brush there paints a bar that never appears to animate at all.
	Style->Set("Crowdy.BusyBar.Track", new FSlateRoundedBoxBrush(Hex(0x18181A), 2.0f, FVector2f(64.0f, 4.0f)));
	Style->Set("Crowdy.BusyBar.Pill",  new FSlateRoundedBoxBrush(TextSecondary(), 2.0f, FVector2f(20.0f, 4.0f)));

	// Text styles.
	const FName Bold = "Bold";
	const FName Reg = "Regular";
	Style->Set("Crowdy.Text.Title",        FTextBlockStyle().SetFont(FCoreStyle::GetDefaultFontStyle(Bold, 17)).SetColorAndOpacity(TextPrimary()));
	Style->Set("Crowdy.Text.Heading",      FTextBlockStyle().SetFont(FCoreStyle::GetDefaultFontStyle(Bold, 12)).SetColorAndOpacity(TextPrimary()));
	Style->Set("Crowdy.Text.Body",         FTextBlockStyle().SetFont(FCoreStyle::GetDefaultFontStyle(Reg, 10)).SetColorAndOpacity(TextSecondary()));
	Style->Set("Crowdy.Text.BodyStrong",   FTextBlockStyle().SetFont(FCoreStyle::GetDefaultFontStyle(Bold, 10)).SetColorAndOpacity(TextPrimary()));
	Style->Set("Crowdy.Text.Subtle",       FTextBlockStyle().SetFont(FCoreStyle::GetDefaultFontStyle(Reg, 9)).SetColorAndOpacity(TextSubtle()));
	Style->Set("Crowdy.Text.SectionLabel", FTextBlockStyle().SetFont(FCoreStyle::GetDefaultFontStyle(Bold, 8)).SetColorAndOpacity(TextSubtle()));

	// Icons (line-art glyphs, white; tinted per-state at the call site).
	auto SvgIcon = [&Style, &Icon18](const TCHAR* Name)
	{
		const FString Rel = FString::Printf(TEXT("Icons/%s"), Name);
		Style->Set(*FString::Printf(TEXT("Crowdy.Icon.%s"), Name),
			new FSlateVectorImageBrush(Style->RootToContentDir(Rel, TEXT(".svg")), Icon18));
	};
	const TCHAR* IconNames[] = {
		TEXT("login"), TEXT("apps"), TEXT("server"), TEXT("config"), TEXT("users"),
		TEXT("broadcast"), TEXT("grid"), TEXT("cube"), TEXT("inspector"), TEXT("external-link"),
		TEXT("refresh"), TEXT("home"), TEXT("wand"), TEXT("chevron-right"), TEXT("plus"),
		TEXT("check"), TEXT("clock"), TEXT("ck"),
		// Sign-in options: federated provider marks (monochrome, tinted at the call site) + magic-link.
		TEXT("google"), TEXT("github"), TEXT("discord"), TEXT("apple"), TEXT("microsoft"),
		TEXT("key"), TEXT("mail")
	};
	for (const TCHAR* Name : IconNames)
	{
		SvgIcon(Name);
	}

	return Style;
}

void FCrowdyStudioStyle::Initialize()
{
	if (!Instance.IsValid())
	{
		Instance = Create();
		FSlateStyleRegistry::RegisterSlateStyle(*Instance);
	}
}

void FCrowdyStudioStyle::Shutdown()
{
	if (Instance.IsValid())
	{
		FSlateStyleRegistry::UnRegisterSlateStyle(*Instance);
		Instance.Reset();
	}
}

const ISlateStyle& FCrowdyStudioStyle::Get()
{
	check(Instance.IsValid());
	return *Instance;
}

FName FCrowdyStudioStyle::StyleName()
{
	static const FName Name(TEXT("CrowdyStudioStyle"));
	return Name;
}

const FSlateBrush* FCrowdyStudioStyle::IconBrush(const TCHAR* IconName)
{
	if (!Instance.IsValid())
	{
		return nullptr;
	}
	return Instance->GetBrush(*FString::Printf(TEXT("Crowdy.Icon.%s"), IconName));
}
