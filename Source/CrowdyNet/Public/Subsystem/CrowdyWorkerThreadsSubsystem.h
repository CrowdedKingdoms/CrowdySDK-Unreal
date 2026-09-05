// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Containers/Queue.h"
#include "CrowdyWorkerThreadsSubsystem.generated.h"


class FWorker;
class FRunnableThread;

/**
 * A pool of worker threads any system can hand short pieces of work to.
 *
 * The pool is created once, on the game instance, and the queues are filled round robin, so a caller gets
 * concurrency without owning a thread of its own.
 */
UCLASS()
class CROWDYNET_API UCrowdyWorkerThreadsSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()
public:

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Create the worker threads. Nothing can be enqueued until this has run. */
	void InitializeWorkerThreadPool();

	/**
	 * Hand tasks to the pool, to be run on whichever worker picks them up.
	 *
	 * @return true when every task was queued. False means none of them were and none of them will run, either
	 *         because the pool has no workers or because it has already been stopped. A caller that reserved
	 *         something for a task to work on has to release it again in that case, or it stays reserved for a
	 *         task that will never exist.
	 */
	bool EnqueueTasks(const TArray<TFunction<void()>>& Tasks);

	void StopAllProcesses();

	/**
	 * How many workers the pool creates on a machine with NumCores logical cores.
	 *
	 * The pool leaves cores for the game and render threads, but never sizes itself to nothing: a pool with
	 * no workers accepts no tasks, and callers that mark a shard as scheduled before handing it over would
	 * then wait forever for a task that was never queued. At least one worker always exists.
	 */
	static int32 ComputeWorkerThreadCount(int32 NumCores);

	/** Cores the game and render threads are left to, and so are not counted toward the pool. */
	static constexpr int32 ReservedCoreCount = 3;

private:

	TArray<FRunnableThread*> WorkerThreads;

	TArray<FWorker*> Workers;

	TArray<TQueue<TFunction<void()>, EQueueMode::Mpsc>*> TaskQueues;
	TArray<FEvent*> TaskEvents;

	void InitializeWorkerThreads();
};
