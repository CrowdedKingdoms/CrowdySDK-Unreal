// Fill out your copyright notice in the Description page of Project Settings.

#include "CrowdyStudioModule.h"

#include "Auth/FCrowdyTokenVault.h"
#include "CrowdyCppClient.h"
#include "CrowdyLog.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameModel/CrowdyEffectPlanCache.h"
#include "Network/CrowdyCpp/CrowdyCppAdminClientHost.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

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

void CrowdyStudioAuth::FetchAppChannelNames(int64 AppId, TFunction<void(const TArray<FString>&)> OnDone)
{
	auto Fail = [OnDone]()
	{
		if (OnDone)
		{
			OnDone(TArray<FString>());
		}
	};

	if (AppId <= 0)
	{
		Fail();
		return;
	}

	// The session token can mint, but the Game API rejects it directly so mint an app token from it,
	// exactly as the console's SendGame path does, then query channels with that app token.
	FString SessionToken;
	if (!FCrowdyTokenVault::Load(SessionToken) || SessionToken.IsEmpty())
	{
		Fail();
		return;
	}

	const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>();
	if (!Settings)
	{
		Fail();
		return;
	}

	FString DiscoveryUrl = Settings->GetDiscoveryUrl();
	DiscoveryUrl.RemoveFromEnd(TEXT("/"));
	DiscoveryUrl += TEXT("/graphql");

	const FString SettingsGameUrl = Settings->GetGameApiHttpUrl();

	// Built against the shared origin: the app's own endpoint isn't known until the mint answers, and the shared
	// origin is what every datacenter responds to in the meantime. The host owns the client and its pending
	// request, so it is captured into every callback below to keep it (and the request) alive until the completion
	// it belongs to actually fires.
	FCrowdyCppClientConfig MintConfig;
	MintConfig.ApiUrl = DiscoveryUrl;
	MintConfig.DiscoveryUrl = DiscoveryUrl;

	TSharedPtr<FCrowdyCppAdminClientHost> MintHost =
		FCrowdyCppAdminClientHost::Create(MintConfig, FString());
	if (!MintHost.IsValid())
	{
		Fail();
		return;
	}
	MintHost->GetClient()->SetManagementToken(SessionToken);

	MintHost->GetClient()->MintAppToken(AppId,
		[MintHost, AppId, SettingsGameUrl, DiscoveryUrl, OnDone](FCrowdyCppAppTokenResult MintResult)
		{
			auto FailInner = [OnDone]()
			{
				if (OnDone)
				{
					OnDone(TArray<FString>());
				}
			};

			if (!MintResult.bOk || MintResult.AppToken.IsEmpty())
			{
				UE_LOG(LogCrowdyStudio, Warning,
					TEXT("Channel picker: could not mint an app token for app %lld (%s) - sign in to Crowdy Studio with a session account. Channels unavailable."),
					AppId, *MintResult.ErrorMessage);
				FailInner();
				return;
			}

			// The mint returns a bare host; the Game API GraphQL lives at /graphql. Fall back to the
			// settings-derived URL (already /graphql-terminated) when the mint omits a game endpoint.
			FString GameEndpoint = MintResult.GameApiUrl;
			if (!GameEndpoint.IsEmpty())
			{
				if (!GameEndpoint.EndsWith(TEXT("/graphql")))
				{
					GameEndpoint.RemoveFromEnd(TEXT("/"));
					GameEndpoint += TEXT("/graphql");
				}
			}
			else
			{
				GameEndpoint = SettingsGameUrl;
			}

			if (GameEndpoint.IsEmpty())
			{
				FailInner();
				return;
			}

			// The mint host talked to the shared origin; a fresh host carries the app's own datacenter endpoint
			// instead of trying to rebuild the mint host mid-completion.
			FCrowdyCppClientConfig ChannelsConfig;
			ChannelsConfig.ApiUrl = GameEndpoint;
			ChannelsConfig.DiscoveryUrl = MintResult.DiscoveryUrl.IsEmpty() ? DiscoveryUrl : MintResult.DiscoveryUrl;

			TSharedPtr<FCrowdyCppAdminClientHost> ChannelsHost =
				FCrowdyCppAdminClientHost::Create(ChannelsConfig, FString());
			if (!ChannelsHost.IsValid())
			{
				FailInner();
				return;
			}
			ChannelsHost->GetClient()->SetGameToken(MintResult.AppToken);

			const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
			Variables->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), AppId));

			ChannelsHost->GetClient()->RunOp(ECrowdyCppApiDomain::Channels, TEXT("Channels"), Variables,
				[ChannelsHost, AppId, OnDone](FCrowdyCppJsonResult ChannelsResult)
				{
					TArray<FString> Names;

					if (!ChannelsResult.bTransportOk)
					{
						UE_LOG(LogCrowdyStudio, Warning,
							TEXT("Channel picker: channels query failed for app %lld (%s)."),
							AppId, *ChannelsResult.ErrorMessage);
						if (OnDone)
						{
							OnDone(Names);
						}
						return;
					}

					// Data is already the bare `data` object (no envelope to unwrap) under the shared transport.
					const TArray<TSharedPtr<FJsonValue>>* ChannelsArray = nullptr;
					if (ChannelsResult.Data.IsValid() && ChannelsResult.Data->TryGetArrayField(TEXT("channels"), ChannelsArray))
					{
						for (const TSharedPtr<FJsonValue>& Value : *ChannelsArray)
						{
							const TSharedPtr<FJsonObject>* ChannelObj = nullptr;
							FString Name;
							if (Value->TryGetObject(ChannelObj) && (*ChannelObj)->TryGetStringField(TEXT("name"), Name) && !Name.IsEmpty())
							{
								Names.AddUnique(Name);
							}
						}
					}

					if (OnDone)
					{
						OnDone(Names);
					}
				},
				ECrowdyCppTokenPlane::Game);
		});
}

