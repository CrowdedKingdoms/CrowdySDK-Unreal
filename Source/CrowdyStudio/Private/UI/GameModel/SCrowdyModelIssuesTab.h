// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Model/CrowdyStudioTypes.h" // FStudioLintFinding
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class FCrowdyStudioController;
class STextBlock;
class SWidgetSwitcher;

// Issues tab: what the server's gameModelLint says is wrong with the selected app's game model, one row per
// finding. Read-only; every finding names an object authored on another tab.
class SCrowdyModelIssuesTab : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyModelIssuesTab) {}
		SLATE_ARGUMENT(TSharedPtr<FCrowdyStudioController>, Controller)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SCrowdyModelIssuesTab() override;

private:
	TSharedRef<ITableRow> MakeFindingRow(TSharedPtr<FStudioLintFinding> Finding,
		const TSharedRef<STableViewBase>& OwnerTable);

	// Rebuild the rows and the header from the controller's report. Never called from paint.
	void RebuildFromReport();

	FReply OnRelintClicked();

	TSharedPtr<FCrowdyStudioController> Controller;

	// Copies, because a re-lint replaces the controller's array wholesale under a live list view.
	TArray<TSharedPtr<FStudioLintFinding>> Findings;

	TSharedPtr<SListView<TSharedPtr<FStudioLintFinding>>> FindingsList;
	TSharedPtr<SWidgetSwitcher> BodySwitcher;
	TSharedPtr<STextBlock> SummaryText;
	TSharedPtr<STextBlock> EmptyText;
};
