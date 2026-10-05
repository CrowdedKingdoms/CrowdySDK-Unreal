#include "CrowdyServerObjectEditor.h"

#include "CrowdyExecCodegen.h"
#include "CrowdyServerCodeFiles.h"
#include "CrowdyServerComputeService.h"
#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectDefinitionCustomization.h"
#include "CrowdyServerObjectSelection.h"
#include "CrowdyStudioModule.h"
#include "SCrowdyServerObjectMembers.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/MultiBox/MultiBoxExtender.h"
#include "IDetailsView.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "SCrowdyServerCodeTab.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleColors.h"
#include "Textures/SlateIcon.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CrowdyServerObjectEditor"

namespace CrowdyServerObjectEditorDetail
{
	const FName AppId(TEXT("CrowdyServerObjectEditorApp"));
	const FName CodeTabId(TEXT("CrowdyServerObjectEditor_Code"));
	const FName MembersTabId(TEXT("CrowdyServerObjectEditor_Members"));
	const FName DetailsTabId(TEXT("CrowdyServerObjectEditor_Details"));
	/** The Server Compute page's id in Crowdy Studio, as the module registers it. */
	const FName ServerComputePageId(TEXT("CrowdyServerCompute"));
	constexpr float StatusMaxWidth = 460.0f;
}

FCrowdyServerObjectEditor::~FCrowdyServerObjectEditor()
{
	FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PropertyChangedHandle);
	FTSTicker::RemoveTicker(RefreshTicker);
	FTSTicker::RemoveTicker(DetailsRefreshTicker);
	if (const TSharedPtr<FCrowdyServerComputeService> Pinned = Service.Pin())
	{
		Pinned->OnChanged().Remove(ServiceChangedHandle);
	}
}

void FCrowdyServerObjectEditor::InitEditor(EToolkitMode::Type Mode, const TSharedPtr<IToolkitHost>& InitToolkitHost, UCrowdyServerObjectDefinition* InDefinition)
{
	using namespace CrowdyServerObjectEditorDetail;
	Definition = InDefinition;
	if (!InDefinition)
	{
		return;
	}
	Service = FCrowdyServerComputeService::Get().AsShared();
	CodeTab = SNew(SCrowdyServerCodeTab)
		.Definition(InDefinition)
		.OnCodeWritten(FSimpleDelegate::CreateSP(this, &FCrowdyServerObjectEditor::RefreshStatus));

	Selection = MakeShared<FCrowdyServerObjectSelection>();
	Selection->OnChanged.AddSP(this, &FCrowdyServerObjectEditor::RequestDetailsRefresh);
	Members = SNew(SCrowdyServerObjectMembers)
		.Definition(InDefinition)
		.Selection(Selection)
		.OnOpenFunction(FOnCrowdyOpenServerFunction::CreateSP(this, &FCrowdyServerObjectEditor::OpenFunction));

	FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
	FDetailsViewArgs DetailsArgs;
	DetailsArgs.bHideSelectionTip = true;
	DetailsArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	DetailsView = PropertyEditor.CreateDetailView(DetailsArgs);
	const TSharedPtr<FCrowdyServerObjectSelection> Selected = Selection;
	DetailsView->RegisterInstancedCustomPropertyLayout(UCrowdyServerObjectDefinition::StaticClass(), FOnGetDetailCustomizationInstance::CreateLambda([Selected]()
	{
		return FCrowdyServerObjectDefinitionCustomization::MakeForEditor(Selected);
	}));
	DetailsView->SetObject(InDefinition);

	PropertyChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddSP(this, &FCrowdyServerObjectEditor::HandleObjectPropertyChanged);
	ConnectService();
	RefreshStatus();

	const TSharedRef<FExtender> ToolbarExtender = MakeShared<FExtender>();
	ToolbarExtender->AddToolBarExtension(TEXT("Asset"), EExtensionHook::After, nullptr, FToolBarExtensionDelegate::CreateSP(this, &FCrowdyServerObjectEditor::FillToolbar));
	AddToolbarExtender(ToolbarExtender);

	const TSharedRef<FTabManager::FLayout> Layout = FTabManager::NewLayout(TEXT("Standalone_CrowdyServerObjectEditor_Layout_v2"))
		->AddArea(
			FTabManager::NewPrimaryArea()->SetOrientation(Orient_Horizontal)
			->Split(FTabManager::NewStack()->SetSizeCoefficient(0.6f)->AddTab(CodeTabId, ETabState::OpenedTab))
			->Split(FTabManager::NewSplitter()->SetOrientation(Orient_Vertical)->SetSizeCoefficient(0.4f)
				->Split(FTabManager::NewStack()->SetSizeCoefficient(0.35f)->AddTab(MembersTabId, ETabState::OpenedTab))
				->Split(FTabManager::NewStack()->SetSizeCoefficient(0.65f)->AddTab(DetailsTabId, ETabState::OpenedTab))));

	InitAssetEditor(Mode, InitToolkitHost, AppId, Layout, true, true, InDefinition);
}

