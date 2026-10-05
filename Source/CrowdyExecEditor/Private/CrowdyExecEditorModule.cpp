#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "CoreGlobals.h"
#include "CrowdyExecCodec.h"
#include "CrowdyExecCodegen.h"
#include "CrowdyExecCodegenMenu.h"
#include "CrowdyServerCodeFiles.h"
#include "CrowdyServerComputeService.h"
#include "CrowdyServerFunctionCustomization.h"
#include "CrowdyServerListRepair.h"
#include "CrowdyServerListValueNameCustomization.h"
#include "CrowdyServerNameCustomization.h"
#include "CrowdyServerObjectAssetTypeActions.h"
#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectDefinitionCustomization.h"
#include "CrowdyStudioModule.h"
#include "IAssetTools.h"
#include "Interfaces/IMainFrameModule.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Misc/MessageDialog.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "SCrowdyServerComputePanel.h"
#include "StructUtils/PropertyBag.h"
#include "StructUtils/UserDefinedStruct.h"
#include "ToolMenus.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "UObject/PackageReload.h"
#include "UObject/PropertyOptional.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "CrowdyExecEditor"

namespace CrowdyExecEditorModule
{
	const FName ServerComputePageId(TEXT("CrowdyServerCompute"));
	const FName PropertyEditorName(TEXT("PropertyEditor"));
	const FName AssetToolsName(TEXT("AssetTools"));
	const FName AssetRegistryName(TEXT("AssetRegistry"));
	const FName DefinitionClassName(TEXT("CrowdyServerObjectDefinition"));
	const FName FieldNameStructName(TEXT("CrowdyServerFieldName"));
	const FName EnumValueNameStructName(TEXT("CrowdyServerEnumValueName"));
	const FName ListValueNameStructName(TEXT("CrowdyServerListValueName"));
	const FName FunctionStructName(TEXT("CrowdyServerFunction"));
	const FName MainFrameName(TEXT("MainFrame"));

	bool StructReaches(const UStruct* Struct, const UScriptStruct* Target, TSet<const UStruct*>& Seen);

