#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyServerComputeService.h"
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerComputeBadgeTest, "CrowdySDK.CrowdyExecEditor.ServerComputeBadgeBuildStateWinsThenLive", CrowdyServerComputePageTest::TestFlags)
bool FCrowdyServerComputeBadgeTest::RunTest(const FString& Parameters)
{
	using CrowdyExecDeploy::ECrateState;
	using CrowdyExecRevisions::EChange;
	using CrowdyStudioWidgets::EBadgeTone;
	const EChange Changes[] = {EChange::Unchanged, EChange::Changed, EChange::New, EChange::Unknown, EChange::Removed, EChange::NotCompared};
	auto Badge = [](ECrateState State, const CrowdyExecRevisions::FTypeChange* Change)
	{
		CrowdyExecDeploy::FTypeState Type;
		Type.State = State;
		Type.Problem = TEXT("the definition does not bake");
		return CrowdyServerComputePage::ChooseChangeBadge(Type, Change);
	};

	struct FBuildRow { ECrateState State; const TCHAR* Label; EBadgeTone Tone; };
	for (const FBuildRow& Row : {FBuildRow{ECrateState::NotGenerated, TEXT("Needs generating"), EBadgeTone::Warning},
		FBuildRow{ECrateState::OutOfDate, TEXT("Out of date"), EBadgeTone::Warning}, FBuildRow{ECrateState::Invalid, TEXT("Problem"), EBadgeTone::Danger}})
	{
		for (const EChange Change : Changes)
		{
			CrowdyExecRevisions::FTypeChange Live;
			Live.Change = Change;
			Live.LiveVersion = 4;
			const CrowdyServerComputePage::FChangeBadge Shown = Badge(Row.State, &Live);
			TestEqualSensitive(FString::Printf(TEXT("%s wins over live change %d"), Row.Label, static_cast<int32>(Change)), Shown.Label.ToString(), FString(Row.Label));
			TestTrue(FString::Printf(TEXT("%s keeps its tone over live change %d"), Row.Label, static_cast<int32>(Change)), Shown.Tone == Row.Tone);
		}
	}
	TestEqualSensitive(TEXT("a Problem shows why"), Badge(ECrateState::Invalid, nullptr).ToolTip.ToString(), FString(TEXT("the definition does not bake")));

	struct FLiveRow { EChange Change; const TCHAR* Label; EBadgeTone Tone; };
	for (const FLiveRow& Row : {FLiveRow{EChange::Unchanged, TEXT("No changes"), EBadgeTone::Success}, FLiveRow{EChange::Changed, TEXT("Changed"), EBadgeTone::Warning},
		FLiveRow{EChange::New, TEXT("New"), EBadgeTone::Info}, FLiveRow{EChange::Unknown, TEXT("Live code unknown"), EBadgeTone::Neutral},
		FLiveRow{EChange::Removed, TEXT("Removed"), EBadgeTone::Neutral}, FLiveRow{EChange::NotCompared, TEXT("Ready"), EBadgeTone::Neutral}})
	{
		CrowdyExecRevisions::FTypeChange Live;
		Live.Change = Row.Change;
		Live.LiveVersion = 4;
		const CrowdyServerComputePage::FChangeBadge Shown = Badge(ECrateState::UpToDate, &Live);
		TestEqualSensitive(FString::Printf(TEXT("a ready type compared as %d shows %s"), static_cast<int32>(Row.Change), Row.Label), Shown.Label.ToString(), FString(Row.Label));
		TestTrue(FString::Printf(TEXT("%s has its tone"), Row.Label), Shown.Tone == Row.Tone);
	}
	TestEqualSensitive(TEXT("a ready type not yet compared is Ready"), Badge(ECrateState::UpToDate, nullptr).Label.ToString(), FString(TEXT("Ready")));

	CrowdyExecRevisions::FTypeChange Changed;
	Changed.Change = EChange::Changed;
	Changed.LiveVersion = 4;
	Changed.What = {TEXT("server code"), TEXT("save interval")};
	TestTrue(TEXT("Changed names the live version and what differs"), Badge(ECrateState::UpToDate, &Changed).ToolTip.ToString().Contains(TEXT("server code, save interval")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerComputeRefreshTest, "CrowdySDK.CrowdyExecEditor.ServerComputeRefillsRowsOnlyWhenTheDataMoved", CrowdyServerComputePageTest::TestFlags)
bool FCrowdyServerComputeRefreshTest::RunTest(const FString& Parameters)
{
	using CrowdyServerComputePage::ChooseServiceRefresh;
	using CrowdyServerComputePage::EServiceRefresh;
	TestTrue(TEXT("deploy progress with the rows filled from the current data keeps them, so an open menu stays open"),
		ChooseServiceRefresh(true, false, 7, 7) == EServiceRefresh::KeepRows);
	TestTrue(TEXT("data newer than the rows refills them"), ChooseServiceRefresh(true, false, 8, 7) == EServiceRefresh::RefillRows);
	TestTrue(TEXT("a generation that wrapped past the rows' refills them"), ChooseServiceRefresh(true, false, 0, MAX_uint32) == EServiceRefresh::RefillRows);
	for (const bool bSameData : {true, false})
	{
		const uint32 Filled = bSameData ? 7 : 6;
		TestTrue(TEXT("a new client rebuilds the page"), ChooseServiceRefresh(true, true, 7, Filled) == EServiceRefresh::RebuildPage);
		TestTrue(TEXT("a page not built yet is built"), ChooseServiceRefresh(false, false, 7, Filled) == EServiceRefresh::RebuildPage);
	}
	return true;
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

	const FString HandWritten = Temp.MakeCrate(TEXT("sbx_arena"));
	FFileHelper::SaveStringToFile(TEXT("[package]\nname = \"sbx_arena\"\n"), *FPaths::Combine(HandWritten, TEXT("Cargo.toml")));
	FFileHelper::SaveStringToFile(TEXT("// my own hub\nuse ckx_sdk::prelude::*;\n"), *FPaths::Combine(HandWritten, TEXT("src/lib.rs")));
	TestTrue(TEXT("a folder of server code the SDK did not generate stays"),
		WhyCrateFolderStays(TEXT("sbx_arena"), HandWritten, Temp.Server, false).ToString().Contains(TEXT("did not generate")));
	FFileHelper::SaveStringToFile(TEXT("// The Server Object type sbx_arena's glue, generated by the Crowdy SDK from its definition. Regenerate it; do not edit.\n"),
		*FPaths::Combine(HandWritten, TEXT("src/lib.rs")));
	TestTrue(TEXT("the same folder once the SDK generated it may be deleted"), WhyCrateFolderStays(TEXT("sbx_arena"), HandWritten, Temp.Server, false).IsEmpty());

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerComputeMoveOfferTest, "CrowdySDK.CrowdyExecEditor.ServerComputeMoveOfferNamesInstancesPlacedBeforeTheDeploy", CrowdyServerComputePageTest::TestFlags)
bool FCrowdyServerComputeMoveOfferTest::RunTest(const FString& Parameters)
{
	using CrowdyExecDeveloper::FInstance;
	const auto Make = [](const TCHAR* NodeType, const TCHAR* Key, int64 Epoch, const TCHAR* Phase)
	{
		FInstance Instance;
		Instance.NodeType = NodeType;
		Instance.Key = Key;
		Instance.Epoch = Epoch;
		Instance.Phase = Phase;
		return Instance;
	};
	const TArray<FInstance> Before = {Make(TEXT("sbx_a"), TEXT("k1"), 3, TEXT("running")), Make(TEXT("sbx_a"), TEXT("k2"), 5, TEXT("running")),
		Make(TEXT("sbx_b"), TEXT("k1"), 2, TEXT("running")), Make(TEXT("sbx_c"), TEXT("k1"), 1, TEXT("running"))};
	const TArray<FString> Changed = {TEXT("sbx_a"), TEXT("sbx_c")};

	int32 Running = -1;
	TArray<FString> Types = CrowdyServerComputeMove::TypesOnPreviousVersion(Before, {Make(TEXT("sbx_a"), TEXT("k1"), 3, TEXT("running"))}, Changed, Running);
	TestEqualSensitive(TEXT("an instance with the same epoch as before the deploy runs the previous version"), FString::Join(Types, TEXT(",")), FString(TEXT("sbx_a")));
	TestEqual(TEXT("and is counted"), Running, 1);

	Types = CrowdyServerComputeMove::TypesOnPreviousVersion(Before, {Make(TEXT("sbx_a"), TEXT("k1"), 4, TEXT("running"))}, Changed, Running);
	TestTrue(TEXT("an instance placed again has a higher epoch and runs the new version"), Types.IsEmpty() && Running == 0);

	Types = CrowdyServerComputeMove::TypesOnPreviousVersion(Before, {Make(TEXT("sbx_a"), TEXT("k9"), 1, TEXT("running"))}, Changed, Running);
	TestTrue(TEXT("an instance that was not there before the deploy is not offered"), Types.IsEmpty() && Running == 0);

	Types = CrowdyServerComputeMove::TypesOnPreviousVersion(Before, {Make(TEXT("sbx_b"), TEXT("k1"), 2, TEXT("running"))}, Changed, Running);
	TestTrue(TEXT("a type the deploy did not change is not offered"), Types.IsEmpty() && Running == 0);

	Types = CrowdyServerComputeMove::TypesOnPreviousVersion(Before, {Make(TEXT("sbx_a"), TEXT("k1"), 3, TEXT("stopping"))}, Changed, Running);
	TestTrue(TEXT("an instance that is not running is not offered"), Types.IsEmpty() && Running == 0);

	Types = CrowdyServerComputeMove::TypesOnPreviousVersion(Before, {Make(TEXT("sbx_a"), TEXT("k1"), 3, TEXT("running")), Make(TEXT("sbx_a"), TEXT("k2"), 5, TEXT("running")),
		Make(TEXT("sbx_c"), TEXT("k1"), 1, TEXT("running")), Make(TEXT("sbx_a"), TEXT("k3"), 6, TEXT("running"))}, Changed, Running);
	TestEqualSensitive(TEXT("each type is named once, in the order its instances are listed"), FString::Join(Types, TEXT(",")), FString(TEXT("sbx_a,sbx_c")));
	TestEqual(TEXT("and every instance still on the previous version is counted"), Running, 3);

	Types = CrowdyServerComputeMove::TypesOnPreviousVersion({}, Before, Changed, Running);
	TestTrue(TEXT("with nothing known from before, nothing is offered"), Types.IsEmpty() && Running == 0);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerComputeInstancesPhraseTest, "CrowdySDK.CrowdyExecEditor.ServerComputeLibraryInstancesPhrase", CrowdyServerComputePageTest::TestFlags)
bool FCrowdyServerComputeInstancesPhraseTest::RunTest(const FString& Parameters)
{
	using CrowdyServerComputePage::InstancesPhrase;
	TestEqualSensitive(TEXT("a range of instances"), InstancesPhrase(1, 4).ToString(), FString(TEXT("1 to 4 instances")));
	TestEqualSensitive(TEXT("equal bounds name one count"), InstancesPhrase(4, 4).ToString(), FString(TEXT("4 instances")));
	TestEqualSensitive(TEXT("a single instance is singular"), InstancesPhrase(1, 1).ToString(), FString(TEXT("1 instance")));
	TestEqualSensitive(TEXT("a Max below Min reads as the Min"), InstancesPhrase(3, 2).ToString(), FString(TEXT("3 instances")));
	return true;
}

#endif
