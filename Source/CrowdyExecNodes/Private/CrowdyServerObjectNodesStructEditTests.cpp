#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "BlueprintActionDatabase.h"
#include "BlueprintNodeSpawner.h"
#include "CrowdyServerObjectDefinition.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Misc/ScopeExit.h"
#include "StructUtils/PropertyBag.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"

namespace CrowdyServerObjectNodesStructEditTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	TArray<const UBlueprintNodeSpawner*> Spawners(UCrowdyServerObjectDefinition* Definition)
	{
		TArray<const UBlueprintNodeSpawner*> Out;
		if (const FBlueprintActionDatabase::FActionList* Actions = FBlueprintActionDatabase::Get().GetAllActions().Find(Definition))
		{
			for (const UBlueprintNodeSpawner* Spawner : *Actions)
			{
				Out.Add(Spawner);
			}
		}
		return Out;
	}

	bool Offers(UCrowdyServerObjectDefinition* Definition, const FString& Title)
	{
		return Spawners(Definition).ContainsByPredicate([&Title](const UBlueprintNodeSpawner* Spawner)
		{
			return Spawner->DefaultMenuSignature.MenuName.ToString() == Title;
		});
	}

	// A Blueprint struct whose one field is FieldA.
	TStrongObjectPtr<UUserDefinedStruct> MakeStruct()
	{
		TStrongObjectPtr<UUserDefinedStruct> Struct(FStructureEditorUtils::CreateUserDefinedStruct(GetTransientPackage(),
			MakeUniqueObjectName(GetTransientPackage(), UUserDefinedStruct::StaticClass(), TEXT("S_CrowdyMenuTest")), RF_Transient));
		if (Struct.IsValid())
		{
			FStructureEditorUtils::RenameVariable(Struct.Get(), FStructureEditorUtils::GetVarDesc(Struct.Get())[0].VarGuid, TEXT("FieldA"));
		}
		return Struct;
	}

	// What a failed re-bake after a nested struct edit saw: the List's struct, its field's struct, a bake now, and whether the editor could find the definition.
	FString DescribeNestedEdit(UCrowdyServerObjectDefinition& Definition, const UUserDefinedStruct* Struct)
	{
		const UPropertyBag* Bag = Definition.StateList.GetPropertyBagStruct();
		const FPropertyBagPropertyDesc* Desc = Definition.StateList.FindPropertyDescByName(TEXT("Stats"));
		const FStructProperty* Property = Bag ? CastField<FStructProperty>(Bag->FindPropertyByName(TEXT("Stats"))) : nullptr;
		bool bIterated = false;
		for (TObjectIterator<UCrowdyServerObjectDefinition> It; It; ++It)
		{
			bIterated |= *It == &Definition;
		}
		TArray<FString> Errors;
		const bool bBakes = Definition.Bake(Errors);
		return FString::Printf(TEXT("bag=%s newer=%d descType=%s descIsStruct=%d propStruct=%s propIsStruct=%d structStatus=%d fields=%s iterated=%d garbage=%d bakeNow=%d errors=%s"),
			*GetNameSafe(Bag), Bag ? (Bag->StructFlags & STRUCT_NewerVersionExists) != 0 : -1, Desc ? *GetNameSafe(Desc->ValueTypeObject) : TEXT("no desc"),
			Desc && Desc->ValueTypeObject == Struct, Property ? *GetNameSafe(Property->Struct) : TEXT("no property"), Property && Property->Struct == Struct,
			static_cast<int32>(Struct->Status.GetValue()), *FString::JoinBy(FStructureEditorUtils::GetVarDesc(Struct), TEXT(","), [](const FStructVariableDescription& Var) { return Var.VarName.ToString(); }),
			bIterated, Definition.HasAnyInternalFlags(EInternalObjectFlags::Garbage), bBakes, *FString::Join(Errors, TEXT(" | ")));
	}

	// Adds FieldB to the State's Blueprint struct and checks the menu offers it with no edit to the definition; bBakeFails watches it before it exists.
	void AddFieldAndCheckMenu(FAutomationTestBase& Test, bool bBakeFails)
	{
		TStrongObjectPtr<UUserDefinedStruct> Struct = MakeStruct();
		if (!Test.TestNotNull(TEXT("the Blueprint struct is created"), Struct.Get()))
		{
			return;
		}

		// Saved as an asset, as the menu offers only asset definitions.
		UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Temp/CrowdyExecNodesTests/DA_StructEdit_%s"), *FGuid::NewGuid().ToString()));
		UCrowdyServerObjectDefinition* Definition = NewObject<UCrowdyServerObjectDefinition>(Package, TEXT("DA_StructEdit"), RF_Public | RF_Standalone);
		ON_SCOPE_EXIT
		{
			FBlueprintActionDatabase::Get().ClearAssetActions(Definition);
			Definition->ClearFlags(RF_Public | RF_Standalone);
			Definition->MarkAsGarbage();
		};
		Definition->TypeName = TEXT("struct_edit");
		Definition->State = Struct.Get();
		Definition->WatchedFields = {TEXT("FieldA")};
		if (bBakeFails)
		{
			Definition->WatchedFields.Add(TEXT("FieldB"));
		}
		TArray<FString> Errors;
		Test.TestTrue(TEXT("the first bake succeeds unless FieldB is watched before it exists"), Definition->Bake(Errors) != bBakeFails);
		Test.TestTrue(TEXT("a failed bake leaves the definition without tables"), Definition->BakedStructs.IsEmpty() == bBakeFails);

		const FString Asset = Definition->GetName();
		const FString GetFieldA = FString::Printf(TEXT("Get FieldA (%s)"), *Asset);
		const FString GetFieldB = FString::Printf(TEXT("Get FieldB (%s)"), *Asset);
		const FString OnFieldBChanged = FString::Printf(TEXT("On FieldB Changed (%s)"), *Asset);
		FBlueprintActionDatabase::Get().RefreshAssetActions(Definition);
		Test.TestTrue(TEXT("the menu offers Get FieldA"), Offers(Definition, GetFieldA));
		Test.TestFalse(TEXT("the menu has no Get FieldB yet"), Offers(Definition, GetFieldB));

		FStructureEditorUtils::AddVariable(Struct.Get(), FEdGraphPinType(UEdGraphSchema_K2::PC_Int, NAME_None, nullptr, EPinContainerType::None, false, FEdGraphTerminalType()));
		const FGuid FieldB = FStructureEditorUtils::GetVarDesc(Struct.Get()).Last().VarGuid;
		if (!bBakeFails)
		{
			// Watched only now, so the struct edit below is the only thing that can refresh the menu, and the bake before it succeeded.
			Definition->WatchedFields.Add(TEXT("FieldB"));
			Test.TestFalse(TEXT("adding a field keeps the tables of a definition that bakes"), Definition->BakedStructs.IsEmpty());
		}
		FStructureEditorUtils::RenameVariable(Struct.Get(), FieldB, TEXT("FieldB"));

		Test.TestTrue(TEXT("after the struct edit the menu offers Get FieldB"), Offers(Definition, GetFieldB));
		Test.TestTrue(TEXT("and On FieldB Changed"), Offers(Definition, OnFieldBChanged));
		Test.TestFalse(TEXT("the rename fixed the bake"), Definition->BakedStructs.IsEmpty());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesStructEditMenuTest, "CrowdySDK.Editor.ServerObjectNodes.BlueprintStructEditRefreshesMenu", CrowdyServerObjectNodesStructEditTests::Flags)
