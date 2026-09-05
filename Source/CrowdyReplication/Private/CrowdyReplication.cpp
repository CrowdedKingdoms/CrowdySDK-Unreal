#include "CrowdyReplication.h"
#include "CrowdyReplicationLog.h"
#include "CrowdyLog.h"
#include "Modules/ModuleManager.h"
#include "Replication/GameModel/CrowdyModelAttributeLookup.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

namespace
{
	// The map profile the SDK ships, offered to maps that configure none of their own so a project can run
	// the SDK without authoring settings first. It carries pure defaults, which is what selects the actor
	// pool: FCrowdyActorManagementConfigStruct::BackendClass already defaults to UCrowdyActorPoolBackend.
	//
	// Drawing entities through any other backend is therefore a deliberate choice a project makes, rather
	// than something it inherits from whichever rendering plugin happens to be installed.
	//
	// The path is the contract between the asset and this registration: renaming or moving the asset without
	// changing this string withdraws the default silently.
	const TCHAR* const CrowdySDKDefaultMapProfilePath =
		TEXT("/CrowdySDK/Data/DA_CrowdySDKDefaultProfile.DA_CrowdySDKDefaultProfile");
}

// The module's reflection caches hold raw FProperty/UFunction references, which stay valid only while the
// engine's reflection data does. Their invalidation hooks are bound here so each one is registered exactly
// once and removed again when the module unloads.
class FCrowdyReplicationModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FCrowdyModelAttributeLookup::Install();
		FCrowdyRPC::InstallFnInfoCacheInvalidation();

		UCrowdySDKDeveloperSettings::RegisterShippedDefaultProfile(TEXT("CrowdySDK"),
			TSoftObjectPtr<UCrowdyMapProfile>(FSoftObjectPath(CrowdySDKDefaultMapProfilePath)));
	}

	virtual void ShutdownModule() override
	{
		UCrowdySDKDeveloperSettings::UnregisterShippedDefaultProfile(TEXT("CrowdySDK"));

		FCrowdyRPC::RemoveFnInfoCacheInvalidation();
		FCrowdyModelAttributeLookup::Uninstall();
	}
};

IMPLEMENT_MODULE(FCrowdyReplicationModule, CrowdyReplication)

DEFINE_LOG_CATEGORY(LogCrowdyReplication);

namespace
{
	CROWDY_DEFINE_TRACE_CVAR(CVarCrowdyEntityTrace, TEXT("crowdy.entity.trace"),
		TEXT("When non-zero, logs CrowdyReplication entity activity: registry add/remove, event ")
		TEXT("routing and dispatch, entity components, and actor tracking/management. Off by default."));

	CROWDY_DEFINE_TRACE_CVAR(CVarCrowdyPoolTrace, TEXT("crowdy.pool.trace"),
		TEXT("When non-zero, logs CrowdyReplication actor-pool activity: spawn, release, reuse, and ")
		TEXT("rendering-backend churn. Off by default."));

	CROWDY_DEFINE_TRACE_CVAR(CVarCrowdyStateTrace, TEXT("crowdy.state.trace"),
		TEXT("When non-zero, logs CrowdyReplication CrowdyState replicator activity: owned-entity ")
		TEXT("diffing, delta emission, and datagram sizes. Off by default."));

	CROWDY_DEFINE_TRACE_CVAR(CVarCrowdyStateScopes, TEXT("crowdy.state.scopes"),
		TEXT("When non-zero, emits a CPU trace scope around each CrowdyState delta decode. Nested inside ")
		TEXT("the enclosing delivery scope, so it shifts that scope's own figure: turn it on only for a ")
		TEXT("run that needs the split. Off by default."));
}

// GetValueOnAnyThread: replication and pool code can log off the game thread.
bool CrowdyReplicationTrace::Entity() { return CVarCrowdyEntityTrace.GetValueOnAnyThread() != 0; }
bool CrowdyReplicationTrace::Pool()   { return CVarCrowdyPoolTrace.GetValueOnAnyThread() != 0; }
bool CrowdyReplicationTrace::State()  { return CVarCrowdyStateTrace.GetValueOnAnyThread() != 0; }

bool CrowdyReplicationProfile::StateScopes() { return CVarCrowdyStateScopes.GetValueOnAnyThread() != 0; }
