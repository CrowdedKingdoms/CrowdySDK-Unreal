#include "CrowdyModScriptRuntime.h"

#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"

namespace
{
	const TCHAR* RunExportName(ECrowdyModScriptExport Export)
	{
		switch (Export)
		{
		case ECrowdyModScriptExport::Init:  return TEXT("init");
		case ECrowdyModScriptExport::Tick:  return TEXT("tick");
		default:                            return TEXT("on_event");
		}
	}
}

FCrowdyModScriptLogRing::FCrowdyModScriptLogRing()
{
	Slots.SetNum(Capacity);
}

void FCrowdyModScriptLogRing::Add(int32 Level, FString&& Text)
{
	const int64 AtMs = FCrowdyModScriptRuntime::UnixNowMs();
	FScopeLock Guard(&Lock);
	const bool bFull = Held == Capacity;
	FCrowdyModScriptLogEntry& Slot = Slots[bFull ? Head : (Head + Held) % Capacity];
	Slot.AtMs = AtMs;
	Slot.Level = Level;
	Slot.Text = MoveTemp(Text);
	Head = bFull ? (Head + 1) % Capacity : Head;
	Held += bFull ? 0 : 1;
	Evicted += bFull ? 1 : 0;
}

void FCrowdyModScriptLogRing::CountRateDropped()
{
	FScopeLock Guard(&Lock);
	++RateDropped;
}

void FCrowdyModScriptLogRing::Read(TArray<FCrowdyModScriptLogEntry>& OutLines, int32& OutEvicted, int32& OutRateDropped) const
{
	FScopeLock Guard(&Lock);
	OutLines.Reset(Held);
	for (int32 Index = 0; Index < Held; ++Index)
	{
		OutLines.Add(Slots[(Head + Index) % Capacity]);
	}
	OutEvicted = Evicted;
	OutRateDropped = RateDropped;
}

// The part of a run that workers touch. The game thread's FCrowdyModScriptRun never reaches a worker.
struct FCrowdyModScriptRunCore
{
	struct FFinished
	{
		FCrowdyModScriptResult Result;
		ECrowdyModScriptExport Export = ECrowdyModScriptExport::Tick;
	};

	// One dispatch at a time per run, so at most one host call waits.
	enum class ECall : uint8
	{
		Idle,
		Waiting,
		Taken,
		Answered
	};

	FCrowdyModScriptRunCore(const FString& InName, TSharedPtr<FCrowdyModScriptLogRing, ESPMode::ThreadSafe> InRing)
		: Name(CrowdyModScriptTrace::Printable(InName))
		, CallDone(FPlatformProcess::GetSynchEventFromPool(false))
		, Ring(MoveTemp(InRing))
	{
	}

	static FCrowdyModScriptHost MakeInstanceHost(const TSharedRef<FCrowdyModScriptRunCore>& RunCore, const FCrowdyModScriptRunHost& Callbacks)
	{
		FCrowdyModScriptHost InstanceHost;
		InstanceHost.OnLog = [RunCore](int32 Level, const FString& Line)
		{
			RunCore->AddLogLine(Level, Line);
		};
		if (!Callbacks.OnHostCall)
		{
			return InstanceHost;
		}
		InstanceHost.OnHostCallOrTimeout = [RunCore, OnWorker = Callbacks.OnHostCallWorker, OnAnswer = Callbacks.OnHostAnswerWorker](
			TConstArrayView<uint8> Request, TArray<uint8>& OutAnswer)
		{
			if (!OnWorker)
			{
				return RunCore->Call(Request, OutAnswer);
			}
			TArray<uint8> Prepared(Request);
			if (!OnWorker(Prepared))
			{
				OutAnswer = MoveTemp(Prepared);
				return true;
			}
			if (!RunCore->Call(Prepared, OutAnswer))
			{
				return false;
			}
			if (OnAnswer)
			{
				OnAnswer(OutAnswer);
			}
			return true;
		};
		return InstanceHost;
	}

	~FCrowdyModScriptRunCore()
	{
		FPlatformProcess::ReturnSynchEventToPool(CallDone);
	}

	UE_NONCOPYABLE(FCrowdyModScriptRunCore);

