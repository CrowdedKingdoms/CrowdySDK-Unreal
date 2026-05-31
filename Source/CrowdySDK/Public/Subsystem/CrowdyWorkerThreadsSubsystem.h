// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Containers/Queue.h"
#include "CrowdyWorkerThreadsSubsystem.generated.h"


class FCrowdyServiceRegistry;
class UCrowdyGameSession;
class FMessageBufferPool;
class FCrowdyMessageParser;
class FWorker;
class FRunnableThread;
class UCrowdyUDPSubsystem;

/**
 * 
 */
UCLASS()
class CROWDYSDK_API UCrowdyWorkerThreadsSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()
public:
	
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	
	void InitializeWorkerThreadPool(FMessageBufferPool* InBufferPool, FCrowdyMessageParser* InMessageParser, FCrowdyServiceRegistry* InServiceRegistry, UCrowdyUDPSubsystem
	                                * InUdpSubsystem, UCrowdyGameSession* InGameSession);
	
	void EnqueueTasks(const TArray<TFunction<void()>>& Tasks);
	
	void StopAllProcesses();
	
private:
	
	UPROPERTY()
	UCrowdyGameSession* GameSession;
	
	UPROPERTY()
	UCrowdyUDPSubsystem* UDPSubsystem;
	
	FMessageBufferPool* BufferPoolSubsystem;
	
	FCrowdyMessageParser* NetworkMessageParser;
	
	FCrowdyServiceRegistry* ServiceRegistry;
	
	TArray<FRunnableThread*> WorkerThreads;
	
	TArray<FWorker*> Workers;

	TArray<TQueue<TFunction<void()>, EQueueMode::Mpsc>*> TaskQueues;
	TArray<FEvent*> TaskEvents;
	
	bool bProcessMainThread = false;
	bool bShouldRun = false;
	bool bWasPreviouslyConnected = false;
	
	void InitializeWorkerThreads();
	
	void RunSendLoop() const;

	void RunReceiveLoop();

	void ProcessOutgoingMessages() const;

	void ProcessIncomingMessages();
};
