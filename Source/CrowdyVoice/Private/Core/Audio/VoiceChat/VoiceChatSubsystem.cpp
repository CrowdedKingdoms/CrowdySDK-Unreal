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
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Internal/FCrowdyServiceRegistry.h"
#include "Kismet/GameplayStatics.h"
#include "Messages/Actor/FActorLeftNotification.h"
#include "Messages/Channels/FChannelMessages.h"
#include "Messages/Communication/FClientAudioNotification.h"
#include "Messages/Communication/FClientAudioPacketMessageRequest.h"
#include "Network/CrowdyCpp/CrowdyCppReplicationSubsystem.h"
#include "Sound/SoundWaveProcedural.h"
#include "Subsystem/CrowdyGameSession.h"

namespace
{
	// Coalesces the burst of notifications one device change produces, and gives the new device time to settle.
	constexpr int64 CrowdyVoiceRestartDelayMs = 500;

	// How long a missing microphone keeps being retried after a device arrives.
	constexpr int64 CrowdyVoiceArrivalRetryMs = 10000;

	int64 CrowdyVoiceNowMs()
	{
		return static_cast<int64>(FPlatformTime::Seconds() * 1000.0);
	}
}

/** The engine-free voice machinery of one world, plus the audio components behind it. */
struct FCrowdyVoiceRuntime final : ICrowdyVoiceSpeakerOutput, ICrowdyVoiceFrameSink
{
	FCrowdyVoiceRuntime(UVoiceChatSubsystem& InOwner, const UCrowdyVoiceDeveloperSettings& Settings)
		: Owner(InOwner)
		, Mic(MakeCrowdyVoiceEngineMicDevice(), *this)
		, Feed(*this)
		, Playback(*this, FMath::Clamp<int64>(Settings.PlayoutDelayMs, 40, 400), Settings.MaxSpeakers)
		, Encoder(FMath::Clamp(Settings.BitrateBps, 6000, 64000))
	{
		Mic.ConfigureGate(Settings.SilenceGateDb, Settings.SilenceHoldMs);
		Feed.ConfigureGate(Settings.SilenceGateDb, Settings.SilenceHoldMs);
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
		const UVoiceChatSubsystem::FSpeakerInfo* Info = Owner.SpeakerInfos.Find(SpeakerKey);
		TalkingChanges.Add({ SpeakerKey, Info ? Info->ChannelId : 0, bTalking });
	}

	UVoiceChatSubsystem& Owner;
	FCrowdyVoiceMic Mic;
	FCrowdyVoiceFeed Feed;
	FCrowdyVoicePlayback Playback;
	FCrowdyCppVoiceEncoder Encoder;
	FCrowdyVoiceDeviceMonitor Monitor;
	FClientAudioPacketMessageRequest OutPacket;
	FChannelAudioRequest OutChannelPacket;

	struct FTalkingChange
	{
		FString SpeakerKey;
		int64 ChannelId = 0;
		bool bTalking = false;
	};
	TArray<FTalkingChange> TalkingChanges;
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

	ReadSettings();
	Runtime = MakePimpl<FCrowdyVoiceRuntime>(*this, *GetDefault<UCrowdyVoiceDeveloperSettings>());
	ConnectToSession();
}

