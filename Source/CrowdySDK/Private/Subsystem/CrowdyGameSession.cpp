// Fill out your copyright notice in the Description page of Project Settings.


#include "Subsystem/CrowdyGameSession.h"
#include "Async/TaskGraphInterfaces.h"
#include "Utils/HelperFunctions.h"

void UCrowdyGameSession::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	UE_LOG(LogTemp, Log, TEXT("[CrowdySDK]: Game Session Initialized."))
	SendEvent = FPlatformProcess::GetSynchEventFromPool(false);
	ReceiveEvent = FPlatformProcess::GetSynchEventFromPool(false);
}

void UCrowdyGameSession::Deinitialize()
{
	if (SendEvent)    SendEvent->Trigger();
	if (ReceiveEvent) ReceiveEvent->Trigger();
	
	FPlatformProcess::ReturnSynchEventToPool(SendEvent);
	FPlatformProcess::ReturnSynchEventToPool(ReceiveEvent);
	ReceiveEvent = nullptr;
	SendEvent = nullptr;
	Super::Deinitialize();
}

bool UCrowdyGameSession::EnqueueMessageToSend(TArray<uint8>&& Message)
{
	++SendCounter;
	const bool bResult = SendQueue.Enqueue(MoveTemp(Message));
	SendEvent->Trigger();
	return bResult;
}

bool UCrowdyGameSession::DequeueMessageToSend(TArray<uint8>& OutMessage)
{
	TArray<uint8> Tmp;
	if (!SendQueue.Dequeue(Tmp))
		return false;

	OutMessage = MoveTemp(Tmp);
	return true;
}

void UCrowdyGameSession::EnqueueMessageToReceive(const TArray<uint8>& Message)
{
	FScopeLock Lock(&ReceiveQueueMutex);
	ReceiveQueue.Enqueue(Message);
	if (ReceiveEvent)
	{
		ReceiveEvent->Trigger();
	}
	++ReceiveCounter;
}

bool UCrowdyGameSession::DequeueMessageToReceive(TArray<uint8>& OutMessage)
{
	FScopeLock Lock(&ReceiveQueueMutex);
	
	if (ReceiveQueue.IsEmpty()) return false;
	
	ReceiveQueue.Dequeue(OutMessage);
	--ReceiveCounter;
	return true;
}

bool UCrowdyGameSession::HasPendingIncomingMessages() const
{
	return !ReceiveQueue.IsEmpty();
}

bool UCrowdyGameSession::HasPendingOutgoingMessages() const
{
	return !SendQueue.IsEmpty();
}

FEvent* UCrowdyGameSession::GetSendEvent() const
{
	return SendEvent;
}

FEvent* UCrowdyGameSession::GetReceiveEvent() const
{
	return ReceiveEvent;
}
