// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/SCrowdyEnvironmentsView.h"

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
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "CrowdyStudio"

void SCrowdyEnvironmentsView::Construct(const FArguments& InArgs)
{
	Controller = InArgs._Controller;
	const bool bEmbedded = InArgs._Embedded;

	if (Controller.IsValid())
	{
		Controller->OnEnvironmentsChanged.AddSP(this, &SCrowdyEnvironmentsView::HandleEnvironmentsChanged);
	}

	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	TSharedRef<SWidget> RefreshButton = SNew(SButton)
		.ButtonStyle(&Style, "Crowdy.Button.Secondary")
		.ContentPadding(FMargin(9.0f, 5.0f))
		.OnClicked(this, &SCrowdyEnvironmentsView::OnRefreshClicked)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[ CrowdyStudioWidgets::Icon(TEXT("refresh"), 14.0f, FSlateColor(FCrowdyStudioStyle::TextSecondary())) ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[ SNew(STextBlock).Text(LOCTEXT("RefreshEnvButton", "Refresh")).Font(FCoreStyle::GetDefaultFontStyle("Regular", 9)).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSecondary())) ]
		];

	// The environment list with its empty-state overlay, shared by both layouts below.
	TSharedRef<SWidget> ListCard = CrowdyStudioWidgets::Card(
		SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SAssignNew(EnvListView, SListView<TSharedPtr<FStudioEnvironment>>)
			.ListItemsSource(Controller.IsValid() ? &Controller->GetEnvironments() : nullptr)
			.OnGenerateRow(this, &SCrowdyEnvironmentsView::MakeEnvRow)
			.OnSelectionChanged(this, &SCrowdyEnvironmentsView::OnEnvSelected)
			.SelectionMode(ESelectionMode::Single)
		]
		+ SOverlay::Slot()
		[
			SNew(SBox)
			.Visibility_Lambda([this]()
			{
				return (Controller.IsValid() && Controller->GetEnvironments().Num() == 0) ? EVisibility::Visible : EVisibility::Collapsed;
			})
			[
				CrowdyStudioWidgets::EmptyState(TEXT("server"), LOCTEXT("NoEnvs", "No environments loaded.\nSelect an organization and press Refresh."))
			]
		],
		FMargin(6.0f), /*bFlat*/ true);

	TSharedRef<SVerticalBox> Root = SNew(SVerticalBox);

	// Standalone keeps its own page title. Embedded sits under the Project page's "Game server"
	// header, so it skips the title to avoid a second heading.
	if (!bEmbedded)
	{
		Root->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
		[ SNew(STextBlock).Text(LOCTEXT("EnvHeader", "Environments")).TextStyle(&Style, "Crowdy.Text.Title") ];
	}

	// Provisioning hint: the things that stay in the web console.
	Root->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
	[
		SNew(SBorder).BorderImage(Style.GetBrush("Crowdy.Inset")).Padding(FMargin(12.0f, 10.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0.0f, 1.0f, 10.0f, 0.0f)
			[ CrowdyStudioWidgets::Icon(TEXT("external-link"), 16.0f, FSlateColor(FCrowdyStudioStyle::Info())) ]
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				SNew(STextBlock).AutoWrapText(true)
				.TextStyle(&Style, "Crowdy.Text.Body")
				.Text(LOCTEXT("EnvProvisionHint",
					"To provision, destroy, or manage secrets for an environment, use the Web Console. Here you can link the selected app to an environment and sync its endpoints into the project."))
			]
		]
	];

	// Available environments list, with the Refresh button in the header.
	Root->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
	[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("EnvAvailable", "Available environments"), TEXT("server"), RefreshButton) ];

	// Embedded sits inside the Project page's scroll box, which gives infinite height, so the
	// list needs a fixed height. Standalone fills the remaining space.
	if (bEmbedded)
	{
		Root->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
		[ SNew(SBox).HeightOverride(200.0f)[ ListCard ] ];
	}
	else
	{
		Root->AddSlot().FillHeight(1.0f).Padding(0.0f, 0.0f, 0.0f, 12.0f)
		[ ListCard ];
	}

	// Actions. Link is always available. The standalone page also offers its own Sync, but the
	// embedded view leaves syncing to the Project page's single Sync button.
	TSharedRef<SHorizontalBox> Actions = SNew(SHorizontalBox);

	Actions->AddSlot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
	[
		SNew(SButton)
		.ButtonStyle(&Style, "Crowdy.Button.Primary")
		.ContentPadding(FMargin(13.0f, 7.0f))
		.ToolTipText_Lambda([this]()
		{
			if (Controller.IsValid() && Controller->GetSelectedAppId() != 0 && !Controller->CanManageEnvironments())
			{
				return LOCTEXT("LinkNoPerm", "Your role in this organization can't link environments (needs the manage_environments permission).");
			}
			return LOCTEXT("LinkTip", "Attach the selected app to the selected environment.");
		})
		.IsEnabled_Lambda([this]()
		{
			return Controller.IsValid()
				&& Controller->GetSelectedAppId() != 0
				&& !Controller->GetSelectedEnvironmentSlug().IsEmpty()
				&& Controller->CanManageEnvironments();
		})
		.OnClicked(this, &SCrowdyEnvironmentsView::OnLinkClicked)
		[
			SNew(STextBlock).Text(LOCTEXT("LinkButton", "Link App to Environment"))
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(FSlateColor::UseForeground())
		]
	];

	if (!bEmbedded)
	{
		Actions->AddSlot().AutoWidth()
		[
			SNew(SButton)
			.ButtonStyle(&Style, "Crowdy.Button.Secondary")
			.ContentPadding(FMargin(13.0f, 7.0f))
			.ToolTipText(LOCTEXT("SyncEndpointsTip", "Write the selected app's endpoints into the project settings."))
			.IsEnabled_Lambda([this]() { return Controller.IsValid() && Controller->GetSelectedApp().IsValid(); })
			.OnClicked(this, &SCrowdyEnvironmentsView::OnSyncEndpointsClicked)
			[
				SNew(STextBlock).Text(LOCTEXT("SyncEndpointsButton", "Sync Endpoints"))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10)).ColorAndOpacity(FSlateColor::UseForeground())
			]
		];
	}

	Root->AddSlot().AutoHeight()[ Actions ];

	ChildSlot
	[
		Root
	];
}

