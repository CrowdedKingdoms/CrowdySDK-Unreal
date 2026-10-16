#include "UI/SCrowdyAccountView.h"

#include "Model/FCrowdyStudioController.h"
#include "Style/CrowdyStudioStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyle.h"
#include "UI/CrowdyStudioWidgets.h"
#include "Web/FCrowdyStudioLinks.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CrowdyStudio"

namespace
{
	// An icon and a bold label, the content every action button on the page carries.
	TSharedRef<SWidget> AcctButtonContent(const TCHAR* IconName, const FText& Label, FSlateColor Tint = FSlateColor::UseForeground())
	{
		TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
		if (IconName)
		{
			Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)[ CrowdyStudioWidgets::Icon(IconName, 14.0f, Tint) ];
		}
		Row->AddSlot().AutoWidth().VAlign(VAlign_Center)[ SNew(STextBlock).Text(Label).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(Tint) ];
		return Row;
	}

	// A label over a value, as the Project page's facts read.
	TSharedRef<SWidget> AcctFact(const FText& Label, TFunction<FString()> Value)
	{
		const ISlateStyle& Style = FCrowdyStudioStyle::Get();
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[ SNew(STextBlock).Text(Label).TextStyle(&Style, "Crowdy.Text.Subtle") ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Regular", 10)).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextPrimary()))
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				.Text_Lambda([Value]() { const FString V = Value(); return FText::FromString(V.IsEmpty() ? TEXT("-") : V); })
			];
	}

	TSharedRef<SWidget> AcctDivider()
	{
		return SNew(SBox).HeightOverride(1.0f)[ SNew(SImage).Image(FCrowdyStudioStyle::Get().GetBrush("Crowdy.Separator")) ];
	}
}

void SCrowdyAccountView::Construct(const FArguments& InArgs)
{
	Controller = InArgs._Controller;
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();
	auto Manageable = [this]() { return Controller.IsValid() && Controller->CanManageAccount(); };

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[ SNew(STextBlock).Text(LOCTEXT("AccountHeader", "Account")).TextStyle(&Style, "Crowdy.Text.Title") ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Ghost").ContentPadding(FMargin(11.0f, 6.0f))
				.IsEnabled_Lambda(Manageable)
				.ToolTipText(LOCTEXT("AccountRefreshTip", "Read your name and email again."))
				.OnClicked_Lambda([this]() { Controller->FetchMyProfile(); return FReply::Handled(); })
				[ AcctButtonContent(TEXT("refresh"), LOCTEXT("AccountRefresh", "Refresh")) ]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Ghost").ContentPadding(FMargin(9.0f, 6.0f))
				.ToolTipText(LOCTEXT("AccountPortalTip", "Open your account in the web console: sign-in methods, connected apps and the apps you have access to."))
				.OnClicked_Lambda([]() { FCrowdyStudioLinks::OpenExternal(FCrowdyStudioLinks::BaseUrl() + TEXT("/account")); return FReply::Handled(); })
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)[ CrowdyStudioWidgets::Icon(TEXT("external-link"), 14.0f, FSlateColor(FCrowdyStudioStyle::TextSecondary())) ]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ SNew(STextBlock).Text(LOCTEXT("AccountPortal", "Web console")).TextStyle(&Style, "Crowdy.Text.Body") ]
				]
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
		[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle").Text(LOCTEXT("AccountSub", "The account this editor is signed in with: your name, your password and your sessions.")) ]
		+ SVerticalBox::Slot().FillHeight(1.0f)
		[
			SNew(SWidgetSwitcher)
			.WidgetIndex_Lambda([Manageable]() { return Manageable() ? 1 : 0; })
			+ SWidgetSwitcher::Slot()
			[
				CrowdyStudioWidgets::Card(
					CrowdyStudioWidgets::EmptyState(TEXT("login"),
						LOCTEXT("AccountNeedsSession", "Sign in with your email, a magic link or a social account to manage it.\nAn organization token has no account of its own.")),
					FMargin(16.0f), /*bFlat*/ true)
			]
			+ SWidgetSwitcher::Slot()
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()[ BuildHeaderCard() ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 12.0f, 0.0f, 8.0f)
					[
						CrowdyStudioWidgets::TabStrip(
							{ TEXT("password"), TEXT("sessions") },
							{ LOCTEXT("TabPassword", "Password"), LOCTEXT("TabSessions", "Sessions") },
							TAttribute<FString>::CreateLambda([this]() { return ActiveTab; }),
							[this](const FString& V)
							{
								ActiveTab = V;
								if (TabSwitcher.IsValid()) { TabSwitcher->SetActiveWidgetIndex(V == TEXT("sessions") ? 1 : 0); }
							})
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SAssignNew(TabSwitcher, SWidgetSwitcher)
						+ SWidgetSwitcher::Slot()[ BuildPasswordTab() ]
						+ SWidgetSwitcher::Slot()[ BuildSessionsTab() ]
					]
				]
			]
		]
	];

	Controller->OnSignInStateChanged.AddSP(this, &SCrowdyAccountView::HandleSignInStateChanged);
	HandleSignInStateChanged();
}