void FCrowdyServerObjectEditor::RegisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	using namespace CrowdyServerObjectEditorDetail;
	WorkspaceMenuCategory = InTabManager->AddLocalWorkspaceMenuCategory(LOCTEXT("WorkspaceMenu", "Server Object"));
	FAssetEditorToolkit::RegisterTabSpawners(InTabManager);
	const TSharedRef<FWorkspaceItem> Group = WorkspaceMenuCategory.ToSharedRef();
	InTabManager->RegisterTabSpawner(CodeTabId, FOnSpawnTab::CreateSP(this, &FCrowdyServerObjectEditor::SpawnCodeTab))
		.SetDisplayName(LOCTEXT("CodeTab", "Server Code"))
		.SetGroup(Group)
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Edit"));
	InTabManager->RegisterTabSpawner(MembersTabId, FOnSpawnTab::CreateSP(this, &FCrowdyServerObjectEditor::SpawnMembersTab))
		.SetDisplayName(LOCTEXT("MembersTab", "Server Object"))
		.SetGroup(Group)
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Server"));
	InTabManager->RegisterTabSpawner(DetailsTabId, FOnSpawnTab::CreateSP(this, &FCrowdyServerObjectEditor::SpawnDetailsTab))
		.SetDisplayName(LOCTEXT("DetailsTab", "Details"))
		.SetGroup(Group)
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details"));
}

void FCrowdyServerObjectEditor::UnregisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	using namespace CrowdyServerObjectEditorDetail;
	FAssetEditorToolkit::UnregisterTabSpawners(InTabManager);
	InTabManager->UnregisterTabSpawner(CodeTabId);
	InTabManager->UnregisterTabSpawner(MembersTabId);
	InTabManager->UnregisterTabSpawner(DetailsTabId);
}

FName FCrowdyServerObjectEditor::GetToolkitFName() const
{
	return FName(TEXT("CrowdyServerObjectEditor"));
}

FText FCrowdyServerObjectEditor::GetBaseToolkitName() const
{
	return LOCTEXT("ToolkitName", "Server Object Editor");
}

FString FCrowdyServerObjectEditor::GetWorldCentricTabPrefix() const
{
	return LOCTEXT("TabPrefix", "Server Object ").ToString();
}

FLinearColor FCrowdyServerObjectEditor::GetWorldCentricTabColorScale() const
{
	return FLinearColor(0.3f, 0.5f, 0.8f, 0.5f);
}

TSharedRef<SDockTab> FCrowdyServerObjectEditor::SpawnCodeTab(const FSpawnTabArgs& Args)
{
	// The code tab is made once with the editor, so closing and reopening this tab keeps its unsaved edits.
	return SNew(SDockTab)
		.Label(LOCTEXT("CodeTabLabel", "Server Code"))
		[
			CodeTab.ToSharedRef()
		];
}

TSharedRef<SDockTab> FCrowdyServerObjectEditor::SpawnMembersTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.Label(LOCTEXT("MembersTabLabel", "Server Object"))
		[
			Members.ToSharedRef()
		];
}

TSharedRef<SDockTab> FCrowdyServerObjectEditor::SpawnDetailsTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.Label(LOCTEXT("DetailsTabLabel", "Details"))
		[
			DetailsView.ToSharedRef()
		];
}

void FCrowdyServerObjectEditor::RequestDetailsRefresh()
{
	if (DetailsRefreshTicker.IsValid())
	{
		return;
	}
	// On the next tick, so the Details tab is never rebuilt from inside one of its own widgets' handlers.
	DetailsRefreshTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateSP(this, &FCrowdyServerObjectEditor::HandleDetailsRefresh));
}

