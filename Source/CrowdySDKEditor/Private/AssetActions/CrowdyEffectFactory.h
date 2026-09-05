// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "CrowdyEffectFactory.generated.h"

/**
 * Creates a UCrowdyEffect data asset from the content browser's right-click "Create" menu, grouped under a
 * "Crowdy" category. New assets default to the structured (designer-friendly) authoring source.
 */
UCLASS()
class UCrowdyEffectFactory : public UFactory
{
	GENERATED_BODY()

public:
	UCrowdyEffectFactory();

	virtual UObject* FactoryCreateNew(UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags,
		UObject* Context, FFeedbackContext* Warn) override;
	virtual bool ShouldShowInNewMenu() const override { return true; }
	virtual uint32 GetMenuCategories() const override;
	virtual FText GetDisplayName() const override;
	virtual FText GetToolTip() const override;
};
