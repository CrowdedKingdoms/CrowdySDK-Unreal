#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Engine/GameInstance.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Subsystem/CrowdyWorkerThreadsSubsystem.h"
#include "UObject/StrongObjectPtr.h"

#include <atomic>

namespace
{
	constexpr EAutomationTestFlags CrowdyWorkerThreadsTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// The pool is a game instance subsystem, so it may only be constructed inside one. Building it against the
	// transient package instead is not merely untidy: the engine refuses it and the object comes back unusable.
	TStrongObjectPtr<UCrowdyWorkerThreadsSubsystem> MakePool(const TStrongObjectPtr<UGameInstance>& Owner)
	{
		return TStrongObjectPtr<UCrowdyWorkerThreadsSubsystem>(
			NewObject<UCrowdyWorkerThreadsSubsystem>(Owner.Get()));
	}
}

// The pool always has at least one worker, whatever the machine reports. A pool sized to zero accepts no
// tasks at all, and a caller that has already marked its shard as scheduled would then wait forever for work
// that was never queued, so the count is a correctness floor rather than a tuning knob.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWorkerThreadCountIsNeverZeroTest,
	"CrowdySDK.Net.WorkerThreadCountIsNeverZero", CrowdyWorkerThreadsTestFlags)
bool FCrowdyWorkerThreadCountIsNeverZeroTest::RunTest(const FString& Parameters)
{
	for (const int32 Cores : { 1, 2, 3, 4, 8, 16 })
	{
		const int32 Count = UCrowdyWorkerThreadsSubsystem::ComputeWorkerThreadCount(Cores);
		TestTrue(*FString::Printf(TEXT("%d cores gives at least one worker"), Cores), Count >= 1);
	}

	// A machine that reports nonsense must not produce a negative count either.
	TestTrue(TEXT("zero reported cores still gives at least one worker"),
		UCrowdyWorkerThreadsSubsystem::ComputeWorkerThreadCount(0) >= 1);
	TestTrue(TEXT("a negative core report still gives at least one worker"),
		UCrowdyWorkerThreadsSubsystem::ComputeWorkerThreadCount(-4) >= 1);

	// The exact count where the floor first has anything to do: cores minus reserved is zero here, so the answer
	// is the floor itself rather than what the subtraction gives.
	TestEqual(TEXT("the first count the floor produces is one"),
		UCrowdyWorkerThreadsSubsystem::ComputeWorkerThreadCount(
			UCrowdyWorkerThreadsSubsystem::ReservedCoreCount), 1);

	return true;
}

// Above the floor the count is unchanged: cores minus the ones reserved for the game and render threads. Every
// case here sits strictly above the floor, so this stays green whatever the floor does and is the control that
// says the floor did not shift the ordinary answer.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWorkerThreadCountLeavesReservedCoresTest,
	"CrowdySDK.Net.WorkerThreadCountLeavesReservedCores", CrowdyWorkerThreadsTestFlags)
bool FCrowdyWorkerThreadCountLeavesReservedCoresTest::RunTest(const FString& Parameters)
{
	constexpr int32 Reserved = UCrowdyWorkerThreadsSubsystem::ReservedCoreCount;

	for (const int32 Cores : { 5, 8, 16, 64 })
	{
		TestEqual(*FString::Printf(TEXT("%d cores leaves the reserved cores free"), Cores),
			UCrowdyWorkerThreadsSubsystem::ComputeWorkerThreadCount(Cores), Cores - Reserved);
	}

	// The lowest core count that still subtracts to more than one worker, so the subtraction is what answers here
	// and not the floor.
	TestEqual(TEXT("two above the reserved cores gives two workers"),
		UCrowdyWorkerThreadsSubsystem::ComputeWorkerThreadCount(Reserved + 2), 2);

	return true;
}

