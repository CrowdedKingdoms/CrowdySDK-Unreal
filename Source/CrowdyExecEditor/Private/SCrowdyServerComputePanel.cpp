#include "SCrowdyServerComputePanel.h"

#include "CrowdyExecCodegen.h"
#include "CrowdyServerCodeFiles.h"
#include "CrowdyServerComputeService.h"
#include "CrowdyServerComputeSettings.h"
#include "CrowdyServerObjectDefinition.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "Style/CrowdyStudioStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyle.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UI/CrowdyStudioWidgets.h"
#include "UObject/SoftObjectPath.h"
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SCrowdyServerComputePanel"

namespace CrowdyComputeUI
{
	using EBadgeTone = CrowdyStudioWidgets::EBadgeTone;
	using EChange = CrowdyExecRevisions::EChange;

	const FString TabOverview(TEXT("overview"));
	const FString TabVersions(TEXT("versions"));
	const FString TabLogs(TEXT("logs"));
	const FString TabActivity(TEXT("activity"));
	const FString TabSettings(TEXT("settings"));
	const FString ErrorSwitch(TEXT("switch"));
	const FString ErrorAppSwitch(TEXT("app-switch"));
	const FString ErrorActivate(TEXT("activate"));
	const FString ErrorRemove(TEXT("remove"));
	const FString ErrorSettings(TEXT("settings"));
	constexpr int32 LogPageSize = 100;
	constexpr double TypesRecheckSeconds = 2.0;

	int32 TabIndex(const FString& Tab)
	{
		if (Tab == TabVersions)
		{
			return 1;
		}
		if (Tab == TabLogs)
		{
			return 2;
		}
		if (Tab == TabActivity)
		{
			return 3;
		}
		return Tab == TabSettings ? 4 : 0;
	}

	int32 MaxLevelFor(const FString& Level)
	{
		if (Level == TEXT("errors"))
		{
			return 0;
		}
		if (Level == TEXT("warnings"))
		{
			return 1;
		}
		return Level == TEXT("info") ? 2 : -1;
	}

	EBadgeTone ToneForLevel(int32 Level)
	{
		switch (Level)
		{
		case 0: return EBadgeTone::Danger;
		case 1: return EBadgeTone::Warning;
		case 2: return EBadgeTone::Info;
		default: return EBadgeTone::Neutral;
		}
	}

	FText LevelName(int32 Level)
	{
		switch (Level)
		{
		case 0: return LOCTEXT("LevelError", "Error");
		case 1: return LOCTEXT("LevelWarning", "Warning");
		case 2: return LOCTEXT("LevelInfo", "Info");
		default: return LOCTEXT("LevelDebug", "Debug");
		}
	}

	FText WindowPhrase(const FString& Window)
	{
		if (Window == TEXT("1440"))
		{
			return LOCTEXT("WindowDay", "in the last day");
		}
		return Window == TEXT("10080") ? LOCTEXT("WindowWeek", "in the last week") : LOCTEXT("WindowHour", "in the last hour");
	}

	FText Unknown()
	{
		return LOCTEXT("Unknown", "-");
	}

	FText VersionNumber(int32 Version)
	{
		return FText::AsNumber(Version, &FNumberFormattingOptions::DefaultNoGrouping());
	}

	FText Decimals(double Value, int32 MinimumDigits)
	{
		FNumberFormattingOptions Options;
		Options.RoundingMode = ERoundingMode::HalfFromZero;
		Options.MinimumFractionalDigits = MinimumDigits;
		Options.MaximumFractionalDigits = 1;
		return FText::AsNumber(Value, &Options);
	}

	/** 950, 1.2k, 3.4M; a negative count reads 0. */
	FText CompactCount(double Value)
	{
		if (FMath::IsNaN(Value))
		{
			return Unknown();
		}
		const double Count = FMath::Max(0.0, Value);
		if (FMath::RoundToDouble(Count) < 1000.0)
		{
			return FText::AsNumber(FMath::RoundToInt64(Count));
		}
		// Compared after rounding to a tenth, so 999950 reads 1.0M rather than 1000.0k.
		if (FMath::RoundToDouble(Count / 100.0) < 10000.0)
		{
			return FText::Format(LOCTEXT("Thousands", "{0}k"), Decimals(Count / 1000.0, 1));
		}
		return FText::Format(LOCTEXT("Millions", "{0}M"), Decimals(Count / 1000000.0, 1));
	}

	/** "no calls in the last hour", "1 call in the last day", "1.2k calls in the last week". */
	FText CallsPhrase(double Calls, const FText& Window)
	{
		const int64 Rounded = FMath::IsNaN(Calls) ? 0 : FMath::RoundToInt64(Calls);
		if (Rounded <= 0)
		{
			return FText::Format(LOCTEXT("NoCallsPhrase", "no calls {0}"), Window);
		}
		if (Rounded == 1)
		{
			return FText::Format(LOCTEXT("OneCallPhrase", "1 call {0}"), Window);
		}
		return FText::Format(LOCTEXT("CallsPhrase", "{0} calls {1}"), CompactCount(Calls), Window);
	}

	/** 0.4 ms, 12 ms, 1.4 s. */
	FText Duration(double Milliseconds)
	{
		if (FMath::IsNaN(Milliseconds))
		{
			return Unknown();
		}
		const double Ms = FMath::Max(0.0, Milliseconds);
		if (FMath::RoundToDouble(Ms * 10.0) < 100.0)
		{
			return FText::Format(LOCTEXT("MsSmall", "{0} ms"), Decimals(Ms, 0));
		}
		if (FMath::RoundToDouble(Ms) < 1000.0)
		{
			return FText::Format(LOCTEXT("Ms", "{0} ms"), FText::AsNumber(FMath::RoundToInt64(Ms)));
		}
		return FText::Format(LOCTEXT("Seconds", "{0} s"), Decimals(Ms / 1000.0, 1));
	}

	FText AbsoluteTime(const FDateTime& Time)
	{
		if (Time == FDateTime())
		{
			return Unknown();
		}
		return FText::FromString(Time.ToString(TEXT("%Y-%m-%d %H:%M:%S UTC")));
	}

	FText RelativeTime(const FDateTime& Time)
	{
		if (Time == FDateTime())
		{
			return Unknown();
		}
		const FTimespan Age = FDateTime::UtcNow() - Time;
		// A time slightly ahead of this machine's clock reads as now; further ahead, as itself.
		if (Age.GetTotalSeconds() <= -60.0)
		{
			return AbsoluteTime(Time);
		}
		if (Age.GetTotalMinutes() < 1.0)
		{
			return LOCTEXT("JustNow", "just now");
		}
		if (Age.GetTotalHours() < 1.0)
		{
			return FText::Format(LOCTEXT("MinutesAgo", "{0} min ago"), FText::AsNumber(FMath::FloorToInt(Age.GetTotalMinutes())));
		}
		if (Age.GetTotalDays() < 1.0)
		{
			return FText::Format(LOCTEXT("HoursAgo", "{0} h ago"), FText::AsNumber(FMath::FloorToInt(Age.GetTotalHours())));
		}
		if (Age.GetTotalDays() < 7.0)
		{
			return FText::Format(LOCTEXT("DaysAgo", "{0} d ago"), FText::AsNumber(FMath::FloorToInt(Age.GetTotalDays())));
		}
		return FText::FromString(Time.ToString(TEXT("%Y-%m-%d")));
	}

	FText VersionDetail(const FDateTime& CreatedAt, const FString& CreatedBy, const FText& Types)
	{
		const FText When = RelativeTime(CreatedAt);
		if (CreatedBy.IsEmpty())
		{
			return FText::Format(LOCTEXT("VersionDetailNoBy", "{0}, {1}"), When, Types);
		}
		return FText::Format(LOCTEXT("VersionDetail", "{0} by {1}, {2}"), When, FText::FromString(CreatedBy), Types);
	}

	FText LogMeta(const FDateTime& At, const FString& NodeType, const FString& Key)
	{
		TArray<FString> Parts = { RelativeTime(At).ToString(), NodeType, Key };
		Parts.RemoveAll([](const FString& Part) { return Part.IsEmpty(); });
		return FText::FromString(FString::Join(Parts, TEXT(", ")));
	}

	FText ErrorText(const FText& What, const CrowdyExecDeveloper::FError& Error)
	{
		return FText::Format(LOCTEXT("ErrorLine", "{0}: {1}"), What, FText::FromString(Error.Message));
	}

	bool Confirm(const FText& Message, const FText& Title)
	{
		return FMessageDialog::Open(EAppMsgType::YesNo, EAppReturnType::No, Message, Title) == EAppReturnType::Yes;
	}

	/** What removing a type from the project does on the server, for the remove confirm. */
	FText RemoveServerEffect(const FText& Type, const FText& Target, const CrowdyExecRevisions::FTypeChange* Change, bool bLastType)
	{
		if (Change && Change->Change == EChange::New)
		{
			return FText::Format(LOCTEXT("RemoveNotLive", "{0} is not live on the server ({1}), so nothing changes there."), Type, Target);
		}
		if (bLastType)
		{
			return FText::Format(LOCTEXT("RemoveLastType", "{0} keeps running on the server ({1}) until the project has a type to deploy again, since a deploy needs at least one type. To stop players' calls to it meanwhile, use Switch off on this page."), Type, Target);
		}
		return FText::Format(LOCTEXT("RemoveUntilDeploy", "{0} keeps running on the server ({1}) until your next deploy, which removes it."), Type, Target);
	}

	/** Deletes a removed type's crate folder once its definition is gone; why it did not, or empty. */
	FText DeleteCrateFolder(const FText& Type, const FString& TypeName, const FString& CrateDirectory, const FString& ServerDirectory, bool bShared, const FString& ShownFolder)
	{
		if (!IFileManager::Get().DirectoryExists(*CrateDirectory))
		{
			return FText::GetEmpty();
		}
		// Checked again, since the folder may have changed while the confirms were open.
		const FText Stays = CrowdyServerComputePage::WhyCrateFolderStays(TypeName, CrateDirectory, ServerDirectory, bShared);
		if (!Stays.IsEmpty())
		{
			return FText::Format(LOCTEXT("RemoveFolderKept", "Removed {0}'s definition. {1}"), Type, Stays);
		}
		if (!IFileManager::Get().DeleteDirectory(*CrateDirectory, false, true))
		{
			return FText::Format(LOCTEXT("RemoveFolderFailed", "Removed {0}'s definition, but could not delete its Server Code folder {1}. Delete it by hand."), Type, FText::FromString(ShownFolder));
		}
		return FText::GetEmpty();
	}