void UVoiceChatSubsystem::ReadSettings()
{
	const UCrowdyVoiceDeveloperSettings* Settings = GetDefault<UCrowdyVoiceDeveloperSettings>();
	PlaybackMode = Settings->PlaybackMode;
	if (PlaybackMode != ECrowdyVoicePlaybackMode::Spatial)
	{
		return;
	}
	OcclusionCheckSeconds = 1.0f / static_cast<float>(FMath::Clamp(Settings->OcclusionChecksPerSecond, 1, 30));

	// Without a Channel Attenuation, panned but never quieter with distance; a speaker the world cannot place plays centred.
	bDirectionalChannelVoice = Settings->bDirectionalChannelVoice;
	ChannelAttenuation.bAttenuate = false;
	ChannelAttenuation.bSpatialize = true;
	ChannelAttenuation.bEnableReverbSend = false;
	ChannelFlatAttenuation.bAttenuate = false;
	ChannelFlatAttenuation.bSpatialize = false;
	ChannelFlatAttenuation.bEnableReverbSend = false;
	const bool bChannelAssetNamed = bDirectionalChannelVoice && !Settings->ChannelAttenuation.IsNull();
	ChannelAttenuationAsset = bChannelAssetNamed ? Settings->ChannelAttenuation.LoadSynchronous() : nullptr;
	UE_CLOG(bChannelAssetNamed && !ChannelAttenuationAsset, LogCrowdyVoice, Warning, TEXT("Voice chat could not load its Channel Attenuation %s, so channel voice keeps full volume at any distance."),
		*Settings->ChannelAttenuation.ToString());
	if (ChannelAttenuationAsset)
	{
		ChannelAttenuation = ChannelAttenuationAsset->Attenuation;
	}

	AttenuationAsset = Settings->Attenuation.LoadSynchronous();
	if (AttenuationAsset)
	{
		return;
	}

	// A plain fade over the same range as the SDK's own asset, for a project whose cook left it out.
	UE_LOG(LogCrowdyVoice, Warning, TEXT("Voice chat could not load its Attenuation %s, so voices fade linearly from 2 m to 40 m, with no muffling."),
		*Settings->Attenuation.ToString());
	SpatialAttenuation.bAttenuate = true;
	SpatialAttenuation.bSpatialize = true;
	SpatialAttenuation.DistanceAlgorithm = EAttenuationDistanceModel::Linear;
	SpatialAttenuation.AttenuationShape = EAttenuationShape::Sphere;
	SpatialAttenuation.AttenuationShapeExtents = FVector(200.0, 0.0, 0.0);
	SpatialAttenuation.FalloffDistance = 3800.0f;
}

void UVoiceChatSubsystem::Deinitialize()
{
	AudioSubscription.Release();
	ChannelAudioSubscription.Release();
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
	SpeakerInfos.Empty();
	Runtime.Reset();
	OnMicStateChanged.Clear();
	OnSpeakerTalking.Clear();
	OnChannelSpeakerTalking.Clear();
	LocateSpeaker = nullptr;
	ResolveSpeakerUser = nullptr;

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
	const bool bDefaultChanged = Runtime->Monitor.ConsumeCaptureDeviceChange();
	const bool bArrived = Runtime->Monitor.ConsumeDeviceArrival();
	const bool bFollowsDefault = Runtime->Mic.GetDeviceId().IsEmpty();
	const ECrowdyVoiceMicState Before = Runtime->Mic.GetState();
	// A chosen microphone raises no default change when it comes back, only an arrival.
	const bool bChosenMayBeBack = bArrived && !bFollowsDefault && Before == ECrowdyVoiceMicState::NoDevice;
	if (((bDefaultChanged && bFollowsDefault) || bChosenMayBeBack) && Before != ECrowdyVoiceMicState::Off)
	{
		RestartAtMs = NowMs + CrowdyVoiceRestartDelayMs;
	}
	if (bArrived)
	{
		ArrivalRetryUntilMs = NowMs + CrowdyVoiceArrivalRetryMs;
	}

	if (RestartAtMs != 0 && NowMs >= RestartAtMs)
	{
		RestartAtMs = 0;
		Runtime->Mic.Restart(NowMs);
	}

	// Polled where changes cannot be seen, and briefly after an arrival, since a device can take a moment to become usable:
	// one just plugged in can fail to open or deliver at first, so a failure is retried in that window too.
	const bool bAfterArrival = NowMs < ArrivalRetryUntilMs;
	if (!Runtime->Monitor.IsWatching() || bAfterArrival)
	{
		Runtime->Mic.RetryIfNoDevice(NowMs, bAfterArrival);
	}

	// A microphone that reopened by itself takes the stream over, so a fed spurt still open ends first.
	const ECrowdyVoiceMicState MicState = Runtime->Mic.GetState();
	if (MicState == ECrowdyVoiceMicState::Starting || MicState == ECrowdyVoiceMicState::Capturing)
	{
		Runtime->Feed.End();
	}
	Runtime->Mic.Pump(NowMs);
	Runtime->Playback.Update(NowMs, StreamTimeoutMs);
	UpdateListenerAndStreams(NowMs);

	const FCrowdyCppVoiceReceiver& Receiver = Runtime->Playback.GetReceiver();
	Stats.FramesConcealed = Receiver.GetConcealedFrames();
	Stats.PacketsMalformed = Receiver.GetMalformedPackets();
	Stats.PacketsOverCap = Runtime->Playback.GetPacketsOverCap() + PacketsUntracked;
	Stats.SpeakersDisplaced = Runtime->Playback.GetSpeakersDisplaced();
	Stats.Speakers = Runtime->Playback.NumSpeakers();
	BroadcastChanges();
}