	const FString Name;
	FEvent* const CallDone;
	const TSharedPtr<FCrowdyModScriptLogRing, ESPMode::ThreadSafe> Ring;
	std::atomic<int32> InFlight{0};

	// Guarded by Lock. CallBytes holds the request until it is taken, then the answer.
	FCriticalSection Lock;
	TArray<FFinished> Finished;
	TArray<uint8> CallBytes;
	double LogWindowSeconds = 0.0;
	uint32 CallId = 0;
	int32 LogLines = 0;
	int32 LogDropped = 0;
	ECall CallState = ECall::Idle;
	bool bStopped = false;

	// Worker thread: waits for the game thread's answer.
	bool Call(TConstArrayView<uint8> Request, TArray<uint8>& OutAnswer)
	{
		{
			FScopeLock Guard(&Lock);
			if (bStopped)
			{
				return false;
			}
			++CallId;
			CallBytes = Request;
			CallState = ECall::Waiting;
		}
		const double DeadlineSeconds = FPlatformTime::Seconds() + FCrowdyModScriptRun::HostCallTimeoutSeconds;
		bool bAnswered = false;
		// Another script's guest may run while this one waits.
		FCrowdyModScriptWorkerPool::WaitOffSlot([this, DeadlineSeconds, &OutAnswer, &bAnswered]()
		{
			for (;;)
			{
				const double LeftSeconds = DeadlineSeconds - FPlatformTime::Seconds();
				{
					FScopeLock Guard(&Lock);
					if (CallState == ECall::Answered)
					{
						OutAnswer = MoveTemp(CallBytes);
						CallState = ECall::Idle;
						bAnswered = true;
						return;
					}
					if (bStopped || LeftSeconds <= 0.0)
					{
						CallState = ECall::Idle;
						return;
					}
				}
				CallDone->Wait(static_cast<uint32>(FMath::Max(1, FMath::CeilToInt32(LeftSeconds * 1000.0))));
			}
		});
		return bAnswered;
	}

	// Game thread: spins briefly so a script's back-to-back calls are answered in one frame.
	void WaitForCall(double UntilSeconds)
	{
		while (FPlatformTime::Seconds() < UntilSeconds)
		{
			{
				FScopeLock Guard(&Lock);
				if (CallState == ECall::Waiting)
				{
					return;
				}
			}
			FPlatformProcess::YieldThread();
		}
	}

	bool TakeCall(TArray<uint8>& OutRequest, uint32& OutId)
	{
		FScopeLock Guard(&Lock);
		if (CallState != ECall::Waiting)
		{
			return false;
		}
		OutRequest = MoveTemp(CallBytes);
		OutId = CallId;
		CallState = ECall::Taken;
		return true;
	}

	bool IsTaken(uint32 Id)
	{
		FScopeLock Guard(&Lock);
		return CallState == ECall::Taken && CallId == Id && !bStopped;
	}

	// Dropped when the worker stopped waiting for this call.
	void Answer(uint32 Id, TArray<uint8>&& Bytes)
	{
		{
			FScopeLock Guard(&Lock);
			if (CallState != ECall::Taken || CallId != Id)
			{
				return;
			}
			CallBytes = MoveTemp(Bytes);
			CallState = ECall::Answered;
		}
		CallDone->Trigger();
	}

	void Stop()
	{
		{
			FScopeLock Guard(&Lock);
			bStopped = true;
		}
		CallDone->Trigger();
	}

	// Call under Lock. Returns the lines dropped in the window that just closed.
	int32 RollLogWindow(double NowSeconds)
	{
		if (NowSeconds - LogWindowSeconds < 1.0)
		{
			return 0;
		}
		LogWindowSeconds = NowSeconds;
		LogLines = 0;
		const int32 Dropped = LogDropped;
		LogDropped = 0;
		return Dropped;
	}

	void ReportDropped(int32 Dropped) const
	{
		UE_CLOG(Dropped > 0 && CrowdyModScriptTrace::Enabled(), LogCrowdyModScripts, Log, TEXT("%s: %d log lines dropped"), *Name, Dropped);
	}

