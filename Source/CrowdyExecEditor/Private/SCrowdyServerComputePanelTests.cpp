#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyServerObjectDefinition.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "SCrowdyServerComputePanel.h"

namespace CrowdyServerComputePageTest
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	struct FTempServer
	{
		FString Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("CrowdyServerComputePageTests"), FGuid::NewGuid().ToString()));
		FString Server = FPaths::Combine(Root, TEXT("Server"));

		~FTempServer()
		{
			IFileManager::Get().DeleteDirectory(*Root, false, true);
		}

		FString MakeCrate(const FString& Name) const
		{
			const FString Crate = FPaths::Combine(Server, Name);
			FFileHelper::SaveStringToFile(TEXT("fn f() {}\n"), *FPaths::Combine(Crate, TEXT("src/logic.rs")));
			return Crate;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerComputeRemoveFolderTest, "CrowdySDK.CrowdyExecEditor.ServerComputeRemoveKeepsOtherFolders", CrowdyServerComputePageTest::TestFlags)
bool FCrowdyServerComputeRemoveFolderTest::RunTest(const FString& Parameters)
{
	using CrowdyServerComputePage::WhyCrateFolderStays;
	const CrowdyServerComputePageTest::FTempServer Temp;
	const FString Crate = Temp.MakeCrate(TEXT("sbx_counter"));

	TestTrue(TEXT("a type's own folder may be deleted"), WhyCrateFolderStays(TEXT("sbx_counter"), Crate, Temp.Server, false).IsEmpty());
	TestFalse(TEXT("a folder another definition shares stays"), WhyCrateFolderStays(TEXT("sbx_counter"), Crate, Temp.Server, true).IsEmpty());
	TestFalse(TEXT("a name that differs only in case stays, since it lands on the other type's folder"),
		WhyCrateFolderStays(TEXT("Sbx_Counter"), FPaths::Combine(Temp.Server, TEXT("Sbx_Counter")), Temp.Server, false).IsEmpty());
	TestFalse(TEXT("a name holding a path stays, since it lands inside another type's folder"),
		WhyCrateFolderStays(TEXT("sbx_counter/src"), FPaths::Combine(Crate, TEXT("src")), Temp.Server, false).IsEmpty());

	const FString Nested = FPaths::Combine(Temp.Server, TEXT("nested"), TEXT("sbx_other"));
	IFileManager::Get().MakeDirectory(*Nested, true);
	TestFalse(TEXT("a folder not directly inside Server stays"), WhyCrateFolderStays(TEXT("sbx_other"), Nested, Temp.Server, false).IsEmpty());
	TestFalse(TEXT("an empty folder path stays"), WhyCrateFolderStays(TEXT("sbx_counter"), FString(), Temp.Server, false).IsEmpty());
	TestFalse(TEXT("a folder that does not exist has nothing to delete"),
		WhyCrateFolderStays(TEXT("sbx_missing"), FPaths::Combine(Temp.Server, TEXT("sbx_missing")), Temp.Server, false).IsEmpty());

#if PLATFORM_WINDOWS
	const FString Target = FPaths::Combine(Temp.Root, TEXT("Elsewhere"));
	IFileManager::Get().MakeDirectory(*Target, true);
	const FString Linked = Temp.MakeCrate(TEXT("sbx_linked"));
	const FString Junction = FPaths::Combine(Linked, TEXT("shared"));
	int32 ReturnCode = -1;
	FString JunctionPath = Junction;
	FString TargetPath = Target;
	FPaths::MakePlatformFilename(JunctionPath);
	FPaths::MakePlatformFilename(TargetPath);
	const FString Command = FString::Printf(TEXT("/c mklink /J \"%s\" \"%s\""), *JunctionPath, *TargetPath);
	FPlatformProcess::ExecProcess(TEXT("cmd.exe"), *Command, &ReturnCode, nullptr, nullptr);
	if (ReturnCode != 0 || !IFileManager::Get().IsSymlink(*Junction))
	{
		AddWarning(TEXT("could not make a junction here, so the link case was not checked"));
		return true;
	}
	TestFalse(TEXT("a folder holding a link stays, since deleting it could delete what the link points to"), WhyCrateFolderStays(TEXT("sbx_linked"), Linked, Temp.Server, false).IsEmpty());
	IFileManager::Get().DeleteDirectory(*Junction, false, false);
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerComputeReadableByTest, "CrowdySDK.CrowdyExecEditor.ServerComputeReadableByPhrase", CrowdyServerComputePageTest::TestFlags)
bool FCrowdyServerComputeReadableByTest::RunTest(const FString& Parameters)
{
	using CrowdyServerComputePage::ReadableByPhrase;
	TestTrue(TEXT("Every Player is readable by every player"),
		ReadableByPhrase(ECrowdyServerObjectVisibility::Public).ToString().Equals(TEXT("readable by every player"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("Owner Only is readable only by its owner"),
		ReadableByPhrase(ECrowdyServerObjectVisibility::OwnerOnly).ToString().Equals(TEXT("only its owner can read it"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("Members is readable by its members, not by every player"),
		ReadableByPhrase(ECrowdyServerObjectVisibility::Members).ToString().Equals(TEXT("readable by its members"), ESearchCase::CaseSensitive));
	return true;
}

#endif
