// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Replication/Subsystems/CrowdyShardScheduling.h"
#include "Subsystem/CrowdyWorkerThreadsSubsystem.h"

#include <atomic>

// Inbound actor updates are spread across shards and each shard is read by one consumer at a time. The rules for
// handing a shard out are exercised here rather than through a live tracker: the case that matters is an update
// arriving in the moment between a consumer's last look at its queue and it letting the shard go, and that is a
// race no test could land on reliably from the outside. Driving the rules directly makes it an ordinary sequence
// of calls, with the update arriving from inside the callback that looks at the queue.
namespace
{
	constexpr EAutomationTestFlags CrowdyShardSchedulingTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	using CrowdyShardScheduling::EShardState;
	using CrowdyShardScheduling::FShardState;

	// Stands in for the worker pool. Counts the consumers that were asked for, and can be told to refuse, which is
	// what a pool with no workers does.
	struct FRecordingScheduler
	{
		int32 Count = 0;
		bool bAccepts = true;

		bool operator()()
		{
			++Count;
			return bAccepts;
		}
	};

	// Stands in for signalling a consumer that may be asleep on its batch window. Counting the signals is the
	// point: one per batch window is the intent, and one per item arriving is the cost that was measured at
	// roughly forty percent of everything receiving an update costs.
	struct FRecordingWake
	{
		int32 Count = 0;

		void operator()()
		{
			++Count;
		}
	};

	const TCHAR* StateName(const EShardState State)
	{
		switch (State)
		{
		case EShardState::Idle:             return TEXT("Idle");
		case EShardState::Taken:            return TEXT("Taken");
		default:                            return TEXT("TakenWithNewWork");
		}
	}
}

// An update that arrives after a consumer has looked at its queue for the last time, but before that consumer has
// let the shard go, is still processed. Without this the update sat on the queue until some other actor happened
// to hash to the same shard, so an actor that had stopped moving was left showing its second to last update, and
// the last actor active in an area was left showing nothing new at all.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyShardPicksUpWorkFromTheHandbackGapTest,
	"CrowdySDK.Replication.ShardPicksUpWorkThatArrivedDuringHandback", CrowdyShardSchedulingTestFlags)
bool FCrowdyShardPicksUpWorkFromTheHandbackGapTest::RunTest(const FString& Parameters)
{
	FShardState State { EShardState::Idle };
	FRecordingScheduler Scheduler;
	FRecordingWake Wake;

	// An update lands on a free shard, so a consumer is asked for and the shard is now taken.
	TestTrue(TEXT("the first update schedules a consumer"),
		CrowdyShardScheduling::ClaimShardForWork(State,
			[&Scheduler]() { return Scheduler(); }, [&Wake]() { Wake(); }));
	TestEqual(TEXT("one consumer was scheduled"), Scheduler.Count, 1);
	TestEqual(TEXT("the shard is taken"), StateName(State.load()), TEXT("Taken"));
	TestEqual(TEXT("a shard that was free is not signalled, since its consumer reads from the start"),
		Wake.Count, 0);

	// That consumer has read everything and is giving the shard back. It looks at the queue once, finds nothing,
	// and an update lands right there, before the shard is free again. Whoever added it can only see a shard that
	// is already taken, so it schedules nothing of its own.
	int32 LookCount = 0;
	bool bQueueHasWork = false;

	const auto LookWithLateArrival = [&]()
	{
		++LookCount;

		if (LookCount == 1)
		{
			bQueueHasWork = true;
			const bool bProducerScheduled = CrowdyShardScheduling::ClaimShardForWork(State,
				[&Scheduler]() { return Scheduler(); }, [&Wake]() { Wake(); });

			TestFalse(TEXT("the update that arrived during the handback schedules nothing itself"), bProducerScheduled);
			TestEqual(TEXT("but it does signal the consumer that still holds the shard"), Wake.Count, 1);
			return false;
		}

		return bQueueHasWork;
	};

	TestTrue(TEXT("the consumer picks the late update up instead of leaving it"),
		CrowdyShardScheduling::ReleaseShard(State, LookWithLateArrival, [&Scheduler]() { return Scheduler(); }));

	TestEqual(TEXT("the consumer looked again after being told there was more"), LookCount, 2);
	TestEqual(TEXT("exactly one further consumer was scheduled"), Scheduler.Count, 2);
	TestEqual(TEXT("the shard is taken by that consumer"), StateName(State.load()), TEXT("Taken"));

	return true;
}

