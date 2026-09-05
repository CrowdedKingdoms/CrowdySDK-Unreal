#include "Replication/State/CrowdyBitwiseCompareTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Replication/State/CrowdyBitwiseCompare.h"
#include "UObject/UnrealType.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyBitwiseCompareTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

// The case the whole optimization exists for: a transform-shaped state struct, which is what the crowd
// sends. Both halves are asserted, the width and the agreement, because a width alone would still be a
// width for the wrong question.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyBitwiseCompareAcceptsTransformTest,
	"CrowdySDK.State.BitwiseCompareAcceptsAGapFreeTransformStruct", CrowdyBitwiseCompareTestFlags)
bool FCrowdyBitwiseCompareAcceptsTransformTest::RunTest(const FString& Parameters)
{
	UScriptStruct* const Struct = FCrowdyBitwiseTransformState::StaticStruct();

	TestEqual(TEXT("a gap-free transform struct is comparable over its whole size"),
		CrowdyBitwiseCompare::ComparableBytes(Struct), Struct->GetStructureSize());

	FCrowdyBitwiseTransformState First;
	First.Location = FVector(1.0, 2.0, 3.0);
	First.Rotation = FRotator(4.0, 5.0, 6.0);
	FCrowdyBitwiseTransformState Second = First;

	const int32 Size = Struct->GetStructureSize();
	TestTrue(TEXT("equal values compare equal both ways"),
		Struct->CompareScriptStruct(&First, &Second, PPF_None)
		&& FMemory::Memcmp(&First, &Second, Size) == 0);

	Second.Location.Y = 2.5;
	TestTrue(TEXT("a moved actor is seen as changed both ways"),
		!Struct->CompareScriptStruct(&First, &Second, PPF_None)
		&& FMemory::Memcmp(&First, &Second, Size) != 0);

	return true;
}

// Trailing padding is the reason the answer is a width rather than a yes. This fixture is the shape of the
// state struct the game's own executor sends, so refusing it outright would have made the whole change
// inert on the one path the plan measured, and comparing its full size would have called an unmoved actor
// changed on every tick forever. The test states all three readings, not just the accepted one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyBitwiseCompareTrailingPaddingTest,
	"CrowdySDK.State.BitwiseCompareStopsShortOfTrailingPadding", CrowdyBitwiseCompareTestFlags)
bool FCrowdyBitwiseCompareTrailingPaddingTest::RunTest(const FString& Parameters)
{
	UScriptStruct* const Struct = FCrowdyBitwisePaddedState::StaticStruct();
	const int32 Size = Struct->GetStructureSize();
	const int32 Comparable = CrowdyBitwiseCompare::ComparableBytes(Struct);

	if (!TestTrue(TEXT("the fixture is genuinely padded, which is its whole job"),
		Comparable > 0 && Comparable < Size))
	{
		return false;
	}

	TArray<uint8> First;
	TArray<uint8> Second;
	First.SetNumUninitialized(Size);
	Second.SetNumUninitialized(Size);
	Struct->InitializeStruct(First.GetData());
	Struct->InitializeStruct(Second.GetData());

	// Written after initialization, because InitializeStruct zero-fills: this stands in for the padding a
	// copy-constructed value carries over from whatever the compiler had on the stack.
	for (int32 Index = Comparable; Index < Size; ++Index)
	{
		Second[Index] = 0xCD;
	}

	TestTrue(TEXT("a property compare reads two values with equal properties as unchanged"),
		Struct->CompareScriptStruct(First.GetData(), Second.GetData(), PPF_None));
	TestTrue(TEXT("a compare over the whole size would call the same pair changed"),
		FMemory::Memcmp(First.GetData(), Second.GetData(), Size) != 0);
	TestTrue(TEXT("and the comparable run agrees with the property compare"),
		FMemory::Memcmp(First.GetData(), Second.GetData(), Comparable) == 0);

	// The run still has to see a real change, or stopping short would be stopping too short.
	FCrowdyBitwisePaddedState* const Typed = reinterpret_cast<FCrowdyBitwisePaddedState*>(Second.GetData());
	Typed->Team = 7;
	TestTrue(TEXT("while a change to the last property inside the run is still seen"),
		FMemory::Memcmp(First.GetData(), Second.GetData(), Comparable) != 0);

	Struct->DestroyStruct(First.GetData());
	Struct->DestroyStruct(Second.GetData());
	return true;
}

