#pragma once
#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"

class FCircularBuffer
{
public:
	
	FCircularBuffer(uint32 InSampleRate = 48000, uint32 InNumChannels = 1, float InBufferDuration = 2.0f);
	~FCircularBuffer();
	
	bool TryEnqueue(const float* InData, int32 InNumSamples);
	bool TryEnqueue(const TArray<float>& InData);
	bool TryDequeue(TArray<float>& OutData, int32 InNumSamples);
	int32 GetAvailableDataSize() const;
	void Reset();
	int32 GetCapacity() const { return BufferCapacity; }
	
private:
	
	TArray<float> Buffer;
	FCriticalSection BufferLock;
	int32 WriteIndex;
	int32 ReadIndex;
	int32 SamplesAvailable;
	int32 BufferCapacity;
	bool bIsEmpty;
	bool bIsFull;
};
