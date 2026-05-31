// Fill out your copyright notice in the Description page of Project Settings.


#include "VoiceChatSubsystem.h"
#include "FCircularBuffer.h"
#include "Structures/FAudioChatTrack.h"
#include "AudioCapture.h"
#include "Core/Audio/Platform/AWinAudioDeviceMonitor.h"
#include "Service/FVoiceChatService.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Async/Async.h"
#include "TimerManager.h"

void UVoiceChatSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	
	TargetNumChannels = 1;
	TargetSampleRate = 48000;
	AUDIO_CHUNK_SIZE = 960;
	
	CaptureBuffer = new FCircularBuffer(96000, 1, 2.0f);
	AudioResampler = nullptr;
	LastDeviceSampleRate = 0;
	
}

void UVoiceChatSubsystem::Deinitialize()
{
	StopVoiceChat();
	
	if (CaptureBuffer)
	{
		delete CaptureBuffer;
		CaptureBuffer = nullptr;
	}
	
	{
		FReadScopeLock Lock(StreamLock);
		for (auto& StreamPair : SoundStreamMap)
		{
			FAudioChatTrack& AudioTrack = StreamPair.Value;
			if (AudioTrack.PlaybackBuffer)
			{
				delete AudioTrack.PlaybackBuffer;
				AudioTrack.PlaybackBuffer = nullptr;
			}
		}
	}
	
	if (AudioResampler)
	{
		src_delete(AudioResampler);
		AudioResampler = nullptr;
	}
	
	Super::Deinitialize();
}

TStatId UVoiceChatSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UVoiceChatSubsystem, STATGROUP_Tickables);
}

void UVoiceChatSubsystem::StartVoiceChat()
{
	UE_LOG(LogTemp, Log, TEXT("[CrowdySDK]: Starting Voice Chat"));
	InitializeAudioCapture();
	StartVoiceCapture();
	bVoiceChatIsRunning = true;
}

void UVoiceChatSubsystem::StopVoiceChat()
{
	UE_LOG(LogTemp, Log, TEXT("[CrowdySDK]: Stopping voice chat"));
	StopVoiceCapture();
	bVoiceChatIsRunning = false;
}

void UVoiceChatSubsystem::PlayVoiceChat()
{
	UE_LOG(LogTemp, Log, TEXT("[CrowdySDK]: Playing Voice chat"));
	
	bIsProcessingIncomingAudio = true;
	
	TickPlayback();
}

void UVoiceChatSubsystem::MuteVoiceChat()
{
	UE_LOG(LogTemp, Log, TEXT("[CrowdySDK]: Muting voice chat"));
	//OnAudioDataReceived.Clear();
	bIsProcessingIncomingAudio = false;
	
	StopTicking();
}

void UVoiceChatSubsystem::SetStreamTimeoutThreshold(const float InSeconds)
{
	StreamTimeoutThreshold = InSeconds;
}

void UVoiceChatSubsystem::SetVoiceChatService(FVoiceChatService* InVoiceChatService)
{
	VoiceChatService = InVoiceChatService;
}

void UVoiceChatSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
}

void UVoiceChatSubsystem::TickPlayback()
{
	if (bIsTicking)
	{
		return;
	}

	bIsTicking = true;
	
	UE::Tasks::Launch(UE_SOURCE_LOCATION, [this]()
	{
		double LastTime = FPlatformTime::Seconds();
		double LocalAccumulatedTime = 0.0;
		constexpr double LocalTickInterval = 0.02; // ~60 FPS equivalent, adjust as needed
		
		while (bIsTicking)
		{
			bool bProcessedWork = false;
			
			const double CurrentTime = FPlatformTime::Seconds();
			const double DeltaTime = CurrentTime - LastTime;
			LastTime = CurrentTime;
			
			LocalAccumulatedTime += DeltaTime;
			
			if (LocalAccumulatedTime >= LocalTickInterval)
			{
				// Process audio playback
				if (bIsProcessingIncomingAudio)
				{
					ManagePlaybackBuffer();
					bProcessedWork = true;
				}
				
				// Check for timed-out streams
				CheckStreamTimeouts();
				bProcessedWork = true;
				
				LocalAccumulatedTime -= LocalTickInterval;
			}
			
			// Only sleep if no work was processed
			if (!bProcessedWork)
			{
				FPlatformProcess::Sleep(0.001f); // 1ms
			}
		}
	});
}