	TSharedRef<SWidget> ButtonLabel(const FText& Label, int32 FontSize = 9)
	{
		return SNew(STextBlock).Text(Label).Font(FCoreStyle::GetDefaultFontStyle("Bold", FontSize)).ColorAndOpacity(FSlateColor::UseForeground());
	}

	TSharedRef<SWidget> QuietLabel(const FText& Label)
	{
		return SNew(STextBlock).Text(Label).Font(FCoreStyle::GetDefaultFontStyle("Regular", 9)).ColorAndOpacity(FSlateColor::UseForeground());
	}

	TSharedRef<SWidget> Dot(EBadgeTone Tone)
	{
		return SNew(SBox).WidthOverride(8.f).HeightOverride(8.f)
			[
				SNew(SImage)
				.Image(FCrowdyStudioStyle::Get().GetBrush("Crowdy.Pill"))
				.ColorAndOpacity(FSlateColor(CrowdyStudioWidgets::ColorForTone(Tone)))
			];
	}

	TSharedRef<SWidget> TipBadge(const FText& Label, EBadgeTone Tone, const FText& ToolTip)
	{
		return SNew(SBox).ToolTipText(ToolTip)[ CrowdyStudioWidgets::Badge(Label, Tone) ];
	}

	/** A red line that shows only while Error has something to say. */
	TSharedRef<SWidget> ErrorLine(TFunction<FText()> Error)
	{
		return SNew(STextBlock).AutoWrapText(true).Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
			.ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::Danger()))
			.Text_Lambda([Error]() { return Error(); })
			.Visibility_Lambda([Error]() { return Error().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; });
	}

	/** The live version runs the type, or may: its manifest lists it, or could not be read. */
	bool IsLive(const CrowdyExecRevisions::FTypeChange* Change)
	{
		if (!Change)
		{
			return false;
		}
		return Change->Change == EChange::Unchanged || Change->Change == EChange::Changed || Change->Change == EChange::Unknown || Change->Change == EChange::Removed;
	}

	/** One badge for a type: what stops its code from building wins, then how it compares with the live version. */
	TSharedRef<SWidget> ChangeBadge(const CrowdyExecDeploy::FTypeState& Type, const CrowdyExecRevisions::FTypeChange* Change)
	{
		switch (Type.State)
		{
		case CrowdyExecDeploy::ECrateState::NotGenerated:
			return TipBadge(LOCTEXT("CodeNeedsGenerating", "Needs generating"), EBadgeTone::Warning,
				LOCTEXT("CodeNeedsGeneratingTip", "Generate its Server Code first, from its definition or by right-clicking the definition in the Content Browser"));
		case CrowdyExecDeploy::ECrateState::OutOfDate:
			return TipBadge(LOCTEXT("CodeOutOfDate", "Out of date"), EBadgeTone::Warning,
				LOCTEXT("CodeOutOfDateTip", "Its definition changed since its Server Code was generated; generate it again"));
		case CrowdyExecDeploy::ECrateState::Invalid:
			return TipBadge(LOCTEXT("CodeProblem", "Problem"), EBadgeTone::Danger, FText::FromString(Type.Problem));
		default:
			break;
		}
		if (!Change || Change->Change == EChange::NotCompared)
		{
			return TipBadge(LOCTEXT("CodeReady", "Ready"), EBadgeTone::Neutral,
				LOCTEXT("CodeReadyTip", "Its Server Code is ready. It is compared with the live version once the app's versions are read."));
		}
		const FText Live = VersionNumber(Change->LiveVersion);
		switch (Change->Change)
		{
		case EChange::Unchanged:
			return TipBadge(LOCTEXT("NoChanges", "No changes"), EBadgeTone::Success,
				FText::Format(LOCTEXT("NoChangesTip", "Live in version {0} exactly as it is here"), Live));
		case EChange::Changed:
		{
			const FString What = FString::Join(Change->What, TEXT(", "));
			return TipBadge(LOCTEXT("Changed", "Changed"), EBadgeTone::Warning, What.IsEmpty()
				? FText::Format(LOCTEXT("ChangedTip", "Changed since version {0}. Deploy to make it live."), Live)
				: FText::Format(LOCTEXT("ChangedWhatTip", "Changed since version {0}: {1}. Deploy to make it live."), Live, FText::FromString(What)));
		}
		case EChange::New:
			return TipBadge(LOCTEXT("New", "New"), EBadgeTone::Info, LOCTEXT("NewTip", "Not live yet. Deploy to add it."));
		case EChange::Unknown:
			return TipBadge(LOCTEXT("LiveUnknown", "Live code unknown"), EBadgeTone::Neutral,
				LOCTEXT("LiveUnknownTip", "The live version runs code this project has no record of, for example deployed from another copy of the project. Deploying replaces it."));
		default:
			return TipBadge(LOCTEXT("Removed", "Removed"), EBadgeTone::Neutral, LOCTEXT("RemovedTip", "On the server, not in this project"));
		}
	}

	/** What changed between two versions' manifests, root left out: "sbx_counter changed · sbx_locker added". */
	FText ManifestChanges(const TArray<CrowdyExecRevisions::FLiveType>& Before, const TArray<CrowdyExecRevisions::FLiveType>& After, const FText& PreviousVersion)
	{
		TArray<FString> Parts;
		bool bCodeUnknown = false;
		for (const CrowdyExecRevisions::FLiveType& Type : After)
		{
			if (Type.TypeName == CrowdyExecDeploy::RootTypeName)
			{
				continue;
			}
			const CrowdyExecRevisions::FLiveType* Old = Before.FindByPredicate([&Type](const CrowdyExecRevisions::FLiveType& Other) { return Other.TypeName == Type.TypeName; });
			if (!Old)
			{
				Parts.Add(FText::Format(LOCTEXT("TypeAdded", "{0} added"), FText::FromString(Type.TypeName)).ToString());
				continue;
			}
			// A manifest without a digest says nothing about the code, so neither same nor changed code is claimed from it.
			const bool bDigestsKnown = !Old->Digest.IsEmpty() && !Type.Digest.IsEmpty();
			bCodeUnknown |= !bDigestsKnown;
			const bool bCodeChanged = bDigestsKnown && !Old->Digest.Equals(Type.Digest, ESearchCase::CaseSensitive);
			if (bCodeChanged || Old->SaveIntervalMs != Type.SaveIntervalMs || Old->IdleTimeoutMs != Type.IdleTimeoutMs)
			{
				Parts.Add(FText::Format(LOCTEXT("TypeChanged", "{0} changed"), FText::FromString(Type.TypeName)).ToString());
			}
		}
		for (const CrowdyExecRevisions::FLiveType& Type : Before)
		{
			if (Type.TypeName == CrowdyExecDeploy::RootTypeName || After.ContainsByPredicate([&Type](const CrowdyExecRevisions::FLiveType& Other) { return Other.TypeName == Type.TypeName; }))
			{
				continue;
			}
			Parts.Add(FText::Format(LOCTEXT("TypeRemoved", "{0} removed"), FText::FromString(Type.TypeName)).ToString());
		}
		if (Parts.IsEmpty() && bCodeUnknown)
		{
			return FText::GetEmpty();
		}
		if (Parts.IsEmpty())
		{
			return FText::Format(LOCTEXT("SameAsPrevious", "Same server code as version {0}"), PreviousVersion);
		}
		return FText::Format(LOCTEXT("SincePrevious", "Since version {0}: {1}"), PreviousVersion, FText::FromString(FString::Join(Parts, TEXT(" · "))));
	}

	/** The list's answer is not in yet, or it came back empty. */
	void AddPlaceholder(SVerticalBox& List, bool bLoaded, const TCHAR* Icon, const FText& Empty)
	{
		if (!bLoaded)
		{
			List.AddSlot().AutoHeight().Padding(4.f)
			[
				SNew(STextBlock).Text(LOCTEXT("Loading", "Loading...")).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.Subtle")
			];
			return;
		}
		List.AddSlot().AutoHeight()[ CrowdyStudioWidgets::EmptyState(Icon, Empty) ];
	}
}

FText CrowdyServerComputePage::ReadableByPhrase(ECrowdyServerObjectVisibility Visibility)
{
	switch (Visibility)
	{
	case ECrowdyServerObjectVisibility::OwnerOnly: return LOCTEXT("MetaOwnerOnly", "only its owner can read it");
	case ECrowdyServerObjectVisibility::Members: return LOCTEXT("MetaMembers", "readable by its members");
	default: return LOCTEXT("MetaPublic", "readable by every player");
	}
}

FText CrowdyServerComputePage::WhyCrateFolderStays(const FString& TypeName, const FString& CrateDirectory, const FString& ServerDirectory, bool bSharedTypeName)
{
	if (bSharedTypeName)
	{
		return LOCTEXT("FolderShared", "Another definition has the same Type Name and so the same Server Code folder, which is left as it is.");
	}
	// Folder names ignore case on Windows, so an invalid name like Sbx_Counter or sbx_counter/src can land on another type's folder.
	if (!CrowdyServerCodeFiles::IsValidTypeName(TypeName))
	{
		return LOCTEXT("FolderInvalidName", "Its Type Name is not a valid one, so its Server Code folder may be another type's; it is left as it is.");
	}
	if (CrateDirectory.IsEmpty() || !FPaths::IsSamePath(FPaths::GetPath(CrateDirectory), ServerDirectory))
	{
		return LOCTEXT("FolderOutside", "Its Server Code folder is not directly inside the project's Server folder, so it is left as it is.");
	}
	IFileManager& FileManager = IFileManager::Get();
	if (!FileManager.DirectoryExists(*CrateDirectory))
	{
		return LOCTEXT("FolderMissing", "It has no Server Code folder.");
	}
	bool bLink = FileManager.IsSymlink(*CrateDirectory);
	if (!bLink)
	{
		FileManager.IterateDirectoryRecursively(*CrateDirectory, [&FileManager, &bLink](const TCHAR* Path, bool)
		{
			bLink = FileManager.IsSymlink(Path);
			return !bLink;
		});
	}
	if (bLink)
	{
		return LOCTEXT("FolderHasLink", "Its Server Code folder is, or holds, a link to somewhere else, and deleting it could delete what the link points to; it is left as it is, so delete it by hand.");
	}
	return FText::GetEmpty();
}

