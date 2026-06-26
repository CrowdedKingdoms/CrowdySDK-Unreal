// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/CrowdyStudioWidgets.h"

#include "Style/CrowdyStudioStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyle.h"
#include "Styling/StyleDefaults.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CrowdyStudio"

namespace
{
	FLinearColor StrongForTone(CrowdyStudioWidgets::EBadgeTone Tone)
	{
		using EBadgeTone = CrowdyStudioWidgets::EBadgeTone;
		switch (Tone)
		{
		case EBadgeTone::Success: return FCrowdyStudioStyle::Success();
		case EBadgeTone::Warning: return FCrowdyStudioStyle::Warning();
		case EBadgeTone::Danger:  return FCrowdyStudioStyle::Danger();
		case EBadgeTone::Info:    return FCrowdyStudioStyle::Info();
		case EBadgeTone::Brand:   return FCrowdyStudioStyle::GoldBright();
		case EBadgeTone::Neutral:
		default:                  return FCrowdyStudioStyle::TextSecondary();
		}
	}
}

TSharedRef<SWidget> CrowdyStudioWidgets::Icon(const TCHAR* Name, float Size, FSlateColor Tint)
{
	const FSlateBrush* Brush = FCrowdyStudioStyle::IconBrush(Name);
	return SNew(SBox)
		.WidthOverride(Size)
		.HeightOverride(Size)
		[
			SNew(SImage)
			.Image(Brush ? Brush : FStyleDefaults::GetNoBrush())
			.ColorAndOpacity(Tint)
		];
}

TSharedRef<SWidget> CrowdyStudioWidgets::BrandMark(int32 FontSize, bool bWithCrown, EHorizontalAlignment HAlign)
{
	const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle("Bold", FontSize);
	const float LineGap = FMath::Max(1.0f, FontSize / 9.0f);

	TSharedRef<SVerticalBox> Words = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("BrandLine1", "CROWDED"))
			.Font(Font)
			.ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextPrimary()))
		]
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign).Padding(0.0f, LineGap, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("BrandLine2", "KINGDOMS"))
			.Font(Font)
			.ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::Gold()))
		];

	// Crown emblem dropped per brand guidance — simple two-tone text wordmark only.
	(void)bWithCrown;
	return Words;
}

TSharedRef<SWidget> CrowdyStudioWidgets::Card(const TSharedRef<SWidget>& Content, const FMargin& Padding, bool bFlat)
{
	return SNew(SBorder)
		.BorderImage(FCrowdyStudioStyle::Get().GetBrush(bFlat ? "Crowdy.Card.Flat" : "Crowdy.Card"))
		.Padding(Padding)
		[
			Content
		];
}

TSharedRef<SWidget> CrowdyStudioWidgets::SectionHeader(const FText& Title, const TCHAR* IconName, TSharedPtr<SWidget> RightAccessory)
{
	TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);

	if (IconName)
	{
		Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
		[
			Icon(IconName, 16.0f, FSlateColor(FCrowdyStudioStyle::TextSecondary()))
		];
	}

	Row->AddSlot().AutoWidth().VAlign(VAlign_Center)
	[
		SNew(STextBlock).Text(Title).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.Heading")
	];

	Row->AddSlot().FillWidth(1.0f).HAlign(HAlign_Right).VAlign(VAlign_Center)
	[
		RightAccessory.IsValid() ? RightAccessory.ToSharedRef() : SNullWidget::NullWidget
	];

	return Row;
}

TSharedRef<SWidget> CrowdyStudioWidgets::GroupLabel(const FText& Text)
{
	return SNew(STextBlock).Text(Text).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.SectionLabel");
}

TSharedRef<SWidget> CrowdyStudioWidgets::Badge(const FText& Label, EBadgeTone Tone)
{
	const FLinearColor Strong = StrongForTone(Tone);
	FLinearColor Fill = Strong;
	Fill.A = 0.16f;

	return SNew(SBorder)
		.BorderImage(FCrowdyStudioStyle::Get().GetBrush("Crowdy.Pill"))
		.BorderBackgroundColor(FSlateColor(Fill))
		.Padding(FMargin(8.0f, 2.0f))
		[
			SNew(STextBlock)
			.Text(Label)
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
			.ColorAndOpacity(FSlateColor(Strong))
		];
}