void UVoiceChatSubsystem::StopTicking()
{
	bIsTicking = false;
}

void UVoiceChatSubsystem::HandleIncomingAudio(TArray<float>&& IncomingAudioData, int32 SampleRate,
                                              int32 NumChannels, FGuid PlayerID)
{
	
	
		bool bDoesStreamExist;
		
		{
			FReadScopeLock Lock(StreamLock);
			bDoesStreamExist = DoesPlayerStreamExist(PlayerID);
		}
		
		if (!bDoesStreamExist)
		{
			UE_LOG(LogTemp, Log, TEXT("[CrowdySDK]: No stream found for PlayerID: %s, creating a new one."), *PlayerID.ToString());

			AsyncTask(ENamedThreads::GameThread, [this, PlayerID, SampleRate]()
			{
				AddPlayerStream(PlayerID, SampleRate);
			});
			return; // Return early if we don't have a stream for this player
		}

		// Get the player's stream
		FAudioChatTrack* PlayerStream = GetPlayerStream(PlayerID);
		if (PlayerStream && PlayerStream->PlaybackBuffer)
		{
			// Add audio data to the player's buffer
			const float* AudioData = IncomingAudioData.GetData();

			if (!PlayerStream->PlaybackBuffer->TryEnqueue(AudioData, IncomingAudioData.Num()))
			{
				UE_LOG(LogTemp, Warning, TEXT("[CrowdySDK]: Failed to enqueue audio data for PlayerID: %s"), *PlayerID.ToString());
			}

			// Update last activity timestamp
			{
				FWriteScopeLock Lock(TimeLock);
				LastUpdateTimes.Add(PlayerID, FPlatformTime::Seconds());
			}
			

			//UE_LOG(LogVoiceChat, Log, TEXT("Updated last activity timestamp for PlayerID: %s"), *PlayerID);
		}
		else
		{
			UE_LOG(LogTemp, Warning,
				   TEXT("[CrowdySDK]: Player stream is invalid or playback buffer not initialized for PlayerID: %s"), *PlayerID.ToString());
		}
	

	// For Audio-Notify icon
	AsyncTask(ENamedThreads::GameThread, [this]
	{
		OnAudioNotify.Broadcast();
	});
}

void UVoiceChatSubsystem::InitializeVoiceChatSubsystem(FVoiceChatService* InVoiceChatService)
{
	VoiceChatService = InVoiceChatService;
	InitializeAudioCapture();
	AudioDeviceMonitor = GetWorld()->SpawnActor<AWinAudioDeviceMonitor>();
	AudioDeviceMonitor->OnAudioDeviceConnected.AddDynamic(this, &UVoiceChatSubsystem::HandleAudioDeviceConnection);
	AudioDeviceMonitor->OnAudioDeviceDisconnected.AddDynamic(this, &UVoiceChatSubsystem::HandleAudioDeviceDisconnection);
	RootActor = GetWorld()->GetFirstPlayerController()->GetPawn();
}

void UVoiceChatSubsystem::InitializeAudioCapture()
{
	AudioCaptureDevice = NewObject<UAudioCapture>(this);

	if (AudioCaptureDevice)
	{
		AudioCaptureDevice->AddGeneratorDelegate(FOnAudioGenerate([this](const float* AudioData, int32 NumSamples)
		{
			HandleAudioGeneration(AudioData, NumSamples);
		}));
	}
}

void UVoiceChatSubsystem::StartVoiceCapture()
{
	if (!bIsCapturing && AudioCaptureDevice)
	{
		// Reset buffers
		CaptureBuffer->Reset();

		// Start the audio capture
		bool bSuccess = AudioCaptureDevice->OpenDefaultAudioStream();

		if (!bSuccess)
		{
			UE_LOG(LogTemp, Error, TEXT("[CrowdySDK]: Failed to open default audio stream"));
			return;
		}

		if (!AudioCaptureDevice->IsCapturingAudio())
		{
			AudioCaptureDevice->StartCapturingAudio();
			bIsCapturing = true;
			bIsProcessing = true;

			// Start the processing thread
			UE::Tasks::Launch(UE_SOURCE_LOCATION, [this]()
			{
				ProcessAudioDataThread();
			}, LowLevelTasks::ETaskPriority::BackgroundLow);
		}
		UE_LOG(LogTemp, Log, TEXT("[CrowdySDK]: Voice capture started"));
	}
}

