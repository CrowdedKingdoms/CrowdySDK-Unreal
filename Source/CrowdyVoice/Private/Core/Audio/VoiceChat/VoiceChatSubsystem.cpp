#include "Core/Audio/VoiceChat/VoiceChatSubsystem.h"

#include "Components/AudioComponent.h"
#include "Core/Audio/Platform/CrowdyVoiceDeviceMonitor.h"
#include "Core/Audio/VoiceChat/CrowdyVoiceMic.h"
#include "Core/Audio/VoiceChat/CrowdyVoicePlayback.h"
#include "Core/Audio/VoiceChat/CrowdyVoiceSettings.h"
#include "Core/CrowdySDKBridgeSubsystem.h"
#include "CrowdyCppVoice.h"
#include "CrowdyVoiceLog.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Internal/FCrowdyServiceRegistry.h"
#include "Kismet/GameplayStatics.h"
#include "Messages/Actor/FActorLeftNotification.h"
#include "Messages/Communication/FClientAudioNotification.h"
#include "Messages/Communication/FClientAudioPacketMessageRequest.h"
#include "Sound/SoundWaveProcedural.h"
#include "Subsystem/CrowdyGameSession.h"

namespace
{
	// Coalesces the burst of notifications one device change produces, and gives the new device time to settle.
	constexpr int64 CrowdyVoiceRestartDelayMs = 500;

	int64 CrowdyVoiceNowMs()
	{
		return static_cast<int64>(FPlatformTime::Seconds() * 1000.0);
	}
}

/** The engine-free voice machinery of one world, plus the audio components behind it. */
struct FCrowdyVoiceRuntime final : ICrowdyVoiceSpeakerOutput, ICrowdyVoiceFrameSink
{
	explicit FCrowdyVoiceRuntime(UVoiceChatSubsystem& InOwner)
		: Owner(InOwner)
		, Mic(MakeCrowdyVoiceEngineMicDevice(), *this)
		, Feed(*this)
		, Playback(*this)
	{
	}

	virtual void SendFrame(const TConstArrayView<int16> Frame, const bool bLast) override
	{
		Owner.SendFrame(Frame, bLast);
	}

	virtual void SkipFrames(const int32 Count) override
	{
		Encoder.Skip(Count);
		Owner.Stats.SilentFramesSkipped += Count;
	}

	virtual void OpenSpeaker(const FString& SpeakerKey, const int32 SampleRate) override
	{
		Owner.OpenSpeakerStream(SpeakerKey, SampleRate);
	}

	virtual void PlaySpeaker(const FString& SpeakerKey, const TConstArrayView<int16> Pcm) override
	{
		Owner.QueueSpeakerAudio(SpeakerKey, Pcm);
	}

	virtual void CloseSpeaker(const FString& SpeakerKey) override
	{
		Owner.CloseSpeakerStream(SpeakerKey);
	}

	// Collected and broadcast once the update is over, so a listener never runs mid-update.
	virtual void SpeakerTalking(const FString& SpeakerKey, const bool bTalking) override
	{
		TalkingChanges.Emplace(SpeakerKey, bTalking);
	}

	UVoiceChatSubsystem& Owner;
	FCrowdyVoiceMic Mic;
	FCrowdyVoiceFeed Feed;
	FCrowdyVoicePlayback Playback;
	FCrowdyCppVoiceEncoder Encoder;
	FCrowdyVoiceDeviceMonitor Monitor;
	FClientAudioPacketMessageRequest OutPacket;
	TArray<TPair<FString, bool>> TalkingChanges;
};

bool UVoiceChatSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}

	const UWorld* World = Outer ? Outer->GetWorld() : nullptr;
	return World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE);
}

void UVoiceChatSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Null in the transient world the engine creates before any game instance exists.
	const UWorld* World = GetWorld();
	if (!World || !World->GetGameInstance())
	{
		UE_CLOG(CrowdyVoiceTrace::Voice(), LogCrowdyVoice, Log, TEXT("Voice chat is inactive in a world with no game instance."));
		return;
	}

	Runtime = MakePimpl<FCrowdyVoiceRuntime>(*this);
	ConnectToSession();
}

void UVoiceChatSubsystem::Deinitialize()
{
	AudioSubscription.Release();
	ActorLeftSubscription.Release();

	if (UCrowdyGameSession* GameSession = Session.Get())
	{
		GameSession->OnOwnerUUIDUpdated.RemoveAll(this);
	}

	if (Runtime)
	{
		Runtime->Feed.End();
		Runtime->Mic.Stop();
		Runtime->Playback.ForgetAll();
		BroadcastChanges();
	}

	SpeakerStreams.Empty();
	Runtime.Reset();
	OnMicStateChanged.Clear();
	OnSpeakerTalking.Clear();

	Super::Deinitialize();
}

