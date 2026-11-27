#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "CrowdyVoiceDeveloperSettings.generated.h"

class USoundAttenuation;

/** How other players' voices are played. */
UENUM(BlueprintType)
enum class ECrowdyVoicePlaybackMode : uint8
{
	/** Every voice at full volume in both ears, wherever the speaker is. */
	Flat UMETA(DisplayName = "Flat (2D)"),

	/** Each voice comes from where the speaker's character is drawn, quieter with distance and panned. */
	Spatial UMETA(DisplayName = "Spatial (3D)"),
};

/** Voice chat settings for the whole project: Project Settings, Plugins, Crowdy Voice. Read when a map's voice chat starts. */
UCLASS(Config=Game, DefaultConfig, meta=(DisplayName="Crowdy Voice"))
class CROWDYVOICE_API UCrowdyVoiceDeveloperSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetCategoryName() const override { return "Plugins"; }
	virtual FName GetSectionName() const override { return "Crowdy Voice"; }

	UPROPERTY(Config, EditAnywhere, Category="Playback", meta=(DisplayName="Playback Mode"))
	ECrowdyVoicePlaybackMode PlaybackMode = ECrowdyVoicePlaybackMode::Flat;

	/** How many players are heard at once. Past it, a new speaker who is nearer than the farthest one heard takes that place. */
	UPROPERTY(Config, EditAnywhere, Category="Playback", meta=(DisplayName="Max Speakers", ClampMin=1, ClampMax=64))
	int32 MaxSpeakers = 16;

	/** Spatial only: how a voice fades, pans and muffles. The SDK's SA_CrowdyVoice follows real hearing: full volume within 2 m, then 6 dB quieter each time the distance doubles, duller with distance, muffled behind walls, silent by 40 m. Pick an asset of your own to change it. */
	UPROPERTY(Config, EditAnywhere, Category="Playback", meta=(DisplayName="Attenuation",
		EditCondition="PlaybackMode == ECrowdyVoicePlaybackMode::Spatial"))
	TSoftObjectPtr<USoundAttenuation> Attenuation = TSoftObjectPtr<USoundAttenuation>(FSoftObjectPath(TEXT("/CrowdySDK/Data/Voice/SA_CrowdyVoice.SA_CrowdyVoice")));

	/** Spatial only, where the Attenuation muffles voices behind walls: how often each heard voice checks for a wall, one trace each time. */
	UPROPERTY(Config, EditAnywhere, Category="Playback", meta=(DisplayName="Occlusion Checks Per Second", ClampMin=1, ClampMax=30,
		EditCondition="PlaybackMode == ECrowdyVoicePlaybackMode::Spatial"))
	int32 OcclusionChecksPerSecond = 10;

	/** Spatial only: voice on a channel, such as a room, comes from where each speaker stands, at full volume however far away unless a Channel Attenuation says otherwise. Off plays it centred. A speaker who is not drawn near you plays centred until they are; each switch restarts their voice, a gap too short to notice in most speech. */
	UPROPERTY(Config, EditAnywhere, Category="Playback", meta=(DisplayName="Directional Channel Voice",
		EditCondition="PlaybackMode == ECrowdyVoicePlaybackMode::Spatial"))
	bool bDirectionalChannelVoice = false;

	/** Directional Channel Voice only: how a drawn speaker's channel voice fades and pans. Empty keeps it at full volume however far away, panned only. A speaker who is not drawn always plays centred at full volume, whatever this fades. */
	UPROPERTY(Config, EditAnywhere, Category="Playback", meta=(DisplayName="Channel Attenuation",
		EditCondition="PlaybackMode == ECrowdyVoicePlaybackMode::Spatial && bDirectionalChannelVoice"))
	TSoftObjectPtr<USoundAttenuation> ChannelAttenuation;

	/** How long received voice waits before it plays, to smooth out uneven arrival. Lower is quicker but breaks up on a poor connection. */
	UPROPERTY(Config, EditAnywhere, Category="Playback", meta=(DisplayName="Playout Delay", ClampMin=40, ClampMax=400, Units="ms"))
	int32 PlayoutDelayMs = 120;

	/** Your microphone sends nothing while it stays below this level, so silence costs nothing. */
	UPROPERTY(Config, EditAnywhere, Category="Microphone", meta=(DisplayName="Silence Gate", ClampMin="-90.0", ClampMax="-10.0", Units="dB"))
	float SilenceGateDb = -50.0f;

	/** How long your voice may stay below the silence gate before sending stops, so pauses between words do not cut out. */
	UPROPERTY(Config, EditAnywhere, Category="Microphone", meta=(DisplayName="Silence Hold", ClampMin=20, ClampMax=2000, Units="ms"))
	int32 SilenceHoldMs = 300;

	/** The Opus bit rate your voice is sent at. */
	UPROPERTY(Config, EditAnywhere, Category="Microphone", meta=(DisplayName="Bit Rate (bits per second)", ClampMin=6000, ClampMax=64000))
	int32 BitrateBps = 24000;
};