void UVoiceChatSubsystem::UpdateListenerAndStreams(const int64 NowMs)
{
	const APlayerController* Controller = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	FVector Front;
	FVector Right;
	bHasListener = Controller != nullptr;
	if (Controller)
	{
		Controller->GetAudioListenerPosition(ListenerLocation, Front, Right);
	}

	// Who left without a goodbye: an id is new at each sign-in, so this would otherwise grow for the whole session.
	if (NowMs >= NextInfoPruneMs)
	{
		NextInfoPruneMs = NowMs + 5000;
		for (auto It = SpeakerInfos.CreateIterator(); It; ++It)
		{
			if (NowMs - It.Value().LastSeenMs > 60000 && !IsSpeakerHeard(It.Key()))
			{
				It.RemoveCurrent();
			}
		}
	}

	if (PlaybackMode != ECrowdyVoicePlaybackMode::Spatial)
	{
		return;
	}
	for (TPair<FString, FCrowdyVoiceSpeakerStream>& Pair : SpeakerStreams)
	{
		const FSpeakerInfo* Info = SpeakerInfos.Find(Pair.Key);
		if (!Info || !IsValid(Pair.Value.Component))
		{
			continue;
		}
		FVector Location;
		if (Info->ChannelId == 0 && LocateSpeakerNow(*Info, Location))
		{
			Pair.Value.Component->SetWorldLocation(Location);
		}
		else if (Info->ChannelId != 0 && bDirectionalChannelVoice)
		{
			PlaceChannelStream(Pair.Value, *Info, NowMs, false);
		}
	}
}

bool UVoiceChatSubsystem::LocateChannelSpeaker(const FSpeakerInfo& Info, FVector& OutLocation) const
{
	// A channel packet carries no position, so only a speaker the world draws can be placed.
	return LocateSpeaker && Info.Id.IsValid() && LocateSpeaker(Info.Id, OutLocation);
}

#if WITH_DEV_AUTOMATION_TESTS
bool UVoiceChatSubsystem::PlaceChannelStreamForTest(const FString& SpeakerKey, FCrowdyVoiceSpeakerStream& Stream, const int64 NowMs, const bool bForce)
{
	ChannelAttenuation.bSpatialize = true;
	ChannelFlatAttenuation.bSpatialize = false;
	const FSpeakerInfo* Info = SpeakerInfos.Find(SpeakerKey);
	if (Info && IsValid(Stream.Component))
	{
		PlaceChannelStream(Stream, *Info, NowMs, bForce);
	}
	return IsValid(Stream.Component) && Stream.Component->bOverrideAttenuation && Stream.Component->AttenuationOverrides.bSpatialize;
}
#endif

void UVoiceChatSubsystem::PlaceChannelStream(FCrowdyVoiceSpeakerStream& Stream, const FSpeakerInfo& Info, const int64 NowMs, const bool bForce)
{
	FVector Location;
	const bool bPlaced = LocateChannelSpeaker(Info, Location);
	if (bPlaced)
	{
		Stream.Component->SetWorldLocation(Location);
	}
	if (!bForce && (bPlaced == Stream.bPlaced || NowMs < Stream.NextSwitchMs))
	{
		return;
	}
	Stream.bPlaced = bPlaced;
	Stream.NextSwitchMs = NowMs + 1000;
	UE_CLOG(CrowdyVoiceTrace::Voice(), LogCrowdyVoice, Log, TEXT("Voice chat plays channel speaker %s %s."),
		*Info.Id.ToString(), bPlaced ? TEXT("from where they stand") : TEXT("centred, as they are not drawn here"));
	// A playing sound keeps its last left and right mix when spatialization changes, so it stops and the next voice frame plays it fresh.
	if (!bForce && Stream.Component->IsPlaying())
	{
		Stream.Component->Stop();
	}
	Stream.Component->AdjustAttenuation(bPlaced ? ChannelAttenuation : ChannelFlatAttenuation);
}

