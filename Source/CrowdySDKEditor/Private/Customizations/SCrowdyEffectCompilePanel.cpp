// Fill out your copyright notice in the Description page of Project Settings.

#include "Customizations/SCrowdyEffectCompilePanel.h"

#include "Customizations/CrowdyEffectCompileSummary.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SCrowdyEffectCompilePanel"

void SCrowdyEffectCompilePanel::Construct(const FArguments& InArgs)
{
	Effect = InArgs._Effect;

	// A splitter rather than fixed proportions: a short compile readout and a long wire payload want very different
	// shares of the panel, and which one matters is the author's call, not ours.
	ChildSlot
	[
		SNew(SSplitter)
		.Orientation(Orient_Horizontal)

		+ SSplitter::Slot()
		.Value(0.4f)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(6.f, 6.f, 6.f, 2.f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("CompileLabel", "Compile Preview"))
				.Font(FAppStyle::GetFontStyle("PropertyWindow.BoldFont"))
			]

			+ SVerticalBox::Slot()
			.FillHeight(1.f)
			.Padding(6.f, 0.f, 6.f, 6.f)
			[
				SNew(SMultiLineEditableTextBox)
				.IsReadOnly(true)
				.AutoWrapText(true)
				.AlwaysShowScrollbars(true)
				.Text(this, &SCrowdyEffectCompilePanel::GetSummaryText)
			]
		]

		+ SSplitter::Slot()
		.Value(0.6f)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(6.f, 6.f, 6.f, 2.f)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.f, 0.f, 8.f, 0.f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("PayloadLabel", "Deploy Payload"))
					.ToolTipText(LOCTEXT("PayloadLabelTip", "Exactly what deploying this effect sends to the server."))
					.Font(FAppStyle::GetFontStyle("PropertyWindow.BoldFont"))
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SSegmentedControl<CrowdyEffectPayloadPreview::EMode>)
					.Value_Lambda([this]() { return PayloadMode; })
					.OnValueChanged_Lambda([this](CrowdyEffectPayloadPreview::EMode NewMode) { PayloadMode = NewMode; })
					+ SSegmentedControl<CrowdyEffectPayloadPreview::EMode>::Slot(CrowdyEffectPayloadPreview::EMode::Summary)
						.Text(LOCTEXT("PayloadSummary", "Summary"))
					+ SSegmentedControl<CrowdyEffectPayloadPreview::EMode>::Slot(CrowdyEffectPayloadPreview::EMode::WireJson)
						.Text(LOCTEXT("PayloadWireJson", "Wire JSON"))
				]
			]

			+ SVerticalBox::Slot()
			.FillHeight(1.f)
			.Padding(6.f, 0.f, 6.f, 6.f)
			[
				SNew(SMultiLineEditableTextBox)
				.IsReadOnly(true)
				.AutoWrapText(false)
				.AlwaysShowScrollbars(true)
				.Text(this, &SCrowdyEffectCompilePanel::GetPayloadText)
			]
		]
	];

	Refresh();
}

void SCrowdyEffectCompilePanel::Refresh()
{
	const UCrowdyEffect* Edited = Effect.Get();
	if (!Edited)
	{
		CachedSummary.Reset();
		CachedPayloadSummary.Reset();
		CachedPayloadWireJson.Reset();
		return;
	}

	CachedSummary = CrowdyEffectCompileSummary::BuildSummary(Edited);

	const FCrowdyEffectLoweringResult Result = Edited->Compile();
	const FString EffectiveName = Edited->GetEffectiveFunctionName();
	CachedPayloadSummary = CrowdyEffectPayloadPreview::BuildPayloadPreview(
		Result, EffectiveName, CrowdyEffectPayloadPreview::EMode::Summary);
	CachedPayloadWireJson = CrowdyEffectPayloadPreview::BuildPayloadPreview(
		Result, EffectiveName, CrowdyEffectPayloadPreview::EMode::WireJson);
}

FText SCrowdyEffectCompilePanel::GetSummaryText() const
{
	return FText::FromString(CachedSummary);
}

FText SCrowdyEffectCompilePanel::GetPayloadText() const
{
	return FText::FromString(
		PayloadMode == CrowdyEffectPayloadPreview::EMode::WireJson ? CachedPayloadWireJson : CachedPayloadSummary);
}

#undef LOCTEXT_NAMESPACE
