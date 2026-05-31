#pragma once

#include "BlueprintCompilerExtension.h"

#include "CrowdyBlueprintCompilerExtension.generated.h"

UCLASS()
class UCrowdyBlueprintCompilerExtension : public UBlueprintCompilerExtension
{
	GENERATED_BODY()

protected:
	virtual void ProcessBlueprintCompiled(
		const FKismetCompilerContext& CompilationContext,
		const FBlueprintCompiledData& Data) override;
};
