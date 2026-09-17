// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/SCrowdyCreateAppDialog.h"

#include "Framework/Application/SlateApplication.h"
#include "Gql/CrowdyStudioQueries.h"
#include "Model/FCrowdyStudioController.h"
#include "Style/CrowdyStudioStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyle.h"
#include "Styling/StyleDefaults.h"
#include "UI/CrowdyStudioWidgets.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CrowdyStudio"

namespace
{
	FText DatacenterLabel(const FString& Code)
	{
		if (Code == TEXT("or")) { return LOCTEXT("DcOregon", "US West (Oregon)"); }
		if (Code == TEXT("va")) { return LOCTEXT("DcVirginia", "US East (Virginia)"); }
		return FText::FromString(Code.ToUpper());
	}

	TSharedRef<SWidget> StepPill(const FText& Label, bool bActive)
	{
		FLinearColor Fill = bActive ? FCrowdyStudioStyle::Gold() : FCrowdyStudioStyle::TextSubtle();
		if (!bActive) { Fill.A = 0.18f; }
		return SNew(SBorder)
			.BorderImage(FCrowdyStudioStyle::Get().GetBrush("Crowdy.Pill"))
			.BorderBackgroundColor(FSlateColor(Fill))
			.Padding(FMargin(9.0f, 2.0f))
			[
				SNew(STextBlock).Text(Label).Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
				.ColorAndOpacity(FSlateColor(bActive ? FCrowdyStudioStyle::Panel() : FCrowdyStudioStyle::TextSecondary()))
			];
	}
}

void SCrowdyCreateAppDialog::Construct(const FArguments& InArgs)
{
	Controller = InArgs._Controller;
	OnClosed = InArgs._OnClosed;
	OnCreated = InArgs._OnCreated;

	if (Controller.IsValid())
	{
		Controller->OnPlaceableDatacentersChanged.AddSP(this, &SCrowdyCreateAppDialog::RebuildDatacenterList);
	}

	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	// A scrim over the page with the sheet centred on it. The scrim swallows clicks so the page under it
	// cannot change selection while a create is half filled in.
	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
		.BorderBackgroundColor(FSlateColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.72f)))
		.Padding(FMargin(24.0f))
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		.OnMouseButtonDown_Lambda([](const FGeometry&, const FPointerEvent&) { return FReply::Handled(); })
		[
			SNew(SBox).WidthOverride(520.0f)
			[
				CrowdyStudioWidgets::Card(
					SNew(SScrollBox)
					+ SScrollBox::Slot()
					[
						SAssignNew(StepSwitcher, SWidgetSwitcher)
						+ SWidgetSwitcher::Slot()[ BuildAppStep() ]
						+ SWidgetSwitcher::Slot()[ BuildReviewStep() ]
					],
					FMargin(20.0f, 18.0f))
			]
		]
	];
}

SCrowdyCreateAppDialog::~SCrowdyCreateAppDialog()
{
	if (Controller.IsValid())
	{
		Controller->OnPlaceableDatacentersChanged.RemoveAll(this);
	}
}

void SCrowdyCreateAppDialog::Open(int64 InOrgId)
{
	OrgId = InOrgId;
	Step = 0;
	Error = FText::GetEmpty();
	bSubmitting = false;
	SelectedDatacenter.Reset();
	// The box resets fire OnTextChanged, so the "user took over the identifier" latch is cleared after them.
	bSyncingSlug = true;
	if (NameBox.IsValid()) { NameBox->SetText(FText::GetEmpty()); }
	if (SlugBox.IsValid()) { SlugBox->SetText(FText::GetEmpty()); }
	if (DescriptionBox.IsValid()) { DescriptionBox->SetText(FText::GetEmpty()); }
	bSyncingSlug = false;
	bSlugEdited = false;
	if (StepSwitcher.IsValid()) { StepSwitcher->SetActiveWidgetIndex(0); }

	// Reopening after a cancel makes the cancelled create's late reply irrelevant to this form.
	++SubmitSerial;
	if (Controller.IsValid() && (!Controller->HasFetchedPlaceableDatacenters() || Controller->DidPlaceableDatacentersFail()))
	{
		Controller->FetchPlaceableDatacenters();
	}
	RebuildDatacenterList();
	SetVisibility(EVisibility::Visible);
	if (NameBox.IsValid())
	{
		FSlateApplication::Get().SetKeyboardFocus(NameBox, EFocusCause::SetDirectly);
	}
}

