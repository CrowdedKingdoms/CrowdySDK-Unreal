// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Replication/GameModel/Effect/CrowdyEffectMagnitudeJson.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyEffectMagnitudeTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

// The pure typed-value <-> canonical-JSON conversion the typed default editor writes: Int/Float produce a bare
// number, Bool produces true/false, String/ContainerRef produce a JSON-quoted string, and the raw form round-trips
// back. This is the exact DefaultValueJson shape BuildInvokeParams / the lowering consume.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectMagnitudeDefaultJsonRoundTripTest,
	"CrowdySDK.Editor.EffectMagnitudeDefaultJsonRoundTrip", CrowdyEffectMagnitudeTestFlags)
bool FCrowdyEffectMagnitudeDefaultJsonRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyEffectMagnitudeJson;
	using EVT = ECrowdyEffectValueType;

	TestEqual(TEXT("int canonical"), ToCanonicalJson(EVT::Int, TEXT("5")), FString(TEXT("5")));
	TestEqual(TEXT("float canonical from int text"), ToCanonicalJson(EVT::Float, TEXT("5")), FString(TEXT("5.0")));
	TestEqual(TEXT("float canonical keeps decimal"), ToCanonicalJson(EVT::Float, TEXT("5.0")), FString(TEXT("5.0")));
	TestEqual(TEXT("bool true"), ToCanonicalJson(EVT::Bool, TEXT("true")), FString(TEXT("true")));
	TestEqual(TEXT("bool false"), ToCanonicalJson(EVT::Bool, TEXT("false")), FString(TEXT("false")));
	TestEqual(TEXT("string quoted"), ToCanonicalJson(EVT::String, TEXT("text")), FString(TEXT("\"text\"")));
	TestEqual(TEXT("container_ref quoted"), ToCanonicalJson(EVT::ContainerRef, TEXT("abc")), FString(TEXT("\"abc\"")));

	// Empty input stays empty (a required magnitude), for every type.
	TestTrue(TEXT("empty int stays empty"), ToCanonicalJson(EVT::Int, TEXT("")).IsEmpty());
	TestTrue(TEXT("empty string stays empty"), ToCanonicalJson(EVT::String, TEXT("")).IsEmpty());

	// FromCanonicalJson reverses each type back to the raw editor value.
	TestEqual(TEXT("int raw"), FromCanonicalJson(EVT::Int, TEXT("5")), FString(TEXT("5")));
	TestEqual(TEXT("float raw"), FromCanonicalJson(EVT::Float, TEXT("5.0")), FString(TEXT("5.0")));
	TestEqual(TEXT("bool raw"), FromCanonicalJson(EVT::Bool, TEXT("true")), FString(TEXT("true")));
	TestEqual(TEXT("string unquoted"), FromCanonicalJson(EVT::String, TEXT("\"text\"")), FString(TEXT("text")));
	TestEqual(TEXT("container_ref unquoted"), FromCanonicalJson(EVT::ContainerRef, TEXT("\"abc\"")), FString(TEXT("abc")));

	// A full raw -> canonical -> raw round trip is stable for the quoting types.
	TestEqual(TEXT("string round trips"),
		FromCanonicalJson(EVT::String, ToCanonicalJson(EVT::String, TEXT("hello world"))), FString(TEXT("hello world")));
	TestEqual(TEXT("container_ref round trips"),
		FromCanonicalJson(EVT::ContainerRef, ToCanonicalJson(EVT::ContainerRef, TEXT("id-42"))), FString(TEXT("id-42")));

	// A string default keeps intentional leading / trailing whitespace (only numeric/bool values are normalized,
	// and only an all-whitespace value counts as an empty default).
	TestEqual(TEXT("string keeps trailing space"),
		ToCanonicalJson(EVT::String, TEXT("Sir ")), FString(TEXT("\"Sir \"")));
	TestEqual(TEXT("string keeps leading space"),
		ToCanonicalJson(EVT::String, TEXT(" leading")), FString(TEXT("\" leading\"")));
	TestEqual(TEXT("padded string round trips losslessly"),
		FromCanonicalJson(EVT::String, ToCanonicalJson(EVT::String, TEXT("Sir "))), FString(TEXT("Sir ")));
	TestTrue(TEXT("all-whitespace string is still an empty default"),
		ToCanonicalJson(EVT::String, TEXT("   ")).IsEmpty());
	TestEqual(TEXT("numeric input is still trimmed"),
		ToCanonicalJson(EVT::Int, TEXT("  7  ")), FString(TEXT("7")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
