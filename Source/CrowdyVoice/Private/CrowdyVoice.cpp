#include "CrowdyVoice.h"
#include "CrowdyVoiceLog.h"
#include "CrowdyLog.h"
#include "Modules/ModuleManager.h"

IMPLEMENT_MODULE(FDefaultModuleImpl, CrowdyVoice)

DEFINE_LOG_CATEGORY(LogCrowdyVoice);

namespace
{
	CROWDY_DEFINE_TRACE_CVAR(CVarCrowdyVoiceTrace, TEXT("crowdy.voice.trace"),
		TEXT("When non-zero, logs CrowdyVoice activity: voice chat subsystem, capture/playback ")
		TEXT("service, and audio device monitoring. Off by default."));
}

bool CrowdyVoiceTrace::Voice() { return CVarCrowdyVoiceTrace.GetValueOnAnyThread() != 0; }
