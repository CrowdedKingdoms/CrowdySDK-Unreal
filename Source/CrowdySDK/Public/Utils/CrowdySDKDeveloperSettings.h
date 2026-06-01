// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Data/ActorUpdatePayloadType.h"
#include "Data/CrowdyActorManagementConfig.h"
#include "Data/CrowdyRepApplicationPolicy.h"
#include "Data/EventPayloadType.h"
#include "Data/FCrowdyObjectManagerConfig.h"
#include "Engine/DeveloperSettings.h"
#include "Replication/Subsystems/CrowdyActorPoolSubsystem.h"
#include "Data/CrowdyEntityRegistry.h"
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

class UCrowdyRepApplicationPolicy;
/**
 * 
 */
UCLASS(Config=Game, DefaultConfig, meta=(DisplayName="Crowdy SDK"))
class CROWDYSDK_API UCrowdySDKDeveloperSettings : public UDeveloperSettings
{
	GENERATED_BODY()
	
public:
	
	virtual FName GetCategoryName() const override { return "Plugins"; }
	virtual FName GetSectionName() const override { return "Crowdy SDK"; }
	
	UPROPERTY(Config, EditAnywhere, Category="Crowdy SDK|Developer|Network")
	int64 AppID = 1;
	
	UPROPERTY(Config, EditAnywhere, Category="Crowdy SDK|Developer|Network")
	int32 OrgId = 1;

	UPROPERTY(Config, EditAnywhere, Category="Crowdy SDK|Developer|Network")
	FString ManagementApiUrl = TEXT("https://api.dev.crowdedkingdoms.com");

	UPROPERTY(Config, EditAnywhere, Category="Crowdy SDK|Developer|Network")
	FString GameApiHttpUrl = TEXT("");

	UPROPERTY(Config, EditAnywhere, Category="Crowdy SDK|Developer|Network")
	FString GameApiWsUrl = TEXT("");

	/** Which IP protocol stack to use when connecting the UDP socket.
	 *  Auto tries IPv6 first and falls back to IPv4 if that fails. */
	UPROPERTY(Config, EditAnywhere, Category="Crowdy SDK|Developer|Network",
		meta=(DisplayName="UDP Protocol"))
	ECrowdyUDPProtocol UDPProtocol = ECrowdyUDPProtocol::Auto;

	/** Seconds of silence before the SDK considers the UDP connection dead and
	 *  automatically reconnects. Must be greater than the ping interval (5 s). */
	UPROPERTY(Config, EditAnywhere, Category="Crowdy SDK|Developer|Network",
		meta=(DisplayName="UDP Timeout (seconds)", ClampMin="6.0", ClampMax="120.0"))
	float UDPTimeoutSeconds = 15.0f;

	/** How often (in seconds) the SDK polls the server for the current game host.
	 *  Polling begins automatically after a successful login. */
	UPROPERTY(Config, EditAnywhere, Category="Crowdy SDK|Developer|Network",
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

	UPROPERTY(Config, EditAnywhere, Category="Crowdy SDK|Developer|Events")
	TSoftObjectPtr<UEventPayloadType> EventPayloadDataAsset;
	
	UPROPERTY(Config, EditAnywhere, Category="Crowdy SDK|Developer|Actor Updates")
	TSoftObjectPtr<UActorUpdatePayloadType> ActorUpdatePayloadDataAsset;
	
	UPROPERTY(Config, EditAnywhere, Category="Crowdy SDK|Developer|Actor Updates")
	bool bUseAutoReplicator = true;
	
	UPROPERTY(Config, EditAnywhere, Category="Crowdy SDK|Developer|Actor Updates", meta=(EditCondition="bUseAutoReplicator", ClampMin=10, ClampMax=20, DisplayName="Replication Interval (Hertz)"));
	int32 ReplicationIntervalHz = 10;
	
	UPROPERTY(Config, EditAnywhere, Category="Crowdy SDK|Developer|Actor Updates", meta=(EditCondition="bUseAutoReplicator"))
	TSet<TSoftObjectPtr<UWorld>> LevelsToUseAutoReplicator;
	
	UPROPERTY(Config, EditAnywhere, Category="Crowdy SDK|Developer|Actor Management")
	TMap<TSoftObjectPtr<UWorld>, TSoftObjectPtr<UCrowdyActorManagementConfig>> ActorManagementConfigs;
	
	UPROPERTY(Config, EditAnywhere, Category="Crowdy SDK|Developer|Entity Management")
	TMap<TSoftObjectPtr<UWorld>, TSoftObjectPtr<UCrowdyEntityRegistry>> EntityRegistryConfigs;

	UPROPERTY(Config, EditAnywhere, Category="Crowdy SDK|Developer|Host Subsystem")
	TMap<TSoftObjectPtr<UWorld>, bool> HostSubsystemAllowedMaps;
	
	UPROPERTY(Config, EditAnywhere, Category="Crowdy SDK|Developer|Event Manager")
	TMap<TSoftObjectPtr<UWorld>, bool> EventManagerAllowedMaps;
	
	// Only populate this array if the output log reports a hash collision
	// during startup. In practice this will be empty for almost every project.
	UPROPERTY(Config, EditAnywhere, Category="Crowdy SDK|Type ID Overrides",
		meta=(DisplayName="ID Collision Overrides",
			  ToolTip="Leave empty unless the log reports: [CrowdyAutoRegistry] Hash collision"))
	TArray<FCrowdyIDOverride> IDOverrides;
};
