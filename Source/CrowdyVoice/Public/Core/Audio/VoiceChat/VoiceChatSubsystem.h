#pragma once

#include "CoreMinimal.h"
#include "Core/Audio/VoiceChat/CrowdyChannelVoiceMode.h"
#include "Core/Audio/VoiceChat/CrowdyVoiceDeveloperSettings.h"
#include "Core/Audio/VoiceChat/CrowdyVoiceMicState.h"
#include "Core/Audio/VoiceChat/CrowdyVoiceSettings.h"
#include "Sound/SoundAttenuation.h"
#include "Core/Audio/VoiceChat/CrowdyVoiceStats.h"
#include "Core/UDP/Subscription/FCrowdySubscription.h"
#include "Serialization/CrowdyActorId.h"
#include "Subsystems/WorldSubsystem.h"
#include "Templates/PimplPtr.h"
#include "VoiceChatSubsystem.generated.h"

class UAudioComponent;
class UCrowdyGameSession;
class UCrowdySDKBridgeSubsystem;
class USoundWaveProcedural;
struct FActorLeftNotification;
struct FChannelAudioNotification;
struct FClientAudioNotification;
struct FCrowdyVoiceRuntime;

DECLARE_MULTICAST_DELEGATE_OneParam(FOnCrowdyVoiceMicStateChanged, ECrowdyVoiceMicState /*State*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnCrowdyVoiceSpeakerTalking, const FString& /*SpeakerUuid*/, bool /*bTalking*/);
DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnCrowdyChannelVoiceTalking, int64 /*ChannelId*/, const FString& /*SpeakerUuid*/, bool /*bTalking*/);

/** One remote speaker's playback: a procedural wave fed with decoded voice, played by an audio component that is 2D or follows the speaker. */
USTRUCT()
struct FCrowdyVoiceSpeakerStream
{
	GENERATED_BODY()

	UPROPERTY(Transient)
	TObjectPtr<UAudioComponent> Component;

	UPROPERTY(Transient)
	TObjectPtr<USoundWaveProcedural> Wave;

	int32 SampleRate = 0;
};

/**
 * Voice chat for one game world: captures the local microphone, sends it in the voice packet format the
 * CrowdyJS and CrowdyCPP SDKs share, and plays every other player's voice.
 *
 * Everything but the microphone callback runs on the game thread in Tick, and everything it creates ends
 * with the world. Blueprints reach it through the Crowdy SDK subsystem, which keeps its settings and
 * bindings across map changes.
 */
