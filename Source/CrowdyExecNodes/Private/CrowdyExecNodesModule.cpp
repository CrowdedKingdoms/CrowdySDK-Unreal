#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "BlueprintActionDatabase.h"
#include "CrowdyServerFunctionPickerPin.h"
#include "CrowdyServerObjectDefinition.h"
#include "EdGraphUtilities.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"

// Hosts the Server Object Blueprint nodes, which must live in an UncookedOnly module so they exist while editing
// and cooking but are stripped from the packaged game.
class FCrowdyExecNodesModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		PinFactory = MakeShared<FCrowdyServerFunctionPickerPinFactory>();
		FEdGraphUtilities::RegisterVisualPinFactory(PinFactory);

		if (!GIsEditor || IsRunningCommandlet())
		{
			return;
		}
		PropertyChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddRaw(this, &FCrowdyExecNodesModule::HandleObjectPropertyChanged);
		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		RenamedHandle = AssetRegistry.OnAssetRenamed().AddRaw(this, &FCrowdyExecNodesModule::HandleAssetRenamed);
		if (AssetRegistry.IsLoadingAssets())
		{
			FilesLoadedHandle = AssetRegistry.OnFilesLoaded().AddRaw(this, &FCrowdyExecNodesModule::LoadDefinitions);
			return;
		}
		LoadDefinitions();
	}

	virtual void ShutdownModule() override
	{
		FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PropertyChangedHandle);
		if (IAssetRegistry* AssetRegistry = IAssetRegistry::Get())
		{
			AssetRegistry->OnAssetRenamed().Remove(RenamedHandle);
			AssetRegistry->OnFilesLoaded().Remove(FilesLoadedHandle);
		}
		if (PinFactory.IsValid())
		{
			FEdGraphUtilities::UnregisterVisualPinFactory(PinFactory);
			PinFactory.Reset();
		}
	}

private:
	// The menu offers loaded definitions; the action database adds each one's entries as it loads.
	void LoadDefinitions()
	{
		TArray<FAssetData> Assets;
		IAssetRegistry::GetChecked().GetAssetsByClass(UCrowdyServerObjectDefinition::StaticClass()->GetClassPathName(), Assets, true);
		for (const FAssetData& Asset : Assets)
		{
			Asset.GetAsset();
		}
	}

	// A menu entry's category names its asset, which the action database does not refresh on a rename.
	void HandleAssetRenamed(const FAssetData& Asset, const FString& OldName)
	{
		if (!Asset.IsAssetLoaded() || !Asset.IsInstanceOf<UCrowdyServerObjectDefinition>())
		{
			return;
		}
		FBlueprintActionDatabase::Get().RefreshAssetActions(Asset.GetAsset());
	}

	void HandleObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& Event)
	{
		if (!Object || !Object->IsA<UCrowdyServerObjectDefinition>() || !Object->IsAsset() || Event.ChangeType == EPropertyChangeType::Interactive)
		{
			return;
		}
		FBlueprintActionDatabase::Get().RefreshAssetActions(Object);
	}

	TSharedPtr<FGraphPanelPinFactory> PinFactory;
	FDelegateHandle PropertyChangedHandle;
	FDelegateHandle RenamedHandle;
	FDelegateHandle FilesLoadedHandle;
};

IMPLEMENT_MODULE(FCrowdyExecNodesModule, CrowdyExecNodes);
