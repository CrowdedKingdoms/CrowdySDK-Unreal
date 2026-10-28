#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Subsystem/CrowdyAuthSessionSwitch.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthSessionSwitchNamesTheSessionFileTest,
	"CrowdySDK.Auth.SessionSwitchNamesTheSessionFile",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdyAuthSessionSwitchNamesTheSessionFileTest::RunTest(const FString& Parameters)
{
	bool bRefused = true;

	TestEqual(TEXT("no switch names no session"), CrowdySessionSuffixFromCommandLine(TEXT("-log -windowed"), &bRefused), FString());
	TestFalse(TEXT("an absent switch is not a refusal"), bRefused);

	TestEqual(TEXT("a plain name becomes the suffix"), CrowdySessionSuffixFromCommandLine(TEXT("-crowdysession=b"), &bRefused), FString(TEXT(".b")));
	TestFalse(TEXT("a valid name is not a refusal"), bRefused);

	TestEqual(TEXT("the switch is found among other arguments"),
		CrowdySessionSuffixFromCommandLine(TEXT("-foo -crowdysession=client_2 -bar"), &bRefused), FString(TEXT(".client_2")));

	TestEqual(TEXT("a 32 character name is accepted"),
		CrowdySessionSuffixFromCommandLine(TEXT("-crowdysession=abcdefghijklmnopqrstuvwxyz012345"), &bRefused),
		FString(TEXT(".abcdefghijklmnopqrstuvwxyz012345")));
	TestFalse(TEXT("the boundary name is not a refusal"), bRefused);

	// FString's == ignores case, so this one compares case-sensitively.
	TestTrue(TEXT("a name is lowercased so names differing only in case share one session"),
		CrowdySessionSuffixFromCommandLine(TEXT("-crowdysession=Client_B"), &bRefused).Equals(TEXT(".client_b"), ESearchCase::CaseSensitive));
	TestEqual(TEXT("a launcher's quotes around the whole argument are kept out of the name"),
		CrowdySessionSuffixFromCommandLine(TEXT("Game.exe \"-crowdysession=b\" -log"), &bRefused), FString(TEXT(".b")));
	TestFalse(TEXT("a quoted argument is not a refusal"), bRefused);
	TestEqual(TEXT("a quoted value is read without its quotes"),
		CrowdySessionSuffixFromCommandLine(TEXT("-crowdysession=\"b\""), &bRefused), FString(TEXT(".b")));
	TestFalse(TEXT("a quoted value is not a refusal"), bRefused);

	const TCHAR* const Refused[] =
	{
		TEXT("-crowdysession=\"b"),
		TEXT("-crowdysession="),
		TEXT("-crowdysession= -bar"),
		TEXT("-crowdysession=.."),
		TEXT("-crowdysession=a/b"),
		TEXT("-crowdysession=a\\b"),
		TEXT("-crowdysession=a.b"),
		TEXT("-crowdysession=\"a b\""),
		TEXT("-crowdysession=abcdefghijklmnopqrstuvwxyz0123456"),
	};
	for (const TCHAR* CommandLine : Refused)
	{
		bRefused = false;
		TestEqual(*FString::Printf(TEXT("%s gets the refused session, never the default one"), CommandLine),
			CrowdySessionSuffixFromCommandLine(CommandLine, &bRefused), FString(TEXT(".~invalid")));
		TestTrue(*FString::Printf(TEXT("%s is reported as refused"), CommandLine), bRefused);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
