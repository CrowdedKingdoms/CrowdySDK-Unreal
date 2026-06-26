// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Data/CrowdyMapProfile.h"
#include "Engine/DeveloperSettings.h"
#include "Core/UDP/Enums/ECrowdyUDPProtocol.h"
#include "CrowdySDKDeveloperSettings.generated.h"


USTRUCT()
struct FCrowdyIDOverride
{
	GENERATED_BODY()
	
	UPROPERTY(EditAnywhere, meta=(DisplayName="Struct"))
	TSoftObjectPtr<UScriptStruct> Struct;
	
	UPROPERTY(EditAnywhere, meta=(DisplayName="Override ID", ClampMin=0, ClampMax=65535))
	int32 OverrideID = 0;
};

USTRUCT()
struct FCrowdyClassIDOverride
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, meta=(DisplayName="Entity Class"))
	TSoftClassPtr<AActor> Class;

	UPROPERTY(EditAnywhere, meta=(DisplayName="Override ID", ClampMin=1))
	uint32 OverrideID = 0;
};

class UCrowdyBakedRegistry;

UENUM()
enum class ECrowdyEnvironment : uint8
{
	Dev    UMETA(DisplayName = "Dev (shared)"),
	Prod   UMETA(DisplayName = "Production"),
	Custom UMETA(DisplayName = "Custom (set Management API URL)")
};

/**
 *
 */
UCLASS(Config=Game, DefaultConfig, meta=(DisplayName="Crowdy SDK"))
class CROWDYREPLICATION_API UCrowdySDKDeveloperSettings : public UDeveloperSettings
{
	GENERATED_BODY()
	
public:
	
	virtual FName GetCategoryName() const override { return "Plugins"; }
	virtual FName GetSectionName() const override { return "Crowdy SDK"; }

	// The network fields below are managed by the CrowdyStudio console (Project page), which is the
	// single source of truth. They are shown read-only so you can see what the game will use; set
	// them from the console, not here.

	/** Which Crowdy backend the game talks to. Dev/Production use built-in management hosts; Custom
	 *  uses the Management API URL below. Choose it from the console's Backend selector. */
	UPROPERTY(Config, VisibleAnywhere, Category="Crowdy SDK|Developer|Network", meta=(DisplayName="Backend (managed by CrowdyStudio)"))
	ECrowdyEnvironment Environment = ECrowdyEnvironment::Prod;

	/** The management base URL used when Backend is Custom (no /graphql suffix). */
	UPROPERTY(Config, VisibleAnywhere, Category="Crowdy SDK|Developer|Network",
		meta=(DisplayName="Management API URL (Custom)"))
	FString ManagementApiUrl = TEXT("https://api.dev.crowdedkingdoms.com");

	/** The effective management API URL: built-in host for Dev/Prod, ManagementApiUrl for
	 *  Custom. Read this rather than ManagementApiUrl directly. */
	FString GetManagementApiUrl() const;

	/** Active app id. The Game API scopes each realtime session to one app, so this must match a
	 *  real app. Set it in the console by picking an app on the Project page. */
	UPROPERTY(Config, VisibleAnywhere, Category="Crowdy SDK|Developer|Network")
	int64 AppID = 1;

	/** Owning organization id. Set in the console with the app. */
	UPROPERTY(Config, VisibleAnywhere, Category="Crowdy SDK|Developer|Network")
	int32 OrgId = 1;

	/** Game API HTTP endpoint, fetched from the server: the console reads the app's gameApiUrl,
	 *  adds the /graphql path, and writes it here. Empty until you pick and sync an app. Read it
	 *  via GetGameApiHttpUrl(). */
	UPROPERTY(Config, VisibleAnywhere, Category="Crowdy SDK|Developer|Network")
	FString GameApiHttpUrl;

	/** Game API WebSocket endpoint. The server exposes only the HTTP gameApiUrl, so the console
	 *  writes this as that same URL (with /graphql) and a ws scheme. Read it via GetGameApiWsUrl(). */
	UPROPERTY(Config, VisibleAnywhere, Category="Crowdy SDK|Developer|Network")
	FString GameApiWsUrl;

	/** The game endpoints exactly as the console fetched them from the server. No derivation. */
	FString GetGameApiHttpUrl() const;
	FString GetGameApiWsUrl() const;

