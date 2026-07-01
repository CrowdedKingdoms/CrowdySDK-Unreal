// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/SCrowdySignInView.h"

#include "Model/FCrowdyStudioController.h"
#include "Style/CrowdyStudioStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyle.h"
#include "UI/CrowdyStudioWidgets.h"
#include "UI/SCrowdyBackendSelector.h"
#include "Widgets/Colors/SColorBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CrowdyStudio"

namespace
{
	// The server exposes a fake "mock" OAuth provider only when it runs with DEV_AUTH_BYPASS, so a
	// developer can exercise the whole browser sign-in flow without a real account. It never appears on
	// a production backend. We label and describe it clearly so it isn't mistaken for a real provider.
	bool IsMockProvider(const FString& Provider)
	{
		return Provider.ToLower().Contains(TEXT("mock"));
	}

	// Human-facing name for a provider key. A short allow-list gets the brand casing right ("github" ->
	// "GitHub"); anything else just gets its first letter capitalised.
	FString PrettyProviderName(const FString& Provider)
	{
		const FString P = Provider.ToLower();
		if (P.Contains(TEXT("google")))    { return TEXT("Google"); }
		if (P.Contains(TEXT("github")))    { return TEXT("GitHub"); }
		if (P.Contains(TEXT("discord")))   { return TEXT("Discord"); }
		if (P.Contains(TEXT("apple")))     { return TEXT("Apple"); }
		if (P.Contains(TEXT("microsoft")) || P.Contains(TEXT("azure"))) { return TEXT("Microsoft"); }
		if (IsMockProvider(Provider))      { return TEXT("Mock (dev test)"); }

		FString Pretty = Provider;
		if (Pretty.Len() > 0)
		{
			Pretty[0] = FChar::ToUpper(Pretty[0]);
		}
		return Pretty;
	}

	// Registered monochrome glyph for a provider, tinted at the call site. The mock provider gets the
	// wand glyph so it reads as a test tool; unknown providers fall back to a generic key so an
	// unexpected provider still renders a button, never a blank.
	const TCHAR* ProviderIconName(const FString& Provider)
	{
		const FString P = Provider.ToLower();
		if (P.Contains(TEXT("google")))    { return TEXT("google"); }
		if (P.Contains(TEXT("github")))    { return TEXT("github"); }
		if (P.Contains(TEXT("discord")))   { return TEXT("discord"); }
		if (P.Contains(TEXT("apple")))     { return TEXT("apple"); }
		if (P.Contains(TEXT("microsoft")) || P.Contains(TEXT("azure"))) { return TEXT("microsoft"); }
		if (IsMockProvider(Provider))      { return TEXT("wand"); }
		return TEXT("key");
	}

	// Hover help for a provider button, so the mock/test provider is self-explanatory.
	FText ProviderTooltip(const FString& Provider)
	{
		if (IsMockProvider(Provider))
		{
			return NSLOCTEXT("CrowdyStudio", "MockProviderTip",
				"Dev-only test provider (the server is running with DEV_AUTH_BYPASS). It signs you in through the browser without a real account so you can exercise the social flow. It will not appear on a production backend.");
		}
		return FText::Format(NSLOCTEXT("CrowdyStudio", "ProviderTip", "Sign in with {0} in your web browser."),
			FText::FromString(PrettyProviderName(Provider)));
	}

	// A centred "or continue with email" divider: a hairline, the label, another hairline. Separates the
	// federated providers above from the email card below.
	TSharedRef<SWidget> MakeOrDivider(const FText& Label)
	{
		auto Hairline = []()
		{
			return SNew(SBox).HeightOverride(1.0f)
			[
				SNew(SColorBlock).Color(FCrowdyStudioStyle::Line())
			];
		};

		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)[ Hairline() ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10.0f, 0.0f)
			[
				SNew(STextBlock).Text(Label).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.Subtle")
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)[ Hairline() ];
	}
}

