// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "UI/GameModel/CrowdyModelCodeLink.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyModelCodeLinkTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

// Which of the two shapes a declaring path has decides where the Open control goes: an asset editor, or a header in
// the project's source. Getting this wrong disables a control that should work, which looks exactly like a row that
// nothing declares.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelCodeLinkNativePathTest,
	"CrowdySDK.CrowdyStudio.ModelCodeLinkNativePathNamesNoAsset", CrowdyModelCodeLinkTestFlags)

bool FCrowdyModelCodeLinkNativePathTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("a module path is a native class"),
		CrowdyModelCodeLink::IsNativeClassPath(TEXT("/Script/CrowdedKingdoms.AHero")));
	TestFalse(TEXT("a package path is not a native class"),
		CrowdyModelCodeLink::IsNativeClassPath(TEXT("/Game/Models/BP_Hero.BP_Hero_C")));

	// A native class is compiled into a module, so there is no package anywhere holding it.
	TestTrue(TEXT("a native class path names no asset"),
		CrowdyModelCodeLink::AssetObjectPath(TEXT("/Script/CrowdedKingdoms.AHero")).IsEmpty());

	// Nothing declares this row, so nothing can be opened for it.
	TestTrue(TEXT("an empty path names no asset"),
		CrowdyModelCodeLink::AssetObjectPath(FString()).IsEmpty());
	TestTrue(TEXT("a whitespace path names no asset"),
		CrowdyModelCodeLink::AssetObjectPath(TEXT("   ")).IsEmpty());
	TestFalse(TEXT("an empty path opens nothing"), CrowdyModelCodeLink::CanOpen(FString()));

	return true;
}

// A Blueprint class path names the generated class inside the asset. The asset is what an editor opens, and it is the
// same path with the generated class's suffix taken off.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelCodeLinkBlueprintPathTest,
	"CrowdySDK.CrowdyStudio.ModelCodeLinkBlueprintPathResolvesToItsAsset", CrowdyModelCodeLinkTestFlags)

bool FCrowdyModelCodeLinkBlueprintPathTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("a Blueprint class path drops its generated-class suffix"),
		CrowdyModelCodeLink::AssetObjectPath(TEXT("/Game/Models/BP_Hero.BP_Hero_C")),
		FString(TEXT("/Game/Models/BP_Hero.BP_Hero")));

	// An effect asset path is already an asset path and must survive untouched.
	TestEqual(TEXT("an asset path is left alone"),
		CrowdyModelCodeLink::AssetObjectPath(TEXT("/Game/Effects/E_Heal.E_Heal")),
		FString(TEXT("/Game/Effects/E_Heal.E_Heal")));

	// Surrounding whitespace comes from wherever the path was recorded and is not part of the name.
	TestEqual(TEXT("surrounding whitespace is not part of the path"),
		CrowdyModelCodeLink::AssetObjectPath(TEXT("  /Game/Effects/E_Heal.E_Heal  ")),
		FString(TEXT("/Game/Effects/E_Heal.E_Heal")));

	return true;
}

// The generated-class suffix is exactly "_C". FString::EndsWith folds case unless told not to, and an asset whose own
// name ends in "_c" is a different asset entirely: chopping that would ask the registry for a name nothing is saved
// under, and the Open control would go quiet for a row that has a perfectly good asset behind it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelCodeLinkSuffixCaseTest,
	"CrowdySDK.CrowdyStudio.ModelCodeLinkGeneratedClassSuffixIsCaseSensitive", CrowdyModelCodeLinkTestFlags)

bool FCrowdyModelCodeLinkSuffixCaseTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("a lowercase suffix is part of the asset name"),
		CrowdyModelCodeLink::AssetObjectPath(TEXT("/Game/Effects/E_Buff_c.E_Buff_c")),
		FString(TEXT("/Game/Effects/E_Buff_c.E_Buff_c")));

	TestEqual(TEXT("the generated-class suffix is still dropped"),
		CrowdyModelCodeLink::AssetObjectPath(TEXT("/Game/Models/BP_Buff_c.BP_Buff_c_C")),
		FString(TEXT("/Game/Models/BP_Buff_c.BP_Buff_c")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
