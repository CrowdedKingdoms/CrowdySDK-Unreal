#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyServerComputeCommandlet.h"
#include "Misc/AutomationTest.h"
#include "Misc/Parse.h"

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

#endif
