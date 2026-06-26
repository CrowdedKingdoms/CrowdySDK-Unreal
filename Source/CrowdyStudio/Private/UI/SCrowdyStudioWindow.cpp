// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/SCrowdyStudioWindow.h"

#include "Framework/Notifications/NotificationManager.h"
#include "Model/FCrowdyStudioController.h"
#include "Style/CrowdyStudioStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyle.h"
#include "Styling/StyleDefaults.h"
#include "UI/CrowdyStudioPages.h"
#include "UI/CrowdyStudioWidgets.h"
#include "UI/SCrowdyProjectView.h"
#include "UI/SCrowdyChannelsView.h"
#include "UI/SCrowdyGameModelView.h"
#include "UI/SCrowdyGridView.h"
#include "UI/SCrowdyHomeView.h"
#include "UI/SCrowdyInspectorView.h"
#include "UI/SCrowdyLogoIntro.h"
#include "UI/SCrowdyRegistryInspector.h"
#include "UI/SCrowdySetupWizard.h"
#include "UI/SCrowdySignInView.h"
#include "UI/SCrowdyTeamsView.h"
#include "UI/SCrowdyWebView.h"
#include "Web/FCrowdyStudioLinks.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CrowdyStudio"

void SCrowdyStudioWindow::Construct(const FArguments& InArgs)
{
	Controller = MakeShared<FCrowdyStudioController>();
	PageFade = PageAnim.AddCurve(0.0f, 0.20f, ECurveEaseFunction::CubicOut);

	// The single web button lands on the selected org's overview (/orgs/{slug}). Fall back to the
	// only/first org, then to the dashboard when nothing is loaded yet. Set before BuildNavRail runs.
	WebOverviewLink = [this]() -> FString
	{
		FString Slug;
		if (const TSharedPtr<FStudioOrg> Org = Controller->GetSelectedOrg())
		{
			Slug = Org->Slug;
		}
		else if (Controller->GetOrganizations().Num() > 0 && Controller->GetOrganizations()[0].IsValid())
		{
			Slug = Controller->GetOrganizations()[0]->Slug;
		}
		return Slug.IsEmpty() ? FCrowdyStudioLinks::Dashboard() : FCrowdyStudioLinks::OrgTab(Slug, TEXT("overview"));
	};

	TSharedRef<SWidget> Body =
		SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight()[ BuildHeader() ]
		+ SVerticalBox::Slot().AutoHeight()[ MakeHairline() ]

		+ SVerticalBox::Slot().FillHeight(1.0f)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().AutoWidth()[ BuildNavRail() ]

			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SBox).WidthOverride(1.0f)
				[ SNew(SImage).Image(FCrowdyStudioStyle::Get().GetBrush("Crowdy.Separator")) ]
			]

			// Page area: cross-fades content in on every page change.
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				SNew(SBorder)
				.BorderImage(FStyleDefaults::GetNoBrush())
				.Padding(FMargin(18.0f, 16.0f))
				.ColorAndOpacity_Lambda([this]() -> FLinearColor
				{
					const float A = PageAnim.IsPlaying() ? PageFade.GetLerp() : 1.0f;
					return FLinearColor(1.0f, 1.0f, 1.0f, A);
				})
				[
					SAssignNew(PageSwitcher, SWidgetSwitcher)

					+ SWidgetSwitcher::Slot()[ SNew(SCrowdySignInView).Controller(Controller) ]
					+ SWidgetSwitcher::Slot()[ SNew(SCrowdySetupWizard).Controller(Controller).OnNavigate(FOnStudioNavigate::CreateSP(this, &SCrowdyStudioWindow::ShowPage)) ]
					+ SWidgetSwitcher::Slot()[ SNew(SCrowdyHomeView).Controller(Controller).OnNavigate(FOnStudioNavigate::CreateSP(this, &SCrowdyStudioWindow::ShowPage)) ]
					+ SWidgetSwitcher::Slot()[ SNew(SCrowdyProjectView).Controller(Controller) ]
					+ SWidgetSwitcher::Slot()[ SNew(SCrowdyTeamsView).Controller(Controller) ]
					+ SWidgetSwitcher::Slot()[ SNew(SCrowdyChannelsView).Controller(Controller) ]
					+ SWidgetSwitcher::Slot()[ SNew(SCrowdyGridView).Controller(Controller) ]
					+ SWidgetSwitcher::Slot()[ SNew(SCrowdyGameModelView).Controller(Controller) ]
					+ SWidgetSwitcher::Slot()[ SNew(SCrowdyInspectorView) ]
					+ SWidgetSwitcher::Slot()[ SNew(SCrowdyRegistryInspector) ]
					+ SWidgetSwitcher::Slot()[ SAssignNew(WebView, SCrowdyWebView).InitialUrl(FCrowdyStudioLinks::Dashboard()) ]
				]
			]
		]

		+ SVerticalBox::Slot().AutoHeight()[ MakeHairline() ]
		+ SVerticalBox::Slot().AutoHeight()[ BuildStatusBar() ];

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FCrowdyStudioStyle::Get().GetBrush("Crowdy.Panel"))
		.Padding(0.0f)
		[
			SNew(SOverlay)
			+ SOverlay::Slot()[ Body ]
			+ SOverlay::Slot()[ SAssignNew(LogoIntro, SCrowdyLogoIntro) ]
		]
	];

	// Default to Sign In; HandleSignInStateChanged promotes to Home once a token validates.
	if (PageSwitcher.IsValid())
	{
		PageSwitcher->SetActiveWidgetIndex(CrowdyStudioPages::SignIn);
		ActiveIndex = CrowdyStudioPages::SignIn;
	}

	Controller->OnSignInStateChanged.AddSP(this, &SCrowdyStudioWindow::HandleSignInStateChanged);
	Controller->OnStatusMessage.AddSP(this, &SCrowdyStudioWindow::HandleStatusMessage);
	Controller->Initialize();
}

