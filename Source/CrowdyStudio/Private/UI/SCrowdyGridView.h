// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Model/CrowdyStudioTypes.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Views/SListView.h"

class FCrowdyStudioController;
class SBox;
class SEditableTextBox;
class FActiveTimerHandle;
class UWorld;
class UObject;

// Grid pane: create world-region grids, scan a chunk region to find existing ones, and edit the
// selected grid's permission whitelist, per-user grants, and group grants. Grids have no list-all
// query, so they are discovered by scanning. Game plane, so it needs a game-capable token. Permission
// keys are picked as checkboxes from the runtimePermissions catalog rather than typed as comma lists.
class SCrowdyGridView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyGridView) {}
		SLATE_ARGUMENT(TSharedPtr<FCrowdyStudioController>, Controller)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SCrowdyGridView() override;

private:
	FReply OnCreateGridClicked();
	FReply OnResetCornersClicked();
	FReply OnScanClicked();
	FReply OnSetWhitelistClicked();
	FReply OnGrantUserClicked();
	FReply OnRevokeUserClicked();
	FReply OnReadUserClicked();
	FReply OnAssignGroupClicked();
	FReply OnRevokeGroupClicked();
	FReply OnListGroupGrantsClicked();

	void HandleGridsChanged();
	void HandleDetailChanged();
	void HandleWhitelistChanged();
	void HandlePermissionCatalogChanged();
	void HandleGridSelectionChanged(TSharedPtr<FStudioGrid> Grid, ESelectInfo::Type SelectInfo);

	// Effective-permissions simulator (B1): pick a tier + user, preview keys on the selected grid.
	void HandleTiersChanged();
	FReply OnSimulateClicked();
	void RebuildSimulator();

	// Visual grid authoring (B2).
	// B2-1: derive a grid from the bounds of the selected level actors (world -> chunk), confirm, create.
	FReply OnCreateFromSelectionClicked();
	FText GetSelectionNote() const;

	// Viewport visualization (B2-2 + live preview): draw the scanned grids AND the pending grid (from
	// the corner fields) as debug boxes in the active world - the PIE/Game world while playing, else the
	// editor world. Event-driven (corner edits, scans, selection, PIE start) and uses persistent lines so a
	// non-realtime editor viewport shows them too.
	void SetShowGridViz(bool bEnable);
	void RefreshGridVisualization();
	void ClearGridVisualization();
	void HandlePieEvent(bool bIsSimulating);
	// Read the six corner fields into a normalized low/high chunk box; false when they are all unset (0).
	bool TryGetCornerChunks(FStudioChunk& OutLow, FStudioChunk& OutHigh) const;
	// Bounds of the current editor selection as a low/high chunk box; false when nothing usable is
	// selected. Shared by the live "from selection" preview and the create-from-selection action.
	bool TryGetSelectionChunks(FStudioChunk& OutLow, FStudioChunk& OutHigh, double ChunkSize) const;
	// Editor selection changed; redraw so the "from selection" preview follows it live.
	void HandleSelectionChanged(UObject* Object);
	// Redraw the "from selection" preview only when the selection's chunk box actually changes (so a
	// drag within one chunk is free). Shared by the selection-changed hook and the move-tracking timer.
	void MaybeRedrawForSelection();
	// Low-rate poll that catches an already-selected actor being dragged into a different chunk; the
	// editor has no during-drag move delegate, and MaybeRedrawForSelection keeps idle ticks cheap.
	EActiveTimerReturnType PollSelectionForMove(double InCurrentTime, float InDeltaTime);

	// The world to draw into (PIE/Game if playing, else the editor world); bOutIsEditorWorld tells which.
	static UWorld* ResolveVizWorld(bool& bOutIsEditorWorld);
	// The running PIE/Game world, or null when nothing is playing.
	static UWorld* FindRunningGameWorld();
	// Chunk size from a running session if one exists, else the 1600 default; bOut tells which.
	double ResolveChunkSize(bool& bOutFromLiveSession) const;
	
	// Draw one chunk box (Low..High inclusive) into World as a persistent debug box + label.
	static void DrawChunkBox(UWorld* World, const FStudioChunk& Low, const FStudioChunk& High, double ChunkSize, const FColor& Color, float Thickness, const FString& Label);

	int64 SelectedGridId() const;
	FText GetSelectedGridLabel() const;
	FText GetWhitelistLabel() const;
	FText GetUserEffectiveLabel() const;

	// Permission pickers: a checkbox per catalog key, backed by a selection set. The rows are rebuilt
	// into the host box when the catalog loads; each checkbox's state binds live to the set, so syncing
	// the whitelist from the server is just a set mutation (no rebuild needed).
	TSharedRef<SWidget> BuildPermissionPicker(TSet<FString>* Selection);
	void RebuildPermissionPickers();
	static TArray<FString> SelectionToKeys(const TArray<FString>& CatalogOrder, const TSet<FString>& Selection);

	TSharedRef<ITableRow> MakeGridRow(TSharedPtr<FStudioGrid> Grid, const TSharedRef<STableViewBase>& OwnerTable);
	TSharedRef<ITableRow> MakeGrantRow(TSharedPtr<FStudioGridGroupGrant> Grant, const TSharedRef<STableViewBase>& OwnerTable);

	static int64 ParseInt(const TSharedPtr<SEditableTextBox>& Box);

	TSharedPtr<FCrowdyStudioController> Controller;
	TSharedPtr<SListView<TSharedPtr<FStudioGrid>>> GridListView;
	TSharedPtr<SListView<TSharedPtr<FStudioGridGroupGrant>>> GrantListView;

	TSharedPtr<SEditableTextBox> Corner1X, Corner1Y, Corner1Z, Corner2X, Corner2Y, Corner2Z;
	TSharedPtr<SEditableTextBox> ScanUserBox, ScanLowX, ScanLowY, ScanLowZ, ScanHighX, ScanHighY, ScanHighZ;
	TSharedPtr<SEditableTextBox> UserIdBox, GroupIdBox, GroupRoleBox;

	// Hosts holding the rebuilt checkbox wrap for each permission input, plus their backing selections.
	TSharedPtr<SBox> WhitelistPickerHost, UserKeysPickerHost, GroupKeysPickerHost;
	TSet<FString> WhitelistSelection, UserKeysSelection, GroupKeysSelection;

	// Effective-permissions simulator (what-if): pick a tier + user, preview keys on the selected grid.
	TSharedPtr<SEditableTextBox> SimUserBox;
	TSharedPtr<SComboBox<TSharedPtr<FStudioAccessTier>>> SimTierCombo;
	TSharedPtr<FStudioAccessTier> SimSelectedTier;
	TSharedPtr<SBox> SimResultHost;

	// B2-1: note shown beside the "create grid from selection" action.
	FText SelectionNote;
	// Viewport viz: master toggle (default on) + PIE-transition hooks so it follows the active world.
	bool bShowGridViz = true;
	// Selection -> chunks mode: true maps each actor to the single chunk its pivot is in (matches the
	// runtime); false covers every chunk the actor's collision bounds touch. Toggled in the UI.
	bool bSelectionByLocation = true;
	double EditTimeChunkSize = 1600.0;
	FDelegateHandle PieStartHandle;
	FDelegateHandle PieEndHandle;
	FDelegateHandle SelectionChangedHandle;
	// Move-tracking: low-rate timer + the last selection chunk box we drew, so the poll only redraws
	// when an actor actually crosses into a different chunk.
	TSharedPtr<FActiveTimerHandle> SelectionPollHandle;
	FStudioChunk LastSelLow, LastSelHigh;
	bool bHadSelectionBox = false;
};
