#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyExecInternal.h"
#include "EdGraphSchema_K2.h"
#include "HAL/PlatformTime.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Misc/AutomationTest.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecFieldLookupTest, "CrowdySDK.CrowdyExecEditor.FieldLookupFollowsAStructEdit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FCrowdyExecFieldLookupTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UUserDefinedStruct> Struct(FStructureEditorUtils::CreateUserDefinedStruct(GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UUserDefinedStruct::StaticClass(), TEXT("S_CrowdyLookup")), RF_Transient));
	const FEdGraphPinType IntPin(UEdGraphSchema_K2::PC_Int, NAME_None, nullptr, EPinContainerType::None, false, FEdGraphTerminalType());
	for (int32 Index = 1; Index < 20; ++Index)
	{
		FStructureEditorUtils::AddVariable(Struct.Get(), IntPin);
	}
	int32 Index = 0;
	const TArray<FStructVariableDescription> Vars = FStructureEditorUtils::GetVarDesc(Struct.Get());
	for (const FStructVariableDescription& Var : Vars)
	{
		FStructureEditorUtils::ChangeVariableType(Struct.Get(), Var.VarGuid, IntPin);
		FStructureEditorUtils::RenameVariable(Struct.Get(), Var.VarGuid, FString::Printf(TEXT("Field %d"), Index++));
	}

	const FProperty* Last = CrowdyExec::FindTravellingField(Struct.Get(), TEXT("Field19"));
	if (!TestNotNull(TEXT("the last field is found by its name without the space"), Last))
	{
		return false;
	}
	constexpr int32 Reads = 20000;
	const double Start = FPlatformTime::Seconds();
	int32 Same = 0;
	for (int32 Read = 0; Read < Reads; ++Read)
	{
		Same += CrowdyExec::FindTravellingField(Struct.Get(), TEXT("Field19")) == Last ? 1 : 0;
	}
	AddInfo(FString::Printf(TEXT("the last of 20 Blueprint-struct fields: %.3f us per read"), (FPlatformTime::Seconds() - Start) * 1e6 / Reads));
	TestEqual(TEXT("every read finds the same field"), Same, Reads);

	FStructureEditorUtils::RenameVariable(Struct.Get(), Vars.Last().VarGuid, TEXT("Top Field"));
	TestNull(TEXT("a renamed field is no longer found by its old name"), CrowdyExec::FindTravellingField(Struct.Get(), TEXT("Field19")));
	const FProperty* Renamed = CrowdyExec::FindTravellingField(Struct.Get(), TEXT("TopField"));
	TestTrue(TEXT("and is found by its new one, as the struct now holds it"), Renamed && Renamed == FindFProperty<FProperty>(Struct.Get(), Struct->FindPropertyNameFromGuid(Vars.Last().VarGuid)));
	return true;
}

#endif
