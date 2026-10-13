#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyExecDeploy.h"
#include "CrowdyNativeExec.h"
#include "CrowdyServerObjectDefinition.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

namespace CrowdyExecPlatformDataTests
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	TStrongObjectPtr<UCrowdyServerObjectDefinition> MakeDefinition(const TCHAR* TypeName)
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
		Definition->TypeName = TypeName;
		return Definition;
	}

	FString ManifestOf(const UCrowdyServerObjectDefinition& Definition)
	{
		return CrowdyExecDeploy::MakeManifest({&Definition});
	}

	CrowdyExecDeveloper::FBuildCrate MakeCrate(const TCHAR* Logic)
	{
		CrowdyExecDeveloper::FBuildCrate Crate;
		Crate.Name = TEXT("builder");
		Crate.Files.Add(CrowdyExecDeveloper::FBuildFile{TEXT("Cargo.toml"), TEXT("[package]\nname = \"builder\"\n# ctx.world().set_voxels(\n")});
		Crate.Files.Add(CrowdyExecDeveloper::FBuildFile{TEXT("src/logic.rs"), Logic});
		return Crate;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecPlatformDataScopesTest, "CrowdySDK.CrowdyExecEditor.PlatformDataAsksForEachScope", CrowdyExecPlatformDataTests::TestFlags)
bool FCrowdyExecPlatformDataScopesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecPlatformDataTests;
	struct FCase
	{
		bool UCrowdyServerObjectDefinition::* Setting;
		const TCHAR* Scope;
	};
	const FCase Cases[] = {
		{&UCrowdyServerObjectDefinition::bReadsPlayers, TEXT("players.read")},
		{&UCrowdyServerObjectDefinition::bWritesPlayerState, TEXT("players.write")},
		{&UCrowdyServerObjectDefinition::bReadsWorld, TEXT("world.read")},
		{&UCrowdyServerObjectDefinition::bWritesWorld, TEXT("world.write")},
		{&UCrowdyServerObjectDefinition::bReadsGrids, TEXT("grids.read")},
		{&UCrowdyServerObjectDefinition::bManagesGrids, TEXT("grids.write")},
		{&UCrowdyServerObjectDefinition::bGrantsGridPermissions, TEXT("permissions.write")},
		{&UCrowdyServerObjectDefinition::bReadsAccess, TEXT("access.read")},
		{&UCrowdyServerObjectDefinition::bManagesAccess, TEXT("access.write")},
	};
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> All = MakeDefinition(TEXT("all"));
	for (const FCase& Case : Cases)
	{
		const TStrongObjectPtr<UCrowdyServerObjectDefinition> One = MakeDefinition(TEXT("one"));
		One.Get()->*Case.Setting = true;
		All.Get()->*Case.Setting = true;
		TestTrue(FString::Printf(TEXT("its setting alone asks for %s alone"), Case.Scope),
			ManifestOf(*One).Contains(FString::Printf(TEXT(",\"scopes\":[\"%s\"]}"), Case.Scope), ESearchCase::CaseSensitive));
	}
	TestTrue(TEXT("every setting on lists every scope once, in the platform's order"), ManifestOf(*All).Contains(
		TEXT(",\"scopes\":[\"players.read\",\"players.write\",\"world.read\",\"world.write\",\"grids.read\",\"grids.write\",\"permissions.write\",\"access.read\",\"access.write\"]}"),
		ESearchCase::CaseSensitive));
	TestFalse(TEXT("a type with none asks for no scope"), ManifestOf(*MakeDefinition(TEXT("none"))).Contains(TEXT("scopes")));

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Implied = MakeDefinition(TEXT("implied"));
	Implied->MembersFrom = ECrowdyServerMembersSource::CrowdyTeam;
	Implied->bReadsPlayers = true;
	TestTrue(TEXT("Reads Players on a Crowdy Team type lists players.read once"), ManifestOf(*Implied).Contains(TEXT(",\"scopes\":[\"players.read\"]}"), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecClosedToPlayersTest, "CrowdySDK.CrowdyExecEditor.TypeClosedToPlayersDeploysClientFalse", CrowdyExecPlatformDataTests::TestFlags)
bool FCrowdyExecClosedToPlayersTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecPlatformDataTests;
	const TCHAR* const Closed = TEXT("\"crate\":\"t\",\"client\":false,");
	const TCHAR* const Open = TEXT("\"crate\":\"t\",\"client\":true,");

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Bare = MakeDefinition(TEXT("t"));
	TestTrue(TEXT("a type with nothing for players is closed to them"), ManifestOf(*Bare).Contains(Closed, ESearchCase::CaseSensitive));
	Bare->Functions.AddDefaulted_GetRef().WhoCanCall = ECrowdyServerFunctionCaller::ServerOnly;
	TestTrue(TEXT("a Server Only function keeps it closed"), ManifestOf(*Bare).Contains(Closed, ESearchCase::CaseSensitive));
	Bare->Functions.AddDefaulted_GetRef().WhoCanCall = ECrowdyServerFunctionCaller::DevelopersOnly;
	TestTrue(TEXT("so does a Developers Only function"), ManifestOf(*Bare).Contains(Closed, ESearchCase::CaseSensitive));

	for (const ECrowdyServerFunctionCaller Caller : {ECrowdyServerFunctionCaller::Players, ECrowdyServerFunctionCaller::Members, ECrowdyServerFunctionCaller::Leader})
	{
		const TStrongObjectPtr<UCrowdyServerObjectDefinition> Callable = MakeDefinition(TEXT("t"));
		Callable->Functions.AddDefaulted_GetRef().WhoCanCall = ECrowdyServerFunctionCaller::ServerOnly;
		Callable->Functions.AddDefaulted_GetRef().WhoCanCall = Caller;
		TestTrue(FString::Printf(TEXT("a function players can call (caller %d) opens it"), static_cast<int32>(Caller)), ManifestOf(*Callable).Contains(Open, ESearchCase::CaseSensitive));
	}

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Watched = MakeDefinition(TEXT("t"));
	Watched->WatchedFields.Add(TEXT("Gold"));
	TestTrue(TEXT("a value visible to players opens it"), ManifestOf(*Watched).Contains(Open, ESearchCase::CaseSensitive));

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Records = MakeDefinition(TEXT("t"));
	Records->Holds = ECrowdyServerObjectHolds::RecordPerEntity;
	Records->Functions.AddDefaulted_GetRef().WhoCanCall = ECrowdyServerFunctionCaller::ServerOnly;
	TestTrue(TEXT("records open it with nothing visible, since each carries its entity, owner and team"), ManifestOf(*Records).Contains(Open, ESearchCase::CaseSensitive));

	for (const ECrowdyServerMembersSource Source : {ECrowdyServerMembersSource::ThisObject, ECrowdyServerMembersSource::CrowdyTeam})
	{
		const TStrongObjectPtr<UCrowdyServerObjectDefinition> Members = MakeDefinition(TEXT("t"));
		Members->MembersFrom = Source;
		TestTrue(FString::Printf(TEXT("members (from %d) open it"), static_cast<int32>(Source)), ManifestOf(*Members).Contains(Open, ESearchCase::CaseSensitive));
	}

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Joined = MakeDefinition(TEXT("t"));
	Joined->bOnPlayerJoined = true;
	TestTrue(TEXT("On Player Joined opens it, since a player is heard only by connecting"), ManifestOf(*Joined).Contains(Open, ESearchCase::CaseSensitive));
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Left = MakeDefinition(TEXT("t"));
	Left->bOnPlayerLeft = true;
	TestTrue(TEXT("so does On Player Left"), ManifestOf(*Left).Contains(Open, ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecScopeWarningsTest, "CrowdySDK.CrowdyExecEditor.ScopeWarningsNameUndeclaredCalls", CrowdyExecPlatformDataTests::TestFlags)
bool FCrowdyExecScopeWarningsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecPlatformDataTests;
	const TCHAR* const Writes = TEXT("fn place(ctx: &Ctx) -> Result<()> {\n    ctx.world()\n        .set_voxels(&writes)?;\n    Ok(())\n}\n");
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Builder = MakeDefinition(TEXT("builder"));
	TArray<FString> Warnings = CrowdyExecDeploy::FindScopeWarnings(*Builder, MakeCrate(Writes));
	if (TestEqual(TEXT("a call split across lines without its scope is one warning"), Warnings.Num(), 1))
	{
		TestEqualSensitive(TEXT("naming the call, the scope and the setting"), Warnings[0],
			FString(TEXT("builder: its server code calls world().set_voxels(), which needs world.write; turn on Changes the World or the platform refuses that call")));
	}
	Builder->bWritesWorld = true;
	TestEqual(TEXT("with Changes the World on there is none"), CrowdyExecDeploy::FindScopeWarnings(*Builder, MakeCrate(Writes)).Num(), 0);
	Builder->bWritesWorld = false;

	const TCHAR* const NotCode = TEXT("// ctx.world().set_voxels(&a)\n/* ctx.grids()\n   .get(1) */\nfn f() { let s = \"ctx.permissions().grant(\"; }\n");
	TestEqual(TEXT("calls in comments, block comments and strings are not code"), CrowdyExecDeploy::FindScopeWarnings(*Builder, MakeCrate(NotCode)).Num(), 0);

	const TCHAR* const Reads = TEXT("let p = ctx.players().get(id)?;\nlet f = ctx.players().features(id)?;\n");
	Warnings = CrowdyExecDeploy::FindScopeWarnings(*Builder, MakeCrate(Reads));
	TestEqual(TEXT("two calls needing one scope are one warning"), Warnings.Num(), 1);
	TestTrue(TEXT("naming Reads Players"), Warnings.Num() == 1 && Warnings[0].Contains(TEXT("turn on Reads Players"), ESearchCase::CaseSensitive));
	Builder->MembersFrom = ECrowdyServerMembersSource::CrowdyTeam;
	TestEqual(TEXT("players.read that Crowdy Team members ask for counts"), CrowdyExecDeploy::FindScopeWarnings(*Builder, MakeCrate(Reads)).Num(), 0);
	Builder->MembersFrom = ECrowdyServerMembersSource::None;

	const TCHAR* const Mixed = TEXT("ctx.grids().at(c, GridPick::Smallest)?;\nctx.permissions().revoke(g, p, None)?;\nctx.players().set_state(p, &b)?;\n");
	Warnings = CrowdyExecDeploy::FindScopeWarnings(*Builder, MakeCrate(Mixed));
	TestEqual(TEXT("one warning for each scope missing"), Warnings.Num(), 3);
	TestEqualSensitive(TEXT("in the order the calls are listed"), FString::JoinBy(Warnings, TEXT("|"), [](const FString& Warning)
	{
		FString Scope;
		Warning.Split(TEXT("which needs "), nullptr, &Scope);
		return Scope.Left(Scope.Find(TEXT(";")));
	}), FString(TEXT("players.write|grids.read|permissions.write")));

	const TCHAR* const Admin = TEXT("ctx.grids().create(low, high)?;\nctx.access().get(p)?;\nctx.access().suspend(p, 60)?;\n");
	Warnings = CrowdyExecDeploy::FindScopeWarnings(*Builder, MakeCrate(Admin));
	TestTrue(TEXT("grid and access management name their settings"), Warnings.Num() == 3
		&& Warnings[0].Contains(TEXT("needs grids.write; turn on Manages Grids"), ESearchCase::CaseSensitive)
		&& Warnings[1].Contains(TEXT("needs access.read; turn on Reads Players' Access"), ESearchCase::CaseSensitive)
		&& Warnings[2].Contains(TEXT("needs access.write; turn on Manages Players' Access"), ESearchCase::CaseSensitive));
	Builder->bManagesGrids = true;
	Builder->bReadsAccess = true;
	Builder->bManagesAccess = true;
	TestEqual(TEXT("and with them on there is none"), CrowdyExecDeploy::FindScopeWarnings(*Builder, MakeCrate(Admin)).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecScopeWarningsCfgTestTest, "CrowdySDK.CrowdyExecEditor.ScopeWarningsSkipTestOnlyCode", CrowdyExecPlatformDataTests::TestFlags)
bool FCrowdyExecScopeWarningsCfgTestTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecPlatformDataTests;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Builder = MakeDefinition(TEXT("builder"));
	const FString InTests = TEXT("#[cfg(test)]\nmod tests { fn t(ctx:&Ctx){ ctx.world().set_voxels(&w); } }\n");
	TestEqual(TEXT("a call inside a cfg(test) module is not counted"), CrowdyExecDeploy::FindScopeWarnings(*Builder, MakeCrate(*InTests)).Num(), 0);

	const FString Outside = TEXT("fn place(ctx: &Ctx) { ctx.world().set_voxels(&w); }\n");
	TestEqual(TEXT("the same call outside it still warns"), CrowdyExecDeploy::FindScopeWarnings(*Builder, MakeCrate(*(InTests + Outside))).Num(), 1);
	TestEqual(TEXT("and before it"), CrowdyExecDeploy::FindScopeWarnings(*Builder, MakeCrate(*(Outside + InTests))).Num(), 1);

	const FString Attributed = TEXT("#[cfg(test)]\n#[allow(dead_code)]\nuse crate::helper;\nfn place(ctx: &Ctx) { ctx.world().set_voxels(&w); }\n");
	TestEqual(TEXT("a cfg(test) item ending in a semicolon takes only itself"), CrowdyExecDeploy::FindScopeWarnings(*Builder, MakeCrate(*Attributed)).Num(), 1);

	const FString Nested = TEXT("#[cfg(test)]\n#[allow(unused)]\nmod tests { mod inner { fn a() {} } fn t(ctx:&Ctx){ ctx.grids().create(a, b); } }\n");
	TestEqual(TEXT("nested braces in a cfg(test) module are matched"), CrowdyExecDeploy::FindScopeWarnings(*Builder, MakeCrate(*Nested)).Num(), 0);

	const FString Field = TEXT("struct S { #[cfg(test)] dbg: u32, real: u32 }\nfn place(ctx: &Ctx) { ctx.world().set_voxels(&w); }\n");
	TestEqual(TEXT("a cfg(test) field takes only itself"), CrowdyExecDeploy::FindScopeWarnings(*Builder, MakeCrate(*Field)).Num(), 1);
	const FString LastField = TEXT("struct S { real: u32, #[cfg(test)] dbg: u32 }\nfn place(ctx: &Ctx) { ctx.world().set_voxels(&w); }\n");
	TestEqual(TEXT("and so does a last field with no comma"), CrowdyExecDeploy::FindScopeWarnings(*Builder, MakeCrate(*LastField)).Num(), 1);
	const FString Arm = TEXT("fn pick(ctx: &Ctx, k: u8) { match k { #[cfg(test)] 0 => 1, _ => { ctx.world().set_voxels(&w); 2 } }; }\n");
	TestEqual(TEXT("and a cfg(test) match arm"), CrowdyExecDeploy::FindScopeWarnings(*Builder, MakeCrate(*Arm)).Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecScopeWarningsRawStringTest, "CrowdySDK.CrowdyExecEditor.ScopeWarningsSkipRawStrings", CrowdyExecPlatformDataTests::TestFlags)
bool FCrowdyExecScopeWarningsRawStringTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecPlatformDataTests;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Builder = MakeDefinition(TEXT("builder"));
	const TCHAR* const Raw = TEXT("let s = r#\"ctx.world().set_voxels(x) \" ctx\"#;\n");
	TestEqual(TEXT("a call inside a raw string is not code"), CrowdyExecDeploy::FindScopeWarnings(*Builder, MakeCrate(Raw)).Num(), 0);
	// A backslash escapes nothing in a raw string, so it closes at its second quote and the call after it is code.
	const TCHAR* const RawThenCall = TEXT("let t = r\"a\\\"; ctx.world().set_voxels(&w)?;\n");
	TestEqual(TEXT("a call after a raw string ending in a backslash is code"), CrowdyExecDeploy::FindScopeWarnings(*Builder, MakeCrate(RawThenCall)).Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecServerLimitsTest, "CrowdySDK.CrowdyExecEditor.ServerLimitsSentOnlyWhenChanged", CrowdyExecPlatformDataTests::TestFlags)
bool FCrowdyExecServerLimitsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecPlatformDataTests;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Arena = MakeDefinition(TEXT("arena"));
	const FString Defaults = ManifestOf(*Arena);
	for (const TCHAR* Field : {TEXT("memory_mb"), TEXT("fuel_per_call"), TEXT("mailbox"), TEXT("deadline_ms")})
	{
		TestFalse(FString::Printf(TEXT("a new type leaves %s to the platform"), Field), Defaults.Contains(Field));
	}

	Arena->MemoryMegabytes = 128;
	const FString MemoryOnly = ManifestOf(*Arena);
	TestTrue(TEXT("a changed limit is sent"), MemoryOnly.Contains(TEXT("\"memory_mb\":128")));
	TestFalse(TEXT("and the others still are not"), MemoryOnly.Contains(TEXT("deadline_ms")) || MemoryOnly.Contains(TEXT("fuel_per_call")) || MemoryOnly.Contains(TEXT("mailbox")));

	Arena->CallTimeLimitMs = 5000;
	Arena->FuelPerCallMillions = 200;
	Arena->MaxWaitingCalls = 16;
	const FString Changed = ManifestOf(*Arena);
	TestTrue(TEXT("memory"), Changed.Contains(TEXT("\"memory_mb\":128")));
	TestTrue(TEXT("fuel in fuel units"), Changed.Contains(TEXT("\"fuel_per_call\":200000000")));
	TestTrue(TEXT("calls waiting"), Changed.Contains(TEXT("\"mailbox\":16")));
	TestTrue(TEXT("time per call"), Changed.Contains(TEXT("\"deadline_ms\":5000")));

	Arena->MemoryMegabytes = 4096;
	Arena->CallTimeLimitMs = 1;
	const FString Clamped = ManifestOf(*Arena);
	TestTrue(TEXT("memory is brought into the platform's range"), Clamped.Contains(TEXT("\"memory_mb\":512")));
	TestTrue(TEXT("and so is time per call"), Clamped.Contains(TEXT("\"deadline_ms\":10")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCallTimeoutTest, "CrowdySDK.CrowdyExecEditor.ClientOutwaitsTheLongestServerCall", CrowdyExecPlatformDataTests::TestFlags)
bool FCrowdyExecCallTimeoutTest::RunTest(const FString& Parameters)
{
	const FProperty* TimePerCall = UCrowdyServerObjectDefinition::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UCrowdyServerObjectDefinition, CallTimeLimitMs));
	if (!TestNotNull(TEXT("Time per Call"), TimePerCall))
	{
		return false;
	}
	const int32 Longest = FCString::Atoi(*TimePerCall->GetMetaData(TEXT("ClampMax")));
	TestTrue(TEXT("the longest a server lets a call run is known"), Longest > 0);
	TestTrue(TEXT("a player's connection waits longer than that for an answer"), FCrowdyNativeExecOptions().CallTimeoutMs > Longest);
	return true;
}

#endif