void SCrowdySignInView::Construct(const FArguments& InArgs)
{
	Controller = InArgs._Controller;

	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	// Federated providers (built dynamically) + an "or continue with email" divider, shown only when the
	// server enables at least one provider so the card stays uncluttered on a password-only backend.
	TSharedRef<SWidget> ProvidersSection =
		SNew(SBox)
		.Visibility_Lambda([this]()
		{
			return (Controller.IsValid() && Controller->GetLoginProviders().Num() > 0)
				? EVisibility::Visible : EVisibility::Collapsed;
		})
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[ SAssignNew(ProvidersBox, SVerticalBox) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 16.0f, 0.0f, 16.0f)
			[ MakeOrDivider(LOCTEXT("OrEmail", "or continue with email")) ]
		];

	TSharedRef<SWidget> AuthCard = CrowdyStudioWidgets::Card(
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[ ProvidersSection ]
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
		// Passwordless email link: a full-weight alternative to the password, reusing the email box above.
		// On a production server it emails a one-time link and waits; on a dev server the server returns a
		// token that signs you in immediately (no email).
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 10.0f, 0.0f, 0.0f)
		[
			SNew(SButton)
			.ButtonStyle(&Style, "Crowdy.Button.Secondary")
			.HAlign(HAlign_Center)
			.ContentPadding(FMargin(13.0f, 8.0f))
			.ToolTipText(LOCTEXT("MagicTip", "Enter your email above, then this emails a one-time sign-in link — no password needed. On a dev server (DEV_AUTH_BYPASS) it skips the email and signs you in instantly."))
			.OnClicked(this, &SCrowdySignInView::OnMagicLinkClicked)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 9.0f, 0.0f)
				[ CrowdyStudioWidgets::Icon(TEXT("mail"), 16.0f, FSlateColor(FCrowdyStudioStyle::TextPrimary())) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[ SNew(STextBlock).Text(LOCTEXT("MagicLink", "Email me a sign-in link")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(FSlateColor::UseForeground()) ]
			]
		]
		// Dev sign-in: developer-only (needs the server DEV_AUTH_BYPASS), so it stays below the real
		// options — readable, but clearly the lightest of the buttons.
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			SNew(SButton)
			.ButtonStyle(&Style, "Crowdy.Button.Ghost")
			.HAlign(HAlign_Center)
			.ContentPadding(FMargin(13.0f, 7.0f))
			.ToolTipText(LOCTEXT("DevSignInTip", "Passwordless sign-in using only the email above, with no browser step. Only works against a dev server (DEV_AUTH_BYPASS); a production server rejects it."))
			.OnClicked(this, &SCrowdySignInView::OnDevLoginClicked)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 9.0f, 0.0f)
				[ CrowdyStudioWidgets::Icon(TEXT("wand"), 15.0f, FSlateColor(FCrowdyStudioStyle::TextSecondary())) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("DevSignIn", "Dev sign-in (dev servers only)"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
					.ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSecondary()))
				]
			]
		]
		// Organization token: management-only (cannot mint / author), tucked behind a toggle.
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 14.0f, 0.0f, 0.0f)
		[
			SNew(SButton)
			.ButtonStyle(&Style, "Crowdy.Button.Ghost")
			.ContentPadding(FMargin(8.0f, 4.0f))
			.OnClicked_Lambda([this]() { bShowToken = !bShowToken; return FReply::Handled(); })
			[
				SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
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
						LOCTEXT("OrgTokenHelp", "Management-only access. A session sign-in (email, provider, or link) is required for game-plane authoring (teams, channels, grid, game model)."))
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

	if (Controller.IsValid())
	{
		// Rebuild the buttons when the provider list arrives; re-probe when the backend changes. AddSP
		// auto-detaches both when this widget dies, so no manual cleanup is needed.
		Controller->OnLoginProvidersChanged.AddSP(this, &SCrowdySignInView::RebuildProviders);
		Controller->OnConfigChanged.AddSP(this, &SCrowdySignInView::HandleBackendChanged);

		RebuildProviders();
		Controller->FetchAvailableProviders();
	}
}

void SCrowdySignInView::HandleBackendChanged()
{
	// The buttons only matter while signed out; skip the probe otherwise (config also changes during a
	// signed-in session, e.g. a config sync).
	if (Controller.IsValid() && !Controller->IsSignedIn())
	{
		Controller->FetchAvailableProviders();
	}
}

void SCrowdySignInView::RebuildProviders()
{
	if (!ProvidersBox.IsValid() || !Controller.IsValid())
	{
		return;
	}

	ProvidersBox->ClearChildren();

	const TArray<FString>& Providers = Controller->GetLoginProviders();
	for (int32 Index = 0; Index < Providers.Num(); ++Index)
	{
		ProvidersBox->AddSlot().AutoHeight().Padding(0.0f, Index == 0 ? 0.0f : 8.0f, 0.0f, 0.0f)
		[
			MakeProviderButton(Providers[Index])
		];
	}
}

TSharedRef<SWidget> SCrowdySignInView::MakeProviderButton(const FString& Provider)
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	return SNew(SButton)
		.ButtonStyle(&Style, "Crowdy.Button.Secondary")
		.HAlign(HAlign_Center)
		.ContentPadding(FMargin(13.0f, 8.0f))
		.ToolTipText(ProviderTooltip(Provider))
		.OnClicked_Lambda([this, Provider]()
		{
			if (Controller.IsValid())
			{
				Controller->SignInWithSocial(Provider);
			}
			return FReply::Handled();
		})
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 10.0f, 0.0f)
			[ CrowdyStudioWidgets::Icon(ProviderIconName(Provider), 16.0f, FSlateColor(FCrowdyStudioStyle::TextPrimary())) ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::Format(LOCTEXT("ContinueWith", "Continue with {0}"), FText::FromString(PrettyProviderName(Provider))))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
				.ColorAndOpacity(FSlateColor::UseForeground())
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

FReply SCrowdySignInView::OnMagicLinkClicked()
{
	// Reuses the email box; the password is ignored. The controller surfaces an empty-email error and any
	// server error through the normal status path.
	if (Controller.IsValid() && EmailBox.IsValid())
	{
		Controller->SignInWithMagicLink(EmailBox->GetText().ToString());
	}
	return FReply::Handled();
}

FReply SCrowdySignInView::OnDevLoginClicked()
{
	// Reuses the same email box, no password. The controller surfaces an empty-email error and any
	// FORBIDDEN from a non-dev server through the normal status path.
	if (Controller.IsValid() && EmailBox.IsValid())
	{
		Controller->SignInWithDevLogin(EmailBox->GetText().ToString());
	}
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
