#pragma once

#include "CoreMinimal.h"
#include "CrowdyActorManagementConfig.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Engine/DataAsset.h"
#include "CrowdyMapProfile.generated.h"

/**
 * Per-map configuration for the Crowdy SDK. One asset configures a map end to
 * end; assign it in Project Settings -> Crowdy SDK -> Map Profiles (or as the
 * Default Profile). Resolved through
 * UCrowdySDKDeveloperSettings::ResolveProfileForWorld.
 */
UCLASS(BlueprintType, Category="Crowdy SDK|Data", meta=(DisplayName="Crowdy Map Profile"))
class CROWDYREPLICATION_API UCrowdyMapProfile : public UDataAsset
{
	GENERATED_BODY()

public:

	/** Gates the host subsystem, the event router and the entity subsystem on this map. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Crowdy SDK|Map Profile")
	bool bEnableNetworking = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Crowdy SDK|Map Profile")
	FCrowdyActorManagementConfigStruct ActorManagement;

	/** Continuous state replication of Dynamic entity components on this map. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Crowdy SDK|Map Profile")
	bool bUseAutoReplicator = true;

	/** Replication rate in Hertz for this map's Dynamic-entity state (used only when Use Auto Replicator is on). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Crowdy SDK|Map Profile",
		meta=(EditCondition="bUseAutoReplicator", ClampMin=1, ClampMax=10, DisplayName="Replication Interval (Hertz)"))
	int32 ReplicationIntervalHz = 10;

	/** Master switch for the CrowdyState per-property replicator on this map. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Crowdy SDK|Map Profile")
	bool bUseStateReplicator = true;

	/** Spatial relevance radius for CrowdyState deltas (tighter than the continuous channel). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Crowdy SDK|Map Profile",
		meta=(EditCondition="bUseStateReplicator", DisplayName="State Relevance Distance"))
	ECrowdyReplicationDistance StateRelevanceDistance = ECrowdyReplicationDistance::Four_Chunks;

	/**
	 * Seconds between periodic CrowdyState keyframe heartbeats a redundant full re-send of every
	 * CrowdyHeartbeat-marked property so a late or packet-loss-desynced observer converges. Set to 0 to
	 * DISABLE the heartbeat map-wide (on-change replication is unaffected; only the periodic baseline stops).
	 * Only properties opted in with meta=(CrowdyHeartbeat) ride the heartbeat regardless of this value.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Crowdy SDK|Map Profile",
		meta=(EditCondition="bUseStateReplicator", ClampMin="0.0", DisplayName="State Keyframe Interval (Seconds, 0 = off)"))
	float StateKeyframeIntervalSeconds = 2.0f;
};
