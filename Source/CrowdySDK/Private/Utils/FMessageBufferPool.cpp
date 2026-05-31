#include "Utils/FMessageBufferPool.h"

FMessageBufferPool::FMessageBufferPool()
{
	for (int i = 0; i < MaxPoolSize; ++i)
	{
		TArray<uint8>* Buffer = new TArray<uint8>();
		Buffer->Reserve(DefaultBufferSize);
		BufferQueue.Enqueue(Buffer);
		BufferCounter.Increment();
	}
	
	UE_LOG(LogTemp, Log, TEXT("[CrowdySDK]: Message buffer pool initialized with %d buffers (capacity %d)"), MaxPoolSize, DefaultBufferSize);
}

FMessageBufferPool::~FMessageBufferPool()
{
	TArray<uint8>* Buffer = nullptr;
	while (BufferQueue.Dequeue(Buffer))
	{
		delete Buffer;
	}
}

TArray<uint8>* FMessageBufferPool::GetBuffer()
{
	FScopeLock Lock(&BufferMutex);
	TArray<uint8>* Buffer = nullptr;
	
	if (BufferQueue.Dequeue(Buffer))
	{
		Buffer->Reset();
		BufferCounter.Decrement();
	}
	
	return Buffer;
}

void FMessageBufferPool::ReleaseBuffer(TArray<uint8>* Buffer)
{
	if (!Buffer) return;
	
	FScopeLock Lock(&BufferMutex);
	if (BufferCounter.GetValue() < MaxPoolSize)
	{
		Buffer->Reset();
		BufferQueue.Enqueue(Buffer);
		BufferCounter.Increment();
	}
	else
	{
		delete Buffer;
	}
	
	
}
