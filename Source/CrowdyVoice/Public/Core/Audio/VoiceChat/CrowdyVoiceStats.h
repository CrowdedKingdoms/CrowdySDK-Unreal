#pragma once

#include "CoreMinimal.h"

/** What one world's voice chat has done since the world began: running totals, plus the speakers heard now. */
struct FCrowdyVoiceStats
{
	/** Voice packets sent to the players near you. */
	int64 PacketsSent = 0;

	/** Voice packets sent to channels, one per channel. */
	int64 ChannelPacketsSent = 0;

	/** Channel voice packets received; they are also counted in Packets Received. */
	int64 ChannelPacketsReceived = 0;

	/** Voice packets from a channel whose voice this player has not joined; not counted as received. */
	int64 ChannelPacketsIgnored = 0;

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

	/** Received packets that could not be read. */
	int64 PacketsMalformed = 0;

	/** Packets from a speaker not heard because Max Speakers nearer ones were. */
	int64 PacketsOverCap = 0;

	/** Heard speakers that gave their place to a nearer one. */
	int64 SpeakersDisplaced = 0;

	/** Packets not played because their speaker's user or channel is muted, or while that user was still being looked up. */
	int64 PacketsMuted = 0;

	/** Speakers heard now. */
	int32 Speakers = 0;
};
