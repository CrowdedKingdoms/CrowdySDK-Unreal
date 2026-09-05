// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Nodes/CrowdyK2Node_ApplyEffectCallBase.h"
#include "CrowdyK2Node_ApplyEffectFireAndForget.generated.h"

/**
 * "Apply Crowdy Effect (Fire and Forget)" with a typed input pin per tuning magnitude of the referenced effect,
 * instead of a raw Overrides map. Applies the effect to a Target that is a registered participant, with no
 * Success / Failed pins; use the latent Apply Crowdy Effect node when the outcome matters.
 */
UCLASS()
class UCrowdyK2Node_ApplyEffectFireAndForget : public UCrowdyK2Node_ApplyEffectCallBase
{
	GENERATED_BODY()

public:
	virtual FText GetTooltipText() const override;
	virtual UFunction* GetWrappedFunction() const override;

protected:
	virtual FText GetBaseNodeTitle() const override;
};