bool UVoiceChatSubsystem::LocateSpeakerNow(const FSpeakerInfo& Info, FVector& OutLocation) const
{
	if (LocateSpeaker && Info.Id.IsValid() && LocateSpeaker(Info.Id, OutLocation))
	{
		return true;
	}
	OutLocation = Info.FallbackLocation;
	return Info.LastSeenMs != 0;
}

void UVoiceChatSubsystem::SetUserMuted(const int64 UserId, const bool bMuted)
{
	if (UserId == 0)
	{
		return;
	}
	if (!bMuted)
	{
		MutedUsers.Remove(UserId);
		return;
	}

	MutedUsers.Add(UserId);
	for (const TPair<FString, FSpeakerInfo>& Pair : SpeakerInfos)
	{
		if (Pair.Value.UserId == UserId && Runtime)
		{
			Runtime->Playback.Forget(Pair.Key);
		}
	}
}

void UVoiceChatSubsystem::SetUserVolume(const int64 UserId, const float Volume)
{
	if (UserId == 0)
	{
		return;
	}
	const float Clamped = FMath::IsFinite(Volume) ? FMath::Clamp(Volume, 0.0f, MaxUserVolume) : 1.0f;
	if (Clamped == 1.0f)
	{
		UserVolumes.Remove(UserId);
	}
	else
	{
		UserVolumes.Add(UserId, Clamped);
	}

	for (const TPair<FString, FSpeakerInfo>& Pair : SpeakerInfos)
	{
		if (Pair.Value.UserId == UserId)
		{
			ApplySpeakerVolume(Pair.Key);
		}
	}
}

float UVoiceChatSubsystem::GetUserVolume(const int64 UserId) const
{
	const float* Volume = UserVolumes.Find(UserId);
	return Volume ? *Volume : 1.0f;
}

int64 UVoiceChatSubsystem::GetSpeakerUserId(const FString& SpeakerUuid) const
{
	const FSpeakerInfo* Info = SpeakerInfos.Find(SpeakerUuid);
	if (Info && Info->UserId != 0)
	{
		return Info->UserId;
	}

	// Heard only on a channel: its stream is keyed by channel as well.
	FGuid Id;
	return FGuid::Parse(SpeakerUuid, Id) ? KnownUserOf(Id) : 0;
}

int64 UVoiceChatSubsystem::KnownUserOf(const FGuid& SpeakerId) const
{
	for (const TPair<FString, FSpeakerInfo>& Pair : SpeakerInfos)
	{
		if (Pair.Value.Id == SpeakerId && Pair.Value.UserId != 0)
		{
			return Pair.Value.UserId;
		}
	}
	return 0;
}

float UVoiceChatSubsystem::SpeakerVolume(const FSpeakerInfo& Info) const
{
	const float ChannelVolume = Info.ChannelId != 0 ? GetChannelVolume(Info.ChannelId) : 1.0f;
	return PlaybackVolume * GetUserVolume(Info.UserId) * ChannelVolume;
}

void UVoiceChatSubsystem::ApplySpeakerVolume(const FString& SpeakerKey)
{
	const FCrowdyVoiceSpeakerStream* Stream = SpeakerStreams.Find(SpeakerKey);
	const FSpeakerInfo* Info = SpeakerInfos.Find(SpeakerKey);
	if (!Stream || !Info || !IsValid(Stream->Component))
	{
		return;
	}
	Stream->Component->SetVolumeMultiplier(SpeakerVolume(*Info));
}

void UVoiceChatSubsystem::SetPlaybackVolume(const float Volume)
{
	PlaybackVolume = FCrowdyVoiceSettings::ClampPlaybackVolume(Volume);
	for (const TPair<FString, FCrowdyVoiceSpeakerStream>& Pair : SpeakerStreams)
	{
		ApplySpeakerVolume(Pair.Key);
	}
}

TArray<FCrowdyVoiceInputDevice> UVoiceChatSubsystem::GetInputDevices()
{
	if (!ensureMsgf(IsInGameThread(), TEXT("Voice input devices are listed on the game thread only.")))
	{
		return {};
	}
	return ListCrowdyVoiceInputDevices();
}

void UVoiceChatSubsystem::SetInputDevice(const FString& DeviceId)
{
	if (!Runtime)
	{
		return;
	}
	// A reopen already done makes a scheduled restart a second, needless one; without one, the restart must still run.
	if (Runtime->Mic.SetDevice(DeviceId, CrowdyVoiceNowMs()))
	{
		RestartAtMs = 0;
	}
}

