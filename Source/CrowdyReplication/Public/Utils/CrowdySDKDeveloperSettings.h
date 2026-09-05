// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Data/CrowdyMapProfile.h"
#include "Engine/DeveloperSettings.h"
#include "Core/UDP/Enums/ECrowdyUDPProtocol.h"
#include "Replication/GameModel/Effect/CrowdyEffectNotificationCarrier.h"
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

	/** Which Crowdy backend the game talks to. Dev/Production use built-in hosts; Custom uses the
	 *  Discovery URL below. Choose it from the console's Backend selector. */
	UPROPERTY(Config, VisibleAnywhere, Category="Crowdy SDK|Developer|Network", meta=(DisplayName="Backend (managed by CrowdyStudio)"))
	ECrowdyEnvironment Environment = ECrowdyEnvironment::Prod;

	/** The shared origin used when Backend is Custom (no /graphql suffix). One name that every
	 *  datacenter answers, so it is where a client asks which datacenter its app lives in and
	 *  where it signs in. Not a gameplay endpoint: that is GameApiHttpUrl, which names one
	 *  instance and is exactly the thing that can go away. */
	UPROPERTY(Config, VisibleAnywhere, Category="Crowdy SDK|Developer|Network",
		meta=(DisplayName="Discovery URL (Custom)"))
	FString DiscoveryUrl = TEXT("https://api.dev.crowdedkingdoms.com");

	/** The effective shared origin: built-in host for Dev/Prod, DiscoveryUrl for Custom. Read this
	 *  rather than DiscoveryUrl directly. */
	FString GetDiscoveryUrl() const;

	/** Active app id. The Game API scopes each realtime session to one app, so this must match a
	 *  real app. Set it in the console by picking an app on the Project page. */
	UPROPERTY(Config, VisibleAnywhere, Category="Crowdy SDK|Developer|Network")
	int64 AppID = 1;

	/** Owning organization id. Set in the console with the app. Like the app id this is the server's
	 *  BigInt scalar, so it needs the full 64-bit range. */
	UPROPERTY(Config, VisibleAnywhere, Category="Crowdy SDK|Developer|Network")
	int64 OrgId = 1;

	/** Game API HTTP endpoint, fetched from the server: the console reads the app's gameApiUrl,
	 *  adds the /graphql path, and writes it here. Empty until you pick and sync an app, in which
	 *  case the runtime resolves it from the shared origin instead. It names ONE datacenter's
	 *  instance, so it can stop answering; the runtime re-resolves rather than retrying it.
	 *  Read it via GetGameApiHttpUrl(). */
	UPROPERTY(Config, VisibleAnywhere, Category="Crowdy SDK|Developer|Network")
	FString GameApiHttpUrl;

	/** Game API WebSocket endpoint. The server exposes only the HTTP gameApiUrl, so the console
	 *  writes this as that same URL (with /graphql) and a ws scheme. Read it via GetGameApiWsUrl(). */
	UPROPERTY(Config, VisibleAnywhere, Category="Crowdy SDK|Developer|Network")
	FString GameApiWsUrl;

	/** The game endpoints exactly as the console fetched them from the server. No derivation. */
	FString GetGameApiHttpUrl() const;
	FString GetGameApiWsUrl() const;

	/** Which IP protocol stack the connection uses. Auto takes the IPv4 address; only IPv6 changes
	 *  the choice, and the connection does not fall back to the other family. Set it from the
	 *  CrowdyStudio console (Project page, Connection section); shown read-only here. */
	UPROPERTY(Config, VisibleAnywhere, Category="Crowdy SDK|Developer|Network",
		meta=(DisplayName="UDP Protocol (managed by CrowdyStudio)"))
	ECrowdyUDPProtocol UDPProtocol = ECrowdyUDPProtocol::Auto;

	/** Seconds of silence, after traffic has been flowing, before the connection re-assigns a server
	 *  and carries on. The game is only told if the re-assignment itself fails. Must be greater than
	 *  the ping interval (5 s). Managed by the CrowdyStudio console (Project page); shown read-only
	 *  here. */
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
	 * the editor and required for these features to work in a packaged build,
	 * since metadata itself is stripped from cooked builds. Ignored in editor/PIE,
	 * which read live metadata.
	 */
	UPROPERTY(Config, EditAnywhere, Category="Crowdy SDK|Developer|Baked Registry",
		meta=(DisplayName="Baked Metadata Registry"))
	TSoftObjectPtr<UCrowdyBakedRegistry> BakedRegistry;

	/** The realtime carrier an authored Game Model effect uses for its "model changed" notification when the
	 *  effect itself is left on Default. Channel (the default) reaches every member of the app's default session
	 *  channel regardless of position - correct for turn-based and session-scoped games; Spatial fans out by
	 *  proximity for location-bound changes; None emits no notification. An individual effect can override this. */
	UPROPERTY(Config, EditAnywhere, Category="Crowdy SDK|Developer|Game Model",
		meta=(DisplayName="Default Model Notification Carrier"))
	ECrowdyEffectNotificationCarrier DefaultModelNotificationCarrier = ECrowdyEffectNotificationCarrier::Channel;

	/** Per-map SDK configuration. Maps without an entry fall back to DefaultProfile. */
	UPROPERTY(Config, EditAnywhere, Category="Crowdy SDK|Developer|Map Profiles")
	TMap<TSoftObjectPtr<UWorld>, TSoftObjectPtr<UCrowdyMapProfile>> MapProfiles;

	/**
	 * Used when the current map has no MapProfiles entry. Leave unset to fall back to the profile shipped by an
	 * installed Crowdy plugin, if one is installed, and to disable the SDK on unlisted maps if none is.
	 */
	UPROPERTY(Config, EditAnywhere, Category="Crowdy SDK|Developer|Map Profiles")
	TSoftObjectPtr<UCrowdyMapProfile> DefaultProfile;

	/**
	 * Resolves the profile configured for the world's map (PIE prefixes
	 * stripped), falling back to DefaultProfile and then to the profile a plugin
	 * ships. Returns null when the map has no profile, in which case the SDK
	 * stays inactive on that map.
	 *
	 * Every Crowdy subsystem resolves through here, so this is also where a Game
	 * or PIE world that resolves nothing is warned about, once per world. Callers
	 * therefore do not need to report a null of their own.
	 */
	static const UCrowdyMapProfile* ResolveProfileForWorld(const UWorld* World);

	/**
	 * Offers a map profile that ships inside a plugin, used on maps that configure nothing of their own so a
	 * project can render remote entities without authoring a profile first.
	 *
	 * Called from a plugin's module startup. ProviderName is that plugin's name and is reported in the log, so a
	 * project can see which installed plugin its maps are running on.
	 *
	 * Only one registration is in effect at a time. A second plugin offering a different profile is refused and
	 * reported, because otherwise which of the two a map ran on would depend on module load order. Re-registering
	 * under the same provider name replaces that provider's own entry.
	 */
	static void RegisterShippedDefaultProfile(const FString& ProviderName, const TSoftObjectPtr<UCrowdyMapProfile>& Profile);

	/** Withdraws ProviderName's offer. Does nothing if another provider's offer is the one in effect. */
	static void UnregisterShippedDefaultProfile(const FString& ProviderName);

	/** The asset path currently offered, or an empty path when no plugin offers one. */
	static FSoftObjectPath GetShippedDefaultProfilePath();

	/** The plugin whose offer is in effect, or an empty string when no plugin offers one. */
	static FString GetShippedDefaultProfileProvider();

	/**
	 * The shipped profile, or null with OutUnavailableReason set to a sentence explaining why there is none.
	 *
	 * OutUnavailableReason is left EMPTY when no plugin offers a profile at all, which is not a fault: a project
	 * that installs no plugin shipping one is simply not using this route. It is filled in only when an offer
	 * exists and could not be honoured, which is a fault worth naming.
	 */
	static const UCrowdyMapProfile* ResolveShippedDefaultProfile(FString& OutUnavailableReason);

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

	/**
	 * Entity classes to load before the startup scan, so this client can resolve them off the wire.
	 *
	 * An actor update names its class by id and has no room for a class path, so a receiver can only
	 * resolve an id it already holds. The startup scan walks loaded classes, and a Blueprint entity
	 * class nobody has spawned yet is not loaded, so an observer that meets one first over the network
	 * cannot name it and the entity stays invisible with nothing saying why.
	 *
	 * List the entity classes a client can be shown before it spawns one itself. Classes already loaded
	 * for any other reason need no entry, and a duplicate entry costs nothing.
	 */
	UPROPERTY(Config, EditAnywhere, Category="Crowdy SDK|Replication",
		meta=(DisplayName="Preloaded Entity Classes",
			  ToolTip="Entity classes this client should load at startup so it can resolve them from an actor update, which carries a class id and no class path."))
	TArray<TSoftClassPtr<AActor>> PreloadedEntityClasses;
};
