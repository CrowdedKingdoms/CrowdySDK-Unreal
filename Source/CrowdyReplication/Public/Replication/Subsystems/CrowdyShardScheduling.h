// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Subsystem/CrowdyWorkerThreadsSubsystem.h"
#include "Templates/Function.h"

#include <atomic>

/**
 * The rules a work shard is sized, handed out and batched by.
 *
 * Work is spread across shards by the thing it belongs to, so everything about one actor lands on the same queue
 * and stays in order. A shard is read by one consumer at a time and nothing here knows what a consumer does with
 * the work, which is what lets each rule be exercised on its own.
 */
namespace CrowdyShardScheduling
{
	/** Who, if anyone, is reading a shard, and whether anything has arrived since they last looked. */
	enum class EShardState : uint8
	{
		/** Nobody is reading it. Whoever adds work next hands it to a consumer. */
		Idle,

		/** A consumer has it. Nobody else may read the queue. */
		Taken,

		/** A consumer has it, and work arrived after that consumer had already looked at the queue. */
		TakenWithNewWork
	};

	using FShardState = std::atomic<EShardState>;

	/**
	 * How many shards work is spread across on a machine with NumCores logical cores.
	 *
	 * The answer depends only on the core count. Asking again must never grow it: the shard an item lands on is
	 * its hash modulo this number and every queue is created up front, so a number that could grow would leave
	 * work addressed to a queue that does not exist.
	 *
	 * A shard is only ever drained by a worker from the shared pool, so shards beyond the number of workers buy no
	 * parallelism. They only spread the same work over more queues, and each consumer occupies a worker for the
	 * length of its batch window, so the extra shard adds a consumer that waits out another shard's window before
	 * it can start. This does not give a shard a worker of its own: the pool picks a worker from one round robin
	 * counter shared with every other caller in the process, so two shard consumers can land on the same worker
	 * while another is idle.
	 */
	inline int32 ComputeShardCount(const int32 NumCores)
	{
		return UCrowdyWorkerThreadsSubsystem::ComputeWorkerThreadCount(NumCores);
	}

	/**
	 * Hand a shard the caller already owns to a consumer, and give it back if the work is refused.
	 *
	 * Schedule reports whether the work was actually accepted. A pool with no workers, or one that has already
	 * been stopped, accepts nothing, and a shard left marked as taken for a consumer that was never created is a
	 * shard nothing ever reads again. Both ways of handing a shard on come through here so they cannot drift.
	 *
	 * @return true when a consumer was scheduled and now owns the shard.
	 */
	inline bool ScheduleOwnedShard(FShardState& State, TFunctionRef<bool()> Schedule)
	{
		if (Schedule())
		{
			return true;
		}

		State.store(EShardState::Idle, std::memory_order_release);
		return false;
	}

	/**
	 * Take a shard and start a consumer on it, or tell the consumer that already has it that more has arrived.
	 *
	 * Called after the work itself has been queued. Only one caller can win an idle shard, which is what keeps a
	 * shard to a single reader. When a consumer already has it, the shard is marked instead: that consumer will
	 * not finish without looking again, so the work is never left waiting for some later, unrelated item to land
	 * on the same shard.
	 *
	 * Wake is called on exactly the transition that can free a stuck consumer: the first item to arrive after a
	 * consumer was last told to look again. Nothing clears that mark until the consumer hands the shard back, so
	 * every further item in the same batch window finds it already set and signals nothing. This is deliberate and
	 * it is where most of the receive path's cost used to go. Signalling per item is not merely redundant, it is
	 * the dominant cost of receiving an update, because a consumer waiting on its batch window is genuinely asleep
	 * and each signal pays for a scheduler wake. It also buys no latency: a batch is dispatched when it fills or
	 * when its window expires, and looking earlier brings neither forward, while a consumer that is signalled not
	 * at all still re-reads its queue within a millisecond because its wait is floored there.
	 *
	 * An idle shard is never woken. The consumer created for it reads the queue from the start, and a signal left
	 * standing would make its first real wait return immediately, which is the busy loop the wait exists to avoid.
	 *
	 * @return true when this call scheduled a consumer.
	 */
	inline bool ClaimShardForWork(FShardState& State, TFunctionRef<bool()> Schedule, TFunctionRef<void()> Wake)
	{
		EShardState Observed = State.load(std::memory_order_acquire);

		for (;;)
		{
			if (Observed == EShardState::TakenWithNewWork)
			{
				return false;
			}

			const EShardState Desired = (Observed == EShardState::Idle)
				? EShardState::Taken
				: EShardState::TakenWithNewWork;

			if (!State.compare_exchange_weak(Observed, Desired, std::memory_order_acq_rel, std::memory_order_acquire))
			{
				continue;
			}

			if (Desired == EShardState::TakenWithNewWork)
			{
				Wake();
				return false;
			}

			return ScheduleOwnedShard(State, Schedule);
		}
	}

