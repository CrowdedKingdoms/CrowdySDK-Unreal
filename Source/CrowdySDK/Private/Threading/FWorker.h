#pragma once
#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "Containers/Queue.h"
#include "HAL/Event.h"
#include <atomic>

class CROWDYSDK_API FWorker : public FRunnable
{

public:
	
	FWorker(TQueue<TFunction<void()>, EQueueMode::Mpsc>& InQueue, FEvent* InTaskEvent);
	virtual ~FWorker() override;
	
	virtual uint32 Run() override;
	virtual void Stop() override;
	virtual void Exit() override;
	
private:
	
	std::atomic<bool> bShouldStop;
	TQueue<TFunction<void()>, EQueueMode::Mpsc>& TaskQueue;
	FEvent* TaskEvent;
};