TSharedRef<SWidget> SCrowdyStudioWindow::MakeHairline()
{
	return SNew(SBox).HeightOverride(1.0f)
		[ SNew(SImage).Image(FCrowdyStudioStyle::Get().GetBrush("Crowdy.Separator")) ];
}

TSharedRef<SWidget> SCrowdyStudioWindow::BuildHeader()
{
	return SNew(SBox).Padding(FMargin(16.0f, 11.0f))
	[
		SNew(SHorizontalBox)

		// Brand: two-tone wordmark (text only, no emblem).
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(STextBlock).Text(LOCTEXT("BrandHdr1", "CROWDED"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 13))
				.ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextPrimary()))
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(5.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock).Text(LOCTEXT("BrandHdr2", "KINGDOMS"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 13))
				.ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::Gold()))
			]
		]

		+ SHorizontalBox::Slot().FillWidth(1.0f)[ SNullWidget::NullWidget ]

		// Signed-in identity chip.
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SBorder)
			.BorderImage(FCrowdyStudioStyle::Get().GetBrush("Crowdy.Card"))
			.Padding(FMargin(10.0f, 5.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(SBox).WidthOverride(8.0f).HeightOverride(8.0f)
					[
						SNew(SImage)
						.Image(FCrowdyStudioStyle::Get().GetBrush("Crowdy.Pill"))
						.ColorAndOpacity_Lambda([this]()
						{
							return FSlateColor((Controller.IsValid() && Controller->IsSignedIn())
								? FCrowdyStudioStyle::Success() : FCrowdyStudioStyle::TextSubtle());
						})
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
					.ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSecondary()))
					.Text_Lambda([this]() -> FText
					{
						if (!Controller.IsValid() || !Controller->IsSignedIn())
						{
							return LOCTEXT("HdrNotSignedIn", "Not signed in");
						}
						const FString Org = Controller->GetSelectedOrg().IsValid() ? Controller->GetSelectedOrg()->Name : FString();
						const FString App = Controller->GetSelectedApp().IsValid() ? Controller->GetSelectedApp()->Name : FString();
						if (!Org.IsEmpty() && !App.IsEmpty())
						{
							return FText::FromString(FString::Printf(TEXT("%s  ·  %s"), *Org, *App));
						}
						if (!Org.IsEmpty())
						{
							return FText::FromString(Org);
						}
						return LOCTEXT("HdrSignedIn", "Signed in");
					})
				]
			]
		]

		// Auth button: lives in the header so sign in / sign out is always reachable.
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SButton)
			.ButtonStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Button.Secondary")
			.ContentPadding(FMargin(12.0f, 5.0f))
			.OnClicked_Lambda([this]()
			{
				if (Controller.IsValid())
				{
					if (Controller->IsSignedIn()) { Controller->SignOut(); }
					else { ShowPage(CrowdyStudioPages::SignIn); }
				}
				return FReply::Handled();
			})
			[
				SNew(STextBlock)
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
				.ColorAndOpacity(FSlateColor::UseForeground())
				.Text_Lambda([this]()
				{
					return (Controller.IsValid() && Controller->IsSignedIn()) ? LOCTEXT("HdrSignOut", "Sign Out") : LOCTEXT("HdrSignIn", "Sign In");
				})
			]
		]
	];
}

