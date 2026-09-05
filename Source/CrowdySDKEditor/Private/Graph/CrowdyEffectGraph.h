// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraph.h"
#include "CrowdyEffectGraph.generated.h"

/**
 * The node graph that authors one Game Model effect. A Material-Editor-style front-end: value nodes feed a single
 * Result node, and the graph compiles to the same structured effect spec a designer would build in the picker, so
 * every authoring surface lowers through one shared core. Data only at this stage; the editor schema and Slate
 * surface are separate concerns.
 */
UCLASS()
class UCrowdyEffectGraph : public UEdGraph
{
	GENERATED_BODY()
};
