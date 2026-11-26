#pragma once

#include "CoreMinimal.h"
#include "Core/Audio/VoiceChat/CrowdyChannelVoiceMode.h"

/** Voice chat requests that outlive one map, applied to the voice chat of each map that loads. */
struct FCrowdyVoiceSettings
{
	// The minimum stays well above the playout delay, or speakers are released between their own packets.
	static constexpr float MinStreamTimeoutSeconds = 0.5f;
	static constexpr float MaxStreamTimeoutSeconds = 3600.0f;

	/** The stream timeout voice chat uses for Seconds; a value that is not a number reads as the minimum. */
	static float ClampStreamTimeout(const float Seconds)
	{
		return FMath::Clamp(FMath::IsFinite(Seconds) ? Seconds : 0.0f, MinStreamTimeoutSeconds, MaxStreamTimeoutSeconds);
	}

	/** The farthest the server relays a voice packet, in chunks around the sender's. */
	static constexpr int32 MaxDistanceChunks = 8;

	/** How far voice reaches on a map whose profile does not say. */
	static constexpr int32 DefaultDistanceChunks = 1;

	static int32 ClampDistance(const int32 Chunks)
	{
		return FMath::Clamp(Chunks, 0, MaxDistanceChunks);
	}

	/** The overall playback volume voice chat uses for Volume, 0 to 1; a value that is not a number reads as 1. */
	static float ClampPlaybackVolume(const float Volume)
	{
		return FMath::IsFinite(Volume) ? FMath::Clamp(Volume, 0.0f, 1.0f) : 1.0f;
	}

	float StreamTimeoutSeconds = 2.0f;
	// Set by the game; unset follows each map's own voice distance.
	TOptional<int32> DistanceChunks;
	TSet<int64> MutedUsers;
	TMap<int64, float> UserVolumes;
	TMap<int64, ECrowdyChannelVoiceMode> VoiceChannels;
	// The user who joined VoiceChannels; channel voice is that account's, not the next one's on this game instance.
	int64 ChannelsUserId = 0;
	TSet<int64> MutedChannels;
	TMap<int64, float> ChannelVolumes;
	// Empty follows the OS default microphone.
	FString InputDeviceId;
	float PlaybackVolume = 1.0f;
	bool bSpatialVoice = true;
	bool bPlaying = false;
	bool bOwnerEcho = false;
	bool bMicWanted = false;

	template <typename TVoiceChat>
	void ApplyTo(TVoiceChat& VoiceChat, const int32 MapDistanceChunks) const
	{
		VoiceChat.SetStreamTimeoutThreshold(StreamTimeoutSeconds);
		VoiceChat.SetDistance(DistanceChunks.Get(MapDistanceChunks));
		for (const int64 UserId : MutedUsers)
		{
			VoiceChat.SetUserMuted(UserId, true);
		}
		for (const TPair<int64, float>& Volume : UserVolumes)
		{
			VoiceChat.SetUserVolume(Volume.Key, Volume.Value);
		}
		for (const TPair<int64, ECrowdyChannelVoiceMode>& Channel : VoiceChannels)
		{
			VoiceChat.SetChannelVoice(Channel.Key, true, Channel.Value);
		}
		for (const int64 ChannelId : MutedChannels)
		{
			VoiceChat.SetChannelMuted(ChannelId, true);
		}
		for (const TPair<int64, float>& Volume : ChannelVolumes)
		{
			VoiceChat.SetChannelVolume(Volume.Key, Volume.Value);
		}
		if (PlaybackVolume != 1.0f)
		{
			VoiceChat.SetPlaybackVolume(PlaybackVolume);
		}
		// Before the microphone starts, so it opens on the chosen device rather than moving to it.
		if (!InputDeviceId.IsEmpty())
		{
			VoiceChat.SetInputDevice(InputDeviceId);
		}
		VoiceChat.SetSpatialVoiceEnabled(bSpatialVoice);
		VoiceChat.SetOwnerEcho(bOwnerEcho);
		if (bPlaying)
		{
			VoiceChat.PlayVoiceChat();
		}
		else
		{
			VoiceChat.MuteVoiceChat();
		}
		if (bMicWanted)
		{
			VoiceChat.StartVoiceChat();
		}
	}
};
