// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Serialization/CrowdyJsonSafety.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyJsonSafetyTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// A JSON array nested Depth levels deep: Depth=0 -> "1", Depth=2 -> "[[1]]".
	FString CrowdyJsonSafetyNestedArray(int32 Depth)
	{
		FString Out = TEXT("1");
		for (int32 Index = 0; Index < Depth; ++Index)
		{
			Out = TEXT("[") + Out + TEXT("]");
		}
		return Out;
	}
}

// The nesting pre-scan bounds object/array depth before parsing, ignoring brackets inside string literals, so a
// forged deeply-nested payload is rejected before it can build a stack-overflowing DOM while a normal shallow
// payload always passes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyJsonNestingLimitTest,
	"CrowdySDK.SharedTypes.JsonNestingLimit", CrowdyJsonSafetyTestFlags)
bool FCrowdyJsonNestingLimitTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyJsonSafety;

	// Shallow, realistic payloads pass under the default bound.
	TestTrue(TEXT("empty string passes"), IsNestingWithinLimit(TEXT("")));
	TestTrue(TEXT("a flat object passes"), IsNestingWithinLimit(TEXT("{\"a\":1,\"b\":[1,2,3]}")));
	TestTrue(TEXT("a mixed nested object passes"), IsNestingWithinLimit(TEXT("{\"a\":{\"b\":[{\"c\":1}]}}")));

	// Exactly at the bound passes; one level deeper is rejected (the boundary is inclusive).
	TestTrue(TEXT("at the bound passes"), IsNestingWithinLimit(CrowdyJsonSafetyNestedArray(MaxNestingDepth)));
	TestFalse(TEXT("one past the bound is rejected"), IsNestingWithinLimit(CrowdyJsonSafetyNestedArray(MaxNestingDepth + 1)));

	// A small explicit bound is easy to exercise: depth 3 passes at limit 3, depth 4 fails.
	TestTrue(TEXT("depth 3 within limit 3"), IsNestingWithinLimit(CrowdyJsonSafetyNestedArray(3), 3));
	TestFalse(TEXT("depth 4 exceeds limit 3"), IsNestingWithinLimit(CrowdyJsonSafetyNestedArray(4), 3));

	// Brackets INSIDE a string literal are data, not nesting - they never build DOM nodes, so they do not count.
	FString BracketsInString = TEXT("{\"note\":\"");
	for (int32 Index = 0; Index < 5000; ++Index)
	{
		BracketsInString += TEXT("[");
	}
	BracketsInString += TEXT("\"}");
	TestTrue(TEXT("brackets inside a string are not counted"), IsNestingWithinLimit(BracketsInString, 4));

	// An escaped quote inside a string does not prematurely end it, so the following brackets stay uncounted.
	TestTrue(TEXT("escaped quote keeps the string open"), IsNestingWithinLimit(TEXT("{\"a\":\"x\\\"[[[[[\"}"), 2));

	// A genuine deep bomb (real brackets) is rejected.
	FString RealBomb;
	for (int32 Index = 0; Index < 5000; ++Index)
	{
		RealBomb += TEXT("[");
	}
	TestFalse(TEXT("a real deep bracket bomb is rejected"), IsNestingWithinLimit(RealBomb, MaxNestingDepth));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
