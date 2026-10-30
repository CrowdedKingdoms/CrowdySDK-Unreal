#include "Core/Audio/VoiceChat/CrowdyVoicePlayback.h"

#include "CrowdyVoiceLog.h"

namespace
{
	int64 CrowdyVoicePlaybackBytesFor(const int64 Milliseconds, const int32 SampleRate)
	{
		return static_cast<int64>(SampleRate) * static_cast<int64>(sizeof(int16)) * Milliseconds / 1000;
	}
}

bool FCrowdyVoiceQueuePolicy::ShouldQueue(const int32 QueuedBytes, const int32 FrameBytes, const int32 SampleRate)
{
	return static_cast<int64>(QueuedBytes) + FrameBytes <= CrowdyVoicePlaybackBytesFor(MaxQueuedMs, SampleRate);
}

bool FCrowdyVoiceQueuePolicy::Admit(const int32 QueuedBytes, const int32 FrameBytes, const int32 SampleRate, FCrowdyVoiceStats& Stats)
{
	if (!ShouldQueue(QueuedBytes, FrameBytes, SampleRate))
	{
		++Stats.FramesDroppedQueueFull;
		return false;
	}
	++Stats.FramesPlayed;
	return true;
}

bool FCrowdyVoiceQueuePolicy::ShouldStart(const int32 QueuedBytes, const int32 SampleRate)
{
	return QueuedBytes >= CrowdyVoicePlaybackBytesFor(PrebufferMs, SampleRate);
}

FCrowdyVoicePlayback::FCrowdyVoicePlayback(ICrowdyVoiceSpeakerOutput& InOutput)
	: Output(InOutput)
	, Receiver(ReceiveDelayMs)
{
}

FCrowdyVoicePlayback::~FCrowdyVoicePlayback() = default;

void FCrowdyVoicePlayback::Push(const FString& SpeakerKey, const TConstArrayView<uint8> Packet, const int64 NowMs)
{
	const FCrowdyCppVoiceReceiver::EPushResult Result = Receiver.Push(SpeakerKey, Packet, NowMs);

	// Only a packet that will play keeps a speaker alive; a stale or repeated one says nothing about now.
	if (Result == FCrowdyCppVoiceReceiver::EPushResult::Buffered)
	{
		Speakers.FindOrAdd(SpeakerKey).LastHeardMs = NowMs;
		return;
	}

	if (Result == FCrowdyCppVoiceReceiver::EPushResult::Malformed)
	{
		UE_CLOG(!bWarnedRefused, LogCrowdyVoice, Warning,
			TEXT("Voice chat ignored a voice packet it could not read. Every player needs the same SDK version for voice, and at most %d players are heard at once."),
			FCrowdyCppVoiceReceiver::MaxSpeakers);
		bWarnedRefused = true;
	}
}

void FCrowdyVoicePlayback::Update(const int64 NowMs, const int64 TimeoutMs)
{
	Playouts.Reset();
	Receiver.Poll(NowMs, Playouts);
	for (const FCrowdyCppVoicePlayout& Playout : Playouts)
	{
		ApplyPlayout(Playout, NowMs);
	}
	ExpireSpeakers(NowMs, TimeoutMs);
}

void FCrowdyVoicePlayback::ApplyPlayout(const FCrowdyCppVoicePlayout& Playout, const int64 NowMs)
{
	FSpeaker& Speaker = Speakers.FindOrAdd(Playout.SpeakerKey);

	// A real frame also starts talking, so a lost first packet of a spurt does not hide the whole spurt.
	const bool bVoiced = !Playout.bConcealed && !Playout.Pcm.IsEmpty();
	if (bVoiced || Playout.bSpurtStart)
	{
		Speaker.LastVoicedMs = NowMs;
	}

	if (!Speaker.bTalking && (bVoiced || Playout.bSpurtStart))
	{
		Speaker.bTalking = true;
		Output.SpeakerTalking(Playout.SpeakerKey, true);
	}

	if (bPlaying && Playout.SampleRate > 0 && !Playout.Pcm.IsEmpty())
	{
		PlayFrame(Playout, Speaker);
	}

	if (Playout.bSpurtEnd && Speaker.bTalking)
	{
		Speaker.bTalking = false;
		Output.SpeakerTalking(Playout.SpeakerKey, false);
	}
}

void FCrowdyVoicePlayback::ExpireSpeakers(const int64 NowMs, const int64 TimeoutMs)
{
	Expired.Reset();
	for (TPair<FString, FSpeaker>& Pair : Speakers)
	{
		if (Pair.Value.bTalking && NowMs - Pair.Value.LastVoicedMs > TalkingHoldMs)
		{
			Pair.Value.bTalking = false;
			Output.SpeakerTalking(Pair.Key, false);
		}
		if (NowMs - Pair.Value.LastHeardMs > TimeoutMs)
		{
			Expired.Add(Pair.Key);
		}
	}

	for (const FString& SpeakerKey : Expired)
	{
		UE_CLOG(CrowdyVoiceTrace::Voice(), LogCrowdyVoice, Log, TEXT("Voice chat stopped hearing %s."), *SpeakerKey);
		Forget(SpeakerKey);
	}
}

void FCrowdyVoicePlayback::Forget(const FString& SpeakerKey)
{
	Receiver.Forget(SpeakerKey);

	FSpeaker Speaker;
	if (!Speakers.RemoveAndCopyValue(SpeakerKey, Speaker))
	{
		return;
	}
	Release(SpeakerKey, Speaker);
}

void FCrowdyVoicePlayback::ForgetAll()
{
	for (const TPair<FString, FSpeaker>& Pair : Speakers)
	{
		Receiver.Forget(Pair.Key);
		Release(Pair.Key, Pair.Value);
	}
	Speakers.Empty();
}

void FCrowdyVoicePlayback::SetPlaying(const bool bInPlaying)
{
	bPlaying = bInPlaying;
	if (bPlaying)
	{
		return;
	}

	for (TPair<FString, FSpeaker>& Pair : Speakers)
	{
		if (Pair.Value.OpenRate != 0)
		{
			Output.CloseSpeaker(Pair.Key);
			Pair.Value.OpenRate = 0;
		}
	}
}

void FCrowdyVoicePlayback::PlayFrame(const FCrowdyCppVoicePlayout& Playout, FSpeaker& Speaker)
{
	if (Speaker.OpenRate != 0 && Speaker.OpenRate != Playout.SampleRate)
	{
		Output.CloseSpeaker(Playout.SpeakerKey);
		Speaker.OpenRate = 0;
	}

	if (Speaker.OpenRate == 0)
	{
		Output.OpenSpeaker(Playout.SpeakerKey, Playout.SampleRate);
		Speaker.OpenRate = Playout.SampleRate;
	}
	Output.PlaySpeaker(Playout.SpeakerKey, Playout.Pcm);
}

void FCrowdyVoicePlayback::Release(const FString& SpeakerKey, const FSpeaker& Speaker)
{
	if (Speaker.bTalking)
	{
		Output.SpeakerTalking(SpeakerKey, false);
	}
	if (Speaker.OpenRate != 0)
	{
		Output.CloseSpeaker(SpeakerKey);
	}
}
