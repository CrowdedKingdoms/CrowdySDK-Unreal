#include "CrowdyMass.h"
#include "CrowdyMassLog.h"
#include "MassRepresentationAnimationProcessor.h"
#include "Modules/ModuleManager.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY(LogCrowdyMass);

// Registers no map profile: the SDK's registered default selects the actor pool, so Mass is reached only by a profile that names it.
class FCrowdyMassModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// The SDK's consume processor stands in for this engine pass, and both running hands every row's animation over twice.
		// Mass reads the flag once after engine init, which is after this; the engine's own setter would also rewrite DefaultMass.ini.
		const FBoolProperty* AutoRegister = FindFProperty<FBoolProperty>(UMassProcessor::StaticClass(), TEXT("bAutoRegisterWithProcessingPhases"));
		if (!AutoRegister)
		{
			UE_LOG(LogCrowdyMass, Warning, TEXT("Crowdy Mass: UMassProcessor has no bAutoRegisterWithProcessingPhases, so the engine's skinned mesh animation pass stays registered. Deregister MassConsumeInstancedSkinnedMeshAnimationProcessor in Config/DefaultMass.ini."));
			return;
		}
		AutoRegister->SetPropertyValue_InContainer(GetMutableDefault<UMassConsumeInstancedSkinnedMeshAnimationProcessor>(), false);
	}
};

IMPLEMENT_MODULE(FCrowdyMassModule, CrowdyMass)