SCrowdyServerComputePanel::~SCrowdyServerComputePanel()
{
	if (const TSharedPtr<FCrowdyServerComputeService> Pinned = Service.Pin())
	{
		Pinned->OnChanged().Remove(ServiceChangedHandle);
	}
}

void SCrowdyServerComputePanel::Construct(const FArguments& InArgs)
{
	ActiveTab = CrowdyComputeUI::TabOverview;
	LogLevel = TEXT("all");
	StatsWindow = TEXT("60");

	FCrowdyServerComputeService& Compute = FCrowdyServerComputeService::Get();
	Service = Compute.AsWeak();
	ServiceChangedHandle = Compute.OnChanged().AddSP(this, &SCrowdyServerComputePanel::OnServiceChanged);

	// Nothing is asked of the platform until the page is first shown.
	RegisterActiveTimer(0.f, FWidgetActiveTimerDelegate::CreateSP(this, &SCrowdyServerComputePanel::HandleFirstShown));
}

EActiveTimerReturnType SCrowdyServerComputePanel::HandleFirstShown(double InCurrentTime, float InDeltaTime)
{
	bShown = true;
	// Either way the service reads the project's types at once and tells the page, which then builds itself.
	if (!RenewIfStale())
	{
		FCrowdyServerComputeService::Get().Refresh();
	}
	return EActiveTimerReturnType::Stop;
}

bool SCrowdyServerComputePanel::RenewIfStale()
{
	return FCrowdyServerComputeService::Get().RenewClientIfStale();
}

void SCrowdyServerComputePanel::OnServiceChanged()
{
	if (!bShown)
	{
		return;
	}
	UpdateDeployState();
	const TSharedPtr<CrowdyExecDeveloper::FClient> Client = FCrowdyServerComputeService::Get().GetClient();
	const bool bNewClient = Client != SeenClient.Pin() || Client.IsValid() != bHadClient;
	if (bContentBuilt && !bNewClient)
	{
		// Deploy progress changes only the deploy line, which reads the service itself; rebuilding rows would close an open menu.
		if (FCrowdyServerComputeService::Get().GetDataGeneration() != FilledGeneration)
		{
			FillServiceRows();
		}
		FillBuildLog();
		return;
	}

	// Logs and activity read with another client belong to another app or developer.
	SeenClient = Client;
	bHadClient = Client.IsValid();
	ResetPageState();
	RebuildContent();
	RefreshLogs(false);
	RefreshStats();
}

void SCrowdyServerComputePanel::UpdateDeployState()
{
	const FCrowdyServerComputeService& Compute = FCrowdyServerComputeService::Get();
	const CrowdyExecDeploy::FProjectDeploy& Project = Compute.GetProject();
	// Every type is ready and compared with a known live version, and none differs from it.
	bNothingToDeploy = Compute.AreVersionsLoaded() && Compute.GetVersionsError().IsEmpty() && !Project.Types.IsEmpty() && Project.Problems.IsEmpty()
		&& !CrowdyExecRevisions::HasChanges(Compute.GetChanges());
}

void SCrowdyServerComputePanel::ResetPageState()
{
	// Answers still on their way were asked with the previous client, so they are dropped.
	++ActionRequest;
	++LogsRequest;
	++StatsRequest;
	LogLines.Reset();
	Stats.Reset();
	VersionTypes.Reset();
	LogQuery = CrowdyExecDeveloper::FLogQuery();
	Errors.Reset();
	LogFlow.Reset();
	StatsShownWindow.Reset();
	ShownBuildLog.Reset();
	bBuildOutputOpen = false;
	bLogsLoaded = false;
	bStatsLoaded = false;
	bMoreLogs = false;
}

void SCrowdyServerComputePanel::RebuildContent()
{
	bContentBuilt = true;
	HeaderBadgeBox.Reset();
	TypesList.Reset();
	VersionsList.Reset();
	LogsList.Reset();
	ActivityList.Reset();
	LogTypeBox.Reset();
	BuildLogBox.Reset();

	const ISlateStyle& Style = FCrowdyStudioStyle::Get();
	if (!bHadClient)
	{
		ChildSlot
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock).Text(LOCTEXT("PageTitle", "Server Compute")).TextStyle(&Style, "Crowdy.Text.Title")
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 24.f, 0.f, 0.f)
			[
				CrowdyStudioWidgets::EmptyState(TEXT("server"), LOCTEXT("NoClient", "Server Compute needs a sign-in and an app.\nSign in and choose an app on the Project page."))
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.f, 0.f, 0.f, 10.f)
			[
				SNew(STextBlock).AutoWrapText(true).Justification(ETextJustify::Center).TextStyle(&Style, "Crowdy.Text.Subtle")
				.Text_Lambda([]() { return FText::FromString(FCrowdyServerComputeService::Get().GetClientError()); })
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
			[
				SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Secondary")
				.OnClicked(this, &SCrowdyServerComputePanel::OnRefreshClicked)
				[ CrowdyComputeUI::ButtonLabel(LOCTEXT("TryAgain", "Try Again")) ]
			]
		];
		return;
	}

	TArray<FString> TabKeys = { CrowdyComputeUI::TabOverview, CrowdyComputeUI::TabVersions, CrowdyComputeUI::TabLogs, CrowdyComputeUI::TabActivity, CrowdyComputeUI::TabSettings };
	TArray<FText> TabLabels = { LOCTEXT("TabOverview", "Overview"), LOCTEXT("TabVersions", "Versions"), LOCTEXT("TabLogs", "Logs"), LOCTEXT("TabActivity", "Activity"), LOCTEXT("TabSettings", "Settings") };

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 12.f)
		[
			BuildPageHeader()
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 10.f)
		[
			CrowdyStudioWidgets::TabStrip(TabKeys, TabLabels,
				TAttribute<FString>::CreateLambda([this]() { return ActiveTab; }),
				[this](const FString& Tab) { OnTabSelected(Tab); })
		]
		+ SVerticalBox::Slot().FillHeight(1.f)
		[
			SAssignNew(TabSwitcher, SWidgetSwitcher)
			+ SWidgetSwitcher::Slot()[ BuildOverviewTab() ]
			+ SWidgetSwitcher::Slot()[ BuildVersionsTab() ]
			+ SWidgetSwitcher::Slot()[ BuildLogsTab() ]
			+ SWidgetSwitcher::Slot()[ BuildActivityTab() ]
			+ SWidgetSwitcher::Slot()[ BuildSettingsTab() ]
		]
	];
	TabSwitcher->SetActiveWidgetIndex(CrowdyComputeUI::TabIndex(ActiveTab));
	FillServiceRows();
	FillLogs();
	FillActivity();
	FillBuildLog();
}

TSharedRef<SWidget> SCrowdyServerComputePanel::BuildPageHeader()
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock).Text(LOCTEXT("PageTitle", "Server Compute")).TextStyle(&Style, "Crowdy.Text.Title")
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
			[
				SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Body")
				.Text(LOCTEXT("PageSubtitle", "Deploy your Server Objects' server code, then watch how players' calls to it go."))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f, 0.f, 0.f)
			[
				SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Subtle").Text(this, &SCrowdyServerComputePanel::GetHeaderText)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
			[
				BuildErrorLine(CrowdyComputeUI::ErrorAppSwitch)
			]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(12.f, 4.f, 8.f, 0.f)
		[
			SAssignNew(HeaderBadgeBox, SBox)
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top)
		[
			SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Secondary").ContentPadding(FMargin(9.f, 5.f))
			.ToolTipText(LOCTEXT("RefreshTip", "Reads the app's status, versions, logs and activity again, and checks the project's Server Object types"))
			.OnClicked(this, &SCrowdyServerComputePanel::OnRefreshClicked)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 6.f, 0.f)
				[ CrowdyStudioWidgets::Icon(TEXT("refresh"), 14.f, FSlateColor(FCrowdyStudioStyle::TextSecondary())) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[ SNew(STextBlock).Text(LOCTEXT("Refresh", "Refresh")).Font(FCoreStyle::GetDefaultFontStyle("Regular", 9)).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSecondary())) ]
			]
		];
}

