#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "CrowdyServerObjectFactory.generated.h"

/** Creates a Server Object definition from the Content Browser's Add menu, under the Crowdy category. */
UCLASS()
class UCrowdyServerObjectFactory : public UFactory
{
	GENERATED_BODY()

public:
	UCrowdyServerObjectFactory();

	virtual UObject* FactoryCreateNew(UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags,
		UObject* Context, FFeedbackContext* Warn) override;
	virtual bool ShouldShowInNewMenu() const override { return true; }
	virtual uint32 GetMenuCategories() const override;
	virtual FText GetDisplayName() const override;
	virtual FText GetToolTip() const override;
	virtual FString GetDefaultNewAssetName() const override;
};