TSharedRef<SWidget> SCrowdyStudioWindow::BuildNavRail()
{
	auto GroupSlot = [](TSharedRef<SVerticalBox> Box, const FText& Label)
	{
		Box->AddSlot().AutoHeight().Padding(6.0f, 13.0f, 6.0f, 5.0f)[ CrowdyStudioWidgets::GroupLabel(Label) ];
	};

	TSharedRef<SVerticalBox> Nav = SNew(SVerticalBox);

	// Guided setup first, then the overview.
	Nav->AddSlot().AutoHeight().Padding(0.0f, 1.0f)[ MakeNavButton(LOCTEXT("NavWizard", "Setup Wizard"), TEXT("wand"), CrowdyStudioPages::Wizard, false) ];
	Nav->AddSlot().AutoHeight().Padding(0.0f, 1.0f)[ MakeNavButton(LOCTEXT("NavHome", "Home"), TEXT("home"), CrowdyStudioPages::Home, true) ];

	GroupSlot(Nav, LOCTEXT("NavGrpConfigure", "CONFIGURE"));
	Nav->AddSlot().AutoHeight().Padding(0.0f, 1.0f)[ MakeNavButton(LOCTEXT("NavProject", "Project"), TEXT("apps"), CrowdyStudioPages::Apps, true) ];

	GroupSlot(Nav, LOCTEXT("NavGrpAuthoring", "AUTHORING"));
	Nav->AddSlot().AutoHeight().Padding(0.0f, 1.0f)[ MakeNavButton(LOCTEXT("NavTeams", "Teams"), TEXT("users"), CrowdyStudioPages::Teams, true) ];
	Nav->AddSlot().AutoHeight().Padding(0.0f, 1.0f)[ MakeNavButton(LOCTEXT("NavChannels", "Channels"), TEXT("broadcast"), CrowdyStudioPages::Channels, true) ];
	Nav->AddSlot().AutoHeight().Padding(0.0f, 1.0f)[ MakeNavButton(LOCTEXT("NavGrid", "Grid"), TEXT("grid"), CrowdyStudioPages::Grid, true) ];
	Nav->AddSlot().AutoHeight().Padding(0.0f, 1.0f)[ MakeNavButton(LOCTEXT("NavGameModel", "Game Model"), TEXT("cube"), CrowdyStudioPages::GameModel, true) ];

	GroupSlot(Nav, LOCTEXT("NavGrpDebug", "DEBUG"));
	Nav->AddSlot().AutoHeight().Padding(0.0f, 1.0f)[ MakeNavButton(LOCTEXT("NavInspector", "Inspector"), TEXT("inspector"), CrowdyStudioPages::Inspector, false) ];
	Nav->AddSlot().AutoHeight().Padding(0.0f, 1.0f)[ MakeNavButton(LOCTEXT("NavRegistry", "Registry"), TEXT("config"), CrowdyStudioPages::Registry, false) ];

	return SNew(SBox).WidthOverride(200.0f)
	[
		SNew(SBorder)
		.BorderImage(FCrowdyStudioStyle::Get().GetBrush("Crowdy.Rail"))
		.Padding(FMargin(10.0f, 12.0f))
		[
			SNew(SVerticalBox)
			// Scrollable nav items so nothing is lost when the window is short.
			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()[ Nav ]
			]
			// Web console pinned at the bottom.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[ MakeWebNavButton(LOCTEXT("NavConsole", "Web Console"), TEXT("external-link"), WebOverviewLink) ]
		]
	];
}

TSharedRef<SWidget> SCrowdyStudioWindow::BuildStatusBar()
{
	auto BusyVis = [this]()
	{
		return (Controller.IsValid() && Controller->IsBusy()) ? EVisibility::Visible : EVisibility::Collapsed;
	};

	return SNew(SBox).Padding(FMargin(16.0f, 6.0f))
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
			.Text_Lambda([this]() { return Controller.IsValid() ? FText::FromString(Controller->GetStatusMessage()) : FText::GetEmpty(); })
			.ColorAndOpacity_Lambda([this]()
			{
				return (Controller.IsValid() && Controller->LastStatusWasError())
					? FSlateColor(FCrowdyStudioStyle::Danger()) : FSlateColor(FCrowdyStudioStyle::TextSecondary());
			})
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[ SNew(SCircularThrobber).Radius(8.0f).Visibility_Lambda(BusyVis) ]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text(LOCTEXT("Working", "Working…"))
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
			.ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSubtle()))
			.Visibility_Lambda(BusyVis)
		]
	];
}