// The ordinary two cases either side of that one: work still on the queue when the consumer looks is handed
// straight on, and a shard with nothing left on it goes free instead of scheduling a consumer with nothing to do.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyShardHandbackWithAndWithoutWorkTest,
	"CrowdySDK.Replication.ShardHandbackSchedulesOnlyWhenWorkRemains", CrowdyShardSchedulingTestFlags)
bool FCrowdyShardHandbackWithAndWithoutWorkTest::RunTest(const FString& Parameters)
{
	FShardState State { EShardState::Taken };
	FRecordingScheduler Scheduler;
	FRecordingWake Wake;

	TestTrue(TEXT("work still on the queue is handed to a further consumer"),
		CrowdyShardScheduling::ReleaseShard(State,
			[]() { return true; },
			[&Scheduler]() { return Scheduler(); }));
	TestEqual(TEXT("one further consumer"), Scheduler.Count, 1);
	TestEqual(TEXT("the shard is still taken"), StateName(State.load()), TEXT("Taken"));

	TestFalse(TEXT("an empty shard is not handed on"),
		CrowdyShardScheduling::ReleaseShard(State,
			[]() { return false; },
			[&Scheduler]() { return Scheduler(); }));
	TestEqual(TEXT("no further consumer"), Scheduler.Count, 1);
	TestEqual(TEXT("the shard is free"), StateName(State.load()), TEXT("Idle"));

	// And it is usable again afterwards, so neither path left it stuck.
	TestTrue(TEXT("a later update schedules normally"),
		CrowdyShardScheduling::ClaimShardForWork(State,
			[&Scheduler]() { return Scheduler(); }, [&Wake]() { Wake(); }));
	TestEqual(TEXT("a second consumer was scheduled"), Scheduler.Count, 2);
	TestEqual(TEXT("and nothing was signalled, since both claims found the shard free"), Wake.Count, 0);

	return true;
}

// A shard has one reader at a time. Updates arriving while a consumer has it are recorded against the shard, not
// given a consumer of their own: two readers on one queue is not a slow path, it is a crash, since taking an item
// off frees the node another reader is looking at.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyShardHasOneConsumerAtATimeTest,
	"CrowdySDK.Replication.ShardHasOneConsumerAtATime", CrowdyShardSchedulingTestFlags)
bool FCrowdyShardHasOneConsumerAtATimeTest::RunTest(const FString& Parameters)
{
	FShardState State { EShardState::Idle };
	FRecordingScheduler Scheduler;
	FRecordingWake Wake;

	TestTrue(TEXT("the first update schedules a consumer"),
		CrowdyShardScheduling::ClaimShardForWork(State,
			[&Scheduler]() { return Scheduler(); }, [&Wake]() { Wake(); }));

	for (int32 Attempt = 0; Attempt < 5; ++Attempt)
	{
		TestFalse(TEXT("a further update does not schedule a second consumer"),
			CrowdyShardScheduling::ClaimShardForWork(State,
				[&Scheduler]() { return Scheduler(); }, [&Wake]() { Wake(); }));
	}

	TestEqual(TEXT("still exactly one consumer"), Scheduler.Count, 1);
	TestEqual(TEXT("and the shard records that more arrived"),
		StateName(State.load()), TEXT("TakenWithNewWork"));

	// Six updates, one signal. The consumer is told once that more has arrived and does not finish without
	// looking again, so telling it a second time changes nothing it was going to do and costs a scheduler wake
	// on the thread that receives. This count is the whole point of the mark existing.
	TestEqual(TEXT("the consumer is signalled once, not once per update"), Wake.Count, 1);

	return true;
}

// Work the pool refuses leaves the shard free, so a later update can try again. A pool with no workers, or one
// that has already been stopped, accepts nothing, and a shard held for a consumer that was never created is a
// shard that never runs again: the updates queued behind it pile up and the actors they belong to stop moving.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRefusedShardScheduleLeavesShardFreeTest,
	"CrowdySDK.Replication.RefusedShardScheduleLeavesShardFree", CrowdyShardSchedulingTestFlags)
