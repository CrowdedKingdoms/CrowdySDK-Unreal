#include "FWorker.h"


FWorker::FWorker(TQueue<TFunction<void()>, EQueueMode::Mpsc>& InQueue, FEvent* InTaskEvent)
	: bShouldStop(false),
	TaskQueue(InQueue),
	TaskEvent(InTaskEvent)
{
}

FWorker::~FWorker()
{
}

uint32 FWorker::Run()
{
	while (!bShouldStop)
	{
		TFunction<void()> Task;
		bool bFoundTask = false;
		
		while (TaskQueue.Dequeue(Task))
		{
			if (Task)
			{
				Task();
				bFoundTask = true;
			}
			if (bShouldStop) break;
		}
		
		if (!bFoundTask && !bShouldStop)
			TaskEvent->Wait();
		
	}
	return 0;
}

void FWorker::Stop()
{
	bShouldStop = true;
	if (TaskEvent)
		TaskEvent->Trigger();
}

void FWorker::Exit()
{
	FRunnable::Exit();
}