// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Model/FCrowdyStudioController.h"
#include "Style/CrowdyStudioStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/ISlateStyle.h"
#include "UI/CrowdyStudioWidgets.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

// Small Slate helpers shared by the Game Model tabs: a labeled text field, a labeled segmented choice, a
// fixed-height card wrapping a list, the invoke-policy rule-type label, a readiness pill, and warning joining.
// Every one of these is `inline`, and this is their only home, because UE's unity build merges every .cpp in
// this module into fewer translation units: two identical free-function definitions in different .cpp files
// would redefine each other and fail to compile, nondeterministically depending on how the unity blob is cut.
namespace CrowdyGameModelWidgets
{
	inline TSharedRef<SWidget> LabeledField(TSharedPtr<SEditableTextBox>& Member, const FText& Label, const TCHAR* Hint)
	{
		const ISlateStyle& Style = FCrowdyStudioStyle::Get();
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 10.0f, 0.0f)
			[ SNew(SBox).WidthOverride(140.0f)[ SNew(STextBlock).Text(Label).TextStyle(&Style, "Crowdy.Text.Body") ] ]
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[ SAssignNew(Member, SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(FText::FromString(Hint)) ];
	}

	// A label-on-the-left row whose input is a segmented single-choice control, matching LabeledField's layout.
	// Values are the stored choices, Labels their display text.
	inline TSharedRef<SWidget> LabeledChoice(const FText& Label, const TArray<FString>& Values, const TArray<FText>& Labels,
		TAttribute<FString> Current, TFunction<void(const FString&)> OnSelected)
	{
		const ISlateStyle& Style = FCrowdyStudioStyle::Get();
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 10.0f, 0.0f)
			[ SNew(SBox).WidthOverride(140.0f)[ SNew(STextBlock).Text(Label).TextStyle(&Style, "Crowdy.Text.Body") ] ]
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[ CrowdyStudioWidgets::SegmentedEnum(Values, Labels, Current, OnSelected) ];
	}

	inline TSharedRef<SWidget> ListCard(const TSharedRef<SWidget>& List, float Height)
	{
		return SNew(SBox).HeightOverride(Height)
			[ CrowdyStudioWidgets::Card(List, FMargin(4.0f), /*bFlat*/ true) ];
	}

	// The eight invoke-policy leaf kinds, in plain language for the rule-type dropdown.
	inline FText PolicyTypeLabel(const FString& Type)
	{
		if (Type == TEXT("owner_of_self")) { return NSLOCTEXT("CrowdyStudio", "PolOwner", "Owns the target (self)"); }
		if (Type == TEXT("is_current_turn")) { return NSLOCTEXT("CrowdyStudio", "PolTurn", "It's their turn"); }
		if (Type == TEXT("is_host")) { return NSLOCTEXT("CrowdyStudio", "PolHost", "Is the host"); }
		if (Type == TEXT("is_participant")) { return NSLOCTEXT("CrowdyStudio", "PolParticipant", "Is in the session"); }
		if (Type == TEXT("tier_feature")) { return NSLOCTEXT("CrowdyStudio", "PolTier", "Has tier feature"); }
		if (Type == TEXT("group_permission")) { return NSLOCTEXT("CrowdyStudio", "PolGroup", "Has team permission"); }
		if (Type == TEXT("grid_permission")) { return NSLOCTEXT("CrowdyStudio", "PolGrid", "Has grid permission"); }
		if (Type == TEXT("condition")) { return NSLOCTEXT("CrowdyStudio", "PolCond", "Custom condition"); }
		return NSLOCTEXT("CrowdyStudio", "PolPick", "Choose a requirement...");
	}

	// One indicator in the Game Model setup strip: a subtle label and a state word coloured by a live readiness
	// getter (green Ready / amber the not-ready word / grey when not yet determined). Lambda-bound so it tracks the
	// controller without any delegate wiring.
	inline TSharedRef<SWidget> ReadinessPill(const FText& Label, const FText& NotReadyWord, TFunction<ECrowdyStudioReadiness()> Getter)
	{
		const ISlateStyle& Style = FCrowdyStudioStyle::Get();
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 5.0f, 0.0f)
			[ SNew(STextBlock).Text(Label).TextStyle(&Style, "Crowdy.Text.Subtle") ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
				.Text_Lambda([Getter, NotReadyWord]()
				{
					switch (Getter())
					{
					case ECrowdyStudioReadiness::Ready:    return NSLOCTEXT("CrowdyStudio", "ReadyWord", "Ready");
					case ECrowdyStudioReadiness::NotReady: return NotReadyWord;
					default:                               return NSLOCTEXT("CrowdyStudio", "UnknownWord", "-");
					}
				})
				.ColorAndOpacity_Lambda([Getter]()
				{
					switch (Getter())
					{
					case ECrowdyStudioReadiness::Ready:    return FSlateColor(FCrowdyStudioStyle::Success());
					case ECrowdyStudioReadiness::NotReady: return FSlateColor(FCrowdyStudioStyle::Warning());
					default:                               return FSlateColor(FCrowdyStudioStyle::TextSecondary());
					}
				})
			];
	}

	// Join the server's static-analysis warnings into one bulleted block for the function editor.
	inline FString JoinWarnings(const TArray<FString>& Warnings)
	{
		FString Out;
		for (const FString& Warning : Warnings)
		{
			if (!Out.IsEmpty())
			{
				Out += LINE_TERMINATOR;
			}
			Out += TEXT("- ") + Warning;
		}
		return Out;
	}
}
