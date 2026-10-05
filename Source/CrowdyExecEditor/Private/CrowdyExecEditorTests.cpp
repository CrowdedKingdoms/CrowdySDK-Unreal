#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyExecCodec.h"
#include "CrowdyServerListRepair.h"
#include "CrowdyServerObjectDefinition.h"
#include "EdGraphSchema_K2.h"
#include "Interfaces/ITargetPlatform.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "StructUtils/PropertyBag.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/StructOnScope.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"

namespace CrowdyExecEditorTests
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	TArray<uint8> FromHex(const FString& Hex)
	{
		TArray<uint8> Bytes;
		Bytes.SetNumUninitialized(Hex.Len() / 2);
		HexToBytes(Hex, Bytes.GetData());
		return Bytes;
	}

	FString ToHex(TConstArrayView<uint8> Bytes)
	{
		return BytesToHex(Bytes.GetData(), Bytes.Num()).ToLower();
	}

	const FIntProperty* FindByGuid(const UUserDefinedStruct& Struct, const FGuid& Guid)
	{
		return FindFProperty<FIntProperty>(&Struct, Struct.FindPropertyNameFromGuid(Guid));
	}
}

using namespace CrowdyExecEditorTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecBlueprintStructTest, "CrowdySDK.CrowdyExecEditor.BlueprintStructFieldSurvivesRecompile", TestFlags)
bool FCrowdyExecBlueprintStructTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UUserDefinedStruct> Struct(FStructureEditorUtils::CreateUserDefinedStruct(GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UUserDefinedStruct::StaticClass(), TEXT("S_CrowdyExecTest")), RF_Transient));
	if (!TestNotNull(TEXT("the Blueprint struct is created"), Struct.Get()))
	{
		return false;
	}
	const FGuid FieldGuid = FStructureEditorUtils::GetVarDesc(Struct.Get())[0].VarGuid;
	FStructureEditorUtils::ChangeVariableType(Struct.Get(), FieldGuid,
		FEdGraphPinType(UEdGraphSchema_K2::PC_Int, NAME_None, nullptr, EPinContainerType::None, false, FEdGraphTerminalType()));
	FStructureEditorUtils::RenameVariable(Struct.Get(), FieldGuid, TEXT("Max Health"));
	FStructureEditorUtils::ChangeVariableDefaultValue(Struct.Get(), FieldGuid, TEXT("250"));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage()));
	Definition->TypeName = TEXT("test_bp_boss");
	Definition->State = Struct.Get();
	TArray<FString> Errors;
	const bool bBaked = Definition->Bake(Errors);
	if (!TestTrue(FString::Printf(TEXT("the definition bakes (%s)"), *FString::Join(Errors, TEXT("; "))), bBaked))
	{
		return false;
	}
	const FCrowdyExecBakedField& Baked = Definition->BakedStructs[0].Fields[0];
	TestEqual(TEXT("the authored name loses its space on the wire"), Baked.ServerName, FString(TEXT("MaxHealth")));
	TestTrue(TEXT("the field is identified by its GUID"), Baked.PropertyGuid == FieldGuid);

	FString Error;
	{
		FStructOnScope Value(Struct.Get());
		const bool bDecoded = CrowdyExec::Decode(*Definition, *Struct, FromHex(TEXT("80")), Value.GetStructMemory(), Error);
		TestTrue(FString::Printf(TEXT("an empty map decodes (%s)"), *Error), bDecoded);
		TestEqual(TEXT("a missing field takes the Blueprint struct's default, not zero"),
			FindByGuid(*Struct, FieldGuid)->GetPropertyValue_InContainer(Value.GetStructMemory()), 250);
	}

	const FProperty* Before = CrowdyExec::FindResolvedPropertyForTest(*Definition, *Struct, TEXT("MaxHealth"));
	const FName NameBefore = Before ? Before->GetFName() : NAME_None;
	FStructureEditorUtils::RenameVariable(Struct.Get(), FieldGuid, TEXT("Top Health"));
	const FIntProperty* Current = FindByGuid(*Struct, FieldGuid);
	if (!TestTrue(TEXT("the rename recompiled the struct under a new property name"), Current && Current->GetFName() != NameBefore))
	{
		return false;
	}
	FCrowdyExecBakedField& Rebaked = Definition->BakedStructs[0].Fields[0];
	TestEqual(TEXT("editing the struct re-baked the definition"), Rebaked.ServerName, FString(TEXT("TopHealth")));
	// Compared before any use: a stale pointer here points at a property the recompile destroyed.
	if (!TestTrue(TEXT("the field is found after the recompile"), CrowdyExec::FindResolvedPropertyForTest(*Definition, *Struct, TEXT("TopHealth")) == Current))
	{
		return false;
	}
	// As a struct recompiled without a re-bake leaves it: the property name is stale, and the GUID still finds the field.
	Rebaked.Property = TEXT("Stale");
	CrowdyExec::NotifyStructsChanged();
	if (!TestTrue(TEXT("the field is found by its GUID"), CrowdyExec::FindResolvedPropertyForTest(*Definition, *Struct, TEXT("TopHealth")) == Current))
	{
		return false;
	}

	FStructOnScope Value(Struct.Get());
	Current->SetPropertyValue_InContainer(Value.GetStructMemory(), 99);
	TArray<uint8> Bytes;
	const bool bEncoded = CrowdyExec::Encode(*Definition, Struct.Get(), Value.GetStructMemory(), Bytes, Error);
	TestTrue(FString::Printf(TEXT("encodes after the recompile (%s)"), *Error), bEncoded);
	TestEqual(TEXT("under the re-baked server name"), ToHex(Bytes), FString(TEXT("81a9546f704865616c746863")));
	const bool bDecoded = CrowdyExec::Decode(*Definition, *Struct, FromHex(TEXT("81a9546f704865616c746805")), Value.GetStructMemory(), Error);
	TestTrue(FString::Printf(TEXT("decodes after the recompile (%s)"), *Error), bDecoded);
	TestEqual(TEXT("into the recompiled field"), Current->GetPropertyValue_InContainer(Value.GetStructMemory()), 5);

	Rebaked.PropertyGuid = FGuid::NewGuid();
	TestTrue(TEXT("a resolved table is kept while nothing is replaced"), CrowdyExec::FindResolvedPropertyForTest(*Definition, *Struct, TEXT("TopHealth")) == Current);
	FCoreUObjectDelegates::OnObjectsReplaced.Broadcast(TMap<UObject*, UObject*>());
	TestNull(TEXT("replacing objects drops every resolved table"), CrowdyExec::FindResolvedPropertyForTest(*Definition, *Struct, TEXT("TopHealth")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCookFailsTest, "CrowdySDK.CrowdyExecEditor.CookFailsOnADefinitionThatCannotBake", TestFlags)
bool FCrowdyExecCookFailsTest::RunTest(const FString& Parameters)
{
	ITargetPlatform* Platform = GetTargetPlatformManagerRef().GetRunningTargetPlatform();
	if (!TestNotNull(TEXT("a platform to cook for"), Platform))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage()));
	Definition->TypeName = TEXT("test_no_state");
	// Only an Error satisfies an Error-minimum expectation, so a Warning here fails the test.
	AddExpectedMessagePlain(TEXT("Choose a State struct"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, 1);
	FObjectSaveContextData Saving;
	Saving.TargetPlatform = Platform;
	Definition->PreSave(FObjectPreSaveContext(Saving));
	TestTrue(TEXT("a cook that cannot bake the definition also leaves it without tables"), Definition->BakedStructs.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecStructEditRepairTest, "CrowdySDK.CrowdyExecEditor.StructEditRepairsDuplicateListTypes", TestFlags)
bool FCrowdyExecStructEditRepairTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UUserDefinedStruct> Struct(FStructureEditorUtils::CreateUserDefinedStruct(GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UUserDefinedStruct::StaticClass(), TEXT("S_CrowdyRepairTest")), RF_Transient));
	TStrongObjectPtr<UUserDefinedStruct> Duplicate(FStructureEditorUtils::CreateUserDefinedStruct(GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UUserDefinedStruct::StaticClass(), TEXT("S_CrowdyRepairTestCopy")), RF_Transient));
	if (!TestTrue(TEXT("the Blueprint structs are created"), Struct.IsValid() && Duplicate.IsValid()))
	{
		return false;
	}
	// Marked as the engine marks the copy it makes while recompiling Struct.
	Duplicate->PrimaryStruct = Struct.Get();
	Duplicate->Status = EUserDefinedStructureStatus::UDSS_Duplicate;

	// Not transient packages, which are never marked dirty.
	UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Temp/CrowdyExecEditorTests/DA_Repair_%s"), *FGuid::NewGuid().ToString()));
	UPackage* CleanPackage = CreatePackage(*FString::Printf(TEXT("/Temp/CrowdyExecEditorTests/DA_NoRepair_%s"), *FGuid::NewGuid().ToString()));
	ON_SCOPE_EXIT
	{
		Package->SetDirtyFlag(false);
		CleanPackage->SetDirtyFlag(false);
	};
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition(NewObject<UCrowdyServerObjectDefinition>(Package, TEXT("DA_Repair")));
	Definition->StateList.AddProperty(TEXT("Amount"), EPropertyBagPropertyType::Int32);
	Definition->StateList.SetValueInt32(TEXT("Amount"), 7);
	Definition->StateList.AddProperty(TEXT("Stats"), EPropertyBagPropertyType::Struct, Duplicate.Get());
	Definition->Functions.AddDefaulted_GetRef().ParamsList.AddProperty(TEXT("Stats"), EPropertyBagPropertyType::Struct, Duplicate.Get());
	TestFalse(TEXT("the package starts clean"), Package->IsDirty());

	TestTrue(TEXT("the repair reports a moved List"), CrowdyExecEditorModule::RepairDuplicateListTypes(*Definition));
	TestTrue(TEXT("and dirties the package"), Package->IsDirty());
	const FPropertyBagPropertyDesc* State = Definition->StateList.FindPropertyDescByName(TEXT("Stats"));
	const FStructProperty* StateProperty = State ? CastField<FStructProperty>(State->CachedProperty) : nullptr;
	TestTrue(TEXT("the Variables name the struct itself"), State && State->ValueTypeObject == Struct.Get());
	TestTrue(TEXT("and so does their property"), StateProperty && StateProperty->Struct == Struct.Get());
	const FPropertyBagPropertyDesc* Params = Definition->Functions[0].ParamsList.FindPropertyDescByName(TEXT("Stats"));
	TestTrue(TEXT("the function's Inputs name the struct itself"), Params && Params->ValueTypeObject == Struct.Get());
	const TValueOrError<int32, EPropertyBagResult> Amount = Definition->StateList.GetValueInt32(TEXT("Amount"));
	TestTrue(TEXT("another variable keeps its value"), Amount.HasValue() && Amount.GetValue() == 7);

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Clean(NewObject<UCrowdyServerObjectDefinition>(CleanPackage, TEXT("DA_NoRepair")));
	Clean->StateList.AddProperty(TEXT("Stats"), EPropertyBagPropertyType::Struct, Struct.Get());
	const UPropertyBag* CleanBag = Clean->StateList.GetPropertyBagStruct();
	TestFalse(TEXT("a definition on the struct itself needs no repair"), CrowdyExecEditorModule::RepairDuplicateListTypes(*Clean));
	TestFalse(TEXT("and its package stays clean"), CleanPackage->IsDirty());
	TestTrue(TEXT("and its Variables keep their bag"), CleanBag && Clean->StateList.GetPropertyBagStruct() == CleanBag);

	// The editor's own struct-change notice repairs a definition left on the copy, as a recompile can leave one.
	UPackage* HookPackage = CreatePackage(*FString::Printf(TEXT("/Temp/CrowdyExecEditorTests/DA_RepairHook_%s"), *FGuid::NewGuid().ToString()));
	ON_SCOPE_EXIT
	{
		HookPackage->SetDirtyFlag(false);
	};
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Left(NewObject<UCrowdyServerObjectDefinition>(HookPackage, TEXT("DA_RepairHook")));
	Left->TypeName = TEXT("repair_hook");
	Left->StateForm = ECrowdyServerValuesForm::List;
	Left->StateList.AddProperty(TEXT("Stats"), EPropertyBagPropertyType::Struct, Duplicate.Get());
	FStructureEditorUtils::BroadcastPostChange(Struct.Get());
	const FPropertyBagPropertyDesc* Hooked = Left->StateList.FindPropertyDescByName(TEXT("Stats"));
	TestTrue(TEXT("a struct change repairs a definition left on the copy"), Hooked && Hooked->ValueTypeObject == Struct.Get());
	return true;
}

#endif