FString UVoiceChatSubsystem::GetInputDevice() const
{
	return Runtime ? Runtime->Mic.GetDeviceId() : FString();
}

FString UVoiceChatSubsystem::ChannelStreamKey(const int64 ChannelId, const FString& SpeakerUuid)
{
	return FString::Printf(TEXT("%lld:%s"), ChannelId, *SpeakerUuid);
}

ECrowdyChannelVoiceMode UVoiceChatSubsystem::GetChannelVoiceMode(const int64 ChannelId) const
{
	const ECrowdyChannelVoiceMode* Mode = VoiceChannels.Find(ChannelId);
	return Mode ? *Mode : ECrowdyChannelVoiceMode::Listen;
}

bool UVoiceChatSubsystem::TalksOnAnyChannel() const
{
	for (const TPair<int64, ECrowdyChannelVoiceMode>& Pair : VoiceChannels)
	{
		if (Pair.Value == ECrowdyChannelVoiceMode::Talk)
		{
			return true;
		}
	}
	return false;
}

void UVoiceChatSubsystem::SetChannelVoice(const int64 ChannelId, const bool bOn, const ECrowdyChannelVoiceMode Mode)
{
	if (ChannelId == 0)
	{
		return;
	}
	if (bOn)
	{
		VoiceChannels.Add(ChannelId, Mode);
		return;
	}

	VoiceChannels.Remove(ChannelId);
	for (const TPair<FString, FSpeakerInfo>& Pair : SpeakerInfos)
	{
		if (Pair.Value.ChannelId == ChannelId && Runtime)
		{
			Runtime->Playback.Forget(Pair.Key);
		}
	}
}

void UVoiceChatSubsystem::SetChannelMuted(const int64 ChannelId, const bool bMuted)
{
	if (ChannelId == 0)
	{
		return;
	}
	if (!bMuted)
	{
		MutedChannels.Remove(ChannelId);
		return;
	}

	MutedChannels.Add(ChannelId);
	for (const TPair<FString, FSpeakerInfo>& Pair : SpeakerInfos)
	{
		if (Pair.Value.ChannelId == ChannelId && Runtime)
		{
			Runtime->Playback.Forget(Pair.Key);
		}
	}
}

void UVoiceChatSubsystem::SetChannelVolume(const int64 ChannelId, const float Volume)
{
	if (ChannelId == 0)
	{
		return;
	}
	const float Clamped = FMath::IsFinite(Volume) ? FMath::Clamp(Volume, 0.0f, MaxUserVolume) : 1.0f;
	if (Clamped == 1.0f)
	{
		ChannelVolumes.Remove(ChannelId);
	}
	else
	{
		ChannelVolumes.Add(ChannelId, Clamped);
	}

	for (const TPair<FString, FSpeakerInfo>& Pair : SpeakerInfos)
	{
		if (Pair.Value.ChannelId == ChannelId)
		{
			ApplySpeakerVolume(Pair.Key);
		}
	}
}

float UVoiceChatSubsystem::GetChannelVolume(const int64 ChannelId) const
{
	const float* Volume = ChannelVolumes.Find(ChannelId);
	return Volume ? *Volume : 1.0f;
}

bool UVoiceChatSubsystem::AdmitSpeaker(const FString& SpeakerKey, FSpeakerInfo& Info, const int64 NowMs)
{
	Info.LastSeenMs = NowMs;
	// Asked again after an answer with no user, since a lookup that failed says to ask later.
	const bool bDue = !Info.bAsking && Info.UserId == 0 && Info.Id.IsValid() && NowMs >= Info.RetryAtMs;
	if (bDue && ResolveSpeakerUser && TakeLookup(NowMs))
	{
		Info.bAsking = true;
		// May answer inside the call, from a cache.
		TWeakObjectPtr<UVoiceChatSubsystem> WeakThis(this);
		ResolveSpeakerUser(Info.Id, [WeakThis, SpeakerKey](const int64 UserId)
		{
			if (UVoiceChatSubsystem* Self = WeakThis.Get())
			{
				Self->HandleSpeakerUser(SpeakerKey, UserId);
			}
		});
	}

	return AdmitsSpeaker(Info.UserId, MutedUsers, Info.bAnswered, NowMs - Info.FirstSeenMs);
}

