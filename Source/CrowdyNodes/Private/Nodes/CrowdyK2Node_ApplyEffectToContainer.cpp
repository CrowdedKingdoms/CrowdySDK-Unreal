// Fill out your copyright notice in the Description page of Project Settings.

#include "Nodes/CrowdyK2Node_ApplyEffectToContainer.h"

#include "Replication/GameModel/CrowdyEffects.h"

#define LOCTEXT_NAMESPACE "CrowdyK2Node_ApplyEffectToContainer"

UFunction* UCrowdyK2Node_ApplyEffectToContainer::GetWrappedFunction() const
{
	return UCrowdyEffects::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(UCrowdyEffects, ApplyToContainer));
}

FText UCrowdyK2Node_ApplyEffectToContainer::GetBaseNodeTitle() const
{
	return LOCTEXT("NodeTitle", "Apply Crowdy Effect to Model (by Id)");
}

FText UCrowdyK2Node_ApplyEffectToContainer::GetTooltipText() const
{
	return LOCTEXT("NodeTooltip",
		"Apply an authored Game Model effect to a container addressed by id, exposing one typed pin per tuning "
		"magnitude.\n\nFire and forget: it reports no outcome.");
}

#undef LOCTEXT_NAMESPACE
