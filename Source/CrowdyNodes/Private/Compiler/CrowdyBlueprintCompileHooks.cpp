#include "Compiler/CrowdyBlueprintCompileHooks.h"

namespace CrowdyBlueprintCompileHooks
{
	namespace
	{
		TFunction<void(const UBlueprint*, UClass*)> GContainerStampHook;
		TFunction<void(UClass*)> GClassCompiledHook;
	}

	void SetContainerStampHook(TFunction<void(const UBlueprint*, UClass*)> Hook)
	{
		GContainerStampHook = MoveTemp(Hook);
	}

	void StampContainerMetadata(const UBlueprint* Blueprint, UClass* Class)
	{
		if (!Blueprint || !Class || !GContainerStampHook) return;

		GContainerStampHook(Blueprint, Class);
	}

	void SetClassCompiledHook(TFunction<void(UClass*)> Hook)
	{
		GClassCompiledHook = MoveTemp(Hook);
	}

	void NotifyClassCompiled(UClass* Class)
	{
		if (!Class || !GClassCompiledHook) return;

		GClassCompiledHook(Class);
	}
}