bool UVoiceChatSubsystem::AdmitsSpeaker(const int64 UserId, const TSet<int64>& Muted, const bool bAnswered, const int64 SinceFirstSeenMs)
{
	if (UserId != 0)
	{
		return !Muted.Contains(UserId);
	}
	// A muted user's next sign-in brings a new actor id, so with anyone muted an unknown speaker waits to be known.
	return Muted.IsEmpty() || bAnswered || SinceFirstSeenMs >= UserLookupHoldMs;
}

float UVoiceChatSubsystem::ChannelStreamRank(const int64 UserId)
{
	return UserId != 0 ? 0.0f : TNumericLimits<float>::Max();
}

bool UVoiceChatSubsystem::TakeLookup(const int64 NowMs)
{
	LookupTokens = FMath::Min(LookupBurst, LookupTokens + static_cast<double>(NowMs - LookupTokensAtMs) * LookupsPerSecond / 1000.0);
	LookupTokensAtMs = NowMs;
	if (LookupTokens < 1.0)
	{
		return false;
	}
	LookupTokens -= 1.0;
	return true;
}

UVoiceChatSubsystem::FSpeakerInfo* UVoiceChatSubsystem::TrackSpeaker(const FString& SpeakerKey, const int64 NowMs, const int64 ChannelId)
{
	if (FSpeakerInfo* Known = SpeakerInfos.Find(SpeakerKey))
	{
		return Known;
	}

	// Full: the oldest entry nobody is hearing makes room, so ids that only keep themselves fresh cannot lock newcomers out.
	if (SpeakerInfos.Num() >= MaxSpeakerInfos && !EvictOldestUnheardInfo())
	{
		return nullptr;
	}

	FSpeakerInfo& Info = SpeakerInfos.Add(SpeakerKey);
	Info.FirstSeenMs = NowMs;
	Info.ChannelId = ChannelId;
	// Every SDK player sends under a guid's digits; an id that is not one has no user to look up.
	int32 Colon = INDEX_NONE;
	const FString Uuid = ChannelId != 0 && SpeakerKey.FindChar(TEXT(':'), Colon) ? SpeakerKey.RightChop(Colon + 1) : SpeakerKey;
	Info.bAnswered = !FGuid::Parse(Uuid, Info.Id) || !Info.Id.IsValid() || !ResolveSpeakerUser;
	// The same speaker on another stream already answered who they are, so this one neither waits nor spends a lookup.
	Info.UserId = Info.Id.IsValid() ? KnownUserOf(Info.Id) : 0;
	Info.bAnswered = Info.bAnswered || Info.UserId != 0;
	return &Info;
}

bool UVoiceChatSubsystem::IsSpeakerHeard(const FString& SpeakerKey) const
{
	return SpeakerStreams.Contains(SpeakerKey) || (Runtime && Runtime->Playback.IsHeard(SpeakerKey));
}

bool UVoiceChatSubsystem::EvictOldestUnheardInfo()
{
	const FString* Oldest = nullptr;
	int64 OldestSeenMs = TNumericLimits<int64>::Max();
	for (const TPair<FString, FSpeakerInfo>& Pair : SpeakerInfos)
	{
		if (Pair.Value.LastSeenMs < OldestSeenMs && !IsSpeakerHeard(Pair.Key))
		{
			OldestSeenMs = Pair.Value.LastSeenMs;
			Oldest = &Pair.Key;
		}
	}
	if (!Oldest)
	{
		return false;
	}
	SpeakerInfos.Remove(FString(*Oldest));
	return true;
}

