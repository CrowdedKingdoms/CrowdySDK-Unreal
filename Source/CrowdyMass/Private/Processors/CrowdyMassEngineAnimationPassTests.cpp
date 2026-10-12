#if WITH_DEV_AUTOMATION_TESTS

#include "MassEntitySettings.h"
#include "MassProcessingPhaseManager.h"
#include "MassRepresentationAnimationProcessor.h"
#include "Misc/AutomationTest.h"
#include "Processors/CrowdyMassConsumeSkinnedMeshAnimationProcessor.h"

// The engine's skinned mesh animation pass is kept out of every Mass phase by the module itself, so a project needs no
// DefaultMass.ini, while the SDK's stand-in is in. Read off the phase lists Mass actually built, not just the flag.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassEngineAnimationPassIsDeregisteredTest,
	"CrowdySDK.Mass.EngineAnimationPassIsDeregistered",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FCrowdyMassEngineAnimationPassIsDeregisteredTest::RunTest(const FString& Parameters)
{
	const UMassProcessor* Engine = GetDefault<UMassConsumeInstancedSkinnedMeshAnimationProcessor>();
	const UMassProcessor* StandIn = GetDefault<UCrowdyMassConsumeSkinnedMeshAnimationProcessor>();
	TestFalse(TEXT("the engine pass is not marked for the phases"), Engine->ShouldAutoAddToGlobalList());

	bool bEngineInAPhase = false;
	bool bStandInInAPhase = false;
	for (const FMassProcessingPhaseConfig& Phase : GetMutableDefault<UMassEntitySettings>()->GetProcessingPhasesConfig())
	{
		bEngineInAPhase |= Phase.ProcessorCDOs.Contains(Engine);
		bStandInInAPhase |= Phase.ProcessorCDOs.Contains(StandIn);
	}
	TestTrue(TEXT("the SDK's stand-in is in a phase"), bStandInInAPhase);
	TestFalse(TEXT("and the engine pass is in none"), bEngineInAPhase);
	return true;
}

#endif
