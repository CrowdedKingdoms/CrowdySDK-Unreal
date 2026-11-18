#include "CrowdyModScriptRuntime.h"

#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/ScopeLock.h"

struct FCrowdyModScriptWorkerPoolState;

namespace
{
	constexpr uint32 WorkerIdleWaitMs = 50;
	constexpr uint32 WorkerStackBytes = 2 * 1024 * 1024;

	// The pool whose running slot this worker thread holds, if any.
	thread_local FCrowdyModScriptWorkerPoolState* SlotHolder = nullptr;
}

struct FCrowdyModScriptWorkerPoolState
{
	struct FJob
	{
		TSharedPtr<FCrowdyModScriptInstance> Instance;
		ECrowdyModScriptExport Export = ECrowdyModScriptExport::Tick;
		int64 Fuel = 0;
		int32 Arg = 0;
		TFunction<void(const FCrowdyModScriptResult&)> OnDone;
		TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> Payload;
	};

	class FWorker final : public FRunnable
	{
	public:
		explicit FWorker(FCrowdyModScriptWorkerPoolState& InPool)
			: Pool(InPool)
		{
		}

		virtual uint32 Run() override
		{
			while (!Pool.bStopping)
			{
				FJob Job;
				if (!Pool.TryPop(Job))
				{
					Pool.WorkReady->Wait(WorkerIdleWaitMs);
					continue;
				}
				if (!Pool.AcquireSlot())
				{
					Pool.Finish(Job, Cancelled());
					continue;
				}
				SlotHolder = &Pool;
				const TConstArrayView<uint8> Payload = Job.Payload.IsValid() ? TConstArrayView<uint8>(*Job.Payload) : TConstArrayView<uint8>();
				const FCrowdyModScriptResult Result = Job.Instance->Dispatch(Job.Export, Job.Fuel, Job.Arg, Payload);
				if (SlotHolder == &Pool)
				{
					SlotHolder = nullptr;
					Pool.ReleaseSlot();
				}
				Pool.Finish(Job, Result);
			}
			return 0;
		}

	private:
		FCrowdyModScriptWorkerPoolState& Pool;
	};

	static FCrowdyModScriptResult Cancelled()
	{
		FCrowdyModScriptResult Result;
		Result.Outcome = ECrowdyModScriptOutcome::Trap;
		Result.Message = TEXT("the worker pool shut down before the dispatch ran");
		return Result;
	}

	// Blocks until one of the running slots is free; false once the pool is stopping.
	bool AcquireSlot()
	{
		while (!bStopping)
		{
			{
				FScopeLock Lock(&SlotLock);
				if (FreeSlots > 0)
				{
					--FreeSlots;
					return true;
				}
			}
			SlotFreed->Wait(WorkerIdleWaitMs);
		}
		return false;
	}

	void ReleaseSlot()
	{
		{
			FScopeLock Lock(&SlotLock);
			++FreeSlots;
		}
		SlotFreed->Trigger();
	}

	FCriticalSection QueueLock;
	FCriticalSection SlotLock;
	TArray<FJob> Queue;
	FEvent* WorkReady = nullptr;
	FEvent* SlotFreed = nullptr;
	int32 FreeSlots = 0;
	std::atomic<bool> bStopping{false};
	std::atomic<int64> Skipped{0};
	TArray<TUniquePtr<FWorker>> Workers;
	TArray<FRunnableThread*> Threads;

	bool TryPop(FJob& Out)
	{
		FScopeLock Lock(&QueueLock);
		if (Queue.IsEmpty())
		{
			return false;
		}
		Out = MoveTemp(Queue[0]);
		Queue.RemoveAt(0);
		return true;
	}

	// The claim is released before OnDone so the callback may queue the next dispatch.
	void Finish(FJob& Job, const FCrowdyModScriptResult& Result)
	{
		Job.Instance->bPoolClaimed = false;
		if (Job.OnDone)
		{
			Job.OnDone(Result);
		}
	}
};

FCrowdyModScriptWorkerPool::FCrowdyModScriptWorkerPool(int32 NumWorkers)
	: State(MakeUnique<FCrowdyModScriptWorkerPoolState>())
{
	State->WorkReady = FPlatformProcess::GetSynchEventFromPool(false);
	State->SlotFreed = FPlatformProcess::GetSynchEventFromPool(false);
	State->FreeSlots = FMath::Clamp(NumWorkers, 1, MaxWorkers);
	const int32 Count = State->FreeSlots + MaxWaitingWorkers;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		TUniquePtr<FCrowdyModScriptWorkerPoolState::FWorker>& Worker = State->Workers.Emplace_GetRef(MakeUnique<FCrowdyModScriptWorkerPoolState::FWorker>(*State));
		const FString Name = FString::Printf(TEXT("CrowdyModScriptWorker%d"), Index);
		if (FRunnableThread* Thread = FRunnableThread::Create(Worker.Get(), *Name, WorkerStackBytes, TPri_BelowNormal))
		{
			State->Threads.Add(Thread);
			continue;
		}
		UE_LOG(LogCrowdyModScripts, Warning, TEXT("Mod Script worker %d could not start"), Index);
	}
}

FCrowdyModScriptWorkerPool::~FCrowdyModScriptWorkerPool()
{
	State->bStopping = true;
	for (FRunnableThread* Thread : State->Threads)
	{
		State->WorkReady->Trigger();
		State->SlotFreed->Trigger();
		Thread->Kill(true);
		delete Thread;
	}

	TArray<FCrowdyModScriptWorkerPoolState::FJob> Pending;
	{
		FScopeLock Lock(&State->QueueLock);
		Pending = MoveTemp(State->Queue);
	}
	for (FCrowdyModScriptWorkerPoolState::FJob& Job : Pending)
	{
		State->Finish(Job, FCrowdyModScriptWorkerPoolState::Cancelled());
	}
	FPlatformProcess::ReturnSynchEventToPool(State->WorkReady);
	FPlatformProcess::ReturnSynchEventToPool(State->SlotFreed);
}

void FCrowdyModScriptWorkerPool::WaitOffSlot(TFunctionRef<void()> Wait)
{
	FCrowdyModScriptWorkerPoolState* const Pool = SlotHolder;
	if (!Pool)
	{
		Wait();
		return;
	}
	SlotHolder = nullptr;
	Pool->ReleaseSlot();
	Wait();
	// A stopping pool hands back no slot; the guest ends with its dispatch either way.
	if (Pool->AcquireSlot())
	{
		SlotHolder = Pool;
	}
}

bool FCrowdyModScriptWorkerPool::TryQueue(const TSharedRef<FCrowdyModScriptInstance>& Instance, ECrowdyModScriptExport Export, int64 FuelPerDispatch, int32 Arg,
	TFunction<void(const FCrowdyModScriptResult&)> OnDone, TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> Payload)
{
	bool bWasClaimed = false;
	if (State->Threads.IsEmpty() || !Instance->bPoolClaimed.compare_exchange_strong(bWasClaimed, true))
	{
		++Instance->SkippedDispatches;
		++State->Skipped;
		return false;
	}
	{
		FScopeLock Lock(&State->QueueLock);
		State->Queue.Add({Instance, Export, FuelPerDispatch, Arg, MoveTemp(OnDone), MoveTemp(Payload)});
	}
	State->WorkReady->Trigger();
	return true;
}

int64 FCrowdyModScriptWorkerPool::GetSkippedCount() const
{
	return State->Skipped.load();
}
