#pragma once
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "InstancedStruct.h"
#include "Containers/RingBuffer.h"
#include "Templates/IsTriviallyDestructible.h"


template<typename T, int32 Capacity = 32>
struct TInterpolatedField
{
	static_assert(std::is_trivially_destructible_v<T>, "T must be trivially destructible for cache-friendly SoA layout.");
	
	T Values[Capacity];
	int64 Timestamps[Capacity];
	int32 Head = 0;
	int32 Count = 0;
	
	FORCEINLINE void Push(const T& Value, int64 Timestamp)
	{
		Values[Head] = Value;
		Timestamps[Head] = Timestamp;
		Head = (Head + 1) % Capacity;
		Count = FMath::Min(Count + 1, Capacity);
	}
	
	FORCEINLINE int32 PhysicalIndex(const int32 LogicalIndex) const
	{
		return (Head - Count + LogicalIndex + Capacity) % Capacity;
	}
	
	template<typename TLerpFn>
	T Sample(int64 RenderTimeMs, TLerpFn LerpFn, const T& Default = T{}) const
	{
		if (Count == 0) return Default;
		if (Count == 1) return Values[PhysicalIndex(0)];
		
		int32 Low = 0, High = Count - 2;

		while (Low <= High)
		{
			const int32 Mid = (Low + High) / 2;
			const int32 IndexA = PhysicalIndex(Mid);
			const int32 IndexB = PhysicalIndex(Mid + 1);
			const int64 TimeA = Timestamps[IndexA];
			const int64 TimeB = Timestamps[IndexB];
			
			if (RenderTimeMs < TimeA) { High = Mid - 1; continue; }
			else if (RenderTimeMs > TimeB) { Low = Mid + 1; continue; }
			
			const float Alpha = FMath::Clamp(
				static_cast<float>(RenderTimeMs - TimeA) / FMath::Max<float>(static_cast<float>(TimeB - TimeA), 1.f),
				0.f, 1.f
			);
			
			return LerpFn(Values[IndexA], Values[IndexB], Alpha);
		}
		
		return Extrapolate(RenderTimeMs, LerpFn);
	}
	
private:
	template<typename TLerpFn>
	T Extrapolate(int64 RenderTimeMs, TLerpFn LerpFn) const
	{
		const int32 LastIndex = PhysicalIndex(Count - 1);
		const int32 PrevIndex = PhysicalIndex(Count - 2);
		
		const float DeltaTime  = FMath::Max(static_cast<float>(Timestamps[LastIndex] - Timestamps[PrevIndex]) / 1000.f,
			0.001f);
		
		const float TimePast = FMath::Min(
			static_cast<float>(RenderTimeMs - Timestamps[LastIndex]) / 1000.f,
			0.2f 
		);
		
		const float Alpha = DeltaTime > 0.f ? TimePast / DeltaTime : 0.f;
		return LerpFn(Values[PrevIndex], Values[LastIndex], 1.f + Alpha);
	}
};
