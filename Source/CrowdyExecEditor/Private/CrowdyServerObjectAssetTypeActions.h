#pragma once

#include "CoreMinimal.h"
#include "AssetTypeActions_Base.h"

/** Opens a Server Object definition in the Server Object editor. */
class FCrowdyServerObjectAssetTypeActions : public FAssetTypeActions_Base
{
public:
	/** The shared "Crowdy" Content Browser category, registered on first use. */
	static uint32 FindOrRegisterCrowdyCategory();

	virtual FText GetName() const override;
	virtual FColor GetTypeColor() const override;
	virtual UClass* GetSupportedClass() const override;
	virtual uint32 GetCategories() override;
	virtual void OpenAssetEditor(const TArray<UObject*>& InObjects, TSharedPtr<IToolkitHost> EditWithinLevelEditor) override;
};
