// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/SCrowdySignInView.h"

#include "Model/FCrowdyStudioController.h"
#include "Style/CrowdyStudioStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyle.h"
#include "UI/CrowdyStudioWidgets.h"
#include "UI/SCrowdyBackendSelector.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CrowdyStudio"

void SCrowdySignInView::Construct(const FArguments& InArgs)
{
	Controller = InArgs._Controller;

	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	TSharedRef<SWidget> AuthCard = CrowdyStudioWidgets::Card(
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
		[
			CrowdyStudioWidgets::Field(LOCTEXT("EmailLabel", "Email"),
				SAssignNew(EmailBox, SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("EmailHint", "you@studio.com")))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 14.0f)
		[
			CrowdyStudioWidgets::Field(LOCTEXT("PasswordLabel", "Password"),
				SAssignNew(PasswordBox, SEditableTextBox).Style(&Style, "Crowdy.Input").IsPassword(true).HintText(LOCTEXT("PasswordHint", "Password")))
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SButton)
			.ButtonStyle(&Style, "Crowdy.Button.Primary")
			.HAlign(HAlign_Center)
			.ContentPadding(FMargin(13.0f, 8.0f))
			.OnClicked(this, &SCrowdySignInView::OnLoginClicked)
			[ SNew(STextBlock).Text(LOCTEXT("LogIn", "Log In")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 11)).ColorAndOpacity(FSlateColor::UseForeground()) ]
		]
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 10.0f, 0.0f, 0.0f)
		[
			SNew(SButton)
			.ButtonStyle(&Style, "Crowdy.Button.Ghost")
			.ContentPadding(FMargin(6.0f, 3.0f))
			.OnClicked_Lambda([this]() { bShowToken = !bShowToken; return FReply::Handled(); })
			[
				SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
				.ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::Info()))
				.Text_Lambda([this]() { return bShowToken ? LOCTEXT("HideToken", "Hide organization token") : LOCTEXT("UseToken", "Use an organization token instead"); })
			]
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SBox)
			.Padding(0.0f, 8.0f, 0.0f, 0.0f)
			.Visibility_Lambda([this]() { return bShowToken ? EVisibility::Visible : EVisibility::Collapsed; })
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
				[
					CrowdyStudioWidgets::Field(LOCTEXT("OrgTokenLabel", "Organization token"),
						SAssignNew(TokenBox, SEditableTextBox).Style(&Style, "Crowdy.Input").IsPassword(true).HintText(LOCTEXT("OrgTokenHint", "Paste a manage_* org token")),
						LOCTEXT("OrgTokenHelp", "Management-only access. Email sign-in is required for game-plane authoring (teams, channels, grid, game model)."))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SButton)
					.ButtonStyle(&Style, "Crowdy.Button.Secondary")
					.HAlign(HAlign_Center)
					.ContentPadding(FMargin(13.0f, 7.0f))
					.OnClicked(this, &SCrowdySignInView::OnSignInWithTokenClicked)
					[ SNew(STextBlock).Text(LOCTEXT("SignInTokenButton", "Sign In with Token")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(FSlateColor::UseForeground()) ]
				]
			]
		],
		FMargin(18.0f, 16.0f));

	ChildSlot
	[
		SNew(SScrollBox)
		+ SScrollBox::Slot().HAlign(HAlign_Center).Padding(0.0f, 36.0f, 0.0f, 26.0f)
		[
			SNew(SBox).WidthOverride(440.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 0.0f, 0.0f, 6.0f)
				[ CrowdyStudioWidgets::BrandMark(34, /*bWithCrown*/ false, HAlign_Center) ]
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 0.0f, 0.0f, 24.0f)
				[
					SNew(STextBlock).Text(LOCTEXT("HeroSubtitle", "STUDIO  CONSOLE"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
					.ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSubtle()))
				]
				// Backend picker first: a fresh project has no settings, so you choose which Crowdy
				// backend to authenticate against before signing in.
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 14.0f)
				[ SNew(SCrowdyBackendSelector).Controller(Controller) ]
				+ SVerticalBox::Slot().AutoHeight()
				[ AuthCard ]
			]
		]
	];
}

FReply SCrowdySignInView::OnSignInWithTokenClicked()
{
	if (Controller.IsValid() && TokenBox.IsValid())
	{
		Controller->SignInWithToken(TokenBox->GetText().ToString());
	}
	return FReply::Handled();
}

FReply SCrowdySignInView::OnLoginClicked()
{
	if (Controller.IsValid() && EmailBox.IsValid() && PasswordBox.IsValid())
	{
		Controller->LoginWithEmail(EmailBox->GetText().ToString(), PasswordBox->GetText().ToString());
	}
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