TStatId UVoiceChatSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UVoiceChatSubsystem, STATGROUP_Tickables);
}

void UVoiceChatSubsystem::Tick(const float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!Runtime)
	{
		return;
	}

	if (!Session.IsValid() || !AudioSubscription.IsValid())
	{
		ConnectToSession();
	}

	const int64 NowMs = CrowdyVoiceNowMs();
	if (Runtime->Monitor.ConsumeCaptureDeviceChange() && Runtime->Mic.GetState() != ECrowdyVoiceMicState::Off)
	{
		RestartAtMs = NowMs + CrowdyVoiceRestartDelayMs;
	}

	if (RestartAtMs != 0 && NowMs >= RestartAtMs)
	{
		RestartAtMs = 0;
		Runtime->Mic.Restart(NowMs);
	}

	if (!Runtime->Monitor.IsWatching())
	{
		Runtime->Mic.RetryIfNoDevice(NowMs);
	}

	// A microphone that reopened by itself takes the stream over, so a fed spurt still open ends first.
	const ECrowdyVoiceMicState MicState = Runtime->Mic.GetState();
	if (MicState == ECrowdyVoiceMicState::Starting || MicState == ECrowdyVoiceMicState::Capturing)
	{
		Runtime->Feed.End();
	}
	Runtime->Mic.Pump(NowMs);
	Runtime->Playback.Update(NowMs, StreamTimeoutMs);

	const FCrowdyCppVoiceReceiver& Receiver = Runtime->Playback.GetReceiver();
	Stats.FramesConcealed = Receiver.GetConcealedFrames();
	Stats.PacketsMalformed = Receiver.GetMalformedPackets();
	Stats.Speakers = Runtime->Playback.NumSpeakers();
	BroadcastChanges();
}

void UVoiceChatSubsystem::StartVoiceChat()
{
	if (!Runtime)
	{
		UE_LOG(LogCrowdyVoice, Warning, TEXT("Voice chat is not available in this world, so the microphone stays closed."));
		return;
	}

	if (!Runtime->Encoder.IsValid())
	{
		Runtime->Mic.Fail(TEXT("Voice chat cannot encode audio on this platform, so the microphone stays closed."));
		BroadcastChanges();
		return;
	}

	// A fed spurt ends first, so the microphone's frames start a spurt of their own.
	Runtime->Feed.End();
	Runtime->Mic.Start(CrowdyVoiceNowMs());
	BroadcastChanges();
}

void UVoiceChatSubsystem::StopVoiceChat()
{
	if (!Runtime)
	{
		return;
	}

	RestartAtMs = 0;
	Runtime->Mic.Stop();
	BroadcastChanges();
}

void UVoiceChatSubsystem::PlayVoiceChat()
{
	if (Runtime)
	{
		Runtime->Playback.SetPlaying(true);
	}
}

void UVoiceChatSubsystem::MuteVoiceChat()
{
	if (Runtime)
	{
		Runtime->Playback.SetPlaying(false);
	}
}

void UVoiceChatSubsystem::SetStreamTimeoutThreshold(const float InSeconds)
{
	const float Seconds = FCrowdyVoiceSettings::ClampStreamTimeout(InSeconds);
	UE_CLOG(Seconds != InSeconds, LogCrowdyVoice, Warning,
		TEXT("Voice chat stream timeout %.2f s is outside %.1f to %.0f s; using %.2f s."),
		InSeconds, FCrowdyVoiceSettings::MinStreamTimeoutSeconds, FCrowdyVoiceSettings::MaxStreamTimeoutSeconds, Seconds);
	StreamTimeoutMs = static_cast<int64>(Seconds * 1000.0f);
}

void UVoiceChatSubsystem::SetOwnerEcho(const bool bEnable)
{
	bOwnerEcho = bEnable;
}

void UVoiceChatSubsystem::SetDistance(const int32 Chunks)
{
	DistanceChunks = FCrowdyVoiceSettings::ClampDistance(Chunks);
	UE_CLOG(DistanceChunks != Chunks, LogCrowdyVoice, Warning,
		TEXT("Voice chat distance %d is outside 0 to %d chunks; using %d."), Chunks, FCrowdyVoiceSettings::MaxDistanceChunks, DistanceChunks);
}

bool UVoiceChatSubsystem::FeedVoicePcm(const TConstArrayView<int16> Samples)
{
	if (!Runtime || !Runtime->Encoder.IsValid())
	{
		return false;
	}

	const ECrowdyVoiceMicState MicState = Runtime->Mic.GetState();
	if (MicState == ECrowdyVoiceMicState::Starting || MicState == ECrowdyVoiceMicState::Capturing)
	{
		return false;
	}

	Runtime->Feed.Push(Samples);
	BroadcastChanges();
	return true;
}

