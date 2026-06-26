// Fill out your copyright notice in the Description page of Project Settings.

#include "ConfigSync/FCrowdyConfigSync.h"

#include "CrowdyStudioModule.h"
#include "Core/CrowdySDKBridgeSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

namespace
{
	// The server exposes only the HTTP gameApiUrl, so the WS endpoint is that same URL with the
	// scheme switched to ws (https gives wss). This is exactly what the docs' client does
	// (gameApiUrl with a ws scheme); it is not host derivation.
	FString GameWsFromHttp(const FString& HttpUrl)
	{
		if (HttpUrl.StartsWith(TEXT("http")))
		{
			return TEXT("ws") + HttpUrl.RightChop(4);
		}
		return HttpUrl;
	}

	// The server's app.gameApiUrl is a bare host, but the Game API is served at /graphql, so add
	// that path when it is missing. This only appends a path to the server's host; it is not host
	// derivation (no api.->game. guessing).
	FString EnsureGraphqlPath(const FString& Url)
	{
		FString U = Url;
		U.RemoveFromEnd(TEXT("/"));
		if (!U.IsEmpty() && !U.EndsWith(TEXT("/graphql")))
		{
			U += TEXT("/graphql");
		}
		return U;
	}

	// Keep a custom management URL a bare base (no /graphql, no trailing slash). Consumers append
	// /graphql themselves, so a pasted ".../graphql" would otherwise end up doubled.
	FString NormalizeManagementBase(const FString& Url)
	{
		FString U = Url;
		U.RemoveFromEnd(TEXT("/"));
		U.RemoveFromEnd(TEXT("/graphql"));
		U.RemoveFromEnd(TEXT("/"));
		return U;
	}

	FString BackendModeFromEnum(ECrowdyEnvironment Env)
	{
		switch (Env)
		{
		case ECrowdyEnvironment::Dev:    return TEXT("Dev");
		case ECrowdyEnvironment::Custom: return TEXT("Custom");
		case ECrowdyEnvironment::Prod:
		default:                         return TEXT("Prod");
		}
	}

	ECrowdyEnvironment BackendModeToEnum(const FString& Mode)
	{
		if (Mode == TEXT("Dev"))    { return ECrowdyEnvironment::Dev; }
		if (Mode == TEXT("Custom")) { return ECrowdyEnvironment::Custom; }
		return ECrowdyEnvironment::Prod;
	}
}

void FCrowdyConfigSync::ApplyAppToSettings(const FStudioApp& App)
{
	UCrowdySDKDeveloperSettings* Settings = GetMutableDefault<UCrowdySDKDeveloperSettings>();
	if (!Settings)
	{
		UE_LOG(LogCrowdyStudio, Error, TEXT("Config Sync: developer settings unavailable."));
		return;
	}

	// Compute the result once (BuildProposedSettings) so the diff the user sees and the values we
	// actually write can never disagree.
	const FStudioSettingsSnapshot Target = BuildProposedSettings(App);

	Settings->AppID = Target.AppId;
	Settings->OrgId = Target.OrgId;

	// The app's own gameApiUrl is the source of truth for where clients connect: the shared
	// endpoint, or a linked dedicated/sandbox server. When the server gives us one, pin it (with the
	// /graphql path added, see BuildProposedSettings). With no gameApiUrl we leave these empty, so
	// the game endpoints stay unset until an app is synced.
	if (!App.GameApiUrl.IsEmpty())
	{
		Settings->GameApiHttpUrl = Target.GameApiHttpUrl;
		Settings->GameApiWsUrl = Target.GameApiWsUrl;
	}

	Settings->TryUpdateDefaultConfigFile();

	UE_LOG(LogCrowdyStudio, Log,
		TEXT("Config Sync: AppID=%lld OrgId=%d Management=%s GameHttp=%s"),
		Settings->AppID, Settings->OrgId, *Settings->GetManagementApiUrl(), *Settings->GetGameApiHttpUrl());
}

FStudioSettingsSnapshot FCrowdyConfigSync::ReadCurrentSettings()
{
	FStudioSettingsSnapshot Snapshot;

	if (const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>())
	{
		Snapshot.AppId = Settings->AppID;
		Snapshot.OrgId = Settings->OrgId;
		Snapshot.ManagementApiUrl = Settings->GetManagementApiUrl();
		Snapshot.GameApiHttpUrl = Settings->GetGameApiHttpUrl();
		Snapshot.GameApiWsUrl = Settings->GetGameApiWsUrl();
	}

	return Snapshot;
}

