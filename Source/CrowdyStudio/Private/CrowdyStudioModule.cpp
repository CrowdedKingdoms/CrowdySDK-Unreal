// Fill out your copyright notice in the Description page of Project Settings.

#include "CrowdyStudioModule.h"

#include "Auth/FCrowdyTokenVault.h"
#include "CrowdyLog.h"

#if WITH_EDITOR
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Style/CrowdyStudioStyle.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "ToolMenus.h"
#include "UI/SCrowdyStudioWindow.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#endif

#define LOCTEXT_NAMESPACE "CrowdyStudio"

IMPLEMENT_MODULE(FCrowdyStudioModule, CrowdyStudio)

DEFINE_LOG_CATEGORY(LogCrowdyStudio)

namespace
{
	CROWDY_DEFINE_TRACE_CVAR(CVarCrowdyStudioTrace, TEXT("crowdy.studio.trace"),
		TEXT("When non-zero, logs each CrowdyStudio console GraphQL op: its plane, operation name, ")
		TEXT("and outcome (HTTP code, error count). The bearer token is never logged. Off by default."));
}

bool CrowdyStudioTrace::Enabled() { return CVarCrowdyStudioTrace.GetValueOnAnyThread() != 0; }

FString CrowdyStudioAuth::GetSignedInToken()
{
	FString Token;
	return FCrowdyTokenVault::Load(Token) ? Token : FString();
}

namespace
{
	// Set by CrowdySDKEditor at startup; invoked by the console's Registry page Rebuild button.
	// Takes an OnComplete callback because the rebuild streams assets asynchronously.
	TFunction<void(TFunction<void()>)> GRegistryRebuildHook;
}

void CrowdyStudioRegistry::SetRebuildHook(TFunction<void(TFunction<void()>)> Hook) { GRegistryRebuildHook = MoveTemp(Hook); }
bool CrowdyStudioRegistry::HasRebuildHook() { return static_cast<bool>(GRegistryRebuildHook); }
void CrowdyStudioRegistry::RequestRebuild(TFunction<void()> OnComplete)
{
	if (GRegistryRebuildHook)
	{
		GRegistryRebuildHook(MoveTemp(OnComplete));
	}
	else
	{
		UE_LOG(LogCrowdyStudio, Warning,
			TEXT("Registry rebuild requested but no rebuild hook is set (CrowdySDKEditor not loaded?)."));
		// Still fire OnComplete so the caller's view refresh isn't stranded.
		if (OnComplete) OnComplete();
	}
}

#if WITH_EDITOR
namespace
{
	const FName StudioTabId(TEXT("CrowdyStudio"));
}
#endif

void FCrowdyStudioModule::StartupModule()
{
#if WITH_EDITOR
	FCrowdyStudioStyle::Initialize();
	RegisterTabSpawner();
	RegisterMenus();
#endif
}

void FCrowdyStudioModule::ShutdownModule()
{
#if WITH_EDITOR
	UToolMenus::UnRegisterStartupCallback(this);

	if (UToolMenus* ToolMenus = UToolMenus::TryGet())
	{
		ToolMenus->UnregisterOwner(this);
	}

	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(StudioTabId);
	}

	FCrowdyStudioStyle::Shutdown();
#endif
}

#if WITH_EDITOR
void FCrowdyStudioModule::RegisterTabSpawner()
{
	FGlobalTabmanager::Get()
		->RegisterNomadTabSpawner(StudioTabId, FOnSpawnTab::CreateRaw(this, &FCrowdyStudioModule::SpawnStudioTab))
		.SetDisplayName(LOCTEXT("StudioTabTitle", "Crowdy Studio"))
		.SetTooltipText(LOCTEXT("StudioTabTooltip", "Crowded Kingdoms management console."))
		.SetMenuType(ETabSpawnerMenuType::Hidden);
}

void FCrowdyStudioModule::RegisterMenus()
{
	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FCrowdyStudioModule::ExtendEditorMenus));
}

void FCrowdyStudioModule::OpenStudioTab()
{
	FGlobalTabmanager::Get()->TryInvokeTab(FTabId(StudioTabId));
}

void FCrowdyStudioModule::ExtendEditorMenus()
{
	ExtendToolsMenu();
	ExtendLevelEditorToolbar();
}

void FCrowdyStudioModule::ExtendToolsMenu()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools"));
	if (!Menu)
	{
		return;
	}

	FToolMenuSection& Section =
		Menu->FindOrAddSection(TEXT("CrowdySDK"), LOCTEXT("CrowdySDKSection", "Crowdy SDK"));

	Section.AddMenuEntry(
		TEXT("OpenCrowdyStudio"),
		LOCTEXT("StudioMenuLabel", "Crowdy Studio"),
		LOCTEXT("StudioMenuTip", "Open the Crowded Kingdoms management console."),
		FSlateIcon(FCrowdyStudioStyle::StyleName(), TEXT("Crowdy.Icon.ck")),
		FUIAction(FExecuteAction::CreateRaw(this, &FCrowdyStudioModule::OpenStudioTab)));
}

void FCrowdyStudioModule::ExtendLevelEditorToolbar()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	// The play toolbar's "Play" section; appending here lands the button just right of the Play
	// controls, the same spot the mod.io plugin uses for its toolbar entry.
	UToolMenu* Toolbar = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.LevelEditorToolBar.PlayToolBar"));
	if (!Toolbar)
	{
		return;
	}

	FToolMenuSection& Section = Toolbar->FindOrAddSection(TEXT("Play"));

	// A full two-tone "CROWDED KINGDOMS" wordmark button, matching the console header logo exactly:
	// white "CROWDED" + Warm Gold "KINGDOMS", bold. A custom widget entry is used because an ordinary
	// toolbar button label is a single colour and cannot render the two-tone wordmark.
	const FSlateFontInfo BrandFont = FCoreStyle::GetDefaultFontStyle("Bold", 12);

	const TSharedRef<SWidget> BrandButton =
		SNew(SButton)
		.ButtonStyle(&FAppStyle::Get(), "SimpleButton")
		.ToolTipText(LOCTEXT("StudioToolbarTip", "Open the Crowded Kingdoms management console."))
		.ContentPadding(FMargin(10.0f, 3.0f))
		.OnClicked(FOnClicked::CreateLambda([this]() { OpenStudioTab(); return FReply::Handled(); }))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("BrandTb1", "CROWDED"))
				.Font(BrandFont)
				.ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextPrimary()))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(5.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("BrandTb2", "KINGDOMS"))
				.Font(BrandFont)
				.ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::Gold()))
			]
		];

	Section.AddEntry(FToolMenuEntry::InitWidget(
		TEXT("OpenCrowdyStudio"),
		BrandButton,
		LOCTEXT("StudioToolbarLabel", "Crowded Kingdoms"),
		/*bNoIndent*/ true));
}

TSharedRef<SDockTab> FCrowdyStudioModule::SpawnStudioTab(const FSpawnTabArgs& /*Args*/)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SCrowdyStudioWindow)
		];
}
#endif

#undef LOCTEXT_NAMESPACE