	/**
	 * Give a shard back once its consumer has finished reading it, handing it straight on if anything is left.
	 *
	 * A consumer stops reading before it gives the shard back, so work can arrive in between. Whoever added it saw
	 * the shard as taken and scheduled nothing, so without this the work would wait for the next item that happens
	 * to land on the same shard, which for a source that has gone quiet may be never.
	 *
	 * The shard is never given back while there is work on it, and the mark left by a late arrival makes the
	 * handback fail, so there is no moment where work is queued and nobody is coming for it. The caller stays the
	 * owner throughout, which is what makes HasWork safe to ask: a shard queue may only be read by its owner, so a
	 * consumer that had already let go could not look without racing whoever took it next.
	 *
	 * @return true when a further consumer was scheduled.
	 */
	inline bool ReleaseShard(FShardState& State, TFunctionRef<bool()> HasWork, TFunctionRef<bool()> Schedule)
	{
		for (;;)
		{
			if (HasWork())
			{
				// Anything marked while we were looking is part of what the next consumer will read, so the mark
				// comes off rather than being passed on as work nobody has accounted for.
				State.store(EShardState::Taken, std::memory_order_release);
				return ScheduleOwnedShard(State, Schedule);
			}

			EShardState Observed = EShardState::Taken;
			if (State.compare_exchange_strong(Observed, EShardState::Idle, std::memory_order_acq_rel, std::memory_order_acquire))
			{
				return false;
			}

			// Work arrived between the look and the handback. Take the mark off and look again, still as the owner.
			State.store(EShardState::Taken, std::memory_order_release);
		}
	}

	/**
	 * Create a consumer for a shard, keeping the count of consumers that exist in step with it.
	 *
	 * The count goes up before the work is handed over, because a consumer can start and finish before the call
	 * that created it returns. Work that is refused never becomes a consumer, so the count comes back down: a
	 * count left standing describes a reader that does not exist, and shutdown waits on that count reaching zero
	 * before it touches a shard queue.
	 *
	 * The count deliberately follows what was accepted rather than what has started running. Counting from the
	 * start of a consumer would leave a gap between handing the work over and it beginning, and a count of zero
	 * read inside that gap says nothing is reading a shard while a consumer is on its way to doing exactly that.
	 *
	 * @return true when a consumer was created.
	 */
	inline bool ScheduleShardConsumer(std::atomic<int32>& ConsumersInFlight, TFunctionRef<bool()> Enqueue)
	{
		ConsumersInFlight.fetch_add(1, std::memory_order_acq_rel);

		if (Enqueue())
		{
			return true;
		}

		ConsumersInFlight.fetch_sub(1, std::memory_order_acq_rel);
		return false;
	}

	/**
	 * Everything a consumer does with the shard it was given, in the order it has to happen in.
	 *
	 * Process reads the shard. The shard is then given back, which hands it straight on if anything arrived while
	 * Process was finishing. OnFinished is what records that this consumer is gone, and it runs last on purpose:
	 * a successor is counted in before its predecessor drops out, so there is no moment where a shard is being
	 * read and the count says otherwise.
	 */
	inline void RunShardConsumer(FShardState& State, TFunctionRef<void()> Process, TFunctionRef<bool()> HasWork,
		TFunctionRef<bool()> Schedule, TFunctionRef<void()> OnFinished)
	{
		Process();

		ReleaseShard(State, HasWork, Schedule);

		OnFinished();
	}

	/**
	 * How long a consumer blocks for, in whole milliseconds, while it waits for its batch to fill.
	 *
	 * Waits are whole milliseconds, so part of one is rounded up rather than down. Rounding down returns straight
	 * away and turns the tail of the window into a busy loop, which is the thing waiting exists to avoid. The
	 * upper clamp only matters if the configured window is nonsense; it keeps the conversion in range.
	 */
	inline uint32 ShardWaitMilliseconds(const double RemainingSeconds)
	{
		const double Milliseconds = FMath::Clamp(RemainingSeconds * 1000.0, 1.0, static_cast<double>(MAX_int32));
		return static_cast<uint32>(FMath::CeilToInt32(Milliseconds));
	}

	/** What a consumer does next, having taken everything currently on its shard. */
	enum class EBatchStep : uint8
	{
		/** Enough has been gathered, or the window is over with something in hand. Send the batch on. */
		Dispatch,

		/** The window is over and nothing arrived. There is nothing to send. */
		Abandon,

		/** The window has time left. Wait for more before deciding again. */
		Wait
	};

	/**
	 * Whether a batch is finished, empty, or still worth waiting on.
	 *
	 * A full batch is finished whatever time is left, so a busy shard never sits out the rest of its window. An
	 * empty one at the end of the window is abandoned rather than dispatched, so consumers that were woken with
	 * nothing to do stay silent.
	 *
	 * Having something in hand is a precondition of finishing, checked before the size limit rather than after it.
	 * A limit of zero or less makes every batch look full while none of them can ever gather anything, and a
	 * consumer that dispatches nothing and immediately finds its queue still occupied creates a successor that
	 * does exactly the same, which occupies a worker for good. The limit is clamped where it is set, so this is
	 * the second line of defence rather than the first, but it belongs here too: the rule is that no progress
	 * means no dispatch, whatever the caller passed in.
	 */
	inline EBatchStep DecideBatchStep(const int32 BatchNum, const int32 MaxUpdatesPerBatch, const double RemainingSeconds)
	{
		if (BatchNum <= 0)
		{
			return RemainingSeconds > 0.0 ? EBatchStep::Wait : EBatchStep::Abandon;
		}

		if (BatchNum >= MaxUpdatesPerBatch)
		{
			return EBatchStep::Dispatch;
		}

		return RemainingSeconds > 0.0 ? EBatchStep::Wait : EBatchStep::Dispatch;
	}
}
