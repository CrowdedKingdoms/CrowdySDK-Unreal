// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/Effect/CrowdyEffectGraphCompileHook.h"

namespace
{
	CrowdyEffectGraphCompile::FCompileHook GCompileHook;
}

void CrowdyEffectGraphCompile::SetCompileHook(FCompileHook Hook)
{
	GCompileHook = MoveTemp(Hook);
}

CrowdyEffectGraphCompile::FCompileHook CrowdyEffectGraphCompile::GetCompileHook()
{
	return GCompileHook;
}

FCrowdyEffectSpec CrowdyEffectGraphCompile::CompileGraphToSpec(
	const UEdGraph* Graph, TArray<FCrowdyEffectDiagnostic>& OutDiagnostics)
{
	if (GCompileHook)
	{
		return GCompileHook(Graph, OutDiagnostics);
	}

	OutDiagnostics.Add({ ECrowdyEffectSeverity::Error, 0, 0,
		TEXT("this effect authors from a node graph, but the graph compiler (editor only) is not available") });
	return FCrowdyEffectSpec();
}
