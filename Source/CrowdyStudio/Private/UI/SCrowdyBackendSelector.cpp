// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/SCrowdyBackendSelector.h"

#include "Model/FCrowdyStudioController.h"
#include "Style/CrowdyStudioStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyle.h"
#include "UI/CrowdyStudioWidgets.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CrowdyStudio"

void SCrowdyBackendSelector::Construct(const FArguments& InArgs)
{
	Controller = InArgs._Controller;

	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	ChildSlot
	[
		CrowdyStudioWidgets::Card(
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
			[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("BackendHeader", "Backend"), TEXT("server")) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
			[
				CrowdyStudioWidgets::Field(LOCTEXT("BackendModeLabel", "Crowdy backend"),
					CrowdyStudioWidgets::SegmentedEnum(
						{ TEXT("Dev"), TEXT("Prod"), TEXT("Custom") },
						{ LOCTEXT("BackendDev", "Dev (shared)"), LOCTEXT("BackendProd", "Production"), LOCTEXT("BackendCustom", "Custom") },
						TAttribute<FString>::CreateLambda([this]() { return Controller.IsValid() ? Controller->GetBackendMode() : FString(); }),
						[this](const FString& V) { if (Controller.IsValid()) { Controller->SetBackendMode(V); } }),
					LOCTEXT("BackendModeHint", "Dev and Production use built-in hosts. Custom lets you set your own Management API URL."))
			]
			// Custom management URL, shown only when Custom is selected.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
			[
				SNew(SBox)
				.Visibility_Lambda([this]() { return (Controller.IsValid() && Controller->GetBackendMode() == TEXT("Custom")) ? EVisibility::Visible : EVisibility::Collapsed; })
				[
					CrowdyStudioWidgets::Field(LOCTEXT("CustomMgmtLabel", "Management API URL"),
						SNew(SEditableTextBox)
						.Style(&Style, "Crowdy.Input")
						.HintText(LOCTEXT("CustomMgmtHint", "https://api.your-host.com"))
						.Text(Controller.IsValid() ? FText::FromString(Controller->GetCustomManagementUrl()) : FText::GetEmpty())
						.OnTextCommitted_Lambda([this](const FText& NewText, ETextCommit::Type) { if (Controller.IsValid()) { Controller->SetCustomManagementUrl(NewText.ToString()); } }))
				]
			]
			// The resulting management URL, read-only.
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[ SNew(STextBlock).Text(LOCTEXT("EffMgmtLabel", "Management URL")).TextStyle(&Style, "Crowdy.Text.Subtle") ]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[ SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Mono", 9)).AutoWrapText(true).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSecondary())).Text_Lambda([this]() { return Controller.IsValid() ? FText::FromString(Controller->GetEffectiveManagementUrl()) : FText::GetEmpty(); }) ]
			],
			FMargin(16.0f, 14.0f))
	];
}

#undef LOCTEXT_NAMESPACE