// A gap between two properties cannot be stepped over the way a tail can be stopped short of, so it is the
// case that must still be refused outright.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyBitwiseCompareInternalGapTest,
	"CrowdySDK.State.BitwiseCompareRefusesAnInternalGap", CrowdyBitwiseCompareTestFlags)
bool FCrowdyBitwiseCompareInternalGapTest::RunTest(const FString& Parameters)
{
	UScriptStruct* const Struct = FCrowdyBitwiseGappedState::StaticStruct();

	const FProperty* const Location = Struct->FindPropertyByName(TEXT("Location"));
	if (!TestNotNull(TEXT("the fixture exposes the property behind the gap"), Location))
	{
		return false;
	}

	if (!TestTrue(TEXT("which genuinely does not start where the byte before it ended"),
		Location->GetOffset_ForInternal() > 1))
	{
		return false;
	}

	TestEqual(TEXT("so the struct is refused outright"),
		CrowdyBitwiseCompare::ComparableBytes(Struct), 0);

	return true;
}

// The leaf kinds, each stated against the reason it is refused rather than only against the answer.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyBitwiseCompareLeafKindsTest,
	"CrowdySDK.State.BitwiseCompareRefusesNonBitwiseLeaves", CrowdyBitwiseCompareTestFlags)
bool FCrowdyBitwiseCompareLeafKindsTest::RunTest(const FString& Parameters)
{
	UScriptStruct* const TextStruct = FCrowdyBitwiseTextState::StaticStruct();

	FCrowdyBitwiseTextState First;
	FCrowdyBitwiseTextState Second;
	First.Name = TEXT("crowd");
	// Built rather than assigned so the two strings hold their characters in different buffers, which is
	// exactly the case a byte compare gets wrong.
	Second.Name = FString(TEXT("crow")) + TEXT("d");

	TestTrue(TEXT("two equal strings compare equal"),
		TextStruct->CompareScriptStruct(&First, &Second, PPF_None));
	TestTrue(TEXT("while their bytes differ, because each holds its own buffer"),
		FMemory::Memcmp(&First, &Second, TextStruct->GetStructureSize()) != 0);
	TestEqual(TEXT("so a struct carrying a string is refused"),
		CrowdyBitwiseCompare::ComparableBytes(TextStruct), 0);

	TestEqual(TEXT("a native bool and a byte tile their struct, so both bytes are comparable"),
		CrowdyBitwiseCompare::ComparableBytes(FCrowdyBitwiseFlagsState::StaticStruct()), 2);

	const FProperty* const Bitfield =
		FCrowdyBitwiseBitfieldState::StaticStruct()->FindPropertyByName(TEXT("bAirborne"));
	if (TestNotNull(TEXT("the bitfield fixture exposes its first flag"), Bitfield))
	{
		TestEqual(TEXT("a bitfield bool is refused, because it shares its byte"),
			CrowdyBitwiseCompare::ComparableBytes(Bitfield), 0);
	}

	const FProperty* const StaticArray =
		FCrowdyBitwiseArrayState::StaticStruct()->FindPropertyByName(TEXT("Samples"));
	if (TestNotNull(TEXT("the array fixture exposes its samples"), StaticArray))
	{
		TestEqual(TEXT("a static array is refused, because Identical reads element zero alone"),
			CrowdyBitwiseCompare::ComparableBytes(StaticArray), 0);
	}

	TestEqual(TEXT("a null struct is refused"),
		CrowdyBitwiseCompare::ComparableBytes(static_cast<const UScriptStruct*>(nullptr)), 0);
	TestEqual(TEXT("a null property is refused"),
		CrowdyBitwiseCompare::ComparableBytes(static_cast<const FProperty*>(nullptr)), 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