SCrowdyAccountView::~SCrowdyAccountView()
{
	if (Controller.IsValid())
	{
		Controller->OnSignInStateChanged.RemoveAll(this);
	}
}

TSharedRef<SWidget> SCrowdyAccountView::BuildHeaderCard()
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();
	const FLinearColor Subtle = FCrowdyStudioStyle::TextSecondary();

	const TSharedRef<SWidget> ViewMode = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Heading").Text_Lambda([this]()
				{
					const FString Name = Controller.IsValid() ? Controller->GetMyGamertag() : FString();
					return Name.IsEmpty() ? LOCTEXT("AccountNoName", "No name yet") : FText::FromString(Name);
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[
				SNew(SBorder).BorderImage(Style.GetBrush("Crowdy.Pill")).Padding(FMargin(8.0f, 2.0f))
				.BorderBackgroundColor(FSlateColor(FLinearColor(Subtle.R, Subtle.G, Subtle.B, 0.16f)))
				.Visibility_Lambda([this]() { return Controller.IsValid() && !Controller->GetMyDisambiguation().IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
				[
					SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Bold", 8)).ColorAndOpacity(FSlateColor(Subtle))
					.Text_Lambda([this]() { return FText::FromString(Controller.IsValid() ? Controller->GetMyDisambiguation() : FString()); })
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[ CrowdyStudioWidgets::Badge(LOCTEXT("AccountBadge", "ACCOUNT"), CrowdyStudioWidgets::EBadgeTone::Success) ]
			+ SHorizontalBox::Slot().FillWidth(1.0f)[ SNullWidget::NullWidget ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[ CrowdyStudioWidgets::CopyChip(TAttribute<FString>::CreateLambda([this]() { return Controller.IsValid() ? LexToString(Controller->GetMyUserId()) : FString(); }), LOCTEXT("UserIdWhat", "user id")) ]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
		[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle").Text(LOCTEXT("AccountNameHint", "The name players see. The gamertag and the disambiguation together are unique.")) ]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 12.0f, 0.0f, 0.0f)
		[
			SNew(SUniformGridPanel).SlotPadding(FMargin(0.0f, 0.0f, 14.0f, 8.0f))
			+ SUniformGridPanel::Slot(0, 0)[ AcctFact(LOCTEXT("FactEmail", "Email"), [this]() { return Controller.IsValid() ? Controller->GetMyEmail() : FString(); }) ]
			+ SUniformGridPanel::Slot(1, 0)[ AcctFact(LOCTEXT("FactGamertag", "Gamertag"), [this]() { return Controller.IsValid() ? Controller->GetMyGamertag() : FString(); }) ]
			+ SUniformGridPanel::Slot(2, 0)[ AcctFact(LOCTEXT("FactDisambiguation", "Disambiguation"), [this]() { return Controller.IsValid() ? Controller->GetMyDisambiguation() : FString(); }) ]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 10.0f, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Secondary").ContentPadding(FMargin(11.0f, 6.0f))
				.ToolTipText(LOCTEXT("EditNameTip", "Change your gamertag and disambiguation."))
				.OnClicked_Lambda([this]()
				{
					GamertagBox->SetText(FText::FromString(Controller->GetMyGamertag()));
					DisambiguationBox->SetText(FText::FromString(Controller->GetMyDisambiguation()));
					HeaderSwitcher->SetActiveWidgetIndex(1);
					return FReply::Handled();
				})
				[ AcctButtonContent(TEXT("edit"), LOCTEXT("EditName", "Edit name")) ]
			]
		];

	const TSharedRef<SWidget> EditMode = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[ CrowdyStudioWidgets::Field(LOCTEXT("GamertagLabel", "Gamertag"), SAssignNew(GamertagBox, SEditableTextBox).Style(&Style, "Crowdy.Input"), LOCTEXT("GamertagHint", "Up to 64 characters.")) ]
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[ CrowdyStudioWidgets::Field(LOCTEXT("DisambiguationLabel", "Disambiguation"), SAssignNew(DisambiguationBox, SEditableTextBox).Style(&Style, "Crowdy.Input"), LOCTEXT("DisambiguationHint", "Up to 128 characters.")) ]
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f)[ SNullWidget::NullWidget ]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Ghost").ContentPadding(FMargin(11.0f, 6.0f))
				.OnClicked_Lambda([this]() { HeaderSwitcher->SetActiveWidgetIndex(0); return FReply::Handled(); })
				[ SNew(STextBlock).Text(LOCTEXT("Cancel", "Cancel")).TextStyle(&Style, "Crowdy.Text.Body") ]
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Secondary").ContentPadding(FMargin(11.0f, 6.0f))
				.IsEnabled_Lambda([this]() { return GamertagBox.IsValid() && !GamertagBox->GetText().ToString().TrimStartAndEnd().IsEmpty(); })
				.OnClicked(this, &SCrowdyAccountView::OnSaveNameClicked)
				[ SNew(STextBlock).Text(LOCTEXT("SaveName", "Save")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(FSlateColor::UseForeground()) ]
			]
		];

	return CrowdyStudioWidgets::Card(
		SAssignNew(HeaderSwitcher, SWidgetSwitcher)
		+ SWidgetSwitcher::Slot()[ ViewMode ]
		+ SWidgetSwitcher::Slot()[ EditMode ],
		FMargin(14.0f, 12.0f));
}

TSharedRef<SWidget> SCrowdyAccountView::BuildPasswordTab()
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();
	auto Adding = [this]() { return PasswordMode == TEXT("add"); };

	return CrowdyStudioWidgets::Card(
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
		[
			SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Body")
			.Text(LOCTEXT("PasswordExplainer", "Change your password, or add one to an account you made with a magic link or a social sign-in, so you can also sign in with your email and a password."))
		]
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left).Padding(0.0f, 0.0f, 0.0f, 12.0f)
		[
			SNew(SBox).WidthOverride(380.0f)
			[
				CrowdyStudioWidgets::SegmentedEnum(
					{ TEXT("change"), TEXT("add") },
					{ LOCTEXT("PasswordChangeMode", "I have a password"), LOCTEXT("PasswordAddMode", "I don't have one yet") },
					TAttribute<FString>::CreateLambda([this]() { return PasswordMode; }),
					[this](const FString& V) { PasswordMode = V; })
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
		[
			SNew(SBorder).BorderImage(Style.GetBrush("Crowdy.Inset")).Padding(FMargin(14.0f, 12.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(SBox).Visibility_Lambda([Adding]() { return Adding() ? EVisibility::Collapsed : EVisibility::Visible; })
					[ CrowdyStudioWidgets::Field(LOCTEXT("CurrentPasswordLabel", "Current password"), SAssignNew(CurrentPasswordBox, SEditableTextBox).Style(&Style, "Crowdy.Input").IsPassword(true)) ]
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[ CrowdyStudioWidgets::Field(LOCTEXT("NewPasswordLabel", "New password"), SAssignNew(NewPasswordBox, SEditableTextBox).Style(&Style, "Crowdy.Input").IsPassword(true)) ]
			]
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle").AutoWrapText(true)
				.Text_Lambda([Adding]()
				{
					return Adding() ? LOCTEXT("AddPasswordHint", "It works at once, and a security email goes to your address.")
						: LOCTEXT("ChangePasswordHint", "Your other devices are signed out; this editor stays signed in.");
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(12.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Primary").ContentPadding(FMargin(14.0f, 8.0f))
				.IsEnabled_Lambda([this, Adding]()
				{
					const bool bNew = NewPasswordBox.IsValid() && !NewPasswordBox->GetText().IsEmpty();
					return bNew && (Adding() || (CurrentPasswordBox.IsValid() && !CurrentPasswordBox->GetText().IsEmpty()));
				})
				.OnClicked(this, &SCrowdyAccountView::OnPasswordClicked)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 7.0f, 0.0f)[ CrowdyStudioWidgets::Icon(TEXT("key"), 15.0f, FSlateColor::UseForeground()) ]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(FSlateColor::UseForeground())
						.Text_Lambda([Adding]() { return Adding() ? LOCTEXT("AddPassword", "Add password") : LOCTEXT("ChangePassword", "Change password"); })
					]
				]
			]
		],
		FMargin(16.0f, 14.0f));
}

TSharedRef<SWidget> SCrowdyAccountView::BuildSessionsTab()
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	auto SessionRow = [&Style](const FText& Label, TFunction<FString()> Value) -> TSharedRef<SWidget>
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 18.0f, 0.0f)
			[ SNew(SBox).WidthOverride(140.0f)[ SNew(STextBlock).Text(Label).TextStyle(&Style, "Crowdy.Text.Body") ] ]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Mono", 9)).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSecondary()))
				.Text_Lambda([Value]() { return FText::FromString(Value()); })
			];
	};

	return CrowdyStudioWidgets::Card(
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
		[
			SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Body")
			.Text(LOCTEXT("SessionsExplainer", "Every device and every game you are signed in to holds a session of this account. Sign Out in the header ends only this editor's."))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
		[
			SNew(SBorder).BorderImage(Style.GetBrush("Crowdy.Inset")).Padding(FMargin(14.0f, 12.0f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
				[ SessionRow(LOCTEXT("SessionThis", "This editor"), [this]() { return Controller.IsValid() && !Controller->GetMyEmail().IsEmpty() ? Controller->GetMyEmail() : FString(TEXT("signed in")); }) ]
				+ SVerticalBox::Slot().AutoHeight()[ AcctDivider() ]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 10.0f, 0.0f, 0.0f)
				[ SessionRow(LOCTEXT("SessionOthers", "Everywhere else"), []() { return FString(TEXT("other devices, games and browsers")); }) ]
			]
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle").AutoWrapText(true).Text(LOCTEXT("SignOutEverywhereHint", "Ends them all, this editor's included; you sign in again here afterwards.")) ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(12.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Secondary").ContentPadding(FMargin(14.0f, 8.0f))
				.OnClicked_Lambda([this]() { Controller->SignOutEverywhere(); return FReply::Handled(); })
				[ AcctButtonContent(TEXT("globe"), LOCTEXT("SignOutEverywhere", "Sign out everywhere"), FSlateColor(FCrowdyStudioStyle::Danger())) ]
			]
		],
		FMargin(16.0f, 14.0f));
}

void SCrowdyAccountView::HandleSignInStateChanged()
{
	if (HeaderSwitcher.IsValid())
	{
		HeaderSwitcher->SetActiveWidgetIndex(0);
	}
	if (Controller.IsValid() && Controller->CanManageAccount())
	{
		Controller->FetchMyProfile();
	}
}

FReply SCrowdyAccountView::OnSaveNameClicked()
{
	Controller->SetMyName(GamertagBox->GetText().ToString().TrimStartAndEnd(), DisambiguationBox->GetText().ToString().TrimStartAndEnd());
	HeaderSwitcher->SetActiveWidgetIndex(0);
	return FReply::Handled();
}

FReply SCrowdyAccountView::OnPasswordClicked()
{
	const FString NewPassword = NewPasswordBox->GetText().ToString();
	if (PasswordMode == TEXT("add"))
	{
		Controller->AddMyPassword(NewPassword);
	}
	else
	{
		Controller->ChangeMyPassword(CurrentPasswordBox->GetText().ToString(), NewPassword);
	}
	CurrentPasswordBox->SetText(FText::GetEmpty());
	NewPasswordBox->SetText(FText::GetEmpty());
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
