// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameModel/CrowdyModelLedger.h"
#include "GameModel/CrowdyModelLoadState.h"
#include "Model/CrowdyStudioTypes.h" // FStudioContainerState
#include "UI/GameModel/SCrowdyModelSectionTable.h" // FCrowdyModelColumn, SCrowdyModelSectionTable
#include "Widgets/SCompoundWidget.h"

class FCrowdyStudioController;
class SSearchBox;
class STextBlock;

// One live model's stored values, an attribute per row, with what it holds beside it.
//
// It reads nothing itself: the tab that owns it hands it a container state and the state of the read behind it,
// and the declared attributes and the plan snapshot are asked of the controller at the moment the rows are built,
// so a vocabulary that lands after the values does not need the caller to pass anything new.
class SCrowdyPropertyInspector : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyPropertyInspector) {}
		SLATE_ARGUMENT(TSharedPtr<FCrowdyStudioController>, Controller)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	// Show one live model. ValuesState is what the read behind State is doing, because an invalid state means
	// "nobody asked", "still reading" and "the read failed" and a panel that shows one message over all three
	// reports a server error as an instance with nothing in it.
	void Show(ECrowdyModelLoadState ValuesState, const FStudioContainerState& State, const FString& InstanceLabel);

	// Rebuild from what is held, for when the model's attributes land after its values did.
	void Refresh();

	// Drop everything held. The app switched, or the instance on screen is no longer listed.
	void Clear();

private:
	static TArray<FCrowdyModelColumn> PropertyColumns();

	void Rebuild();
	void UpdateFooter();

	FReply OnCopyValueClicked();
	FReply OnCopyAllClicked();

	FString SearchQuery() const;

	TSharedPtr<FCrowdyStudioController> Controller;

	// The live model on screen, held by value: the controller replaces its own copy on the next read, and a
	// reference to that would describe a different instance from the one these rows were built from.
	FStudioContainerState HeldState;
	FString HeldLabel;
	ECrowdyModelLoadState ValuesState = ECrowdyModelLoadState::NeverRequested;

	// Every row the instance yields, before the search box narrows them. Held so that typing filters what is
	// already built instead of re-parsing the values on every keystroke.
	TArray<FCrowdyModelRow> AllRows;

	// The runtime's own bookkeeping keys are out of the way until a reader asks for them.
	bool bShowInternal = false;

	// What the footer says until the next rebuild, when a copy has just happened. Empty the rest of the time,
	// which is when the counts speak for themselves.
	FText CopyNotice;

	TSharedPtr<SCrowdyModelSectionTable> Table;
	TSharedPtr<SSearchBox> SearchBox;
	TSharedPtr<STextBlock> TitleText;
	TSharedPtr<STextBlock> SubtitleText;
	TSharedPtr<STextBlock> FooterText;
};
