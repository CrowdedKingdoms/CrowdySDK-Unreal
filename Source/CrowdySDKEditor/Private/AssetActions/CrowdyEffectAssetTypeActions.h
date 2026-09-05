// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "AssetTypeActions_Base.h"

/**
 * Asset type actions for UCrowdyEffect. A Graph-sourced effect opens in the node-graph asset editor
 * (FCrowdyEffectAssetEditor); a Text or Structured effect keeps the default property editor, so the graph editor is
 * opt-in per the effect's authoring mode and existing assets are unaffected.
 */
class FCrowdyEffectAssetTypeActions : public FAssetTypeActions_Base
{
public:
	explicit FCrowdyEffectAssetTypeActions(EAssetTypeCategories::Type InCategory);

	virtual FText GetName() const override;
	virtual FColor GetTypeColor() const override;
	virtual UClass* GetSupportedClass() const override;
	virtual uint32 GetCategories() override;
	virtual void OpenAssetEditor(
		const TArray<UObject*>& InObjects, TSharedPtr<IToolkitHost> EditWithinLevelEditor) override;

private:
	EAssetTypeCategories::Type Category;
};