bool FCrowdyRefusedShardScheduleLeavesShardFreeTest::RunTest(const FString& Parameters)
{
	FShardState State { EShardState::Idle };
	FRecordingScheduler Scheduler;
	FRecordingWake Wake;
	Scheduler.bAccepts = false;

	TestFalse(TEXT("a refused update reports that nothing was scheduled"),
		CrowdyShardScheduling::ClaimShardForWork(State,
			[&Scheduler]() { return Scheduler(); }, [&Wake]() { Wake(); }));
	TestEqual(TEXT("the pool was asked once"), Scheduler.Count, 1);
	TestEqual(TEXT("the shard is left free after a refusal"), StateName(State.load()), TEXT("Idle"));

	// The refusal must not be terminal: once the pool is willing again, a later update is processed.
	Scheduler.bAccepts = true;
	TestTrue(TEXT("a later update schedules a consumer"),
		CrowdyShardScheduling::ClaimShardForWork(State,
			[&Scheduler]() { return Scheduler(); }, [&Wake]() { Wake(); }));
	TestEqual(TEXT("the pool was asked again"), Scheduler.Count, 2);
	TestEqual(TEXT("the shard is taken"), StateName(State.load()), TEXT("Taken"));

	// The same has to hold where a consumer hands the shard straight on to its successor.
	Scheduler.bAccepts = false;
	TestFalse(TEXT("a refused hand-on reports that nothing was scheduled"),
		CrowdyShardScheduling::ReleaseShard(State,
			[]() { return true; },
			[&Scheduler]() { return Scheduler(); }));
	TestEqual(TEXT("the shard is free after a refused hand-on"), StateName(State.load()), TEXT("Idle"));

	Scheduler.bAccepts = true;
	TestTrue(TEXT("and the shard can be taken again"),
		CrowdyShardScheduling::ClaimShardForWork(State,
			[&Scheduler]() { return Scheduler(); }, [&Wake]() { Wake(); }));

	// Every claim here found the shard free, and a free shard has no consumer that could be asleep on it. A
	// signal left standing would be cleared by the next consumer's first wait, turning it into a spin.
	TestEqual(TEXT("a refusal never leaves a signal behind"), Wake.Count, 0);

	return true;
}

// The shard count comes from the core count and nothing else. It has to be at least one, since every update is
// addressed to its hash modulo this number, and asking again must never grow it, since the queues are built once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyShardCountIsStableAndAtLeastOneTest,
	"CrowdySDK.Replication.ShardCountIsStableAndAtLeastOne", CrowdyShardSchedulingTestFlags)
bool FCrowdyShardCountIsStableAndAtLeastOneTest::RunTest(const FString& Parameters)
{
	constexpr int32 Reserved = UCrowdyWorkerThreadsSubsystem::ReservedCoreCount;

	for (const int32 Cores : { 1, 2, 3, 4, 8, 16, 64 })
	{
		TestTrue(*FString::Printf(TEXT("%d cores gives at least one shard"), Cores),
			CrowdyShardScheduling::ComputeShardCount(Cores) >= 1);
	}

	// A machine that reports nonsense must not produce a zero or negative count either: the count is a divisor on
	// the enqueue path and an array size everywhere else.
	TestTrue(TEXT("zero reported cores still gives at least one shard"),
		CrowdyShardScheduling::ComputeShardCount(0) >= 1);
	TestTrue(TEXT("a negative core report still gives at least one shard"),
		CrowdyShardScheduling::ComputeShardCount(-4) >= 1);

	// Descending on purpose. A count that kept the largest answer it had ever given would still be answering for
	// 64 cores here, so each of these would stop matching its own core count.
	for (const int32 Cores : { 64, 16, 8, 4, 2, 1 })
	{
		TestEqual(*FString::Printf(TEXT("%d cores answers from that core count alone"), Cores),
			CrowdyShardScheduling::ComputeShardCount(Cores), FMath::Max(1, Cores - Reserved));
	}

	// The shard count is the worker count and not a number of its own. Shards past that buy no parallelism, since
	// a shard is only ever drained by a pool worker: the extra shard just adds a consumer that waits out another
	// shard's batch window before it can start. It does not follow that each shard gets a worker to itself. The
	// pool hands work out on one round robin counter shared with every other caller in the process, so which
	// worker a consumer lands on is not fixed and two of them can share one while another sits idle.
	for (const int32 Cores : { 1, 4, 16 })
	{
		TestEqual(*FString::Printf(TEXT("%d cores gives as many shards as the pool has workers"), Cores),
			CrowdyShardScheduling::ComputeShardCount(Cores),
			UCrowdyWorkerThreadsSubsystem::ComputeWorkerThreadCount(Cores));
	}

	return true;
}

