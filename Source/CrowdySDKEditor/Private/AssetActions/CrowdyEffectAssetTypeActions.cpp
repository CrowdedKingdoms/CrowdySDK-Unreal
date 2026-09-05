// Fill out your copyright notice in the Description page of Project Settings.

#include "AssetActions/CrowdyEffectAssetTypeActions.h"

#include "Graph/CrowdyEffectAssetEditor.h"
#include "Graph/CrowdyEffectScriptAssetEditor.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"
#include "Replication/GameModel/Effect/CrowdyEffectSpec.h"
#include "Toolkits/SimpleAssetEditor.h"

#define LOCTEXT_NAMESPACE "CrowdyEffectAssetTypeActions"

FCrowdyEffectAssetTypeActions::FCrowdyEffectAssetTypeActions(EAssetTypeCategories::Type InCategory)
	: Category(InCategory)
{
}

FText FCrowdyEffectAssetTypeActions::GetName() const
{
	return LOCTEXT("Name", "Crowdy Effect");
}

FColor FCrowdyEffectAssetTypeActions::GetTypeColor() const
{
	return FColor(56, 168, 189);
}

UClass* FCrowdyEffectAssetTypeActions::GetSupportedClass() const
{
	return UCrowdyEffect::StaticClass();
}

uint32 FCrowdyEffectAssetTypeActions::GetCategories()
{
	return Category;
}

void FCrowdyEffectAssetTypeActions::OpenAssetEditor(
	const TArray<UObject*>& InObjects, TSharedPtr<IToolkitHost> EditWithinLevelEditor)
{
	const EToolkitMode::Type Mode =
		EditWithinLevelEditor.IsValid() ? EToolkitMode::WorldCentric : EToolkitMode::Standalone;

	for (UObject* Object : InObjects)
	{
		UCrowdyEffect* Effect = Cast<UCrowdyEffect>(Object);
		if (!Effect)
		{
			continue;
		}

		if (Effect->Source == ECrowdyEffectSource::Graph)
		{
			const TSharedRef<FCrowdyEffectAssetEditor> Editor = MakeShared<FCrowdyEffectAssetEditor>();
			Editor->InitEditor(Mode, EditWithinLevelEditor, Effect);
		}
		else if (Effect->Source == ECrowdyEffectSource::Text)
		{
			// Text effects open in their own window, with the EffectScript editor as the canvas and the rest of the
			// asset in a side details tab, matching the graph editor's shape.
			const TSharedRef<FCrowdyEffectScriptAssetEditor> Editor = MakeShared<FCrowdyEffectScriptAssetEditor>();
			Editor->InitEditor(Mode, EditWithinLevelEditor, Effect);
		}
		else
		{
			// Source is a closed two-value enum (Text or Graph); this only runs for an asset holding a stale byte
			// from before the retired Structured picker mode was removed. The default property editor still opens,
			// so the asset is not stuck unopenable, and its authoring-mode switch lets it migrate to Script or Graph.
			FSimpleAssetEditor::CreateEditor(Mode, EditWithinLevelEditor, Effect);
		}
	}
}

#undef LOCTEXT_NAMESPACE
