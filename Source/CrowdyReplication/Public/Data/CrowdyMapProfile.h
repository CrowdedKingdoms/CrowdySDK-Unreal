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

	/** Off: this map ignores spawn and destroy events, takes no part in host election, and drops Crowdy Events and Crowdy State sent over the realtime connection. Movement and sending have their own switches. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Events", meta=(DisplayName="Handle Events and Spawns"))
	bool bEnableNetworking = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Remote Entities", meta=(ShowOnlyInnerProperties))
	FCrowdyActorManagementConfigStruct ActorManagement;

	/** Send each entity's movement, or its executor's state, on the continuous channel. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Movement", meta=(DisplayName="Send Movement"))
	bool bUseAutoReplicator = true;

	/** How many times a second movement is sent. Crowdy State is sent at the same rate. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Movement",
		meta=(EditCondition="bUseAutoReplicator || bUseStateReplicator", ClampMin=1, ClampMax=10, Units="Hz", DisplayName="Send Rate"))
	int32 ReplicationIntervalHz = 10;

	/** Send an unchanged actor as the keyframes and heartbeats below instead of restating it every interval. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Movement",
		meta=(EditCondition="bUseAutoReplicator", DisplayName="Send Only On Change"))
	bool bSendActorStateOnlyOnChange = true;

	/** Full re-send of an unchanged actor's state: the only way other players keep seeing an idle actor, so keep it below their Actor Timeout. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Movement",
		meta=(EditCondition="bUseAutoReplicator && bSendActorStateOnlyOnChange", ClampMin="0.0", Units="s",
			DisplayName="Keyframe Interval"))
	float ActorKeyframeIntervalSeconds = 3.0f;

	/** A cheap "still here" that keeps an unchanged actor's presence on the server, which goes stale after about 5 s; other players never receive it. 0 turns it off. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Movement",
		meta=(EditCondition="bUseAutoReplicator && bSendActorStateOnlyOnChange", ClampMin="0.0", Units="s",
			DisplayName="Heartbeat Interval"))
	float ActorHeartbeatIntervalSeconds = 1.0f;

	/** Replicate properties marked Crowdy State on this map. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Crowdy State", meta=(DisplayName="Replicate Crowdy State"))
	bool bUseStateReplicator = true;

	/** How far a Crowdy State change travels, in chunks. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Crowdy State",
		meta=(EditCondition="bUseStateReplicator", DisplayName="Relevance Distance"))
	ECrowdyReplicationDistance StateRelevanceDistance = ECrowdyReplicationDistance::Four_Chunks;

	/** Full re-send of every property marked CrowdyHeartbeat, so a late observer catches up. 0 turns it off; changes still send. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Crowdy State",
		meta=(EditCondition="bUseStateReplicator", ClampMin="0.0", Units="s", DisplayName="Keyframe Interval"))
	float StateKeyframeIntervalSeconds = 2.0f;

#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif
};