UCLASS()
class CROWDYVOICE_API UVoiceChatSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickableWhenPaused() const override { return true; }

	/** Opens the microphone and sends your voice; does nothing while it is already starting or capturing. */
	void StartVoiceChat();

	/** Closes the microphone, first sending what was captured so listeners see you stop talking. */
	void StopVoiceChat();

	/** Plays other players' voices. */
	void PlayVoiceChat();

	/** Stops playing other players' voices; talking events keep arriving. */
	void MuteVoiceChat();

	/** How long a player may be silent before their stream is released. */
	void SetStreamTimeoutThreshold(float InSeconds);

	/** Plays your own voice back to you, as the server returns it. */
	void SetOwnerEcho(bool bEnable);

	/** How far your voice reaches, in chunks around the chunk your pawn stands in: 0 is that chunk only, 8 the farthest the server relays. */
	void SetDistance(int32 Chunks);
	int32 GetDistance() const { return DistanceChunks; }

	/**
	 * Sends Samples, 48 kHz mono, as your voice, through the same silence gate, encoder and send path as the microphone.
	 * Push in real time, about 960 samples every 20 ms: a larger push is sent at once. Refused, answering false, while
	 * the microphone is starting or capturing, and in a world with no voice chat.
	 */
	bool FeedVoicePcm(TConstArrayView<int16> Samples);

	/** Ends the talk spurt Feed Voice Pcm started, so listeners see you stop talking. */
	void EndVoiceFeed();

	ECrowdyVoiceMicState GetMicState() const;
	bool IsCapturing() const { return GetMicState() == ECrowdyVoiceMicState::Capturing; }
	bool IsPlaying() const;

	/** What this world's voice chat has sent, received and played; the concealed, malformed and speaker counts refresh each tick. */
	const FCrowdyVoiceStats& GetStats() const { return Stats; }

	/** Game thread. Also fires from Deinitialize when the world ends with the microphone open. */
	FOnCrowdyVoiceMicStateChanged OnMicStateChanged;

	/** Game thread. A speaker is the sender's actor id as text. Fires for spatial voice only. */
	FOnCrowdyVoiceSpeakerTalking OnSpeakerTalking;

	/** Game thread. A speaker talking on a channel whose voice you joined; the same player can talk spatially at the same time. */
	FOnCrowdyChannelVoiceTalking OnChannelSpeakerTalking;

	/** Joins ChannelId's voice in Mode while bOn, or changes the mode of one already joined. A channel whose voice you have not joined is never played. */
	void SetChannelVoice(int64 ChannelId, bool bOn, ECrowdyChannelVoiceMode Mode = ECrowdyChannelVoiceMode::Talk);
	bool IsChannelVoiceOn(int64 ChannelId) const { return VoiceChannels.Contains(ChannelId); }

	/** Listen for a channel whose voice you have not joined. */
	ECrowdyChannelVoiceMode GetChannelVoiceMode(int64 ChannelId) const;

	/** Whether your voice also goes to the players near you; on by default. Channel voice is unaffected. */
	void SetSpatialVoiceEnabled(bool bEnabled) { bSpatialVoice = bEnabled; }
	bool IsSpatialVoiceEnabled() const { return bSpatialVoice; }

	/** A muted channel's voice is not decoded or played and takes no speaker place. */
	void SetChannelMuted(int64 ChannelId, bool bMuted);
	bool IsChannelMuted(int64 ChannelId) const { return MutedChannels.Contains(ChannelId); }

	/** 1 is as received, 0 silent, up to 4; multiplies the speaker's own volume. */
	void SetChannelVolume(int64 ChannelId, float Volume);
	float GetChannelVolume(int64 ChannelId) const;

	/** The key a channel speaker's stream plays under, the form CrowdyJS uses: "<ChannelId>:<SpeakerUuid>". */
	static FString ChannelStreamKey(int64 ChannelId, const FString& SpeakerUuid);

	/** Where a speaker's character is drawn on this client, for spatial playback and for hearing the nearest speakers; false when it is not drawn. Set by the Crowdy SDK subsystem. */
	TFunction<bool(const FGuid& SpeakerId, FVector& OutLocation)> LocateSpeaker;

	/** Looks up the user a speaker's actor id belongs to and answers once, with 0 when it is not known. Set by the Crowdy SDK subsystem. */
	TFunction<void(const FGuid& SpeakerId, TFunction<void(int64 UserId)> OnResolved)> ResolveSpeakerUser;

	/** A muted user's voice is not played or decoded and takes no speaker place. While any user is muted, a new speaker waits up to UserLookupHoldMs for their user to be known. */
	void SetUserMuted(int64 UserId, bool bMuted);
	bool IsUserMuted(int64 UserId) const { return MutedUsers.Contains(UserId); }

	/** 1 is as received, 0 silent, up to 4. */
	void SetUserVolume(int64 UserId, float Volume);
	float GetUserVolume(int64 UserId) const;

	/** The user a speaker's actor id belongs to, heard spatially or on a channel; 0 until it is known. */
	int64 GetSpeakerUserId(const FString& SpeakerUuid) const;

	static constexpr int64 UserLookupHoldMs = 1500;
	static constexpr float MaxUserVolume = 4.0f;

	/** Whether a speaker's packets play: never a muted user's, and with anyone muted, not an unknown speaker's until the lookup answers or UserLookupHoldMs passes from when they were first heard. */
	static bool AdmitsSpeaker(int64 UserId, const TSet<int64>& Muted, bool bAnswered, int64 SinceFirstSeenMs);

	/** How a channel stream ranks for a speaker place, as a squared distance: a known player's ahead of every spatial speaker, one with no user behind every one, so made-up ids only fill free places. */
	static float ChannelStreamRank(int64 UserId);

private:
	friend struct FCrowdyVoiceRuntime;

	/** What is known about one remote stream, heard or not: a speaker's spatial voice, or their voice on one channel. */
	struct FSpeakerInfo
	{
		FGuid Id;
		FVector FallbackLocation = FVector::ZeroVector;
		int64 UserId = 0;
		/** 0 for spatial voice. */
		int64 ChannelId = 0;
		int64 FirstSeenMs = 0;
		int64 LastSeenMs = 0;
		int64 RetryAtMs = 0;
		bool bAsking = false;
		bool bAnswered = false;
	};

	// A peer can send under any number of fresh actor ids, so lookups and tracked speakers are both bounded.
	static constexpr double LookupsPerSecond = 10.0;
	static constexpr double LookupBurst = 20.0;
	static constexpr int64 LookupRetryMs = 10000;
	static constexpr int32 MaxSpeakerInfos = 256;

	bool TakeLookup(int64 NowMs);
	FSpeakerInfo* TrackSpeaker(const FString& SpeakerKey, int64 NowMs, int64 ChannelId = 0);
	bool IsSpeakerHeard(const FString& SpeakerKey) const;
	int64 KnownUserOf(const FGuid& SpeakerId) const;
	bool TalksOnAnyChannel() const;
	bool EvictOldestUnheardInfo();

