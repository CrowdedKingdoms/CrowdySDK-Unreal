// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Layout/Margin.h"
#include "Misc/Attribute.h"
#include "Styling/SlateColor.h"
#include "Templates/Function.h"
#include "Types/SlateEnums.h"

class SWidget;
class SEditableTextBox;

/**
 * Reusable building blocks for the CrowdyStudio console, all drawn from FCrowdyStudioStyle.
 * Views compose these instead of hand-rolling SBorders/fonts, so every surface shares the
 * same card radius, padding rhythm, badge colours, and typography.
 */
namespace CrowdyStudioWidgets
{
	enum class EBadgeTone : uint8
	{
		Neutral,
		Success,
		Warning,
		Danger,
		Info,
		Brand
	};

	// The two-line CROWDED / KINGDOMS wordmark (white over gold), with an optional crown above.
	// FontSize scales the words; the crown scales with them.
	TSharedRef<SWidget> BrandMark(int32 FontSize = 28, bool bWithCrown = true, EHorizontalAlignment HAlign = HAlign_Center);

	// A rounded card/platter wrapping arbitrary content.
	TSharedRef<SWidget> Card(const TSharedRef<SWidget>& Content, const FMargin& Padding = FMargin(16.0f, 14.0f), bool bFlat = false);

	// Section header: an optional leading icon, a bold title, and an optional right-aligned accessory.
	TSharedRef<SWidget> SectionHeader(const FText& Title, const TCHAR* Icon = nullptr, TSharedPtr<SWidget> RightAccessory = nullptr);

	// A small bold caps group label for the nav rail and section dividers.
	TSharedRef<SWidget> GroupLabel(const FText& Text);

	// A coloured status pill.
	TSharedRef<SWidget> Badge(const FText& Label, EBadgeTone Tone);

	// Maps an app/environment/group status string (LIVE/DRAFT/ARCHIVED/...) to a badge tone.
	EBadgeTone ToneForStatus(const FString& Status);

	// The strong colour a tone draws its text in (its fill is the same at low alpha), for badges built by hand.
	FLinearColor ColorForTone(EBadgeTone Tone);

	// A monospace chip, e.g. a slug or id.
	TSharedRef<SWidget> Chip(const FText& Text);

	// A small ghost button that copies Value to the clipboard on click; the tooltip names what it copies.
	TSharedRef<SWidget> CopyButton(TAttribute<FString> Value, const FText& What);

	// A chip with a copy button beside it, for ids, slugs and URLs the user will paste elsewhere.
	TSharedRef<SWidget> CopyChip(TAttribute<FString> Value, const FText& What);

	// A tinted, sized icon image. Name is the bare glyph name ("login", "apps", ...).
	TSharedRef<SWidget> Icon(const TCHAR* Name, float Size = 18.0f, FSlateColor Tint = FSlateColor(FLinearColor::White));

	// A labelled field: caption above the input, with an optional hint below it.
	TSharedRef<SWidget> Field(const FText& Label, const TSharedRef<SWidget>& Input, const FText& Hint = FText::GetEmpty());

	// A segmented single-choice control (e.g. an enum). Values are the stored choices and Labels
	// their display text (parallel arrays). Current returns the selected value; OnSelected fires
	// with the chosen value.
	TSharedRef<SWidget> SegmentedEnum(const TArray<FString>& Values, const TArray<FText>& Labels,
		TAttribute<FString> Current, TFunction<void(const FString&)> OnSelected);

	// A row of page tabs, same call shape as SegmentedEnum: Values are the stored tab keys and
	// Labels their display text (parallel arrays), Current returns the open tab, OnSelected fires
	// with the clicked one. Unlike SegmentedEnum this marks the open tab with weight and a neutral
	// underline instead of the brand gold, so it can sit on a page that already spends colour on
	// meaning (a warning or an error state) without competing with it.
	TSharedRef<SWidget> TabStrip(const TArray<FString>& Values, const TArray<FText>& Labels,
		TAttribute<FString> Current, TFunction<void(const FString&)> OnSelected);

	// One tab, for the overload below.
	struct FCrowdyTabItem
	{
		FString Value;
		FText Label;

		// Unset means always available. An unavailable tab keeps its place, so the strip never reflows.
		TAttribute<bool> IsEnabled;

		// Shown on hover in either state, so a tab the user cannot click can say why not.
		TAttribute<FText> ToolTip;
	};

	// The same strip, with per-tab availability. An unavailable tab is drawn in the subtle tier, takes no
	// underline, and cannot be clicked; its tooltip is what tells the user why.
	TSharedRef<SWidget> TabStrip(const TArray<FCrowdyTabItem>& Tabs,
		TAttribute<FString> Current, TFunction<void(const FString&)> OnSelected);

	// A centred empty-state placeholder (icon + message).
	TSharedRef<SWidget> EmptyState(const TCHAR* Icon, const FText& Message);

	// A styled editable text box (Crowdy.Input look).
	TSharedRef<SEditableTextBox> Input(const FText& Hint, bool bPassword = false);

	// A member/group cap text box reads as a number, or 0 ("unlimited") when blank or non-positive.
	int32 ReadCapBox(const TSharedPtr<SEditableTextBox>& Box);

	// Show a positive cap as a plain integer (no grouping commas, so it re-parses cleanly); 0 = blank.
	void SetCapBox(const TSharedPtr<SEditableTextBox>& Box, int32 Value);
}
