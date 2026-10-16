#include "UI/SCrowdyAccountView.h"

#include "Model/FCrowdyStudioController.h"
#include "Style/CrowdyStudioStyle.h"
#include "Styling/CoreStyle.h"
#include "UI/CrowdyStudioWidgets.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CrowdyStudio"

void SCrowdyAccountView::Construct(const FArguments& InArgs)
{
	Controller = InArgs._Controller;
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	auto Button = [](const FText& Label, bool bPrimary, TFunction<void()> OnClick) -> TSharedRef<SWidget>
	{
		return SNew(SButton)
			.ButtonStyle(&FCrowdyStudioStyle::Get(), bPrimary ? "Crowdy.Button.Primary" : "Crowdy.Button.Secondary")
			.ContentPadding(FMargin(12.0f, 6.0f))
			.OnClicked_Lambda([OnClick]() { OnClick(); return FReply::Handled(); })
			[ SNew(STextBlock).Text(Label).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(FSlateColor::UseForeground()) ];
	};

	auto Visible = [this](bool bWhenManageable)
	{
		return TAttribute<EVisibility>::CreateLambda([this, bWhenManageable]()
		{
			const bool bManageable = Controller.IsValid() && Controller->CanManageAccount();
			return bManageable == bWhenManageable ? EVisibility::Visible : EVisibility::Collapsed;
		});
	};

	const TSharedRef<SWidget> NameCard = CrowdyStudioWidgets::Card(
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("AccountNameHeader", "Name"), TEXT("edit")) ]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f)
		[
			SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Body").Text_Lambda([this]() -> FText
			{
				if (!Controller.IsValid() || Controller->GetMyGamertag().IsEmpty())
				{
					return LOCTEXT("AccountNoName", "You have not chosen a name yet.");
				}
				return FText::Format(LOCTEXT("AccountCurrentName", "Players see you as {0} ({1})."),
					FText::FromString(Controller->GetMyGamertag()), FText::FromString(Controller->GetMyDisambiguation()));
			})
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
		[ CrowdyStudioWidgets::Field(LOCTEXT("AccountGamertag", "Gamertag"), SAssignNew(GamertagBox, SEditableTextBox).Style(&Style, "Crowdy.Input"), LOCTEXT("AccountGamertagHint", "Up to 64 characters.")) ]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
		[ CrowdyStudioWidgets::Field(LOCTEXT("AccountDisambiguation", "Disambiguation"), SAssignNew(DisambiguationBox, SEditableTextBox).Style(&Style, "Crowdy.Input"),
			LOCTEXT("AccountDisambiguationHint", "Makes the name unique: no two accounts share the gamertag and disambiguation together.")) ]
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left).Padding(0.0f, 6.0f, 0.0f, 0.0f)
		[
			Button(LOCTEXT("AccountSaveName", "Save name"), true, [this]()
			{
				Controller->SetMyName(GamertagBox->GetText().ToString(), DisambiguationBox->GetText().ToString());
			})
		],
		FMargin(14.0f, 12.0f), true);

	const TSharedRef<SWidget> PasswordCard = CrowdyStudioWidgets::Card(
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("AccountPasswordHeader", "Password"), TEXT("key")) ]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
		[ CrowdyStudioWidgets::Field(LOCTEXT("AccountCurrentPassword", "Current password"), SAssignNew(CurrentPasswordBox, SEditableTextBox).Style(&Style, "Crowdy.Input").IsPassword(true),
			LOCTEXT("AccountCurrentPasswordHint", "Not needed to add a password to an account made by magic link or social sign-in.")) ]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
		[ CrowdyStudioWidgets::Field(LOCTEXT("AccountNewPassword", "New password"), SAssignNew(NewPasswordBox, SEditableTextBox).Style(&Style, "Crowdy.Input").IsPassword(true), FText()) ]
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left).Padding(0.0f, 6.0f, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				Button(LOCTEXT("AccountChangePassword", "Change password"), true, [this]()
				{
					Controller->ChangeMyPassword(CurrentPasswordBox->GetText().ToString(), NewPasswordBox->GetText().ToString());
					CurrentPasswordBox->SetText(FText::GetEmpty());
					NewPasswordBox->SetText(FText::GetEmpty());
				})
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				Button(LOCTEXT("AccountAddPassword", "Add password"), false, [this]()
				{
					Controller->AddMyPassword(NewPasswordBox->GetText().ToString());
					CurrentPasswordBox->SetText(FText::GetEmpty());
					NewPasswordBox->SetText(FText::GetEmpty());
				})
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)
		[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle").AutoWrapText(true)
			.Text(LOCTEXT("AccountPasswordNote", "Changing the password signs out your other devices; this editor stays signed in.")) ],
		FMargin(14.0f, 12.0f), true);

	const TSharedRef<SWidget> SessionsCard = CrowdyStudioWidgets::Card(
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("AccountSessionsHeader", "Sessions"), TEXT("globe")) ]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f)
		[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Body").AutoWrapText(true)
			.Text(LOCTEXT("AccountSessionsBody", "Sign out everywhere ends every session of your account, on every device and in every game, this editor included.")) ]
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left)
		[ Button(LOCTEXT("AccountSignOutEverywhere", "Sign out everywhere"), false, [this]() { Controller->SignOutEverywhere(); }) ],
		FMargin(14.0f, 12.0f), true);

	ChildSlot
	[
		SNew(SScrollBox)
		+ SScrollBox::Slot().Padding(2.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[ SNew(STextBlock).Text(LOCTEXT("AccountTitle", "Account")).TextStyle(&Style, "Crowdy.Text.Title") ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 16.0f)
			[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Body").AutoWrapText(true)
				.Text_Lambda([this]() -> FText
				{
					const FString Email = Controller.IsValid() ? Controller->GetMyEmail() : FString();
					return Email.IsEmpty() ? LOCTEXT("AccountSubtitle", "The account this editor is signed in with.")
						: FText::Format(LOCTEXT("AccountSubtitleEmail", "The account this editor is signed in with: {0}."), FText::FromString(Email));
				}) ]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBox).Visibility(Visible(false))
				[ CrowdyStudioWidgets::EmptyState(TEXT("login"),
					LOCTEXT("AccountNeedsSession", "Sign in with your email, a magic link or a social account to manage it.\nAn organization token has no account of its own.")) ]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)[ SNew(SBox).Visibility(Visible(true))[ NameCard ] ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)[ SNew(SBox).Visibility(Visible(true))[ PasswordCard ] ]
			+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(Visible(true))[ SessionsCard ] ]
		]
	];

	Controller->OnSignInStateChanged.AddSP(this, &SCrowdyAccountView::HandleSignInStateChanged);
	Controller->OnMyProfileChanged.AddSP(this, &SCrowdyAccountView::HandleProfileChanged);
	HandleSignInStateChanged();
}

void SCrowdyAccountView::HandleSignInStateChanged()
{
	if (Controller.IsValid() && Controller->CanManageAccount())
	{
		Controller->FetchMyProfile();
	}
}

void SCrowdyAccountView::HandleProfileChanged()
{
	GamertagBox->SetText(FText::FromString(Controller->GetMyGamertag()));
	DisambiguationBox->SetText(FText::FromString(Controller->GetMyDisambiguation()));
}

#undef LOCTEXT_NAMESPACE
