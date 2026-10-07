#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "CrowdyExecDeploy.h"
#include "Styling/SlateColor.h"
#include "Toolkits/AssetEditorToolkit.h"

class FCrowdyServerComputeService;
class FToolBarBuilder;
class IDetailsView;
class SCrowdyServerCodeTab;
class SCrowdyServerObjectMembers;
class UCrowdyServerObjectDefinition;
struct FCrowdyServerObjectSelection;
struct FPropertyChangedEvent;

/**
 * The Server Object definition asset's editor, laid out like the Blueprint editor: its server code, a panel listing its
 * variables and functions, and the selected one's details, with Generate, Deploy and how the type stands against the live version.
 */
class FCrowdyServerObjectEditor : public FAssetEditorToolkit
{
public:
	virtual ~FCrowdyServerObjectEditor() override;

	void InitEditor(EToolkitMode::Type Mode, const TSharedPtr<IToolkitHost>& InitToolkitHost, UCrowdyServerObjectDefinition* InDefinition);

	virtual void RegisterTabSpawners(const TSharedRef<FTabManager>& InTabManager) override;
	virtual void UnregisterTabSpawners(const TSharedRef<FTabManager>& InTabManager) override;
	virtual FName GetToolkitFName() const override;
	virtual FText GetBaseToolkitName() const override;
	virtual FString GetWorldCentricTabPrefix() const override;
	virtual FLinearColor GetWorldCentricTabColorScale() const override;
	virtual void OnClose() override;

private:
	enum class EStatusTone : uint8
	{
		Neutral,
		Good,
		Info,
		Warning,
		Problem
	};

	TSharedRef<SDockTab> SpawnCodeTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnMembersTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnDetailsTab(const FSpawnTabArgs& Args);
	void RequestDetailsRefresh();
	bool HandleDetailsRefresh(float DeltaTime);
	void OpenFunction(const FString& Method);
	void ShowTypeSettings();
	bool IsShowingTypeSettings() const;
	void FillToolbar(FToolBarBuilder& ToolbarBuilder);
	TSharedRef<SWidget> MakeStatusReadout();
	TSharedRef<SWidget> MakeDeployMenu();

	void ConnectService();
	void RefreshStatus();
	void RefreshStatusLine();
	void SetStatus(EStatusTone Tone, const FText& Text, const FText& ToolTip);
	void HandleObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& Event);
	bool HandleDeferredRefresh(float DeltaTime);
	void HandleServiceChanged();

	void Generate();
	bool CanGenerate() const;
	void Deploy();
	bool CanDeploy() const;
	void OpenServerCompute();
	void ShowBuildOutput();
	bool HasBuildOutput() const;

	FText GetGenerateToolTip() const;
	FText GetDeployToolTip() const;
	FText GetStatusText() const;
	FText GetStatusToolTip() const;
	FSlateColor GetStatusColor() const;
	bool IsServiceBusy() const;

	TWeakObjectPtr<UCrowdyServerObjectDefinition> Definition;
	TWeakPtr<FCrowdyServerComputeService> Service;
	TSharedPtr<SCrowdyServerCodeTab> CodeTab;
	TSharedPtr<FCrowdyServerObjectSelection> Selection;
	TSharedPtr<SCrowdyServerObjectMembers> Members;
	TSharedPtr<IDetailsView> DetailsView;
	FTSTicker::FDelegateHandle DetailsRefreshTicker;
	TWeakPtr<SWindow> BuildOutputWindow;

	CrowdyExecDeploy::FTypeState State;
	FString TypeName;
	FText StatusText;
	FText StatusToolTip;
	EStatusTone StatusTone = EStatusTone::Neutral;

	FDelegateHandle PropertyChangedHandle;
	FDelegateHandle ServiceChangedHandle;
	FTSTicker::FDelegateHandle RefreshTicker;
	/** The service's data generation the status last showed. */
	uint32 SeenGeneration = 0;
	bool bTypeNameValid = false;
	/** Another of the project's definitions has this Type Name, so both would write one crate folder. */
	bool bSharedTypeName = false;
};
