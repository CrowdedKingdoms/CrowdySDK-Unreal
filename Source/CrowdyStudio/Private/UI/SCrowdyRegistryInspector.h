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

/** One CrowdyState replicated property (a flattened FCrowdyBakedRepProperty) for display. */
struct FCrowdyRepPropRowItem
{
	FString ClassShort;         // friendly trailing name, e.g. "BP_Door_C"
	FString ClassPath;          // full path, used for grouping/sorting and shown as a tooltip
	int64   LayoutHash = 0;     // this class's layout hash (from RepLayoutHashes)
	FString PropertyName;
	int64   PropertyID = 0;
	bool    bOwnerOnly = false;
	bool    bManualDirty = false;
	FString OnRepFunction;      // empty when NAME_None
	int32   LayoutOrder = 0;

	// Class-level CrowdyState host-authority defaults, resolved LIVE (never baked - the registry has no
	// per-instance data model) from the class's default UCrowdyEntityComponent, same value duplicated
	// across every row of this class. Empty when the class has no entity component or isn't currently
	// loaded; a placed level instance can still override these per-instance.
	bool  bHasEntityDefaults = false;
	FText OwnershipText;        // e.g. "Local Client" or "Host"
	FText HostOverrideText;     // e.g. "Allow" / "Owner Only"; empty when Ownership==Host (mirrors the
	                            // component's own EditConditionHides) or when not resolved
};

using FCrowdyRepPropRowItemPtr = TSharedPtr<FCrowdyRepPropRowItem>;

/**
 * Everything one declaring class contributes to the registry - its RPC functions AND its CrowdyState
 * replicated properties - gathered into a single group. This is the unit both views render: one card
 * per group (Cards view) or one subheadered block of table rows per group (Table view). Grouping the
 * two member kinds by class is what removes the old clutter of a class appearing in two separate lists.
 */
struct FCrowdyClassGroup
{
	FString ClassShort;
	FString ClassPath;
	int64   LayoutHash = 0;             // 0 when the class declares no replicated properties
	bool    bHasRepProps = false;
	bool    bHasEntityDefaults = false; // class-level Ownership/HostOverride were resolvable (see above)
	FText   OwnershipText;
	FText   HostOverrideText;
	TArray<FCrowdyRpcRowItemPtr>     Functions;   // class-then-name sorted
	TArray<FCrowdyRepPropRowItemPtr> Props;       // class-then-layout-order sorted
};

using FCrowdyClassGroupPtr = TSharedPtr<FCrowdyClassGroup>;

/** How the per-class metadata is laid out: rich expandable cards, or a dense column-aligned table. */
enum class ECrowdyRegistryViewMode : uint8
{
	Cards,
	Table
};

/**
 * Read-only inspector for the baked Crowdy metadata registry (UCrowdyBakedRegistry), drawn in the
 * CrowdyStudio design language so it reads as one of the console's pages.
 *
 * Metadata is grouped by declaring class: each class becomes one entry showing both its RPC functions
 * and its CrowdyState replicated properties together (a class no longer appears in two separate lists).
 * A Cards/Table toggle switches between rich expandable cards - one per class, each member a row whose
 * details expand vertically - and a dense table where every member is one column-aligned row for quick
 * scanning. A class card/subheader also surfaces that class's default Ownership / Host Override
 * (host-authority settings on UCrowdyEntityComponent), resolved LIVE from the class's default component
 * when the view is built - display-only, never written into the baked asset, since those settings are
 * per-placed-instance and have no single "the" value for a class.
 *
 * Above the grouped list are the persistent / singleton struct lists, a search filter, and two actions:
 *   - Rebuild (Deep Scan) - regenerate the registry from all current project metadata. The actual bake
 *     lives in the editor-only baker, supplied via CrowdyStudioRegistry::SetRebuildHook; the button is
 *     disabled when no hook is registered.
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

	// Reloads the resolved asset into the row arrays and rebuilds the content.
	void RefreshData();

	// Filters + regroups the rows and re-renders ContentContainer for the active view mode.
	void RebuildContent();

	// Unions AllRpcRows / AllRepRows into per-class groups, honouring the current search filter.
	TArray<FCrowdyClassGroupPtr> BuildFilteredGroups() const;

	// The two render paths (both fill ContentContainer).
	void BuildCardsInto(const TArray<FCrowdyClassGroupPtr>& Groups);
	void BuildTableInto(const TArray<FCrowdyClassGroupPtr>& Groups);

	void OnSearchTextChanged(const FText& NewText);
	void SetViewMode(ECrowdyRegistryViewMode Mode);

	// Cards view: one unified card per class.
	TSharedRef<SWidget> MakeClassCard(const FCrowdyClassGroupPtr& Group);
	TSharedRef<SWidget> MakeClassMetaStrip(const FCrowdyClassGroupPtr& Group) const;
	TSharedRef<SWidget> MakeFunctionEntry(const FCrowdyRpcRowItemPtr& Fn);
	TSharedRef<SWidget> MakeRepPropertyEntry(const FCrowdyRepPropRowItemPtr& Prop);

	// Table view: a shared column skeleton, one class subheader per group, one row per member.
	TSharedRef<SWidget> MakeTableClassSubheader(const FCrowdyClassGroupPtr& Group) const;
	TSharedRef<SWidget> MakeTableRpcRow(const FCrowdyRpcRowItemPtr& Fn) const;
	TSharedRef<SWidget> MakeTableRepRow(const FCrowdyRepPropRowItemPtr& Prop) const;

	// Struct list-view row factory (shared by both struct sections).
	TSharedRef<ITableRow> OnGenerateStructRow(TSharedPtr<FString> Item, const TSharedRef<STableViewBase>& OwnerTable);

	// Bound text getters.
	FText GetStatusText() const;
	FText GetPersistentTitle() const;
	FText GetSingletonTitle() const;

	TArray<FCrowdyRpcRowItemPtr>     AllRpcRows;   // sorted by class then function
	TArray<FCrowdyRepPropRowItemPtr> AllRepRows;   // sorted by class then layout order
	TArray<TSharedPtr<FString>>      PersistentStructItems;
	TArray<TSharedPtr<FString>>      SingletonStructItems;
	int32                            NumClasses = 0; // distinct classes across RPC + rep rows

	TSharedPtr<SVerticalBox>                    ContentContainer;  // holds the per-class cards or the table
	TSharedPtr<SListView<TSharedPtr<FString>>>  PersistentStructListView;
	TSharedPtr<SListView<TSharedPtr<FString>>>  SingletonStructListView;
	TSharedPtr<SEditableTextBox>                 SearchBox;

	FString SearchText;
	FString ResolvedAssetPath;
	bool    bResolved = false;
	ECrowdyRegistryViewMode ViewMode = ECrowdyRegistryViewMode::Cards;

	// True from clicking Rebuild until the async bake finishes; drives the in-progress spinner and
	// disables the Rebuild button so it can't be re-triggered mid-bake.
	bool    bRebuildInProgress = false;
};
