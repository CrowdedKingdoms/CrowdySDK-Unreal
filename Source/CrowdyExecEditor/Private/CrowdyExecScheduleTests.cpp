#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyExecCodegen.h"
#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectDefinitionCustomization.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

namespace CrowdyExecScheduleTests
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	/** A definition that bakes: the boss test state, and Heal. The test structs are private to CrowdyExec, so they are found by path. */
	TStrongObjectPtr<UCrowdyServerObjectDefinition> MakeDefinition(FAutomationTestBase& Test)
	{
		UScriptStruct* State = FindObject<UScriptStruct>(nullptr, TEXT("/Script/CrowdyExec.CrowdyExecTestBossState"));
		if (!State)
		{
			Test.AddError(TEXT("the test struct CrowdyExecTestBossState is not loaded"));
			return TStrongObjectPtr<UCrowdyServerObjectDefinition>();
		}
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
		Definition->TypeName = TEXT("ck_exec_schedule");
		Definition->Visibility = ECrowdyServerObjectVisibility::Public;
		Definition->WatchedFields = {TEXT("Health")};
		Definition->State = State;
		FCrowdyServerFunction& Heal = Definition->Functions.AddDefaulted_GetRef();
		Heal.Name = TEXT("Heal");
		Heal.WhoCanCall = ECrowdyServerFunctionCaller::ServerOnly;
		return Definition;
	}

	FCrowdyServerTimer& AddSchedule(UCrowdyServerObjectDefinition& Definition, const TCHAR* Name, const TCHAR* Cron, int32 UtcOffsetMinutes = 0)
	{
		FCrowdyServerTimer& Timer = Definition.Timers.AddDefaulted_GetRef();
		Timer.Name = Name;
		Timer.Repeat = ECrowdyServerTimerRepeat::Schedule;
		Timer.Cron = Cron;
		Timer.UtcOffsetMinutes = UtcOffsetMinutes;
		return Timer;
	}

	FCrowdyServerTimer& AddPlainTimer(UCrowdyServerObjectDefinition& Definition, const TCHAR* Name, ECrowdyServerTimerRepeat Repeat, float Seconds)
	{
		FCrowdyServerTimer& Timer = Definition.Timers.AddDefaulted_GetRef();
		Timer.Name = Name;
		Timer.Repeat = Repeat;
		Timer.Seconds = Seconds;
		return Timer;
	}

	TArray<FString> BakeErrors(UCrowdyServerObjectDefinition& Definition)
	{
		TArray<FString> Errors;
		Definition.Bake(Errors);
		return Errors;
	}

	/** Bakes and generates, then reads src/lib.rs and src/logic.rs. */
	bool Generate(FAutomationTestBase& Test, UCrowdyServerObjectDefinition& Definition, FString& OutLib, FString& OutLogic)
	{
		TArray<FString> Errors;
		if (!Definition.Bake(Errors))
		{
			Test.AddError(FString::Printf(TEXT("the test definition does not bake (%s)"), *FString::Join(Errors, TEXT("; "))));
			return false;
		}
		CrowdyExecCodegen::FGeneratedCrate Crate;
		FString Error;
		if (!CrowdyExecCodegen::Generate(Definition, Crate, Error))
		{
			Test.AddError(FString::Printf(TEXT("the test definition does not generate (%s)"), *Error));
			return false;
		}
		for (const CrowdyExecCodegen::FGeneratedFile& File : Crate.Files)
		{
			if (File.Path.Equals(TEXT("src/lib.rs"), ESearchCase::CaseSensitive))
			{
				OutLib = File.Text;
			}
			if (File.Path.Equals(TEXT("src/logic.rs"), ESearchCase::CaseSensitive))
			{
				OutLogic = File.Text;
			}
		}
		return Test.TestFalse(TEXT("the crate has a lib.rs"), OutLib.IsEmpty());
	}

	bool Has(const FString& Text, const TCHAR* Part)
	{
		return Text.Contains(Part, ESearchCase::CaseSensitive);
	}

	/** Where Part first appears at or after Start, or INDEX_NONE. */
	int32 Find(const FString& Text, const TCHAR* Part, int32 Start)
	{
		return Text.Find(Part, ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
	}

	/** Compared case-sensitively, since FString's == ignores case. */
	void TestRefuses(FAutomationTestBase& Test, UCrowdyServerObjectDefinition& Definition, const FString& Expected)
	{
		const TArray<FString> Errors = BakeErrors(Definition);
		const bool bFound = Errors.ContainsByPredicate([&Expected](const FString& Error) { return Error.Equals(Expected, ESearchCase::CaseSensitive); });
		Test.TestTrue(FString::Printf(TEXT("refused with \"%s\" (got: %s)"), *Expected, *FString::Join(Errors, TEXT("; "))), bFound);
	}

	void TestBakes(FAutomationTestBase& Test, UCrowdyServerObjectDefinition& Definition, const TCHAR* What)
	{
		const TArray<FString> Errors = BakeErrors(Definition);
		Test.TestTrue(FString::Printf(TEXT("%s bakes (%s)"), What, *FString::Join(Errors, TEXT("; "))), Errors.IsEmpty());
	}

	FString Named(const TCHAR* Name)
	{
		return FName(Name).ToString();
	}

	FString NotASchedule(const TCHAR* Name, const TCHAR* Cron)
	{
		return FString::Printf(TEXT("Timers: %s: Cron '%s' is not a schedule; use five fields (minute hour day-of-month month day-of-week), or six with seconds first"), *Named(Name), Cron);
	}

	/** A schedule of its own that must be refused for the one field Label, which must be Range. */
	void TestFieldRefused(FAutomationTestBase& Test, const TCHAR* Cron, const TCHAR* Label, const TCHAR* Range)
	{
		const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(Test);
		if (Definition)
		{
			AddSchedule(*Definition, TEXT("Restock"), Cron);
			TestRefuses(Test, *Definition, FString::Printf(TEXT("Timers: %s: Cron '%s': the %s must be %s"), *Named(TEXT("Restock")), Cron, Label, Range));
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecScheduleArmTest, "CrowdySDK.CrowdyExecEditor.ScheduleArmsWithTimerCron", CrowdyExecScheduleTests::TestFlags)
bool FCrowdyExecScheduleArmTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecScheduleTests;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(*this);
	if (!Definition)
	{
		return false;
	}
	AddSchedule(*Definition, TEXT("Restock"), TEXT("  0 */6 * * *  "));
	FString Lib;
	FString Logic;
	if (!Generate(*this, *Definition, Lib, Logic))
	{
		return false;
	}
	TestTrue(TEXT("a schedule is armed from its trimmed expression"), Has(Lib, TEXT("\"Restock\" => ctx.timer_cron(\"Restock\", &ckx_sdk::cron::Cron::parse(\"0 */6 * * *\")?),\n")));
	TestFalse(TEXT("an offset of 0 emits no with_utc_offset"), Has(Lib, TEXT("with_utc_offset")));
	TestFalse(TEXT("a schedule is not armed as an Every or Once After timer"), Has(Lib, TEXT("ctx.timer_every(\"Restock\"")) || Has(Lib, TEXT("ctx.timer_after(\"Restock\"")));
	TestTrue(TEXT("start and stop know it by name"), Has(Lib, TEXT("matches!(name, \"Restock\")")));
	TestTrue(TEXT("a timer that starts automatically is armed when the instance starts"), Has(Lib, TEXT("        timers::arm(ctx, \"Restock\")?;\n")));
	TestTrue(TEXT("the trait asks for the same function as any timer"), Has(Lib, TEXT("    fn restock(&mut self, ctx: &Ctx) -> Result<()>;\n")));
	TestTrue(TEXT("logic.rs gets the same stub as any timer"), Has(Logic, TEXT("    fn restock(&mut self, ctx: &Ctx) -> Result<()> {\n        Ok(())\n    }\n")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecScheduleOffsetTest, "CrowdySDK.CrowdyExecEditor.ScheduleOffsetShiftsTheCron", CrowdyExecScheduleTests::TestFlags)
bool FCrowdyExecScheduleOffsetTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecScheduleTests;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(*this);
	if (!Definition)
	{
		return false;
	}
	AddSchedule(*Definition, TEXT("Restock"), TEXT("@daily"), 300);
	AddSchedule(*Definition, TEXT("Reset"), TEXT("0 0 * * 1#2"), -90);
	FString Lib;
	FString Logic;
	if (!Generate(*this, *Definition, Lib, Logic))
	{
		return false;
	}
	TestTrue(TEXT("a positive offset shifts the parsed cron"), Has(Lib, TEXT("\"Restock\" => ctx.timer_cron(\"Restock\", &ckx_sdk::cron::Cron::parse(\"@daily\")?.with_utc_offset(300)?),\n")));
	TestTrue(TEXT("a west offset keeps its sign"), Has(Lib, TEXT("\"Reset\" => ctx.timer_cron(\"Reset\", &ckx_sdk::cron::Cron::parse(\"0 0 * * 1#2\")?.with_utc_offset(-90)?),\n")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecScheduleRearmTest, "CrowdySDK.CrowdyExecEditor.ScheduleRearmsBeforeTheUserFunction", CrowdyExecScheduleTests::TestFlags)
bool FCrowdyExecScheduleRearmTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecScheduleTests;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(*this);
	if (!Definition)
	{
		return false;
	}
	AddSchedule(*Definition, TEXT("Restock"), TEXT("0 */6 * * *"));
	AddPlainTimer(*Definition, TEXT("Tick"), ECrowdyServerTimerRepeat::Every, 5.f);
	FString Lib;
	FString Logic;
	if (!Generate(*this, *Definition, Lib, Logic))
	{
		return false;
	}
	const int32 Hook = Lib.Find(TEXT("    fn on_timer(&mut self, ctx: &Ctx, name: &str) -> Result<()> {\n"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (!TestTrue(TEXT("the hub has an on_timer"), Hook != INDEX_NONE))
	{
		return false;
	}
	const int32 Rearm = Find(Lib, TEXT("if let Err(error) = timers::arm(ctx, \"Restock\") {\n"), Hook);
	const int32 Logged = Find(Lib, TEXT("let _ = ctx.log(Level::Warn, &format!(\"timer {} could not be armed again: {error}\", \"Restock\"));\n"), Hook);
	const int32 Call = Find(Lib, TEXT("                Functions::restock(&mut self.state, ctx)\n            }\n"), Hook);
	TestTrue(TEXT("on_timer re-arms the schedule"), Rearm != INDEX_NONE);
	TestTrue(TEXT("a re-arm that fails is logged"), Logged != INDEX_NONE && Rearm < Logged);
	TestTrue(TEXT("on_timer hands finish the user's result alone"), Call != INDEX_NONE);
	TestFalse(TEXT("the re-arm's result is not folded into the user's"), Has(Lib, TEXT(".and(rearmed)")) || Has(Lib, TEXT("let rearmed")));
	TestTrue(TEXT("the schedule is re-armed before the user's function runs"), Rearm != INDEX_NONE && Call != INDEX_NONE && Rearm < Call);
	TestTrue(TEXT("an Every timer beside it still calls its function directly"), Has(Lib, TEXT("\"Tick\" => Functions::tick(&mut self.state, ctx),\n")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecScheduleValidationTest, "CrowdySDK.CrowdyExecEditor.ScheduleValidatesTheExpression", CrowdyExecScheduleTests::TestFlags)
bool FCrowdyExecScheduleValidationTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecScheduleTests;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Empty = MakeDefinition(*this);
	if (!Empty)
	{
		return false;
	}
	AddSchedule(*Empty, TEXT("Restock"), TEXT("   "));
	TestRefuses(*this, *Empty, FString::Printf(TEXT("Timers: %s: On Schedule needs a Cron expression, such as 0 */6 * * *"), *Named(TEXT("Restock"))));

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> TooFew = MakeDefinition(*this);
	AddSchedule(*TooFew, TEXT("Restock"), TEXT("0 */6 * *"));
	TestRefuses(*this, *TooFew, NotASchedule(TEXT("Restock"), TEXT("0 */6 * *")));

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> TooMany = MakeDefinition(*this);
	AddSchedule(*TooMany, TEXT("Restock"), TEXT("0 0 0 * * * *"));
	TestRefuses(*this, *TooMany, NotASchedule(TEXT("Restock"), TEXT("0 0 0 * * * *")));

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Strange = MakeDefinition(*this);
	AddSchedule(*Strange, TEXT("Restock"), TEXT("0 0 * * \"x\""));
	TestRefuses(*this, *Strange, NotASchedule(TEXT("Restock"), TEXT("0 0 * * \"x\"")));

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Macro = MakeDefinition(*this);
	AddSchedule(*Macro, TEXT("Restock"), TEXT("@sometimes"));
	TestRefuses(*this, *Macro, FString::Printf(TEXT("Timers: %s: Cron '@sometimes' is not a schedule; use @hourly, @daily, @weekly, @monthly or @yearly"), *Named(TEXT("Restock"))));

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Shifted = MakeDefinition(*this);
	AddSchedule(*Shifted, TEXT("Restock"), TEXT("0 */6 * * *"), 841);
	TestRefuses(*this, *Shifted, FString::Printf(TEXT("Timers: %s: UTC Offset must be from -840 to 840 minutes"), *Named(TEXT("Restock"))));

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> UpperMacro = MakeDefinition(*this);
	AddSchedule(*UpperMacro, TEXT("Restock"), TEXT("@DAILY"));
	TestRefuses(*this, *UpperMacro, FString::Printf(TEXT("Timers: %s: Cron '@DAILY' is not a schedule; use @hourly, @daily, @weekly, @monthly or @yearly"), *Named(TEXT("Restock"))));

	TestFieldRefused(*this, TEXT("0 25 * * *"), TEXT("hour"), TEXT("0 to 23"));
	TestFieldRefused(*this, TEXT("61 * * * *"), TEXT("minute"), TEXT("0 to 59"));
	TestFieldRefused(*this, TEXT("0 0 32 * *"), TEXT("day of the month"), TEXT("1 to 31, or L"));
	TestFieldRefused(*this, TEXT("0 0 0 * *"), TEXT("day of the month"), TEXT("1 to 31, or L"));
	TestFieldRefused(*this, TEXT("0 0 * 13 *"), TEXT("month"), TEXT("1 to 12 or JAN to DEC"));
	TestFieldRefused(*this, TEXT("0 0 * * 8"), TEXT("day of the week"), TEXT("0 to 7 or SUN to SAT, such as 5L or 1#2"));
	TestFieldRefused(*this, TEXT("60 0 0 * * *"), TEXT("seconds"), TEXT("0 to 59"));
	TestFieldRefused(*this, TEXT("MON * * * *"), TEXT("minute"), TEXT("0 to 59"));
	TestFieldRefused(*this, TEXT("L * * * *"), TEXT("minute"), TEXT("0 to 59"));
	TestFieldRefused(*this, TEXT("1#2 * * * *"), TEXT("minute"), TEXT("0 to 59"));
	TestFieldRefused(*this, TEXT("*/0 * * * *"), TEXT("minute"), TEXT("0 to 59"));
	TestFieldRefused(*this, TEXT("0 0 * JAN-X *"), TEXT("month"), TEXT("1 to 12 or JAN to DEC"));
	TestFieldRefused(*this, TEXT("0 0 5L * *"), TEXT("day of the month"), TEXT("1 to 31, or L"));
	TestFieldRefused(*this, TEXT("0 0 * * L"), TEXT("day of the week"), TEXT("0 to 7 or SUN to SAT, such as 5L or 1#2"));
	TestFieldRefused(*this, TEXT("0 0 * * 1#6"), TEXT("day of the week"), TEXT("0 to 7 or SUN to SAT, such as 5L or 1#2"));
	TestFieldRefused(*this, TEXT("0 0 * * 1#2/2"), TEXT("day of the week"), TEXT("0 to 7 or SUN to SAT, such as 5L or 1#2"));
	TestFieldRefused(*this, TEXT("0 0 ? * ?/2"), TEXT("day of the week"), TEXT("0 to 7 or SUN to SAT, such as 5L or 1#2"));
	TestFieldRefused(*this, TEXT("0,, * * * *"), TEXT("minute"), TEXT("0 to 59"));
	TestFieldRefused(*this, TEXT("0, * * * *"), TEXT("minute"), TEXT("0 to 59"));

	for (const TCHAR* Good : {TEXT("0 */6 * * *"), TEXT("@daily"), TEXT("30 0 */6 * * *"), TEXT("0 0 L * *"), TEXT("0 0 * * 1#2"), TEXT("0 9 * * MON-FRI"),
		TEXT("*/15 9-17 * * MON-FRI"), TEXT("0 0 * * 5L"), TEXT("0 0 12 1 JAN *"), TEXT("30 0 0 * * *"), TEXT("0,30 * ? * mon"), TEXT("0 0 1 JAN-MAR *"), TEXT("0 0 * * 7")})
	{
		const TStrongObjectPtr<UCrowdyServerObjectDefinition> Accepted = MakeDefinition(*this);
		AddSchedule(*Accepted, TEXT("Restock"), Good, -840);
		TestBakes(*this, *Accepted, Good);
	}

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> NoTime = MakeDefinition(*this);
	AddSchedule(*NoTime, TEXT("Restock"), TEXT("0 */6 * * *")).Seconds = 0.f;
	TestBakes(*this, *NoTime, TEXT("a schedule whose unused Seconds is 0"));

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> EveryIgnoresCron = MakeDefinition(*this);
	AddPlainTimer(*EveryIgnoresCron, TEXT("Tick"), ECrowdyServerTimerRepeat::Every, 5.f).Cron = TEXT("not a cron");
	TestBakes(*this, *EveryIgnoresCron, TEXT("an Every timer with a stray Cron"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecScheduleTitleTest, "CrowdySDK.CrowdyExecEditor.ScheduleTitleShowsTheCron", CrowdyExecScheduleTests::TestFlags)
bool FCrowdyExecScheduleTitleTest::RunTest(const FString& Parameters)
{
	FCrowdyServerTimer Restock;
	Restock.Name = TEXT("Restock");
	Restock.Repeat = ECrowdyServerTimerRepeat::Schedule;
	Restock.Cron = TEXT("  0 */6 * * *  ");
	Restock.Seconds = 60.f;
	TestEqual(TEXT("a schedule's title shows its trimmed cron, not its unused Seconds"), CrowdyServerObjectText::TimerTitle(Restock).ToString(), FString(TEXT("Restock: On Schedule 0 */6 * * *")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecScheduleLeavesTimersTest, "CrowdySDK.CrowdyExecEditor.ScheduleLeavesEveryAndOnceAfterUnchanged", CrowdyExecScheduleTests::TestFlags)
bool FCrowdyExecScheduleLeavesTimersTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecScheduleTests;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(*this);
	if (!Definition)
	{
		return false;
	}
	AddPlainTimer(*Definition, TEXT("Tick"), ECrowdyServerTimerRepeat::Every, 5.f);
	AddPlainTimer(*Definition, TEXT("EndMatch"), ECrowdyServerTimerRepeat::After, 10.f);
	FString Lib;
	FString Logic;
	if (!Generate(*this, *Definition, Lib, Logic))
	{
		return false;
	}
	TestTrue(TEXT("Every arms with timer_every"), Has(Lib, TEXT("\"Tick\" => ctx.timer_every(\"Tick\", 5000),\n")));
	TestTrue(TEXT("Once After arms with timer_after"), Has(Lib, TEXT("\"EndMatch\" => ctx.timer_after(\"EndMatch\", 10000),\n")));
	TestTrue(TEXT("on_timer calls the Every function directly"), Has(Lib, TEXT("\"Tick\" => Functions::tick(&mut self.state, ctx),\n")));
	TestTrue(TEXT("on_timer calls the Once After function directly"), Has(Lib, TEXT("\"EndMatch\" => Functions::end_match(&mut self.state, ctx),\n")));
	TestFalse(TEXT("without a schedule nothing uses cron"), Has(Lib, TEXT("ckx_sdk::cron")) || Has(Lib, TEXT("timer_cron")));
	TestFalse(TEXT("without a schedule nothing re-arms"), Has(Lib, TEXT("rearmed")));
	return true;
}

#endif
