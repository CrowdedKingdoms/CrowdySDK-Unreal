// Fill out your copyright notice in the Description page of Project Settings.

#include "Nodes/CrowdyK2Node_ApplyEffectFireAndForget.h"

#include "Replication/GameModel/CrowdyEffects.h"

#define LOCTEXT_NAMESPACE "CrowdyK2Node_ApplyEffectFireAndForget"

UFunction* UCrowdyK2Node_ApplyEffectFireAndForget::GetWrappedFunction() const
{
	return UCrowdyEffects::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UCrowdyEffects, Apply));
}

FText UCrowdyK2Node_ApplyEffectFireAndForget::GetBaseNodeTitle() const
{
	return LOCTEXT("NodeTitle", "Apply Crowdy Effect (Fire and Forget)");
}

FText UCrowdyK2Node_ApplyEffectFireAndForget::GetTooltipText() const
{
	return LOCTEXT("NodeTooltip",
		"Apply an authored Game Model effect to a Target, exposing one typed pin per tuning magnitude.\n\n"
		"Fire and forget: it reports no outcome. Use Apply Crowdy Effect when you need Success / Failed.");
}

#undef LOCTEXT_NAMESPACE