// A consumer gathers updates for a short window before passing them on. A full batch goes out immediately, so a
// busy shard is never held back by the rest of its window, and a window that ends with nothing in hand is dropped
// rather than dispatched as an empty batch.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyBatchIsDispatchedAsSoonAsItIsFullTest,
	"CrowdySDK.Replication.BatchIsDispatchedAsSoonAsItIsFull", CrowdyShardSchedulingTestFlags)
bool FCrowdyBatchIsDispatchedAsSoonAsItIsFullTest::RunTest(const FString& Parameters)
{
	const auto StepName = [](const CrowdyShardScheduling::EBatchStep Step) -> const TCHAR*
	{
		switch (Step)
		{
		case CrowdyShardScheduling::EBatchStep::Dispatch: return TEXT("Dispatch");
		case CrowdyShardScheduling::EBatchStep::Abandon:  return TEXT("Abandon");
		default:                                          return TEXT("Wait");
		}
	};

	// Full, with almost the whole window still to run.
	TestEqual(TEXT("a full batch is dispatched without waiting out the window"),
		StepName(CrowdyShardScheduling::DecideBatchStep(64, 64, 5.0)), TEXT("Dispatch"));
	TestEqual(TEXT("an over-full batch is dispatched too"),
		StepName(CrowdyShardScheduling::DecideBatchStep(65, 64, 5.0)), TEXT("Dispatch"));
	TestEqual(TEXT("a batch limit of one dispatches on the first update"),
		StepName(CrowdyShardScheduling::DecideBatchStep(1, 1, 5.0)), TEXT("Dispatch"));

	// Not full, and there is time left.
	TestEqual(TEXT("a partial batch waits while the window is open"),
		StepName(CrowdyShardScheduling::DecideBatchStep(1, 64, 0.001)), TEXT("Wait"));
	TestEqual(TEXT("an empty batch waits while the window is open"),
		StepName(CrowdyShardScheduling::DecideBatchStep(0, 64, 0.001)), TEXT("Wait"));

	// The window is over.
	TestEqual(TEXT("a partial batch is dispatched at the end of the window"),
		StepName(CrowdyShardScheduling::DecideBatchStep(1, 64, 0.0)), TEXT("Dispatch"));
	TestEqual(TEXT("an empty batch is abandoned at the end of the window"),
		StepName(CrowdyShardScheduling::DecideBatchStep(0, 64, 0.0)), TEXT("Abandon"));
	TestEqual(TEXT("an empty batch is abandoned past the end of the window"),
		StepName(CrowdyShardScheduling::DecideBatchStep(0, 64, -0.5)), TEXT("Abandon"));

	return true;
}

// A batch limit of zero or less makes every batch look full while none of them can gather anything, so a consumer
// would dispatch an empty batch, find its queue untouched and still occupied, and create a successor that does
// exactly the same. That is a worker pinned for the rest of the session with no update ever delivered. Having
// something in hand is a precondition of finishing, so a nonsense limit costs an idle window rather than a core.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEmptyBatchIsNeverDispatchedTest,
	"CrowdySDK.Replication.EmptyBatchIsNeverDispatched", CrowdyShardSchedulingTestFlags)