TSharedRef<SWidget> SCrowdyStudioWindow::MakeNavButton(const FText& Label, const TCHAR* IconName, int32 PageIndex, bool bRequiresSignIn)
{
	const FSlateBrush* IconBrush = FCrowdyStudioStyle::IconBrush(IconName);

	auto Enabled = [this, bRequiresSignIn]()
	{
		return !bRequiresSignIn || (Controller.IsValid() && Controller->IsSignedIn());
	};
	auto IconTint = [this, PageIndex, bRequiresSignIn]() -> FSlateColor
	{
		if (bRequiresSignIn && !(Controller.IsValid() && Controller->IsSignedIn()))
		{
			return FSlateColor(FCrowdyStudioStyle::TextSubtle());
		}
		return FSlateColor(ActiveIndex == PageIndex ? FCrowdyStudioStyle::Gold() : FCrowdyStudioStyle::TextSecondary());
	};
	auto TextTint = [this, PageIndex, bRequiresSignIn]() -> FSlateColor
	{
		if (bRequiresSignIn && !(Controller.IsValid() && Controller->IsSignedIn()))
		{
			return FSlateColor(FCrowdyStudioStyle::TextSubtle());
		}
		return FSlateColor(ActiveIndex == PageIndex ? FCrowdyStudioStyle::TextPrimary() : FCrowdyStudioStyle::TextSecondary());
	};

	return SNew(SBorder)
		.BorderImage_Lambda([this, PageIndex]() -> const FSlateBrush*
		{
			return ActiveIndex == PageIndex
				? FCrowdyStudioStyle::Get().GetBrush("Crowdy.Nav.Active")
				: FStyleDefaults::GetNoBrush();
		})
		.Padding(0.0f)
		[
			SNew(SButton)
			.ButtonStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Button.Nav")
			.ContentPadding(FMargin(10.0f, 7.0f))
			.HAlign(HAlign_Fill)
			.IsEnabled_Lambda(Enabled)
			.OnClicked_Lambda([this, PageIndex]() { ShowPage(PageIndex); return FReply::Handled(); })
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 10.0f, 0.0f)
				[
					SNew(SBox).WidthOverride(18.0f).HeightOverride(18.0f)
					[
						SNew(SImage)
						.Image(IconBrush ? IconBrush : FStyleDefaults::GetNoBrush())
						.ColorAndOpacity_Lambda(IconTint)
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(Label)
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
					.ColorAndOpacity_Lambda(TextTint)
				]
			]
		];
}

TSharedRef<SWidget> SCrowdyStudioWindow::MakeWebNavButton(const FText& Label, const TCHAR* IconName, TFunction<FString()> UrlGetter)
{
	const FSlateBrush* IconBrush = FCrowdyStudioStyle::IconBrush(IconName);

	return SNew(SButton)
		.ButtonStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Button.Secondary")
		.ContentPadding(FMargin(10.0f, 7.0f))
		.HAlign(HAlign_Fill)
		.IsEnabled_Lambda([this]() { return Controller.IsValid() && Controller->IsSignedIn(); })
		.OnClicked_Lambda([this, UrlGetter]()
		{
			if (PageSwitcher.IsValid() && WebView.IsValid())
			{
				ActiveIndex = CrowdyStudioPages::WebConsole;
				PageSwitcher->SetActiveWidgetIndex(CrowdyStudioPages::WebConsole);
				WebView->LoadUrl(UrlGetter(), Controller->GetAuthToken());
				PageAnim.Play(SharedThis(this));
			}
			return FReply::Handled();
		})
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(SBox).WidthOverride(16.0f).HeightOverride(16.0f)
				[
					SNew(SImage)
					.Image(IconBrush ? IconBrush : FStyleDefaults::GetNoBrush())
					.ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSecondary()))
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(Label)
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
				.ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSecondary()))
			]
		];
}

void SCrowdyStudioWindow::ShowPage(int32 PageIndex)
{
	if (!PageSwitcher.IsValid())
	{
		return;
	}
	ActiveIndex = PageIndex;
	PageSwitcher->SetActiveWidgetIndex(PageIndex);
	PageAnim.Play(SharedThis(this));
}

void SCrowdyStudioWindow::HandleStatusMessage(const FString& Message, bool bIsError)
{
	if (!bIsError || Message.IsEmpty())
	{
		return;
	}

	FNotificationInfo Info(FText::FromString(Message));
	Info.ExpireDuration = 6.0f;
	Info.bUseSuccessFailIcons = true;
	if (const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
	{
		Item->SetCompletionState(SNotificationItem::CS_Fail);
	}
}

void SCrowdyStudioWindow::HandleSignInStateChanged()
{
	if (!Controller.IsValid())
	{
		return;
	}

	if (!Controller->IsSignedIn())
	{
		// Drop the embedded web session and return to Sign In.
		if (WebView.IsValid())
		{
			WebView->ClearSession();
		}
		if (PageSwitcher.IsValid() && ActiveIndex != CrowdyStudioPages::SignIn)
		{
			ShowPage(CrowdyStudioPages::SignIn);
		}
	}
	else if (ActiveIndex == CrowdyStudioPages::SignIn)
	{
		// A remembered token validated (or the user just signed in) while on Sign In: land on the
		// Setup Wizard so the guided path is the first thing seen after signing in.
		ShowPage(CrowdyStudioPages::Wizard);
	}
}

#undef LOCTEXT_NAMESPACE
