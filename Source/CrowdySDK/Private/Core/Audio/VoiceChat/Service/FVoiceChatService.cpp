#include "FVoiceChatService.h"

#include "Core/Audio/VoiceChat/VoiceChatSubsystem.h"
#include "Messages/Communication/FClientAudioNotification.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Subsystem/CrowdySDKSubsystem.h"
#include "Utils/SerializationFunctionLibrary.h"


FVoiceChatService::FVoiceChatService(UCrowdySDKSubsystem* InCrowdySDK, UCrowdyGameSession* InGameSession)
{
	AudioEncoder = nullptr;
	AudioDecoder = nullptr;
	VoiceChatManager = nullptr;
	CrowdySDKSubsystem = InCrowdySDK;
	GameSessionSubsystem = InGameSession;
	CrowdySDKSubsystem->RegisterReceptionLayer(this);
}

FVoiceChatService::~FVoiceChatService()
{
	CleanupOpus();
	CleanupAllDecoders();
}

void FVoiceChatService::CompressAudioData(const TArray<float>& InAudioData, int32 SampleRate, int32 NumChannels)
{
	FScopeLock Lock(&AudioLock);

	// Comprehensive input validation
	if (InAudioData.Num() == 0 || InAudioData.Num() % 2 != 0)
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdySDK]: Input audio data is empty or invalid."));
		return;
	}
	
	// Ensure the encoder is initialized
	if (!AudioEncoder)
	{
		InitializeOpusEncoder(SampleRate, 1);
        
		if (!AudioEncoder)
		{
			UE_LOG(LogTemp, Error, TEXT("[CrowdySDK]: Failed to initialize Opus Encoder"));
			return;
		}
	}

	
	const int32 Frame_Size = SampleRate * 1 * CaptureDuration;
	const int32 MaxCompressedSize = Frame_Size * sizeof(float);
	const int32 TotalFrames = (InAudioData.Num() + Frame_Size - 1) / Frame_Size;

	for (int32 FrameIndex = 0; FrameIndex < TotalFrames; ++FrameIndex)
	{
		// Extract or pad input audio frame
		TArray<float> FrameData;
		FrameData.SetNumZeroed(Frame_Size);  // Pre-fill with zeros

		const int32 StartSample = FrameIndex * Frame_Size;
		const int32 CopySize = FMath::Min(Frame_Size, InAudioData.Num() - StartSample);
		FMemory::Memcpy(FrameData.GetData(), InAudioData.GetData() + StartSample, CopySize * sizeof(float));

		TArray<uint8> CompressedAudioData;
		CompressedAudioData.SetNumUninitialized(MaxCompressedSize);

		const int32 EncodedBytes = opus_encode_float(AudioEncoder, FrameData.GetData(), Frame_Size,
													 CompressedAudioData.GetData(), MaxCompressedSize);

		if (EncodedBytes <= 0)
		{
			const char* ErrorMessage = opus_strerror(EncodedBytes);
			UE_LOG(LogTemp, Error, TEXT("[CrowdySDK]: Opus encoding failed: %s"), UTF8_TO_TCHAR(ErrorMessage));
			return;
		}

		UE_LOG(LogTemp, Log, TEXT("[CrowdySDK]:Compressed audio data size: %d"), EncodedBytes);
		
		// Trim the compressed data to fixed size, for easier encode/decode
		CompressedAudioData.SetNum(EncodedBytes, EAllowShrinking::No);

		SendAudioData(CompressedAudioData, EncodedBytes, SampleRate, 1);
	}
	
}

bool FVoiceChatService::DecompressAudioData(OpusDecoder* DecoderToUse, const TArray<uint8>& CompressedData,
	TArray<float>& OutAudioData, int32 SampleRate, int32 Channles)
{
	FScopeLock Lock(&AudioLock);

	if (!DecoderToUse)
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdySDK]: Opus Decoder is null."));
		return false;
	}

	if (CompressedData.Num() <= 0)
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdySDK]: Input audio data is empty."));
		return false;
	}

	// Must Match this from compression side and Capture side. Otherwise, won't work.
	const int32 FrameSize = 1 * SampleRate * CaptureDuration;
	//UE_LOG(LogVoiceService, Log, TEXT("Frame size Decompression: %d"), FrameSize);
	
	const int32 FloatBufferSize = FrameSize * 1;
	OutAudioData.SetNumUninitialized(FloatBufferSize);

	// Encoding
	const int32 DecodedSamples = opus_decode_float(
		DecoderToUse,
		CompressedData.GetData(),
		CompressedData.Num(),
		OutAudioData.GetData(),
		FrameSize,
		0);
	

	if (DecodedSamples <= 0)
	{
		const char* ErrorMsg = opus_strerror(DecodedSamples);
		UE_LOG(LogTemp, Error, TEXT("[CrowdySDK]: Opus decoding failed: %s"), UTF8_TO_TCHAR(ErrorMsg));
		return false;
	}

	//Resizing Float Buffer to actual decoded size. 
	const int32 ActualFloatSize = DecodedSamples * 1;
	OutAudioData.SetNum(ActualFloatSize);


	//UE_LOG(LogVoiceService, Log, TEXT("Decompression successful - Decoded samples: %d, Output Float size: %d bytes, OutAudioData[0]: %f"), DecodedSamples, OutAudioData.Num(), OutAudioData[0]);
	return true;	
}