bool FCrowdyEmptyBatchIsNeverDispatchedTest::RunTest(const FString& Parameters)
{
	const auto StepName = [](const CrowdyShardScheduling::EBatchStep Step) -> const TCHAR*
	{
		switch (Step)
		{
		case CrowdyShardScheduling::EBatchStep::Dispatch: return TEXT("Dispatch");
		case CrowdyShardScheduling::EBatchStep::Abandon:  return TEXT("Abandon");
		default:                                          return TEXT("Wait");
		}
	};

	for (const int32 Limit : { 0, -1, -64 })
	{
		TestEqual(*FString::Printf(TEXT("a limit of %d waits out its window rather than dispatching nothing"), Limit),
			StepName(CrowdyShardScheduling::DecideBatchStep(0, Limit, 0.002)), TEXT("Wait"));

		TestEqual(*FString::Printf(TEXT("a limit of %d ends the window empty handed rather than dispatching"), Limit),
			StepName(CrowdyShardScheduling::DecideBatchStep(0, Limit, 0.0)), TEXT("Abandon"));
	}

	// The ordinary limits are untouched by the precondition: the batch is empty in each of these too, and the
	// answer is the same as it has always been.
	TestEqual(TEXT("an empty batch still waits while an ordinary window is open"),
		StepName(CrowdyShardScheduling::DecideBatchStep(0, 64, 0.002)), TEXT("Wait"));
	TestEqual(TEXT("an empty batch is still abandoned at the end of an ordinary window"),
		StepName(CrowdyShardScheduling::DecideBatchStep(0, 64, 0.0)), TEXT("Abandon"));

	// And a batch that did gather something under a nonsense limit is still delivered rather than thrown away.
	TestEqual(TEXT("work already gathered under a nonsense limit is dispatched"),
		StepName(CrowdyShardScheduling::DecideBatchStep(3, 0, 5.0)), TEXT("Dispatch"));

	return true;
}

// What a consumer does with the shard it was given, driven exactly as the tracker drives it. The tracker's task
// body is a call to this and nothing else, so the ordering rules below are the ones that actually run.
//
// The case that matters is work arriving while the consumer is finishing: whoever added it can only see a shard
// that is already taken, so it schedules nothing of its own and the departing consumer has to hand the shard on.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyShardConsumerHandsOnLateWorkTest,
	"CrowdySDK.Replication.ShardConsumerHandsOnWorkThatArrivedWhileItRan", CrowdyShardSchedulingTestFlags)
bool FCrowdyShardConsumerHandsOnLateWorkTest::RunTest(const FString& Parameters)
{
	FShardState State { EShardState::Taken };
	FRecordingScheduler Scheduler;

	bool bQueueHasWork = false;
	int32 ProcessCount = 0;
	int32 FinishCount = 0;

	CrowdyShardScheduling::RunShardConsumer(
		State,
		[&]()
		{
			++ProcessCount;
			// The update lands after this consumer has read everything, which is the whole point of the case.
			bQueueHasWork = true;
		},
		[&]() { return bQueueHasWork; },
		[&Scheduler]() { return Scheduler(); },
		[&]() { ++FinishCount; });

	TestEqual(TEXT("the shard was read once"), ProcessCount, 1);
	TestEqual(TEXT("exactly one successor was created for the late work"), Scheduler.Count, 1);
	TestEqual(TEXT("the shard belongs to that successor"), StateName(State.load()), TEXT("Taken"));
	TestEqual(TEXT("and this consumer reported itself finished"), FinishCount, 1);

	// Nothing left behind: a consumer that read an empty shard gives it back instead of creating a successor with
	// nothing to do, and the shard is free for whatever arrives next.
	FShardState Quiet { EShardState::Taken };
	FRecordingScheduler QuietScheduler;
	int32 QuietFinishCount = 0;

	CrowdyShardScheduling::RunShardConsumer(
		Quiet,
		[]() {},
		[]() { return false; },
		[&QuietScheduler]() { return QuietScheduler(); },
		[&]() { ++QuietFinishCount; });

	TestEqual(TEXT("no successor for an empty shard"), QuietScheduler.Count, 0);
	TestEqual(TEXT("the shard is free"), StateName(Quiet.load()), TEXT("Idle"));
	TestEqual(TEXT("and that consumer reported itself finished too"), QuietFinishCount, 1);

	return true;
}

// A successor exists before its predecessor stops existing. Shutdown reads the count of consumers and treats zero
// as nothing reading a shard, whereupon it drains the queues. A count that dropped to zero in the gap between one
// consumer leaving and the next being created would let that drain run against a live reader, and a queue that
// allows one reader does not survive two: taking an item off frees the node the other one is looking at.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyShardConsumerCountNeverDipsAtHandoverTest,
	"CrowdySDK.Replication.ShardConsumerCountNeverDipsAtHandover", CrowdyShardSchedulingTestFlags)