bool FCrowdyServerObjectEditor::HandleDetailsRefresh(float DeltaTime)
{
	DetailsRefreshTicker.Reset();
	if (DetailsView.IsValid())
	{
		DetailsView->ForceRefresh();
	}
	return false;
}

void FCrowdyServerObjectEditor::OpenFunction(const FString& Method)
{
	if (!CodeTab.IsValid())
	{
		return;
	}
	TabManager->TryInvokeTab(CrowdyServerObjectEditorDetail::CodeTabId);
	CodeTab->RevealFunction(Method);
}

void FCrowdyServerObjectEditor::ShowTypeSettings()
{
	if (Members.IsValid())
	{
		Members->ShowTypeSettings();
	}
	TabManager->TryInvokeTab(CrowdyServerObjectEditorDetail::DetailsTabId);
}

bool FCrowdyServerObjectEditor::IsShowingTypeSettings() const
{
	return Selection.IsValid() && Selection->Kind == FCrowdyServerObjectSelection::EKind::None;
}

void FCrowdyServerObjectEditor::FillToolbar(FToolBarBuilder& ToolbarBuilder)
{
	ToolbarBuilder.BeginSection(TEXT("CrowdyServerObject"));
	ToolbarBuilder.AddToolBarButton(
		FUIAction(FExecuteAction::CreateSP(this, &FCrowdyServerObjectEditor::ShowTypeSettings), FCanExecuteAction(),
			FIsActionChecked::CreateSP(this, &FCrowdyServerObjectEditor::IsShowingTypeSettings)),
		NAME_None,
		LOCTEXT("TypeSettings", "Type Settings"),
		LOCTEXT("TypeSettingsToolTip", "Edit the type's own settings: access, members, timers, saving and Server Names"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Toolbar.Settings"),
		EUserInterfaceActionType::ToggleButton);
	ToolbarBuilder.AddToolBarButton(
		FUIAction(FExecuteAction::CreateSP(this, &FCrowdyServerObjectEditor::Generate), FCanExecuteAction::CreateSP(this, &FCrowdyServerObjectEditor::CanGenerate)),
		NAME_None,
		LOCTEXT("Generate", "Generate"),
		TAttribute<FText>::CreateSP(this, &FCrowdyServerObjectEditor::GetGenerateToolTip),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Recompile"));
	ToolbarBuilder.AddToolBarButton(
		FUIAction(FExecuteAction::CreateSP(this, &FCrowdyServerObjectEditor::Deploy), FCanExecuteAction::CreateSP(this, &FCrowdyServerObjectEditor::CanDeploy)),
		NAME_None,
		LOCTEXT("Deploy", "Deploy"),
		TAttribute<FText>::CreateSP(this, &FCrowdyServerObjectEditor::GetDeployToolTip),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "PlayWorld.RepeatLastLaunch"));
	ToolbarBuilder.AddComboButton(
		FUIAction(),
		FOnGetContent::CreateSP(this, &FCrowdyServerObjectEditor::MakeDeployMenu),
		FText::GetEmpty(),
		LOCTEXT("DeployMenuToolTip", "Server Compute and the last build's output"),
		FSlateIcon(),
		true);
	ToolbarBuilder.EndSection();

	ToolbarBuilder.BeginSection(TEXT("CrowdyServerObjectStatus"));
	FMenuEntryStyleParams StatusStyle;
	StatusStyle.HorizontalAlignment = HAlign_Right;
	StatusStyle.SizeRule = FSizeParam::SizeRule_Stretch;
	ToolbarBuilder.AddWidget(MakeStatusReadout(), StatusStyle);
	ToolbarBuilder.EndSection();
}

TSharedRef<SWidget> FCrowdyServerObjectEditor::MakeStatusReadout()
{
	return SNew(SBox)
		.Padding(FMargin(8.0f, 0.0f))
		.VAlign(VAlign_Center)
		.ToolTipText(TAttribute<FText>::CreateSP(this, &FCrowdyServerObjectEditor::GetStatusToolTip))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SNew(SBox)
				.WidthOverride(8.0f)
				.HeightOverride(8.0f)
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush("Icons.FilledCircle"))
					.ColorAndOpacity(TAttribute<FSlateColor>::CreateSP(this, &FCrowdyServerObjectEditor::GetStatusColor))
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SBox)
				.MaxDesiredWidth(CrowdyServerObjectEditorDetail::StatusMaxWidth)
				[
					SNew(STextBlock)
					.Text(TAttribute<FText>::CreateSP(this, &FCrowdyServerObjectEditor::GetStatusText))
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				]
			]
		];
}