TSharedRef<SWidget> SCrowdyCreateAppDialog::BuildAppStep()
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();
	const FSlateFontInfo Mono = FCoreStyle::GetDefaultFontStyle("Mono", 9);

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[ SNew(STextBlock).Text(LOCTEXT("CreateTitle", "Name your app")).TextStyle(&Style, "Crowdy.Text.Title") ]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 12.0f)
		[ SNew(STextBlock).Text(LOCTEXT("CreateSub", "It is available immediately. No card required to start.")).TextStyle(&Style, "Crowdy.Text.Body") ]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 14.0f)
		[ SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()[ StepPill(LOCTEXT("Step1", "1. APP"), true) ]
			+ SHorizontalBox::Slot().AutoWidth().Padding(6.0f, 0.0f, 0.0f, 0.0f)[ StepPill(LOCTEXT("Step2", "2. REVIEW"), false) ] ]

		+ SVerticalBox::Slot().AutoHeight()
		[
			SAssignNew(NameBox, SEditableTextBox).Style(&Style, "Crowdy.Input")
			.HintText(LOCTEXT("AppNameHint", "My Game"))
			.OnTextChanged_Lambda([this](const FText& T)
			{
				Error = FText::GetEmpty();
				if (!bSlugEdited && SlugBox.IsValid())
				{
					// SetText fires the identifier box's own OnTextChanged; without the flag one keystroke here
					// would read as the user taking over the identifier.
					bSyncingSlug = true;
					SlugBox->SetText(FText::FromString(CrowdyStudioGql::SlugFromName(T.ToString())));
					bSyncingSlug = false;
				}
			})
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 14.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[ SNew(STextBlock).Text(LOCTEXT("SlugPreview", "URL identifier:")).TextStyle(&Style, "Crowdy.Text.Subtle") ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[ SNew(STextBlock).Font(Mono).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSecondary()))
				.Text_Lambda([this]() { return FText::FromString(CurrentSlug()); }) ]
		]

		+ SVerticalBox::Slot().AutoHeight()
		[ SNew(STextBlock).Text(LOCTEXT("DcQuestion", "Where should this app live?")).TextStyle(&Style, "Crowdy.Text.BodyStrong") ]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 8.0f)
		[ SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Subtle")
			.Text(LOCTEXT("DcExplain", "Everything the app stores lives in one datacenter, chosen now and permanent. Pick the one closest to most of your players.")) ]
		+ SVerticalBox::Slot().AutoHeight()
		[ SAssignNew(DatacenterList, SVerticalBox) ]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 12.0f, 0.0f, 0.0f)
		[
			SNew(SExpandableArea)
			.InitiallyCollapsed(true)
			.HeaderContent()[ SNew(STextBlock).Text(LOCTEXT("AdvancedHdr", "Advanced options")).TextStyle(&Style, "Crowdy.Text.BodyStrong") ]
			.BodyContent()
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 10.0f)
				[
					CrowdyStudioWidgets::Field(LOCTEXT("IdentifierLabel", "Identifier"),
						SAssignNew(SlugBox, SEditableTextBox).Style(&Style, "Crowdy.Input").Font(Mono)
						.HintText(LOCTEXT("SlugHint", "my-game"))
						.OnTextChanged_Lambda([this](const FText&) { if (!bSyncingSlug) { bSlugEdited = true; } Error = FText::GetEmpty(); }),
						LOCTEXT("SlugRule", "Lowercase letters, numbers, and hyphens only. Auto-filled from your app name unless you change it here."))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					CrowdyStudioWidgets::Field(LOCTEXT("DescLabel", "Description (optional)"),
						SNew(SBorder).BorderImage(Style.GetBrush("Crowdy.Inset")).Padding(FMargin(2.0f))
						[ SNew(SBox).HeightOverride(60.0f)[ SAssignNew(DescriptionBox, SMultiLineEditableTextBox).HintText(LOCTEXT("DescHint", "A short blurb for your app listing")) ] ])
				]
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 10.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::Danger()))
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
			.Text_Lambda([this]() { return Error; })
			.Visibility_Lambda([this]() { return Error.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 14.0f, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Ghost").ContentPadding(FMargin(12.0f, 7.0f))
				.OnClicked(this, &SCrowdyCreateAppDialog::OnCancelClicked)
				[ SNew(STextBlock).Text(LOCTEXT("Cancel", "Cancel")).TextStyle(&Style, "Crowdy.Text.Body") ]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f)[ SNullWidget::NullWidget ]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Primary").ContentPadding(FMargin(14.0f, 7.0f))
				.OnClicked(this, &SCrowdyCreateAppDialog::OnContinueClicked)
				[ SNew(STextBlock).Text(LOCTEXT("Continue", "Continue")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(FSlateColor::UseForeground()) ]
			]
		];
}