TSharedRef<SWidget> SCrowdyServerComputePanel::BuildOverviewTab()
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();
	return SNew(SScrollBox)
		+ SScrollBox::Slot()
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 12.f)
			[
				CrowdyStudioWidgets::Card(
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 10.f)
					[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("TypesHeader", "Server Object types"), TEXT("cube")) ]
					+ SVerticalBox::Slot().AutoHeight()
					[ SAssignNew(TypesList, SVerticalBox) ])
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				CrowdyStudioWidgets::Card(
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)
					[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("DeployHeader", "Deploy"), TEXT("server")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 12.f)
					[
						SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Subtle")
						.Text(LOCTEXT("DeployExplain", "Builds every type's server code on the server and makes it the app's live version. Players use it at once."))
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 12.f, 0.f)
						[
							SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Primary").ContentPadding(FMargin(18.f, 7.f))
							.Visibility_Lambda([this]() { return (bNothingToDeploy && !FCrowdyServerComputeService::Get().IsBusy()) ? EVisibility::Collapsed : EVisibility::Visible; })
							.IsEnabled(this, &SCrowdyServerComputePanel::CanDeploy)
							.OnClicked(this, &SCrowdyServerComputePanel::OnDeployClicked)
							[ CrowdyComputeUI::ButtonLabel(LOCTEXT("Deploy", "Deploy"), 10) ]
						]
						+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(0.f, 0.f, 12.f, 0.f)
						[
							SNew(STextBlock).AutoWrapText(true).Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
							.Text(this, &SCrowdyServerComputePanel::GetDeployLine)
							.ColorAndOpacity(this, &SCrowdyServerComputePanel::GetDeployLineColor)
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[
							SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Ghost").ContentPadding(FMargin(8.f, 4.f))
							.ToolTipText(LOCTEXT("StartersTip", "Deploys the platform's example Server Object types in place of the app's live version, to try Server Compute out"))
							.IsEnabled(this, &SCrowdyServerComputePanel::IsIdle)
							.OnClicked(this, &SCrowdyServerComputePanel::OnDeployStartersClicked)
							[ CrowdyComputeUI::QuietLabel(LOCTEXT("DeployStarters", "Deploy starter pack")) ]
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f).HAlign(HAlign_Left)
					[
						SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Ghost").ContentPadding(FMargin(4.f, 3.f))
						.Visibility_Lambda([this]() { return ShownBuildLog.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
						.OnClicked(this, &SCrowdyServerComputePanel::OnToggleBuildOutput)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 6.f, 0.f)
							[
								SNew(SBox).WidthOverride(12.f).HeightOverride(12.f)
								[
									SNew(SImage)
									.Image_Lambda([this]() { return FCrowdyStudioStyle::IconBrush(bBuildOutputOpen ? TEXT("chevron-down") : TEXT("chevron-right")); })
									.ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSecondary()))
								]
							]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
							[ CrowdyComputeUI::QuietLabel(LOCTEXT("BuildOutput", "Build output")) ]
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
					[
						SNew(SBox).MaxDesiredHeight(280.f)
						.Visibility_Lambda([this]() { return (bBuildOutputOpen && !ShownBuildLog.IsEmpty()) ? EVisibility::Visible : EVisibility::Collapsed; })
						[
							SAssignNew(BuildLogBox, SMultiLineEditableTextBox)
							.Style(&Style, "Crowdy.Input")
							.IsReadOnly(true)
							.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
						]
					])
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
			[
				BuildErrorLine(CrowdyComputeUI::ErrorRemove)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
			[
				BuildErrorLine(CrowdyComputeUI::ErrorSwitch)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
			[
				CrowdyComputeUI::ErrorLine([]() { return FCrowdyServerComputeService::Get().GetStatusError(); })
			]
		];
}

TSharedRef<SWidget> SCrowdyServerComputePanel::BuildVersionsTab()
{
	return SNew(SScrollBox)
		+ SScrollBox::Slot()
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				CrowdyStudioWidgets::Card(SAssignNew(VersionsList, SVerticalBox))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
			[
				BuildErrorLine(CrowdyComputeUI::ErrorActivate)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
			[
				CrowdyComputeUI::ErrorLine([]() { return FCrowdyServerComputeService::Get().GetVersionsError(); })
			]
		];
}

TSharedRef<SWidget> SCrowdyServerComputePanel::BuildLogsTab()
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();
	return SNew(SScrollBox)
		+ SScrollBox::Slot()
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 10.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f).MaxWidth(260.f).VAlign(VAlign_Center).Padding(0.f, 0.f, 10.f, 0.f)
				[
					SAssignNew(LogTypeBox, SEditableTextBox)
					.Style(&Style, "Crowdy.Input")
					.HintText(LOCTEXT("LogTypeHint", "All types"))
					.ToolTipText(LOCTEXT("LogTypeTip", "Type a Server Object type's name and press Enter to see only its lines [node type]"))
					.OnTextCommitted_Lambda([this](const FText&, ETextCommit::Type Commit)
					{
						if (Commit == ETextCommit::OnEnter)
						{
							RefreshLogs(false);
						}
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 10.f, 0.f)
				[
					CrowdyStudioWidgets::SegmentedEnum(
						{ TEXT("all"), TEXT("info"), TEXT("warnings"), TEXT("errors") },
						{ LOCTEXT("LevelAll", "All"), LOCTEXT("LevelInfoFilter", "Info"), LOCTEXT("LevelWarnings", "Warnings"), LOCTEXT("LevelErrors", "Errors") },
						TAttribute<FString>::CreateLambda([this]() { return LogLevel; }),
						[this](const FString& Level) { LogLevel = Level; RefreshLogs(false); })
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SBorder).BorderImage(Style.GetBrush("Crowdy.Chip")).Padding(FMargin(8.f, 1.f, 2.f, 1.f))
					.Visibility_Lambda([this]() { return LogFlow.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
					.ToolTipText_Lambda([this]() { return FText::Format(LOCTEXT("OneCallTip", "Showing only the lines of call {0} [flow]"), FText::FromString(LogFlow)); })
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[ SNew(STextBlock).Text(LOCTEXT("OneCall", "One call")).Font(FCoreStyle::GetDefaultFontStyle("Regular", 9)).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextPrimary())) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.f, 0.f, 0.f, 0.f)
						[
							SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Ghost").ContentPadding(FMargin(2.f))
							.ToolTipText(LOCTEXT("ClearCallTip", "Show every call's lines again"))
							.OnClicked(this, &SCrowdyServerComputePanel::OnClearCallClicked)
							[ CrowdyStudioWidgets::Icon(TEXT("x"), 11.f, FSlateColor(FCrowdyStudioStyle::TextSecondary())) ]
						]
					]
				]
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				CrowdyStudioWidgets::Card(SAssignNew(LogsList, SVerticalBox))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f).HAlign(HAlign_Center)
			[
				SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Secondary")
				.Visibility_Lambda([this]() { return (bMoreLogs && !LogLines.IsEmpty()) ? EVisibility::Visible : EVisibility::Collapsed; })
				.OnClicked(this, &SCrowdyServerComputePanel::OnLoadOlderClicked)
				[ CrowdyComputeUI::ButtonLabel(LOCTEXT("LoadOlder", "Load older")) ]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
			[
				BuildErrorLine(CrowdyComputeUI::TabLogs)
			]
		];
}

TSharedRef<SWidget> SCrowdyServerComputePanel::BuildActivityTab()
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();
	return SNew(SScrollBox)
		+ SScrollBox::Slot()
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 10.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Subtle")
					.Text(LOCTEXT("ActivityExplain", "How players' calls to each Server Function went, busiest first."))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10.f, 0.f, 0.f, 0.f)
				[
					CrowdyStudioWidgets::SegmentedEnum(
						{ TEXT("60"), TEXT("1440"), TEXT("10080") },
						{ LOCTEXT("LastHour", "Last hour"), LOCTEXT("LastDay", "Last day"), LOCTEXT("LastWeek", "Last week") },
						TAttribute<FString>::CreateLambda([this]() { return StatsWindow; }),
						[this](const FString& Window) { StatsWindow = Window; RefreshStats(); })
				]
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SAssignNew(ActivityList, SVerticalBox)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
			[
				BuildErrorLine(CrowdyComputeUI::TabActivity)
			]
		];
}

TSharedRef<SWidget> SCrowdyServerComputePanel::BuildSettingsTab()
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();
	const int32 MinRevisions = UCrowdyServerComputeSettings::MinRevisionsToKeep;
	const int32 MaxRevisions = UCrowdyServerComputeSettings::MaxRevisionsToKeep;
	return SNew(SScrollBox)
		+ SScrollBox::Slot()
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 10.f)
			[
				SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Subtle")
				.Text(LOCTEXT("SettingsExplain", "Server Compute's settings for this project. They are saved with the project, so everyone working on it shares them."))
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				CrowdyStudioWidgets::Card(
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 10.f)
					[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("RevisionsHeader", "Revisions"), TEXT("archive")) ]
					+ SVerticalBox::Slot().AutoHeight()
					[
						CrowdyStudioWidgets::Field(LOCTEXT("RevisionsToKeep", "Revisions to keep"),
							SNew(SBox).WidthOverride(160.f).HAlign(HAlign_Left)
							[
								SNew(SSpinBox<int32>)
								.MinValue(MinRevisions).MaxValue(MaxRevisions).MinSliderValue(MinRevisions).MaxSliderValue(MaxRevisions)
								.Delta(1)
								.Value_Lambda([]() { return GetDefault<UCrowdyServerComputeSettings>()->GetRevisionsToKeep(); })
								.OnValueCommitted_Lambda([this](int32 Value, ETextCommit::Type)
								{
									const bool bSaved = GetMutableDefault<UCrowdyServerComputeSettings>()->SetRevisionsToKeep(Value);
									SetError(CrowdyComputeUI::ErrorSettings, bSaved ? FText::GetEmpty()
										: LOCTEXT("SettingsNotSaved", "Could not save to Config/DefaultEditor.ini, so the change lasts only until the editor closes. Check that the file can be written, for example that it is not read-only."));
								})
							],
							FText::Format(LOCTEXT("RevisionsToKeepHint", "Per type, how many deployed revisions of its server code the project keeps to view, compare or restore ({0} to {1}). Saved in Config/DefaultEditor.ini; keeping fewer takes effect at the next deploy."),
								MinRevisions, MaxRevisions))
					])
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
			[
				BuildErrorLine(CrowdyComputeUI::ErrorSettings)
			]
		];
}

TSharedRef<SWidget> SCrowdyServerComputePanel::BuildErrorLine(const FString& Key)
{
	return CrowdyComputeUI::ErrorLine([this, Key]() { return Errors.FindRef(Key); });
}

FText SCrowdyServerComputePanel::GetHeaderText() const
{
	const FCrowdyServerComputeService& Compute = FCrowdyServerComputeService::Get();
	const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>();
	const FText Line = FText::Format(LOCTEXT("HeaderTarget", "{0}."), Compute.GetAppTarget());
	const TSharedPtr<CrowdyExecDeveloper::FClient> Client = Compute.GetClient();
	if (!Client || Client->GetAppId() == Settings->AppID)
	{
		return Line;
	}
	return FText::Format(LOCTEXT("HeaderChanged", "{0} The project's app has changed since; press Refresh to show it."), Line);
}

