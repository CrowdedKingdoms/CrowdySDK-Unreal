#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyServerCodeFiles.h"
#include "CrowdyServerObjectDefinition.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SCrowdyServerCodeTab.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace SCrowdyServerCodeTabTests
{
	const TCHAR* const Relative = TEXT("Saved/CrowdyTests/ServerCodeTab/follow_logic.rs");

	/** Writes the file and gives it a time stamp of its own, so each write is a change on disk however fast the test runs. */
	void WriteOnDisk(const FString& Path, const TCHAR* Text, int32 Day)
	{
		FFileHelper::SaveStringToFile(FString(Text), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		IFileManager::Get().SetTimeStamp(*Path, FDateTime(2026, 1, Day));
	}

	TSharedRef<SCrowdyServerCodeTab> MakeTab(UCrowdyServerObjectDefinition& Definition)
	{
		return SNew(SCrowdyServerCodeTab).Definition(&Definition);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCrowdyServerCodeTabFollowsDiskTest, "CrowdySDK.CrowdyExecEditor.ServerCodeTabFollowsTheDisk", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSCrowdyServerCodeTabFollowsDiskTest::RunTest(const FString& Parameters)
{
	using namespace SCrowdyServerCodeTabTests;
	if (!TestTrue(TEXT("Slate is running, so a Server Code tab can be made"), FSlateApplication::IsInitialized()))
	{
		return false;
	}
	const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), Relative));
	const FString Folder = FPaths::GetPath(Path);
	WriteOnDisk(Path, TEXT("fn first() {}\n"), 1);
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
	Definition->TypeName = TEXT("ck_exec_follow");
	Definition->CodeSource = ECrowdyServerCodeSource::OwnFile;
	Definition->LogicFile.FilePath = Relative;

	const TSharedRef<SCrowdyServerCodeTab> Clean = MakeTab(*Definition);
	TestEqualSensitive(TEXT("the tab shows the file"), Clean->GetEditedText(), FString(TEXT("fn first() {}\n")));
	Clean->FollowDisk();
	TestFalse(TEXT("an unchanged file is no change"), Clean->HasChangedOnDisk());
	WriteOnDisk(Path, TEXT("fn second() {}\n"), 2);
	Clean->FollowDisk();
	TestEqualSensitive(TEXT("a save in another editor shows at once"), Clean->GetEditedText(), FString(TEXT("fn second() {}\n")));
	TestTrue(TEXT("as the file, not as unsaved changes"), Clean->GetUnsavedFile().IsEmpty() && !Clean->HasChangedOnDisk());

	CrowdyServerCodeEdits::Kept().Put(Path, CrowdyServerCodeFiles::FEdits{TEXT("fn mine() {}\n"), CrowdyServerCodeFiles::Read(Path)});
	const TSharedRef<SCrowdyServerCodeTab> Edited = MakeTab(*Definition);
	TestEqualSensitive(TEXT("a tab with unsaved edits shows them"), Edited->GetEditedText(), FString(TEXT("fn mine() {}\n")));
	WriteOnDisk(Path, TEXT("fn third() {}\n"), 3);
	Edited->FollowDisk();
	TestEqualSensitive(TEXT("a change on disk does not replace unsaved edits"), Edited->GetEditedText(), FString(TEXT("fn mine() {}\n")));
	TestTrue(TEXT("it is reported instead"), Edited->HasChangedOnDisk());
	Edited->FollowDisk();
	TestTrue(TEXT("and stays reported until answered"), Edited->HasChangedOnDisk());

	Edited->ForgetEditsUnder(Folder);
	TestEqualSensitive(TEXT("dropping the edits shows the file from disk"), Edited->GetEditedText(), FString(TEXT("fn third() {}\n")));
	TestFalse(TEXT("with nothing left to report"), Edited->HasChangedOnDisk());
	IFileManager::Get().DeleteDirectory(*Folder, false, true);
	return true;
}

#endif
