#pragma once

#include "CoreMinimal.h"
#include "CrowdyExecDeploy.h"
#include "CrowdyExecDeveloperApi.h"
#include "CrowdyExecRevisions.h"
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

	/** Who can read an instance, as a type row says it for the definition's Readable By. */
	FText ReadableByPhrase(ECrowdyServerObjectVisibility Visibility);
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
	/** Running or Switched off, with its switch, for a type the live version runs; nothing otherwise. */
	TSharedRef<SWidget> MakeRunningSwitch(const FString& TypeName);
	TSharedRef<SWidget> MakeTypeMenu(const FString& TypeName, const FString& AssetPath);
	FText MakeTypeMeta(const UCrowdyServerObjectDefinition* Definition, const FString& TypeName) const;
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
	FReply OnDeployClicked();
	FReply OnDeployStartersClicked();
	FReply OnMakeActiveClicked(int32 Version);
	FReply OnOpenDefinitionClicked(FString AssetPath);
	FReply OnRemoveTypeClicked(FString TypeName, FString AssetPath);
	FReply OnShowCallClicked(FString Flow);
	FReply OnClearCallClicked();
	FReply OnLoadOlderClicked();
	FReply OnToggleBuildOutput();

	FText GetDeployLine() const;
	FSlateColor GetDeployLineColor() const;

	/** No deploy or other change to the live app is waiting on the platform. */
	bool IsIdle() const;
	bool CanDeploy() const;
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
