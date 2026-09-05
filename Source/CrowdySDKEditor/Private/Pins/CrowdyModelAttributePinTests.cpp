// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Pins/CrowdyModelAttributeNamePin.h"
#include "Customizations/CrowdyEffectPickerTestFixture.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyModelAttributePinTestFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;
}

// The pure option-computation core of the attribute-key dropdown: given a container class and a getter's value
// type, ComputeMatchingKeys returns only the keys of that type; ValueTypeForGetter maps each getter's function
// name to its value type; and the list is purely additive (free text is never restricted).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelAttributePinOptionsTest,
	"CrowdySDK.Editor.ModelAttributePinOptions", CrowdyModelAttributePinTestFlags)
bool FCrowdyModelAttributePinOptionsTest::RunTest(const FString& Parameters)
{
	const UClass* Class = UCrowdyEffectPickerTestTarget::StaticClass();

	// The int filter lists only the int-typed key ("health"), and not the float/bool/string keys.
	const TArray<FName> IntKeys = SCrowdyModelAttributeNamePin::ComputeMatchingKeys(Class, TEXT("int"));
	TestEqual(TEXT("one int key"), IntKeys.Num(), 1);
	TestTrue(TEXT("int lists health"), IntKeys.Contains(FName(TEXT("health"))));
	TestFalse(TEXT("int omits float key"), IntKeys.Contains(FName(TEXT("speed"))));

	const TArray<FName> FloatKeys = SCrowdyModelAttributeNamePin::ComputeMatchingKeys(Class, TEXT("float"));
	TestEqual(TEXT("one float key"), FloatKeys.Num(), 1);
	TestTrue(TEXT("float lists speed"), FloatKeys.Contains(FName(TEXT("speed"))));

	const TArray<FName> BoolKeys = SCrowdyModelAttributeNamePin::ComputeMatchingKeys(Class, TEXT("bool"));
	TestEqual(TEXT("one bool key"), BoolKeys.Num(), 1);
	TestTrue(TEXT("bool lists bstunned"), BoolKeys.Contains(FName(TEXT("bstunned"))));

	const TArray<FName> StringKeys = SCrowdyModelAttributeNamePin::ComputeMatchingKeys(Class, TEXT("string"));
	TestEqual(TEXT("one string key"), StringKeys.Num(), 1);
	TestTrue(TEXT("string lists title"), StringKeys.Contains(FName(TEXT("title"))));

	// A plain (non-CrowdyModel) property is never listed under any filter.
	TestFalse(TEXT("int omits plain property"), IntKeys.Contains(FName(TEXT("plainint"))));

	// Unknown class or empty value type yields nothing.
	TestEqual(TEXT("null class -> empty"),
		SCrowdyModelAttributeNamePin::ComputeMatchingKeys(nullptr, TEXT("int")).Num(), 0);
	TestEqual(TEXT("empty value type -> empty"),
		SCrowdyModelAttributeNamePin::ComputeMatchingKeys(Class, FString()).Num(), 0);

	// The list is a suggestion set, never a whitelist: a key the class does not declare is simply absent, and the
	// helper offers no membership check that could reject a typed key. Free text stays valid.
	TestFalse(TEXT("undiscovered key is absent, not rejected"),
		IntKeys.Contains(FName(TEXT("madeupkey"))));

	// Getter function name -> value type.
	TestEqual(TEXT("GetInt -> int"),
		SCrowdyModelAttributeNamePin::ValueTypeForGetter(TEXT("GetInt")), FString(TEXT("int")));
	TestEqual(TEXT("GetFloat -> float"),
		SCrowdyModelAttributeNamePin::ValueTypeForGetter(TEXT("GetFloat")), FString(TEXT("float")));
	TestEqual(TEXT("GetBool -> bool"),
		SCrowdyModelAttributeNamePin::ValueTypeForGetter(TEXT("GetBool")), FString(TEXT("bool")));
	TestEqual(TEXT("GetString -> string"),
		SCrowdyModelAttributeNamePin::ValueTypeForGetter(TEXT("GetString")), FString(TEXT("string")));
	TestEqual(TEXT("unrelated function -> empty"),
		SCrowdyModelAttributeNamePin::ValueTypeForGetter(TEXT("PullNow")), FString());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