SCrowdyEnvironmentsView::~SCrowdyEnvironmentsView()
{
	if (Controller.IsValid())
	{
		Controller->OnEnvironmentsChanged.RemoveAll(this);
	}
}

FReply SCrowdyEnvironmentsView::OnRefreshClicked()
{
	if (Controller.IsValid() && Controller->GetSelectedOrgId() != 0)
	{
		Controller->FetchEnvironments(Controller->GetSelectedOrgId());
	}
	return FReply::Handled();
}

FReply SCrowdyEnvironmentsView::OnLinkClicked()
{
	if (Controller.IsValid())
	{
		Controller->LinkAppToEnvironment(Controller->GetSelectedAppId(), Controller->GetSelectedEnvironmentSlug());
	}
	return FReply::Handled();
}

FReply SCrowdyEnvironmentsView::OnSyncEndpointsClicked()
{
	if (Controller.IsValid())
	{
		Controller->SyncConfig();
	}
	return FReply::Handled();
}

void SCrowdyEnvironmentsView::HandleEnvironmentsChanged()
{
	if (EnvListView.IsValid())
	{
		EnvListView->RequestListRefresh();
	}
}

TSharedRef<ITableRow> SCrowdyEnvironmentsView::MakeEnvRow(TSharedPtr<FStudioEnvironment> Env, const TSharedRef<STableViewBase>& OwnerTable)
{
	const FString Name = Env.IsValid() ? Env->DisplayName : FString();
	const FString Slug = Env.IsValid() ? Env->Slug : FString();
	const FString Status = Env.IsValid() ? Env->Status : FString();
	const FString Class = Env.IsValid() ? Env->EnvironmentClass : FString();

	return SNew(STableRow<TSharedPtr<FStudioEnvironment>>, OwnerTable)
		.Style(&FCrowdyStudioStyle::Get(), "Crowdy.TableRow")
		.Padding(FMargin(0.0f, 1.0f))
		[
			SNew(SBorder)
			.BorderImage(FStyleDefaults::GetNoBrush())
			.Padding(FMargin(11.0f, 8.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[ SNew(STextBlock).Text(FText::FromString(Name)).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.BodyStrong") ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ CrowdyStudioWidgets::Chip(FText::FromString(Slug)) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
						[ SNew(STextBlock).Text(FText::FromString(Class)).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.Subtle") ]
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					CrowdyStudioWidgets::Badge(FText::FromString(Status), CrowdyStudioWidgets::ToneForStatus(Status))
				]
			]
		];
}

void SCrowdyEnvironmentsView::OnEnvSelected(TSharedPtr<FStudioEnvironment> Env, ESelectInfo::Type SelectInfo)
{
	if (SelectInfo != ESelectInfo::Direct && Controller.IsValid() && Env.IsValid())
	{
		Controller->SelectEnvironment(Env->Slug);
	}
}

#undef LOCTEXT_NAMESPACE
