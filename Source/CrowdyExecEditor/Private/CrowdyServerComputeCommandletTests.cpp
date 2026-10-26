#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyServerComputeCommandlet.h"
#include "CrowdyServerObjectDefinition.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerComputeCommandletBatchLinesTest, "CrowdySDK.CrowdyExecEditor.ServerComputeCommandlet.BatchLines",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCrowdyServerComputeCommandletBatchLinesTest::RunTest(const FString& Parameters)
{
	TArray<FString> Lines;
	FString Error;

	const FString Text = TEXT("# first the reads\r\n")
		TEXT("-op=status -out=\"C:\\My Game\\K1-status.json\"\r\n")
		TEXT("\r\n")
		TEXT("   \r\n")
		TEXT("  # indented comment\r\n")
		TEXT("  -op=call -type=leaderboard -fn=Submit -key=K -inputs=\"@f.json\" -yes -batchcontinue  \r\n")
		TEXT("-op=versions");
	TestTrue(TEXT("a file of operations, comments and blank lines parses"), UCrowdyServerComputeCommandlet::ParseBatchLines(Text, Lines, Error));
	TestTrue(TEXT("no error is reported"), Error.IsEmpty());
	TestEqual(TEXT("only the operation lines are kept"), Lines.Num(), 3);
	if (Lines.Num() == 3)
	{
		FString Out;
		TestTrue(TEXT("a quoted value with a space survives"), FParse::Value(*Lines[0], TEXT("-out="), Out) && Out == TEXT("C:\\My Game\\K1-status.json"));
		TestEqual(TEXT("a line is trimmed"), Lines[1], FString(TEXT("-op=call -type=leaderboard -fn=Submit -key=K -inputs=\"@f.json\" -yes -batchcontinue")));
		TestFalse(TEXT("a line without the token does not continue"), FParse::Param(*Lines[0], TEXT("batchcontinue")));
		TestTrue(TEXT("a line with the token continues"), FParse::Param(*Lines[1], TEXT("batchcontinue")));
		TestEqual(TEXT("the last line needs no newline"), Lines[2], FString(TEXT("-op=versions")));
	}

	TestFalse(TEXT("a nested batch is refused"), UCrowdyServerComputeCommandlet::ParseBatchLines(TEXT("-op=status\n-op=batch -ops=other.txt\n"), Lines, Error));
	TestTrue(TEXT("the refusal names the line"), Error.Contains(TEXT("line 2")));
	TestTrue(TEXT("a refused batch keeps no lines"), Lines.IsEmpty());

	TestFalse(TEXT("the nested check ignores case"), UCrowdyServerComputeCommandlet::ParseBatchLines(TEXT("-OP=Batch -ops=other.txt"), Lines, Error));

	TestFalse(TEXT("a file with only comments is refused"), UCrowdyServerComputeCommandlet::ParseBatchLines(TEXT("# nothing\n\n"), Lines, Error));
	TestFalse(TEXT("an empty file is refused"), UCrowdyServerComputeCommandlet::ParseBatchLines(FString(), Lines, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerComputeCommandletQuotedAtFileTest, "CrowdySDK.CrowdyExecEditor.ServerComputeCommandlet.ReadsAQuotedAtFileWithSpaces",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCrowdyServerComputeCommandletQuotedAtFileTest::RunTest(const FString& Parameters)
{
	const FString Path = FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir() / TEXT("My Game") / TEXT("inputs.json"));
	const FString Json = TEXT("{\"score\":7}");
	if (!TestTrue(TEXT("the temp file is written"), FFileHelper::SaveStringToFile(Json, *Path)))
	{
		return false;
	}

	const FString WindowsPath = Path.Replace(TEXT("/"), TEXT("\\"));
	FString Out;
	FString Error;
	TestTrue(TEXT("-inputs=@\"path with spaces and backslashes\" reads"), UCrowdyServerComputeCommandlet::ReadJsonSwitch(FString::Printf(TEXT("-op=call -inputs=@\"%s\" -yes"), *WindowsPath), TEXT("-inputs="), Out, Error));
	TestEqual(TEXT("the file's content is returned"), Out, Json);

	Out.Reset();
	TestTrue(TEXT("-inputs=\"@path with spaces and backslashes\" reads, FParse keeping its backslashes"), UCrowdyServerComputeCommandlet::ReadJsonSwitch(FString::Printf(TEXT("-op=call -inputs=\"@%s\" -yes"), *WindowsPath), TEXT("-inputs="), Out, Error));
	TestEqual(TEXT("the same content is returned"), Out, Json);

	Out.Reset();
	TestTrue(TEXT("inline JSON is passed through"), UCrowdyServerComputeCommandlet::ReadJsonSwitch(TEXT("-inputs={\"a\":1}"), TEXT("-inputs="), Out, Error));
	TestEqual(TEXT("the JSON is unchanged"), Out, FString(TEXT("{\"a\":1}")));

	Out.Reset();
	TestFalse(TEXT("a missing quoted file fails"), UCrowdyServerComputeCommandlet::ReadJsonSwitch(TEXT("-inputs=@\"C:\\No Such Dir\\none.json\""), TEXT("-inputs="), Out, Error));
	TestTrue(TEXT("the error names the whole path"), Error.Contains(TEXT("No Such Dir")) && Error.Contains(TEXT("none.json")));

	IFileManager::Get().Delete(*Path);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerComputeCommandletUsageTest, "CrowdySDK.CrowdyExecEditor.ServerComputeCommandlet.UsageNamesEveryOperation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCrowdyServerComputeCommandletUsageTest::RunTest(const FString& Parameters)
{
	const FString Usage = UCrowdyServerComputeCommandlet::GetUsage();
	const TArray<FString> Names = UCrowdyServerComputeCommandlet::GetOperationNames();
	for (const FString& Name : Names)
	{
		TestTrue(FString::Printf(TEXT("Usage names %s"), *Name), Usage.Contains(TEXT("-op=") + Name + TEXT(" ")) || Usage.Contains(TEXT("| ") + Name + TEXT(" ")));
	}
	const TCHAR* const Expected[] = {TEXT("status"), TEXT("versions"), TEXT("changes"), TEXT("deploy"), TEXT("starters"), TEXT("addstarter"), TEXT("removestarter"), TEXT("activate"),
		TEXT("disable"), TEXT("enable"), TEXT("restart"), TEXT("call"), TEXT("grant"), TEXT("deposit"), TEXT("instances"), TEXT("logs"), TEXT("stats"), TEXT("batch"), TEXT("generate"), TEXT("whoami"), TEXT("exporttype"), TEXT("newtype"), TEXT("edittype"), TEXT("removetype"), TEXT("renametype")};
	for (const TCHAR* Name : Expected)
	{
		TestTrue(FString::Printf(TEXT("the commandlet accepts %s"), Name), Names.Contains(Name));
	}
	TestTrue(TEXT("Usage names -editoropen"), Usage.Contains(TEXT("-editoropen")));
	for (const TCHAR* Switch : {TEXT("-level=all|info|warnings|errors"), TEXT("-before="), TEXT("-window=hour|day|week"), TEXT("restart [-type=T]"), TEXT("-to=/Game/Path/NewAssetName")})
	{
		TestTrue(FString::Printf(TEXT("Usage names %s"), Switch), Usage.Contains(Switch));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerComputeCommandletRemoveTypeTest, "CrowdySDK.CrowdyExecEditor.ServerComputeCommandlet.RemoveTypeRefusesAStarterAndAUsedDefinition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCrowdyServerComputeCommandletRemoveTypeTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Ordinary(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
	Ordinary->TypeName = TEXT("tip_jar");
	TestTrue(TEXT("an unused ordinary type is not refused"), UCrowdyServerComputeCommandlet::FindRemoveTypeProblem(*Ordinary, {}).IsEmpty());
	TestEqualSensitive(TEXT("a definition other assets use is refused, naming them"), UCrowdyServerComputeCommandlet::FindRemoveTypeProblem(*Ordinary, {FName(TEXT("/Game/Maps/Lobby")), FName(TEXT("/Game/BP_Jar"))}),
		FString(TEXT("tip_jar's definition /Engine/Transient is used by /Game/Maps/Lobby, /Game/BP_Jar; remove those uses first; nothing was removed")));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Starter(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
	Starter->TypeName = TEXT("player");
	Starter->CodeSource = ECrowdyServerCodeSource::GameKitStarter;
	TestEqualSensitive(TEXT("a Game Kit starter is sent to removestarter"), UCrowdyServerComputeCommandlet::FindRemoveTypeProblem(*Starter, {}),
		FString(TEXT("player is a Game Kit starter, and removing one takes the starters that need it with it; use -op=removestarter -starter=player")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerComputeCommandletRemoveGuardsTest, "CrowdySDK.CrowdyExecEditor.ServerComputeCommandlet.RemoveTypeStaysInsideGameAndChecksTheDeleteHappened",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCrowdyServerComputeCommandletRemoveGuardsTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("a package under /Game/ is a project type"), UCrowdyServerComputeCommandlet::FindNotInGameProblem(TEXT("/Game/ServerObjects/CSO_TipJar"), TEXT("removed")).IsEmpty());
	TestEqualSensitive(TEXT("a plugin's package cannot be removed"), UCrowdyServerComputeCommandlet::FindNotInGameProblem(TEXT("/CrowdySDK/Starters/CSO_Player"), TEXT("removed")),
		FString(TEXT("only a project type under /Game/ can be removed here")));
	TestEqualSensitive(TEXT("nor moved"), UCrowdyServerComputeCommandlet::FindNotInGameProblem(TEXT("/Engine/Transient"), TEXT("moved")), FString(TEXT("only a project type under /Game/ can be moved here")));
	TestTrue(TEXT("a name that only starts like /Game is not under it"), !UCrowdyServerComputeCommandlet::FindNotInGameProblem(TEXT("/GameExtras/CSO_TipJar"), TEXT("removed")).IsEmpty());

	const auto StillThere = [](const FString& Package) { return Package == TEXT("/Game/ServerObjects/CSO_TipJar"); };
	TestEqualSensitive(TEXT("a package still on disk after the delete stops the removal before anything else goes"), UCrowdyServerComputeCommandlet::FindNotDeletedProblem(TEXT("/Game/ServerObjects/CSO_TipJar"), StillThere),
		FString(TEXT("the definition could not be deleted; nothing else was removed")));
	TestTrue(TEXT("a package that is gone does not"), UCrowdyServerComputeCommandlet::FindNotDeletedProblem(TEXT("/Game/ServerObjects/CSO_Gone"), StillThere).IsEmpty());

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Ordinary(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
	Ordinary->TypeName = TEXT("tip_jar");
	TestEqualSensitive(TEXT("a redirector that something still uses asks for a fix up first"),
		UCrowdyServerComputeCommandlet::FindRemoveTypeProblem(*Ordinary, {FName(TEXT("/Game/Old/CSO_TipJar")), FName(TEXT("/Game/Maps/Lobby"))}, {FName(TEXT("/Game/Old/CSO_TipJar"))}),
		FString(TEXT("tip_jar's definition /Engine/Transient is used by /Game/Old/CSO_TipJar, /Game/Maps/Lobby; remove those uses first; the redirectors /Game/Old/CSO_TipJar point to it and are still used, so fix up redirectors in the Content Browser first; nothing was removed")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerComputeCommandletDescribeRemoveTest,"CrowdySDK.CrowdyExecEditor.ServerComputeCommandlet.RemoveTypeSaysWhatItWouldRemove",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCrowdyServerComputeCommandletDescribeRemoveTest::RunTest(const FString& Parameters)
{
	TestEqualSensitive(TEXT("a type with a folder the SDK generated loses both"), UCrowdyServerComputeCommandlet::DescribeRemoveType(TEXT("tip_jar"), TEXT("/Game/ServerObjects/CSO_TipJar"), TEXT("Server/tip_jar"), FString()),
		FString(TEXT("This removes tip_jar: its definition /Game/ServerObjects/CSO_TipJar and its Server Code folder Server/tip_jar")));
	TestEqualSensitive(TEXT("a folder the SDK did not generate stays, and the reason is given"), UCrowdyServerComputeCommandlet::DescribeRemoveType(TEXT("tip_jar"), TEXT("/Game/ServerObjects/CSO_TipJar"), TEXT("Server/tip_jar"), TEXT("it holds server code the Crowdy SDK did not generate")),
		FString(TEXT("This removes tip_jar: its definition /Game/ServerObjects/CSO_TipJar only; its Server Code folder Server/tip_jar is left as it is: it holds server code the Crowdy SDK did not generate")));
	TestEqualSensitive(TEXT("a type with no folder loses its definition"), UCrowdyServerComputeCommandlet::DescribeRemoveType(TEXT("tip_jar"), TEXT("/Game/ServerObjects/CSO_TipJar"), FString(), FString()),
		FString(TEXT("This removes tip_jar: its definition /Game/ServerObjects/CSO_TipJar (it has no Server Code folder)")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerComputeCommandletRenameTypeTest, "CrowdySDK.CrowdyExecEditor.ServerComputeCommandlet.RenameTypeRefusesATargetOutsideGameOrTaken",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCrowdyServerComputeCommandletRenameTypeTest::RunTest(const FString& Parameters)
{
	const FString Current = TEXT("/Game/ServerObjects/CSO_TipJar");
	const auto Taken = [](const FString& Package) { return Package == TEXT("/Game/ServerObjects/CSO_Taken"); };
	TestEqualSensitive(TEXT("a target outside /Game/ is refused"), UCrowdyServerComputeCommandlet::FindRenameProblem(TEXT("/Engine/Transient/CSO_TipJar"), Current, Taken),
		FString(TEXT("-to is \"/Engine/Transient/CSO_TipJar\"; the asset stays inside the project, so it must start with /Game/ and end with the new asset name, as /Game/ServerObjects/CSO_TipJar")));
	TestTrue(TEXT("a target that is no package name is refused"), UCrowdyServerComputeCommandlet::FindRenameProblem(TEXT("/Game/Server Objects/CSO TipJar"), Current, Taken).Contains(TEXT("is not a package an asset can have")));
	TestEqualSensitive(TEXT("the asset's own place is refused"), UCrowdyServerComputeCommandlet::FindRenameProblem(TEXT("/Game/ServerObjects/cso_tipjar"), Current, Taken),
		FString(TEXT("The definition is already at /Game/ServerObjects/CSO_TipJar")));
	TestEqualSensitive(TEXT("a package that exists is refused"), UCrowdyServerComputeCommandlet::FindRenameProblem(TEXT("/Game/ServerObjects/CSO_Taken"), Current, Taken),
		FString(TEXT("/Game/ServerObjects/CSO_Taken already exists; choose another -to")));
	TestTrue(TEXT("a free place under /Game/ is not refused"), UCrowdyServerComputeCommandlet::FindRenameProblem(TEXT("/Game/Types/CSO_Jar"), Current, Taken).IsEmpty());
	TestEqualSensitive(TEXT("without -yes it says what it would move, and that the Type Name stays"), UCrowdyServerComputeCommandlet::DescribeRenameType(TEXT("tip_jar"), Current, TEXT("/Game/Types/CSO_Jar")),
		FString(TEXT("This moves tip_jar's definition from /Game/ServerObjects/CSO_TipJar to /Game/Types/CSO_Jar; its Server Code folder follows, and its Type Name stays tip_jar")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerComputeCommandletLogsStatsSwitchesTest, "CrowdySDK.CrowdyExecEditor.ServerComputeCommandlet.LogsLevelAndStatsWindowTakeThePanelsNames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCrowdyServerComputeCommandletLogsStatsSwitchesTest::RunTest(const FString& Parameters)
{
	int32 MaxLevel = 99;
	FString Error;
	TestTrue(TEXT("no -level= sets no limit"), UCrowdyServerComputeCommandlet::ReadLogLevel(TEXT("-op=logs"), MaxLevel, Error) && MaxLevel == -1);
	TestTrue(TEXT("-level=errors shows errors only"), UCrowdyServerComputeCommandlet::ReadLogLevel(TEXT("-op=logs -level=errors"), MaxLevel, Error) && MaxLevel == 0);
	TestTrue(TEXT("-level=warnings shows warnings and errors"), UCrowdyServerComputeCommandlet::ReadLogLevel(TEXT("-op=logs -level=warnings"), MaxLevel, Error) && MaxLevel == 1);
	TestTrue(TEXT("-level=info shows info, warnings and errors"), UCrowdyServerComputeCommandlet::ReadLogLevel(TEXT("-op=logs -level=info"), MaxLevel, Error) && MaxLevel == 2);
	TestTrue(TEXT("-level=all sets no limit"), UCrowdyServerComputeCommandlet::ReadLogLevel(TEXT("-op=logs -level=all"), MaxLevel, Error) && MaxLevel == -1);
	TestFalse(TEXT("a level the tab has no filter for is refused"), UCrowdyServerComputeCommandlet::ReadLogLevel(TEXT("-op=logs -level=debug"), MaxLevel, Error));
	TestEqualSensitive(TEXT("saying which are"), Error, FString(TEXT("-level=debug is not one of all, info, warnings or errors, the Logs tab's filters")));

	int32 Minutes = 99;
	TestTrue(TEXT("no -window= leaves the platform's hour"), UCrowdyServerComputeCommandlet::ReadStatsWindow(TEXT("-op=stats"), Minutes, Error) && Minutes == 0);
	TestTrue(TEXT("-window=hour is 60 minutes"), UCrowdyServerComputeCommandlet::ReadStatsWindow(TEXT("-op=stats -window=hour"), Minutes, Error) && Minutes == 60);
	TestTrue(TEXT("-window=day is 1440 minutes"), UCrowdyServerComputeCommandlet::ReadStatsWindow(TEXT("-op=stats -window=day"), Minutes, Error) && Minutes == 1440);
	TestTrue(TEXT("-window=week is 10080 minutes"), UCrowdyServerComputeCommandlet::ReadStatsWindow(TEXT("-op=stats -window=week"), Minutes, Error) && Minutes == 10080);
	TestFalse(TEXT("a window the tab has no button for is refused"), UCrowdyServerComputeCommandlet::ReadStatsWindow(TEXT("-op=stats -window=month"), Minutes, Error));
	TestEqualSensitive(TEXT("saying which are"), Error, FString(TEXT("-window=month is not one of hour, day or week, the Activity tab's windows")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerComputeCommandletEditorOpenTest, "CrowdySDK.CrowdyExecEditor.ServerComputeCommandlet.WritingOpsRefuseWhileAnEditorIsOpen",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCrowdyServerComputeCommandletEditorOpenTest::RunTest(const FString& Parameters)
{
	const auto Running = []() { return true; };
	const auto Closed = []() { return false; };
	const FString Problem = UCrowdyServerComputeCommandlet::FindEditorOpenProblem(TEXT("-op=generate -yes"), Running);
	TestTrue(TEXT("an editor that is running refuses the op"), Problem.StartsWith(TEXT("An Unreal Editor is running. Close it first: it holds this project's Server Object types in memory")));
	TestTrue(TEXT("saying how to write anyway"), Problem.EndsWith(TEXT("Pass -editoropen to write anyway.")));
	TestTrue(TEXT("no editor running refuses nothing"), UCrowdyServerComputeCommandlet::FindEditorOpenProblem(TEXT("-op=generate -yes"), Closed).IsEmpty());
	TestTrue(TEXT("-editoropen writes anyway"), UCrowdyServerComputeCommandlet::FindEditorOpenProblem(TEXT("-op=generate -yes -editoropen"), Running).IsEmpty());
	TestTrue(TEXT("whatever its case"), UCrowdyServerComputeCommandlet::FindEditorOpenProblem(TEXT("-op=generate -EditorOpen"), Running).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerComputeCommandletScopeNameTest, "CrowdySDK.CrowdyExecEditor.ServerComputeCommandlet.WhoamiNamesTheSignInScope",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCrowdyServerComputeCommandletScopeNameTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("scope 1 is the developer's own session"), FString(UCrowdyServerComputeCommandlet::GetSignInScopeName(true, 1)), FString(TEXT("session")));
	TestEqual(TEXT("scope 2 is an organization token"), FString(UCrowdyServerComputeCommandlet::GetSignInScopeName(true, 2)), FString(TEXT("organizationToken")));
	TestEqual(TEXT("an old scope 0 is restored as an organization token"), FString(UCrowdyServerComputeCommandlet::GetSignInScopeName(true, 0)), FString(TEXT("organizationToken")));
	TestEqual(TEXT("not signed in has no scope"), FString(UCrowdyServerComputeCommandlet::GetSignInScopeName(false, 1)), FString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerComputeCommandletSwitchTypeTest, "CrowdySDK.CrowdyExecEditor.ServerComputeCommandlet.EnableTakesNoTypeButDisableNeedsOne",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCrowdyServerComputeCommandletSwitchTypeTest::RunTest(const FString& Parameters)
{
	FString Type;
	FString Error;
	TestTrue(TEXT("enable without -type switches the whole app on"), UCrowdyServerComputeCommandlet::ReadSwitchType(TEXT("-op=enable -yes"), true, Type, Error));
	TestTrue(TEXT("with no type"), Type.IsEmpty());
	TestTrue(TEXT("enable with -type names the type"), UCrowdyServerComputeCommandlet::ReadSwitchType(TEXT("-op=enable -type=boss -yes"), true, Type, Error) && Type == TEXT("boss"));
	TestFalse(TEXT("disable without -type is refused"), UCrowdyServerComputeCommandlet::ReadSwitchType(TEXT("-op=disable -yes"), false, Type, Error));
	TestTrue(TEXT("saying what it needs"), Error.Contains(TEXT("disable needs -type=T")));
	TestTrue(TEXT("disable with -type names the type"), UCrowdyServerComputeCommandlet::ReadSwitchType(TEXT("-op=disable -type=boss -yes"), false, Type, Error) && Type == TEXT("boss"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerComputeCommandletSignInProblemTest, "CrowdySDK.CrowdyExecEditor.ServerComputeCommandlet.WhoamiNamesWhatIsMissing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCrowdyServerComputeCommandletSignInProblemTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("signed in with an app is fine"), UCrowdyServerComputeCommandlet::FindSignInProblem(true, 42).IsEmpty());
	TestEqual(TEXT("without a sign-in"), UCrowdyServerComputeCommandlet::FindSignInProblem(false, 42), FString(TEXT("Sign in to Crowdy Studio on this machine")));
	TestEqual(TEXT("without an app"), UCrowdyServerComputeCommandlet::FindSignInProblem(true, 0), FString(TEXT("Choose the app in Crowdy Studio and press Sync")));
	TestEqual(TEXT("with neither, both are named"), UCrowdyServerComputeCommandlet::FindSignInProblem(false, 0),
		FString(TEXT("Sign in to Crowdy Studio on this machine; Choose the app in Crowdy Studio and press Sync")));
	return true;
}

#endif