void UVoiceChatSubsystem::EndVoiceFeed()
{
	if (!Runtime)
	{
		return;
	}
	Runtime->Feed.End();
	BroadcastChanges();
}

ECrowdyVoiceMicState UVoiceChatSubsystem::GetMicState() const
{
	return Runtime ? Runtime->Mic.GetState() : ECrowdyVoiceMicState::Off;
}

bool UVoiceChatSubsystem::IsPlaying() const
{
	return Runtime && Runtime->Playback.IsPlaying();
}

void UVoiceChatSubsystem::ConnectToSession()
{
	const UWorld* World = GetWorld();
	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	if (!GameInstance)
	{
		return;
	}

	// Read once as well as subscribed to, because the id is normally settled before this world exists.
	UCrowdyGameSession* GameSession = GameInstance->GetSubsystem<UCrowdyGameSession>();
	if (GameSession && !Session.IsValid())
	{
		Session = GameSession;
		HandleOwnerUUIDUpdated(GameSession->GetUUID());
		GameSession->OnOwnerUUIDUpdated.AddUniqueDynamic(this, &UVoiceChatSubsystem::HandleOwnerUUIDUpdated);
	}

	UCrowdySDKBridgeSubsystem* SdkBridge = GameInstance->GetSubsystem<UCrowdySDKBridgeSubsystem>();
	if (AudioSubscription.IsValid() || !SdkBridge || !SdkBridge->ServiceRegistry)
	{
		return;
	}

	Bridge = SdkBridge;
	AudioSubscription = SdkBridge->ServiceRegistry->SubscribeToOpcode<FClientAudioNotification>(
		ECrowdyMessageType::CLIENT_AUDIO_NOTIFICATION,
		{ ECrowdySubscriptionRole::Observe, false, TEXT("CrowdyVoice") },
		[this](const FClientAudioNotification& Notification, const FCrowdyDelivery&) { HandleAudio(Notification); });
	ActorLeftSubscription = SdkBridge->ServiceRegistry->SubscribeToOpcode<FActorLeftNotification>(
		ECrowdyMessageType::ACTOR_LEFT_NOTIFICATION,
		{ ECrowdySubscriptionRole::Observe, false, TEXT("CrowdyVoice") },
		[this](const FActorLeftNotification& Notification, const FCrowdyDelivery&) { HandleActorLeft(Notification); });

	UE_CLOG(CrowdyVoiceTrace::Voice(), LogCrowdyVoice, Log, TEXT("Voice chat is receiving."));
}

void UVoiceChatSubsystem::HandleAudio(const FClientAudioNotification& Notification)
{
	if (!Runtime)
	{
		return;
	}

	// The server returns a sender's own voice to it, which plays only as owner echo.
	if (!bOwnerEcho && LocalSenderId.IsSet() && Notification.UUID == LocalSenderId)
	{
		return;
	}
	++Stats.PacketsReceived;
	Runtime->Playback.Push(Notification.UUID.ToString(), Notification.Payload, CrowdyVoiceNowMs());
}

void UVoiceChatSubsystem::HandleActorLeft(const FActorLeftNotification& Notification)
{
	if (Runtime)
	{
		Runtime->Playback.Forget(Notification.UUID.ToString());
	}
}

void UVoiceChatSubsystem::HandleOwnerUUIDUpdated(const FString NewUUID)
{
	LocalSenderId = FCrowdyActorId::FromStringOrUnset(NewUUID);
}

void UVoiceChatSubsystem::SendFrame(const TConstArrayView<int16> Frame, const bool bLast)
{
	FCrowdyVoiceRuntime& Voice = *Runtime;
	UCrowdyGameSession* GameSession = Session.Get();
	const UCrowdySDKBridgeSubsystem* SdkBridge = Bridge.Get();
	if (!GameSession || !SdkBridge || !SdkBridge->SendMessageFn || !LocalSenderId.IsSet())
	{
		// Keeps the stream's clock moving over the frames that could not be sent.
		Voice.Encoder.Skip(1);
		UE_CLOG(!bWarnedNoSend && CrowdyVoiceTrace::Voice(), LogCrowdyVoice, Log,
			TEXT("Voice chat is capturing but drops frames until the session is connected and has an id."));
		bWarnedNoSend = true;
		return;
	}

	if (!Voice.Encoder.Encode(Frame, bLast, Voice.OutPacket.Payload))
	{
		UE_CLOG(!bWarnedEncode, LogCrowdyVoice, Warning, TEXT("Voice chat could not encode a microphone frame and dropped it."));
		bWarnedEncode = true;
		return;
	}

	// Between pawns (a respawn, the end of a map) the voice stays where the pawn last stood.
	FInt64Vector Chunk;
	if (GameSession->TryGetLocalPlayerChunk(GetWorld(), Chunk))
	{
		LastPawnChunk = Chunk;
		bHasPawnChunk = true;
	}
	else
	{
		Chunk = bHasPawnChunk ? LastPawnChunk : GameSession->GetPlayerCurrentChunkCoordinates();
	}
	Voice.OutPacket.AppID = GameSession->GetAppID();
	Voice.OutPacket.ChunkX = Chunk.X;
	Voice.OutPacket.ChunkY = Chunk.Y;
	Voice.OutPacket.ChunkZ = Chunk.Z;
	Voice.OutPacket.ReplicationDistance = static_cast<ECrowdyReplicationDistance>(DistanceChunks);
	Voice.OutPacket.UUID = LocalSenderId;
	SdkBridge->SendMessageFn(Voice.OutPacket);
	++Stats.PacketsSent;
}

