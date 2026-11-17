#include "Subsystem/CrowdyModScripts.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Containers/Ticker.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include <type_traits>

namespace CrowdyModScriptsStatusTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

using namespace CrowdyModScriptsStatusTests;

namespace CrowdyModScriptsStatusTests
{
	/** Starts one read and checks it answers exactly once, on a later tick. */
	template <typename AnswerType>
	AnswerType AskOnce(FAutomationTestBase& Test, const TCHAR* Call, TFunctionRef<void(TFunction<void(const AnswerType&)>)> Start)
	{
		const TSharedRef<int32> Answers = MakeShared<int32>(0);
		const TSharedRef<AnswerType> Seen = MakeShared<AnswerType>();
		Start([Answers, Seen](const AnswerType& Answer)
		{
			*Seen = Answer;
			++*Answers;
		});
		Test.TestEqual(FString::Printf(TEXT("%s does not answer inside the call"), Call), *Answers, 0);
		for (int32 Tick = 0; Tick < 4; ++Tick)
		{
			FTSTicker::GetCoreTicker().Tick(0.f);
		}
		Test.TestEqual(FString::Printf(TEXT("%s answers exactly once"), Call), *Answers, 1);
		return *Seen;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsStatusNotInGridTest, "CrowdySDK.ModScripts.StatusAndLogRefuseAGridThePlayerIsNotIn", Flags)
bool FCrowdyModScriptsStatusNotInGridTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyModScripts> Scripts(NewObject<UCrowdyModScripts>(GetTransientPackage()));
	UCrowdyModScripts* Raw = Scripts.Get();

	const FCrowdyModScriptStatusList Status = AskOnce<FCrowdyModScriptStatusList>(*this, TEXT("Get Script Status"),
		[Raw](TFunction<void(const FCrowdyModScriptStatusList&)> OnDone) { Raw->GetScriptStatus(901, MoveTemp(OnDone)); });
	TestTrue(TEXT("a grid the player is not in is not in grid"), Status.Result == ECrowdyModScriptReadResult::NotInGrid);
	TestTrue(TEXT("it lists no scripts"), Status.Scripts.IsEmpty());
	TestEqual(TEXT("and names the grid asked for"), Status.GridId, static_cast<int64>(901));
	TestFalse(TEXT("with a reason in words"), Status.Message.IsEmpty());

	const FCrowdyModScriptStatusList Zero = AskOnce<FCrowdyModScriptStatusList>(*this, TEXT("Get Script Status of grid 0"),
		[Raw](TFunction<void(const FCrowdyModScriptStatusList&)> OnDone) { Raw->GetScriptStatus(0, MoveTemp(OnDone)); });
	TestTrue(TEXT("grid 0 is outside every grid"), Zero.Result == ECrowdyModScriptReadResult::NotInGrid);

	const FCrowdyModScriptLog Log = AskOnce<FCrowdyModScriptLog>(*this, TEXT("Get Script Log"),
		[Raw](TFunction<void(const FCrowdyModScriptLog&)> OnDone) { Raw->GetScriptLog(901, TEXT("m-7"), MoveTemp(OnDone)); });
	TestTrue(TEXT("a log outside a grid is not in grid"), Log.Result == ECrowdyModScriptReadResult::NotInGrid);
	TestTrue(TEXT("it holds no lines"), Log.Lines.IsEmpty() && Log.DroppedLines == 0);
	TestEqualSensitive(TEXT("and names its script"), Log.ModId, FString(TEXT("m-7")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModScriptsStatusDefaultsTest, "CrowdySDK.ModScripts.StatusAnswersDefaultToNothing", Flags)
bool FCrowdyModScriptsStatusDefaultsTest::RunTest(const FString& Parameters)
{
	static_assert(std::is_same_v<decltype(FCrowdyModScriptStatusList::GridId), int64>, "a grid id is a BigInt");
	static_assert(std::is_same_v<decltype(FCrowdyModScriptLogLine::AtMs), int64>, "a time is int64 milliseconds");
	const FCrowdyModScriptStatusList List;
	TestTrue(TEXT("a default status is not a success and is empty"), List.Result != ECrowdyModScriptReadResult::Success && List.GridId == 0 && List.Scripts.IsEmpty());
	const FCrowdyModScriptStatus Status;
	TestTrue(TEXT("a default script is not running"), Status.State != ECrowdyModScriptState::Running && Status.Script.ModId.IsEmpty());
	TestTrue(TEXT("a stop reason is never a state"), Status.StopReason != ECrowdyModScriptStop::CircuitOpen && Status.StopReason != ECrowdyModScriptStop::Failed);
	const FCrowdyModScriptLog Log;
	TestTrue(TEXT("a default log is not a success and is empty"), Log.Result != ECrowdyModScriptReadResult::Success && Log.Lines.IsEmpty() && Log.DroppedLines == 0);
	TestEqual(TEXT("the ring keeps 200 lines"), UCrowdyModScripts::MaxLogLines, 200);
	return true;
}

#endif