	bool PropertyReaches(const FProperty* Property, const UScriptStruct* Target, TSet<const UStruct*>& Seen)
	{
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			return StructReaches(StructProperty->Struct, Target, Seen);
		}
		if (const FArrayProperty* Array = CastField<FArrayProperty>(Property))
		{
			return PropertyReaches(Array->Inner, Target, Seen);
		}
		if (const FSetProperty* Set = CastField<FSetProperty>(Property))
		{
			return PropertyReaches(Set->ElementProp, Target, Seen);
		}
		if (const FMapProperty* Map = CastField<FMapProperty>(Property))
		{
			return PropertyReaches(Map->KeyProp, Target, Seen) || PropertyReaches(Map->ValueProp, Target, Seen);
		}
		const FOptionalProperty* Optional = CastField<FOptionalProperty>(Property);
		return Optional && PropertyReaches(Optional->GetValueProperty(), Target, Seen);
	}

	// True when Struct is Target or holds it at any depth; Seen walks each struct once.
	bool StructReaches(const UStruct* Struct, const UScriptStruct* Target, TSet<const UStruct*>& Seen)
	{
		if (!Struct)
		{
			return false;
		}
		if (Struct == Target)
		{
			return true;
		}
		bool bSeen = false;
		Seen.Add(Struct, &bSeen);
		if (bSeen)
		{
			return false;
		}
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			if (PropertyReaches(*It, Target, Seen))
			{
				return true;
			}
		}
		return false;
	}

	// Walks the structs a definition names as well as checking its tables, which a failed bake leaves empty.
	bool UsesStruct(const UCrowdyServerObjectDefinition& Definition, const UScriptStruct* Struct)
	{
		TSet<const UStruct*> Seen;
		if (StructReaches(Definition.GetStateStruct(), Struct, Seen))
		{
			return true;
		}
		for (const FCrowdyServerFunction& Function : Definition.Functions)
		{
			if (StructReaches(Function.GetParamsStruct(), Struct, Seen) || StructReaches(Function.GetReplyStruct(), Struct, Seen))
			{
				return true;
			}
		}
		return Definition.BakedStructs.ContainsByPredicate([Struct](const FCrowdyExecBakedStruct& Baked) { return Baked.Struct.Get() == Struct; });
	}

	// The struct a recompile's temporary copy stands in for; null for any other type.
	const UObject* PrimaryOfDuplicate(const UObject* Type)
	{
		const UUserDefinedStruct* Struct = Cast<UUserDefinedStruct>(Type);
		return (Struct && Struct->Status == EUserDefinedStructureStatus::UDSS_Duplicate) ? Struct->PrimaryStruct.Get() : nullptr;
	}

	// Values are carried over by property id; a value whose struct type changed starts from its defaults.
	bool RepairList(FInstancedPropertyBag& List)
	{
		const UPropertyBag* Bag = List.GetPropertyBagStruct();
		if (!Bag)
		{
			return false;
		}
		TArray<FPropertyBagPropertyDesc> Descs(Bag->GetPropertyDescs());
		bool bRepaired = false;
		for (FPropertyBagPropertyDesc& Desc : Descs)
		{
			if (const UObject* Primary = PrimaryOfDuplicate(Desc.ValueTypeObject))
			{
				Desc.ValueTypeObject = Primary;
				bRepaired = true;
			}
			if (const UObject* Primary = PrimaryOfDuplicate(Desc.KeyTypeObject))
			{
				Desc.KeyTypeObject = Primary;
				bRepaired = true;
			}
		}
		if (!bRepaired)
		{
			return false;
		}
		List.MigrateToNewBagStruct(UPropertyBag::GetOrCreateFromDescs(Descs));
		return true;
	}

	bool RepairDuplicateListTypes(UCrowdyServerObjectDefinition& Definition)
	{
		bool bRepaired = RepairList(Definition.StateList);
		for (FCrowdyServerFunction& Function : Definition.Functions)
		{
			bRepaired |= RepairList(Function.ParamsList);
			bRepaired |= RepairList(Function.ReplyList);
		}
		if (bRepaired)
		{
			Definition.MarkPackageDirty();
		}
		return bRepaired;
	}
}

