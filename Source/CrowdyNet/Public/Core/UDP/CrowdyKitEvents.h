#pragma once

#include "CoreMinimal.h"

/**
 * Client events the Game Kit defines, shared with every other Crowdy client library.
 *
 * Their numbers are fixed by the platform, so no struct a project defines may hash onto one, and a kit
 * event's state is the bytes after the two-octet event type with no length prefix and no routing block,
 * which is how every other client library writes and reads it.
 */
namespace CrowdyKitEvent
{
	/** An emote: a JSON body of the emote's name and the actor it is aimed at. */
	inline constexpr uint16 Emote = 101;

	/** The most state an emote carries. */
	inline constexpr int32 MaxEmoteStateBytes = 1024;

	/** True for a client event whose state runs to the end of the message instead of following a length. */
	constexpr bool HasBareState(const uint16 EventType)
	{
		return EventType == Emote;
	}
}
