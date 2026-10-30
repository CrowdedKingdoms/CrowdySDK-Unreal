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

	float StreamTimeoutSeconds = 2.0f;
	bool bPlaying = false;
	bool bOwnerEcho = false;
	bool bMicWanted = false;

	template <typename TVoiceChat>
	void ApplyTo(TVoiceChat& VoiceChat) const
	{
		VoiceChat.SetStreamTimeoutThreshold(StreamTimeoutSeconds);
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