TSharedRef<SWidget> FCrowdyServerObjectEditor::MakeDeployMenu()
{
	FMenuBuilder Menu(true, nullptr);
	Menu.AddMenuEntry(
		LOCTEXT("OpenServerCompute", "Open Server Compute"),
		LOCTEXT("OpenServerComputeToolTip", "Opens Crowdy Studio's Server Compute page: every type, the app's versions, logs and calls"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Server"),
		FUIAction(FExecuteAction::CreateSP(this, &FCrowdyServerObjectEditor::OpenServerCompute)));
	Menu.AddMenuEntry(
		LOCTEXT("ShowBuildOutput", "Show build output"),
		LOCTEXT("ShowBuildOutputToolTip", "Shows what the last build of this session printed"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Documentation"),
		FUIAction(FExecuteAction::CreateSP(this, &FCrowdyServerObjectEditor::ShowBuildOutput), FCanExecuteAction::CreateSP(this, &FCrowdyServerObjectEditor::HasBuildOutput)));
	return Menu.MakeWidget();
}

void FCrowdyServerObjectEditor::ConnectService()
{
	const TSharedPtr<FCrowdyServerComputeService> Pinned = Service.Pin();
	if (!Pinned.IsValid())
	{
		return;
	}
	ServiceChangedHandle = Pinned->OnChanged().AddSP(this, &FCrowdyServerObjectEditor::HandleServiceChanged);
	SeenGeneration = Pinned->GetDataGeneration();
	if (Pinned->IsBusy())
	{
		return;
	}
	if (!Pinned->AreVersionsLoaded())
	{
		if (!Pinned->RenewClientIfStale())
		{
			Pinned->Refresh();
		}
		return;
	}
	Pinned->RequestRefreshTypes();
}

void FCrowdyServerObjectEditor::RefreshStatus()
{
	const UCrowdyServerObjectDefinition* Current = Definition.Get();
	if (!Current)
	{
		return;
	}
	const TSharedPtr<FCrowdyServerComputeService> Pinned = Service.Pin();
	TypeName = Current->TypeName;
	bTypeNameValid = CrowdyServerCodeFiles::IsValidTypeName(TypeName);
	bSharedTypeName = bTypeNameValid && Pinned.IsValid() && Pinned->IsSharedTypeName(TypeName);
	if (bTypeNameValid)
	{
		State = CrowdyExecDeploy::InspectType(*Current, CrowdyExecCodegen::GetCrateDirectory(*Current));
		RefreshStatusLine();
		return;
	}
	State = CrowdyExecDeploy::FTypeState();
	State.TypeName = TypeName;
	State.AssetPath = Current->GetPathName();
	State.State = CrowdyExecDeploy::ECrateState::Invalid;
	State.Problem = CodeTab.IsValid() ? CodeTab->GetWriteBlockReason().ToString() : FString();
	RefreshStatusLine();
}

void FCrowdyServerObjectEditor::RefreshStatusLine()
{
	using CrowdyExecDeploy::ECrateState;
	using CrowdyExecRevisions::EChange;
	using CrowdyServerCodeFiles::VersionText;
	const FText ProblemFormat = LOCTEXT("StatusProblem", "Problem: {0}");
	if (bSharedTypeName)
	{
		SetStatus(EStatusTone::Problem, FText::Format(ProblemFormat, CrowdyServerCodeFiles::SharedTypeNameProblem()), CrowdyServerCodeFiles::SharedTypeNameProblem());
		return;
	}
	const FText Problem = FText::FromString(State.Problem);
	if (State.State == ECrateState::Invalid)
	{
		SetStatus(EStatusTone::Problem, FText::Format(ProblemFormat, Problem), Problem);
		return;
	}
	if (State.State != ECrateState::UpToDate)
	{
		SetStatus(EStatusTone::Warning, State.State == ECrateState::OutOfDate ? LOCTEXT("OutOfDate", "Out of date") : LOCTEXT("NotGenerated", "Needs generating"), Problem);
		return;
	}
	const TSharedPtr<FCrowdyServerComputeService> Pinned = Service.Pin();
	const CrowdyExecRevisions::FTypeChange* Change = Pinned.IsValid() ? Pinned->FindChange(TypeName) : nullptr;
	switch (Change ? Change->Change : EChange::NotCompared)
	{
	case EChange::Unchanged:
		SetStatus(EStatusTone::Good, FText::Format(LOCTEXT("NoChanges", "No changes · live in version {0}"), VersionText(Change->LiveVersion)),
			FText::Format(LOCTEXT("NoChangesToolTip", "Version {0}, the live version, runs this code with these settings"), VersionText(Change->LiveVersion)));
		return;
	case EChange::Changed:
		SetStatus(EStatusTone::Warning,
			FText::Format(LOCTEXT("ChangedSince", "Changed since version {0} ({1})"), VersionText(Change->LiveVersion), FText::FromString(FString::Join(Change->What, TEXT(", ")))),
			FText::Format(LOCTEXT("ChangedToolTip", "Changed: {0}. Deploy to make it live."), FText::FromString(FString::Join(Change->What, TEXT(", ")))));
		return;
	case EChange::New:
		SetStatus(EStatusTone::Info, LOCTEXT("NotDeployed", "Not deployed yet"), LOCTEXT("NotDeployedToolTip", "The live version does not run this type. Deploy adds it."));
		return;
	case EChange::Unknown:
		SetStatus(EStatusTone::Warning, FText::Format(LOCTEXT("LiveUnknown", "Live code unknown · app version {0}"), VersionText(Change->LiveVersion)),
			LOCTEXT("LiveUnknownToolTip", "The live version runs code this project has no record of, for example deployed from another copy of the project"));
		return;
	default:
		SetStatus(EStatusTone::Neutral, LOCTEXT("Ready", "Ready"),
			LOCTEXT("ReadyToolTip", "The server code on disk matches this definition. It is compared with the live version once Server Compute has read it: sign in to Crowdy Studio."));
		return;
	}
}

void FCrowdyServerObjectEditor::SetStatus(EStatusTone Tone, const FText& Text, const FText& ToolTip)
{
	StatusTone = Tone;
	StatusText = Text;
	StatusToolTip = ToolTip;
}

void FCrowdyServerObjectEditor::HandleObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& Event)
{
	if (Object != Definition.Get() || (Event.ChangeType & EPropertyChangeType::Interactive) != 0 || RefreshTicker.IsValid())
	{
		return;
	}
	// The definition re-bakes after this notice, so it is read again on the next tick.
	RefreshTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateSP(this, &FCrowdyServerObjectEditor::HandleDeferredRefresh));
}

