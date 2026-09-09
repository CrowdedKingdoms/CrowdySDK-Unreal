// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class FCrowdyStudioController;
class SComboButton;
class SMenuAnchor;
class SWidget;

// Page-header strip for the Game Model view: the three setup readiness pills, how much the last check found,
// and the Preview/Sync buttons for reflecting CrowdyContainer classes and Crowdy Effect assets into the server
// schema. The bulk "Remove Server-Only" prune lives in the Models tab's delete review instead; it is a delete,
// not a sync control.
//
// The two things a reader consults rather than acts on, the plan's prose report and the key to the Models tab's
// marks, open as panels over the page rather than as part of this strip. Both want a few hundred pixels when
// they are open and none at all the rest of the time, and this strip sits above a filling tab area that does
// not scroll, so anything given a permanent place here is taken from the browser below for good.
//
// Sync to Server opens the apply review over the page for the same reason, and it is the ONE confirmation that
// write has. There is deliberately no second confirm behind it: two suppressing sites mean no single change can
// prove either of them is doing the work, so the one that can show what is about to be written, let a reader
// hold part of it back and refuse a plan that moved under them is the only one kept.
class SCrowdyReconcileStrip : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyReconcileStrip) {}
		SLATE_ARGUMENT(TSharedPtr<FCrowdyStudioController>, Controller)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SCrowdyReconcileStrip() override;

private:
	FReply OnPreviewClicked();
	FReply OnSyncClicked();

	// Built when the panel is opened rather than held: the report is rebuilt from the controller each time, and a
	// panel nobody opens costs nothing.
	TSharedRef<SWidget> MakeDetailsPanel();
	TSharedRef<SWidget> MakeMarksPanel();
	// Likewise built per press, so the review always opens on the plan that is pending now and a selection can
	// never outlive the plan it was made against.
	TSharedRef<SWidget> MakeApplyPanel();

	// Refreshes CachedCountLine and notes whether the new report is worth reading. Bound to the controller's
	// report-changed broadcast so neither string is rebuilt per paint.
	void HandleSchemaSyncReportChanged();

	FText GetCachedCountLineText() const;
	FText GetSchemaSyncReportText() const;
	FText GetDetailsButtonLabel() const;
	// The Schema pill's advisory word. Two different states reach Advisory (server-only entities to review, and a
	// plan that never read the server at all), and one fixed string would misdescribe whichever it is not.
	FText GetSchemaAdvisoryWord() const;

	TSharedPtr<FCrowdyStudioController> Controller;

	// Refreshed only on OnSchemaSyncReportChanged, not rebuilt per paint.
	FString CachedCountLine;

	// Whether the last report carried a warning or a status note. It marks the button rather than opening the
	// panel: this panel now floats over the page, and a panel that appears unbidden over what someone is reading
	// is an interruption, where the inline version it replaces was merely an expansion. The attention is kept,
	// the hijack is not.
	bool bDetailsWorthReading = false;

	TSharedPtr<SComboButton> DetailsButton;
	TSharedPtr<SComboButton> MarksButton;

	// The apply review's anchor. Held so the review can close itself once it has issued a send, and so the primary
	// button keeps its own look rather than becoming a drop-down.
	TSharedPtr<SMenuAnchor> ApplyAnchor;
};