bool FCrowdyServerObjectNodesStructEditMenuTest::RunTest(const FString& Parameters)
{
	CrowdyServerObjectNodesStructEditTests::AddFieldAndCheckMenu(*this, false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesStructEditFailedBakeTest, "CrowdySDK.Editor.ServerObjectNodes.BlueprintStructEditRefreshesMenuAfterFailedBake",
	CrowdyServerObjectNodesStructEditTests::Flags)
bool FCrowdyServerObjectNodesStructEditFailedBakeTest::RunTest(const FString& Parameters)
{
	CrowdyServerObjectNodesStructEditTests::AddFieldAndCheckMenu(*this, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectNodesNestedStructEditTest, "CrowdySDK.Editor.ServerObjectNodes.NestedBlueprintStructEditRefreshesMenu",
	CrowdyServerObjectNodesStructEditTests::Flags)
bool FCrowdyServerObjectNodesNestedStructEditTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectNodesStructEditTests;
	TStrongObjectPtr<UUserDefinedStruct> Struct = MakeStruct();
	if (!TestNotNull(TEXT("the Blueprint struct is created"), Struct.Get()))
	{
		return false;
	}
	UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Temp/CrowdyExecNodesTests/DA_NestedEdit_%s"), *FGuid::NewGuid().ToString()));
	UCrowdyServerObjectDefinition* Definition = NewObject<UCrowdyServerObjectDefinition>(Package, TEXT("DA_NestedEdit"), RF_Public | RF_Standalone);
	ON_SCOPE_EXIT
	{
		FBlueprintActionDatabase::Get().ClearAssetActions(Definition);
		Definition->ClearFlags(RF_Public | RF_Standalone);
		Definition->MarkAsGarbage();
	};
	// The struct is only the type of a List variable, and a server name for its missing FieldB fails every bake until the struct has it.
	Definition->TypeName = TEXT("nested_edit");
	Definition->StateForm = ECrowdyServerValuesForm::List;
	Definition->StateList.AddProperty(TEXT("Stats"), EPropertyBagPropertyType::Struct, Struct.Get());
	Definition->WatchedFields = {TEXT("Stats")};
	FCrowdyServerFieldName& ServerName = Definition->FieldNames.AddDefaulted_GetRef();
	ServerName.Struct = Struct.Get();
	ServerName.Field = TEXT("FieldB");
	ServerName.ServerName = TEXT("field_b");
	TArray<FString> Errors;
	TestFalse(TEXT("the bake fails while FieldB is missing"), Definition->Bake(Errors));
	TestTrue(TEXT("leaving the definition without tables"), Definition->BakedStructs.IsEmpty());

	FBlueprintActionDatabase::Get().RefreshAssetActions(Definition);
	const TArray<const UBlueprintNodeSpawner*> Before = Spawners(Definition);
	TestFalse(TEXT("the menu offers the definition's nodes"), Before.IsEmpty());

	FStructureEditorUtils::AddVariable(Struct.Get(), FEdGraphPinType(UEdGraphSchema_K2::PC_Int, NAME_None, nullptr, EPinContainerType::None, false, FEdGraphTerminalType()));
	FStructureEditorUtils::RenameVariable(Struct.Get(), FStructureEditorUtils::GetVarDesc(Struct.Get()).Last().VarGuid, TEXT("FieldB"));

	if (!TestFalse(TEXT("editing the nested struct re-baked the definition"), Definition->BakedStructs.IsEmpty()))
	{
		AddError(DescribeNestedEdit(*Definition, Struct.Get()));
	}
	const TArray<const UBlueprintNodeSpawner*> After = Spawners(Definition);
	const bool bRebuilt = !After.IsEmpty() && !After.ContainsByPredicate([&Before](const UBlueprintNodeSpawner* Spawner) { return Before.Contains(Spawner); });
	TestTrue(TEXT("and rebuilt its menu entries"), bRebuilt);
	return true;
}

#endif
