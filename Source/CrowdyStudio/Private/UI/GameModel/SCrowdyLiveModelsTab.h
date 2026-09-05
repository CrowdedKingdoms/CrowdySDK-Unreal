// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameModel/CrowdyModelLedger.h"
#include "UI/GameModel/SCrowdyModelSectionTable.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class FCrowdyStudioController;
class SBox;
class SEditableTextBox;
class STextBlock;
class SWidgetSwitcher;

// Asks for a model to be opened on the Models tab. Only the shell knows both tabs, so it carries the request
// between them; the model's type name travels in the call rather than being stored anywhere along the way.
DECLARE_DELEGATE_OneParam(FOnShowModelInBrowser, const FString& /*TypeName*/);

// One model in the Live tab's left-hand list. Built from the app's container types and rebuilt whenever those
// or the live instances change, so the status word beside a model is never computed while the list is painting.
struct FCrowdyLiveModelEntry
{
	FString TypeName;

	// DisplayName, falling back to TypeName when it is blank. A blank name in the list is never acceptable.
	FString Display;

	// "not loaded" until a read has covered this model, then "showing N". The server reports no count of any
	// kind, so N is what is in hand and nothing here ever claims to be a total.
	FString Status;
};

// Live tab: a master list of the app's models beside the live instances the server currently holds of the
// selected one, plus a confirm-gated delete of one instance. Reads the server directly, so it shows live state
// even outside Play. Game plane, so it needs a game-capable token.
class SCrowdyLiveModelsTab : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyLiveModelsTab) {}
		SLATE_ARGUMENT(TSharedPtr<FCrowdyStudioController>, Controller)
		// Fired when the user asks to see the open model on the Models tab.
		SLATE_EVENT(FOnShowModelInBrowser, OnShowModelInBrowser)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SCrowdyLiveModelsTab() override;

private:
	// The five columns of the live table. A column set is data, so the live table and the Models tables are the
	// same widget with different arrays rather than two table widgets that drift apart.
	static TArray<FCrowdyModelColumn> LiveInstanceColumns();

	static FString CountPhrase(int32 Count, const TCHAR* Singular, const TCHAR* Plural);

	// Master list
	TSharedRef<ITableRow> MakeModelRow(TSharedPtr<FCrowdyLiveModelEntry> Entry, const TSharedRef<STableViewBase>& OwnerTable);
	void OnModelSelectionChanged(TSharedPtr<FCrowdyLiveModelEntry> Entry, ESelectInfo::Type SelectInfo);
	void RebuildModelList();
	bool IsModelLoaded(const FString& TypeName) const;
	int32 CountLiveInstances(const FString& TypeName) const;
	// Record what a landed read covered. A read with no filters covers every model only when its page came back
	// short: a full page is the edge of what was asked for, so anything past it was never looked at. A read that
	// replaced the list stops covering whatever the read before it did.
	void MarkTypesRead(const FString& TypeFilter, const FString& SessionFilter, bool bAppend);
	// Remember that a landed read covered this model. Type names are server keys, so the match is case-sensitive.
	void AddLoadedType(const FString& TypeName);

	// Buttons
	FReply OnRefreshClicked();
	FReply OnLoadMoreClicked();
	FReply OnShowInModelsClicked();
	FReply OnDeleteInstanceClicked();

	// Ask for one page of live instances under the filters as they are typed right now. bAppend asks for the page
	// after the ones already held; it is honoured only while the filters still match the ones those pages were
	// read under, since a later page of a different query says nothing about the rows on screen.
	void RequestContainers(bool bAppend);

	// Controller signals
	void HandleContainerTypesChanged();
	void HandleContainersChanged();
	void HandleContainerStateChanged();
	// The active app changed and its token was minted.
	void HandleAppChanged();

	void OnInstanceSelectionChanged(TSharedPtr<FCrowdyModelRow> Row, ESelectInfo::Type SelectInfo);

	// Detail pane
	void RefreshInstanceTable();
	void UpdateDetailHeader();
	void UpdateActionBar();
	void UpdateStatusLine();
	void SetInspectorLine(const FText& Line);

	// Notice that this tab's contents describe an app other than the selected one, and empty them if so. Called
	// both from the app-changed announcement and from the list refreshes the switch itself triggers, because the
	// announcement waits on the new app's token being minted and a mint can fail, leaving this tab stale with no
	// notification at all. Comparing the app id makes running it twice a no-op.
	void SyncEditorAppScope();
	// Empty everything this tab keeps for itself. The container list is the controller's and it has already
	// emptied it; what survives an app switch is exactly what is held here.
	void ClearAppScopedEditors();

	TSharedPtr<FCrowdyStudioController> Controller;

	FOnShowModelInBrowser ShowModelInBrowser;

	// The app the filters, the open model, the table and the inspector line below describe. A container id means
	// nothing outside the app it was listed for, so a delete must be aimed at the app its target was read from
	// rather than at whatever happens to be selected when the button is pressed. Zero means nothing has been
	// loaded for an app yet.
	int64 EditorAppId = 0;

	// The model whose instances the right-hand pane shows, held by name rather than by row pointer: every
	// rebuild of the list produces a fresh set of objects.
	TOptional<FString> SelectedTypeName;

	// The models a landed read has covered. "Nothing came back" and "nobody has asked yet" are different answers
	// and the table shows a different message for each, so an empty result has to be distinguishable from an
	// absent one.
	TArray<FString> LoadedTypeNames;
	bool bLoadedEveryType = false;

	// A read this tab issued is in flight, and what it was issued with. Only a read of its own tells it which
	// models the answer covers, and the boxes are free to be typed into while it is in flight.
	bool bReadPending = false;
	FString PendingTypeFilter;
	FString PendingSessionFilter;
	bool bPendingAppend = false;

	// The filters the pages currently held were read under, promoted from the pending pair only when a read
	// actually lands. A read that failed added no rows, so the next page has to be the one it did not deliver,
	// and a box edited since has to start a new sequence rather than continue this one.
	FString LoadedTypeFilter;
	FString LoadedSessionFilter;

	TArray<TSharedPtr<FCrowdyLiveModelEntry>> ModelEntries;

	TSharedPtr<SListView<TSharedPtr<FCrowdyLiveModelEntry>>> ModelListView;
	// What an empty model list says. Which of four situations it is in comes from the read's own state, so this is
	// written when the list is rebuilt rather than fixed when the widget is built.
	TSharedPtr<STextBlock> ModelListPlaceholderText;
	TSharedPtr<STextBlock> ModelListFooterText;
	TSharedPtr<SEditableTextBox> ContainerTypeFilterBox;
	TSharedPtr<SEditableTextBox> ContainerSessionFilterBox;

	TSharedPtr<SWidgetSwitcher> DetailSwitcher;
	TSharedPtr<STextBlock> DetailTitleText;
	TSharedPtr<SBox> DetailBadgeBox;
	TSharedPtr<SCrowdyModelSectionTable> InstanceTable;
	TSharedPtr<STextBlock> InspectorText;
	TSharedPtr<STextBlock> SelectionCountText;
	TSharedPtr<STextBlock> StatusLineText;
};
