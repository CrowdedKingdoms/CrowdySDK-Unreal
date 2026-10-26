#pragma once

#include "CoreMinimal.h"
#include "CrowdyExecDeploy.h"
#include "CrowdyExecDeveloperApi.h"
#include "CrowdyExecRevisions.h"
#include "UI/CrowdyStudioWidgets.h"
#include "UObject/SoftObjectPath.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class FCrowdyServerComputeService;
class SBox;
class SEditableTextBox;
class SMultiLineEditableTextBox;
class SVerticalBox;
class SWidgetSwitcher;
class UCrowdyServerObjectDefinition;
enum class ECrowdyServerObjectVisibility : uint8;

namespace CrowdyServerComputePage
{
	/** Why removing a type must leave its crate folder, or empty when the folder may be deleted: it must exist, be the type's own (valid Type Name, not shared) directly inside ServerDirectory, and hold no link. */
	FText WhyCrateFolderStays(const FString& TypeName, const FString& CrateDirectory, const FString& ServerDirectory, bool bSharedTypeName);

	struct FRemovalEffects
	{
		/** The type was named by the Game Kit World or Game Kit Actor Starters setting, which no longer names it. */
		bool bSettingsCleared = false;
		/** False when those settings changed but the project's config file could not be written, so it still names the type. */
		bool bSettingsSaved = true;
		/** Why the Server Code folder is still there or could not be deleted; empty when it was deleted or never existed. */
		FText FolderProblem;
	};

	/** What follows deleting a type's definition asset, whether from the page or from -op=removetype: takes the asset out of the Game Kit World and Game Kit Actor Starters settings (saving the project's config unless bSaveSettings is false), then deletes the type's Server Code folder when bDeleteFolder is, which the caller sets from WhyCrateFolderStays being empty. Type and ShownFolder are for the messages. */
	FRemovalEffects FinishRemoval(const FText& Type, const FString& TypeName, const FSoftObjectPath& Asset, const FString& CrateDirectory, const FString& ServerDirectory,
		bool bSharedTypeName, const FString& ShownFolder, bool bDeleteFolder, bool bSaveSettings = true);

	/** The Activity tab's windows, in minutes as the stats query takes them. */
	inline const TCHAR* const WindowHour = TEXT("60");
	inline const TCHAR* const WindowDay = TEXT("1440");
	inline const TCHAR* const WindowWeek = TEXT("10080");

	/** The Activity window named hour, day or week, as the tab stores it; empty for any other name. */
	FString FindActivityWindow(const FString& Name);

	/** The most verbose level the Logs tab's filter shows (0 error, 1 warning, 2 info), -1 for the filter all, which sets no limit. False, with OutMaxLevel -1, for a name that is not one of all, info, warnings or errors. */
	bool FindLogMaxLevel(const FString& Filter, int32& OutMaxLevel);

	/** Who can read an instance, as a type row says it for the definition's Readable By. */
	FText ReadableByPhrase(ECrowdyServerObjectVisibility Visibility);

	/** How many instances a Server Function Library runs, as a type row says it: 1 to 4 instances, or 4 instances when the two are equal. */
	FText InstancesPhrase(int32 MinInstances, int32 MaxInstances);

	struct FChangeBadge
	{
		FText Label;
		CrowdyStudioWidgets::EBadgeTone Tone = CrowdyStudioWidgets::EBadgeTone::Neutral;
		FText ToolTip;
	};

	/** One badge for a type: what stops its code from building wins, then how it compares with the live version. */
	FChangeBadge ChooseChangeBadge(const CrowdyExecDeploy::FTypeState& Type, const CrowdyExecRevisions::FTypeChange* Change);

	enum class EServiceRefresh : uint8
	{
		RebuildPage,
		RefillRows,
		KeepRows
	};

	/** What a change of the service rebuilds: the whole page for a new client, the rows only when the service's data moved on, else nothing that could close an open menu. */
	EServiceRefresh ChooseServiceRefresh(bool bContentBuilt, bool bNewClient, uint32 DataGeneration, uint32 FilledGeneration);
}