void UVoiceChatSubsystem::HandleSpeakerUser(const FString& SpeakerKey, const int64 UserId)
{
	FSpeakerInfo* Info = SpeakerInfos.Find(SpeakerKey);
	if (!Info)
	{
		return;
	}
	Info->bAnswered = true;
	Info->bAsking = false;
	if (UserId == 0)
	{
		// Another stream of the same speaker may already know them; a failed lookup does not undo that.
		Info->UserId = KnownUserOf(Info->Id);
		Info->RetryAtMs = Info->UserId == 0 ? CrowdyVoiceNowMs() + LookupRetryMs : 0;
		ApplySpeakerVolume(SpeakerKey);
		return;
	}

	// One answer is the speaker's on every stream: their spatial voice and each channel they are heard on.
	const FGuid SpeakerId = Info->Id;
	for (TPair<FString, FSpeakerInfo>& Pair : SpeakerInfos)
	{
		if (Pair.Value.Id != SpeakerId)
		{
			continue;
		}
		Pair.Value.UserId = UserId;
		Pair.Value.bAnswered = true;
		if (MutedUsers.Contains(UserId) && Runtime)
		{
			Runtime->Playback.Forget(Pair.Key);
		}
		ApplySpeakerVolume(Pair.Key);
	}
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
	const bool bChanged = bOwnerEcho != bEnable;
	bOwnerEcho = bEnable;

	// The server only sends a session its own voice when asked; a new map restating the setting must not undo a request made elsewhere.
	if (!bChanged)
	{
		return;
	}
	const UWorld* World = GetWorld();
	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	if (UCrowdyCppReplicationSubsystem* Replication = GameInstance ? GameInstance->GetSubsystem<UCrowdyCppReplicationSubsystem>() : nullptr)
	{
		Replication->SetOwnMediaEcho(bEnable);
	}
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
	ChannelAudioSubscription = SdkBridge->ServiceRegistry->SubscribeToOpcode<FChannelAudioNotification>(
		ECrowdyMessageType::CHANNEL_AUDIO_NOTIFICATION,
		{ ECrowdySubscriptionRole::Observe, false, TEXT("CrowdyVoice") },
		[this](const FChannelAudioNotification& Notification, const FCrowdyDelivery&) { HandleChannelAudio(Notification); });
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
		++Stats.OwnPacketsIgnored;
		return;
	}
	++Stats.PacketsReceived;

	const FString SpeakerKey = Notification.UUID.ToString();
	const int64 NowMs = CrowdyVoiceNowMs();
	FSpeakerInfo* Info = TrackSpeaker(SpeakerKey, NowMs);
	if (!Info)
	{
		++PacketsUntracked;
		return;
	}
	const UCrowdyGameSession* GameSession = Session.Get();
	const double ChunkSize = GameSession ? GameSession->GetChunkSize() : 1600.0;
	Info->FallbackLocation = (FVector(static_cast<double>(Notification.ChunkX), static_cast<double>(Notification.ChunkY),
		static_cast<double>(Notification.ChunkZ)) + FVector(0.5)) * ChunkSize;

	if (!AdmitSpeaker(SpeakerKey, *Info, NowMs))
	{
		++Stats.PacketsMuted;
		return;
	}

	// A speaker this client does not draw ranks farthest: its envelope chunk is the sender's own claim.
	FVector Drawn;
	const bool bDrawn = LocateSpeaker && Info->Id.IsValid() && LocateSpeaker(Info->Id, Drawn);
	const float DistanceSquared = !bDrawn ? TNumericLimits<float>::Max()
		: bHasListener ? static_cast<float>(FVector::DistSquared(ListenerLocation, Drawn)) : 0.0f;
	Runtime->Playback.Push(SpeakerKey, Notification.Payload, NowMs, DistanceSquared);
}

void UVoiceChatSubsystem::HandleChannelAudio(const FChannelAudioNotification& Notification)
{
	if (!Runtime || Notification.ChannelId == 0 || !Notification.UUID.IsSet())
	{
		return;
	}

	// Membership is not consent: every player sits in the SDK's own channels, and any channel's leader may speak in it.
	if (!VoiceChannels.Contains(Notification.ChannelId))
	{
		++Stats.ChannelPacketsIgnored;
		return;
	}
	++Stats.PacketsReceived;
	++Stats.ChannelPacketsReceived;

	if (MutedChannels.Contains(Notification.ChannelId))
	{
		++Stats.PacketsMuted;
		return;
	}

	const FString SpeakerKey = ChannelStreamKey(Notification.ChannelId, Notification.UUID.ToString());
	const int64 NowMs = CrowdyVoiceNowMs();
	FSpeakerInfo* Info = TrackSpeaker(SpeakerKey, NowMs, Notification.ChannelId);
	if (!Info)
	{
		++PacketsUntracked;
		return;
	}
	if (!AdmitSpeaker(SpeakerKey, *Info, NowMs))
	{
		++Stats.PacketsMuted;
		return;
	}

	Runtime->Playback.Push(SpeakerKey, Notification.Payload, NowMs, ChannelStreamRank(Info->UserId));
}