// Work handed to a pool that has no workers is not queued and never runs, and the caller is told so. Callers
// reserve something for a task to work on before handing it over, so a refusal they cannot see leaves that
// reservation held for a task that does not exist. A pool has no workers before it is created and after it is
// stopped, and teardown order between subsystems is not fixed, so the second case is reachable in a running game.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEnqueueTasksRefusesWithoutWorkersTest,
	"CrowdySDK.Net.EnqueueTasksRefusesWithoutWorkers", CrowdyWorkerThreadsTestFlags)
bool FCrowdyEnqueueTasksRefusesWithoutWorkersTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UGameInstance> Owner(NewObject<UGameInstance>(GetTransientPackage()));
	const TStrongObjectPtr<UCrowdyWorkerThreadsSubsystem> Pool = MakePool(Owner);

	if (!TestTrue(TEXT("a pool was created"), Pool.IsValid()))
	{
		return false;
	}

	bool bTaskRan = false;
	TArray<TFunction<void()>> Tasks;
	Tasks.Add([&bTaskRan]() { bTaskRan = true; });

	TestFalse(TEXT("a pool with no workers refuses the work"), Pool->EnqueueTasks(Tasks));
	TestFalse(TEXT("and nothing ran"), bTaskRan);

	// Nothing to queue is also nothing accepted, so a caller that reserved something releases it again rather
	// than holding it for a task it never actually handed over.
	const TArray<TFunction<void()>> Nothing;
	TestFalse(TEXT("an empty list is not accepted either"), Pool->EnqueueTasks(Nothing));

	return true;
}

// The control for the case above: a running pool accepts the work, so a refusal means what it says rather than
// being the only answer the pool ever gives. The two answers this asserts are read straight off the call and owe
// nothing to thread scheduling, which is what makes them a usable control.
//
// The third assertion, that the accepted work actually ran, is the one that says acceptance means the task really
// reached a worker rather than being reported as taken and dropped. It cannot be observed without letting a real
// worker thread run, so it waits, on an event the task itself signals rather than by polling, and gives up after
// a bound instead of hanging the run. A failure of that one assertion alone, with the two acceptance answers
// still correct, is a machine too busy to have scheduled a thread in ten seconds before it is anything else.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEnqueueTasksAcceptsWhenPoolIsRunningTest,
	"CrowdySDK.Net.EnqueueTasksAcceptsWhenPoolIsRunning", CrowdyWorkerThreadsTestFlags)
bool FCrowdyEnqueueTasksAcceptsWhenPoolIsRunningTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UGameInstance> Owner(NewObject<UGameInstance>(GetTransientPackage()));
	const TStrongObjectPtr<UCrowdyWorkerThreadsSubsystem> Pool = MakePool(Owner);

	if (!TestTrue(TEXT("a pool was created"), Pool.IsValid()))
	{
		return false;
	}

	Pool->InitializeWorkerThreadPool();

	// Both have to outlive anything the pool might still run, so the event is destroyed only after the workers
	// have been joined below. Manual reset, so a task that gets there first is not missed.
	std::atomic<bool> bTaskRan { false };
	FEvent* TaskRan = FPlatformProcess::GetSynchEventFromPool(true);

	TArray<TFunction<void()>> Tasks;
	Tasks.Add([&bTaskRan, TaskRan]()
	{
		bTaskRan.store(true, std::memory_order_release);
		TaskRan->Trigger();
	});

	TestTrue(TEXT("a running pool accepts the work"), Pool->EnqueueTasks(Tasks));

	TaskRan->Wait(10000);
	TestTrue(TEXT("and the work ran"), bTaskRan.load(std::memory_order_acquire));

	// Stopping the pool joins the workers, so nothing is left holding the captures above.
	Pool->StopAllProcesses();

	TestFalse(TEXT("a stopped pool refuses further work"), Pool->EnqueueTasks(Tasks));

	FPlatformProcess::ReturnSynchEventToPool(TaskRan);

	return true;
}

#endif
