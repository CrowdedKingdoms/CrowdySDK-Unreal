#pragma once

#include "CoreMinimal.h"
#include "CrowdyServerObjectTypes.h"
#include "UObject/Object.h"
#include "CrowdyServerObjectParityTestTypes.generated.h"

class UCrowdyServerObject;

/** Records the Blueprint record events bound to it, as a Blueprint graph would receive them. */
UCLASS(Transient)
class UCrowdyServerObjectParityListener : public UObject
{
	GENERATED_BODY()

public:
	TArray<FString> WatchedRecordIds;
	TArray<TArray<FString>> WatchedChanged;
	TArray<TArray<FCrowdyServerRecordChange>> UpdatedChanges;
	TArray<TArray<FString>> UpdatedRemoved;
	TArray<TArray<FString>> ChangedIds;
	TArray<TArray<FString>> RemovedIds;

	/** Runs after HandleRecord records an event, as more of a handler's graph would. */
	TFunction<void()> OnRecord;

	UFUNCTION()
	void HandleRecord(UCrowdyServerObject* Object, const FString& RecordId, const TArray<FString>& Changed)
	{
		WatchedRecordIds.Add(RecordId);
		WatchedChanged.Add(Changed);
		if (OnRecord)
		{
			OnRecord();
		}
	}

	UFUNCTION()
	void HandleUpdated(UCrowdyServerObject* Object, const TArray<FCrowdyServerRecordChange>& Changed, const TArray<FString>& Removed)
	{
		UpdatedChanges.Add(Changed);
		UpdatedRemoved.Add(Removed);
	}

	UFUNCTION()
	void HandleChanged(UCrowdyServerObject* Object, const TArray<FString>& Changed, const TArray<FString>& Removed)
	{
		ChangedIds.Add(Changed);
		RemovedIds.Add(Removed);
	}
};
