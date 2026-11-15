#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"

#include <atomic>

CROWDYMODSCRIPTRUNTIME_API DECLARE_LOG_CATEGORY_EXTERN(LogCrowdyModScripts, Log, All);

namespace CrowdyModScriptTrace
{
	CROWDYMODSCRIPTRUNTIME_API bool Enabled();

	// Replaces control characters with spaces, so untrusted text always logs as one line of its own.
	CROWDYMODSCRIPTRUNTIME_API FString Printable(FString Text);
}

enum class ECrowdyModScriptExport : uint8
{
	Init,
	Tick,
	Event
};

enum class ECrowdyModScriptOutcome : uint8
{
	Ok,
	Fuel,
	Watchdog,
	Trap,
	HostTimeout
};

CROWDYMODSCRIPTRUNTIME_API const TCHAR* LexToString(ECrowdyModScriptOutcome Outcome);

enum class ECrowdyModScriptRunEnd : uint8
{
	InitFailed,
	CircuitOpen
};

struct FCrowdyModScriptResult
{
	ECrowdyModScriptOutcome Outcome = ECrowdyModScriptOutcome::Ok;
	FString Message;
	double ElapsedMs = 0.0;
};

// Every callback runs on the thread that dispatches the script, which is a worker thread under FCrowdyModScriptWorkerPool.
struct FCrowdyModScriptHost
{
	TFunction<void(int32 Level, const FString& Message)> OnLog;
	TFunction<TArray<uint8>(TConstArrayView<uint8> RequestUtf8)> OnHostCall;
	// Used instead of OnHostCall when set; returning false ends the dispatch with HostTimeout.
	TFunction<bool(TConstArrayView<uint8> RequestUtf8, TArray<uint8>& OutAnswer)> OnHostCallOrTimeout;
};

struct FCrowdyModScriptEngineState;
struct FCrowdyModScriptModuleState;
struct FCrowdyModScriptInstanceState;
struct FCrowdyModScriptWorkerPoolState;
struct FCrowdyModScriptRunCore;

// The process-wide WebAssembly engine: Pulley interpreter only, with a 50 ms epoch watchdog.
class CROWDYMODSCRIPTRUNTIME_API FCrowdyModScriptRuntime
{
public:
	// Null before the module starts and after it shuts down.
	static TSharedPtr<FCrowdyModScriptRuntime> Get();

	static int64 UnixNowMs();

	FCrowdyModScriptRuntime();
	~FCrowdyModScriptRuntime();

	bool IsAvailable() const;
	const FString& GetUnavailableReason() const;

private:
	friend class FCrowdyModScriptModule;
	friend class FCrowdyModScriptInstance;

	TUniquePtr<FCrowdyModScriptEngineState> State;
};

class CROWDYMODSCRIPTRUNTIME_API FCrowdyModScriptModule
{
public:
	static constexpr int32 MaxModuleBytes = 512 * 1024;

	// Compiles and validates untrusted bytes. On refusal returns null and sets OutError to the reason.
	static TSharedPtr<FCrowdyModScriptModule> Compile(const TSharedRef<FCrowdyModScriptRuntime>& Runtime, TConstArrayView<uint8> Bytes, FString& OutError);

	~FCrowdyModScriptModule();

private:
	friend class FCrowdyModScriptInstance;

	FCrowdyModScriptModule();

	TUniquePtr<FCrowdyModScriptModuleState> State;
};

// Compiled modules shared by the whole process, keyed by lowercase SHA-256.
class CROWDYMODSCRIPTRUNTIME_API FCrowdyModScriptModuleCache
{
public:
	static constexpr int32 MaxModules = 16;

	// Thread-safe. Refuses Bytes whose SHA-256 is not DigestHex, compiles on a miss and drops the least recently used past MaxModules.
	static TSharedPtr<FCrowdyModScriptModule> Get(const TSharedRef<FCrowdyModScriptRuntime>& Runtime, const FString& DigestHex, TConstArrayView<uint8> Bytes, FString& OutError);

	// Get on a background task. OnDone runs on the game thread, never before this returns.
	static void GetAsync(const TSharedRef<FCrowdyModScriptRuntime>& Runtime, const FString& DigestHex, TArray<uint8> Bytes,
		TFunction<void(TSharedPtr<FCrowdyModScriptModule> Module, const FString& Error)> OnDone);

	// Lowercase hex, or empty where Mod Scripts are not supported.
	static FString Sha256Hex(TConstArrayView<uint8> Bytes);
};

