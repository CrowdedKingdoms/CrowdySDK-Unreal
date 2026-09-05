// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Replication/GameModel/Effect/CrowdyEffectAst.h"
#include "Replication/GameModel/Effect/CrowdyEffectSpec.h"

class UEdGraph;

/**
 * The no-cycle seam that lets UCrowdyEffect (this runtime module) compile a Graph-sourced effect whose node classes
 * and compiler live in the editor-only CrowdySDKEditor module. CrowdySDKEditor registers the hook at startup;
 * UCrowdyEffect::Compile() calls CompileGraphToSpec when its Source is Graph. Editor-only in practice: the graph is
 * editor-only data and is only ever compiled in the editor. Mirrors the CrowdyStudioRegistry rebuild / load-assets
 * hooks: a runtime module holds the entry point, an editor module supplies the implementation.
 */
namespace CrowdyEffectGraphCompile
{
	using FCompileHook = TFunction<FCrowdyEffectSpec(const UEdGraph*, TArray<FCrowdyEffectDiagnostic>&)>;

	// Registered by CrowdySDKEditor at module startup, cleared to null at shutdown.
	CROWDYREPLICATION_API void SetCompileHook(FCompileHook Hook);

	// Whatever is registered right now, so a caller installing its own compiler for a moment can put back what was
	// there instead of clearing a hook another module owns.
	CROWDYREPLICATION_API FCompileHook GetCompileHook();

	// Compiles the graph to a structured spec through the registered hook. Without a hook (the editor module is not
	// loaded, e.g. a cooked build), records one Error diagnostic and returns an empty spec, so a Graph effect never
	// silently compiles to a blank function.
	CROWDYREPLICATION_API FCrowdyEffectSpec CompileGraphToSpec(
		const UEdGraph* Graph, TArray<FCrowdyEffectDiagnostic>& OutDiagnostics);
}
