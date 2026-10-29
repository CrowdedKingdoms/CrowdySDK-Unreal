#include "CrowdyVoice.h"
#include "CrowdyVoiceLog.h"
#include "CrowdyLog.h"
#include "Core/Audio/VoiceChat/CrowdyVoiceMic.h"
#include "Modules/ModuleManager.h"

namespace
{
	// Longer than a background close can wait for its stream's first delivery, so a normal exit never hits it.
	constexpr double CrowdyVoiceShutdownWaitSeconds = 4.0;

	CROWDY_DEFINE_TRACE_CVAR(CVarCrowdyVoiceTrace, TEXT("crowdy.voice.trace"),
		TEXT("When non-zero, logs CrowdyVoice activity: microphone, playback and audio device changes. ")
		TEXT("Off by default."));
}

class FCrowdyVoiceModule final : public IModuleInterface
{
public:
	virtual void ShutdownModule() override
	{
		// A microphone stream still closing runs capture code that must not outlive the modules behind it.
		const int32 Remaining = WaitForCrowdyVoiceMicCloses(CrowdyVoiceShutdownWaitSeconds);
		UE_CLOG(Remaining > 0, LogCrowdyVoice, Warning,
			TEXT("Voice chat is shutting down with %d microphone stream(s) still closing after %.0f s."),
			Remaining, CrowdyVoiceShutdownWaitSeconds);
	}
};

IMPLEMENT_MODULE(FCrowdyVoiceModule, CrowdyVoice)

DEFINE_LOG_CATEGORY(LogCrowdyVoice);

bool CrowdyVoiceTrace::Voice() { return CVarCrowdyVoiceTrace.GetValueOnAnyThread() != 0; }