void UVoiceChatSubsystem::StopVoiceCapture()
{
	if (bIsCapturing && AudioCaptureDevice && AudioCaptureDevice->IsCapturingAudio())
	{
		// Stop the audio capture
		AudioCaptureDevice->StopCapturingAudio();
		bIsCapturing = false;
		bIsProcessing = false;
		AudioCaptureDevice = nullptr;
		UE_LOG(LogTemp, Log, TEXT("[CrowdySDK]: Voice capture stopped"));
	}
}

void UVoiceChatSubsystem::HandleAudioGeneration(const float* AudioData, int32 NumSamples)
{
	if (!bIsCapturing)
	{
		return;
	}
	
	// Get current device sample rate
	const int32 DeviceSampleRate = AudioCaptureDevice->GetSampleRate();
	const int32 DeviceChannels = AudioCaptureDevice->GetNumChannels();

	// Calculate the time length of this audio chunk in seconds
	const float ChunkTimeSeconds = static_cast<float>(NumSamples) / (DeviceSampleRate * DeviceChannels);

	// Calculate the equivalent number of samples at our target rate (48kHz)
	const int32 TargetNumSamples = FMath::RoundToInt(ChunkTimeSeconds * TargetSampleRate);

	// First, convert to mono if necessary
	const float* AudioToProcess = AudioData;
	const int32 MonoSampleCount = NumSamples / DeviceChannels;

	if (DeviceChannels > 1)
	{
		TArray<float> MonoAudio;
		MonoAudio.Reserve(MonoSampleCount);

		for (int32 i = 0; i < NumSamples; i += DeviceChannels)
		{
			float MonoSample = 0.0f;
			for (int32 Channel = 0; Channel < DeviceChannels; ++Channel)
			{
				MonoSample += AudioData[i + Channel];
			}
			MonoSample /= DeviceChannels; // Average across all channels
			MonoAudio.Add(MonoSample);
		}

		AudioToProcess = MonoAudio.GetData();
	}

	// If the sample rate doesn't match our target, resample
	if (DeviceSampleRate != TargetSampleRate)
	{
		// Check if we need to create or update the resampler
		if (!AudioResampler || LastDeviceSampleRate != DeviceSampleRate)
		{
			// Create or recreate resampler with the right conversion ratio
			if (AudioResampler)
			{
				src_delete(AudioResampler);
			}

			int Error;
			AudioResampler = src_new(SRC_SINC_BEST_QUALITY, 1, &Error);
			LastDeviceSampleRate = DeviceSampleRate;

			if (Error != 0)
			{
				UE_LOG(LogTemp, Error, TEXT("[CrowdySDK]: Failed to create resampler: %s"),
				       UTF8_TO_TCHAR(src_strerror(Error)));
				return;
			}
		}

		// Prepare resampling
		const double ResampleRatio = static_cast<double>(TargetSampleRate) / DeviceSampleRate;

		TArray<float> ResampledAudio;
		ResampledAudio.SetNumUninitialized(TargetNumSamples + 32); // Add padding

		SRC_DATA SrcData;
		SrcData.data_in = const_cast<float*>(AudioToProcess);
		SrcData.data_out = ResampledAudio.GetData();
		SrcData.input_frames = MonoSampleCount;
		SrcData.output_frames = TargetNumSamples + 32;
		SrcData.src_ratio = ResampleRatio;
		SrcData.end_of_input = 0; // We're preserving state between calls

		// Process with resampler state preservation for smooth transitions
		int Error = src_process(AudioResampler, &SrcData);

		if (Error != 0)
		{
			UE_LOG(LogTemp, Error, TEXT("[CrowdySDK]: Resampling error: %s"),
			       UTF8_TO_TCHAR(src_strerror(Error)));
			return;
		}

		// Resize to actual output size
		ResampledAudio.SetNum(SrcData.output_frames_gen);

		// Add resampled data to buffer
		if (CaptureBuffer && ResampledAudio.Num() > 0)
		{
			CaptureBuffer->TryEnqueue(ResampledAudio.GetData(), ResampledAudio.Num());
		}
	}
	else
	{
		// Sample rate already matches target, add directly to buffer
		if (CaptureBuffer)
		{
			CaptureBuffer->TryEnqueue(AudioToProcess, MonoSampleCount);
		}
	}
}

