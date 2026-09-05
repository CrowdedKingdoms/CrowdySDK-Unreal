// Fill out your copyright notice in the Description page of Project Settings.


#include "Subsystem/CrowdyWorkerThreadsSubsystem.h"
#include "CrowdyNetLog.h"
#include <atomic>
#include "Threading/FWorker.h"

void UCrowdyWorkerThreadsSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
}

void UCrowdyWorkerThreadsSubsystem::Deinitialize()
{
	StopAllProcesses();

	Super::Deinitialize();
}

void UCrowdyWorkerThreadsSubsystem::InitializeWorkerThreadPool()
{
	UE_CLOG(CrowdyNetTrace::Net(), LogCrowdyNet, Log, TEXT("Worker Thread Subsystem Initialized."));

	InitializeWorkerThreads();
}


bool UCrowdyWorkerThreadsSubsystem::EnqueueTasks(const TArray<TFunction<void()>>& Tasks)
{
	// There are no workers before the pool is created and none after it is stopped, and teardown order between
	// subsystems is not fixed, so this is reachable while the rest of the game is still running.
	if (TaskQueues.Num() == 0)
		return false;

	if (Tasks.IsEmpty())
		return false;

	// Unsigned so that the round robin survives the counter wrapping. A signed counter goes negative there, and a
	// negative index runs off the front of the queue array.
	static std::atomic<uint32> QueueIndex{0};

	for (const auto& Task : Tasks)
	{
		const uint32 Index = QueueIndex.fetch_add(1);
		const int32 Slot = static_cast<int32>(Index % static_cast<uint32>(TaskQueues.Num()));
		TaskQueues[Slot]->Enqueue(Task);
		TaskEvents[Slot]->Trigger();
	}

	return true;
}

int32 UCrowdyWorkerThreadsSubsystem::ComputeWorkerThreadCount(const int32 NumCores)
{
	return FMath::Max(1, NumCores - ReservedCoreCount);
}

void UCrowdyWorkerThreadsSubsystem::InitializeWorkerThreads()
{
	const int32 NumOfCores = FPlatformMisc::NumberOfCores();
	const int32 NumOfThreads = ComputeWorkerThreadCount(NumOfCores);

	if (NumOfThreads > NumOfCores - ReservedCoreCount)
	{
		UE_LOG(LogCrowdyNet, Warning,
			TEXT("Only %d logical cores are available. Leaving %d for the game and render threads would size the ")
			TEXT("worker pool to nothing, so it is running %d thread(s) instead. Work that would normally run in ")
			TEXT("parallel is serialized on this machine."),
			NumOfCores, ReservedCoreCount, NumOfThreads);
	}

	for (int i = 0; i < NumOfThreads; i++)
	{
		// Create a separate queue and event for each worker
		TQueue<TFunction<void()>, EQueueMode::Mpsc>* Queue = new TQueue<TFunction<void()>, EQueueMode::Mpsc>();
		FEvent* Event = FPlatformProcess::GetSynchEventFromPool(false);

		TaskQueues.Add(Queue);
		TaskEvents.Add(Event);

		FWorker* Worker = new FWorker(*Queue, Event);
		FString ThreadName = FString::Printf(TEXT("WorkerThread_%i"), i);
		FRunnableThread* Thread = FRunnableThread::Create(Worker, *ThreadName, 0, TPri_AboveNormal);
		Workers.Add(Worker);
		WorkerThreads.Add(Thread);
	}


	UE_CLOG(CrowdyNetTrace::Net(), LogCrowdyNet, Log, TEXT("%d Worker Threads Created"), NumOfThreads);
}

void UCrowdyWorkerThreadsSubsystem::StopAllProcesses()
{
	for (FWorker* Worker : Workers)
	{
		Worker->Stop();
	}

	for (FRunnableThread* Thread : WorkerThreads)
	{
		if (Thread)
		{
			Thread->Kill(true);
			delete Thread;
		}
	}

	// Every thread has been joined above, so nothing is running inside a worker any more and they can go.
	for (FWorker* Worker : Workers)
	{
		delete Worker;
	}

	// Clean up queues and events
	for (const auto* Queue : TaskQueues)
	{
		delete Queue;
	}
	for (auto* Event : TaskEvents)
	{
		FPlatformProcess::ReturnSynchEventToPool(Event);
	}

	Workers.Empty();
	WorkerThreads.Empty();
	TaskQueues.Empty();
	TaskEvents.Empty();
}

