// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/SCrowdyHomeView.h"

#include "Model/FCrowdyStudioController.h"
#include "Style/CrowdyStudioStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyle.h"
#include "Styling/StyleDefaults.h"
#include "UI/CrowdyStudioWidgets.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CrowdyStudio"

void SCrowdyHomeView::Construct(const FArguments& InArgs)
{
	Controller = InArgs._Controller;
	OnNavigate = InArgs._OnNavigate;

	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	// A small status card: icon + label, with a live, colour-coded value beneath.
	auto StatCard = [](const TCHAR* IconName, const FText& Label, TAttribute<FText> Value, TAttribute<FSlateColor> ValueColor) -> TSharedRef<SWidget>
	{
		return CrowdyStudioWidgets::Card(
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 7.0f, 0.0f)
				[ CrowdyStudioWidgets::Icon(IconName, 15.0f, FSlateColor(FCrowdyStudioStyle::TextSubtle())) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[ SNew(STextBlock).Text(Label).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.Subtle") ]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[ SNew(STextBlock).Text(Value).Font(FCoreStyle::GetDefaultFontStyle("Bold", 14)).ColorAndOpacity(ValueColor) ],
			FMargin(14.0f, 12.0f), /*bFlat*/ true);
	};

	// A quick-action button that navigates to another page.
	auto ActionButton = [this](const FText& Label, const TCHAR* IconName, int32 Page, bool bPrimary) -> TSharedRef<SWidget>
	{
		const FSlateBrush* IconBrush = FCrowdyStudioStyle::IconBrush(IconName);
		const FSlateColor Tint = bPrimary ? FSlateColor::UseForeground() : FSlateColor(FCrowdyStudioStyle::TextSecondary());
		return SNew(SButton)
			.ButtonStyle(&FCrowdyStudioStyle::Get(), bPrimary ? "Crowdy.Button.Primary" : "Crowdy.Button.Secondary")
			.ContentPadding(FMargin(12.0f, 7.0f))
			.OnClicked_Lambda([this, Page]() { OnNavigate.ExecuteIfBound(Page); return FReply::Handled(); })
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 7.0f, 0.0f)
				[ SNew(SBox).WidthOverride(15.0f).HeightOverride(15.0f)[ SNew(SImage).Image(IconBrush ? IconBrush : FStyleDefaults::GetNoBrush()).ColorAndOpacity(Tint) ] ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[ SNew(STextBlock).Text(Label).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(Tint) ]
			];
	};

	ChildSlot
	[
		SNew(SScrollBox)
		+ SScrollBox::Slot().Padding(2.0f)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight()
			[ SNew(STextBlock).Text(LOCTEXT("HomeTitle", "Overview")).TextStyle(&Style, "Crowdy.Text.Title") ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 16.0f)
			[ SNew(STextBlock).Text(LOCTEXT("HomeSubtitle", "Point your project at a Crowded Kingdoms app, then author the runtime pieces.")).TextStyle(&Style, "Crowdy.Text.Body") ]

			// Status cards (2 x 2).
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 5.0f, 0.0f)
				[
					StatCard(TEXT("login"), LOCTEXT("CardAccount", "ACCOUNT"),
						TAttribute<FText>::CreateLambda([this]() { return (Controller.IsValid() && Controller->IsSignedIn()) ? LOCTEXT("AccIn", "Signed in") : LOCTEXT("AccOut", "Not signed in"); }),
						TAttribute<FSlateColor>::CreateLambda([this]() { return FSlateColor((Controller.IsValid() && Controller->IsSignedIn()) ? FCrowdyStudioStyle::Success() : FCrowdyStudioStyle::TextSubtle()); }))
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(5.0f, 0.0f, 0.0f, 0.0f)
				[
					StatCard(TEXT("users"), LOCTEXT("CardOrg", "ORGANIZATION"),
						TAttribute<FText>::CreateLambda([this]()
						{
							const TSharedPtr<FStudioOrg> Org = Controller.IsValid() ? Controller->GetSelectedOrg() : nullptr;
							return Org.IsValid() ? FText::FromString(Org->Name) : LOCTEXT("Dash", "—");
						}),
						TAttribute<FSlateColor>::CreateLambda([this]()
						{
							const bool bHas = Controller.IsValid() && Controller->GetSelectedOrg().IsValid();
							return FSlateColor(bHas ? FCrowdyStudioStyle::TextPrimary() : FCrowdyStudioStyle::TextSubtle());
						}))
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 18.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 5.0f, 0.0f)
				[
					StatCard(TEXT("apps"), LOCTEXT("CardApp", "ACTIVE APP"),
						TAttribute<FText>::CreateLambda([this]()
						{
							const TSharedPtr<FStudioApp> App = Controller.IsValid() ? Controller->GetSelectedApp() : nullptr;
							return App.IsValid() ? FText::FromString(App->Name) : LOCTEXT("NoneSelected", "None selected");
						}),
						TAttribute<FSlateColor>::CreateLambda([this]()
						{
							const bool bHas = Controller.IsValid() && Controller->GetSelectedApp().IsValid();
							return FSlateColor(bHas ? FCrowdyStudioStyle::TextPrimary() : FCrowdyStudioStyle::TextSubtle());
						}))
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(5.0f, 0.0f, 0.0f, 0.0f)
				[
					StatCard(TEXT("config"), LOCTEXT("CardSync", "PROJECT SYNC"),
						TAttribute<FText>::CreateLambda([this]()
						{
							if (!Controller.IsValid() || !Controller->GetSelectedApp().IsValid()) { return LOCTEXT("SyncNoApp", "No app selected"); }
							const int64 Cur = Controller->GetCurrentSettings().AppId;
							if (Cur == 0) { return LOCTEXT("SyncUnconf", "Not configured"); }
							return (Cur == Controller->GetSelectedAppId()) ? LOCTEXT("SyncIn", "In sync") : LOCTEXT("SyncOut", "Out of sync");
						}),
						TAttribute<FSlateColor>::CreateLambda([this]()
						{
							if (!Controller.IsValid() || !Controller->GetSelectedApp().IsValid()) { return FSlateColor(FCrowdyStudioStyle::TextSubtle()); }
							const int64 Cur = Controller->GetCurrentSettings().AppId;
							if (Cur == 0) { return FSlateColor(FCrowdyStudioStyle::Warning()); }
							return FSlateColor((Cur == Controller->GetSelectedAppId()) ? FCrowdyStudioStyle::Success() : FCrowdyStudioStyle::Warning());
						}))
				]
			]

			// Quick actions.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("QuickActions", "Quick actions"), TEXT("wand")) ]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(8.0f, 8.0f))
				+ SWrapBox::Slot()[ ActionButton(LOCTEXT("ActWizard", "Run Setup Wizard"), TEXT("wand"), CrowdyStudioPages::Wizard, true) ]
				+ SWrapBox::Slot()[ ActionButton(LOCTEXT("ActProject", "Project"), TEXT("apps"), CrowdyStudioPages::Apps, false) ]
			]
		]
	];
}

#undef LOCTEXT_NAMESPACE
