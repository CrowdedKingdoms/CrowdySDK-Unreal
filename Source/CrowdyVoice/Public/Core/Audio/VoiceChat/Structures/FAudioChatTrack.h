#pragma once
#include "CoreMinimal.h"
#include "Components/AudioComponent.h"
#include "Sound/SoundWaveProcedural.h"
#include "UObject/WeakObjectPtr.h"
#include "UObject/ObjectPtr.h"
#include "FAudioChatTrack.generated.h"

class FCircularBuffer;

USTRUCT()
struct FAudioChatTrack
{
	GENERATED_BODY()
	
	UPROPERTY()
	uint8 Version = 1;
	
	UPROPERTY()
	TObjectPtr<USoundWaveProcedural> SoundWave;
	
	UPROPERTY()
	TObjectPtr<UAudioComponent> AudioComponent;
	
	FCircularBuffer* PlaybackBuffer = nullptr;
};