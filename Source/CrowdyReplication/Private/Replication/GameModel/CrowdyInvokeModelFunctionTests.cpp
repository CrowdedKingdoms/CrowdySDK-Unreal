#include "Replication/GameModel/CrowdyInvokeModelFunctionActions.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyCallModelFunctionTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

// Each map value is parsed as a JSON literal and typed accordingly: an int/float becomes a number, a bool a
// boolean, a quoted literal a string. Keys are the parameter names verbatim.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCallModelFunctionBuildParamsTest,
	"CrowdySDK.GameModel.CallModelFunctionBuildParams", CrowdyCallModelFunctionTestFlags)
bool FCrowdyCallModelFunctionBuildParamsTest::RunTest(const FString& Parameters)
{
	TMap<FName, FString> In;
	In.Add(TEXT("amount"), TEXT("5"));
	In.Add(TEXT("rate"), TEXT("2.5"));
	In.Add(TEXT("flag"), TEXT("true"));
	In.Add(TEXT("label"), TEXT("\"hi\""));

	const TSharedPtr<FJsonObject> Params = UCrowdyInvokeModelFunctionAction::BuildParamsJson(In);
	if (!TestNotNull(TEXT("params built"), Params.Get()))
	{
		return false;
	}

	const TSharedPtr<FJsonValue> Amount = Params->TryGetField(TEXT("amount"));
	if (TestTrue(TEXT("amount present"), Amount.IsValid()))
	{
		TestEqual(TEXT("amount is a number"), static_cast<int32>(Amount->Type), static_cast<int32>(EJson::Number));
		TestEqual(TEXT("amount value"), static_cast<int32>(Amount->AsNumber()), 5);
	}
	TestEqual(TEXT("rate value"), Params->GetNumberField(TEXT("rate")), 2.5);

	const TSharedPtr<FJsonValue> Flag = Params->TryGetField(TEXT("flag"));
	if (TestTrue(TEXT("flag present"), Flag.IsValid()))
	{
		TestEqual(TEXT("flag is a boolean"), static_cast<int32>(Flag->Type), static_cast<int32>(EJson::Boolean));
		TestTrue(TEXT("flag value"), Flag->AsBool());
	}

	const TSharedPtr<FJsonValue> Label = Params->TryGetField(TEXT("label"));
	if (TestTrue(TEXT("label present"), Label.IsValid()))
	{
		TestEqual(TEXT("label is a string"), static_cast<int32>(Label->Type), static_cast<int32>(EJson::String));
		TestEqual(TEXT("label value"), Label->AsString(), FString(TEXT("hi")));
	}
	return true;
}

// A value that is not valid JSON is sent as a string, so a designer can type a bare word or a bare id without
// quotes. A bare all-digit id still parses as a number (the ambiguity is inherent without per-param type info);
// callers that need a string id quote it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCallModelFunctionForgivesNonJsonTest,
	"CrowdySDK.GameModel.CallModelFunctionForgivesNonJson", CrowdyCallModelFunctionTestFlags)
bool FCrowdyCallModelFunctionForgivesNonJsonTest::RunTest(const FString& Parameters)
{
	TMap<FName, FString> In;
	In.Add(TEXT("reason"), TEXT("critical"));       // a bare word -> a JSON string
	In.Add(TEXT("target_id"), TEXT("\"abc-123\"")); // an explicitly quoted id -> a JSON string

	const TSharedPtr<FJsonObject> Params = UCrowdyInvokeModelFunctionAction::BuildParamsJson(In);
	if (!TestNotNull(TEXT("params built"), Params.Get()))
	{
		return false;
	}

	const TSharedPtr<FJsonValue> Reason = Params->TryGetField(TEXT("reason"));
	if (TestTrue(TEXT("reason present"), Reason.IsValid()))
	{
		TestEqual(TEXT("bare word is a string"), static_cast<int32>(Reason->Type), static_cast<int32>(EJson::String));
		TestEqual(TEXT("bare word value"), Reason->AsString(), FString(TEXT("critical")));
	}

	const TSharedPtr<FJsonValue> TargetId = Params->TryGetField(TEXT("target_id"));
	if (TestTrue(TEXT("target_id present"), TargetId.IsValid()))
	{
		TestEqual(TEXT("quoted id is a string"), static_cast<int32>(TargetId->Type), static_cast<int32>(EJson::String));
		TestEqual(TEXT("quoted id value"), TargetId->AsString(), FString(TEXT("abc-123")));
	}
	return true;
}

// An empty map yields an empty object (not null); a None-key entry is skipped.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCallModelFunctionEmptyAndNoneKeyTest,
	"CrowdySDK.GameModel.CallModelFunctionEmptyAndNoneKey", CrowdyCallModelFunctionTestFlags)
bool FCrowdyCallModelFunctionEmptyAndNoneKeyTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Empty = UCrowdyInvokeModelFunctionAction::BuildParamsJson({});
	if (TestNotNull(TEXT("empty map yields an object"), Empty.Get()))
	{
		TestEqual(TEXT("no fields"), Empty->Values.Num(), 0);
	}

	TMap<FName, FString> In;
	In.Add(NAME_None, TEXT("5"));
	In.Add(TEXT("keep"), TEXT("1"));
	const TSharedPtr<FJsonObject> Params = UCrowdyInvokeModelFunctionAction::BuildParamsJson(In);
	if (TestNotNull(TEXT("params built"), Params.Get()))
	{
		TestFalse(TEXT("None key skipped"), Params->HasField(TEXT("None")));
		TestTrue(TEXT("real key kept"), Params->HasField(TEXT("keep")));
	}
	return true;
}

// A pathologically deep literal (a designer could thread an untrusted runtime string through a param value) is
// depth-rejected before parse and sent verbatim as a string, never built into a deep DOM that overflows on teardown.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCallModelFunctionRejectsDeepLiteralTest,
	"CrowdySDK.GameModel.CallModelFunctionRejectsDeepLiteral", CrowdyCallModelFunctionTestFlags)
bool FCrowdyCallModelFunctionRejectsDeepLiteralTest::RunTest(const FString& Parameters)
{
	FString Deep;
	const int32 Depth = 512; // well past CrowdyJsonSafety::MaxNestingDepth
	for (int32 i = 0; i < Depth; ++i) { Deep.AppendChar(TEXT('[')); }
	for (int32 i = 0; i < Depth; ++i) { Deep.AppendChar(TEXT(']')); }

	TMap<FName, FString> In;
	In.Add(TEXT("payload"), Deep);
	const TSharedPtr<FJsonObject> Params = UCrowdyInvokeModelFunctionAction::BuildParamsJson(In);
	if (!TestNotNull(TEXT("params built (no crash)"), Params.Get()))
	{
		return false;
	}
	const TSharedPtr<FJsonValue> Payload = Params->TryGetField(TEXT("payload"));
	if (TestTrue(TEXT("payload present"), Payload.IsValid()))
	{
		TestEqual(TEXT("deep literal sent as a string, not a parsed array"),
			static_cast<int32>(Payload->Type), static_cast<int32>(EJson::String));
		TestEqual(TEXT("string is the verbatim literal"), Payload->AsString(), Deep);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
