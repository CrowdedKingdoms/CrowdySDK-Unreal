#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyGameKitTypes.h"
#include "CrowdyServerComputeService.h"
#include "CrowdyServerObjectDefinition.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "SCrowdyServerComputePanel.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

namespace CrowdyServerComputePageTest
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	using CrowdyExecDeveloper::FInstance;

	CrowdyExecDeveloper::FVersion MakeVersion(int32 Number, bool bActive, const FDateTime& Now, double HoursOld, const FString& Manifest)
	{
		CrowdyExecDeveloper::FVersion Version;
		Version.Version = Number;
		Version.bActive = bActive;
		Version.CreatedAt = Now - FTimespan::FromHours(HoursOld);
		Version.ManifestJson = Manifest;
		return Version;
	}

	FString MakeManifest(const TCHAR* DigestA, const TCHAR* DigestB)
	{
		return FString::Printf(TEXT("{\"root\":\"root\",\"types\":{\"sbx_a\":{\"kind\":\"hub\",\"digest\":\"%s\"},\"sbx_b\":{\"kind\":\"hub\",\"digest\":\"%s\"}}}"), DigestA, DigestB);
	}

	CrowdyExecDeveloper::FInstance MakeInstance(const TCHAR* NodeType, const TCHAR* InstanceId, const TCHAR* Phase, double SecondsOld)
	{
		CrowdyExecDeveloper::FInstance Instance;
		Instance.NodeType = NodeType;
		Instance.InstanceId = InstanceId;
		Instance.Phase = Phase;
		Instance.SinceMs = SecondsOld * 1000.0;
		return Instance;
	}

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

	FInstance Orphan = Make(TEXT("sbx_a"), TEXT("k1"), 3, TEXT("running"));
	Orphan.bOrphaned = true;
	Types = CrowdyServerComputeMove::TypesOnPreviousVersion(Before, {Orphan}, Changed, Running);
	TestTrue(TEXT("an orphaned instance is not offered"), Types.IsEmpty() && Running == 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerComputeEarlierVersionTest, "CrowdySDK.CrowdyExecEditor.ServerComputeRestartAllNamesChangedTypesRunningFromBeforeTheActiveVersion", CrowdyServerComputePageTest::TestFlags)
