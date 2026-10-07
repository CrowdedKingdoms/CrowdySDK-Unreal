#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyServerObjectDefinitionCustomization.h"
#include "Misc/AutomationTest.h"
#include "StructUtils/PropertyBag.h"

namespace CrowdyServerObjectRulesEditorTest
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerValueRangeEditTest, "CrowdySDK.CrowdyExecEditor.ValueRangeWritesAndClearsInputMetadata", CrowdyServerObjectRulesEditorTest::TestFlags)
bool FCrowdyServerValueRangeEditTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerValueRange;

	FPropertyBagPropertyDesc Amount(TEXT("Amount"), EPropertyBagPropertyType::Int32);
	TestTrue(TEXT("an Integer input can have a Value Range"), CanHaveRange(Amount));
	TestTrue(TEXT("typing a Min writes ClampMin"), SetBound(Amount, MinKey, TEXT(" 1 ")));
	TestEqual(TEXT("ClampMin is the trimmed number"), Amount.GetMetaData(MinKey), FString(TEXT("1")));
	TestTrue(TEXT("typing a Max writes ClampMax"), SetBound(Amount, MaxKey, TEXT("100")));
	TestFalse(TEXT("the same Min again changes nothing"), SetBound(Amount, MinKey, TEXT("1")));
	TestFalse(TEXT("words are refused"), SetBound(Amount, MaxKey, TEXT("lots")));
	TestFalse(TEXT("a fraction on an Integer input is refused"), SetBound(Amount, MinKey, TEXT("1.5")));
	TestFalse(TEXT("a plus sign is refused, as the bake refuses it"), SetBound(Amount, MinKey, TEXT("+5")));
	TestEqual(TEXT("a refused Max keeps the old one"), Amount.GetMetaData(MaxKey), FString(TEXT("100")));

	// The stored bounds become the input's own metadata, which the default value editor and the generated code read.
	FInstancedPropertyBag Inputs;
	Inputs.AddProperties({Amount});
	const FPropertyBagPropertyDesc* Stored = Inputs.FindPropertyDescByName(TEXT("Amount"));
	const FProperty* Property = Stored ? Stored->CachedProperty : nullptr;
	if (!TestNotNull(TEXT("the bag has Amount"), Property))
	{
		return false;
	}
	TestEqual(TEXT("Amount's property carries ClampMin"), Property->GetMetaData(MinKey), FString(TEXT("1")));
	TestEqual(TEXT("Amount's property carries ClampMax"), Property->GetMetaData(MaxKey), FString(TEXT("100")));

	TestTrue(TEXT("an empty Min clears ClampMin"), SetBound(Amount, MinKey, TEXT("  ")));
	TestFalse(TEXT("ClampMin is gone"), Amount.HasMetaData(MinKey));
	TestFalse(TEXT("clearing an empty Min changes nothing"), SetBound(Amount, MinKey, FString()));
	TestTrue(TEXT("ClampMax stays when Min is cleared"), Amount.HasMetaData(MaxKey));

	FPropertyBagPropertyDesc Speed(TEXT("Speed"), EPropertyBagPropertyType::Float);
	TestTrue(TEXT("a Float input takes a fraction"), SetBound(Speed, MaxKey, TEXT("0.5")));

	FPropertyBagPropertyDesc Label(TEXT("Label"), EPropertyBagPropertyType::String);
	FPropertyBagPropertyDesc Ready(TEXT("Ready"), EPropertyBagPropertyType::Bool);
	FPropertyBagPropertyDesc Scores(TEXT("Scores"), EPropertyBagContainerType::Array, EPropertyBagPropertyType::Int32);
	TestFalse(TEXT("a String input has no Value Range"), CanHaveRange(Label));
	TestFalse(TEXT("a Boolean input has no Value Range"), CanHaveRange(Ready));
	TestFalse(TEXT("an Array input has no Value Range"), CanHaveRange(Scores));
	TestFalse(TEXT("a String input refuses a bound"), SetBound(Label, MinKey, TEXT("1")));

	// An input retyped from a number keeps its range until it is cleared.
	Label.SetMetaData(MinKey, TEXT("1"));
	TestTrue(TEXT("a leftover range on a String input can be cleared"), SetBound(Label, MinKey, FString()));
	TestFalse(TEXT("the leftover ClampMin is gone"), Label.HasMetaData(MinKey));
	return true;
}

#endif