	// Worker thread.
	void AddLogLine(int32 Level, const FString& Line)
	{
		int32 Dropped = 0;
		bool bKeep = false;
		{
			FScopeLock Guard(&Lock);
			// A replaced run's last dispatch must not write into the ring its successor shares.
			if (bStopped)
			{
				return;
			}
			Dropped = RollLogWindow(FPlatformTime::Seconds());
			bKeep = LogLines < FCrowdyModScriptRun::MaxLogLinesPerSecond;
			LogLines += bKeep ? 1 : 0;
			LogDropped += bKeep ? 0 : 1;
		}
		ReportDropped(Dropped);
		if (!bKeep)
		{
			if (Ring)
			{
				Ring->CountRateDropped();
			}
			return;
		}
		const bool bTrace = CrowdyModScriptTrace::Enabled();
		if (!Ring && !bTrace)
		{
			return;
		}
		FString Text = Line.Left(FCrowdyModScriptRun::MaxLogLineChars);
		// A script's own text reaches the game's log only while tracing is on.
		UE_CLOG(bTrace, LogCrowdyModScripts, Log, TEXT("%s: %s"), *Name, *Text);
		if (Ring)
		{
			Ring->Add(Level, MoveTemp(Text));
		}
	}

	void FlushLog()
	{
		int32 Dropped = 0;
		{
			FScopeLock Guard(&Lock);
			Dropped = RollLogWindow(FPlatformTime::Seconds());
		}
		ReportDropped(Dropped);
	}

	// Worker thread. The result is queued before InFlight drops, so a finished run's result is always there to read.
	void Finish(ECrowdyModScriptExport Export, const FCrowdyModScriptResult& Result)
	{
		UE_CLOG(CrowdyModScriptTrace::Enabled(), LogCrowdyModScripts, Log, TEXT("%s: %s ended %s after %.1f ms %s"), *Name,
			RunExportName(Export), LexToString(Result.Outcome), Result.ElapsedMs, *Result.Message);
		{
			FScopeLock Guard(&Lock);
			Finished.Add({Result, Export});
		}
		--InFlight;
	}
};

TSharedPtr<FCrowdyModScriptRun> FCrowdyModScriptRun::Start(const TSharedRef<FCrowdyModScriptModule>& Module, FCrowdyModScriptRunHost Host, int64 FuelPerDispatch,
	int32 TickIntervalMs, const FString& Name, FString& OutError)
{
	TSharedPtr<FCrowdyModScriptWorkerPool> SharedPool = FCrowdyModScriptWorkerPool::GetShared();
	if (!SharedPool)
	{
		OutError = TEXT("the Mod Script worker pool is not running");
		return nullptr;
	}

	TSharedRef<FCrowdyModScriptRunCore> RunCore = MakeShared<FCrowdyModScriptRunCore>(Name, Host.Log);
	TSharedPtr<FCrowdyModScriptInstance> NewInstance = FCrowdyModScriptInstance::Create(Module, FCrowdyModScriptRunCore::MakeInstanceHost(RunCore, Host), OutError);
	if (!NewInstance)
	{
		return nullptr;
	}

	TSharedPtr<FCrowdyModScriptRun> Run = MakeShareable(new FCrowdyModScriptRun());
	Run->Core = RunCore;
	Run->ScriptModule = Module;
	Run->Instance = NewInstance;
	Run->Pool = SharedPool;
	Run->Callbacks = MoveTemp(Host);
	Run->Fuel = FuelPerDispatch;
	Run->TickIntervalSeconds = FMath::Clamp(TickIntervalMs, MinTickIntervalMs, MaxTickIntervalMs) / 1000.0;
	if (!Run->Queue(ECrowdyModScriptExport::Init, 0))
	{
		OutError = TEXT("the worker pool refused init");
		return nullptr;
	}
	return Run;
}

FCrowdyModScriptRun::~FCrowdyModScriptRun()
{
	Stop();
}

bool FCrowdyModScriptRun::Queue(ECrowdyModScriptExport Export, int32 Arg, TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> Payload)
{
	++Core->InFlight;
	TSharedRef<FCrowdyModScriptRunCore> RunCore = Core.ToSharedRef();
	const bool bQueued = Pool->TryQueue(Instance.ToSharedRef(), Export, Fuel, Arg, [RunCore, Export](const FCrowdyModScriptResult& Result)
	{
		RunCore->Finish(Export, Result);
	}, MoveTemp(Payload));
	if (bQueued)
	{
		ActiveEventDepth = Export == ECrowdyModScriptExport::Event ? Arg : 0;
		return true;
	}
	--Core->InFlight;
	return false;
}