void UVoiceChatSubsystem::ProcessAudioDataThread() const
{
	TArray<float> AudioChunk;

	while (bIsProcessing)
	{
		bool bProcessedAudio = false;
		
		// Lock for thread safety
		{
			// Check if we have enough data to process
			if (CaptureBuffer && CaptureBuffer->GetAvailableDataSize() >= AUDIO_CHUNK_SIZE)
			{
				// Dequeue audio data for processing
				if (CaptureBuffer->TryDequeue(AudioChunk, AUDIO_CHUNK_SIZE))
				{
					bProcessedAudio = true;
					
					// Broadcast audio data for networking or further processing
					AsyncTask(ENamedThreads::GameThread, [this, AudioChunk]()
					{
						VoiceChatService->CompressAudioData(AudioChunk, TargetSampleRate, TargetNumChannels);
					});
				}
			}
		}
		
		// Only sleep if no audio was processed
		if (!bProcessedAudio)
		{
			FPlatformProcess::Sleep(0.01f);
		}
	}
}

void UVoiceChatSubsystem::ManagePlaybackBuffer()
{
	// Process each stream
	StreamsToProcess.Reset();

	// Build list of streams to process
	{
		FReadScopeLock Lock(StreamLock);
		for (auto& StreamPair : SoundStreamMap)
		{
			StreamsToProcess.Add(TPair<FGuid, FAudioChatTrack*>(StreamPair.Key, &StreamPair.Value));
		}
	}

	// Process each stream
	for (auto& StreamPair : StreamsToProcess)
	{
		ProcessPlayerStream(StreamPair.Value);
	}
}

void UVoiceChatSubsystem::HandleAudioDeviceDisconnection(const FString& DeviceID)
{
	UE_LOG(LogTemp, Log, TEXT("[CrowdySDK]: Audio device disconnected: %s"), *DeviceID);
	// Reset the voice chat state completely
	bIsCapturing = false;
	bIsProcessing = false;

	// Make sure the audio device is properly released
	if (AudioCaptureDevice)
	{
		if (AudioCaptureDevice->IsCapturingAudio())
		{
			AudioCaptureDevice->StopCapturingAudio();
		}
		AudioCaptureDevice = nullptr;
	}
}

void UVoiceChatSubsystem::HandleAudioDeviceConnection(const FString& DeviceID)
{
	UE_LOG(LogTemp, Log, TEXT("[CrowdySDK]: Audio device connected: %s"), *DeviceID);

	// automatic restart:
	if (bVoiceChatIsRunning)
	{
		// Make sure we're starting from a clean state
		StopVoiceChat();
		// Wait a short time to ensure the device is ready
		FTimerHandle RestartTimerHandle;
		GetWorld()->GetTimerManager().SetTimer(
			RestartTimerHandle,
			[this]() { StartVoiceChat(); },
			0.5f, // Half-second delay
			false
		);
	}
}

bool UVoiceChatSubsystem::DoesPlayerStreamExist(const FGuid& PlayerID) const
{
	return SoundStreamMap.Contains(PlayerID);
}

FAudioChatTrack* UVoiceChatSubsystem::GetPlayerStream(const FGuid& PlayerID)
{
	FReadScopeLock Lock(StreamLock);
	if (DoesPlayerStreamExist(PlayerID))
	{
		return &SoundStreamMap[PlayerID];
	}
	return nullptr;
}

void UVoiceChatSubsystem::AddPlayerStream(const FGuid& PlayerID, uint32 SampleRate)
{
	// Create a new audio track for this player
	FAudioChatTrack NewTrack;

	// Create a procedural sound wave
	NewTrack.SoundWave = NewObject<USoundWaveProcedural>();
	if (NewTrack.SoundWave)
	{
		NewTrack.SoundWave->SetSampleRate(SampleRate);
		NewTrack.SoundWave->NumChannels = 1;
		NewTrack.SoundWave->Duration = INDEFINITELY_LOOPING_DURATION;
		NewTrack.SoundWave->SoundGroup = SOUNDGROUP_Voice;
		NewTrack.SoundWave->bLooping = false;
	}

	// Create an audio component for playback
	NewTrack.AudioComponent = NewObject<UAudioComponent>(RootActor);
	if (NewTrack.AudioComponent)
	{
		NewTrack.AudioComponent->SetSound(NewTrack.SoundWave);
		NewTrack.AudioComponent->bAutoActivate = true;
		NewTrack.AudioComponent->RegisterComponent();
	}

	// Create a buffer for playback (2-second capacity)
	NewTrack.PlaybackBuffer = new FCircularBuffer(SampleRate, 1, 2.0f);

	// Add to map
	{
		FWriteScopeLock Lock(StreamLock);
		SoundStreamMap.Add(PlayerID, NewTrack);
		LastUpdateTimes.Add(PlayerID, FPlatformTime::Seconds());
	}

	UE_LOG(LogTemp, Log, TEXT("[CrowdySDK]: Added player %s to voice chat"), *PlayerID.ToString());
}