TSharedRef<SWidget> SCrowdyCreateAppDialog::BuildReviewStep()
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();
	const FSlateFontInfo Mono = FCoreStyle::GetDefaultFontStyle("Mono", 9);

	auto Heading = [&Style](const FText& T) { return SNew(STextBlock).Text(T).TextStyle(&Style, "Crowdy.Text.BodyStrong"); };

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[ SNew(STextBlock).Text(LOCTEXT("ReviewTitle", "Review")).TextStyle(&Style, "Crowdy.Text.Title") ]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 12.0f)
		[ SNew(STextBlock).Text(LOCTEXT("ReviewSub", "Your Game API connection is available immediately after create. No provisioning wait.")).TextStyle(&Style, "Crowdy.Text.Body").AutoWrapText(true) ]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 14.0f)
		[ SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()[ StepPill(LOCTEXT("Step1", "1. APP"), false) ]
			+ SHorizontalBox::Slot().AutoWidth().Padding(6.0f, 0.0f, 0.0f, 0.0f)[ StepPill(LOCTEXT("Step2", "2. REVIEW"), true) ] ]

		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SBorder).BorderImage(Style.GetBrush("Crowdy.Inset")).Padding(FMargin(14.0f, 12.0f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()[ Heading(LOCTEXT("ReviewApp", "App")) ]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 10.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
					[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Body").ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextPrimary()))
						.Text_Lambda([this]() { return NameBox.IsValid() ? NameBox->GetText() : FText::GetEmpty(); }) ]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
					[ SNew(SBorder).BorderImage(Style.GetBrush("Crowdy.Chip")).Padding(FMargin(6.0f, 1.0f))
						[ SNew(STextBlock).Font(Mono).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSecondary()))
							.Text_Lambda([this]() { return FText::FromString(TEXT("/") + CurrentSlug()); }) ] ]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle")
						.Text_Lambda([this]() { return FText::Format(LOCTEXT("ReviewInOrg", "in {0}"), FText::FromString(OrgName())); }) ]
				]
				+ SVerticalBox::Slot().AutoHeight()[ Heading(LOCTEXT("ReviewHosting", "Hosting")) ]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 10.0f)
				[
					SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Body")
					.Text_Lambda([this]()
					{
						return FText::Format(LOCTEXT("ReviewDc", "Datacenter: {0} ({1}). This is permanent for the life of the app."),
							DatacenterLabel(SelectedDatacenter), FText::FromString(SelectedDatacenter));
					})
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SBorder).BorderImage(Style.GetBrush("Crowdy.Card")).Padding(FMargin(12.0f, 10.0f))
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()[ Heading(LOCTEXT("FreeTierHdr", "Free tier")) ]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
						[
							SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Subtle")
							.Text_Lambda([this]()
							{
								const int32 Free = Controller.IsValid() ? Controller->GetFreeAppsPerOrg() : 0;
								const FText Slots = Free > 0
									? FText::Format(LOCTEXT("FreeSlots", "{0} of {1} free app slots used in this org."), FText::AsNumber(OrgAppCount()), FText::AsNumber(Free))
									: LOCTEXT("FreeSlotsUnknown", "Every org gets a few free app slots.");
								return FText::Format(LOCTEXT("FreeTierBody", "{0} No credit card required to start. Usage meters appear on the web console as players connect."), Slots);
							})
						]
					]
				]
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 10.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::Danger()))
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
			.Text_Lambda([this]() { return Error; })
			.Visibility_Lambda([this]() { return Error.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 14.0f, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Ghost").ContentPadding(FMargin(12.0f, 7.0f))
				.IsEnabled_Lambda([this]() { return !bSubmitting; })
				.OnClicked(this, &SCrowdyCreateAppDialog::OnBackClicked)
				[ SNew(STextBlock).Text(LOCTEXT("Back", "Back")).TextStyle(&Style, "Crowdy.Text.Body") ]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f)[ SNullWidget::NullWidget ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 10.0f, 0.0f)
			[ SNew(SCircularThrobber).Radius(8.0f).Visibility_Lambda([this]() { return bSubmitting ? EVisibility::Visible : EVisibility::Collapsed; }) ]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Primary").ContentPadding(FMargin(14.0f, 7.0f))
				.IsEnabled_Lambda([this]() { return !bSubmitting; })
				.OnClicked(this, &SCrowdyCreateAppDialog::OnCreateClicked)
				[ SNew(STextBlock).Text(LOCTEXT("CreateApp", "Create app")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(FSlateColor::UseForeground()) ]
			]
		];
}