namespace
{
	// Set by CrowdySDKEditor at startup; invoked by the console's Registry page Rebuild button.
	// Takes an OnComplete callback because the rebuild streams assets asynchronously.
	TFunction<void(TFunction<void()>)> GRegistryRebuildHook;

	// Set by CrowdySDKEditor at startup; invoked by the schema sync before it gathers container classes so a
	// Blueprint marked as a container but not opened this session is resident first (and thus discoverable).
	// Asynchronous: it streams the assets and calls back, so the plan continues from the completion.
	TFunction<void(TFunction<void()>)> GLoadContainerAssetsHook;
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

void CrowdyStudioRegistry::SetLoadContainerAssetsHook(TFunction<void(TFunction<void()>)> Hook) { GLoadContainerAssetsHook = MoveTemp(Hook); }
TFunction<void(TFunction<void()>)> CrowdyStudioRegistry::GetLoadContainerAssetsHook() { return GLoadContainerAssetsHook; }
void CrowdyStudioRegistry::RequestLoadContainerAssets(TFunction<void()> OnComplete)
{
	if (GLoadContainerAssetsHook)
	{
		GLoadContainerAssetsHook(MoveTemp(OnComplete));
	}
	else
	{
		// Graceful degradation: without the editor baker's hook the sync still works for containers already
		// loaded (opened) this session; only never-opened ones are missed. Not an error, so this stays quiet.
		UE_LOG(LogCrowdyStudio, Verbose,
			TEXT("Container-asset load requested but no hook is set (CrowdySDKEditor not loaded?); syncing only loaded containers."));
		// Still fire it, so the plan behind this call is never stranded on a missing hook.
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
	// The schema plan's compile cache lives for the editor session, so it has to be emptied here or it would survive
	// a module reload holding results compiled by code that is no longer loaded.
	FCrowdyEffectPlanCache::Shutdown();

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
		.SetIcon(FSlateIcon(FCrowdyStudioStyle::StyleName(), TEXT("Crowdy.Icon.ck")))
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