TSharedRef<SWidget> SCrowdyServerComputePanel::MakeStatusBadge() const
{
	using CrowdyStudioWidgets::EBadgeTone;
	const CrowdyExecDeveloper::FAppStatus& Status = FCrowdyServerComputeService::Get().GetStatus().GetValue();
	if (Status.bBudgetPaused)
	{
		return CrowdyComputeUI::TipBadge(LOCTEXT("BudgetPaused", "Budget paused"), EBadgeTone::Danger,
			LOCTEXT("BudgetPausedTip", "The app used up its Server Compute budget, so players' calls are refused until it is raised"));
	}
	if (Status.bDisabled)
	{
		return CrowdyComputeUI::TipBadge(LOCTEXT("AppOff", "Switched off"), EBadgeTone::Warning,
			LOCTEXT("AppOffTip", "The app's Server Compute is switched off, so players' calls are refused"));
	}
	if (!Status.ActiveVersion.IsSet())
	{
		return CrowdyStudioWidgets::Badge(LOCTEXT("NothingDeployed", "Nothing deployed"), EBadgeTone::Neutral);
	}
	return CrowdyStudioWidgets::Badge(FText::Format(LOCTEXT("LiveVersion", "Live: version {0}"), CrowdyComputeUI::VersionNumber(Status.ActiveVersion.GetValue())), EBadgeTone::Success);
}

void SCrowdyServerComputePanel::FillServiceRows()
{
	FilledGeneration = FCrowdyServerComputeService::Get().GetDataGeneration();
	FillHeaderBadge();
	FillTypes();
	FillVersions();
}

void SCrowdyServerComputePanel::FillHeaderBadge()
{
	if (!HeaderBadgeBox)
	{
		return;
	}
	const TOptional<CrowdyExecDeveloper::FAppStatus>& Status = FCrowdyServerComputeService::Get().GetStatus();
	if (!Status.IsSet())
	{
		HeaderBadgeBox->SetContent(SNullWidget::NullWidget);
		return;
	}
	if (!Status->bDisabled)
	{
		HeaderBadgeBox->SetContent(MakeStatusBadge());
		return;
	}
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();
	HeaderBadgeBox->SetContent(
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			MakeStatusBadge()
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
		[
			SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Secondary").ContentPadding(FMargin(9.f, 4.f))
			.ToolTipText(LOCTEXT("AppOnTip", "Lets players call the app's Server Functions again"))
			.IsEnabled(this, &SCrowdyServerComputePanel::CanSwitch)
			.OnClicked(this, &SCrowdyServerComputePanel::OnSwitchAppOnClicked)
			[ CrowdyComputeUI::ButtonLabel(LOCTEXT("AppOn", "Switch on")) ]
		]);
}

void SCrowdyServerComputePanel::FillTypes()
{
	if (!TypesList)
	{
		return;
	}
	const FCrowdyServerComputeService& Compute = FCrowdyServerComputeService::Get();
	TypesList->ClearChildren();
	int32 Rows = 0;
	for (const CrowdyExecDeploy::FTypeState& Type : Compute.GetProject().Types)
	{
		TypesList->AddSlot().AutoHeight().Padding(0.f, Rows++ == 0 ? 0.f : 14.f, 0.f, 0.f)[ MakeTypeRow(Type) ];
	}
	for (const CrowdyExecRevisions::FTypeChange& Change : Compute.GetChanges())
	{
		if (Change.Change != CrowdyExecRevisions::EChange::Removed || Change.TypeName == CrowdyExecDeploy::RootTypeName)
		{
			continue;
		}
		TypesList->AddSlot().AutoHeight().Padding(0.f, Rows++ == 0 ? 0.f : 14.f, 0.f, 0.f)[ MakeRemovedRow(Change.TypeName) ];
	}
	if (Rows == 0)
	{
		TypesList->AddSlot().AutoHeight()
		[
			CrowdyStudioWidgets::EmptyState(TEXT("cube"), LOCTEXT("NoTypes", "No Server Object types yet.\nCreate a Server Object Definition asset to add one."))
		];
	}
}

TSharedRef<SWidget> SCrowdyServerComputePanel::MakeTypeRow(const CrowdyExecDeploy::FTypeState& Type)
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();
	const FCrowdyServerComputeService& Compute = FCrowdyServerComputeService::Get();
	const CrowdyExecRevisions::FTypeChange* Change = Compute.FindChange(Type.TypeName);
	// The service has the project's definitions loaded; nothing is loaded here.
	const UCrowdyServerObjectDefinition* Definition = Cast<UCrowdyServerObjectDefinition>(FSoftObjectPath(Type.AssetPath).ResolveObject());
	const FString Description = Definition ? Definition->Description.TrimStartAndEnd() : FString();
	const FText Meta = MakeTypeMeta(Definition, Type.TypeName);

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(FText::FromString(Type.TypeName)).TextStyle(&Style, "Crowdy.Text.BodyStrong")
				.ToolTipText(FText::Format(LOCTEXT("TypeTip", "Type Name [node type], defined by {0}"), FText::FromString(Type.AssetPath)))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10.f, 0.f, 0.f, 0.f)
			[
				CrowdyComputeUI::ChangeBadge(Type, Change)
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(14.f, 0.f, 0.f, 0.f)
			[
				MakeRunningSwitch(Type.TypeName)
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.f, 0.f, 0.f, 0.f)
			[
				MakeTypeMenu(Type.TypeName, Type.AssetPath)
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 3.f, 0.f, 0.f)
		[
			SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Body").Text(FText::FromString(Description))
			.Visibility(Description.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 3.f, 0.f, 0.f)
		[
			SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Subtle").Text(Meta)
			.Visibility(Meta.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 3.f, 0.f, 0.f)
		[
			SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Subtle").Text(FText::FromString(Type.Problem))
			.ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::Warning()))
			.Visibility(Type.Problem.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
		];
}

TSharedRef<SWidget> SCrowdyServerComputePanel::MakeRemovedRow(const FString& TypeName)
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();
	// A deploy needs at least one type, so with none left nothing can take it off.
	const FText Line = FCrowdyServerComputeService::Get().GetProject().Types.IsEmpty()
		? LOCTEXT("RemovedLineNoTypes", "On the server, not in this project. It keeps running until the project has a type to deploy again, since a deploy needs at least one type. Switch it off here to stop players' calls to it.")
		: LOCTEXT("RemovedLine", "On the server, not in this project. Your next deploy removes it.");
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(FText::FromString(TypeName)).TextStyle(&Style, "Crowdy.Text.Body")
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10.f, 0.f, 0.f, 0.f)
			[
				CrowdyComputeUI::TipBadge(LOCTEXT("Removed", "Removed"), CrowdyStudioWidgets::EBadgeTone::Neutral, LOCTEXT("RemovedTip", "On the server, not in this project"))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(14.f, 0.f, 0.f, 0.f)
			[
				MakeRunningSwitch(TypeName)
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 3.f, 0.f, 0.f)
		[
			SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Subtle").Text(Line)
		];
}

TSharedRef<SWidget> SCrowdyServerComputePanel::MakeRunningSwitch(const FString& TypeName)
{
	const FCrowdyServerComputeService& Compute = FCrowdyServerComputeService::Get();
	const TOptional<CrowdyExecDeveloper::FAppStatus>& Status = Compute.GetStatus();
	if (!Status.IsSet())
	{
		return SNullWidget::NullWidget;
	}
	const bool bOff = Status->DisabledTypes.Contains(TypeName);
	// A type switched off can always be switched on again; otherwise only what the live version runs has anything to switch.
	if (!bOff && !CrowdyComputeUI::IsLive(Compute.FindChange(TypeName)))
	{
		return SNullWidget::NullWidget;
	}
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
			.Text(bOff ? LOCTEXT("TypeOff", "Switched off") : LOCTEXT("TypeRunning", "Running"))
			.ColorAndOpacity(FSlateColor(bOff ? FCrowdyStudioStyle::Warning() : FCrowdyStudioStyle::TextSecondary()))
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.f, 0.f, 0.f, 0.f)
		[
			SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Ghost").ContentPadding(FMargin(8.f, 4.f))
			.ToolTipText(bOff ? LOCTEXT("SwitchOnTip", "Lets players call this type's Server Functions again")
				: LOCTEXT("SwitchOffTip", "Refuses players' calls to this type and stops its Server Objects, for example while you fix a bug"))
			.IsEnabled(this, &SCrowdyServerComputePanel::CanSwitch)
			.OnClicked(this, &SCrowdyServerComputePanel::OnSwitchClicked, TypeName, bOff)
			[ CrowdyComputeUI::QuietLabel(bOff ? LOCTEXT("SwitchOn", "Switch on") : LOCTEXT("SwitchOff", "Switch off")) ]
		];
}

TSharedRef<SWidget> SCrowdyServerComputePanel::MakeTypeMenu(const FString& TypeName, const FString& AssetPath)
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();
	return SNew(SComboButton)
		.ButtonStyle(&Style, "Crowdy.Button.Ghost")
		.ContentPadding(FMargin(7.f, 3.f))
		.HasDownArrow(false)
		.ToolTipText(LOCTEXT("TypeMenuTip", "More for this type"))
		.ButtonContent()
		[
			CrowdyComputeUI::ButtonLabel(LOCTEXT("TypeMenu", "..."), 10)
		]
		.MenuContent()
		[
			SNew(SBorder).BorderImage(Style.GetBrush("Crowdy.Card")).Padding(4.f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Ghost").ContentPadding(FMargin(12.f, 6.f)).HAlign(HAlign_Left)
					.ToolTipText(LOCTEXT("OpenDefinitionTip", "Opens the type's definition asset"))
					.OnClicked(this, &SCrowdyServerComputePanel::OnOpenDefinitionClicked, AssetPath)
					[ SNew(STextBlock).Text(LOCTEXT("OpenDefinition", "Open definition")).Font(FCoreStyle::GetDefaultFontStyle("Regular", 10)).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextPrimary())) ]
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Ghost").ContentPadding(FMargin(12.f, 6.f)).HAlign(HAlign_Left)
					.ToolTipText(LOCTEXT("RemoveTypeTip", "Deletes the definition and, when it is the type's own, its Server Code folder. The type keeps running on the server until a deploy removes it."))
					.IsEnabled(this, &SCrowdyServerComputePanel::IsIdle)
					.OnClicked(this, &SCrowdyServerComputePanel::OnRemoveTypeClicked, TypeName, AssetPath)
					[ SNew(STextBlock).Text(LOCTEXT("RemoveType", "Remove from project...")).Font(FCoreStyle::GetDefaultFontStyle("Regular", 10)).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::Danger())) ]
				]
			]
		];
}

