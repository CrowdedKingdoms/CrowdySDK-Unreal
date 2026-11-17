#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Subsystem/CrowdyModScriptTypes.h"
#include "CrowdyModScriptsActions.generated.h"

// One signature serves each node's pins, because a Blueprint async node builds its pins from its delegates.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FCrowdyModScriptStatusAsyncResult, const TArray<FCrowdyModScriptStatus>&, Scripts, ECrowdyModScriptReadResult, Result,
	const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FCrowdyModScriptLogAsyncResult, const TArray<FCrowdyModScriptLogLine>&, Lines, ECrowdyModScriptReadResult, Result,
	const FString&, Message);

/** Where each of a grid's Mod Scripts is. C++ callers use UCrowdyModScripts::GetScriptStatus. */
UCLASS()
class CROWDYSERVICES_API UCrowdyGetScriptStatus : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FCrowdyModScriptStatusAsyncResult OnDone;

	UPROPERTY(BlueprintAssignable)
	FCrowdyModScriptStatusAsyncResult OnFailed;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Get Script Status"), Category="Crowdy SDK|Mod Scripts")
	static UCrowdyGetScriptStatus* GetScriptStatus(UObject* WorldContextObject, int64 GridId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 GridId = 0;

	void Finish(const FCrowdyModScriptStatusList& Status);
};

/** A Mod Script's last log lines, oldest first, from memory. C++ callers use UCrowdyModScripts::GetScriptLog. */
UCLASS()
class CROWDYSERVICES_API UCrowdyGetScriptLog : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FCrowdyModScriptLogAsyncResult OnDone;

	UPROPERTY(BlueprintAssignable)
	FCrowdyModScriptLogAsyncResult OnFailed;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Get Script Log"), Category="Crowdy SDK|Mod Scripts")
	static UCrowdyGetScriptLog* GetScriptLog(UObject* WorldContextObject, int64 GridId, const FString& ModId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 GridId = 0;
	FString ModId;

	void Finish(const FCrowdyModScriptLog& Log);
};
