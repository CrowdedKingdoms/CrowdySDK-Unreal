#pragma once

#include "Network/UDP/CrowdyCppSendAdapter.h"

namespace CrowdyCppSend
{
	/**
	 * The header-reading half of the split, over bytes that are already serialized.
	 *
	 * Deliberately not part of the module's public surface. It reads fields at fixed offsets, which is only safe
	 * because the caller has already established that the header was written by our own encoder with a 32-octet
	 * actor id: a spatial header carrying a shorter id shifts every field after it, and nothing in the bytes
	 * themselves reveals that. SplitMessage establishes it. This is not a parser for received data.
	 */
	bool SplitSerializedFrame(TArrayView<const uint8> Frame, FCrowdyCppOutboundFrame& OutFrame, FString& OutError);
}