// A Blueprint struct edit, a package reload and a reinstance all replace struct fields, so every resolved field table goes stale.
class FCrowdyExecEditorModule : public IModuleInterface, public FStructureEditorUtils::INotifyOnStructChanged
{
public:
	virtual void StartupModule() override
	{
		PackageReloadedHandle = FCoreUObjectDelegates::OnPackageReloaded.AddRaw(this, &FCrowdyExecEditorModule::HandlePackageReloaded);
		ObjectsReplacedHandle = FCoreUObjectDelegates::OnObjectsReplaced.AddRaw(this, &FCrowdyExecEditorModule::HandleObjectsReplaced);
		AssetRenamedHandle = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(CrowdyExecEditorModule::AssetRegistryName).Get().OnAssetRenamed().AddRaw(this, &FCrowdyExecEditorModule::HandleAssetRenamed);
		PackageSavedHandle = UPackage::PackageSavedWithContextEvent.AddRaw(this, &FCrowdyExecEditorModule::HandlePackageSaved);
		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FCrowdyExecEditorModule::RegisterMenus));
		CrowdyStudioExtraPages::RegisterPage(CrowdyExecEditorModule::ServerComputePageId, LOCTEXT("ServerComputeGroup", "COMPUTE"),
			LOCTEXT("ServerComputePage", "Server Compute"), TEXT("server"),
			[]() -> TSharedRef<SWidget> { return SNew(SCrowdyServerComputePanel); });
		FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(CrowdyExecEditorModule::PropertyEditorName);
		PropertyEditor.RegisterCustomClassLayout(CrowdyExecEditorModule::DefinitionClassName,
			FOnGetDetailCustomizationInstance::CreateStatic(&FCrowdyServerObjectDefinitionCustomization::MakeInstance));
		PropertyEditor.RegisterCustomPropertyTypeLayout(CrowdyExecEditorModule::FieldNameStructName,
			FOnGetPropertyTypeCustomizationInstance::CreateStatic(&FCrowdyServerNameCustomization::MakeFieldInstance));
		PropertyEditor.RegisterCustomPropertyTypeLayout(CrowdyExecEditorModule::EnumValueNameStructName,
			FOnGetPropertyTypeCustomizationInstance::CreateStatic(&FCrowdyServerNameCustomization::MakeEnumValueInstance));
		PropertyEditor.RegisterCustomPropertyTypeLayout(CrowdyExecEditorModule::ListValueNameStructName,
			FOnGetPropertyTypeCustomizationInstance::CreateStatic(&FCrowdyServerListValueNameCustomization::MakeInstance));
		PropertyEditor.RegisterCustomPropertyTypeLayout(CrowdyExecEditorModule::FunctionStructName,
			FOnGetPropertyTypeCustomizationInstance::CreateStatic(&FCrowdyServerFunctionCustomization::MakeInstance));
		ServerObjectActions = MakeShared<FCrowdyServerObjectAssetTypeActions>();
		FModuleManager::LoadModuleChecked<FAssetToolsModule>(CrowdyExecEditorModule::AssetToolsName).Get().RegisterAssetTypeActions(ServerObjectActions.ToSharedRef());
		IMainFrameModule* MainFrame = GIsEditor && !IsRunningCommandlet() ? FModuleManager::LoadModulePtr<IMainFrameModule>(CrowdyExecEditorModule::MainFrameName) : nullptr;
		if (MainFrame)
		{
			CanCloseEditorHandle = MainFrame->RegisterCanCloseEditor(IMainFrameModule::FMainFrameCanCloseEditor::CreateRaw(this, &FCrowdyExecEditorModule::CanCloseEditor));
		}
	}

	virtual void ShutdownModule() override
	{
		IMainFrameModule* MainFrame = CanCloseEditorHandle.IsValid() ? FModuleManager::GetModulePtr<IMainFrameModule>(CrowdyExecEditorModule::MainFrameName) : nullptr;
		if (MainFrame)
		{
			MainFrame->UnregisterCanCloseEditor(CanCloseEditorHandle);
		}
		if (FPropertyEditorModule* PropertyEditor = FModuleManager::GetModulePtr<FPropertyEditorModule>(CrowdyExecEditorModule::PropertyEditorName))
		{
			PropertyEditor->UnregisterCustomClassLayout(CrowdyExecEditorModule::DefinitionClassName);
			PropertyEditor->UnregisterCustomPropertyTypeLayout(CrowdyExecEditorModule::FieldNameStructName);
			PropertyEditor->UnregisterCustomPropertyTypeLayout(CrowdyExecEditorModule::EnumValueNameStructName);
			PropertyEditor->UnregisterCustomPropertyTypeLayout(CrowdyExecEditorModule::ListValueNameStructName);
			PropertyEditor->UnregisterCustomPropertyTypeLayout(CrowdyExecEditorModule::FunctionStructName);
		}
		FAssetToolsModule* AssetTools = ServerObjectActions.IsValid() ? FModuleManager::GetModulePtr<FAssetToolsModule>(CrowdyExecEditorModule::AssetToolsName) : nullptr;
		if (AssetTools)
		{
			AssetTools->Get().UnregisterAssetTypeActions(ServerObjectActions.ToSharedRef());
		}
		ServerObjectActions.Reset();
		FCrowdyServerComputeService::Shutdown();
		CrowdyStudioExtraPages::UnregisterPage(CrowdyExecEditorModule::ServerComputePageId);
		FCoreUObjectDelegates::OnPackageReloaded.Remove(PackageReloadedHandle);
		FCoreUObjectDelegates::OnObjectsReplaced.Remove(ObjectsReplacedHandle);
		UPackage::PackageSavedWithContextEvent.Remove(PackageSavedHandle);
		if (IAssetRegistry* AssetRegistry = IAssetRegistry::Get())
		{
			AssetRegistry->OnAssetRenamed().Remove(AssetRenamedHandle);
		}
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
	}

	virtual void PreChange(const UUserDefinedStruct* Changed, FStructureEditorUtils::EStructureEditorChangeInfo ChangedType) override
	{
	}

	// Re-baked now, so editor play uses the tables the next save or cook would write.
	virtual void PostChange(const UUserDefinedStruct* Changed, FStructureEditorUtils::EStructureEditorChangeInfo ChangedType) override
	{
		CrowdyExec::NotifyStructsChanged();
		for (TObjectIterator<UCrowdyServerObjectDefinition> It; It; ++It)
		{
			const bool bRepaired = CrowdyExecEditorModule::RepairDuplicateListTypes(**It);
			if (!bRepaired && !CrowdyExecEditorModule::UsesStruct(**It, Changed))
			{
				continue;
			}
			// Re-bakes through the definition's own edit notice, so open editors and details panels see the new fields too.
			It->PostEditChange();
			It->MarkPackageDirty();
		}
	}