bool FCrowdyServerObjectEditor::HandleDeferredRefresh(float DeltaTime)
{
	RefreshTicker.Reset();
	if (CodeTab.IsValid())
	{
		CodeTab->HandleDefinitionChanged();
	}
	RefreshStatus();
	// An added or removed function or variable changes what the selection's details are made of.
	RequestDetailsRefresh();
	return false;
}

void FCrowdyServerObjectEditor::HandleServiceChanged()
{
	const TSharedPtr<FCrowdyServerComputeService> Pinned = Service.Pin();
	if (!Pinned.IsValid() || Pinned->GetDataGeneration() == SeenGeneration)
	{
		return;
	}
	SeenGeneration = Pinned->GetDataGeneration();
	// Code generated or edited outside this editor changes the crate's state too, not only the live version's.
	RefreshStatus();
}

void FCrowdyServerObjectEditor::OnClose()
{
	// Kept now rather than when the widget is destroyed, which can come after an asset reload has opened the editor again.
	if (CodeTab.IsValid())
	{
		CodeTab->KeepUnsavedEdits();
	}
	FAssetEditorToolkit::OnClose();
}

void FCrowdyServerObjectEditor::Generate()
{
	if (CodeTab.IsValid())
	{
		CodeTab->Generate();
	}
}

bool FCrowdyServerObjectEditor::CanGenerate() const
{
	return CodeTab.IsValid() && CodeTab->CanGenerate();
}

void FCrowdyServerObjectEditor::Deploy()
{
	const TSharedPtr<FCrowdyServerComputeService> Pinned = Service.Pin();
	if (!Pinned.IsValid() || !CanDeploy())
	{
		return;
	}
	if (CodeTab.IsValid())
	{
		CodeTab->OfferSaveBeforeDeploy();
	}
	Pinned->Deploy();
}

