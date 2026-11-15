#pragma once

#include "CoreMinimal.h"
#include "Subsystem/CrowdyModScriptHost.h"
#include "Subsystem/CrowdyModScripts.h"
#include "UObject/Object.h"
#include "CrowdyModScriptsTestTypes.generated.h"

/** Records what Mod Scripts broadcast and which scripts the host was told to clear, for its automation tests. */
UCLASS(Transient, Hidden, meta=(CrowdyTestFixture))
class UCrowdyModScriptsTestRecorder : public UObject, public ICrowdyModScriptHost
{
	GENERATED_BODY()

public:
	TArray<FCrowdyModScriptPrompt> Prompts;
	TArray<int32> Withdrawn;
	TArray<FCrowdyModScript> Started;
	TArray<TPair<FString, ECrowdyModScriptStop>> Stopped;
	TArray<FCrowdyModScriptError> Errors;
	TArray<FString> Cleared;
	TArray<FString> Hud;
	TArray<FString> HudFrom;
	TArray<TArray<FCrowdyModScriptShape>> Overlays;
	TArray<FCrowdyModScriptGrid> PlayerAreas;
	TArray<FCrowdyVoxelWrite> Writes;
	int32 ChunkReads = 0;
	int32 ClickDrains = 0;

	UFUNCTION()
	void HandlePrompt(const FCrowdyModScriptPrompt& Prompt) { Prompts.Add(Prompt); }

	UFUNCTION()
	void HandleWithdrawn(int32 PromptId) { Withdrawn.Add(PromptId); }

	UFUNCTION()
	void HandleStarted(const FCrowdyModScript& Script) { Started.Add(Script); }

	UFUNCTION()
	void HandleStopped(const FCrowdyModScript& Script, ECrowdyModScriptStop Reason) { Stopped.Emplace(Script.ModId, Reason); }

	UFUNCTION()
	void HandleError(const FCrowdyModScriptError& Error) { Errors.Add(Error); }

	bool WasStopped(const FString& ModId, ECrowdyModScriptStop Reason) const
	{
		return Stopped.ContainsByPredicate([&ModId, Reason](const TPair<FString, ECrowdyModScriptStop>& Entry)
		{
			return Entry.Key.Equals(ModId, ESearchCase::CaseSensitive) && Entry.Value == Reason;
		});
	}

	virtual void DrainClicks_Implementation(FCrowdyModScriptClicks& Clicks) override
	{
		++ClickDrains;
		Clicks.Buttons = 1;
		Clicks.Clicks.AddDefaulted_GetRef().bDown = true;
	}

	/** Every chunk reads as loaded, its first cell of type 3. */
	virtual bool ReadChunk_Implementation(int64 ChunkX, int64 ChunkY, int64 ChunkZ, FCrowdyModScriptChunk& Chunk) override
	{
		++ChunkReads;
		Chunk.Types.Init(0, 4096);
		Chunk.Types[0] = 3;
		return true;
	}

	virtual void ListPlayers_Implementation(const FCrowdyModScriptGrid& Box, TArray<FCrowdyModScriptPlayer>& Players) override
	{
		PlayerAreas.Add(Box);
		FCrowdyModScriptPlayer& Self = Players.AddDefaulted_GetRef();
		Self.ActorId = TEXT("ab");
		Self.bSelf = true;
	}

	virtual bool SetVoxel_Implementation(const FCrowdyModScript& Script, const FCrowdyVoxelWrite& Write, FString& Refusal) override
	{
		Writes.Add(Write);
		return true;
	}
	virtual void ShowHud_Implementation(const FCrowdyModScript& Script, const FString& Text) override
	{
		Hud.Add(Text);
		HudFrom.Add(Script.ModId);
	}
	virtual void DrawOverlay_Implementation(const FCrowdyModScript& Script, const TArray<FCrowdyModScriptShape>& Shapes) override { Overlays.Add(Shapes); }
	virtual void ClearScript_Implementation(const FCrowdyModScript& Script) override { Cleared.Add(Script.ModId); }
};