FText SCrowdyServerComputePanel::MakeTypeMeta(const UCrowdyServerObjectDefinition* Definition, const FString& TypeName) const
{
	TArray<FString> Parts;
	if (Definition)
	{
		Parts.Add(FText::Format(LOCTEXT("MetaFunctions", "{0} {0}|plural(one=function,other=functions)"), Definition->Functions.Num()).ToString());
		Parts.Add(FText::Format(LOCTEXT("MetaWatched", "{0} {0}|plural(one=variable,other=variables) visible to players"), Definition->WatchedFields.Num()).ToString());
		Parts.Add(CrowdyServerComputePage::ReadableByPhrase(Definition->Visibility).ToString());
	}
	if (bStatsLoaded)
	{
		double Calls = 0.0;
		for (const CrowdyExecDeveloper::FEndpointStat& Stat : Stats)
		{
			Calls += Stat.NodeType.Equals(TypeName, ESearchCase::CaseSensitive) ? Stat.Calls : 0.0;
		}
		Parts.Add(CrowdyComputeUI::CallsPhrase(Calls, CrowdyComputeUI::WindowPhrase(StatsShownWindow)).ToString());
	}
	return FText::FromString(FString::Join(Parts, TEXT(" · ")));
}

void SCrowdyServerComputePanel::FillVersions()
{
	if (!VersionsList)
	{
		return;
	}
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();
	const FCrowdyServerComputeService& Compute = FCrowdyServerComputeService::Get();
	const TArray<CrowdyExecDeveloper::FVersion>& Versions = Compute.GetVersions();
	VersionsList->ClearChildren();
	if (Versions.IsEmpty())
	{
		if (Compute.GetVersionsError().IsEmpty())
		{
			CrowdyComputeUI::AddPlaceholder(*VersionsList, Compute.AreVersionsLoaded(), TEXT("archive"), LOCTEXT("NoVersions", "Nothing deployed yet.\nDeploy from the Overview tab."));
		}
		return;
	}

	for (int32 Index = 0; Index < Versions.Num(); ++Index)
	{
		const CrowdyExecDeveloper::FVersion& Version = Versions[Index];
		const FText Types = FText::Format(LOCTEXT("VersionTypes", "{0} {0}|plural(one=type,other=types)"), Version.Types);
		const FDateTime CreatedAt = Version.CreatedAt;
		const FString CreatedBy = Version.CreatedBy;
		const FText Changes = DescribeVersionChanges(Version);

		VersionsList->AddSlot().AutoHeight().Padding(0.f, Index == 0 ? 0.f : 12.f, 0.f, 0.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(STextBlock).Text(FText::Format(LOCTEXT("VersionName", "Version {0}"), CrowdyComputeUI::VersionNumber(Version.Version))).TextStyle(&Style, "Crowdy.Text.BodyStrong")
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
					[
						Version.bActive ? CrowdyStudioWidgets::Badge(LOCTEXT("Active", "Active"), CrowdyStudioWidgets::EBadgeTone::Success) : SNullWidget::NullWidget
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f, 0.f, 0.f)
				[
					SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle")
					.Text_Lambda([CreatedAt, CreatedBy, Types]() { return CrowdyComputeUI::VersionDetail(CreatedAt, CreatedBy, Types); })
					.ToolTipText(FText::Format(LOCTEXT("VersionWhenTip", "Deployed {0}."), CrowdyComputeUI::AbsoluteTime(Version.CreatedAt)))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f, 0.f, 0.f)
				[
					SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Subtle").Text(Changes)
					.ToolTipText(LOCTEXT("VersionChangesTip", "The types whose server code or settings differ from the version before it"))
					.Visibility(Changes.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Secondary").ContentPadding(FMargin(12.f, 5.f))
				.Visibility(Version.bActive ? EVisibility::Collapsed : EVisibility::Visible)
				.ToolTipText(LOCTEXT("MakeActiveTip", "Players use this version's server code from now on, for example to roll back a bad deploy"))
				.IsEnabled(this, &SCrowdyServerComputePanel::IsIdle)
				.OnClicked(this, &SCrowdyServerComputePanel::OnMakeActiveClicked, Version.Version)
				[ CrowdyComputeUI::ButtonLabel(LOCTEXT("MakeActive", "Make active")) ]
			]
		];
	}
}

bool SCrowdyServerComputePanel::CacheVersionTypes(const CrowdyExecDeveloper::FVersion& Version)
{
	if (VersionTypes.Contains(Version.Version))
	{
		return true;
	}
	TArray<CrowdyExecRevisions::FLiveType> Types;
	if (!CrowdyExecRevisions::ParseManifest(Version.ManifestJson, Types))
	{
		return false;
	}
	VersionTypes.Add(Version.Version, MoveTemp(Types));
	return true;
}

FText SCrowdyServerComputePanel::DescribeVersionChanges(const CrowdyExecDeveloper::FVersion& Version)
{
	const CrowdyExecDeveloper::FVersion* Previous = nullptr;
	for (const CrowdyExecDeveloper::FVersion& Other : FCrowdyServerComputeService::Get().GetVersions())
	{
		if (Other.Version < Version.Version && (!Previous || Other.Version > Previous->Version))
		{
			Previous = &Other;
		}
	}
	// Both are cached before either is looked up, since adding one can move the other.
	if (!Previous || !CacheVersionTypes(Version) || !CacheVersionTypes(*Previous))
	{
		return FText::GetEmpty();
	}
	return CrowdyComputeUI::ManifestChanges(VersionTypes.FindChecked(Previous->Version), VersionTypes.FindChecked(Version.Version), CrowdyComputeUI::VersionNumber(Previous->Version));
}

void SCrowdyServerComputePanel::FillLogs()
{
	if (!LogsList)
	{
		return;
	}
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();
	LogsList->ClearChildren();
	if (LogLines.IsEmpty())
	{
		if (!Errors.Contains(CrowdyComputeUI::TabLogs))
		{
			CrowdyComputeUI::AddPlaceholder(*LogsList, bLogsLoaded, TEXT("search"), LOCTEXT("NoLogs", "No log lines match.\nLines are kept for 24 hours."));
		}
		return;
	}

	for (int32 Index = 0; Index < LogLines.Num(); ++Index)
	{
		const CrowdyExecDeveloper::FLogLine& Line = LogLines[Index];
		const FDateTime At = Line.At;
		const FString NodeType = Line.NodeType;
		const FString Key = Line.Key;
		const bool bCanShowCall = !Line.Flow.IsEmpty() && LogFlow.IsEmpty();

		LogsList->AddSlot().AutoHeight().Padding(0.f, Index == 0 ? 0.f : 10.f, 0.f, 0.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0.f, 5.f, 10.f, 0.f)
			[
				SNew(SBox).ToolTipText(CrowdyComputeUI::LevelName(Line.Level))[ CrowdyComputeUI::Dot(CrowdyComputeUI::ToneForLevel(Line.Level)) ]
			]
			+ SHorizontalBox::Slot().FillWidth(1.f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(Line.Text))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10)).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextPrimary()))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f, 0.f, 0.f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
					[
						SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle")
						.Text_Lambda([At, NodeType, Key]() { return CrowdyComputeUI::LogMeta(At, NodeType, Key); })
						.ToolTipText(FText::Format(LOCTEXT("LogMetaTip", "Written {0} by the Server Object type [node type] and Instance Id [key] shown"), CrowdyComputeUI::AbsoluteTime(Line.At)))
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Ghost").ContentPadding(FMargin(6.f, 2.f))
						.Visibility(bCanShowCall ? EVisibility::Visible : EVisibility::Collapsed)
						.ToolTipText(LOCTEXT("ShowCallTip", "Show only the lines of this call [flow]"))
						.OnClicked(this, &SCrowdyServerComputePanel::OnShowCallClicked, Line.Flow)
						[ CrowdyComputeUI::QuietLabel(LOCTEXT("ShowCall", "Show this call")) ]
					]
				]
			]
		];
	}
}