void SCrowdyCreateAppDialog::RebuildDatacenterList()
{
	if (!DatacenterList.IsValid())
	{
		return;
	}
	DatacenterList->ClearChildren();

	const ISlateStyle& Style = FCrowdyStudioStyle::Get();
	const FSlateFontInfo Mono = FCoreStyle::GetDefaultFontStyle("Mono", 9);

	if (!Controller.IsValid())
	{
		return;
	}
	if (Controller->DidPlaceableDatacentersFail())
	{
		DatacenterList->AddSlot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[ SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::Danger())).Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
				.Text(LOCTEXT("DcFailed", "Could not read the datacenters this deployment can place an app in.")) ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Secondary").ContentPadding(FMargin(10.0f, 5.0f))
				.OnClicked_Lambda([this]() { Controller->FetchPlaceableDatacenters(); return FReply::Handled(); })
				[ SNew(STextBlock).Text(LOCTEXT("DcRetry", "Retry")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 9)).ColorAndOpacity(FSlateColor::UseForeground()) ]
			]
		];
		return;
	}
	if (!Controller->HasFetchedPlaceableDatacenters())
	{
		DatacenterList->AddSlot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)[ SNew(SCircularThrobber).Radius(7.0f) ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[ SNew(STextBlock).Text(LOCTEXT("DcLoading", "Loading datacenters...")).TextStyle(&Style, "Crowdy.Text.Subtle") ]
		];
		return;
	}

	int32 Offered = 0;
	for (const FStudioDatacenter& Dc : Controller->GetPlaceableDatacenters())
	{
		if (!Dc.bPlaceable)
		{
			continue;
		}
		++Offered;
		if (SelectedDatacenter.IsEmpty())
		{
			SelectedDatacenter = Dc.Code;
		}
		const FString Code = Dc.Code;
		DatacenterList->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
		[
			SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Nav").ContentPadding(0.0f)
			.OnClicked_Lambda([this, Code]() { SelectedDatacenter = Code; Error = FText::GetEmpty(); return FReply::Handled(); })
			[
				SNew(SBorder)
				.BorderImage_Lambda([this, Code]() { return FCrowdyStudioStyle::Get().GetBrush(SelectedDatacenter == Code ? "Crowdy.Card.Selected" : "Crowdy.Card"); })
				.Padding(FMargin(12.0f, 9.0f))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 10.0f, 0.0f)
					[
						SNew(SBox).WidthOverride(14.0f).HeightOverride(14.0f)
						[
							SNew(SBorder).BorderImage(Style.GetBrush("Crowdy.Pill"))
							.BorderBackgroundColor_Lambda([this, Code]() { return FSlateColor(SelectedDatacenter == Code ? FCrowdyStudioStyle::Gold() : FCrowdyStudioStyle::Line()); })
							.Padding(3.0f)
							[
								SNew(SBorder).BorderImage(Style.GetBrush("Crowdy.Pill"))
								.BorderBackgroundColor_Lambda([this, Code]() { return FSlateColor(SelectedDatacenter == Code ? FCrowdyStudioStyle::Panel() : FCrowdyStudioStyle::Surface()); })
							]
						]
					]
					+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.BodyStrong")
							.Text(FText::Format(LOCTEXT("DcRow", "{0}  {1}"), DatacenterLabel(Dc.Code), FText::FromString(Dc.Code))) ]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
						[ SNew(STextBlock).Font(Mono).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSubtle())).Text(FText::FromString(Dc.GameApiUrl)) ]
					]
				]
			]
		];
	}

	if (Offered == 0)
	{
		DatacenterList->AddSlot().AutoHeight()
		[
			SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::Warning())).Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
			.Text(LOCTEXT("DcNone", "This deployment has no datacenter that can take a new app right now, so creation will be refused. Ask the server operator."))
		];
	}
}

