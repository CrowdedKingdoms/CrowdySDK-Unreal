#pragma once

#include "CoreMinimal.h"
#include "Data/CrowdyContainerManifest.h"
#include "CrowdyContainerManifestApply.generated.h"

/** One manifest row the server refused or the transport lost. */
USTRUCT(BlueprintType)
struct CROWDYREPLICATION_API FCrowdyManifestRowFailure
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Containers")
	FString TypeName;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Containers")
	FString BindingKey;

	/** The server's refusal code when it gave one, else a short SDK word (transport, shutdown, no_client). */
	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Containers")
	FString Code;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Containers")
	FString Message;

	/** True for a refusal that applying again may clear (the allowance, a dropped connection), false for a policy refusal. */
	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Containers")
	bool bRetryable = false;
};

/**
 * What applying a manifest did. The apply stops at the first failure it cannot retry, so Unanswered counts the rows
 * never tried plus the ones in flight at a teardown; an ensure is idempotent, so applying again is always safe.
 */
USTRUCT(BlueprintType)
struct CROWDYREPLICATION_API FCrowdyApplyManifestResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Containers")
	int32 Created = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Containers")
	int32 Existing = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Containers")
	int32 Failed = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Containers")
	int32 Unanswered = 0;

	/** Rows of an app-scoped type, sent with no session whatever session the apply named. */
	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Containers")
	int32 AppScoped = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Containers")
	TArray<FCrowdyManifestRowFailure> Failures;

	bool IsComplete() const { return Failed == 0 && Unanswered == 0; }
};

/** The answer to one row's ensure, as the runner classifies it. */
struct FCrowdyManifestApplyOutcome
{
	bool bOk = false;
	bool bCreated = false;
	bool bRetryable = false;
	FString Code;
	FString Message;
};

/**
 * Applies a manifest's rows through a caller-supplied sender, at most MaxInFlight at a time, pacing on a
 * caller-supplied allowance check and retrying a retryable refusal with backoff, stopping at the first failure it
 * cannot retry so a systemic refusal is one report and not one per row. Owns no transport or clock: the subsystem
 * hands it the ensure call and a world timer, a test hands it canned ones. Completes exactly once, on the last
 * outcome or on Abort.
 */
class CROWDYREPLICATION_API FCrowdyManifestApplyRunner : public TSharedFromThis<FCrowdyManifestApplyRunner>
{
public:
	using FOnRowDone = TFunction<void(const FCrowdyManifestApplyOutcome&)>;
	using FSend = TFunction<void(const FCrowdyContainerManifestRow& Row, FOnRowDone OnDone)>;
	using FOnDone = TFunction<void(const FCrowdyApplyManifestResult&)>;
	using FCanSend = TFunction<bool()>;
	using FSchedule = TFunction<void(float Seconds, TFunction<void()> Fn)>;

	static constexpr int32 DefaultMaxInFlight = 8;
	static constexpr int32 MaxRetriesPerRow = 4;
	static constexpr float PaceWaitSeconds = 1.0f;

	FCrowdyManifestApplyRunner(TArray<FCrowdyContainerManifestRow> InRows, int32 InMaxInFlight, FSend InSend, FOnDone InOnDone);

	/** Optional. CanSend false parks the runner until Schedule fires; without Schedule there is no pacing or retry. */
	void SetPacing(FCanSend InCanSend, FSchedule InSchedule);

	void Start();
	/** Fails everything not yet answered with Code and completes now; the rows in flight are counted as unanswered. */
	void Abort(const FString& Code, const FString& Message);
	bool IsDone() const { return bDone; }
	int32 GetActive() const { return Active; }

	/** The wait before a row's Nth retry: doubling from one second, capped just past the allowance window. */
	static float RetryDelaySeconds(int32 RetryIndex);

private:
	void Pump();
	void Settle(int32 RowIndex, const FCrowdyManifestApplyOutcome& Outcome);
	void ScheduleResume(float Seconds);
	void Finish();

	TArray<FCrowdyContainerManifestRow> Rows;
	TArray<int32> RetriesByRow;
	TArray<int32> RetryQueue;
	int32 MaxInFlight = DefaultMaxInFlight;
	FSend Send;
	FOnDone OnDone;
	FCanSend CanSend;
	FSchedule Schedule;
	FCrowdyApplyManifestResult Result;
	int32 Cursor = 0;
	int32 Active = 0;
	bool bStopped = false;
	bool bDone = false;
	bool bWaiting = false;
	bool bPumping = false;
	bool bPumpRequested = false;
};
