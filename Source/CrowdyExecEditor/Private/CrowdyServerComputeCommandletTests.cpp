#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyServerComputeCommandlet.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

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
		TEXT("disable"), TEXT("enable"), TEXT("restart"), TEXT("call"), TEXT("grant"), TEXT("deposit"), TEXT("instances"), TEXT("logs"), TEXT("stats"), TEXT("batch"), TEXT("generate"), TEXT("whoami"), TEXT("exporttype"), TEXT("newtype"), TEXT("edittype")};
	for (const TCHAR* Name : Expected)
	{
		TestTrue(FString::Printf(TEXT("the commandlet accepts %s"), Name), Names.Contains(Name));
	}
	TestTrue(TEXT("Usage names -editoropen"), Usage.Contains(TEXT("-editoropen")));
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