bool FCrowdyServerComputeEarlierVersionTest::RunTest(const FString& Parameters)
{
	using CrowdyExecDeveloper::FInstance;
	using CrowdyExecDeveloper::FVersion;
	const FDateTime Now(2026, 10, 25, 12, 0, 0);
	const auto MakeVersion = [&Now](int32 Number, bool bActive, double HoursOld, const FString& Manifest)
	{
		FVersion Version;
		Version.Version = Number;
		Version.bActive = bActive;
		Version.CreatedAt = Now - FTimespan::FromHours(HoursOld);
		Version.ManifestJson = Manifest;
		return Version;
	};
	const auto MakeManifest = [](const TCHAR* DigestA, const TCHAR* DigestB)
	{
		return FString::Printf(TEXT("{\"root\":\"root\",\"types\":{\"sbx_a\":{\"kind\":\"hub\",\"digest\":\"%s\"},\"sbx_b\":{\"kind\":\"hub\",\"digest\":\"%s\"}}}"), DigestA, DigestB);
	};
	const auto MakeInstance = [](const TCHAR* NodeType, const TCHAR* Key, const TCHAR* Phase, double HoursInPhase)
	{
		FInstance Instance;
		Instance.NodeType = NodeType;
		Instance.Key = Key;
		Instance.Phase = Phase;
		Instance.SinceMs = HoursInPhase * 3600000.0;
		return Instance;
	};
	const TArray<FVersion> Versions = {MakeVersion(1, false, 5.0, MakeManifest(TEXT("a1"), TEXT("b1"))), MakeVersion(2, true, 1.0, MakeManifest(TEXT("a2"), TEXT("b1")))};
	const TArray<FInstance> Instances = {MakeInstance(TEXT("sbx_a"), TEXT("old"), TEXT("running"), 2.0), MakeInstance(TEXT("sbx_a"), TEXT("restarted"), TEXT("running"), 0.25),
		MakeInstance(TEXT("sbx_a"), TEXT("stopping"), TEXT("stopping"), 2.0), MakeInstance(TEXT("sbx_b"), TEXT("same"), TEXT("running"), 2.0)};

	TArray<FString> Types;
	int32 Running = -1;
	FString Problem;
	TestTrue(TEXT("two readable versions are compared"), CrowdyServerComputeMove::FindTypesOnEarlierVersion(Versions, Instances, Now, Types, Running, Problem));
	TestEqualSensitive(TEXT("only the changed type with an instance running from before the active version is named"), FString::Join(Types, TEXT(",")), FString(TEXT("sbx_a")));
	TestEqual(TEXT("and only that instance is counted: not the restarted one, the stopping one, or the unchanged type's"), Running, 1);

	const TArray<FVersion> WithNewType = {MakeVersion(1, false, 5.0, TEXT("{\"types\":{\"sbx_b\":{\"digest\":\"b1\"}}}")), MakeVersion(2, true, 1.0, MakeManifest(TEXT("a2"), TEXT("b1")))};
	CrowdyServerComputeMove::FindTypesOnEarlierVersion(WithNewType, {MakeInstance(TEXT("sbx_a"), TEXT("k"), TEXT("running"), 2.0)}, Now, Types, Running, Problem);
	TestEqualSensitive(TEXT("a type the previous version did not have counts as changed"), FString::Join(Types, TEXT(",")), FString(TEXT("sbx_a")));

	const TArray<FVersion> NoDigests = {MakeVersion(1, false, 5.0, TEXT("{\"types\":{\"sbx_b\":{\"kind\":\"hub\"}}}")), MakeVersion(2, true, 1.0, TEXT("{\"types\":{\"sbx_b\":{\"kind\":\"hub\"}}}"))};
	CrowdyServerComputeMove::FindTypesOnEarlierVersion(NoDigests, {MakeInstance(TEXT("sbx_b"), TEXT("k"), TEXT("running"), 2.0)}, Now, Types, Running, Problem);
	TestEqualSensitive(TEXT("a manifest with no digest says nothing of the code, so the type counts as changed"), FString::Join(Types, TEXT(",")), FString(TEXT("sbx_b")));

	TestTrue(TEXT("the first version has nothing before it"), CrowdyServerComputeMove::FindTypesOnEarlierVersion({Versions[1]}, Instances, Now, Types, Running, Problem) && Types.IsEmpty() && Running == 0);
	TestTrue(TEXT("nothing live has nothing to move"), CrowdyServerComputeMove::FindTypesOnEarlierVersion({Versions[0]}, Instances, Now, Types, Running, Problem) && Types.IsEmpty());

	FInstance Orphan = MakeInstance(TEXT("sbx_a"), TEXT("orphan"), TEXT("running"), 2.0);
	Orphan.bOrphaned = true;
	TestTrue(TEXT("an orphaned instance is not named"), CrowdyServerComputeMove::FindTypesOnEarlierVersion(Versions, {Orphan}, Now, Types, Running, Problem) && Types.IsEmpty() && Running == 0);

	const TArray<FVersion> Unreadable = {MakeVersion(1, false, 5.0, TEXT("not json")), Versions[1]};
	TestFalse(TEXT("a manifest that is not JSON cannot be compared"), CrowdyServerComputeMove::FindTypesOnEarlierVersion(Unreadable, Instances, Now, Types, Running, Problem));
	TestEqualSensitive(TEXT("and the problem names both versions"), Problem,
		FString(TEXT("Version 2 or 1 has a manifest or creation time that cannot be read, so which instances run an earlier version is not known")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerComputeRolledBackRestartTest, "CrowdySDK.CrowdyExecEditor.ServerComputeRestartAllRefusesAnAppRolledBack", CrowdyServerComputePageTest::TestFlags)
bool FCrowdyServerComputeRolledBackRestartTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerComputePageTest;
	using CrowdyExecDeveloper::FVersion;
	const FDateTime Now(2026, 10, 25, 12, 0, 0);
	const FString Manifest = MakeManifest(TEXT("a1"), TEXT("b1"));
	const TArray<FInstance> Instances = {MakeInstance(TEXT("sbx_a"), TEXT("i1"), TEXT("running"), 4 * 3600.0)};

	TArray<FString> Types = {TEXT("stale")};
	int32 Running = 7;
	FString Problem;
	const TArray<FVersion> RolledBack = {MakeVersion(1, false, Now, 5.0, Manifest), MakeVersion(2, true, Now, 3.0, Manifest), MakeVersion(3, false, Now, 1.0, Manifest)};
	TestFalse(TEXT("an active version that is not the newest cannot be compared"), CrowdyServerComputeMove::FindTypesOnEarlierVersion(RolledBack, Instances, Now, Types, Running, Problem));
	TestEqualSensitive(TEXT("and the problem says to restart each type"), Problem,
		FString(TEXT("The active version 2 is not the newest (3); the app was rolled back, so which instances run older code cannot be told. Restart each type with -type=T.")));
	TestTrue(TEXT("and names nothing"), Types.IsEmpty() && Running == 0);

	const TArray<FVersion> NotRolledBack = {MakeVersion(1, false, Now, 5.0, Manifest), MakeVersion(2, false, Now, 3.0, Manifest), MakeVersion(3, true, Now, 1.0, Manifest)};
	TestTrue(TEXT("the newest version active is compared as usual"), CrowdyServerComputeMove::FindTypesOnEarlierVersion(NotRolledBack, Instances, Now, Types, Running, Problem));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerComputeChainedDeployRestartTest, "CrowdySDK.CrowdyExecEditor.ServerComputeRestartAllComparesEachInstanceWithTheVersionItStartedOn", CrowdyServerComputePageTest::TestFlags)
bool FCrowdyServerComputeChainedDeployRestartTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerComputePageTest;
	using CrowdyExecDeveloper::FVersion;
	const FDateTime Now(2026, 10, 25, 12, 0, 0);
	const TArray<FVersion> Versions = {MakeVersion(1, false, Now, 10.0, MakeManifest(TEXT("a1"), TEXT("b1"))), MakeVersion(2, false, Now, 5.0, MakeManifest(TEXT("a2"), TEXT("b1"))),
		MakeVersion(3, true, Now, 1.0, MakeManifest(TEXT("a2"), TEXT("b1")))};
	const TArray<FInstance> Instances = {MakeInstance(TEXT("sbx_a"), TEXT("on_v1"), TEXT("running"), 7 * 3600.0), MakeInstance(TEXT("sbx_a"), TEXT("on_v2"), TEXT("running"), 3 * 3600.0),
		MakeInstance(TEXT("sbx_b"), TEXT("same_code"), TEXT("running"), 7 * 3600.0)};

	TArray<FString> Types;
	int32 Running = -1;
	FString Problem;
	TestTrue(TEXT("the versions are compared"), CrowdyServerComputeMove::FindTypesOnEarlierVersion(Versions, Instances, Now, Types, Running, Problem));
	TestEqualSensitive(TEXT("an instance two deploys back is named although the version before the active one has the same code"), FString::Join(Types, TEXT(",")), FString(TEXT("sbx_a")));
	TestEqual(TEXT("and only it is counted: not the instance on the version before, nor the type that never changed"), Running, 1);

	TestTrue(TEXT("an instance older than every known version is compared"), CrowdyServerComputeMove::FindTypesOnEarlierVersion(Versions, {MakeInstance(TEXT("sbx_b"), TEXT("unknown"), TEXT("running"), 20 * 3600.0)}, Now, Types, Running, Problem));
	TestTrue(TEXT("and counts as running other code, since its version is not known"), Types.Num() == 1 && Running == 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerComputeClockSkewRestartTest, "CrowdySDK.CrowdyExecEditor.ServerComputeRestartAllMovesInstancesStartedWithinAMinuteOfTheDeploy", CrowdyServerComputePageTest::TestFlags)
bool FCrowdyServerComputeClockSkewRestartTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerComputePageTest;
	using CrowdyExecDeveloper::FVersion;
	const FDateTime Now(2026, 10, 25, 12, 0, 0);
	const TArray<FVersion> Versions = {MakeVersion(1, false, Now, 5.0, MakeManifest(TEXT("a1"), TEXT("b1"))), MakeVersion(2, true, Now, 1.0, MakeManifest(TEXT("a2"), TEXT("b1")))};
	const double DeployAge = 3600.0;

	TArray<FString> Types;
	int32 Running = -1;
	FString Problem;
	CrowdyServerComputeMove::FindTypesOnEarlierVersion(Versions, {MakeInstance(TEXT("sbx_a"), TEXT("after_30s"), TEXT("running"), DeployAge - 30.0)}, Now, Types, Running, Problem);
	TestTrue(TEXT("an instance that started 30 seconds after the version was created may still run the code before it, so it is moved"), Types.Num() == 1 && Running == 1);

	CrowdyServerComputeMove::FindTypesOnEarlierVersion(Versions, {MakeInstance(TEXT("sbx_a"), TEXT("after_90s"), TEXT("running"), DeployAge - 90.0)}, Now, Types, Running, Problem);
	TestTrue(TEXT("one that started 90 seconds after is on the active version"), Types.IsEmpty() && Running == 0);

	TestEqualSensitive(TEXT("the step line says instances started near the deploy are moved as well"), CrowdyServerComputeMove::DescribeMove({TEXT("sbx_a"), TEXT("sbx_b")}, 3),
		FString(TEXT("moving 3 running instances of sbx_a, sbx_b to the active version; instances started within a minute of the deploy are moved as well")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerComputeRestartStopsEveryInstanceTest, "CrowdySDK.CrowdyExecEditor.ServerComputeRestartAllListsEveryInstanceItStops", CrowdyServerComputePageTest::TestFlags)
bool FCrowdyServerComputeRestartStopsEveryInstanceTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerComputePageTest;
	const TArray<FInstance> Instances = {MakeInstance(TEXT("sbx_a"), TEXT("i1"), TEXT("running"), 2 * 3600.0), MakeInstance(TEXT("sbx_a"), TEXT("i2"), TEXT("running"), 600.0),
		MakeInstance(TEXT("sbx_a"), TEXT("i3"), TEXT("stopping"), 90.0), MakeInstance(TEXT("sbx_b"), TEXT("i4"), TEXT("starting"), 30.0), MakeInstance(TEXT("sbx_c"), TEXT("i5"), TEXT("running"), 60.0)};

	const TArray<FInstance> Stopping = CrowdyServerComputeMove::FindInstancesToStop(Instances, {TEXT("sbx_a"), TEXT("sbx_b")});
	TestEqual(TEXT("every running or starting instance of the chosen types is stopped, those on the active version too, but not a stopping one or another type's"), Stopping.Num(), 3);
	TestEqualSensitive(TEXT("each is listed with its Instance Id, phase and age, under the count"), CrowdyServerComputeMove::DescribeInstancesToStop(Stopping),
		FString(TEXT("This stops 3 running instances, those already on the active version included:\nsbx_a i1: running, 2h old\nsbx_a i2: running, 10m old\nsbx_b i4: starting, 30s old")));
	TestEqualSensitive(TEXT("one instance is singular"), CrowdyServerComputeMove::DescribeInstancesToStop({Instances[4]}),
		FString(TEXT("This stops 1 running instance, those already on the active version included:\nsbx_c i5: running, 60s old")));

	FInstance Orphan = MakeInstance(TEXT("sbx_a"), TEXT("i6"), TEXT("running"), 60.0);
	Orphan.bOrphaned = true;
	TestTrue(TEXT("an orphaned instance, whose type the active version does not deploy, is never picked to stop"), CrowdyServerComputeMove::FindInstancesToStop({Orphan}, {TEXT("sbx_a")}).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerComputeFilterNamesTest,"CrowdySDK.CrowdyExecEditor.ServerComputeLogLevelAndActivityWindowNames", CrowdyServerComputePageTest::TestFlags)
bool FCrowdyServerComputeFilterNamesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerComputePage;
	struct FLevelRow { const TCHAR* Filter; int32 MaxLevel; };
	for (const FLevelRow& Row : {FLevelRow{TEXT("all"), -1}, FLevelRow{TEXT("info"), 2}, FLevelRow{TEXT("warnings"), 1}, FLevelRow{TEXT("errors"), 0}})
	{
		int32 MaxLevel = 99;
		TestTrue(FString::Printf(TEXT("%s is a Logs filter"), Row.Filter), FindLogMaxLevel(Row.Filter, MaxLevel));
		TestEqual(FString::Printf(TEXT("%s shows up to its level"), Row.Filter), MaxLevel, Row.MaxLevel);
	}
	int32 Unknown = 99;
	TestFalse(TEXT("a level the tab has no filter for is not one"), FindLogMaxLevel(TEXT("debug"), Unknown));
	TestEqual(TEXT("and sets no limit"), Unknown, -1);

	TestEqualSensitive(TEXT("hour is the tab's last hour"), FindActivityWindow(TEXT("hour")), FString(WindowHour));
	TestEqualSensitive(TEXT("day is the tab's last day"), FindActivityWindow(TEXT("day")), FString(WindowDay));
	TestEqualSensitive(TEXT("week is the tab's last week"), FindActivityWindow(TEXT("week")), FString(WindowWeek));
	TestTrue(TEXT("any other name is no window"), FindActivityWindow(TEXT("month")).IsEmpty());
	TestEqual(TEXT("the windows are minutes"), FCString::Atoi(WindowWeek), 7 * 24 * 60);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerComputeFinishRemovalTest, "CrowdySDK.CrowdyExecEditor.ServerComputeFinishRemovalDeletesOnlyTheFolderTheSdkGenerated", CrowdyServerComputePageTest::TestFlags)
bool FCrowdyServerComputeFinishRemovalTest::RunTest(const FString& Parameters)
{
	const CrowdyServerComputePageTest::FTempServer Temp;
	const FSoftObjectPath NotNamed(TEXT("/Game/CrowdyRemovalTest/DA_NotInAnySetting.DA_NotInAnySetting"));
	const FString Crate = Temp.MakeCrate(TEXT("sbx_counter"));
	CrowdyServerComputePage::FRemovalEffects Effects = CrowdyServerComputePage::FinishRemoval(FText::FromString(TEXT("sbx_counter")), TEXT("sbx_counter"), NotNamed, Crate, Temp.Server, false, TEXT("Server/sbx_counter"), true, false);
	TestFalse(TEXT("a folder the type owns is deleted"), IFileManager::Get().DirectoryExists(*Crate));
	TestTrue(TEXT("without a problem"), Effects.FolderProblem.IsEmpty());
	TestFalse(TEXT("a definition no setting names clears none"), Effects.bSettingsCleared);

	const FString Kept = Temp.MakeCrate(TEXT("sbx_kept"));
	Effects = CrowdyServerComputePage::FinishRemoval(FText::FromString(TEXT("sbx_kept")), TEXT("sbx_kept"), NotNamed, Kept, Temp.Server, false, TEXT("Server/sbx_kept"), false, false);
	TestTrue(TEXT("a folder the plan said stays is not deleted, though it would be allowed to be"), IFileManager::Get().DirectoryExists(*Kept));
	TestTrue(TEXT("and no problem is reported for it"), Effects.FolderProblem.IsEmpty());

	const FString HandWritten = Temp.MakeCrate(TEXT("sbx_arena"));
	FFileHelper::SaveStringToFile(TEXT("[package]\nname = \"sbx_arena\"\n"), *FPaths::Combine(HandWritten, TEXT("Cargo.toml")));
	FFileHelper::SaveStringToFile(TEXT("// my own hub\nuse ckx_sdk::prelude::*;\n"), *FPaths::Combine(HandWritten, TEXT("src/lib.rs")));
	Effects = CrowdyServerComputePage::FinishRemoval(FText::FromString(TEXT("sbx_arena")), TEXT("sbx_arena"), NotNamed, HandWritten, Temp.Server, false, TEXT("Server/sbx_arena"), true, false);
	TestTrue(TEXT("a folder the SDK did not generate is left"), IFileManager::Get().DirectoryExists(*HandWritten));
	TestEqualSensitive(TEXT("and the removal says so"), Effects.FolderProblem.ToString(),
		FString(TEXT("Removed sbx_arena's definition. Its Server Code folder holds server code the Crowdy SDK did not generate, so it is left as it is.")));

	const FString Shared = Temp.MakeCrate(TEXT("sbx_shared"));
	Effects = CrowdyServerComputePage::FinishRemoval(FText::FromString(TEXT("sbx_shared")), TEXT("sbx_shared"), NotNamed, Shared, Temp.Server, true, TEXT("Server/sbx_shared"), true, false);
	TestTrue(TEXT("a folder another definition shares is left"), IFileManager::Get().DirectoryExists(*Shared));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerComputeFinishRemovalSettingsTest, "CrowdySDK.CrowdyExecEditor.ServerComputeFinishRemovalClearsTheGameKitSettingsNamingTheAsset", CrowdyServerComputePageTest::TestFlags)
bool FCrowdyServerComputeFinishRemovalSettingsTest::RunTest(const FString& Parameters)
{
	UCrowdySDKDeveloperSettings* Settings = GetMutableDefault<UCrowdySDKDeveloperSettings>();
	const FSoftObjectPath SavedWorld = Settings->GameKitWorld;
	const TArray<FSoftObjectPath> SavedStarters = Settings->GameKitActorStarters;
	ON_SCOPE_EXIT
	{
		Settings->GameKitWorld = SavedWorld;
		Settings->GameKitActorStarters = SavedStarters;
	};

	const CrowdyServerComputePageTest::FTempServer Temp;
	const FSoftObjectPath Removed(TEXT("/Game/CrowdyRemovalTest/DA_Removed.DA_Removed"));
	const FSoftObjectPath Other(TEXT("/Game/CrowdyRemovalTest/DA_Other.DA_Other"));
	const FString Crate = Temp.MakeCrate(TEXT("sbx_named"));
	const FText Type = FText::FromString(TEXT("sbx_named"));

	Settings->GameKitWorld = Removed;
	Settings->GameKitActorStarters = {Other, Removed};
	CrowdyServerComputePage::FRemovalEffects Effects = CrowdyServerComputePage::FinishRemoval(Type, TEXT("sbx_named"), Removed, Crate, Temp.Server, false, TEXT("Server/sbx_named"), false, false);
	TestTrue(TEXT("a definition the settings name reports them cleared"), Effects.bSettingsCleared);
	TestTrue(TEXT("the Game Kit World no longer names it"), Settings->GameKitWorld.IsNull());
	TestFalse(TEXT("the Game Kit Actor Starters no longer name it"), Settings->GameKitActorStarters.Contains(Removed));
	TestTrue(TEXT("and still name the others"), Settings->GameKitActorStarters.Contains(Other));

	Settings->GameKitWorld = Other;
	Settings->GameKitActorStarters = {Other};
	Effects = CrowdyServerComputePage::FinishRemoval(Type, TEXT("sbx_named"), Removed, Crate, Temp.Server, false, TEXT("Server/sbx_named"), false, false);
	TestFalse(TEXT("settings naming only other definitions are not cleared"), Effects.bSettingsCleared);
	TestTrue(TEXT("and keep them"), Settings->GameKitWorld == Other && Settings->GameKitActorStarters.Contains(Other));

	Settings->GameKitWorld = Other;
	Settings->GameKitActorStarters = {Removed};
	Effects = CrowdyServerComputePage::FinishRemoval(Type, TEXT("sbx_named"), Removed, Crate, Temp.Server, false, TEXT("Server/sbx_named"), false, false);
	TestTrue(TEXT("a definition only the Actor Starters name reports them cleared"), Effects.bSettingsCleared && Settings->GameKitActorStarters.IsEmpty());
	TestTrue(TEXT("and leaves the world another definition names"), Settings->GameKitWorld == Other);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerComputeVisibleVariablesTest, "CrowdySDK.CrowdyExecEditor.ServerComputeStarterCountsItsWholeState", CrowdyServerComputePageTest::TestFlags)
bool FCrowdyServerComputeVisibleVariablesTest::RunTest(const FString& Parameters)
{
	UCrowdyServerObjectDefinition* Definition = NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage());
	Definition->State = FCrowdyKitQueueCounts::StaticStruct();
	Definition->WatchedFields = {TEXT("Waiting")};
	TestEqual(TEXT("an own type counts the variables ticked Visible to Players"), CrowdyServerComputePage::CountVisibleVariables(*Definition), 1);

	Definition->CodeSource = ECrowdyServerCodeSource::GameKitStarter;
	Definition->KitStateTopic = TEXT("queue");
	Definition->TypeName = TEXT("matchmaker");
	Definition->WatchedFields.Reset();
	TArray<FString> Errors;
	TestTrue(TEXT("the matchmaker bakes"), Definition->Bake(Errors));
	TestEqual(TEXT("a starter counts every variable of its State"), CrowdyServerComputePage::CountVisibleVariables(*Definition), 3);

	Definition->KitOwnerValues = {{TEXT("forming"), TEXT("forming"), TEXT("Forming")}};
	TestEqual(TEXT("but not an Owner Value, which only its own player reads"), CrowdyServerComputePage::CountVisibleVariables(*Definition), 2);

	UCrowdyServerObjectDefinition* World = NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage());
	World->State = FCrowdyKitWorldConditions::StaticStruct();
	World->CodeSource = ECrowdyServerCodeSource::GameKitStarter;
	World->KitStateTopic = TEXT("world");
	World->TypeName = TEXT("world");
	TestTrue(TEXT("the world bakes"), World->Bake(Errors));
	TestEqual(TEXT("bHasOnline rides with Online, so it is no variable of its own"), CrowdyServerComputePage::CountVisibleVariables(*World), 5);
	return true;
}

#endif