FReply SCrowdyCreateAppDialog::OnContinueClicked()
{
	const FString Name = NameBox.IsValid() ? NameBox->GetText().ToString().TrimStartAndEnd() : FString();
	const FString Slug = CurrentSlug();
	if (Name.IsEmpty())
	{
		Error = LOCTEXT("ErrNoName", "Give the app a name.");
		return FReply::Handled();
	}
	if (Slug.IsEmpty() || Slug != CrowdyStudioGql::SlugFromName(Slug))
	{
		Error = LOCTEXT("ErrBadSlug", "The identifier can only use lowercase letters, numbers, and single hyphens.");
		return FReply::Handled();
	}
	if (SelectedDatacenter.IsEmpty())
	{
		Error = LOCTEXT("ErrNoDc", "Choose the datacenter the app will live in.");
		return FReply::Handled();
	}
	Error = FText::GetEmpty();
	Step = 1;
	if (StepSwitcher.IsValid()) { StepSwitcher->SetActiveWidgetIndex(1); }
	return FReply::Handled();
}

FReply SCrowdyCreateAppDialog::OnBackClicked()
{
	Error = FText::GetEmpty();
	Step = 0;
	if (StepSwitcher.IsValid()) { StepSwitcher->SetActiveWidgetIndex(0); }
	return FReply::Handled();
}

FReply SCrowdyCreateAppDialog::OnCreateClicked()
{
	if (!Controller.IsValid() || bSubmitting)
	{
		return FReply::Handled();
	}
	bSubmitting = true;
	Error = FText::GetEmpty();

	const FString Name = NameBox.IsValid() ? NameBox->GetText().ToString().TrimStartAndEnd() : FString();
	const FString Description = DescriptionBox.IsValid() ? DescriptionBox->GetText().ToString().TrimStartAndEnd() : FString();
	TWeakPtr<SCrowdyCreateAppDialog> WeakSelf = SharedThis(this);
	const int32 Serial = SubmitSerial;
	Controller->CreateApp(OrgId, Name, CurrentSlug(), SelectedDatacenter, Description,
		[WeakSelf, Serial](int64 CreatedAppId)
		{
			const TSharedPtr<SCrowdyCreateAppDialog> Self = WeakSelf.Pin();
			if (!Self.IsValid() || Self->SubmitSerial != Serial)
			{
				// The sheet was cancelled and reopened meanwhile; the list refetch already shows the result.
				return;
			}
			Self->bSubmitting = false;
			if (CreatedAppId == 0)
			{
				// The controller has already put the server's reason on the status line; repeat it here where the
				// user is looking. A cancelled request sets no status, so the line may still hold an older message.
				const bool bHaveReason = Self->Controller.IsValid() && Self->Controller->LastStatusWasError();
				Self->Error = bHaveReason ? FText::FromString(Self->Controller->GetStatusMessage())
					: LOCTEXT("ErrCreateFailed", "The app could not be created. Try again.");
				return;
			}
			Self->SetVisibility(EVisibility::Collapsed);
			Self->OnCreated.ExecuteIfBound(CreatedAppId);
			Self->OnClosed.ExecuteIfBound();
		});
	return FReply::Handled();
}

FReply SCrowdyCreateAppDialog::OnCancelClicked()
{
	SetVisibility(EVisibility::Collapsed);
	OnClosed.ExecuteIfBound();
	return FReply::Handled();
}

FString SCrowdyCreateAppDialog::CurrentSlug() const
{
	if (SlugBox.IsValid() && !SlugBox->GetText().IsEmpty())
	{
		return SlugBox->GetText().ToString().TrimStartAndEnd();
	}
	return NameBox.IsValid() ? CrowdyStudioGql::SlugFromName(NameBox->GetText().ToString()) : FString();
}

FString SCrowdyCreateAppDialog::OrgName() const
{
	if (!Controller.IsValid())
	{
		return FString();
	}
	for (const TSharedPtr<FStudioOrg>& Org : Controller->GetOrganizations())
	{
		if (Org.IsValid() && Org->OrgId == OrgId)
		{
			return Org->Name;
		}
	}
	return FString();
}

int32 SCrowdyCreateAppDialog::OrgAppCount() const
{
	int32 N = 0;
	if (!Controller.IsValid())
	{
		return N;
	}
	for (const TSharedPtr<FStudioApp>& App : Controller->GetApps())
	{
		if (App.IsValid() && App->OrgId == OrgId && App->Status != TEXT("ARCHIVED"))
		{
			++N;
		}
	}
	return N;
}

#undef LOCTEXT_NAMESPACE
