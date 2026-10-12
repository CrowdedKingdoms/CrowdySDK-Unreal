#include "CrowdyMassNodesLog.h"

#include "Compiler/CrowdyAuthoringContributions.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphUtilities.h"
#include "HAL/IConsoleManager.h"
#include "Mass/CrowdyMassActionEvents.h"
#include "Modules/ModuleManager.h"
#include "Pins/CrowdyMassEventPickerPin.h"

DEFINE_LOG_CATEGORY(LogCrowdyMassNodes);

static int32 GCrowdyMassNodesTraceEnabled = 0;
static FAutoConsoleVariableRef CVarCrowdyMassNodesTrace(
	TEXT("crowdy.mass.nodes.trace"),
	GCrowdyMassNodesTraceEnabled,
	TEXT("Log where the Call Crowdy Event on Entity dropdown's entries came from (0 = off, 1 = on)."),
	ECVF_Default);

bool CrowdyMassNodesTrace::IsEnabled()
{
	return GCrowdyMassNodesTraceEnabled != 0;
}

namespace
{
	// The pins a one-shot action event is read by, in the order an author wants them made. The names come
	// from the same constants the runtime handler reads, so the pins the details panel offers to create and
	// the parameters that actually land cannot be two different spellings.
	//
	// A Blueprint float pin is PC_Real subcategoried PC_Double, and that is correct here: the handler reads
	// the rate through GetFloat, which accepts a double property and narrows it.
	TArray<FCrowdyActionParameterSpec> DescribeActionParameters()
	{
		return {
			FCrowdyActionParameterSpec{CrowdyMassActionParamNames::ActionId, UEdGraphSchema_K2::PC_Int, NAME_None},
			FCrowdyActionParameterSpec{CrowdyMassActionParamNames::Variant, UEdGraphSchema_K2::PC_Int, NAME_None},
			FCrowdyActionParameterSpec{CrowdyMassActionParamNames::PlayRate, UEdGraphSchema_K2::PC_Real,
				UEdGraphSchema_K2::PC_Double}
		};
	}
}

/**
 * Hosts the plugin's UK2Node types. A UK2Node placed in a runtime Blueprint must live in an UncookedOnly
 * module so the node class exists while editing or cooking, yet is stripped from the packaged game. Each
 * node registers itself through the Blueprint action database; the pin factory that draws the event
 * dropdown they share has to be registered by hand, and released again on shutdown.
 *
 * It is also where this plugin supplies what the SDK's crowd authoring surfaces need to know about actions.
 * This is the only module of the plugin that both loads wherever Blueprints are edited and may depend on
 * CrowdyNodes, where those seams are declared: the runtime module is present in cooked games, which do no
 * authoring, and an editor module could not reach the compile pass, which is uncooked-only.
 */
class FCrowdyMassNodesModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		PinFactory = MakeShared<FCrowdyMassEventPickerPinFactory>();
		FEdGraphUtilities::RegisterVisualPinFactory(PinFactory);

		// Kept rather than discarded, and put back below rather than cleared. Unloading this module (a Live
		// Coding recompile) must not leave the SDK with no check installed while a
		// second backend that installed after this one is still loaded.
		//
		// Nothing here says whether the crowd surfaces are SHOWN. That follows from the backend a map profile
		// selects, which is a project setting the SDK reads for itself, so this module installs only the
		// Mass-specific knowledge the SDK cannot have: what an action event must carry, and which pins to
		// offer to create.
		DisplacedValidator =
			CrowdyAuthoringContributions::SetActionEventValidator(&CrowdyMassDescribeActionEventProblem);
		DisplacedParameterProvider =
			CrowdyAuthoringContributions::SetActionParameterProvider(&DescribeActionParameters);
	}

	virtual void ShutdownModule() override
	{
		CrowdyAuthoringContributions::SetActionParameterProvider(MoveTemp(DisplacedParameterProvider));
		CrowdyAuthoringContributions::SetActionEventValidator(MoveTemp(DisplacedValidator));

		if (PinFactory.IsValid())
		{
			FEdGraphUtilities::UnregisterVisualPinFactory(PinFactory);
			PinFactory.Reset();
		}
	}

private:
	TSharedPtr<FGraphPanelPinFactory> PinFactory;

	// What the crowd authoring contributions held before this module installed its own, so shutdown can
	// restore rather than clear.
	TFunction<FString(const UFunction*)> DisplacedValidator;
	TFunction<TArray<FCrowdyActionParameterSpec>()> DisplacedParameterProvider;
};

IMPLEMENT_MODULE(FCrowdyMassNodesModule, CrowdyMassNodes);
