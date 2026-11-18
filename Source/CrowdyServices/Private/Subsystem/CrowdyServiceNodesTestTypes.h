#pragma once

#include "CoreMinimal.h"
#include "Subsystem/AsyncActions/CrowdyClaimsActions.h"
#include "Subsystem/AsyncActions/CrowdyModScriptsActions.h"
#include "Subsystem/AsyncActions/CrowdyModsActions.h"
#include "Subsystem/AsyncActions/CrowdyStudioActions.h"
#include "UObject/Object.h"
#include "CrowdyServiceNodesTestTypes.generated.h"

/** Counts what one pin of a service's Blueprint async node delivers, whatever its signature. */
UCLASS(Transient, Hidden, meta=(CrowdyTestFixture))
class UCrowdyServiceNodeTestListener : public UObject
{
	GENERATED_BODY()

public:
	int32 Calls = 0;
	FString LastMessage;

	UFUNCTION()
	void HandleBoot(const FCrowdyStudioAgentBoot& Boot, ECrowdyStudioResult Result, const FString& Message) { Note(Message); }

	UFUNCTION()
	void HandleConsent(bool bConsented, int64 ConsentedAtMs, ECrowdyStudioResult Result, const FString& Message) { Note(Message); }

	UFUNCTION()
	void HandleUsage(const FCrowdyStudioModelUsage& Usage, ECrowdyStudioResult Result, const FString& Message) { Note(Message); }

	UFUNCTION()
	void HandleLiveModding(bool bAllowed, ECrowdyStudioResult Result, const FString& Message) { Note(Message); }

	UFUNCTION()
	void HandleProject(const FCrowdyStudioProject& Project, ECrowdyStudioResult Result, const FString& Message) { Note(Message); }

	UFUNCTION()
	void HandleProjectList(const TArray<FCrowdyStudioProject>& Projects, bool bMore, ECrowdyStudioResult Result, const FString& Message) { Note(Message); }

	UFUNCTION()
	void HandleScriptStatus(const TArray<FCrowdyModScriptStatus>& Scripts, ECrowdyModScriptReadResult Result, const FString& Message) { Note(Message); }

	UFUNCTION()
	void HandleScriptLog(const TArray<FCrowdyModScriptLogLine>& Lines, ECrowdyModScriptReadResult Result, const FString& Message) { Note(Message); }

	UFUNCTION()
	void HandleBuild(const FString& BuildId, ECrowdyModBuildStatus Status, const FString& Log, const FString& SdkVersion, ECrowdyModResult Result, const FString& Message)
	{
		Note(Message);
	}

	UFUNCTION()
	void HandleClientInfo(const FCrowdyModClient& Client, ECrowdyModResult Result, const FString& Message) { Note(Message); }

	UFUNCTION()
	void HandleRun(const FCrowdyModRun& Run, ECrowdyModResult Result, const FString& Message) { Note(Message); }

	UFUNCTION()
	void HandleProjectStatus(const FCrowdyModProjectStatus& Status, ECrowdyModResult Result, const FString& Message) { Note(Message); }

	UFUNCTION()
	void HandleGridChannels(const TArray<FCrowdyChannel>& Channels, ECrowdyClaimResult Result, const FString& Message) { Note(Message); }

private:
	void Note(const FString& Message)
	{
		++Calls;
		LastMessage = Message;
	}
};