#if WITH_DEV_AUTOMATION_TESTS
public:
	/** Tracks SpeakerKey as a packet from it would, for a test with no world; false when the table could not take it. */
	bool TrackSpeakerForTest(const FString& SpeakerKey, const int64 NowMs, const int64 ChannelId = 0) { return TrackSpeaker(SpeakerKey, NowMs, ChannelId) != nullptr; }
	bool IsSpeakerTrackedForTest(const FString& SpeakerKey) const { return SpeakerInfos.Contains(SpeakerKey); }
	/** The actor id a tracked stream looks its user up by; invalid when it has none. */
	FGuid TrackedSpeakerIdForTest(const FString& SpeakerKey) const { const FSpeakerInfo* Info = SpeakerInfos.Find(SpeakerKey); return Info ? Info->Id : FGuid(); }
	/** Tracks and admits SpeakerKey as a packet from it would, asking Resolve Speaker User; whether it would play. */
	bool AdmitSpeakerForTest(const FString& SpeakerKey, const int64 NowMs, const int64 ChannelId = 0) { FSpeakerInfo* Info = TrackSpeaker(SpeakerKey, NowMs, ChannelId); return Info && AdmitSpeaker(SpeakerKey, *Info, NowMs); }
	int64 TrackedUserIdForTest(const FString& SpeakerKey) const { const FSpeakerInfo* Info = SpeakerInfos.Find(SpeakerKey); return Info ? Info->UserId : 0; }private:
#endif

	bool AdmitSpeaker(const FString& SpeakerKey, FSpeakerInfo& Info, int64 NowMs);
	void HandleSpeakerUser(const FString& SpeakerKey, int64 UserId);
	bool LocateSpeakerNow(const FSpeakerInfo& Info, FVector& OutLocation) const;
	void ApplySpeakerVolume(const FString& SpeakerKey);
	void UpdateListenerAndStreams(int64 NowMs);
	void ReadSettings();

	void ConnectToSession();
	void HandleAudio(const FClientAudioNotification& Notification);
	void HandleChannelAudio(const FChannelAudioNotification& Notification);
	void HandleActorLeft(const FActorLeftNotification& Notification);
	void SendFrame(TConstArrayView<int16> Frame, bool bLast);
	void BroadcastChanges();

	UFUNCTION()
	void HandleOwnerUUIDUpdated(FString NewUUID);

	void OpenSpeakerStream(const FString& SpeakerKey, int32 SampleRate);
	void QueueSpeakerAudio(const FString& SpeakerKey, TConstArrayView<int16> Pcm);
	void CloseSpeakerStream(const FString& SpeakerKey);

	UPROPERTY(Transient)
	TMap<FString, FCrowdyVoiceSpeakerStream> SpeakerStreams;

	UPROPERTY(Transient)
	TObjectPtr<USoundAttenuation> AttenuationAsset;

	TMap<FString, FSpeakerInfo> SpeakerInfos;
	TSet<int64> MutedUsers;
	TMap<int64, float> UserVolumes;
	TMap<int64, ECrowdyChannelVoiceMode> VoiceChannels;
	TSet<int64> MutedChannels;
	TMap<int64, float> ChannelVolumes;
	FSoundAttenuationSettings SpatialAttenuation;
	FVector ListenerLocation = FVector::ZeroVector;
	float OcclusionCheckSeconds = 0.1f;
	double LookupTokens = LookupBurst;
	int64 LookupTokensAtMs = 0;
	int64 PacketsUntracked = 0;
	int64 NextInfoPruneMs = 0;
	ECrowdyVoicePlaybackMode PlaybackMode = ECrowdyVoicePlaybackMode::Flat;
	bool bHasListener = false;

	TWeakObjectPtr<UCrowdySDKBridgeSubsystem> Bridge;
	TWeakObjectPtr<UCrowdyGameSession> Session;

	FCrowdySubscription AudioSubscription;
	FCrowdySubscription ChannelAudioSubscription;
	FCrowdySubscription ActorLeftSubscription;

	// Null in a world that has no game instance, which never captures or plays anything.
	TPimplPtr<FCrowdyVoiceRuntime> Runtime;

	FCrowdyActorId LocalSenderId;
	FCrowdyVoiceStats Stats;
	FInt64Vector LastPawnChunk = FInt64Vector::ZeroValue;
	int64 StreamTimeoutMs = 2000;
	int64 RestartAtMs = 0;
	int32 DistanceChunks = FCrowdyVoiceSettings::DefaultDistanceChunks;
	ECrowdyVoiceMicState BroadcastMicState = ECrowdyVoiceMicState::Off;
	bool bOwnerEcho = false;
	bool bSpatialVoice = true;
	bool bHasPawnChunk = false;
	bool bWarnedEncode = false;
	bool bWarnedNoSend = false;
};
