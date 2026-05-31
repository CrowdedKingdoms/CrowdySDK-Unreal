// Fill out your copyright notice in the Description page of Project Settings.


#include "Subsystem/CrowdyWorkerThreadsSubsystem.h"
#include "Internal/FCrowdyServiceRegistry.h"
#include "Network/UDP/CrowdyUDPSubsystem.h"
#include "Serialization/FCrowdyMessageParser.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Threading/FWorker.h"
#include "Utils/FMessageBufferPool.h"

void UCrowdyWorkerThreadsSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
}

void UCrowdyWorkerThreadsSubsystem::Deinitialize()
{
	bShouldRun = false;
	bProcessMainThread = false;
	
	// Wake up both blocked threads so they can see bProcessMainThread == false
	if (IsValid(GameSession))
	{
		if (FEvent* SE = GameSession->GetSendEvent())    SE->Trigger();
		if (FEvent* RE = GameSession->GetReceiveEvent()) RE->Trigger();
	}
	
	StopAllProcesses();
	
	GameSession   = nullptr;
	UDPSubsystem  = nullptr;
	
	Super::Deinitialize();
}

void UCrowdyWorkerThreadsSubsystem::InitializeWorkerThreadPool(FMessageBufferPool* InBufferPool,
                                                               FCrowdyMessageParser* InMessageParser,
                                                               FCrowdyServiceRegistry* InServiceRegistry, UCrowdyUDPSubsystem* InUdpSubsystem, UCrowdyGameSession* InGameSession)
{
	BufferPoolSubsystem = InBufferPool;
	NetworkMessageParser = InMessageParser;
	GameSession = InGameSession;
	UDPSubsystem = InUdpSubsystem;
	ServiceRegistry = InServiceRegistry;

	if (!IsValid(GameSession) || !IsValid(UDPSubsystem) || !BufferPoolSubsystem || !NetworkMessageParser || !
		ServiceRegistry)
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdySDK] WorkerThreadSubsystem::Some or one of the subsystem is invalid."));
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("[CrowdySDK] Worker Thread Subsystem Initialized."));

	InitializeWorkerThreads();
}


void UCrowdyWorkerThreadsSubsystem::EnqueueTasks(const TArray<TFunction<void()>>& Tasks)
{
	if (TaskQueues.Num() == 0)
		return;

	static int32 QueueIndex = 0;

	for (const auto& Task : Tasks)
	{
		TaskQueues[QueueIndex % TaskQueues.Num()]->Enqueue(Task);
		TaskEvents[QueueIndex % TaskEvents.Num()]->Trigger();
		QueueIndex++;
	}
}

void UCrowdyWorkerThreadsSubsystem::InitializeWorkerThreads()
{
	const int32 NumOfThreads = FPlatformMisc::NumberOfCores() - 3;

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


	UE_LOG(LogTemp, Log, TEXT("[CrowdySDK] %d Worker Threads Created"), NumOfThreads);

	bShouldRun = true;
	bProcessMainThread = true;

	RunSendLoop();
	RunReceiveLoop();
}

void UCrowdyWorkerThreadsSubsystem::StopAllProcesses()
{
	bProcessMainThread = false;

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

void UCrowdyWorkerThreadsSubsystem::RunSendLoop() const
{
	// Use a dedicated OS thread instead of a long-lived UE::Tasks task to avoid starving the task graph
	Async(EAsyncExecution::Thread, [this]()
	{
		while (bProcessMainThread)
		{
			FEvent* Event = GameSession ? GameSession->GetSendEvent() : nullptr;
			if (!Event) break;

			Event->Wait();
			
			if (!bProcessMainThread) break;
			if (!IsValid(GameSession) || !IsValid(UDPSubsystem)) break;
			
			ProcessOutgoingMessages();
		}
	});
}

void UCrowdyWorkerThreadsSubsystem::RunReceiveLoop()
{
	// Use a dedicated OS thread instead of a long-lived UE::Tasks task to avoid starving the task graph
	Async(EAsyncExecution::Thread, [this]()
	{
		while (bProcessMainThread)
		{
			FEvent* Event = GameSession ? GameSession->GetReceiveEvent() : nullptr;
			if (!Event) break;

			Event->Wait();

			if (!bProcessMainThread) break;
			if (!IsValid(GameSession)) break;
			
			ProcessIncomingMessages();
		}
	});
}

void UCrowdyWorkerThreadsSubsystem::ProcessOutgoingMessages() const
{
	TArray<uint8> Msg;
	while (GameSession->DequeueMessageToSend(Msg))
	{
		const bool bMessageSent = UDPSubsystem->SendMessage(MoveTemp(Msg));
		Msg.Reset();
		if (!bMessageSent)
		{
			UE_LOG(LogTemp, Log, TEXT("WorkerThreadsSubsystem: Message send failure."));
		}
	}
}

void UCrowdyWorkerThreadsSubsystem::ProcessIncomingMessages()
{
	auto BatchTask = [this]()
	{
		for (int i = 0; i < 50; ++i)
		{
			TArray<uint8>* ReceiveBuffer = BufferPoolSubsystem->GetBuffer();
			if (!ReceiveBuffer)
				break;

			// If no message, return the buffer and continue
			if (!GameSession->DequeueMessageToReceive(*ReceiveBuffer))
			{
				BufferPoolSubsystem->ReleaseBuffer(ReceiveBuffer);
				continue;
			}

			// Parse while we still own buffer
			const TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe> Message = NetworkMessageParser->ParseMessage(*ReceiveBuffer);

			// Return buffer exactly once
			BufferPoolSubsystem->ReleaseBuffer(ReceiveBuffer);
			
			UDPSubsystem->IncrementReceivedMessageCount();
			
			// Dispatch the message
			ServiceRegistry->DispatchMessage(Message);
		}
	};

	TArray<TFunction<void()>> Tasks;
	Tasks.Add(BatchTask);
	EnqueueTasks(Tasks);
}