void SCrowdyServerComputePanel::FillActivity()
{
	if (!ActivityList)
	{
		return;
	}
	using CrowdyStudioWidgets::EBadgeTone;
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();
	ActivityList->ClearChildren();
	if (Stats.IsEmpty())
	{
		if (!Errors.Contains(CrowdyComputeUI::TabActivity))
		{
			CrowdyComputeUI::AddPlaceholder(*ActivityList, bStatsLoaded, TEXT("clock"), LOCTEXT("NoCalls", "No calls yet in this period."));
		}
		return;
	}

	TArray<CrowdyExecDeveloper::FEndpointStat> Busiest = Stats;
	Busiest.Sort([](const CrowdyExecDeveloper::FEndpointStat& A, const CrowdyExecDeveloper::FEndpointStat& B) { return A.Calls > B.Calls; });
	const FText Window = CrowdyComputeUI::WindowPhrase(StatsShownWindow);

	for (int32 Index = 0; Index < Busiest.Num(); ++Index)
	{
		const CrowdyExecDeveloper::FEndpointStat& Stat = Busiest[Index];
		const double ServerErrors = Stat.AppErrors + Stat.OtherErrors;
		const double Failed = ServerErrors + Stat.Busy + Stat.Denied + Stat.DeadlineExceeded;
		const double Succeeded = FMath::Max(0.0, Stat.Calls - Failed);

		TSharedRef<SWrapBox> Phrases = SNew(SWrapBox).UseAllottedSize(true);
		auto AddPhrase = [&Phrases](const FText& Text, const FLinearColor& Color)
		{
			Phrases->AddSlot().Padding(0.f, 0.f, 16.f, 0.f)
			[
				SNew(STextBlock).Text(Text).Font(FCoreStyle::GetDefaultFontStyle("Regular", 10)).ColorAndOpacity(FSlateColor(Color))
			];
		};
		AddPhrase(CrowdyComputeUI::CallsPhrase(Stat.Calls, Window), FCrowdyStudioStyle::TextPrimary());
		if (Stat.Calls > 0.0 && Failed <= 0.0)
		{
			AddPhrase(LOCTEXT("NoFailures", "no failures"), FCrowdyStudioStyle::Success());
		}
		else if (Stat.Calls > 0.0)
		{
			const int32 Percent = FMath::FloorToInt(100.0 * Succeeded / Stat.Calls);
			AddPhrase(FText::Format(LOCTEXT("SucceededPhrase", "{0}% succeeded"), Percent), FCrowdyStudioStyle::TextSecondary());
		}
		if (Stat.LatencyMsAvg.IsSet())
		{
			const FText Usual = CrowdyComputeUI::Duration(Stat.LatencyMsAvg.GetValue());
			AddPhrase(Stat.LatencyMsMax.IsSet()
				? FText::Format(LOCTEXT("LatencyPhrase", "usually answers in {0} (slowest {1})"), Usual, CrowdyComputeUI::Duration(Stat.LatencyMsMax.GetValue()))
				: FText::Format(LOCTEXT("LatencyAvgPhrase", "usually answers in {0}"), Usual), FCrowdyStudioStyle::TextSecondary());
		}

		TSharedRef<SHorizontalBox> Problems = SNew(SHorizontalBox);
		auto AddProblem = [&Problems](double Count, const FText& Format, EBadgeTone Tone, const FText& ToolTip)
		{
			if (Count <= 0.0)
			{
				return;
			}
			Problems->AddSlot().AutoWidth().Padding(6.f, 0.f, 0.f, 0.f)
			[
				CrowdyComputeUI::TipBadge(FText::Format(Format, CrowdyComputeUI::CompactCount(Count)), Tone, ToolTip)
			];
		};
		AddProblem(ServerErrors, LOCTEXT("ServerErrors", "{0} server errors"), EBadgeTone::Danger,
			LOCTEXT("ServerErrorsTip", "Calls the Server Function answered with an error, or that failed on the server [app errors, other errors]"));
		AddProblem(Stat.DeadlineExceeded, LOCTEXT("TimedOut", "{0} timed out"), EBadgeTone::Warning,
			LOCTEXT("TimedOutTip", "Calls that ran past their deadline [deadline exceeded]"));
		AddProblem(Stat.Denied, LOCTEXT("Refused", "{0} refused"), EBadgeTone::Warning,
			LOCTEXT("RefusedTip", "Calls refused because the caller was not allowed to make them [denied]"));
		AddProblem(Stat.Busy, LOCTEXT("Busy", "{0} busy"), EBadgeTone::Warning,
			LOCTEXT("BusyTip", "Calls refused because the server was busy; the caller may try again [busy]"));

		ActivityList->AddSlot().AutoHeight().Padding(0.f, Index == 0 ? 0.f : 8.f, 0.f, 0.f)
		[
			CrowdyStudioWidgets::Card(
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
					[
						SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.BodyStrong")
						.Text(FText::Format(LOCTEXT("FunctionName", "{0}.{1}"), FText::FromString(Stat.NodeType), FText::FromString(Stat.Method)))
						.ToolTipText(LOCTEXT("FunctionNameTip", "Server Object type [node type] and Server Function [method]"))
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						Problems
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
				[
					Phrases
				],
				FMargin(14.f, 10.f))
		];
	}
}

void SCrowdyServerComputePanel::FillBuildLog()
{
	const FCrowdyServerComputeService& Compute = FCrowdyServerComputeService::Get();
	const FString& Log = Compute.GetBuildLog();
	if (!BuildLogBox || Log.Equals(ShownBuildLog, ESearchCase::CaseSensitive))
	{
		return;
	}
	ShownBuildLog = Log;
	// A new build's output opens by itself only when the build failed.
	bBuildOutputOpen = !Log.IsEmpty() && Compute.DidLastBuildFail();
	BuildLogBox->SetText(FText::FromString(Log));
}

void SCrowdyServerComputePanel::RefreshLogs(bool bOlder)
{
	const TSharedPtr<CrowdyExecDeveloper::FClient> Client = FCrowdyServerComputeService::Get().GetClient();
	if (!Client || !LogTypeBox)
	{
		return;
	}
	if (!bOlder)
	{
		LogQuery = CrowdyExecDeveloper::FLogQuery();
		LogQuery.NodeType = LogTypeBox->GetText().ToString().TrimStartAndEnd();
		LogQuery.Flow = LogFlow;
		LogQuery.MaxLevel = CrowdyComputeUI::MaxLevelFor(LogLevel);
		LogQuery.Limit = CrowdyComputeUI::LogPageSize;
	}
	CrowdyExecDeveloper::FLogQuery Query = LogQuery;
	if (bOlder && !LogLines.IsEmpty())
	{
		Query.Before = LogLines.Last().Id;
	}

	// A newer request replaces the list, so an answer that arrives after it is dropped.
	const int32 Request = ++LogsRequest;
	TWeakPtr<SCrowdyServerComputePanel> WeakThis = SharedThis(this);
	Client->Logs(Query, [WeakThis, Request, bOlder](const CrowdyExecDeveloper::TResult<TArray<CrowdyExecDeveloper::FLogLine>>& Result)
	{
		const TSharedPtr<SCrowdyServerComputePanel> Self = WeakThis.Pin();
		if (!Self || Request != Self->LogsRequest)
		{
			return;
		}
		if (!Result.bOk)
		{
			Self->SetError(CrowdyComputeUI::TabLogs, CrowdyComputeUI::ErrorText(LOCTEXT("LogsFailed", "Could not read the logs"), Result.Error));
			Self->FillLogs();
			return;
		}
		Self->SetError(CrowdyComputeUI::TabLogs, FText::GetEmpty());
		if (!bOlder)
		{
			Self->LogLines.Reset();
		}
		Self->LogLines.Append(Result.Value);
		Self->bMoreLogs = Result.Value.Num() >= CrowdyComputeUI::LogPageSize;
		Self->bLogsLoaded = true;
		Self->FillLogs();
	});
}

void SCrowdyServerComputePanel::RefreshStats()
{
	const TSharedPtr<CrowdyExecDeveloper::FClient> Client = FCrowdyServerComputeService::Get().GetClient();
	if (!Client)
	{
		return;
	}
	const int32 Request = ++StatsRequest;
	const FString Window = StatsWindow;
	TWeakPtr<SCrowdyServerComputePanel> WeakThis = SharedThis(this);
	Client->EndpointStats(FString(), FCString::Atoi(*Window), [WeakThis, Request, Window](const CrowdyExecDeveloper::TResult<TArray<CrowdyExecDeveloper::FEndpointStat>>& Result)
	{
		const TSharedPtr<SCrowdyServerComputePanel> Self = WeakThis.Pin();
		if (!Self || Request != Self->StatsRequest)
		{
			return;
		}
		if (!Result.bOk)
		{
			Self->SetError(CrowdyComputeUI::TabActivity, CrowdyComputeUI::ErrorText(LOCTEXT("StatsFailed", "Could not read the activity"), Result.Error));
			Self->FillActivity();
			return;
		}
		Self->SetError(CrowdyComputeUI::TabActivity, FText::GetEmpty());
		Self->Stats = Result.Value;
		Self->StatsShownWindow = Window;
		Self->bStatsLoaded = true;
		Self->FillActivity();
		Self->FillTypes();
	});
}

void SCrowdyServerComputePanel::SetError(const FString& Key, const FText& Error)
{
	if (Error.IsEmpty())
	{
		Errors.Remove(Key);
		return;
	}
	Errors.Add(Key, Error);
}

TFunction<void(const CrowdyExecDeveloper::TResult<CrowdyExecDeveloper::FAppStatus>&)> SCrowdyServerComputePanel::MakeActionAnswer(FCrowdyServerComputeService& Compute, const FString& ErrorKey, const FText& Failed)
{
	const int32 Action = ++ActionRequest;
	TWeakPtr<SCrowdyServerComputePanel> WeakThis = SharedThis(this);
	TWeakPtr<FCrowdyServerComputeService> WeakService = Compute.AsWeak();
	return [WeakThis, WeakService, Action, ErrorKey, Failed](const CrowdyExecDeveloper::TResult<CrowdyExecDeveloper::FAppStatus>& Result)
	{
		const TSharedPtr<FCrowdyServerComputeService> Pinned = WeakService.Pin();
		if (!Pinned)
		{
			return;
		}
		// The action ends even when the page is gone or no longer wants the answer, or no deploy could start again.
		Pinned->EndAction();
		const TSharedPtr<SCrowdyServerComputePanel> Self = WeakThis.Pin();
		if (!Self || Action != Self->ActionRequest)
		{
			return;
		}
		if (!Result.bOk)
		{
			Self->SetError(ErrorKey, CrowdyComputeUI::ErrorText(Failed, Result.Error));
			return;
		}
		Self->SetError(ErrorKey, FText::GetEmpty());
		Pinned->ApplyStatus(Result.Value);
	};
}

void SCrowdyServerComputePanel::OnTabSelected(const FString& Tab)
{
	ActiveTab = Tab;
	if (TabSwitcher)
	{
		TabSwitcher->SetActiveWidgetIndex(CrowdyComputeUI::TabIndex(Tab));
	}
	// Server Code may have been generated since; checking it reads only local files.
	if (Tab == CrowdyComputeUI::TabOverview && FPlatformTime::Seconds() - TypesCheckedSeconds > CrowdyComputeUI::TypesRecheckSeconds)
	{
		TypesCheckedSeconds = FPlatformTime::Seconds();
		FCrowdyServerComputeService::Get().RefreshTypes();
	}
}

FReply SCrowdyServerComputePanel::OnRefreshClicked()
{
	if (RenewIfStale())
	{
		return FReply::Handled();
	}
	FCrowdyServerComputeService::Get().Refresh();
	RefreshLogs(false);
	RefreshStats();
	return FReply::Handled();
}

FReply SCrowdyServerComputePanel::OnSwitchClicked(FString TypeName, bool bEnable)
{
	if (RenewIfStale())
	{
		return FReply::Handled();
	}
	FCrowdyServerComputeService& Compute = FCrowdyServerComputeService::Get();
	const TSharedPtr<CrowdyExecDeveloper::FClient> Client = Compute.GetClient();
	if (!Client || !IsIdle())
	{
		return FReply::Handled();
	}
	const FText Type = FText::FromString(TypeName);
	if (!bEnable && !CrowdyComputeUI::Confirm(FText::Format(LOCTEXT("ConfirmOff", "Players' calls to {0} in {1} are refused until it is switched on again. Its Server Objects are saved and stopped."), Type, Compute.GetAppTarget()),
		FText::Format(LOCTEXT("ConfirmOffTitle", "Switch off {0}"), Type)))
	{
		return FReply::Handled();
	}
	// A deploy may have started while the confirm was open.
	if (!Compute.TryBeginAction())
	{
		return FReply::Handled();
	}
	const FText Failed = FText::Format(bEnable ? LOCTEXT("SwitchOnFailed", "Could not switch {0} on") : LOCTEXT("SwitchOffFailed", "Could not switch {0} off"), Type);
	Client->SetEnabled(bEnable, TypeName, MakeActionAnswer(Compute, CrowdyComputeUI::ErrorSwitch, Failed));
	return FReply::Handled();
}

FReply SCrowdyServerComputePanel::OnSwitchAppOnClicked()
{
	if (RenewIfStale())
	{
		return FReply::Handled();
	}
	FCrowdyServerComputeService& Compute = FCrowdyServerComputeService::Get();
	const TSharedPtr<CrowdyExecDeveloper::FClient> Client = Compute.GetClient();
	if (!Client || !IsIdle())
	{
		return FReply::Handled();
	}
	const FText Target = Compute.GetAppTarget();
	if (!CrowdyComputeUI::Confirm(FText::Format(LOCTEXT("ConfirmAppOn", "Players can call the Server Functions of {0} again."), Target),
		FText::Format(LOCTEXT("ConfirmAppOnTitle", "Switch on {0}"), Target)))
	{
		return FReply::Handled();
	}
	if (!Compute.TryBeginAction())
	{
		return FReply::Handled();
	}
	Client->SetEnabled(true, FString(), MakeActionAnswer(Compute, CrowdyComputeUI::ErrorAppSwitch, LOCTEXT("AppOnFailed", "Could not switch the app on")));
	return FReply::Handled();
}

FReply SCrowdyServerComputePanel::OnDeployClicked()
{
	if (RenewIfStale() || !CanDeploy())
	{
		return FReply::Handled();
	}
	FCrowdyServerComputeService::Get().Deploy();
	return FReply::Handled();
}

FReply SCrowdyServerComputePanel::OnDeployStartersClicked()
{
	if (RenewIfStale() || !IsIdle())
	{
		return FReply::Handled();
	}
	FCrowdyServerComputeService::Get().DeployStarters();
	return FReply::Handled();
}

FReply SCrowdyServerComputePanel::OnMakeActiveClicked(int32 Version)
{
	if (RenewIfStale())
	{
		return FReply::Handled();
	}
	FCrowdyServerComputeService& Compute = FCrowdyServerComputeService::Get();
	const TSharedPtr<CrowdyExecDeveloper::FClient> Client = Compute.GetClient();
	if (!Client || !IsIdle())
	{
		return FReply::Handled();
	}
	const FText Number = CrowdyComputeUI::VersionNumber(Version);
	if (!CrowdyComputeUI::Confirm(FText::Format(LOCTEXT("ConfirmActivate", "Makes version {0} the active server code of {1}."), Number, Compute.GetAppTarget()),
		FText::Format(LOCTEXT("ConfirmActivateTitle", "Make version {0} active"), Number)))
	{
		return FReply::Handled();
	}
	if (!Compute.TryBeginAction())
	{
		return FReply::Handled();
	}
	Client->ActivateVersion(Version, MakeActionAnswer(Compute, CrowdyComputeUI::ErrorActivate, FText::Format(LOCTEXT("ActivateFailed", "Could not make version {0} active"), Number)));
	return FReply::Handled();
}

FReply SCrowdyServerComputePanel::OnOpenDefinitionClicked(FString AssetPath)
{
	FSlateApplication::Get().DismissAllMenus();
	UObject* Asset = FSoftObjectPath(AssetPath).TryLoad();
	if (!Asset || !GEditor)
	{
		return FReply::Handled();
	}
	GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Asset);
	return FReply::Handled();
}

