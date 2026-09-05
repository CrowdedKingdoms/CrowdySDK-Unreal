// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/CrowdyEffects.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"
#include "UObject/Package.h"

#include <limits>

namespace
{
	constexpr EAutomationTestFlags CrowdyEffectJsonHelperTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

// The typed-value -> JSON-literal encoders the smart Apply node feeds into its Overrides map: an int/float is a
// bare number, a bool is true/false, and a string is JSON-quoted with escaping. GetContainerIdFor returns empty for
// an object with no world / binding. These are exactly the literal shapes BuildInvokeParams accepts.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectJsonConvHelpersTest,
	"CrowdySDK.Replication.EffectJsonConvHelpers", CrowdyEffectJsonHelperTestFlags)
bool FCrowdyEffectJsonConvHelpersTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("int positive"), UCrowdyEffects::JsonFromInt(5), FString(TEXT("5")));
	TestEqual(TEXT("int negative"), UCrowdyEffects::JsonFromInt(-3), FString(TEXT("-3")));
	TestEqual(TEXT("int zero"), UCrowdyEffects::JsonFromInt(0), FString(TEXT("0")));

	TestEqual(TEXT("bool true"), UCrowdyEffects::JsonFromBool(true), FString(TEXT("true")));
	TestEqual(TEXT("bool false"), UCrowdyEffects::JsonFromBool(false), FString(TEXT("false")));

	TestEqual(TEXT("float half"), UCrowdyEffects::JsonFromFloat(2.5), FString(TEXT("2.5")));
	// A non-finite value has no JSON form, so it degrades to a safe 0 rather than "nan"/"inf".
	TestEqual(TEXT("float non-finite -> 0"),
		UCrowdyEffects::JsonFromFloat(std::numeric_limits<double>::infinity()), FString(TEXT("0")));

	TestEqual(TEXT("string quoted"), UCrowdyEffects::JsonFromString(TEXT("text")), FString(TEXT("\"text\"")));
	TestEqual(TEXT("empty string quoted"), UCrowdyEffects::JsonFromString(TEXT("")), FString(TEXT("\"\"")));
	// An embedded quote and backslash are escaped so the result is valid JSON.
	TestEqual(TEXT("string escapes quote"),
		UCrowdyEffects::JsonFromString(TEXT("a\"b")), FString(TEXT("\"a\\\"b\"")));
	TestEqual(TEXT("string escapes backslash"),
		UCrowdyEffects::JsonFromString(TEXT("a\\b")), FString(TEXT("\"a\\\\b\"")));

	// GetContainerIdFor: null and a worldless object both resolve to empty (no binding to read).
	TestTrue(TEXT("null object -> empty id"), UCrowdyEffects::GetContainerIdFor(nullptr).IsEmpty());
	UObject* Loose = NewObject<UCrowdyEffect>(GetTransientPackage());
	TestTrue(TEXT("worldless object -> empty id"), UCrowdyEffects::GetContainerIdFor(Loose).IsEmpty());

	return true;
}

// The JsonTo* decoders the smart Apply Crowdy Effect node splices behind its typed Return Value pin: the exact
// inverse of the JsonFrom* encoders above, delegating to UCrowdyModelValue. Each round-trips a JsonFrom* output and
// answers with its type's zero value (0 / 0.0 / false / "") for empty, malformed, and wrong-typed input.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectJsonDecodeHelpersTest,
	"CrowdySDK.Replication.EffectJsonDecodeHelpers", CrowdyEffectJsonHelperTestFlags)
bool FCrowdyEffectJsonDecodeHelpersTest::RunTest(const FString& Parameters)
{
	// Round trip: JsonFrom* -> JsonTo* recovers the original typed value.
	TestEqual(TEXT("int round trip negative"),
		UCrowdyEffects::JsonToInt(UCrowdyEffects::JsonFromInt(-3)), -3);
	TestEqual(TEXT("float round trip fractional"),
		UCrowdyEffects::JsonToFloat(UCrowdyEffects::JsonFromFloat(2.5)), 2.5);
	TestEqual(TEXT("bool round trip true"),
		UCrowdyEffects::JsonToBool(UCrowdyEffects::JsonFromBool(true)), true);
	TestEqual(TEXT("bool round trip false"),
		UCrowdyEffects::JsonToBool(UCrowdyEffects::JsonFromBool(false)), false);
	TestEqual(TEXT("string round trip needing escaping"),
		UCrowdyEffects::JsonToString(UCrowdyEffects::JsonFromString(TEXT("a\"b"))), FString(TEXT("a\"b")));

	// Empty input answers with each type's zero value.
	TestEqual(TEXT("int empty -> 0"), UCrowdyEffects::JsonToInt(TEXT("")), 0);
	TestEqual(TEXT("float empty -> 0.0"), UCrowdyEffects::JsonToFloat(TEXT("")), 0.0);
	TestEqual(TEXT("bool empty -> false"), UCrowdyEffects::JsonToBool(TEXT("")), false);
	TestEqual(TEXT("string empty -> empty"), UCrowdyEffects::JsonToString(TEXT("")), FString());

	// Malformed input answers with each type's zero value.
	TestEqual(TEXT("int malformed -> 0"), UCrowdyEffects::JsonToInt(TEXT("{not json")), 0);
	TestEqual(TEXT("float malformed -> 0.0"), UCrowdyEffects::JsonToFloat(TEXT("{not json")), 0.0);
	TestEqual(TEXT("bool malformed -> false"), UCrowdyEffects::JsonToBool(TEXT("{not json")), false);
	TestEqual(TEXT("string malformed -> empty"), UCrowdyEffects::JsonToString(TEXT("{not json")), FString());

	// Wrong-typed input (a string where a number/bool is expected, a number where a string is expected) answers
	// with each type's zero value.
	TestEqual(TEXT("int wrong-typed -> 0"), UCrowdyEffects::JsonToInt(TEXT("\"text\"")), 0);
	TestEqual(TEXT("float wrong-typed -> 0.0"), UCrowdyEffects::JsonToFloat(TEXT("\"text\"")), 0.0);
	TestEqual(TEXT("bool wrong-typed -> false"), UCrowdyEffects::JsonToBool(TEXT("5")), false);
	TestEqual(TEXT("string wrong-typed -> empty"), UCrowdyEffects::JsonToString(TEXT("5")), FString());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
