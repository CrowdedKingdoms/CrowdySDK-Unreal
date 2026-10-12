// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/SCrowdyConsentPrompt.h"

#include "GenericPlatform/ICursor.h"
#include "HAL/PlatformProcess.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateTypes.h"
#include "Styling/StyleDefaults.h"
#include "Subsystem/CrowdyAuthentication.h"
#include "UI/CrowdyBrandStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CrowdyConsentPrompt"

namespace
{
	// The card is 480 wide with 24 of padding a side; the box and its gap take the rest.
	constexpr float CheckLabelWidth = 396.0f;
}

void SCrowdyConsentPrompt::Construct(const FArguments& InArgs)
{
	OnAcceptDelegate = InArgs._OnAccept;
	OnDeclineDelegate = InArgs._OnDecline;

	const ISlateStyle& Style = FCrowdyBrandStyle::Get();

	TSharedRef<SVerticalBox> Documents = SNew(SVerticalBox);
	for (const FCrowdyLegalDocument& Document : UCrowdyAuthentication::GetLegalDocuments())
	{
		Documents->AddSlot().AutoHeight().HAlign(HAlign_Left).Padding(0.0f, 3.0f)
		[
			MakeDocumentLink(Document.Title, Document.Url)
		];
	}

	const FSlateFontInfo WordmarkFont = FCoreStyle::GetDefaultFontStyle("Bold", 22);
	const TSharedRef<SWidget> LegalRow = MakeCheckRow(LOCTEXT("AgreeLegal", "I agree to the documents listed above."), &bLegalChecked);
	FirstControl = LegalRow;

	TSharedRef<SWidget> Card =
		SNew(SBorder)
		.BorderImage(Style.GetBrush("Crowdy.Card"))
		.Padding(FMargin(24.0f, 22.0f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
			[
				SNew(STextBlock).Text(LOCTEXT("WordmarkLine1", "CROWDED")).Font(WordmarkFont)
				.ColorAndOpacity(FSlateColor(FCrowdyBrandStyle::TextPrimary()))
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 2.0f, 0.0f, 20.0f)
			[
				SNew(STextBlock).Text(LOCTEXT("WordmarkLine2", "KINGDOMS")).Font(WordmarkFont)
				.ColorAndOpacity(FSlateColor(FCrowdyBrandStyle::Gold()))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(STextBlock).Text(LOCTEXT("Title", "Agree to continue"))
				.TextStyle(&Style, "Crowdy.Text.Title")
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 14.0f)
			[
				SNew(STextBlock).Text(LOCTEXT("Body", "Review the documents below and confirm both statements to continue."))
				.TextStyle(&Style, "Crowdy.Text.Body")
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 16.0f)
			[
				SNew(SBorder)
				.BorderImage(Style.GetBrush("Crowdy.Inset"))
				.Padding(FMargin(14.0f, 10.0f))
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
					[
						SNew(STextBlock).Text(LOCTEXT("DocumentsLabel", "DOCUMENTS"))
						.TextStyle(&Style, "Crowdy.Text.SectionLabel")
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						Documents
					]
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
			[
				LegalRow
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 16.0f)
			[
				MakeCheckRow(LOCTEXT("AgeOfMajority",
					"I am at least 18, or the age of majority where I live if that is higher."),
					&bAgeChecked)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[
				SNew(STextBlock)
				.Visibility_Lambda([this]() { return ErrorText.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
				.Text_Lambda([this]() { return ErrorText; })
				.TextStyle(&Style, "Crowdy.Text.Error")
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					SNullWidget::NullWidget
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 10.0f, 0.0f)
				[
					SNew(SButton)
					.ButtonStyle(&Style, "Crowdy.Button.Secondary")
					.HAlign(HAlign_Center)
					.ContentPadding(FMargin(16.0f, 8.0f))
					.IsEnabled_Lambda([this]() { return !bBusy; })
					.OnClicked_Lambda([this]() { Decline(); return FReply::Handled(); })
					[
						SNew(STextBlock).Text(LOCTEXT("Decline", "Decline"))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(FSlateColor::UseForeground())
					]
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.ButtonStyle(&Style, "Crowdy.Button.Primary")
					.HAlign(HAlign_Center)
					.ContentPadding(FMargin(22.0f, 8.0f))
					.IsEnabled_Lambda([this]() { return CanAccept(); })
					.OnClicked_Lambda([this]() { Accept(); return FReply::Handled(); })
					[
						SNew(STextBlock)
						.Text_Lambda([this]() { return bBusy ? LOCTEXT("Saving", "Saving...") : LOCTEXT("Accept", "Accept"); })
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(FSlateColor::UseForeground())
					]
				]
			]
		];

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(InArgs._bShowBackdrop ? Style.GetBrush("Crowdy.Backdrop") : FStyleDefaults::GetNoBrush())
		// Its own pointer, so it shows over a game that hides the cursor.
		.Cursor(EMouseCursor::Default)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		.Padding(FMargin(24.0f))
		[
			SNew(SBox).WidthOverride(480.0f)
			[
				Card
			]
		]
	];
}

TSharedRef<SWidget> SCrowdyConsentPrompt::MakeCheckRow(const FText& Label, bool* bState)
{
	const ISlateStyle& Style = FCrowdyBrandStyle::Get();

	return SNew(SCheckBox)
		.Style(&Style.GetWidgetStyle<FCheckBoxStyle>("Crowdy.CheckBox"))
		.IsChecked_Lambda([bState]() { return *bState ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
		.OnCheckStateChanged_Lambda([bState](ECheckBoxState State) { *bState = State == ECheckBoxState::Checked; })
		[
			// A fixed width: auto-wrap inside a check box measures the previous frame and breaks the first line early.
			SNew(STextBlock).Text(Label).TextStyle(&Style, "Crowdy.Text.Body").WrapTextAt(CheckLabelWidth)
		];
}

TSharedRef<SWidget> SCrowdyConsentPrompt::MakeDocumentLink(const FText& Title, const FString& Url)
{
	return SNew(SButton)
		.ButtonStyle(&FCrowdyBrandStyle::Get(), "Crowdy.Button.Link")
		.Cursor(EMouseCursor::Hand)
		.ToolTipText(FText::FromString(Url))
		.OnClicked_Lambda([Url]()
		{
			FPlatformProcess::LaunchURL(*Url, nullptr, nullptr);
			return FReply::Handled();
		})
		[
			SNew(STextBlock).Text(Title)
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(FSlateColor::UseForeground())
		];
}

void SCrowdyConsentPrompt::SetBusy(bool bInBusy)
{
	bBusy = bInBusy;
	if (bBusy)
	{
		ErrorText = FText::GetEmpty();
	}
}

void SCrowdyConsentPrompt::SetError(const FText& InError)
{
	ErrorText = InError;
}

void SCrowdyConsentPrompt::Accept()
{
	if (!CanAccept())
	{
		return;
	}
	OnAcceptDelegate.ExecuteIfBound();
}

void SCrowdyConsentPrompt::Decline()
{
	if (bBusy)
	{
		return;
	}
	OnDeclineDelegate.ExecuteIfBound();
}

#undef LOCTEXT_NAMESPACE