bool FCrowdyShardConsumerCountNeverDipsAtHandoverTest::RunTest(const FString& Parameters)
{
	FShardState State { EShardState::Taken };

	// One consumer already exists, which is what got us here.
	std::atomic<int32> ConsumersInFlight { 1 };
	int32 LowestObserved = MAX_int32;
	int32 CountWhenFinished = MAX_int32;

	CrowdyShardScheduling::RunShardConsumer(
		State,
		[]() {},
		[]() { return true; },
		[&]()
		{
			// Scheduling a successor is what the tracker does here, so the count is read on the way through.
			const bool bScheduled = CrowdyShardScheduling::ScheduleShardConsumer(
				ConsumersInFlight, []() { return true; });

			LowestObserved = FMath::Min(LowestObserved, ConsumersInFlight.load(std::memory_order_acquire));
			return bScheduled;
		},
		[&]()
		{
			ConsumersInFlight.fetch_sub(1, std::memory_order_acq_rel);
			CountWhenFinished = ConsumersInFlight.load(std::memory_order_acquire);
		});

	TestEqual(TEXT("the successor was counted in while its predecessor was still counted"), LowestObserved, 2);
	TestEqual(TEXT("and one consumer is left afterwards, the successor"), CountWhenFinished, 1);
	TestTrue(TEXT("the count never read zero"), LowestObserved > 0 && CountWhenFinished > 0);

	return true;
}

// A consumer the pool refuses is not a consumer. The count has to come back down, or shutdown waits out its whole
// bound for a reader that does not exist and then reports one, and the shard has to be left claimable, or the
// updates queued on it pile up and the actors they belong to stop moving for good.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRefusedShardConsumerIsNotCountedTest,
	"CrowdySDK.Replication.RefusedShardConsumerIsNotCounted", CrowdyShardSchedulingTestFlags)
bool FCrowdyRefusedShardConsumerIsNotCountedTest::RunTest(const FString& Parameters)
{
	std::atomic<int32> ConsumersInFlight { 0 };

	TestFalse(TEXT("a refused consumer reports that nothing was created"),
		CrowdyShardScheduling::ScheduleShardConsumer(ConsumersInFlight, []() { return false; }));
	TestEqual(TEXT("and is not counted"), ConsumersInFlight.load(), 0);

	TestTrue(TEXT("an accepted consumer reports that one was created"),
		CrowdyShardScheduling::ScheduleShardConsumer(ConsumersInFlight, []() { return true; }));
	TestEqual(TEXT("and is counted, until it says it has finished"), ConsumersInFlight.load(), 1);

	// The count goes up before the work is handed over, since a consumer can start and finish before the call
	// that created it returns. Reading it from inside the handover is the only way to see that ordering.
	std::atomic<int32> Ordered { 0 };
	int32 CountDuringHandover = 0;

	CrowdyShardScheduling::ScheduleShardConsumer(Ordered, [&]()
	{
		CountDuringHandover = Ordered.load(std::memory_order_acquire);
		return true;
	});

	TestEqual(TEXT("the consumer is already counted while the work is being handed over"), CountDuringHandover, 1);

	// A refusal at the point a consumer hands its shard on leaves the shard free rather than held for a consumer
	// that was never created, so a later update can try again.
	FShardState State { EShardState::Taken };
	FRecordingScheduler Scheduler;
	Scheduler.bAccepts = false;
	int32 FinishCount = 0;

	CrowdyShardScheduling::RunShardConsumer(
		State,
		[]() {},
		[]() { return true; },
		[&Scheduler]() { return Scheduler(); },
		[&]() { ++FinishCount; });

	TestEqual(TEXT("the pool was asked once"), Scheduler.Count, 1);
	TestEqual(TEXT("the shard is free rather than held for a consumer that does not exist"),
		StateName(State.load()), TEXT("Idle"));
	TestEqual(TEXT("and the departing consumer still reported itself finished"), FinishCount, 1);

	Scheduler.bAccepts = true;
	TestTrue(TEXT("a later update can claim the shard again"),
		CrowdyShardScheduling::ClaimShardForWork(State,
			[&Scheduler]() { return Scheduler(); }, []() {}));

	return true;
}

// A consumer is signalled on the one transition that can free it, and on no other. The three shard states are
// exercised directly here because each answers a different question: a free shard has nobody asleep on it, a
// consumer that has not yet been told needs telling, and a consumer that has already been told will look again
// before it finishes, so telling it twice wakes a thread for nothing.
//
// The last of those is not a tidiness point. Signalling per item was measured at roughly forty percent of
// everything receiving an update costs, because a consumer waiting out its batch window is genuinely asleep and
// every signal pays for a scheduler wake on the thread that receives.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyShardIsSignalledOncePerBatchWindowTest,
	"CrowdySDK.Replication.ShardIsSignalledOncePerBatchWindow", CrowdyShardSchedulingTestFlags)
