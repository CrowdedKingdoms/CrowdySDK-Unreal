#pragma once

#include "CoreMinimal.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Utils/SerializationFunctionLibrary.h"

/** One encoded audio frame inside a voice packet. */
struct FCrowdyAudioFrame
{
	/**
	 * The length a received frame declared for itself. A sender leaves this alone: the length that goes
	 * on the wire is the number of octets actually being sent, so the two cannot disagree.
	 */
	int32 FrameSize = 0;

	TArray<uint8> AudioData;
};

/**
 * The payload a voice packet carries, shared by the outbound request and the inbound notification so
 * that one description of the layout serves both directions.
 *
 * The layout is the sample rate, the channel count, a frame count, and then each frame as a declared
 * length followed by that many octets.
 */
struct FClientAudioBody : ICrowdyMessage
{
	int32 SampleRate = 0;
	int32 NumChannels = 0;

	TArray<FCrowdyAudioFrame> Frames;

protected:

	void AppendBody(TArray<uint8>& Data) const
	{
		USerializationFunctionLibrary::AppendValue(Data, SampleRate);
		USerializationFunctionLibrary::AppendValue(Data, NumChannels);
		USerializationFunctionLibrary::AppendValue(Data, Frames.Num());

		for (const FCrowdyAudioFrame& Frame : Frames)
		{
			// The length written is the number of octets appended right after it, never the length the
			// frame arrived with, so a frame whose audio was replaced cannot advertise the old length.
			USerializationFunctionLibrary::AppendValue(Data, Frame.AudioData.Num());
			Data.Append(Frame.AudioData);
		}
	}
};
