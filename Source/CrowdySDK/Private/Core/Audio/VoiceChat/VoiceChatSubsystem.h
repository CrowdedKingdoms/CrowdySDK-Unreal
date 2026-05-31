// Fill out your copyright notice in the Description page of Project Settings.

#pragma once
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include <samplerate.h>
#include "Core/Audio/VoiceChat/Structures/FAudioChatTrack.h"
#include "Misc/Guid.h"
#include "HAL/CriticalSection.h"
#include <atomic>
#include "GameFramework/Actor.h"
#include "VoiceChatSubsystem.generated.h"

class FVoiceChatService;
class AWinAudioDeviceMonitor;
class FCircularBuffer;
class UAudioCapture;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnAudioDataGenerated, const TArray<float>&, AudioData, int32,
                                               SampleRate, int32, NumChannels);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FOnAudioDataReceived, const TArray<float>&, AudioData, int32, SampleRate, int32, NumChannels, FString, PlayerID);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStreamTimeout, const FString&, UUID);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FAudioNotify);

/**
 * 
 */
UCLASS()
class CROWDYSDK_API UVoiceChatSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()
public:
	
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual TStatId GetStatId() const override;
	
	friend class UCrowdySDKSubsystem;
	
	UPROPERTY()
	FOnAudioDataGenerated OnAudioDataGenerated;
	
	UPROPERTY()
	FOnAudioDataReceived OnAudioDataReceived;

	UPROPERTY()
	FAudioNotify OnAudioNotify;
	
	UPROPERTY()
	FOnStreamTimeout OnStreamTimeout;
	
	void HandleIncomingAudio(TArray<float>&& IncomingAudioData, int32 SampleRate, int32 NumChannels, FGuid PlayerID);

protected:
	
	virtual void Tick(float DeltaTime) override;
	void TickPlayback();
	void StopTicking();
	
	UPROPERTY()
	UAudioCapture* AudioCaptureDevice;
	
	UPROPERTY()
	float StreamTimeoutThreshold = 2.0f;
	
	std::atomic<bool> bIsTicking = false;
	
	void InitializeVoiceChatSubsystem(FVoiceChatService* InVoiceChatService);
	void StartVoiceChat();
	void StopVoiceChat();
	void PlayVoiceChat();
	void MuteVoiceChat();
	
	void SetStreamTimeoutThreshold(const float InSeconds);
	inline void SetVoiceChatService(FVoiceChatService* InVoiceChatService);
	
	void InitializeAudioCapture();
	
	void StartVoiceCapture();
	
	void StopVoiceCapture();
	
	void HandleAudioGeneration(const float* AudioData, int32 NumSamples);
	
	void ProcessAudioDataThread() const;
	
	void ManagePlaybackBuffer();

private:
	
	UPROPERTY()
	int32 TargetNumChannels;
	
	UPROPERTY()
	int32 TargetSampleRate;
	
    UPROPERTY()
    int32 AUDIO_CHUNK_SIZE;
	
    UPROPERTY()
    TMap<FGuid, FAudioChatTrack> SoundStreamMap;
	
	UPROPERTY()
	bool bVoiceChatIsRunning;
	
	UPROPERTY()
	int32 LastDeviceSampleRate = 0;

	UPROPERTY()
	float TickInterval = 0.02f;

	UPROPERTY()
	float AccumulatedTime = 0.0f;

	UPROPERTY()
	AActor* RootActor;
	
	UPROPERTY()
	AWinAudioDeviceMonitor* AudioDeviceMonitor;
	
	UPROPERTY()
	TMap<FGuid, double> LastUpdateTimes;
	
	SRC_STATE* AudioResampler = nullptr;
	
	TArray<TPair<FGuid, FAudioChatTrack*>> StreamsToProcess;
	FCircularBuffer* CaptureBuffer;
	
	mutable FRWLock StreamLock;
	mutable FRWLock TimeLock;
	std::atomic<bool> bIsCapturing;
	std::atomic<bool> bIsProcessing;
	std::atomic<bool> bIsProcessingIncomingAudio;
	
	FVoiceChatService* VoiceChatService;
	
    UFUNCTION()
    void HandleAudioDeviceDisconnection(const FString& DeviceID);
	
    UFUNCTION()
    void HandleAudioDeviceConnection(const FString& DeviceID);

	bool DoesPlayerStreamExist(const FGuid& PlayerID) const;
	
	FAudioChatTrack* GetPlayerStream(const FGuid& PlayerID);
	
	void AddPlayerStream(const FGuid& PlayerID, uint32 SampleRate);
	
	void RemovePlayerStream(const FGuid& PlayerID);
	
	void ProcessPlayerStream(FAudioChatTrack* AudioTrack);
	
	void CheckStreamTimeouts();
	
};
