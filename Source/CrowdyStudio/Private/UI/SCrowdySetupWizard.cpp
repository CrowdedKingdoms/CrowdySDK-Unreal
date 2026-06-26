// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/SCrowdySetupWizard.h"

#include "Model/FCrowdyStudioController.h"
#include "Style/CrowdyStudioStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyle.h"
#include "Styling/StyleDefaults.h"
#include "UI/CrowdyStudioWidgets.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CrowdyStudio"

void SCrowdySetupWizard::Construct(const FArguments& InArgs)
{
	Controller = InArgs._Controller;
	OnNavigate = InArgs._OnNavigate;

	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	ChildSlot
	[
		SNew(SScrollBox)
		+ SScrollBox::Slot().HAlign(HAlign_Center).Padding(0.0f, 8.0f, 0.0f, 24.0f)
		[
			SNew(SBox).WidthOverride(560.0f)
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot().AutoHeight()
				[ SNew(STextBlock).Text(LOCTEXT("WizTitle", "Set up your project")).TextStyle(&Style, "Crowdy.Text.Title") ]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 2.0f)
				[ SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Body")
					.Text(LOCTEXT("WizSubtitle", "Four steps connect this project to a Crowded Kingdoms app. Each opens the matching page.")) ]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 16.0f)
				[
					SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle")
					.Text_Lambda([this]()
					{
						int32 Done = 0;
						if (Controller.IsValid())
						{
							if (Controller->IsSignedIn()) { ++Done; }
							if (Controller->GetSelectedApp().IsValid()) { ++Done; }
							if (!Controller->GetSelectedEnvironmentSlug().IsEmpty()) { ++Done; }
							if (Controller->GetSelectedApp().IsValid() && Controller->GetCurrentSettings().AppId != 0
								&& Controller->GetCurrentSettings().AppId == Controller->GetSelectedAppId()) { ++Done; }
						}
						return FText::Format(LOCTEXT("WizProgress", "{0} of 4 steps complete"), FText::AsNumber(Done));
					})
				]

				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
				[
					MakeStep(1, LOCTEXT("Step1Title", "Sign in"), LOCTEXT("Step1Desc", "Sign in with email and password (or an organization token) to reach your apps."),
						[this]() { return Controller.IsValid() && Controller->IsSignedIn(); },
						CrowdyStudioPages::SignIn, LOCTEXT("Step1Cta", "Sign In"))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
				[
					MakeStep(2, LOCTEXT("Step2Title", "Choose an app"), LOCTEXT("Step2Desc", "Pick the app this project should connect to, or create a new one."),
						[this]() { return Controller.IsValid() && Controller->GetSelectedApp().IsValid(); },
						CrowdyStudioPages::Apps, LOCTEXT("Step2Cta", "Open Project"))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
				[
					MakeStep(3, LOCTEXT("Step3Title", "Link a game server"), LOCTEXT("Step3Desc", "Choose the game server (environment) the app runs on and link them (Optional if not linked), in the Project page's Game server section."),
						[this]() { return Controller.IsValid() && !Controller->GetSelectedEnvironmentSlug().IsEmpty(); },
						CrowdyStudioPages::Apps, LOCTEXT("Step3Cta", "Open Project"))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					MakeStep(4, LOCTEXT("Step4Title", "Sync to project"), LOCTEXT("Step4Desc", "Review the app's identifiers and endpoints and write them into the project settings (on the Project page)."),
						[this]() { return Controller.IsValid() && Controller->GetSelectedApp().IsValid() && Controller->GetCurrentSettings().AppId != 0
							&& Controller->GetCurrentSettings().AppId == Controller->GetSelectedAppId(); },
						CrowdyStudioPages::Apps, LOCTEXT("Step4Cta", "Review & Sync"))
				]
			]
		]
	];
}

TSharedRef<SWidget> SCrowdySetupWizard::MakeStep(int32 Number, const FText& Title, const FText& Description,
	TFunction<bool()> IsDone, int32 NavPage, const FText& Cta)
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	return CrowdyStudioWidgets::Card(
		SNew(SHorizontalBox)

		// Step indicator: a number that becomes a check when complete.
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 14.0f, 0.0f)
		[
			SNew(SBox).WidthOverride(28.0f).HeightOverride(28.0f)
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SNew(SImage).Image(Style.GetBrush("Crowdy.Pill"))
					.ColorAndOpacity_Lambda([IsDone]() -> FSlateColor
					{
						FLinearColor C = IsDone() ? FCrowdyStudioStyle::Success() : FCrowdyStudioStyle::TextSubtle();
						C.A = 0.16f;
						return FSlateColor(C);
					})
				]
				+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
				[
					SNew(SBox).Visibility_Lambda([IsDone]() { return IsDone() ? EVisibility::Collapsed : EVisibility::Visible; })
					[
						SNew(STextBlock).Text(FText::AsNumber(Number)).Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
						.ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSecondary()))
					]
				]
				+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
				[
					SNew(SBox).Visibility_Lambda([IsDone]() { return IsDone() ? EVisibility::Visible : EVisibility::Collapsed; })
					[
						CrowdyStudioWidgets::Icon(TEXT("check"), 15.0f, FSlateColor(FCrowdyStudioStyle::Success()))
					]
				]
			]
		]

		// Title + description.
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[ SNew(STextBlock).Text(Title).TextStyle(&Style, "Crowdy.Text.BodyStrong") ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
			[ SNew(STextBlock).Text(Description).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Subtle") ]
		]

		// Status badge.
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10.0f, 0.0f, 10.0f, 0.0f)
		[
			SNew(SBorder).BorderImage(Style.GetBrush("Crowdy.Pill"))
			.BorderBackgroundColor_Lambda([IsDone]() -> FSlateColor
			{
				FLinearColor C = IsDone() ? FCrowdyStudioStyle::Success() : FCrowdyStudioStyle::TextSecondary();
				C.A = 0.16f;
				return FSlateColor(C);
			})
			.Padding(FMargin(8.0f, 2.0f))
			[
				SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
				.Text_Lambda([IsDone]() { return IsDone() ? LOCTEXT("StepDone", "DONE") : LOCTEXT("StepTodo", "TO DO"); })
				.ColorAndOpacity_Lambda([IsDone]() { return FSlateColor(IsDone() ? FCrowdyStudioStyle::Success() : FCrowdyStudioStyle::TextSecondary()); })
			]
		]

		// Go button.
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SButton)
			.ButtonStyle(&Style, "Crowdy.Button.Secondary")
			.ContentPadding(FMargin(12.0f, 6.0f))
			.OnClicked_Lambda([this, NavPage]() { OnNavigate.ExecuteIfBound(NavPage); return FReply::Handled(); })
			[
				SNew(STextBlock).Text(Cta).Font(FCoreStyle::GetDefaultFontStyle("Bold", 9)).ColorAndOpacity(FSlateColor::UseForeground())
			]
		],
		FMargin(14.0f, 12.0f));
}

#undef LOCTEXT_NAMESPACE