void UVoiceChatSubsystem::HandleActorLeft(const FActorLeftNotification& Notification)
{
	if (Runtime)
	{
		Runtime->Playback.Forget(Notification.UUID.ToString());
	}
	SpeakerInfos.Remove(Notification.UUID.ToString());
}

void UVoiceChatSubsystem::HandleOwnerUUIDUpdated(const FString NewUUID)
{
	LocalSenderId = FCrowdyActorId::FromStringOrUnset(NewUUID);
}

void UVoiceChatSubsystem::SendFrame(const TConstArrayView<int16> Frame, const bool bLast)
{
	FCrowdyVoiceRuntime& Voice = *Runtime;
	const bool bTalksOnChannel = TalksOnAnyChannel();
	if (!bSpatialVoice && !bTalksOnChannel)
	{
		Voice.Encoder.Skip(1);
		return;
	}

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

	// Each channel gets the same packet, so a listener on two of them hears two streams with one sequence.
	FChannelAudioRequest& ChannelPacket = Voice.OutChannelPacket;
	if (bTalksOnChannel)
	{
		ChannelPacket.Payload = Voice.OutPacket.Payload;
		ChannelPacket.UUID = LocalSenderId;
	}
	for (const TPair<int64, ECrowdyChannelVoiceMode>& Channel : VoiceChannels)
	{
		if (Channel.Value != ECrowdyChannelVoiceMode::Talk)
		{
			continue;
		}
		ChannelPacket.ChannelId = Channel.Key;
		SdkBridge->SendMessageFn(ChannelPacket);
		++Stats.ChannelPacketsSent;
	}
	if (!bSpatialVoice)
	{
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
	TArray<FCrowdyVoiceRuntime::FTalkingChange> Changes = MoveTemp(Runtime->TalkingChanges);
	Runtime->TalkingChanges.Reset();
	for (const FCrowdyVoiceRuntime::FTalkingChange& Change : Changes)
	{
		if (Change.ChannelId == 0)
		{
			OnSpeakerTalking.Broadcast(Change.SpeakerKey, Change.bTalking);
			continue;
		}
		// The key is built here as "<ChannelId>:<Uuid>", and a channel id has no colon, so the first one ends it.
		int32 Colon = INDEX_NONE;
		Change.SpeakerKey.FindChar(TEXT(':'), Colon);
		OnChannelSpeakerTalking.Broadcast(Change.ChannelId, Change.SpeakerKey.RightChop(Colon + 1), Change.bTalking);
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

	const FSpeakerInfo* Info = SpeakerInfos.Find(SpeakerKey);
	FVector Location;
	const bool bChannel = Info && Info->ChannelId != 0;
	if (Component && PlaybackMode == ECrowdyVoicePlaybackMode::Spatial && !bChannel)
	{
		Component->bAllowSpatialization = true;
		Component->bIsUISound = false;
		Component->AttenuationSettings = AttenuationAsset;
		Component->bOverrideAttenuation = AttenuationAsset == nullptr;
		Component->AttenuationOverrides = SpatialAttenuation;
		Component->OcclusionCheckInterval = OcclusionCheckSeconds;
		if (Info && LocateSpeakerNow(*Info, Location))
		{
			Component->SetWorldLocation(Location);
		}
	}
	const bool bDirectionalChannel = Component && PlaybackMode == ECrowdyVoicePlaybackMode::Spatial && bChannel && bDirectionalChannelVoice;
	if (bDirectionalChannel)
	{
		// Left a UI sound, as flat channel voice is, so a pause does not silence the room.
		Component->bAllowSpatialization = true;
		Component->AttenuationSettings = nullptr;
		Component->OcclusionCheckInterval = OcclusionCheckSeconds;
	}

	FCrowdyVoiceSpeakerStream& Stream = SpeakerStreams.Add(SpeakerKey);
	Stream.Wave = Wave;
	Stream.Component = Component;
	Stream.SampleRate = SampleRate;
	if (bDirectionalChannel)
	{
		PlaceChannelStream(Stream, *Info, 0, true);
	}
	ApplySpeakerVolume(SpeakerKey);
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