	/** Which IP protocol stack to use when connecting the UDP socket. Auto tries IPv6 first and
	 *  falls back to IPv4 if that fails. Set it from the CrowdyStudio console (Project page,
	 *  Connection section); shown read-only here. */
	UPROPERTY(Config, VisibleAnywhere, Category="Crowdy SDK|Developer|Network",
		meta=(DisplayName="UDP Protocol (managed by CrowdyStudio)"))
	ECrowdyUDPProtocol UDPProtocol = ECrowdyUDPProtocol::Auto;

	/** Seconds of silence before the SDK considers the UDP connection dead and automatically
	 *  reconnects. Must be greater than the ping interval (5 s). Managed by the CrowdyStudio
	 *  console (Project page); shown read-only here. */
	UPROPERTY(Config, VisibleAnywhere, Category="Crowdy SDK|Developer|Network",
		meta=(DisplayName="UDP Timeout (seconds)", ClampMin="6.0", ClampMax="120.0"))
	float UDPTimeoutSeconds = 15.0f;

	/** How often (in seconds) the SDK polls the server for the current game host. Polling begins
	 *  automatically after a successful login. Managed by the CrowdyStudio console (Project page);
	 *  shown read-only here. */
	UPROPERTY(Config, VisibleAnywhere, Category="Crowdy SDK|Developer|Network",
		meta=(DisplayName="Host Poll Interval (seconds)", ClampMin="1.0", ClampMax="60.0"))
	float HostPollIntervalSeconds = 5.0f;

	/**
	 * If true, UCrowdyPersistenceSubsystem will zero-out every voxel slot it
	 * wrote during this session when the subsystem deinitializes.
	 *
	 * Requires UDP to still be connected at shutdown time.
	 * For guaranteed delivery, call ClearAllState() manually before logout
	 * rather than relying on this option alone.
	 */
	UPROPERTY(Config, EditAnywhere, Category="Crowdy SDK|Developer|Persistence",
		meta=(DisplayName="Auto-Clear Persistent State on Shutdown"))
	bool bAutoClearPersistentStateOnShutdown = false;

	/**
	 * Cooked snapshot of the editor-only Crowdy metadata (event handlers,
	 * listeners, persistent structs). Generated by 'Rebuild Crowdy Registry' in
	 * the editor and required for these features to work in a packaged build —
	 * metadata itself is stripped from cooked builds. Ignored in editor/PIE,
	 * which read live metadata.
	 */
	UPROPERTY(Config, EditAnywhere, Category="Crowdy SDK|Developer|Baked Registry",
		meta=(DisplayName="Baked Metadata Registry"))
	TSoftObjectPtr<UCrowdyBakedRegistry> BakedRegistry;

	/** Per-map SDK configuration. Maps without an entry fall back to DefaultProfile. */
	UPROPERTY(Config, EditAnywhere, Category="Crowdy SDK|Developer|Map Profiles")
	TMap<TSoftObjectPtr<UWorld>, TSoftObjectPtr<UCrowdyMapProfile>> MapProfiles;

	/** Used when the current map has no MapProfiles entry. Leave unset to disable the SDK on unlisted maps. */
	UPROPERTY(Config, EditAnywhere, Category="Crowdy SDK|Developer|Map Profiles")
	TSoftObjectPtr<UCrowdyMapProfile> DefaultProfile;

	/**
	 * Resolves the profile configured for the world's map (PIE prefixes
	 * stripped), falling back to DefaultProfile. Returns null when the map has
	 * no profile — the SDK stays inactive on that map.
	 */
	static const UCrowdyMapProfile* ResolveProfileForWorld(const UWorld* World);

	// Only populate this array if the output log reports a hash collision
	// during startup. In practice this will be empty for almost every project.
	UPROPERTY(Config, EditAnywhere, Category="Crowdy SDK|Type ID Overrides",
		meta=(DisplayName="ID Collision Overrides",
			  ToolTip="Leave empty unless the log reports: [CrowdyAutoRegistry] Hash collision"))
	TArray<FCrowdyIDOverride> IDOverrides;

	UPROPERTY(Config, EditAnywhere, Category="Crowdy SDK|Type ID Overrides",
		meta=(DisplayName="Class ID Collision Overrides",
			  ToolTip="Leave empty unless the log reports: [CrowdyClassRegistry] ClassID collision"))
	TArray<FCrowdyClassIDOverride> ClassIDOverrides;
};
