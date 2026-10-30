#pragma once

#include "CoreMinimal.h"
#include "Core/Audio/VoiceChat/CrowdyVoiceMicState.h"
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
struct FClientAudioNotification;
struct FCrowdyVoiceRuntime;

DECLARE_MULTICAST_DELEGATE_OneParam(FOnCrowdyVoiceMicStateChanged, ECrowdyVoiceMicState /*State*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnCrowdyVoiceSpeakerTalking, const FString& /*SpeakerUuid*/, bool /*bTalking*/);

/** One remote speaker's playback: a procedural wave fed with decoded voice, played by a 2D audio component. */
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

	/** How far your voice reaches, in chunks around the chunk your pawn stands in: 0 is that chunk only, 8 (the default) the farthest the server relays. */
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

	/** Game thread. A speaker is the sender's actor id as text. */
	FOnCrowdyVoiceSpeakerTalking OnSpeakerTalking;

private:
	friend struct FCrowdyVoiceRuntime;

	void ConnectToSession();
	void HandleAudio(const FClientAudioNotification& Notification);
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

	TWeakObjectPtr<UCrowdySDKBridgeSubsystem> Bridge;
	TWeakObjectPtr<UCrowdyGameSession> Session;

	FCrowdySubscription AudioSubscription;
	FCrowdySubscription ActorLeftSubscription;

	// Null in a world that has no game instance, which never captures or plays anything.
	TPimplPtr<FCrowdyVoiceRuntime> Runtime;

	FCrowdyActorId LocalSenderId;
	FCrowdyVoiceStats Stats;
	FInt64Vector LastPawnChunk = FInt64Vector::ZeroValue;
	int64 StreamTimeoutMs = 2000;
	int64 RestartAtMs = 0;
	int32 DistanceChunks = 8;
	ECrowdyVoiceMicState BroadcastMicState = ECrowdyVoiceMicState::Off;
	bool bOwnerEcho = false;
	bool bHasPawnChunk = false;
	bool bWarnedEncode = false;
	bool bWarnedNoSend = false;
};