bool FCrowdyModScriptRun::QueueEvent(const TSharedRef<const TArray<uint8>, ESPMode::ThreadSafe>& Payload, int32 Depth)
{
	if (!bRunning || bNeedsInstance || !Instance.IsValid())
	{
		return false;
	}
	// Delivered, as the browser counts it, though a module without on_event has nothing to call.
	if (!Instance->HasOnEvent())
	{
		return true;
	}
	if (Events.Num() >= MaxQueuedEvents || QueuedEventBytes + Payload->Num() > MaxQueuedEventBytes)
	{
		return false;
	}
	Events.Add({Payload, Depth});
	QueuedEventBytes += Payload->Num();
	return true;
}

void FCrowdyModScriptRun::Tick(double NowSeconds)
{
	if (!bRunning)
	{
		return;
	}
	// A callback may drop the caller's last reference to this run.
	TSharedRef<FCrowdyModScriptRun> KeepAlive = AsShared();
	Core->FlushLog();
	// Read before the results: a dispatch leaves its result before it stops counting as in flight, so idle here means every
	// result is about to be read, and nothing runs on an instance about to be replaced.
	const bool bIdle = !IsDispatching();
	if (!ConsumeResults(NowSeconds))
	{
		return;
	}
	const bool bTickDue = NowSeconds >= NextTickSeconds;
	if (bNeedsInstance)
	{
		if (bTickDue && bIdle)
		{
			NextTickSeconds = NowSeconds + TickIntervalSeconds;
			Recycle();
		}
		return;
	}
	if (!bInitDone)
	{
		return;
	}
	// A due tick goes first, so a stream of events cannot starve it; events take the frames between, in the order sent.
	if (!bTickDue)
	{
		if (bIdle && !Events.IsEmpty())
		{
			FQueuedEvent Next = MoveTemp(Events[0]);
			Events.RemoveAt(0);
			QueuedEventBytes -= Next.Payload->Num();
			Queue(ECrowdyModScriptExport::Event, Next.Depth, MoveTemp(Next.Payload));
		}
		return;
	}
	NextTickSeconds = NowSeconds + TickIntervalSeconds;
	const int64 DtMs = FMath::RoundToInt64((NowSeconds - LastTickSeconds) * 1000.0);
	if (!Queue(ECrowdyModScriptExport::Tick, static_cast<int32>(FMath::Clamp<int64>(DtMs, 0, MAX_int32))))
	{
		++SkippedTicks;
		return;
	}
	LastTickSeconds = NowSeconds;
}

int32 FCrowdyModScriptRun::DrainHostCalls(double BudgetMs)
{
	if (!bRunning)
	{
		return 0;
	}
	TSharedRef<FCrowdyModScriptRun> KeepAlive = AsShared();
	const double EndSeconds = FPlatformTime::Seconds() + BudgetMs / 1000.0;
	int32 Answered = 0;
	TArray<uint8> Request;
	uint32 Id = 0;
	while (bRunning && FPlatformTime::Seconds() < EndSeconds && Core->TakeCall(Request, Id))
	{
		AnsweringCallId = Id;
		bDeferred = false;
		TArray<uint8> Answer = Callbacks.OnHostCall(Request);
		AnsweringCallId = 0;
		if (!bRunning || bDeferred)
		{
			break;
		}
		Core->Answer(Id, MoveTemp(Answer));
		++Answered;
		Core->WaitForCall(FMath::Min(FPlatformTime::Seconds() + NextHostCallWaitSeconds, EndSeconds));
	}
	return Answered;
}

FCrowdyModScriptAnswer FCrowdyModScriptRun::DeferHostCall()
{
	// Outside OnHostCall the id is 0, which no call has, so the handle answers nothing.
	FCrowdyModScriptAnswer Handle;
	bDeferred = true;
	Handle.Core = Core;
	Handle.Id = AnsweringCallId;
	return Handle;
}

void FCrowdyModScriptAnswer::Send(TArray<uint8>&& AnswerUtf8)
{
	const TSharedPtr<FCrowdyModScriptRunCore, ESPMode::ThreadSafe> Pinned = Core.Pin();
	Core.Reset();
	if (Pinned.IsValid())
	{
		Pinned->Answer(Id, MoveTemp(AnswerUtf8));
	}
}

