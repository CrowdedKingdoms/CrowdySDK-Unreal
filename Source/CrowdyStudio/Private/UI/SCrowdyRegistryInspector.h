// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class ITableRow;
class STableViewBase;
class SEditableTextBox;
class SVerticalBox;
template <typename ItemType> class SListView;

/** One CrowdyEvent/RPC function (a flattened FCrowdyBakedRpcFunction) for display. */
struct FCrowdyRpcRowItem
{
	FString ClassShort;     // friendly trailing name, e.g. "BP_Door_C"
	FString ClassPath;      // full path, used for grouping/sorting and shown as a tooltip
	FString Function;
	int64   FunctionID = 0;
	FText   Recipient;      // resolved display names (UMETA DisplayName)
	FText   Decay;
	FText   Distance;
	bool    bParamsPOD = false;
	bool    bReplicated = false;
};

using FCrowdyRpcRowItemPtr = TSharedPtr<FCrowdyRpcRowItem>;

/**
 * Read-only inspector for the baked Crowdy metadata registry (UCrowdyBakedRegistry), drawn in the
 * CrowdyStudio design language so it reads as one of the console's pages.
 *
 * RPC functions are shown as one collapsible card per declaring class; inside each card every
 * function is an expandable row whose details (FunctionID, routing, POD, replicated) appear
 * stacked vertically. Above the cards are the persistent / singleton struct lists and a search
 * filter. Two actions:
 *   - Rebuild (Deep Scan) - regenerate the registry from all current project metadata. The actual
 *     bake lives in the editor-only baker, supplied via CrowdyStudioRegistry::SetRebuildHook; the
 *     button is disabled when no hook is registered.
 *   - Refresh View        - re-read the asset without re-baking.
 *
 * Editor/PIE read live metadata (not this asset), so this is a preview of what gets baked into a
 * packaged build.
 */
class SCrowdyRegistryInspector : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyRegistryInspector) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	// Toolbar actions.
	FReply OnRebuildClicked();
	FReply OnRefreshClicked();

	// Reloads the resolved asset into the row arrays and rebuilds every view.
	void RefreshData();

	// Rebuilds the per-class cards from AllRpcRows, honouring the current search filter.
	void BuildCardList();
	void OnSearchTextChanged(const FText& NewText);

	// Card / row builders.
	TSharedRef<SWidget> MakeClassCard(const FString& ClassShort, const FString& ClassPath,
		const TArray<FCrowdyRpcRowItemPtr>& Functions);
	TSharedRef<SWidget> MakeFunctionEntry(const FCrowdyRpcRowItemPtr& Fn);

	// Struct list-view row factory (shared by both struct sections).
	TSharedRef<ITableRow> OnGenerateStructRow(TSharedPtr<FString> Item, const TSharedRef<STableViewBase>& OwnerTable);

	// Bound text getters.
	FText GetStatusText() const;
	FText GetPersistentTitle() const;
	FText GetSingletonTitle() const;

	TArray<FCrowdyRpcRowItemPtr> AllRpcRows;   // sorted by class then function
	TArray<TSharedPtr<FString>>  PersistentStructItems;
	TArray<TSharedPtr<FString>>  SingletonStructItems;

	TSharedPtr<SVerticalBox>                    CardContainer;  // holds the per-class cards
	TSharedPtr<SListView<TSharedPtr<FString>>>  PersistentStructListView;
	TSharedPtr<SListView<TSharedPtr<FString>>>  SingletonStructListView;
	TSharedPtr<SEditableTextBox>                 SearchBox;

	FString SearchText;
	FString ResolvedAssetPath;
	bool    bResolved = false;

	// True from clicking Rebuild until the async bake finishes; drives the in-progress spinner and
	// disables the Rebuild button so it can't be re-triggered mid-bake.
	bool    bRebuildInProgress = false;
};
