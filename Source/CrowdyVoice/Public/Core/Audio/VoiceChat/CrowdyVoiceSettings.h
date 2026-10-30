#pragma once

#include "CoreMinimal.h"

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

	static int32 ClampDistance(const int32 Chunks)
	{
		return FMath::Clamp(Chunks, 0, MaxDistanceChunks);
	}

	float StreamTimeoutSeconds = 2.0f;
	int32 DistanceChunks = MaxDistanceChunks;
	bool bPlaying = false;
	bool bOwnerEcho = false;
	bool bMicWanted = false;

	template <typename TVoiceChat>
	void ApplyTo(TVoiceChat& VoiceChat) const
	{
		VoiceChat.SetStreamTimeoutThreshold(StreamTimeoutSeconds);
		VoiceChat.SetDistance(DistanceChunks);
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
