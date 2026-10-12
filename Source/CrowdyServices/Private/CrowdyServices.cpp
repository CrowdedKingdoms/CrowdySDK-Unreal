#include "CrowdyServices.h"
#include "CrowdyServicesLog.h"
#include "CrowdyLog.h"
#include "Modules/ModuleManager.h"
#include "UI/CrowdyBrandStyle.h"

class FCrowdyServicesModule : public FDefaultModuleImpl
{
public:
	virtual void ShutdownModule() override
	{
		FCrowdyBrandStyle::Shutdown();
	}
};

IMPLEMENT_MODULE(FCrowdyServicesModule, CrowdyServices)

DEFINE_LOG_CATEGORY(LogCrowdyServices);

namespace
{
	CROWDY_DEFINE_TRACE_CVAR(CVarCrowdyServicesTrace, TEXT("crowdy.services.trace"),
		TEXT("When non-zero, logs CrowdyServices activity: authentication, teams, avatars, ")
		TEXT("persistence, host subsystem, utilities, and Blueprint reception. Off by default."));

	CROWDY_DEFINE_TRACE_CVAR(CVarCrowdyHudTrace, TEXT("crowdy.hud.trace"),
		TEXT("When non-zero, logs CrowdyServices HUD activity: HUD base and widgets. Off by default."));
}

bool CrowdyServicesTrace::Services() { return CVarCrowdyServicesTrace.GetValueOnAnyThread() != 0; }
bool CrowdyServicesTrace::Hud()      { return CVarCrowdyHudTrace.GetValueOnAnyThread() != 0; }
