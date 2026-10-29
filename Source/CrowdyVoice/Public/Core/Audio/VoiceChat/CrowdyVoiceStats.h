#pragma once

#include "CoreMinimal.h"

/** What one world's voice chat has done since the world began: running totals, plus the speakers heard now. */
struct FCrowdyVoiceStats
{
	/** Voice packets sent from the microphone. */
	int64 PacketsSent = 0;

	/** Microphone frames not sent: silence below the gate, or a backlog dropped after a hitch. */
	int64 SilentFramesSkipped = 0;

	/** Voice packets received for playback: other players', and your own while owner echo is on. */
	int64 PacketsReceived = 0;

	/** Decoded frames queued to play. */
	int64 FramesPlayed = 0;

	/** Frames whose packet was lost, filled in by concealment. */
	int64 FramesConcealed = 0;

	/** Decoded frames dropped because that speaker's playback queue was full. */
	int64 FramesDroppedQueueFull = 0;

	/** Received packets that could not be read, or came from a speaker past the limit. */
	int64 PacketsMalformed = 0;

	/** Speakers heard now. */
	int32 Speakers = 0;
};
