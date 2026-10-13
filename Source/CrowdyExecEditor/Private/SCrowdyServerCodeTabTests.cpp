#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyServerCodeFiles.h"
#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectLibrary.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SCrowdyServerCodeTab.h"
#include "StructUtils/PropertyBag.h"
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCrowdyServerCodeTabRemovedStubsTest, "CrowdySDK.CrowdyExecEditor.ServerCodeTabNamesStubsAnotherProgramRemoved", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSCrowdyServerCodeTabRemovedStubsTest::RunTest(const FString& Parameters)
{
	using namespace SCrowdyServerCodeTabTests;
	if (!TestTrue(TEXT("Slate is running, so a Server Code tab can be made"), FSlateApplication::IsInitialized()))
	{
		return false;
	}
	const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), Relative));
	const FString Folder = FPaths::GetPath(Path);
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
	Definition->TypeName = TEXT("ck_exec_stubs");
	Definition->StateForm = ECrowdyServerValuesForm::List;
	Definition->StateList.AddProperty(TEXT("Gold"), EPropertyBagPropertyType::Int32);
	Definition->Functions.AddDefaulted_GetRef().Name = TEXT("Store");
	Definition->Functions.AddDefaulted_GetRef().Name = TEXT("Clear");
	Definition->CodeSource = ECrowdyServerCodeSource::OwnFile;
	Definition->LogicFile.FilePath = Relative;
	TArray<FString> Errors;
	CrowdyExecCodegen::FGeneratedCrate Crate;
	FString Error;
	if (!TestTrue(TEXT("the definition bakes and generates"), Definition->Bake(Errors) && CrowdyExecCodegen::Generate(*Definition, Crate, Error)))
	{
		return false;
	}
	FString WithClear = TEXT("impl Functions for ") + Crate.StateName + TEXT(" {\n");
	FString WithoutClear = WithClear;
	for (const CrowdyExecCodegen::FLogicStub& Stub : Crate.Stubs)
	{
		WithClear += Stub.Text;
		WithoutClear += Stub.Method == TEXT("clear") ? FString() : Stub.Text;
	}
	WithClear += TEXT("}\n");
	WithoutClear += TEXT("}\n");
	WriteOnDisk(Path, *WithClear, 1);

	const TSharedRef<SCrowdyServerCodeTab> Tab = MakeTab(*Definition);
	TestTrue(TEXT("nothing is recorded, so nothing is said"), Tab->GetRemovedStubsText().IsEmpty());
	CrowdyServerCodeEdits::NoteStubsInserted(Path, {TEXT("clear")});
	WriteOnDisk(Path, *WithClear, 2);
	Tab->FollowDisk();
	TestTrue(TEXT("a save that keeps the function says nothing"), Tab->GetRemovedStubsText().IsEmpty());
	WriteOnDisk(Path, *WithoutClear, 3);
	Tab->FollowDisk();
	TestEqualSensitive(TEXT("a save without it names it"), Tab->GetRemovedStubsText().ToString(), FString(TEXT("follow_logic.rs was saved by another program without the functions Generate added: clear.")));
	WriteOnDisk(Path, *WithoutClear, 4);
	Tab->FollowDisk();
	TestFalse(TEXT("another save still without it keeps saying so"), Tab->GetRemovedStubsText().IsEmpty());
	WriteOnDisk(Path, *WithClear, 5);
	Tab->FollowDisk();
	TestTrue(TEXT("a save with it back ends the notice"), Tab->GetRemovedStubsText().IsEmpty());
	TestTrue(TEXT("and forgets what Generate added"), CrowdyServerCodeEdits::InsertedStubs(Path).IsEmpty());

	CrowdyServerCodeEdits::NoteStubsInserted(Path, {TEXT("clear")});
	CrowdyServerCodeEdits::Kept().Put(Path, CrowdyServerCodeFiles::FEdits{WithClear + TEXT("// mine\n"), CrowdyServerCodeFiles::Read(Path)});
	const TSharedRef<SCrowdyServerCodeTab> Edited = MakeTab(*Definition);
	WriteOnDisk(Path, *WithoutClear, 6);
	Edited->FollowDisk();
	TestTrue(TEXT("with unsaved edits the change is reported"), Edited->HasChangedOnDisk());
	TestTrue(TEXT("and the removed function named"), Edited->GetRemovedStubsText().ToString().Contains(TEXT("clear")));

	CrowdyServerCodeEdits::ForgetInsertedStubs(Path);
	CrowdyServerCodeEdits::Kept().Forget(Path);
	IFileManager::Get().DeleteDirectory(*Folder, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCrowdyServerCodeTabRootTest, "CrowdySDK.CrowdyExecEditor.ServerCodeTabRefusesGenerateForTheSdkRoot", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSCrowdyServerCodeTabRootTest::RunTest(const FString& Parameters)
{
	using namespace SCrowdyServerCodeTabTests;
	if (!TestTrue(TEXT("Slate is running, so a Server Code tab can be made"), FSlateApplication::IsInitialized()))
	{
		return false;
	}
	UCrowdyServerObjectDefinition* SdkRoot = UCrowdyServerObjectLibrary::GetPlayersOnlineDefinition();
	if (!TestNotNull(TEXT("the SDK's root definition loads"), SdkRoot))
	{
		return false;
	}
	const TSharedRef<SCrowdyServerCodeTab> Tab = MakeTab(*SdkRoot);
	TestFalse(TEXT("Generate is off for the SDK's own root"), Tab->CanGenerate());
	TestTrue(TEXT("with the reason given"), Tab->GetWriteBlockReason().ToString().Contains(TEXT("own root")));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Named(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
	Named->TypeName = UCrowdyServerObjectDefinition::RootTypeName;
	const TSharedRef<SCrowdyServerCodeTab> NamedTab = MakeTab(*Named);
	TestFalse(TEXT("and for a type of the project named root"), NamedTab->CanGenerate());
	TestTrue(TEXT("which is told to pick another name"), NamedTab->GetWriteBlockReason().ToString().Contains(TEXT("choose another Type Name")));
	return true;
}

#endif