// One store per instance. Never used by two threads at once: a Dispatch while another runs is refused.
class CROWDYMODSCRIPTRUNTIME_API FCrowdyModScriptInstance
{
public:
	static constexpr int64 MemoryLimitBytes = 32 * 1024 * 1024;

	static TSharedPtr<FCrowdyModScriptInstance> Create(const TSharedRef<FCrowdyModScriptModule>& Module, FCrowdyModScriptHost Host, FString& OutError);

	~FCrowdyModScriptInstance();

	// Arg is passed to tick and ignored for init. Event copies Payload into guest memory and calls on_event with it; a module
	// without on_event ends it Ok.
	FCrowdyModScriptResult Dispatch(ECrowdyModScriptExport Export, int64 FuelPerDispatch, int32 Arg = 0, TConstArrayView<uint8> Payload = {});

	// Guest memory size as of the end of the last dispatch.
	uint64 GetMemoryBytes() const;

	// Whether the module exports an on_event taking (ptr, len); fixed once the instance exists.
	bool HasOnEvent() const;

	int64 GetSkippedDispatches() const { return SkippedDispatches.load(); }

	// Safe from any thread. A running dispatch ends within one watchdog tick and every later one is refused.
	void RequestStop();

private:
	friend struct FCrowdyModScriptWorkerPoolState;
	friend class FCrowdyModScriptWorkerPool;

	FCrowdyModScriptInstance();

	TUniquePtr<FCrowdyModScriptInstanceState> State;
	std::atomic<bool> bPoolClaimed{false};
	std::atomic<int64> SkippedDispatches{0};
};

// Runs at most two guests at once on below-normal-priority threads; a guest waiting on a host call gives up its slot.
class CROWDYMODSCRIPTRUNTIME_API FCrowdyModScriptWorkerPool
{
public:
	static constexpr int32 MaxWorkers = 2;

	// Threads beyond the running slots, so scripts waiting on the platform do not stop the others.
	static constexpr int32 MaxWaitingWorkers = 8;

	// On a pool worker, frees its running slot while Wait runs and takes one back after; elsewhere only runs Wait.
	static void WaitOffSlot(TFunctionRef<void()> Wait);

	// The process-wide pool, started on first use. Null before the module starts and after it shuts down.
	static TSharedPtr<FCrowdyModScriptWorkerPool> GetShared();

	explicit FCrowdyModScriptWorkerPool(int32 NumWorkers = MaxWorkers);
	~FCrowdyModScriptWorkerPool();

	UE_NONCOPYABLE(FCrowdyModScriptWorkerPool);

	// Refuses and counts a skip while the instance's previous dispatch is queued or running. OnDone runs on the worker.
	bool TryQueue(const TSharedRef<FCrowdyModScriptInstance>& Instance, ECrowdyModScriptExport Export, int64 FuelPerDispatch, int32 Arg, TFunction<void(const FCrowdyModScriptResult&)> OnDone,
		TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> Payload = nullptr);

	int64 GetSkippedCount() const;

private:
	TUniquePtr<FCrowdyModScriptWorkerPoolState> State;
};

// Every callback but OnHostCallWorker runs on the game thread, and none runs once the run has stopped.
struct FCrowdyModScriptRunHost
{
	// Answers the guest's JSON request with JSON.
	TFunction<TArray<uint8>(TConstArrayView<uint8> RequestUtf8)> OnHostCall;
	// Optional, on the worker before the request reaches the game thread: false answers the guest at once with InOutRequest,
	// true hands InOutRequest, perhaps rewritten, to OnHostCall. It must touch no UObject.
	TFunction<bool(TArray<uint8>& InOutRequest)> OnHostCallWorker;
	// Optional, on the worker once OnHostCall has answered: may rewrite the answer before the guest reads it. It must touch
	// no UObject.
	TFunction<void(TArray<uint8>& InOutAnswer)> OnHostAnswerWorker;
	// Fires once when the run ends by itself; never after Stop.
	TFunction<void(ECrowdyModScriptRunEnd End, const FString& Message)> OnStopped;
};

// A host call answered after OnHostCall returned. Send works from any thread; only the first Send counts, and one after the
// call timed out or its run stopped is dropped.
class CROWDYMODSCRIPTRUNTIME_API FCrowdyModScriptAnswer
{
public:
	void Send(TArray<uint8>&& AnswerUtf8);

	// Whether the script still waits for this answer: false once sent, timed out or stopped.
	bool IsWaiting() const;

private:
	friend class FCrowdyModScriptRun;

	TWeakPtr<FCrowdyModScriptRunCore, ESPMode::ThreadSafe> Core;
	uint32 Id = 0;
};