void UVoiceChatSubsystem::RemovePlayerStream(const FGuid& PlayerID)
{
	// Get the player's stream
	if (const FAudioChatTrack* PlayerStream = GetPlayerStream(PlayerID))
	{
		// For Voice Chat Service
		VoiceChatService->CleanupDecoder(PlayerID);
		//OnStreamTimeout.Broadcast(PlayerID);

		// Clean up resources
		if (PlayerStream->AudioComponent)
		{
			PlayerStream->AudioComponent->Stop();
			PlayerStream->AudioComponent->DestroyComponent();
		}

		if (PlayerStream->PlaybackBuffer)
		{
			delete PlayerStream->PlaybackBuffer;
		}

		// Remove from maps
		{
			FWriteScopeLock Lock(StreamLock);
			SoundStreamMap.Remove(PlayerID);
			LastUpdateTimes.Remove(PlayerID);
		}

		UE_LOG(LogTemp, Log, TEXT("[CrowdySDK]: Removed player %s from voice chat"), *PlayerID.ToString());
	}
}

void UVoiceChatSubsystem::ProcessPlayerStream(FAudioChatTrack* AudioTrack)
{
	if (!AudioTrack || !AudioTrack->PlaybackBuffer || !AudioTrack->SoundWave)
	{
		return;
	}

	// Number of samples to read for this update
	constexpr int32 MaxSamplesToRead = 960; // Adjust this for your needs
	constexpr int32 MinSamplesToRead = 512; // Adjust this for your needs

	const int32 AvailableSamples = AudioTrack->PlaybackBuffer->GetAvailableDataSize();

	// Process if we have enough data
	if (AvailableSamples >= MinSamplesToRead)
	{
		const int32 SamplesToRead = FMath::Min(AvailableSamples, MaxSamplesToRead);

		TArray<float> AudioChunk;

		if (AudioTrack->PlaybackBuffer->TryDequeue(AudioChunk, SamplesToRead))
		{
			// Convert float [-1.0f, 1.0f] to 16-bit PCM
			TArray<int16> PCMInt16Data;
			PCMInt16Data.SetNumUninitialized(AudioChunk.Num());

			for (int32 i = 0; i < AudioChunk.Num(); ++i)
			{
				const float ClampedSample = FMath::Clamp(AudioChunk[i], -1.0f, 1.0f);
				PCMInt16Data[i] = static_cast<int16>(ClampedSample * 32767.0f);
			}

			// Convert to uint8 buffer for QueueAudio
			TArray<uint8> PCMData;
			PCMData.SetNumUninitialized(PCMInt16Data.Num() * sizeof(int16));
			FMemory::Memcpy(PCMData.GetData(), PCMInt16Data.GetData(), PCMData.Num());

			// Queue to sound wave
			AudioTrack->SoundWave->QueueAudio(PCMData.GetData(), PCMData.Num());

			// Start playback if not playing
			if (AudioTrack->AudioComponent && !AudioTrack->AudioComponent->IsPlaying())
			{
				AudioTrack->AudioComponent->Play();
			}
		}
	}
}

void UVoiceChatSubsystem::CheckStreamTimeouts()
{
	// Current time
	const double CurrentTime = FPlatformTime::Seconds();
	TArray<FGuid> StreamsToRemove;

	// Check each stream for timeout
	for (auto& TimePair : LastUpdateTimes)
	{
		if (CurrentTime - TimePair.Value > StreamTimeoutThreshold)
		{
			StreamsToRemove.Add(TimePair.Key);
		}
	}

	// Remove timed-out streams
	for (const FGuid& PlayerID : StreamsToRemove)
	{
		RemovePlayerStream(PlayerID);
	}
}
