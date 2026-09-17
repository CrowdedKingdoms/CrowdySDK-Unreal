// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Model/CrowdyStudioTypes.h"
#include "Widgets/SCompoundWidget.h"

class FCrowdyStudioController;
class SEditableTextBox;
class SMultiLineEditableTextBox;
class SVerticalBox;
class SWidgetSwitcher;

DECLARE_DELEGATE_OneParam(FOnStudioAppCreated, int64 /*AppId*/);

// The two-step create-app sheet, drawn over the Project page: name and datacenter first, then a review
// with the permanent-placement note and the org's free-slot count. Mirrors the web console's wizard so
// the two never ask for different things. Open() resets it for an org; it closes itself on cancel or
// once the server has answered.
class SCrowdyCreateAppDialog : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyCreateAppDialog) {}
		SLATE_ARGUMENT(TSharedPtr<FCrowdyStudioController>, Controller)
		SLATE_EVENT(FSimpleDelegate, OnClosed)
		SLATE_EVENT(FOnStudioAppCreated, OnCreated)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SCrowdyCreateAppDialog() override;

	void Open(int64 InOrgId);

private:
	TSharedRef<SWidget> BuildAppStep();
	TSharedRef<SWidget> BuildReviewStep();
	void RebuildDatacenterList();

	FReply OnContinueClicked();
	FReply OnBackClicked();
	FReply OnCreateClicked();
	FReply OnCancelClicked();

	FString CurrentSlug() const;
	FString OrgName() const;
	// Apps in the org that still hold a free slot (everything not archived).
	int32 OrgAppCount() const;

	TSharedPtr<FCrowdyStudioController> Controller;
	FSimpleDelegate OnClosed;
	FOnStudioAppCreated OnCreated;

	TSharedPtr<SWidgetSwitcher> StepSwitcher;
	TSharedPtr<SEditableTextBox> NameBox;
	TSharedPtr<SEditableTextBox> SlugBox;
	TSharedPtr<SMultiLineEditableTextBox> DescriptionBox;
	TSharedPtr<SVerticalBox> DatacenterList;

	int64 OrgId = 0;
	int32 Step = 0;
	FString SelectedDatacenter;
	// Set once the user types in the identifier box; until then it follows the name.
	bool bSlugEdited = false;
	bool bSubmitting = false;
	// Bumped by Open(); a create reply carrying an older serial belongs to a cancelled form and is ignored.
	int32 SubmitSerial = 0;
	FText Error;
	// Set around the programmatic slug write, so the identifier box does not count it as the user typing.
	bool bSyncingSlug = false;
};
