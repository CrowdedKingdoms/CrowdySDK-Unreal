#include "Compiler/CrowdyBlueprintCompileHooks.h"

namespace CrowdyBlueprintCompileHooks
{
	namespace
	{
		TFunction<void(UClass*)> GClassCompiledHook;
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