FReply SCrowdyServerComputePanel::OnRemoveTypeClicked(FString TypeName, FString AssetPath)
{
	FSlateApplication::Get().DismissAllMenus();
	if (!IsIdle())
	{
		return FReply::Handled();
	}
	// The row may be older than the project: its asset may have moved or had its Type Name changed since.
	FCrowdyServerComputeService& Compute = FCrowdyServerComputeService::Get();
	Compute.RefreshTypes();
	const FText Type = FText::FromString(TypeName);
	const FText Asset = FText::FromString(FSoftObjectPath(AssetPath).GetLongPackageName());
	const TWeakObjectPtr<UCrowdyServerObjectDefinition> Definition = Cast<UCrowdyServerObjectDefinition>(FSoftObjectPath(AssetPath).TryLoad());
	if (!Definition.IsValid() || !Definition->TypeName.Equals(TypeName, ESearchCase::CaseSensitive))
	{
		SetError(CrowdyComputeUI::ErrorRemove, FText::Format(LOCTEXT("RemoveStale", "{1} is no longer the definition of {0}, so nothing was removed. The list is read again; try once more from it."), Type, Asset));
		return FReply::Handled();
	}

	const FString ProjectDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString ServerDirectory = CrowdyExecCodegen::GetServerDirectory();
	const FString CrateDirectory = Compute.GetCrateDirectory(TypeName);
	const bool bShared = Compute.IsSharedTypeName(TypeName);
	const FText FolderStays = CrowdyServerComputePage::WhyCrateFolderStays(TypeName, CrateDirectory, ServerDirectory, bShared);
	FString ShownFolder = CrateDirectory;
	FPaths::MakePathRelativeTo(ShownFolder, *ProjectDirectory);

	TArray<FText> Lines;
	Lines.Add(FText::Format(LOCTEXT("ConfirmRemove", "Remove {0} from this project?"), Type));
	Lines.Add(FolderStays.IsEmpty()
		? FText::Format(LOCTEXT("ConfirmRemoveBoth", "This deletes its definition {0} and its Server Code folder {1}."), Asset, FText::FromString(ShownFolder))
		: FText::Format(LOCTEXT("ConfirmRemoveAsset", "This deletes its definition {0} only. {1}"), Asset, FolderStays));
	TArray<FString> LostEdits;
	for (const FString& File : CrowdyServerCodeEdits::UnsavedFiles())
	{
		if (!FolderStays.IsEmpty() || !FPaths::IsUnderDirectory(File, CrateDirectory))
		{
			continue;
		}
		FString Shown = File;
		FPaths::MakePathRelativeTo(Shown, *ProjectDirectory);
		LostEdits.Add(Shown);
	}
	if (!LostEdits.IsEmpty())
	{
		Lines.Add(FText::Format(LOCTEXT("ConfirmRemoveUnsaved", "Its unsaved code edits are lost:\n{0}"), FText::FromString(FString::Join(LostEdits, TEXT("\n")))));
	}
	Lines.Add(CrowdyComputeUI::RemoveServerEffect(Type, Compute.GetAppTarget(), Compute.FindChange(TypeName), Compute.GetProject().Types.Num() <= 1));
	if (!CrowdyComputeUI::Confirm(FText::Join(FText::FromString(TEXT("\n\n")), Lines), FText::Format(LOCTEXT("ConfirmRemoveTitle", "Remove {0}"), Type)))
	{
		return FReply::Handled();
	}

	if (!Definition.IsValid())
	{
		SetError(CrowdyComputeUI::ErrorRemove, FText::Format(LOCTEXT("RemoveGone", "{0}'s definition {1} is no longer loaded, so nothing was removed."), Type, Asset));
		return FReply::Handled();
	}
	// The editor's own delete checks what still references the definition and asks again; nothing more happens unless it deleted it.
	const TArray<UObject*> ToDelete = { Definition.Get() };
	if (ObjectTools::DeleteObjects(ToDelete, true) == 0)
	{
		return FReply::Handled();
	}
	const FText FolderError = FolderStays.IsEmpty() ? CrowdyComputeUI::DeleteCrateFolder(Type, TypeName, CrateDirectory, ServerDirectory, bShared, ShownFolder) : FText::GetEmpty();
	SetError(CrowdyComputeUI::ErrorRemove, FolderError);
	// Closing the definition's editor kept its unsaved code edits; with their files gone they would only haunt later prompts.
	if (FolderStays.IsEmpty() && FolderError.IsEmpty())
	{
		CrowdyServerCodeEdits::ForgetUnder(CrateDirectory);
	}
	Compute.RefreshTypes();
	return FReply::Handled();
}

FReply SCrowdyServerComputePanel::OnShowCallClicked(FString Flow)
{
	LogFlow = Flow;
	RefreshLogs(false);
	return FReply::Handled();
}

FReply SCrowdyServerComputePanel::OnClearCallClicked()
{
	LogFlow.Reset();
	RefreshLogs(false);
	return FReply::Handled();
}

FReply SCrowdyServerComputePanel::OnLoadOlderClicked()
{
	RefreshLogs(true);
	return FReply::Handled();
}

FReply SCrowdyServerComputePanel::OnToggleBuildOutput()
{
	bBuildOutputOpen = !bBuildOutputOpen;
	return FReply::Handled();
}

FText SCrowdyServerComputePanel::GetDeployLine() const
{
	return FCrowdyServerComputeService::Get().GetDeployLine();
}

FSlateColor SCrowdyServerComputePanel::GetDeployLineColor() const
{
	return FSlateColor(FCrowdyServerComputeService::Get().GetDeployLineColor());
}

bool SCrowdyServerComputePanel::IsIdle() const
{
	return !FCrowdyServerComputeService::Get().IsBusy();
}

bool SCrowdyServerComputePanel::CanDeploy() const
{
	return IsIdle() && FCrowdyServerComputeService::Get().CanDeploy();
}

bool SCrowdyServerComputePanel::CanSwitch() const
{
	return IsIdle() && FCrowdyServerComputeService::Get().GetStatus().IsSet();
}

#undef LOCTEXT_NAMESPACE