void UVoiceChatSubsystem::BroadcastChanges()
{
	if (!Runtime)
	{
		return;
	}

	const ECrowdyVoiceMicState MicState = Runtime->Mic.GetState();
	if (MicState != BroadcastMicState)
	{
		BroadcastMicState = MicState;
		OnMicStateChanged.Broadcast(MicState);
	}

	// Moved out first: a listener may stop or mute voice chat, which can queue more changes.
	TArray<TPair<FString, bool>> Changes = MoveTemp(Runtime->TalkingChanges);
	Runtime->TalkingChanges.Reset();
	for (const TPair<FString, bool>& Change : Changes)
	{
		OnSpeakerTalking.Broadcast(Change.Key, Change.Value);
	}
}

void UVoiceChatSubsystem::OpenSpeakerStream(const FString& SpeakerKey, const int32 SampleRate)
{
	if (SpeakerStreams.Contains(SpeakerKey) || SpeakerStreams.Num() >= FCrowdyCppVoiceReceiver::MaxSpeakers)
	{
		return;
	}

	USoundWaveProcedural* Wave = NewObject<USoundWaveProcedural>(this);
	Wave->SetSampleRate(static_cast<uint32>(SampleRate));
	Wave->NumChannels = 1;
	Wave->Duration = INDEFINITELY_LOOPING_DURATION;
	Wave->SoundGroup = SOUNDGROUP_Voice;
	Wave->bLooping = false;

	// Registered with the world rather than attached to any actor, so it plays whatever happens to the pawn.
	UAudioComponent* Component = UGameplayStatics::CreateSound2D(this, Wave, 1.0f, 1.0f, 0.0f, nullptr, false, false);
	UE_CLOG(!Component && CrowdyVoiceTrace::Voice(), LogCrowdyVoice, Log,
		TEXT("Voice chat has no audio device to play %s on."), *SpeakerKey);

	FCrowdyVoiceSpeakerStream& Stream = SpeakerStreams.Add(SpeakerKey);
	Stream.Wave = Wave;
	Stream.Component = Component;
	Stream.SampleRate = SampleRate;
}

void UVoiceChatSubsystem::QueueSpeakerAudio(const FString& SpeakerKey, const TConstArrayView<int16> Pcm)
{
	FCrowdyVoiceSpeakerStream* Stream = SpeakerStreams.Find(SpeakerKey);
	if (!Stream || !Stream->Wave || !IsValid(Stream->Component))
	{
		return;
	}

	// The newest frame is dropped when the queue is full: clearing it from here would race the audio thread.
	const int32 FrameBytes = Pcm.Num() * static_cast<int32>(sizeof(int16));
	const int32 QueuedBytes = Stream->Wave->GetAvailableAudioByteCount();
	if (!FCrowdyVoiceQueuePolicy::Admit(QueuedBytes, FrameBytes, Stream->SampleRate, Stats))
	{
		return;
	}
	Stream->Wave->QueueAudio(reinterpret_cast<const uint8*>(Pcm.GetData()), FrameBytes);

	if (!Stream->Component->IsPlaying() && FCrowdyVoiceQueuePolicy::ShouldStart(QueuedBytes + FrameBytes, Stream->SampleRate))
	{
		Stream->Component->Play();
	}
}

void UVoiceChatSubsystem::CloseSpeakerStream(const FString& SpeakerKey)
{
	FCrowdyVoiceSpeakerStream Stream;
	if (!SpeakerStreams.RemoveAndCopyValue(SpeakerKey, Stream) || !IsValid(Stream.Component))
	{
		return;
	}
	Stream.Component->Stop();
	Stream.Component->DestroyComponent();
}
