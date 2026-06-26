#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Modules/ModuleManager.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/AssetRegistryTagsContext.h"

// Dirties a UserDefinedStruct and refreshes its asset-registry tags after its Crowdy
// metadata changes. Defined inline so the struct toolbar and the struct context menu
// share one copy inside Unreal's unity translation units rather than colliding.
inline void NotifyStructMetadataChanged(UUserDefinedStruct* Struct)
{
	if (!IsValid(Struct)) return;

	Struct->MarkPackageDirty();

	FAssetRegistryModule& AssetRegistryModule =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	AssetRegistryModule.Get().AssetUpdateTags(
		Struct,
		EAssetRegistryTagsCaller::FullUpdate);
}