CrowdyStudioWidgets::EBadgeTone CrowdyStudioWidgets::ToneForStatus(const FString& Status)
{
	const FString S = Status.ToUpper();
	if (S == TEXT("LIVE") || S == TEXT("ACTIVE") || S == TEXT("READY") || S == TEXT("ENABLED") || S == TEXT("PUBLIC"))
	{
		return EBadgeTone::Success;
	}
	if (S == TEXT("DRAFT") || S == TEXT("PENDING") || S == TEXT("UNLISTED") || S == TEXT("PROVISIONING"))
	{
		return EBadgeTone::Warning;
	}
	if (S == TEXT("ERROR") || S == TEXT("FAILED") || S == TEXT("DENIED") || S == TEXT("SUSPENDED"))
	{
		return EBadgeTone::Danger;
	}
	return EBadgeTone::Neutral; // ARCHIVED / PRIVATE / unknown
}

TSharedRef<SWidget> CrowdyStudioWidgets::Chip(const FText& Text)
{
	return SNew(SBorder)
		.BorderImage(FCrowdyStudioStyle::Get().GetBrush("Crowdy.Chip"))
		.Padding(FMargin(6.0f, 1.0f))
		[
			SNew(STextBlock)
			.Text(Text)
			.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
			.ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSecondary()))
		];
}

TSharedRef<SWidget> CrowdyStudioWidgets::Field(const FText& Label, const TSharedRef<SWidget>& Input, const FText& Hint)
{
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
		[
			SNew(STextBlock).Text(Label).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.BodyStrong")
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			Input
		];

	if (!Hint.IsEmpty())
	{
		Box->AddSlot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock).Text(Hint).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.Subtle")
		];
	}

	return Box;
}

TSharedRef<SWidget> CrowdyStudioWidgets::SegmentedEnum(const TArray<FString>& Values, const TArray<FText>& Labels,
	TAttribute<FString> Current, TFunction<void(const FString&)> OnSelected)
{
	TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);

	for (int32 Index = 0; Index < Values.Num(); ++Index)
	{
		const FString Value = Values[Index];
		const FText Label = Labels.IsValidIndex(Index) ? Labels[Index] : FText::FromString(Value);
		Row->AddSlot().FillWidth(1.0f).Padding(1.0f, 0.0f)
		[
			SNew(SBorder)
			.BorderImage_Lambda([Current, Value]()
			{
				return Current.Get() == Value
					? FCrowdyStudioStyle::Get().GetBrush("Crowdy.Nav.Active")
					: FStyleDefaults::GetNoBrush();
			})
			.Padding(0.0f)
			[
				SNew(SButton)
				.ButtonStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Button.Nav")
				.ContentPadding(FMargin(6.0f, 5.0f))
				.HAlign(HAlign_Center)
				.OnClicked_Lambda([OnSelected, Value]()
				{
					if (OnSelected)
					{
						OnSelected(Value);
					}
					return FReply::Handled();
				})
				[
					SNew(STextBlock)
					.Text(Label)
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
					.ColorAndOpacity_Lambda([Current, Value]()
					{
						return FSlateColor(Current.Get() == Value ? FCrowdyStudioStyle::Gold() : FCrowdyStudioStyle::TextSecondary());
					})
				]
			]
		];
	}

	return SNew(SBorder)
		.BorderImage(FCrowdyStudioStyle::Get().GetBrush("Crowdy.Inset"))
		.Padding(FMargin(2.0f))
		[
			Row
		];
}

TSharedRef<SWidget> CrowdyStudioWidgets::EmptyState(const TCHAR* IconName, const FText& Message)
{
	return SNew(SBox).HAlign(HAlign_Center).VAlign(VAlign_Center).Padding(20.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 0.0f, 0.0f, 10.0f)
			[
				Icon(IconName, 30.0f, FSlateColor(FCrowdyStudioStyle::TextSubtle()))
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
			[
				SNew(STextBlock)
				.Text(Message)
				.TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.Subtle")
				.Justification(ETextJustify::Center)
			]
		];
}

TSharedRef<SEditableTextBox> CrowdyStudioWidgets::Input(const FText& Hint, bool bPassword)
{
	return SNew(SEditableTextBox)
		.Style(&FCrowdyStudioStyle::Get(), "Crowdy.Input")
		.HintText(Hint)
		.IsPassword(bPassword);
}

int32 CrowdyStudioWidgets::ReadCapBox(const TSharedPtr<SEditableTextBox>& Box)
{
	if (!Box.IsValid())
	{
		return 0;
	}
	const FString Text = Box->GetText().ToString().TrimStartAndEnd();
	if (Text.IsEmpty())
	{
		return 0;
	}
	const int32 Value = FCString::Atoi(*Text);
	return Value > 0 ? Value : 0;
}

void CrowdyStudioWidgets::SetCapBox(const TSharedPtr<SEditableTextBox>& Box, int32 Value)
{
	if (Box.IsValid())
	{
		Box->SetText(Value > 0 ? FText::FromString(FString::FromInt(Value)) : FText::GetEmpty());
	}
}

#undef LOCTEXT_NAMESPACE
