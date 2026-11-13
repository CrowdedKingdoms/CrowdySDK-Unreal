#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "Subsystem/CrowdyModScriptTypes.h"
#include "Subsystem/CrowdyVoxelWorld.h"
#include "CrowdyModScriptHost.generated.h"

UINTERFACE(BlueprintType, Blueprintable)
class CROWDYSERVICES_API UCrowdyModScriptHost : public UInterface
{
	GENERATED_BODY()
};

/**
 * The game's answers to the host calls only it can give, set on UCrowdyModScripts with SetHost. Every method runs on
 * the game thread with values the SDK has already checked against the grid, the player's agreement and the call's
 * limits, and must answer from state the game already holds, without waiting on the network.
 */
class CROWDYSERVICES_API ICrowdyModScriptHost
{
	GENERATED_BODY()

public:
	/** pointer_clicks: the clicks since the last drain. */
	UFUNCTION(BlueprintNativeEvent, Category="Crowdy SDK|Mod Scripts")
	void DrainClicks(FCrowdyModScriptClicks& Clicks);

	/** chunk_get and voxels_list: false when the chunk is not loaded. */
	UFUNCTION(BlueprintNativeEvent, Category="Crowdy SDK|Mod Scripts")
	bool ReadChunk(int64 ChunkX, int64 ChunkY, int64 ChunkZ, FCrowdyModScriptChunk& Chunk);

	/** actors_list and actors_list_radius: the players in the box, the local one included, at most 64. */
	UFUNCTION(BlueprintNativeEvent, Category="Crowdy SDK|Mod Scripts")
	void ListPlayers(const FCrowdyModScriptGrid& Box, TArray<FCrowdyModScriptPlayer>& Players);

	/** voxel_set: true once submitted to the voxel world; false with a reason only for a failure known at once. */
	UFUNCTION(BlueprintNativeEvent, Category="Crowdy SDK|Mod Scripts")
	bool SetVoxel(const FCrowdyModScript& Script, const FCrowdyVoxelWrite& Write, FString& Refusal);

	/** hud_set: the script's HUD text; empty removes its card. */
	UFUNCTION(BlueprintNativeEvent, Category="Crowdy SDK|Mod Scripts")
	void ShowHud(const FCrowdyModScript& Script, const FString& Text);

	/** overlay_draw: replaces the script's shapes; an empty array clears them. */
	UFUNCTION(BlueprintNativeEvent, Category="Crowdy SDK|Mod Scripts")
	void DrawOverlay(const FCrowdyModScript& Script, const TArray<FCrowdyModScriptShape>& Shapes);

	/** The script stopped: remove its HUD card and its shapes. No call for that run follows. */
	UFUNCTION(BlueprintNativeEvent, Category="Crowdy SDK|Mod Scripts")
	void ClearScript(const FCrowdyModScript& Script);
};