bool FCrowdyServerObjectEditor::CanDeploy() const
{
	const TSharedPtr<FCrowdyServerComputeService> Pinned = Service.Pin();
	return bTypeNameValid && !bSharedTypeName && Pinned.IsValid() && !Pinned->IsBusy() && Pinned->CanDeploy();
}

void FCrowdyServerObjectEditor::OpenServerCompute()
{
	CrowdyStudioExtraPages::OpenPage(CrowdyServerObjectEditorDetail::ServerComputePageId);
}

void FCrowdyServerObjectEditor::ShowBuildOutput()
{
	const TSharedPtr<FCrowdyServerComputeService> Pinned = Service.Pin();
	if (!Pinned.IsValid() || Pinned->GetBuildLog().IsEmpty())
	{
		return;
	}
	if (const TSharedPtr<SWindow> Open = BuildOutputWindow.Pin())
	{
		Open->RequestDestroyWindow();
	}
	const TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("BuildOutputTitle", "Build Output"))
		.ClientSize(FVector2D(900.0f, 600.0f))
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
			.Padding(4.0f)
			[
				SNew(SMultiLineEditableTextBox)
				.Text(FText::FromString(Pinned->GetBuildLog()))
				.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
				.AutoWrapText(false)
				.IsReadOnly(true)
			]
		];
	BuildOutputWindow = Window;
	const TSharedPtr<SWindow> Parent = FSlateApplication::Get().FindWidgetWindow(GetToolkitHost()->GetParentWidget());
	if (Parent.IsValid())
	{
		FSlateApplication::Get().AddWindowAsNativeChild(Window, Parent.ToSharedRef());
		return;
	}
	FSlateApplication::Get().AddWindow(Window);
}

bool FCrowdyServerObjectEditor::HasBuildOutput() const
{
	const TSharedPtr<FCrowdyServerComputeService> Pinned = Service.Pin();
	return Pinned.IsValid() && !Pinned->GetBuildLog().IsEmpty();
}

FText FCrowdyServerObjectEditor::GetGenerateToolTip() const
{
	return CodeTab.IsValid() ? CodeTab->GetGenerateToolTip() : FText::GetEmpty();
}

FText FCrowdyServerObjectEditor::GetDeployToolTip() const
{
	if (!bTypeNameValid)
	{
		return LOCTEXT("DeployNeedsTypeName", "Set a valid Type Name first");
	}
	if (bSharedTypeName)
	{
		return CrowdyServerCodeFiles::SharedTypeNameProblem();
	}
	const TSharedPtr<FCrowdyServerComputeService> Pinned = Service.Pin();
	return FText::Format(LOCTEXT("DeployToolTip", "Builds and deploys every Server Object type in this project together, since a deploy replaces the app's whole version. You confirm first.\n\n{0}"),
		Pinned.IsValid() ? Pinned->GetDeployLine() : FText::GetEmpty());
}

FText FCrowdyServerObjectEditor::GetStatusText() const
{
	// While a deploy runs its progress is the status; the line counts seconds, so it is read as it changes.
	const TSharedPtr<FCrowdyServerComputeService> Pinned = Service.Pin();
	return Pinned.IsValid() && Pinned->IsBusy() ? Pinned->GetDeployLine() : StatusText;
}

FText FCrowdyServerObjectEditor::GetStatusToolTip() const
{
	return IsServiceBusy() ? GetStatusText() : StatusToolTip;
}

FSlateColor FCrowdyServerObjectEditor::GetStatusColor() const
{
	const TSharedPtr<FCrowdyServerComputeService> Pinned = Service.Pin();
	if (Pinned.IsValid() && Pinned->IsBusy())
	{
		return FSlateColor(Pinned->GetDeployLineColor());
	}
	switch (StatusTone)
	{
	case EStatusTone::Good: return FStyleColors::Success;
	case EStatusTone::Info: return FStyleColors::AccentBlue;
	case EStatusTone::Warning: return FStyleColors::Warning;
	case EStatusTone::Problem: return FStyleColors::Error;
	default: return FStyleColors::Foreground;
	}
}

bool FCrowdyServerObjectEditor::IsServiceBusy() const
{
	const TSharedPtr<FCrowdyServerComputeService> Pinned = Service.Pin();
	return Pinned.IsValid() && Pinned->IsBusy();
}

#undef LOCTEXT_NAMESPACE
