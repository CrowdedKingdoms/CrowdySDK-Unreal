#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Customizations/CrowdyChannelNameCustomization.h"
#include "IDetailTreeNode.h"
#include "IPropertyRowGenerator.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyChannelNameTestFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	TSharedPtr<IPropertyHandle> FindHandle(const TArray<TSharedRef<IDetailTreeNode>>& Nodes, const FName PropertyName)
	{
		for (const TSharedRef<IDetailTreeNode>& Node : Nodes)
		{
			const TSharedPtr<IPropertyHandle> Handle = Node->CreatePropertyHandle();
			if (Handle.IsValid() && Handle->GetProperty() && Handle->GetProperty()->GetFName() == PropertyName)
			{
				return Handle;
			}
			TArray<TSharedRef<IDetailTreeNode>> Children;
			Node->GetChildren(Children);
			if (const TSharedPtr<IPropertyHandle> Found = FindHandle(Children, PropertyName))
			{
				return Found;
			}
		}
		return nullptr;
	}

	/** Whether the channel name row claims the property as the details panel hands it over. */
	bool UsesChannelPicker(FPropertyEditorModule& PropertyEditor, const FName PropertyName, bool& bOutFound)
	{
		const TSharedRef<IPropertyRowGenerator> Generator = PropertyEditor.CreatePropertyRowGenerator(FPropertyRowGeneratorArgs());
		Generator->SetObjects({ GetMutableDefault<UCrowdySDKDeveloperSettings>() });
		const TSharedPtr<IPropertyHandle> Handle = FindHandle(Generator->GetRootTreeNodes(), PropertyName);
		bOutFound = Handle.IsValid();
		return bOutFound && FCrowdyChannelNameTypeIdentifier().IsPropertyTypeCustomized(*Handle);
	}
}

// Only an FString that asks for it gets the Pick button; every other text field keeps the plain box.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyChannelNameCustomizationMetaTest,
	"CrowdySDK.Editor.ChannelPickerOnTaggedStrings", CrowdyChannelNameTestFlags)
bool FCrowdyChannelNameCustomizationMetaTest::RunTest(const FString& Parameters)
{
	FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	const FName PropertyName = GET_MEMBER_NAME_CHECKED(UCrowdySDKDeveloperSettings, DiscoveryUrl);
	FProperty* Property = UCrowdySDKDeveloperSettings::StaticClass()->FindPropertyByName(PropertyName);
	if (!TestNotNull(TEXT("the settings carry a plain FString to tag"), Property))
	{
		return false;
	}

	bool bFound = false;
	TestFalse(TEXT("an untagged FString keeps the plain text box"), UsesChannelPicker(PropertyEditor, PropertyName, bFound));
	if (!TestTrue(TEXT("the details panel lists the property"), bFound))
	{
		return false;
	}

	Property->SetMetaData(FName(CrowdyChannelPickerMetaKey), TEXT(""));
	ON_SCOPE_EXIT { Property->RemoveMetaData(FName(CrowdyChannelPickerMetaKey)); };
	TestTrue(TEXT("an FString tagged CrowdyChannelPicker gets the channel row"), UsesChannelPicker(PropertyEditor, PropertyName, bFound));
	return true;
}

#endif
