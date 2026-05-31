#pragma once
#include "opus.h"
#include "Core/UDP/Interfaces/ICrowdyReceptionLayer.h"
#include "Messages/Communication/FClientAudioPacketMessageRequest.h"

class UCrowdySDKSubsystem;
class UCrowdyGameSession;
class UVoiceChatSubsystem;
struct FClientAudioNotification;

class FVoiceChatService : public ICrowdyReceptionLayer
{
	
public:
	
	FVoiceChatService(UCrowdySDKSubsystem* InCrowdySDK, UCrowdyGameSession* InGameSession);
	virtual ~FVoiceChatService() override;
	
	void CompressAudioData(const TArray<float>& InAudioData, int32 SampleRate, int32 NumChannels);
	bool DecompressAudioData(OpusDecoder* DecoderToUse, const TArray<uint8>& CompressedData, TArray<float>& OutAudioData, int32 SampleRate, int32 Channles);
	void SendAudioData(const TArray<uint8>& InAudioData, int32 EncodedBytes, const int32 SampleRate, const int32 NumChannels);
	void HandleClientAudioNotification(const FClientAudioNotification& ClientAudioNotification);
	void CleanupDecoder(const FGuid& UUID);
	void SetVoiceChatManagerReference(UVoiceChatSubsystem* InVoiceChatManager);
	void ToggleOwnerEcho(bool bEnable);
	virtual void OnMessageReceived(TSharedRef<ICrowdyMessage> Message) override;
	virtual TArray<ECrowdyMessageType> GetSupportedResponseTypes() const override;
	
private:
	
	// Opus Vars
	OpusEncoder* AudioEncoder;
	OpusDecoder* AudioDecoder;

	// UUID to Opus Decoder Map
	TMap<FGuid, OpusDecoder*> UUIDToDecoderMap;

	// Chat Manager References
	UVoiceChatSubsystem* VoiceChatManager;

	// Syncing And Threading
	FCriticalSection AudioLock;

	// Sending Payload Related vars
	TArray<uint8> AccumulatedPayload;
	FClientAudioPacketMessageRequest AccumulatedAudioPacket;
	const int32 MaxPayloadSize = 4096;
	const int32 MinPayloadThreshold = 212; // To ensure the total packet size does not exceed 256 Bytes

	// Timestamp Calculation for Sending Function 
	double LastSendTimestamp = 0.0f;
	const double MaxAggregationTime = 0.1f;

	//Send Header
	bool bHeaderWritten = false;

	// Audio Params
	int32 CurrentSampleRate = 0;
	int32 CurrentNumChannels = 0;
	int32 FrameCount;
	const float CaptureDuration = 0.02f; // 20ms
	
	// Core References
	UCrowdyGameSession* GameSessionSubsystem;
	UCrowdySDKSubsystem* CrowdySDKSubsystem;
	
	bool bOwnerEcho = false;
	
	// Opus Functions
	void InitializeOpusEncoder(int32 SampleRate, int32 Channels);
	OpusDecoder* GetOrCreateDecoder(const FGuid& UUID, int32 SampleRate, int32 Channels);
	void CleanupAllDecoders();
	void CleanupOpus();
};
