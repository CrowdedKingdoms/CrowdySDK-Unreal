// Fill out your copyright notice in the Description page of Project Settings.

#include "BlueprintCompilationManager.h"
#include "Compiler/CrowdyBlueprintCompilerExtension.h"
#include "CrowdyNodesLog.h"
#include "Engine/Blueprint.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY(LogCrowdyNodes);

// Hosts the plugin's UK2Node types and the Crowdy Blueprint compile pass. A UK2Node placed in a runtime
// Blueprint must live in an UncookedOnly (or Developer) module so the node class is available while cooking
// or editing uncooked assets, yet is stripped from the packaged game. The nodes self-register through the
// Blueprint action database; the compile pass does not, so it is registered below.
class FCrowdyNodesModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// Registered from this module, not the editor one, because every uncooked non-editor process
		// (Play Standalone, an uncooked dedicated server, any -game run of the editor binary) discards the
		// Blueprint bytecode on disk and regenerates it by recompiling on load. An extension registered only
		// where GIsEditor is true is absent for that recompile, so every Blueprint replicated event comes out
		// with no dispatch gate: its body runs locally and it sends nothing, silently. UncookedOnly loads in
		// all three, and in the cook commandlet, which is every process that ever compiles a Blueprint.
		FBlueprintCompilationManager::RegisterCompilerExtension(
			UBlueprint::StaticClass(),
			NewObject<UCrowdyBlueprintCompilerExtension>(GetTransientPackage()));
	}
};

IMPLEMENT_MODULE(FCrowdyNodesModule, CrowdyNodes);
