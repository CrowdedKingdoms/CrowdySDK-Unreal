// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Nodes/CrowdyK2Node_ApplyEffectCallBase.h"
#include "CrowdyK2Node_ApplyEffectToContainer.generated.h"

/**
 * "Apply Crowdy Effect to Model (by Id)" with a typed input pin per tuning magnitude of the referenced effect,
 * instead of a raw Overrides map. Applies the effect to a free or data container addressed by id (an inventory, a
 * quest) rather than to an actor.
 */
UCLASS()
class UCrowdyK2Node_ApplyEffectToContainer : public UCrowdyK2Node_ApplyEffectCallBase
{
	GENERATED_BODY()

public:
	virtual FText GetTooltipText() const override;
	virtual UFunction* GetWrappedFunction() const override;

protected:
	virtual FText GetBaseNodeTitle() const override;
};
