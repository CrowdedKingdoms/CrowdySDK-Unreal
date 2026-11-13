#include "Subsystem/CrowdyModScripts.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_METADATA

#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace CrowdyModScriptsTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

using namespace CrowdyModScriptsTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsUnavailableTest, "CrowdySDK.ModScripts.SurfaceRunsNothing", Flags)
bool FCrowdyModScriptsUnavailableTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyModScripts> Scripts(NewObject<UCrowdyModScripts>(GetTransientPackage()));
	TestFalse(TEXT("this version cannot run Mod Scripts"), Scripts->IsAvailable());

	Scripts->EnterGrid(1, 2, 3);
	Scripts->AnswerPrompt(1, true);
	Scripts->Revoke(TEXT("mod"));
	Scripts->ForgetAuthor(7);
	TestEqual(TEXT("entering a chunk finds no grid"), Scripts->GetGrid().GridId, static_cast<int64>(0));
	TestEqual(TEXT("and runs nothing"), Scripts->GetRunning().Num(), 0);
	Scripts->LeaveGrid();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsShapeDefaultsTest, "CrowdySDK.ModScripts.ShapeDefaults", Flags)
bool FCrowdyModScriptsShapeDefaultsTest::RunTest(const FString& Parameters)
{
	const FCrowdyModScriptShape Shape;
	TestEqual(TEXT("a shape is a box"), Shape.Kind, ECrowdyModScriptShapeKind::Box);
	TestEqual(TEXT("one voxel on each axis"), Shape.Size, FVector::OneVector);
	TestTrue(TEXT("unrotated"), Shape.Rotation.Equals(FQuat::Identity));
	TestEqual(TEXT("white"), Shape.Color, FLinearColor::White);
	TestEqual(TEXT("opaque"), Shape.Opacity, 1.f);
	TestTrue(TEXT("and visible"), Shape.bVisible && !Shape.bUnlit);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsHostIsImplementableTest, "CrowdySDK.ModScripts.HostIsImplementable", Flags)
bool FCrowdyModScriptsHostIsImplementableTest::RunTest(const FString& Parameters)
{
	const UClass* Host = UCrowdyModScriptHost::StaticClass();
	TestTrue(TEXT("a Blueprint can implement the host"), Host->GetBoolMetaData(TEXT("IsBlueprintBase")));
	const TCHAR* Methods[] = {
		TEXT("DrainClicks"), TEXT("ReadChunk"), TEXT("ListPlayers"), TEXT("SetVoxel"),
		TEXT("ShowHud"), TEXT("DrawOverlay"), TEXT("ClearScript")};
	for (const TCHAR* Name : Methods)
	{
		const UFunction* Method = Host->FindFunctionByName(Name);
		TestTrue(FString::Printf(TEXT("%s is a Blueprint-implementable host call"), Name),
			Method && Method->HasAnyFunctionFlags(FUNC_BlueprintEvent));
	}
	const UFunction* SetVoxel = Host->FindFunctionByName(TEXT("SetVoxel"));
	TestTrue(TEXT("SetVoxel names the script that wrote"),
		SetVoxel && SetVoxel->FindPropertyByName(TEXT("Script")) && SetVoxel->FindPropertyByName(TEXT("Write"))
		&& SetVoxel->FindPropertyByName(TEXT("Refusal")));
	return true;
}

#endif