void FVoiceChatService::SendAudioData(const TArray<uint8>& InAudioData, int32 EncodedBytes, const int32 SampleRate,
	const int32 NumChannels)
{
	FScopeLock Lock(&AudioLock);

	if(InAudioData.IsEmpty() || EncodedBytes <= 0)
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdySDK]: Compressed Payload is empty. Cannot be sent."))
		return;
	}
	
	// Write header once
	if (!bHeaderWritten || SampleRate != CurrentSampleRate || NumChannels != CurrentNumChannels)
	{
		const FInt64Vector ChunkCoordinates = GameSessionSubsystem->GetPlayerCurrentChunkCoordinates();
		AccumulatedAudioPacket = {};
		AccumulatedAudioPacket.AppID = GameSessionSubsystem->GetAppID();
		AccumulatedAudioPacket.ChunkX = ChunkCoordinates.X;
		AccumulatedAudioPacket.ChunkY = ChunkCoordinates.Y;
		AccumulatedAudioPacket.ChunkZ = ChunkCoordinates.Z;
		AccumulatedAudioPacket.UUID = GameSessionSubsystem->GetUUID();
		AccumulatedAudioPacket.SampleRate = SampleRate;
		AccumulatedAudioPacket.NumChannels = NumChannels;
		
		bHeaderWritten = true;
		CurrentSampleRate = SampleRate;
		CurrentNumChannels = NumChannels;
		AccumulatedAudioPacket.Frames.Reset();
	}

	// Append audio data
	FClientAudioFrame AudioFrame;
	AudioFrame.AudioData.Append(InAudioData.GetData(), EncodedBytes);
	AccumulatedAudioPacket.Frames.Add(MoveTemp(AudioFrame));
	
	const int32 PayloadSize = AccumulatedAudioPacket.GetAccumulatedMessageSize();
	const bool bShouldDispatch = (PayloadSize >= MaxPayloadSize) || (PayloadSize >= MinPayloadThreshold);
	
	if (!bShouldDispatch)
		return;
	
	CrowdySDKSubsystem->SendMessage(AccumulatedAudioPacket);
	
	AccumulatedAudioPacket.Frames.Reset();
	bHeaderWritten = false;
}

