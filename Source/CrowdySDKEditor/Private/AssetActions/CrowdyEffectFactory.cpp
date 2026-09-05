// Fill out your copyright notice in the Description page of Project Settings.

#include "AssetActions/CrowdyEffectFactory.h"

#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Modules/ModuleManager.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"
#include "Replication/GameModel/Effect/CrowdyEffectSpec.h"

#define LOCTEXT_NAMESPACE "CrowdyEffectFactory"

UCrowdyEffectFactory::UCrowdyEffectFactory()
{
	SupportedClass = UCrowdyEffect::StaticClass();
	bCreateNew = true;
	bEditAfterNew = true;
}

UObject* UCrowdyEffectFactory::FactoryCreateNew(UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags,
	UObject* Context, FFeedbackContext* Warn)
{
	UCrowdyEffect* NewEffect = NewObject<UCrowdyEffect>(InParent, InClass, InName, Flags);
	if (NewEffect)
	{
		// New effects author in Script (EffectScript) mode, so a freshly created asset opens straight into the script
		// window. This matches the property's own default; it is set explicitly here for clarity. Switch to Graph
		// as needed.
		NewEffect->Source = ECrowdyEffectSource::Text;
	}
	return NewEffect;
}

uint32 UCrowdyEffectFactory::GetMenuCategories() const
{
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	const FName CategoryKey(TEXT("Crowdy"));
	EAssetTypeCategories::Type Category = AssetTools.FindAdvancedAssetCategory(CategoryKey);
	if (Category == EAssetTypeCategories::Misc)
	{
		Category = AssetTools.RegisterAdvancedAssetCategory(CategoryKey, LOCTEXT("CrowdyAssetCategory", "Crowdy"));
	}
	return Category;
}

FText UCrowdyEffectFactory::GetDisplayName() const
{
	return LOCTEXT("CrowdyEffectDisplayName", "Crowdy Effect");
}

FText UCrowdyEffectFactory::GetToolTip() const
{
	return LOCTEXT("CrowdyEffectToolTip",
		"A Game Model effect: a target container class, tuning magnitudes, and the mutation it applies.");
}

#undef LOCTEXT_NAMESPACE
