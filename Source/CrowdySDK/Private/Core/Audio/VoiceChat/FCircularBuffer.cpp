#include "FCircularBuffer.h"

FCircularBuffer::FCircularBuffer(const uint32 InSampleRate, const uint32 InNumChannels, const float InBufferDuration)
{
	BufferCapacity = static_cast<int32>(InSampleRate * InNumChannels * InBufferDuration);
	
	Buffer.SetNumZeroed(BufferCapacity);
	
	ReadIndex = 0;
	WriteIndex = 0;
	SamplesAvailable = 0;
	
	bIsEmpty = true;
	bIsFull = false;
}

FCircularBuffer::~FCircularBuffer()
{
	FScopeLock Lock(&BufferLock);
	Buffer.Empty();
}

bool FCircularBuffer::TryEnqueue(const float* InData, int32 InNumSamples)
{
	if (InData == nullptr || InNumSamples <= 0)
	{
		return false;
	}
    
	FScopeLock Lock(&BufferLock);
    
	// Handle case where input exceeds buffer capacity
	if (InNumSamples > BufferCapacity)
	{
		// Only use the most recent samples that fit in the buffer
		InData += (InNumSamples - BufferCapacity);
		InNumSamples = BufferCapacity;
	}
    
	// Add data sample by sample, handling wrap-around
	for (int32 i = 0; i < InNumSamples; ++i)
	{
		Buffer[WriteIndex] = InData[i];
        
		// Update write index with wrap-around
		WriteIndex = (WriteIndex + 1) % BufferCapacity;
        
		// If buffer was full, move read index too (overwrite oldest data)
		if (bIsFull)
		{
			ReadIndex = (ReadIndex + 1) % BufferCapacity;
		}
		else
		{
			// Increment available samples count
			SamplesAvailable++;
			if (SamplesAvailable >= BufferCapacity)
			{
				bIsFull = true;
				SamplesAvailable = BufferCapacity;
			}
		}
        
		bIsEmpty = false;
	}
    
	return true;
}

bool FCircularBuffer::TryEnqueue(const TArray<float>& InData)
{
	if (InData.IsEmpty())
	{
		return false;
	}
    
	return TryEnqueue(InData.GetData(), InData.Num());
}

bool FCircularBuffer::TryDequeue(TArray<float>& OutData, int32 InNumSamples)
{
	FScopeLock Lock(&BufferLock);
    
	// Can't dequeue if buffer is empty or requested samples exceed available data
	if (bIsEmpty || InNumSamples <= 0 || InNumSamples > SamplesAvailable)
	{
		return false;
	}
    
	// Resize output array
	OutData.SetNumUninitialized(InNumSamples);
    
	// Extract data sample by sample, handling wrap-around
	for (int32 i = 0; i < InNumSamples; ++i)
	{
		OutData[i] = Buffer[ReadIndex];
        
		// Update read index with wrap-around
		ReadIndex = (ReadIndex + 1) % BufferCapacity;
	}
    
	// Update state
	SamplesAvailable -= InNumSamples;
	bIsFull = false;
	bIsEmpty = SamplesAvailable == 0;
    
	return true;
}

int32 FCircularBuffer::GetAvailableDataSize() const
{
	FScopeLock Lock(&const_cast<FCriticalSection&>(BufferLock));
	return SamplesAvailable;
}

void FCircularBuffer::Reset()
{
	FScopeLock Lock(&BufferLock);
	
	ReadIndex = 0;
	WriteIndex = 0;
	SamplesAvailable = 0;
	bIsEmpty = true;
	bIsFull = false;
	
	FMemory::Memzero(Buffer.GetData(), Buffer.Num() + sizeof(float));
}