// One run of one Mod Script, owned and called on the game thread. The guest runs on the shared worker pool.
// After a Watchdog or HostTimeout the run starts a fresh instance and runs init again, losing the state blob.
class CROWDYMODSCRIPTRUNTIME_API FCrowdyModScriptRun : public TSharedFromThis<FCrowdyModScriptRun>
{
public:
	static constexpr int32 MinTickIntervalMs = 16;
	static constexpr int32 MaxTickIntervalMs = 1000;
	static constexpr int32 MaxConsecutiveFailures = 5;
	static constexpr double HostCallTimeoutSeconds = 5.0;
	static constexpr double NextHostCallWaitSeconds = 0.0001;
	static constexpr int32 MaxLogLinesPerSecond = 20;
	static constexpr int32 MaxLogLineChars = 1000;
	static constexpr int32 MaxQueuedEvents = 64;
	static constexpr int32 MaxQueuedEventBytes = 1024 * 1024;

	// Instantiates the module and queues init. On refusal returns null and sets OutError to the reason.
	static TSharedPtr<FCrowdyModScriptRun> Start(const TSharedRef<FCrowdyModScriptModule>& Module, FCrowdyModScriptRunHost Host, int64 FuelPerDispatch, int32 TickIntervalMs,
		const FString& Name, FString& OutError);

	~FCrowdyModScriptRun();

	UE_NONCOPYABLE(FCrowdyModScriptRun);

	// Call once a frame. Queues tick once the interval has passed, skipping it while the last dispatch still runs.
	void Tick(double NowSeconds);

	// Answers waiting host calls in order until none waits or BudgetMs is spent, and returns how many it answered.
	// After each answer it waits up to NextHostCallWaitSeconds for the script's next call.
	int32 DrainHostCalls(double BudgetMs);

	// Only inside OnHostCall: the call is answered later through the handle, and what OnHostCall returns is ignored. The
	// script waits, without spending its watchdog, until the answer or HostCallTimeoutSeconds.
	FCrowdyModScriptAnswer DeferHostCall();

	// Never waits on a worker: a waiting host call fails at once and a running guest ends within one watchdog tick.
	void Stop();

	uint32 GetGeneration() const { return Generation; }
	bool IsRunning() const { return bRunning; }
	int64 GetSkippedTicks() const { return SkippedTicks; }

	// True while a dispatch is queued or running, including one still ending after Stop.
	bool IsDispatching() const;

	// Queues Payload for on_event, dispatched in order on the frames a tick does not take; one payload may be queued on many
	// runs. True with nothing queued for a module without on_event. False once the run stopped or is being replaced, or when
	// MaxQueuedEvents or MaxQueuedEventBytes already wait. Depth is the event's cascade depth, read back by GetEventDepth.
	bool QueueEvent(const TSharedRef<const TArray<uint8>, ESPMode::ThreadSafe>& Payload, int32 Depth);

	// The cascade depth of the event dispatch queued last, or 0 when that was init or tick.
	int32 GetEventDepth() const { return ActiveEventDepth; }

private:
	struct FQueuedEvent
	{
		TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> Payload;
		int32 Depth = 0;
	};

	FCrowdyModScriptRun() = default;

	bool Queue(ECrowdyModScriptExport Export, int32 Arg, TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> Payload = nullptr);
	bool ConsumeResults(double NowSeconds);
	bool Recycle();
	bool CountFailure(const FString& Message);
	void EndRun(ECrowdyModScriptRunEnd Reason, const FString& Message);

	TSharedPtr<FCrowdyModScriptRunCore> Core;
	TSharedPtr<FCrowdyModScriptModule> ScriptModule;
	TSharedPtr<FCrowdyModScriptInstance> Instance;
	TSharedPtr<FCrowdyModScriptWorkerPool> Pool;
	FCrowdyModScriptRunHost Callbacks;
	TArray<FQueuedEvent> Events;
	int64 QueuedEventBytes = 0;
	int64 Fuel = 0;
	int64 SkippedTicks = 0;
	double TickIntervalSeconds = 1.0;
	double LastTickSeconds = 0.0;
	double NextTickSeconds = 0.0;
	uint32 Generation = 1;
	uint32 AnsweringCallId = 0;
	int32 ConsecutiveFailures = 0;
	int32 ActiveEventDepth = 0;
	bool bRunning = true;
	bool bInitDone = false;
	bool bEverInitialized = false;
	bool bNeedsInstance = false;
	bool bDeferred = false;
};
