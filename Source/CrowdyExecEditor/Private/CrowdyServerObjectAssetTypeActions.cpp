#include "CrowdyServerObjectAssetTypeActions.h"

#include "AssetToolsModule.h"
#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectEditor.h"
#include "IAssetTools.h"
#include "Modules/ModuleManager.h"

#define LOCTEXT_NAMESPACE "CrowdyServerObjectAssetTypeActions"

FText FCrowdyServerObjectAssetTypeActions::GetName() const
{
	return LOCTEXT("Name", "Server Object Definition");
}

FColor FCrowdyServerObjectAssetTypeActions::GetTypeColor() const
{
	return FColor(77, 128, 204);
}

UClass* FCrowdyServerObjectAssetTypeActions::GetSupportedClass() const
{
	return UCrowdyServerObjectDefinition::StaticClass();
}

uint32 FCrowdyServerObjectAssetTypeActions::FindOrRegisterCrowdyCategory()
{
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	const FName CategoryKey(TEXT("Crowdy"));
	const EAssetTypeCategories::Type Existing = AssetTools.FindAdvancedAssetCategory(CategoryKey);
	if (Existing != EAssetTypeCategories::Misc)
	{
		return Existing;
	}
	return AssetTools.RegisterAdvancedAssetCategory(CategoryKey, LOCTEXT("CrowdyAssetCategory", "Crowdy"));
}

uint32 FCrowdyServerObjectAssetTypeActions::GetCategories()
{
	return FindOrRegisterCrowdyCategory();
}

void FCrowdyServerObjectAssetTypeActions::OpenAssetEditor(const TArray<UObject*>& InObjects, TSharedPtr<IToolkitHost> EditWithinLevelEditor)
{
	const EToolkitMode::Type Mode = EditWithinLevelEditor.IsValid() ? EToolkitMode::WorldCentric : EToolkitMode::Standalone;
	for (UObject* Object : InObjects)
	{
		UCrowdyServerObjectDefinition* Definition = Cast<UCrowdyServerObjectDefinition>(Object);
		if (!Definition)
		{
			continue;
		}
		const TSharedRef<FCrowdyServerObjectEditor> Editor = MakeShared<FCrowdyServerObjectEditor>();
		Editor->InitEditor(Mode, EditWithinLevelEditor, Definition);
	}
}

#undef LOCTEXT_NAMESPACE