void FVoiceChatService::HandleClientAudioNotification(const FClientAudioNotification& ClientAudioNotification)
{
	const int32 SampleRate = ClientAudioNotification.SampleRate;
	const int32 NumChannels = ClientAudioNotification.NumChannels;
	const FString& UUID = ClientAudioNotification.UUID;
	FGuid Guid = USerializationFunctionLibrary::ToGuid(UUID);
	
	if (UUID.Equals(GameSessionSubsystem->GetUUID()))
	{
		if (!bOwnerEcho)
		{
			return;
		}
	}
	
	OpusDecoder* Decoder = GetOrCreateDecoder(Guid, SampleRate, NumChannels);
	
	if (!Decoder)
	{
		UE_LOG(LogTemp, Log, TEXT("[CrowdySDK]: Failed to get or create Opus decoder for UUID %s"), *ClientAudioNotification.UUID);
		return;
	}
	
	for (int32 i = 0; i < ClientAudioNotification.Frames.Num(); ++i)
	{
		const FClientAudioNotificationFrame& Frame = ClientAudioNotification.Frames[i];
		
		if (Frame.FrameSize <= 0 || Frame.AudioData.IsEmpty())
		{
			UE_LOG(LogTemp, Warning, TEXT("[CrowdySDK]: Empty Frame %d for UUID %s"), i, *ClientAudioNotification.UUID);
			continue;
		}
		
		// Make sure size matches data
		if (Frame.FrameSize != Frame.AudioData.Num())
		{
			UE_LOG(LogTemp, Warning, TEXT("[CrowdySDK]: FrameSize mismatch on frame %d. FrameSize=%d, AudioData.Num()=%d"),
				   i, Frame.FrameSize, Frame.AudioData.Num());
			// choose one behavior:
			// continue;
			// or just trust AudioData.Num()
		}
		
		TArray<float> Decoded;
		if (!DecompressAudioData(Decoder, Frame.AudioData, Decoded, SampleRate, NumChannels))
		{
			UE_LOG(LogTemp, Error, TEXT("[CrowdySDK]: Decoding failed for UUID %s (frame %d)"), *UUID, i);
			continue;
		}
		// Launch the broadcast on the game thread
		UE::Tasks::Launch(UE_SOURCE_LOCATION, [this, DecodedData = MoveTemp(Decoded), SampleRate, NumChannels, Guid]() mutable 
		{
			VoiceChatManager->HandleIncomingAudio(MoveTemp(DecodedData), SampleRate, NumChannels, Guid);
		}, LowLevelTasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
	}
	
}

void FVoiceChatService::CleanupDecoder(const FGuid& UUID)
{
	FScopeLock Lock(&AudioLock);
	
	if (OpusDecoder** FoundDecoder  = UUIDToDecoderMap.Find(UUID))
	{
		opus_decoder_destroy(*FoundDecoder);
		UUIDToDecoderMap.Remove(UUID);
		UE_LOG(LogTemp, Log, TEXT("[CrowdySDK]: Destroyed Opus Decoder for UUID %s"), *UUID.ToString());
	}
}

void FVoiceChatService::SetVoiceChatManagerReference(UVoiceChatSubsystem* InVoiceChatManager)
{
	VoiceChatManager = InVoiceChatManager;
}

void FVoiceChatService::ToggleOwnerEcho(const bool bEnable)
{
	bOwnerEcho = bEnable;
}

void FVoiceChatService::OnMessageReceived(TSharedRef<ICrowdyMessage> Message)
{
	if (Message->GetType() != ECrowdyMessageType::CLIENT_AUDIO_NOTIFICATION)
	{
		//UE_LOG(LogTemp, Error, TEXT("[CrowdySDK]: Received unexpected message type: %s"), *Message->GetTypeName().ToString());
		return;
	}

	const FClientAudioNotification ClientAudioNotification = static_cast<FClientAudioNotification&>(*Message);
	HandleClientAudioNotification(ClientAudioNotification);
}

TArray<ECrowdyMessageType> FVoiceChatService::GetSupportedResponseTypes() const
{
	return 
	{
		ECrowdyMessageType::CLIENT_AUDIO_NOTIFICATION
	};
}

void FVoiceChatService::InitializeOpusEncoder(int32 SampleRate, int32 Channels)
{
	// Additional validation
	if (Channels < 1 || Channels > 2)
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdySDK]: Invalid channel count. Must be 1 or 2. Got: %d"), Channels);
		return;
	}

	// Validated sample rates for Opus
	const int32 ValidSampleRates[] = {8000, 12000, 16000, 24000, 48000};
	bool bValidSampleRate = false;
	for (const int32 ValidRate : ValidSampleRates)
	{
		if (SampleRate == ValidRate)
		{
			bValidSampleRate = true;
			break;
		}
	}

	if (!bValidSampleRate)
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdySDK]: Invalid sample rate for Opus. Nearest valid rate is 48000"));
		SampleRate = 48000;
	}

	int Error = OPUS_OK;
	AudioEncoder = opus_encoder_create(
	(SampleRate), 
	(Channels), 
	OPUS_APPLICATION_VOIP, 
	&Error
	);

	if (Error != OPUS_OK)
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdySDK]: Opus Encoder creation failed. Error: %d"), Error);
		AudioEncoder = nullptr;
		return;
	}

	// Set encoder parameters
	opus_encoder_ctl(AudioEncoder, OPUS_SET_BITRATE(64000));
	opus_encoder_ctl(AudioEncoder, OPUS_SET_COMPLEXITY(10));
	opus_encoder_ctl(AudioEncoder, OPUS_SET_SIGNAL(OPUS_APPLICATION_VOIP));
}

OpusDecoder* FVoiceChatService::GetOrCreateDecoder(const FGuid& UUID, int32 SampleRate, int32 Channels)
{
	FScopeLock Lock(&AudioLock);

	if (OpusDecoder** FoundDecoder = UUIDToDecoderMap.Find(UUID))
	{
		UE_LOG(LogTemp, Log, TEXT("[CrowdySDK]: Found existing Opus decoder for UUID %s"), *UUID.ToString());
		return *FoundDecoder;
	}

	int ErrorCode = 0;

	OpusDecoder* NewDecoder = opus_decoder_create(
		(SampleRate), 
		(Channels), 
		&ErrorCode
	);

	if (ErrorCode != OPUS_OK || !NewDecoder)
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdySDK]: Failed to create Opus decoder for UUID %s: %d"), *UUID.ToString(), ErrorCode);
		return nullptr;
	}

	UUIDToDecoderMap.Add(UUID, NewDecoder);
	UE_LOG(LogTemp, Log, TEXT("[CrowdySDK]: Created new Opus decoder for UUID %s"), *UUID.ToString());
	return NewDecoder;
}

void FVoiceChatService::CleanupAllDecoders()
{
	FScopeLock Lock(&AudioLock);
    
	for (const auto& Pair : UUIDToDecoderMap)
	{
		if (Pair.Value)
		{
			opus_decoder_destroy(Pair.Value);
		}
	}
	UUIDToDecoderMap.Empty();
	
	UE_LOG(LogTemp, Log, TEXT("[CrowdySDK]: Destroyed all Opus decoders"));
}

void FVoiceChatService::CleanupOpus()
{
	if (AudioEncoder)
	{
		opus_encoder_destroy(AudioEncoder);
		AudioEncoder = nullptr;
	}
	UE_LOG(LogTemp, Log, TEXT("[CrowdySDK]: Destroyed Opus Encoder"));
}