bool FCrowdyModScriptAnswer::IsWaiting() const
{
	const TSharedPtr<FCrowdyModScriptRunCore, ESPMode::ThreadSafe> Pinned = Core.Pin();
	return Pinned.IsValid() && Pinned->IsTaken(Id);
}

void FCrowdyModScriptRun::Stop()
{
	if (!bRunning)
	{
		return;
	}
	bRunning = false;
	++Generation;
	Instance->RequestStop();
	Core->Stop();
	Instance.Reset();
	Pool.Reset();
}

bool FCrowdyModScriptRun::IsDispatching() const
{
	return Core->InFlight.load() > 0;
}

bool FCrowdyModScriptRun::ConsumeResults(double NowSeconds)
{
	TArray<FCrowdyModScriptRunCore::FFinished> Done;
	{
		FScopeLock Guard(&Core->Lock);
		Done = MoveTemp(Core->Finished);
	}
	for (const FCrowdyModScriptRunCore::FFinished& Item : Done)
	{
		const ECrowdyModScriptOutcome Outcome = Item.Result.Outcome;
		const bool bInit = Item.Export == ECrowdyModScriptExport::Init;
		const bool bOk = Outcome == ECrowdyModScriptOutcome::Ok;
		if (bInit && bOk)
		{
			bInitDone = true;
			bEverInitialized = true;
			LastTickSeconds = NowSeconds;
			NextTickSeconds = NowSeconds + TickIntervalSeconds;
			continue;
		}
		if (bOk)
		{
			ConsecutiveFailures = 0;
			continue;
		}
		const FString Message = FString::Printf(TEXT("%s ended %s: %s"), RunExportName(Item.Export), LexToString(Outcome), *Item.Result.Message);
		if (bInit && !bEverInitialized)
		{
			EndRun(ECrowdyModScriptRunEnd::InitFailed, Message);
			return false;
		}
		if (bInit || Outcome == ECrowdyModScriptOutcome::Watchdog || Outcome == ECrowdyModScriptOutcome::HostTimeout)
		{
			bNeedsInstance = true;
		}
		if (!CountFailure(Message))
		{
			return false;
		}
	}
	return true;
}

bool FCrowdyModScriptRun::Recycle()
{
	FString Error;
	TSharedPtr<FCrowdyModScriptInstance> Fresh = FCrowdyModScriptInstance::Create(ScriptModule.ToSharedRef(),
		FCrowdyModScriptRunCore::MakeInstanceHost(Core.ToSharedRef(), Callbacks), Error);
	if (!Fresh)
	{
		return CountFailure(FString::Printf(TEXT("a fresh instance failed: %s"), *Error));
	}
	Instance = Fresh;
	bInitDone = false;
	bNeedsInstance = false;
	// The old instance's events die with it, as the browser's die with its worker.
	Events.Reset();
	QueuedEventBytes = 0;
	if (Queue(ECrowdyModScriptExport::Init, 0))
	{
		return true;
	}
	bNeedsInstance = true;
	return CountFailure(TEXT("the worker pool refused init"));
}

bool FCrowdyModScriptRun::CountFailure(const FString& Message)
{
	if (++ConsecutiveFailures < MaxConsecutiveFailures)
	{
		return true;
	}
	EndRun(ECrowdyModScriptRunEnd::CircuitOpen, Message);
	return false;
}

void FCrowdyModScriptRun::EndRun(ECrowdyModScriptRunEnd Reason, const FString& Message)
{
	TFunction<void(ECrowdyModScriptRunEnd, const FString&)> OnStopped = MoveTemp(Callbacks.OnStopped);
	Stop();
	const TCHAR* ReasonName = Reason == ECrowdyModScriptRunEnd::InitFailed ? TEXT("init failed") : TEXT("circuit open");
	UE_LOG(LogCrowdyModScripts, Log, TEXT("%s: stopped (%s)"), *Core->Name, ReasonName);
	UE_CLOG(CrowdyModScriptTrace::Enabled(), LogCrowdyModScripts, Log, TEXT("%s: stop detail: %s"), *Core->Name, *Message);
	if (OnStopped)
	{
		OnStopped(Reason, Message);
	}
}