FStudioSettingsSnapshot FCrowdyConfigSync::BuildProposedSettings(const FStudioApp& App)
{
	// Start from what is on disk so untouched fields read identically in the diff, then apply
	// exactly what a sync changes: the app's identity, and (per the routing rule) the game
	// endpoints when the app reports its own gameApiUrl. A zero id from an incomplete record never
	// clobbers a good one. The management URL is owned by the backend selector, never by a sync.
	FStudioSettingsSnapshot Proposed = ReadCurrentSettings();

	if (App.AppId > 0)
	{
		Proposed.AppId = App.AppId;
	}
	if (App.OrgId > 0)
	{
		Proposed.OrgId = static_cast<int32>(App.OrgId);
	}

	if (!App.GameApiUrl.IsEmpty())
	{
		// Fetched from the server. The gameApiUrl is a bare host, so add the /graphql path (a path,
		// not host derivation). The WS endpoint is that same URL with a ws scheme.
		const FString Http = EnsureGraphqlPath(App.GameApiUrl);
		Proposed.GameApiHttpUrl = Http;
		Proposed.GameApiWsUrl = GameWsFromHttp(Http);
	}

	return Proposed;
}

int32 FCrowdyConfigSync::ApplyToRunningSessions()
{
	if (!GEngine)
	{
		return 0;
	}

	int32 Applied = 0;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType != EWorldType::PIE && Context.WorldType != EWorldType::Game)
		{
			continue;
		}

		const UGameInstance* GameInstance = Context.OwningGameInstance;
		if (!GameInstance)
		{
			continue;
		}

		if (UCrowdySDKBridgeSubsystem* Bridge = GameInstance->GetSubsystem<UCrowdySDKBridgeSubsystem>();
			Bridge && Bridge->ReloadConfigFn)
		{
			Bridge->ReloadConfigFn();
			++Applied;
		}
	}
	return Applied;
}

FString FCrowdyConfigSync::GetBackendMode()
{
	if (const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>())
	{
		return BackendModeFromEnum(Settings->Environment);
	}
	return TEXT("Prod");
}

void FCrowdyConfigSync::SetBackendMode(const FString& Mode)
{
	if (UCrowdySDKDeveloperSettings* Settings = GetMutableDefault<UCrowdySDKDeveloperSettings>())
	{
		Settings->Environment = BackendModeToEnum(Mode);
		// Let the game endpoints re-derive from the new backend until an app pins them again.
		Settings->GameApiHttpUrl.Reset();
		Settings->GameApiWsUrl.Reset();
		Settings->TryUpdateDefaultConfigFile();
	}
}

FString FCrowdyConfigSync::GetCustomManagementUrl()
{
	if (const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>())
	{
		return Settings->ManagementApiUrl;
	}
	return FString();
}

void FCrowdyConfigSync::SetCustomManagementUrl(const FString& Url)
{
	if (UCrowdySDKDeveloperSettings* Settings = GetMutableDefault<UCrowdySDKDeveloperSettings>())
	{
		Settings->ManagementApiUrl = NormalizeManagementBase(Url);
		// Endpoints derive from the management URL until an app pins them.
		Settings->GameApiHttpUrl.Reset();
		Settings->GameApiWsUrl.Reset();
		Settings->TryUpdateDefaultConfigFile();
	}
}

FString FCrowdyConfigSync::GetEffectiveManagementUrl()
{
	if (const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>())
	{
		return Settings->GetManagementApiUrl();
	}
	return FString();
}

ECrowdyUDPProtocol FCrowdyConfigSync::GetUdpProtocol()
{
	if (const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>())
	{
		return Settings->UDPProtocol;
	}
	return ECrowdyUDPProtocol::Auto;
}

void FCrowdyConfigSync::SetUdpProtocol(ECrowdyUDPProtocol Protocol)
{
	if (UCrowdySDKDeveloperSettings* Settings = GetMutableDefault<UCrowdySDKDeveloperSettings>())
	{
		Settings->UDPProtocol = Protocol;
		Settings->TryUpdateDefaultConfigFile();
	}
}

float FCrowdyConfigSync::GetUdpTimeoutSeconds()
{
	if (const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>())
	{
		return Settings->UDPTimeoutSeconds;
	}
	return 15.0f;
}

void FCrowdyConfigSync::SetUdpTimeoutSeconds(float Seconds)
{
	if (UCrowdySDKDeveloperSettings* Settings = GetMutableDefault<UCrowdySDKDeveloperSettings>())
	{
		Settings->UDPTimeoutSeconds = FMath::Clamp(Seconds, 6.0f, 120.0f);
		Settings->TryUpdateDefaultConfigFile();
	}
}

float FCrowdyConfigSync::GetHostPollIntervalSeconds()
{
	if (const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>())
	{
		return Settings->HostPollIntervalSeconds;
	}
	return 5.0f;
}

void FCrowdyConfigSync::SetHostPollIntervalSeconds(float Seconds)
{
	if (UCrowdySDKDeveloperSettings* Settings = GetMutableDefault<UCrowdySDKDeveloperSettings>())
	{
		Settings->HostPollIntervalSeconds = FMath::Clamp(Seconds, 1.0f, 60.0f);
		Settings->TryUpdateDefaultConfigFile();
	}
}
