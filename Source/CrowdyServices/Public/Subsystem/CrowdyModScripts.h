#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Subsystem/CrowdyModScriptHost.h"
#include "Subsystem/CrowdyModScriptTypes.h"
#include "CrowdyModScripts.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnCrowdyModScriptPrompt, const FCrowdyModScriptPrompt&, Prompt);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnCrowdyModScriptPromptWithdrawn, int32, PromptId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnCrowdyModScriptStarted, const FCrowdyModScript&, Script);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnCrowdyModScriptStopped, const FCrowdyModScript&, Script, ECrowdyModScriptStop, Reason);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnCrowdyModScriptError, const FCrowdyModScriptError&, Error);

/**
 * Mod Scripts: the code a mod's grid serves to the players standing in it (the platform's CLIENT halves). The game says
 * which chunk the player stands in, asks the player before an author's scripts run, and answers the calls only it can
 * through SetHost. This version declares the surface only: IsAvailable is false and nothing is listed, asked or run.
 */
UCLASS()
class CROWDYSERVICES_API UCrowdyModScripts : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;

	/** Whether this build can run Mod Scripts. False in this version. */
	UFUNCTION(BlueprintPure, Category="Crowdy SDK|Mod Scripts")
	bool IsAvailable() const;

	/** The local player stands in this chunk; the SDK finds the grid that decides it. */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Mod Scripts")
	void EnterGrid(int64 ChunkX, int64 ChunkY, int64 ChunkZ);

	/** The local player left the grid: every Mod Script of it stops. */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Mod Scripts")
	void LeaveGrid();

	/** The grid the scripts run in, as the platform reported it; empty outside a grid. */
	UFUNCTION(BlueprintPure, Category="Crowdy SDK|Mod Scripts")
	FCrowdyModScriptGrid GetGrid() const;

	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Mod Scripts")
	void SetHost(TScriptInterface<ICrowdyModScriptHost> Host);

	/** Clears the host only if it is the one set. */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Mod Scripts")
	void ClearHost(TScriptInterface<ICrowdyModScriptHost> Host);

	/** Run trusts the prompt's author on this grid for its calls; a decline is not asked again until they change. */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Mod Scripts")
	void AnswerPrompt(int32 PromptId, bool bRun);

	UFUNCTION(BlueprintPure, Category="Crowdy SDK|Mod Scripts")
	TArray<FCrowdyModScript> GetRunning() const;

	/** Stops the script and takes back the player's agreement to it. */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Mod Scripts")
	void Revoke(const FString& ModId);

	/** Stops all of the author's scripts on this grid and takes back the player's trust in the author. */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Mod Scripts")
	void ForgetAuthor(int64 AuthorId);

	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Mod Scripts")
	FOnCrowdyModScriptPrompt OnModScriptPrompt;

	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Mod Scripts")
	FOnCrowdyModScriptPromptWithdrawn OnModScriptPromptWithdrawn;

	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Mod Scripts")
	FOnCrowdyModScriptStarted OnModScriptStarted;

	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Mod Scripts")
	FOnCrowdyModScriptStopped OnModScriptStopped;

	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Mod Scripts")
	FOnCrowdyModScriptError OnModScriptError;

private:
	TWeakObjectPtr<UObject> HostObject;
};