bool FCrowdyShardIsSignalledOncePerBatchWindowTest::RunTest(const FString& Parameters)
{
	FRecordingScheduler Scheduler;
	FRecordingWake Wake;

	const auto Claim = [&](FShardState& State)
	{
		return CrowdyShardScheduling::ClaimShardForWork(State,
			[&Scheduler]() { return Scheduler(); }, [&Wake]() { Wake(); });
	};

	// A free shard has nobody asleep on it, and the consumer about to be created reads the queue from the start.
	// A signal left standing here would be cleared by that consumer's first real wait, turning it into a spin.
	FShardState Idle { EShardState::Idle };
	Claim(Idle);
	TestEqual(TEXT("a free shard is not signalled"), Wake.Count, 0);
	TestEqual(TEXT("it gets a consumer instead"), Scheduler.Count, 1);

	// A consumer holds this one and has not been told anything arrived since it last looked. It may be asleep on
	// its batch window, and this is the only signal that can bring it back.
	FShardState Taken { EShardState::Taken };
	Claim(Taken);
	TestEqual(TEXT("a consumer that has not yet been told is signalled"), Wake.Count, 1);
	TestEqual(TEXT("and no second consumer is created for a shard that already has one"), Scheduler.Count, 1);

	// Already marked. That consumer cannot finish without looking again, so it is coming back for this item with
	// or without a signal, and signalling costs a scheduler wake that changes nothing.
	FShardState Marked { EShardState::TakenWithNewWork };
	Claim(Marked);
	TestEqual(TEXT("a consumer that has already been told is not signalled again"), Wake.Count, 1);

	// And repeating it stays silent, which is the case that carries the cost: a busy shard sees this branch once
	// per item for the whole of a batch window.
	for (int32 Attempt = 0; Attempt < 10; ++Attempt)
	{
		Claim(Marked);
	}

	TestEqual(TEXT("ten further items on a marked shard signal nothing"), Wake.Count, 1);
	TestEqual(TEXT("and create no consumers"), Scheduler.Count, 1);

	return true;
}

// A consumer waiting for its batch to fill blocks for whole milliseconds. Part of one rounds up: rounding down
// gives a wait of zero, which returns straight away and turns the tail of every batch window into a spin on a
// worker thread. That spin is what the wait replaced.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyShardWaitNeverRoundsToZeroTest,
	"CrowdySDK.Replication.ShardWaitNeverRoundsToZero", CrowdyShardSchedulingTestFlags)
bool FCrowdyShardWaitNeverRoundsToZeroTest::RunTest(const FString& Parameters)
{
	// The default batch window is 2 ms, so most of what this ever sees is a fraction of a millisecond.
	for (const double Remaining : { 0.0000001, 0.0001, 0.0009, 0.001 })
	{
		TestEqual(*FString::Printf(TEXT("%g seconds left rounds up to a whole millisecond"), Remaining),
			static_cast<int32>(CrowdyShardScheduling::ShardWaitMilliseconds(Remaining)), 1);
	}

	TestEqual(TEXT("a window with time left waits for what is left"),
		static_cast<int32>(CrowdyShardScheduling::ShardWaitMilliseconds(0.002)), 2);
	TestEqual(TEXT("and part of a millisecond on top of that rounds up too"),
		static_cast<int32>(CrowdyShardScheduling::ShardWaitMilliseconds(0.0021)), 3);

	// A window that has already run out, or was configured as nonsense, still gives a wait that is in range and
	// not zero. A caller only asks when it has decided to keep waiting.
	TestEqual(TEXT("no time left still waits a millisecond rather than not at all"),
		static_cast<int32>(CrowdyShardScheduling::ShardWaitMilliseconds(0.0)), 1);
	TestEqual(TEXT("a negative window does not wrap round to a huge wait"),
		static_cast<int32>(CrowdyShardScheduling::ShardWaitMilliseconds(-5.0)), 1);
	TestTrue(TEXT("an absurd window stays inside the range a wait is expressed in"),
		CrowdyShardScheduling::ShardWaitMilliseconds(1.0e12) <= static_cast<uint32>(MAX_int32));

	return true;
}

#endif