/** The Server Compute page of Crowdy Studio: deploys the project's Server Object types, and shows the app's versions, logs and call activity. */
class SCrowdyServerComputePanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyServerComputePanel) {}
	SLATE_END_ARGS()

	virtual ~SCrowdyServerComputePanel() override;

	void Construct(const FArguments& InArgs);

private:
	EActiveTimerReturnType HandleFirstShown(double InCurrentTime, float InDeltaTime);
	/** Renews the service's client when the sign-in, app or backend changed, and reads everything again; true when it did. */
	bool RenewIfStale();
	void OnServiceChanged();
	void UpdateDeployState();
	void ResetPageState();
	void RebuildContent();

	TSharedRef<SWidget> BuildPageHeader();
	TSharedRef<SWidget> BuildOverviewTab();
	TSharedRef<SWidget> BuildVersionsTab();
	TSharedRef<SWidget> BuildLogsTab();
	TSharedRef<SWidget> BuildActivityTab();
	TSharedRef<SWidget> BuildSettingsTab();
	TSharedRef<SWidget> BuildErrorLine(const FString& Key);

	FText GetHeaderText() const;
	TSharedRef<SWidget> MakeStatusBadge() const;
	TSharedRef<SWidget> MakeTypeRow(const CrowdyExecDeploy::FTypeState& Type);
	TSharedRef<SWidget> MakeRemovedRow(const FString& TypeName);
	/** One entry of the type list on the left: the name and its badge, selecting the type on click. */
	TSharedRef<SWidget> MakeTypeRailRow(const FString& TypeName, const CrowdyServerComputePage::FChangeBadge& Badge);
	/** Shows the selected type's details on the right, or what to do when there is none. */
	void FillTypeDetail();
	/** How many types the next deploy adds, changes or removes. */
	int32 CountPendingTypes() const;
	/** How many types Generate would write: out of date, or with no server code yet. */
	int32 CountToGenerate() const;
	/** Running or Switched off, with its switch, for a type the live version runs; nothing otherwise. */
	TSharedRef<SWidget> MakeRunningSwitch(const FString& TypeName);
	TSharedRef<SWidget> MakeTypeMenu(const FString& TypeName, const FString& AssetPath);
	FText MakeTypeMeta(const UCrowdyServerObjectDefinition* Definition, const FString& TypeName) const;
	/** "3 of 16 Server Objects · 192 of 1,024 MB", or empty when the platform does not say. */
	static FText MakeUsageText(const CrowdyExecDeveloper::FAppStatus& Status);
	/** One line per instance of the type the platform will not place, with its reason; empty when none. */
	FText MakeHeldBackLine(const FString& TypeName) const;
	bool CacheVersionTypes(const CrowdyExecDeveloper::FVersion& Version);
	FText DescribeVersionChanges(const CrowdyExecDeveloper::FVersion& Version);

	/** Fills the rows that show the service's data (header badge, types, versions) and notes which data they show. */
	void FillServiceRows();
	void FillHeaderBadge();
	void FillTypes();
	void FillVersions();
	void FillLogs();
	void FillActivity();
	void FillBuildLog();

	void RefreshLogs(bool bOlder);
	void RefreshStats();
	void SetError(const FString& Key, const FText& Error);
	/** The answer to one of the page's changes to the live app, begun on Compute: it ends that action however it comes back, then applies the new status or shows Failed under ErrorKey. */
	TFunction<void(const CrowdyExecDeveloper::TResult<CrowdyExecDeveloper::FAppStatus>&)> MakeActionAnswer(FCrowdyServerComputeService& Compute, const FString& ErrorKey, const FText& Failed);

	void OnTabSelected(const FString& Tab);
	FReply OnRefreshClicked();
	FReply OnSwitchClicked(FString TypeName, bool bEnable);
	FReply OnSwitchAppOnClicked();
	FReply OnGenerateClicked();
	FReply OnDeployClicked();
	FReply OnRebuildAllClicked();
	/** The arrow beside Deploy, opening Rebuild All and Deploy. */
	TSharedRef<SWidget> MakeDeployMenu();
	FReply OnDeployStartersClicked();
	/** The Game Kit starters, each adding itself and the starters it needs to the project. */
	TSharedRef<SWidget> MakeAddStarterMenu();
	FReply OnAddStarterClicked(FString TypeName);
	FReply OnMakeActiveClicked(int32 Version);
	FReply OnOpenDefinitionClicked(FString AssetPath);
	/** Opens Call as Developer on the player starter's grant or deposit. */
	FReply OnSoftCurrencyClicked(FString AssetPath, FString Method);
	FReply OnRemoveTypeClicked(FString TypeName, FString AssetPath);
	// Remove from project for a Game Kit starter: the starters that go with it, their folders and definitions, as -op=removestarter.
	void RemoveStarter(const FString& TypeName);
	FReply OnShowCallClicked(FString Flow);
	FReply OnClearCallClicked();
	FReply OnLoadOlderClicked();
	FReply OnToggleBuildOutput();
	FReply OnRestartClicked(FString TypeName);
	FReply OnMoveRunningClicked();
	FReply OnDismissMoveClicked();

	/** After a deploy, how many Server Objects of the changed types still run the previous version; empty when none. */
	FText GetMoveLine() const;
	FText GetDeployLine() const;
	FSlateColor GetDeployLineColor() const;

	/** No deploy or other change to the live app is waiting on the platform. */
	bool IsIdle() const;
	bool CanGenerate() const;
	bool CanDeploy() const;
	bool CanRebuildAll() const;
	bool CanSwitch() const;

	/** The service this page listens to, held weakly so a page outliving it never touches it. */
	TWeakPtr<FCrowdyServerComputeService> Service;
	FDelegateHandle ServiceChangedHandle;
	/** The client the page's logs and activity were read with; another one means they are dropped and read again. */
	TWeakPtr<CrowdyExecDeveloper::FClient> SeenClient;
	bool bHadClient = false;

	TArray<CrowdyExecDeveloper::FLogLine> LogLines;
	TArray<CrowdyExecDeveloper::FEndpointStat> Stats;
	CrowdyExecDeveloper::FLogQuery LogQuery;
	/** Each version's types, parsed once from its manifest, by version number. */
	TMap<int32, TArray<CrowdyExecRevisions::FLiveType>> VersionTypes;
	/** One error line per list read or action, so one answer never erases another's error. */
	TMap<FString, FText> Errors;

	FString ActiveTab;
	FString LogLevel;
	FString LogFlow;
	FString StatsWindow;
	/** The window the Stats held were read for, which may lag StatsWindow while a new read is on its way. */
	FString StatsShownWindow;
	/** The build output the page shows, as last taken from the service. */
	FString ShownBuildLog;

	TSharedPtr<SBox> HeaderBadgeBox;
	TSharedPtr<SWidgetSwitcher> TabSwitcher;
	TSharedPtr<SVerticalBox> TypesList;
	TSharedPtr<SBox> TypeDetailBox;
	/** The type the right side shows, kept across refills while it still exists. */
	FString SelectedType;
	/** "", "changed" or "problems": which types the list shows. */
	FString TypeFilter;
	FString TypeSearch;
	TSharedPtr<SVerticalBox> VersionsList;
	TSharedPtr<SVerticalBox> LogsList;
	TSharedPtr<SVerticalBox> ActivityList;
	TSharedPtr<SEditableTextBox> LogTypeBox;
	TSharedPtr<SMultiLineEditableTextBox> BuildLogBox;

	double TypesCheckedSeconds = 0.0;
	int32 ActionRequest = 0;
	int32 LogsRequest = 0;
	int32 StatsRequest = 0;
	/** The service's data generation the rows were last filled from. */
	uint32 FilledGeneration = 0;
	bool bShown = false;
	bool bContentBuilt = false;
	bool bNothingToDeploy = false;
	bool bBuildOutputOpen = false;
	bool bLogsLoaded = false;
	bool bStatsLoaded = false;
	bool bMoreLogs = false;
};