private:
	void RegisterMenus()
	{
		FToolMenuOwnerScoped OwnerScoped(this);
		CrowdyExecCodegenMenu::ExtendAssetMenu();
	}

	void HandlePackageReloaded(EPackageReloadPhase Phase, FPackageReloadedEvent* Event)
	{
		if (Phase == EPackageReloadPhase::PrePackageFixup)
		{
			CrowdyExec::NotifyStructsChanged();
		}
	}

	void HandleObjectsReplaced(const TMap<UObject*, UObject*>& Replaced)
	{
		CrowdyExec::NotifyStructsChanged();
	}

	// A crate names the definition asset it was generated from, so a moved or renamed asset still finds its crate when its Type Name changes.
	void HandleAssetRenamed(const FAssetData& Asset, const FString& OldObjectPath)
	{
		if (Asset.AssetClassPath != UCrowdyServerObjectDefinition::StaticClass()->GetClassPathName())
		{
			return;
		}
		PendingRetags.Record(OldObjectPath, Asset.GetObjectPathString());
	}

	void HandlePackageSaved(const FString& PackageFileName, UPackage* Package, FObjectPostSaveContext SaveContext)
	{
		if (!Package)
		{
			return;
		}
		for (const TPair<FString, FString>& Move : PendingRetags.TakeForPackage(Package->GetName()))
		{
			CrowdyExecCodegen::RetagCrates(CrowdyExecCodegen::GetServerDirectory(), Move.Key, Move.Value);
		}
	}

	bool CanCloseEditor()
	{
		const TArray<FString> Unsaved = CrowdyServerCodeEdits::UnsavedFiles();
		if (Unsaved.IsEmpty())
		{
			return true;
		}
		const FText Message = FText::Format(LOCTEXT("QuitWithUnsavedServerCode", "These server code files have edits that are not saved:\n\n{0}\n\nQuit anyway and lose the edits?"),
			FText::FromString(FString::Join(Unsaved, TEXT("\n"))));
		return FMessageDialog::Open(EAppMsgCategory::Warning, EAppMsgType::YesNo, EAppReturnType::No, Message, LOCTEXT("QuitWithUnsavedServerCodeTitle", "Server Code")) == EAppReturnType::Yes;
	}

	TSharedPtr<FCrowdyServerObjectAssetTypeActions> ServerObjectActions;
	FDelegateHandle CanCloseEditorHandle;
	FDelegateHandle PackageReloadedHandle;
	FDelegateHandle ObjectsReplacedHandle;
	FDelegateHandle AssetRenamedHandle;
	FDelegateHandle PackageSavedHandle;
	CrowdyExecCodegenMenu::FPendingRetags PendingRetags;
};

IMPLEMENT_MODULE(FCrowdyExecEditorModule, CrowdyExecEditor);

#undef LOCTEXT_NAMESPACE
