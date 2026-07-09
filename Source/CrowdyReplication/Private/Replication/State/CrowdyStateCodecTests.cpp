#include "Replication/State/CrowdyStateTestTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Replication/State/CrowdyStateCodec.h"
#include "Replication/State/FCrowdyRepLayout.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyStateCodecTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Sets every replicated int32 on a wide target to a distinct value derived from its index, so a
	// round-trip can prove each slot decoded to the right place (not merely to the right count).
	int32 WideValueForIndex(int32 Index)
	{
		return 1000 + Index * 7;
	}
}

// A full keyframe of every supported property kind round-trips: numerics/bool/byte/enum/name/string
// exactly, plain FVector/FRotator exactly (SerializeItem), and the net-quantized FVector within
// tolerance. Decode target starts fresh/zeroed so a pass proves the values were carried, not left over.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateCodecFullRoundTripTest,
	"CrowdySDK.State.FullRoundTrip", CrowdyStateCodecTestFlags)
bool FCrowdyStateCodecFullRoundTripTest::RunTest(const FString& Parameters)
{
	FCrowdyRepLayout Layout;
	TestTrue(TEXT("layout built"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyStateCodecTarget::StaticClass(), Layout));
	const int32 N = Layout.Properties.Num();
	TestTrue(TEXT("layout non-empty"), N > 0);

	UCrowdyStateCodecTarget* Source = NewObject<UCrowdyStateCodecTarget>();
	Source->RepInt = 42;
	Source->RepBigInt = static_cast<int64>(9000000001);
	Source->RepFloat = 1.25f;
	Source->RepDouble = -3.5;
	Source->bRepFlag = true;
	Source->RepByte = 200;
	Source->RepEnum = ECrowdyStateTestEnum::Gamma;
	Source->RepName = FName(TEXT("MyTag"));
	Source->RepString = TEXT("hello crowdy state");
	Source->RepVector = FVector(1.0, -2.5, 3.25);
	Source->RepRotator = FRotator(10.0, 20.0, 30.0);
	Source->RepQuantized = FVector_NetQuantize(12.4, -7.8, 3.6);

	// Keyframe: every layout bit treated as set regardless of Dirty.
	TBitArray<> Dirty(true, N);
	TArray<uint8> Blob;
	FCrowdyStateCodec::Encode(Layout, Source, Dirty, /*bKeyframe=*/true, Blob);
	TestTrue(TEXT("keyframe blob non-empty"), Blob.Num() > 2);

	UCrowdyStateCodecTarget* Dest = NewObject<UCrowdyStateCodecTarget>();
	TArray<int32> Changed;
	const bool bOk = FCrowdyStateCodec::Decode(Layout, Layout.LayoutHash, Blob, Dest, Changed);
	TestTrue(TEXT("decode succeeded"), bOk);
	TestEqual(TEXT("every slot reported changed"), Changed.Num(), N);

	TestEqual(TEXT("int32"), Dest->RepInt, 42);
	TestEqual(TEXT("int64"), Dest->RepBigInt, static_cast<int64>(9000000001));
	TestEqual(TEXT("float"), Dest->RepFloat, 1.25f);
	TestEqual(TEXT("double"), Dest->RepDouble, -3.5);
	TestEqual(TEXT("bool"), Dest->bRepFlag, true);
	TestEqual(TEXT("byte"), static_cast<int32>(Dest->RepByte), 200);
	TestTrue(TEXT("enum"), Dest->RepEnum == ECrowdyStateTestEnum::Gamma);
	TestTrue(TEXT("name"), Dest->RepName == FName(TEXT("MyTag")));
	TestEqual(TEXT("string"), Dest->RepString, FString(TEXT("hello crowdy state")));
	TestTrue(TEXT("plain vector exact"), Dest->RepVector.Equals(Source->RepVector));
	// FRotator declares a native net serializer (NetSerialize -> SerializeCompressedShort, a uint16 per
	// axis, ~0.0055 deg step), so like any STRUCT_NetSerializeNative struct it rides the quantized
	// NetSerializeItem path, not SerializeItem, and round-trips within the compression step, not exactly.
	TestTrue(TEXT("rotator within net-quantization tolerance"),
		Dest->RepRotator.Equals(Source->RepRotator, 0.01));
	// FVector_NetQuantize serializes at scale 1 (0 decimal places), so a fractional input rounds to the
	// nearest integer per component  max error 0.5. The tolerance reflects that quantization, not codec
	// error: the codec faithfully carries whatever the quantizer produced.
	TestTrue(TEXT("quantized vector within quantization tolerance"),
		Dest->RepQuantized.Equals(Source->RepQuantized, 0.5));
	return true;
}

// A hot delta carrying exactly one changed property is materially smaller than the keyframe, decodes
// onto a baseline touching only that property, and reports exactly that one changed index.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateCodecOnlyChangedDeltaTest,
	"CrowdySDK.State.OnlyChangedDelta", CrowdyStateCodecTestFlags)
bool FCrowdyStateCodecOnlyChangedDeltaTest::RunTest(const FString& Parameters)
{
	FCrowdyRepLayout Layout;
	TestTrue(TEXT("layout built"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyStateCodecTarget::StaticClass(), Layout));
	const int32 N = Layout.Properties.Num();

	// Resolve the layout index of RepString by name so the test does not hard-code positional order.
	int32 TargetIndex = INDEX_NONE;
	for (int32 i = 0; i < N; ++i)
	{
		if (Layout.Properties[i].Property && Layout.Properties[i].Property->GetFName() == FName(TEXT("RepString")))
		{
			TargetIndex = i;
			break;
		}
	}
	TestNotEqual(TEXT("RepString found in layout"), TargetIndex, static_cast<int32>(INDEX_NONE));
	if (TargetIndex == INDEX_NONE)
	{
		return false;
	}

	// A baseline with known non-default values in every slot, so "unchanged" is testable.
	const auto Populate = [](UCrowdyStateCodecTarget* T)
	{
		T->RepInt = 7;
		T->RepBigInt = 8;
		T->RepFloat = 9.f;
		T->RepDouble = 10.0;
		T->bRepFlag = true;
		T->RepByte = 11;
		T->RepEnum = ECrowdyStateTestEnum::Beta;
		T->RepName = FName(TEXT("Base"));
		T->RepString = TEXT("baseline");
		T->RepVector = FVector(1.0, 2.0, 3.0);
		T->RepRotator = FRotator(4.0, 5.0, 6.0);
		T->RepQuantized = FVector_NetQuantize(1.0, 2.0, 3.0);
	};

	UCrowdyStateCodecTarget* Source = NewObject<UCrowdyStateCodecTarget>();
	Populate(Source);
	Source->RepString = TEXT("changed only me");

	// The keyframe size, for the smaller-than comparison.
	TArray<uint8> KeyframeBlob;
	FCrowdyStateCodec::Encode(Layout, Source, TBitArray<>(true, N), /*bKeyframe=*/true, KeyframeBlob);

	// The hot delta: only the one changed index dirty.
	TBitArray<> Dirty(false, N);
	Dirty[TargetIndex] = true;
	TArray<uint8> HotBlob;
	FCrowdyStateCodec::Encode(Layout, Source, Dirty, /*bKeyframe=*/false, HotBlob);
	TestTrue(TEXT("hot delta smaller than keyframe"), HotBlob.Num() < KeyframeBlob.Num());

	// Decode onto an instance holding the baseline; only RepString may change.
	UCrowdyStateCodecTarget* Dest = NewObject<UCrowdyStateCodecTarget>();
	Populate(Dest);
	TArray<int32> Changed;
	const bool bOk = FCrowdyStateCodec::Decode(Layout, Layout.LayoutHash, HotBlob, Dest, Changed);
	TestTrue(TEXT("decode succeeded"), bOk);

	TestEqual(TEXT("exactly one changed index"), Changed.Num(), 1);
	if (Changed.Num() == 1)
	{
		TestEqual(TEXT("changed index is RepString"), Changed[0], TargetIndex);
	}

	TestEqual(TEXT("RepString updated"), Dest->RepString, FString(TEXT("changed only me")));
	// Every other property still equals the baseline.
	TestEqual(TEXT("RepInt unchanged"), Dest->RepInt, 7);
	TestEqual(TEXT("RepBigInt unchanged"), Dest->RepBigInt, static_cast<int64>(8));
	TestEqual(TEXT("RepFloat unchanged"), Dest->RepFloat, 9.f);
	TestEqual(TEXT("RepDouble unchanged"), Dest->RepDouble, 10.0);
	TestEqual(TEXT("bRepFlag unchanged"), Dest->bRepFlag, true);
	TestEqual(TEXT("RepByte unchanged"), static_cast<int32>(Dest->RepByte), 11);
	TestTrue(TEXT("RepEnum unchanged"), Dest->RepEnum == ECrowdyStateTestEnum::Beta);
	TestTrue(TEXT("RepName unchanged"), Dest->RepName == FName(TEXT("Base")));
	TestTrue(TEXT("RepVector unchanged"), Dest->RepVector.Equals(FVector(1.0, 2.0, 3.0)));
	TestTrue(TEXT("RepRotator unchanged"), Dest->RepRotator.Equals(FRotator(4.0, 5.0, 6.0)));
	TestTrue(TEXT("RepQuantized unchanged"), Dest->RepQuantized.Equals(FVector_NetQuantize(1.0, 2.0, 3.0), 0.01));
	return true;
}

// The smaller-of selector picks the index-list for a single dirty bit (mode 1) and the bitmask for all
// 64 bits (mode 0), and both round-trip. OutBlob[0] is the version, OutBlob[1] the selector-mode tag.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateCodecSelectorPicksSmallerTest,
	"CrowdySDK.State.SelectorPicksSmaller", CrowdyStateCodecTestFlags)
bool FCrowdyStateCodecSelectorPicksSmallerTest::RunTest(const FString& Parameters)
{
	FCrowdyRepLayout Layout;
	TestTrue(TEXT("layout built"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyStateWideTarget::StaticClass(), Layout));
	const int32 N = Layout.Properties.Num();
	TestEqual(TEXT("wide target has 64 replicated properties"), N, 64);

	UCrowdyStateWideTarget* Source = NewObject<UCrowdyStateWideTarget>();
	{
		// Distinct value per slot via the resolved FProperty so this does not depend on member order.
		for (int32 i = 0; i < N; ++i)
		{
			const FProperty* Prop = Layout.Properties[i].Property;
			if (const FIntProperty* IntProp = CastField<FIntProperty>(Prop))
			{
				IntProp->SetPropertyValue_InContainer(Source, WideValueForIndex(i));
			}
		}
	}

	// One dirty bit: index-list must win.
	{
		TBitArray<> Dirty(false, N);
		const int32 One = 5;
		Dirty[One] = true;
		TArray<uint8> Blob;
		FCrowdyStateCodec::Encode(Layout, Source, Dirty, /*bKeyframe=*/false, Blob);
		TestTrue(TEXT("single-bit blob has a header"), Blob.Num() >= 2);
		TestEqual(TEXT("single bit picks index-list (mode 1)"), static_cast<int32>(Blob[1]), 1);

		UCrowdyStateWideTarget* Dest = NewObject<UCrowdyStateWideTarget>();
		TArray<int32> Changed;
		TestTrue(TEXT("single-bit round-trips"),
			FCrowdyStateCodec::Decode(Layout, Layout.LayoutHash, Blob, Dest, Changed));
		TestEqual(TEXT("one changed index"), Changed.Num(), 1);
		if (Changed.Num() == 1)
		{
			TestEqual(TEXT("changed index matches"), Changed[0], One);
			const FIntProperty* IntProp = CastField<FIntProperty>(Layout.Properties[One].Property);
			TestNotNull(TEXT("int property resolved"), IntProp);
			if (IntProp)
			{
				TestEqual(TEXT("value round-tripped"),
					IntProp->GetPropertyValue_InContainer(Dest), WideValueForIndex(One));
			}
		}
	}

	// All 64 bits: bitmask must win (ties favour bitmask, and all-set is the canonical bitmask case).
	{
		TBitArray<> Dirty(true, N);
		TArray<uint8> Blob;
		FCrowdyStateCodec::Encode(Layout, Source, Dirty, /*bKeyframe=*/false, Blob);
		TestTrue(TEXT("all-set blob has a header"), Blob.Num() >= 2);
		TestEqual(TEXT("all bits pick bitmask (mode 0)"), static_cast<int32>(Blob[1]), 0);

		UCrowdyStateWideTarget* Dest = NewObject<UCrowdyStateWideTarget>();
		TArray<int32> Changed;
		TestTrue(TEXT("all-set round-trips"),
			FCrowdyStateCodec::Decode(Layout, Layout.LayoutHash, Blob, Dest, Changed));
		TestEqual(TEXT("all slots changed"), Changed.Num(), N);

		bool bAllValues = true;
		for (int32 i = 0; i < N && bAllValues; ++i)
		{
			const FIntProperty* IntProp = CastField<FIntProperty>(Layout.Properties[i].Property);
			bAllValues = IntProp && IntProp->GetPropertyValue_InContainer(Dest) == WideValueForIndex(i);
		}
		TestTrue(TEXT("every slot round-tripped to its distinct value"), bAllValues);
	}

	return true;
}

// A net-quantized struct round-trips within quantization tolerance via the length-prefixed
// NetSerializeItem branch; a plain FVector with the same fractional components round-trips exactly via
// SerializeItem. Proves encode/decode branch identically on the net-serialized test.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateCodecQuantizedStructsTest,
	"CrowdySDK.State.QuantizedStructs", CrowdyStateCodecTestFlags)
bool FCrowdyStateCodecQuantizedStructsTest::RunTest(const FString& Parameters)
{
	FCrowdyRepLayout Layout;
	TestTrue(TEXT("layout built"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyStateCodecTarget::StaticClass(), Layout));
	const int32 N = Layout.Properties.Num();

	const FVector PlainSent(4.25, -8.5, 16.125);
	const FVector_NetQuantize QuantSent(12.4, -7.8, 3.6);

	UCrowdyStateCodecTarget* Source = NewObject<UCrowdyStateCodecTarget>();
	Source->RepVector = PlainSent;
	Source->RepQuantized = QuantSent;

	TArray<uint8> Blob;
	FCrowdyStateCodec::Encode(Layout, Source, TBitArray<>(true, N), /*bKeyframe=*/true, Blob);

	UCrowdyStateCodecTarget* Dest = NewObject<UCrowdyStateCodecTarget>();
	TArray<int32> Changed;
	const bool bOk = FCrowdyStateCodec::Decode(Layout, Layout.LayoutHash, Blob, Dest, Changed);
	TestTrue(TEXT("decode succeeded"), bOk);

	TestTrue(TEXT("plain FVector round-trips exact"), Dest->RepVector.Equals(PlainSent, 0.0));
	// FVector_NetQuantize rounds each component to the nearest integer (scale 1, 0 decimal places), so a
	// fractional input cannot come back exact  it lands within 0.5. Prove both directions: it is NOT
	// equal at a sub-quantization 0.01 (the rounding really happened) but IS within the 0.5 grid step.
	TestFalse(TEXT("quantized FVector is lossy (not exact at 0.01)"),
		Dest->RepQuantized.Equals(QuantSent, 0.01));
	TestTrue(TEXT("quantized FVector round-trips within quantization tolerance"),
		Dest->RepQuantized.Equals(QuantSent, 0.5));
	return true;
}

// A layout-hash mismatch drops before any blob byte is read, leaving the target's poisoned values
// completely intact.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateCodecMismatchedHashDropsTest,
	"CrowdySDK.State.MismatchedHashDrops", CrowdyStateCodecTestFlags)
bool FCrowdyStateCodecMismatchedHashDropsTest::RunTest(const FString& Parameters)
{
	FCrowdyRepLayout Layout;
	TestTrue(TEXT("layout built"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyStateCodecTarget::StaticClass(), Layout));
	const int32 N = Layout.Properties.Num();

	UCrowdyStateCodecTarget* Source = NewObject<UCrowdyStateCodecTarget>();
	Source->RepInt = 555;
	Source->RepString = TEXT("payload");
	TArray<uint8> Blob;
	FCrowdyStateCodec::Encode(Layout, Source, TBitArray<>(true, N), /*bKeyframe=*/true, Blob);

	// Poison the destination; a correct drop must leave every value untouched.
	UCrowdyStateCodecTarget* Dest = NewObject<UCrowdyStateCodecTarget>();
	Dest->RepInt = 999;
	Dest->RepString = TEXT("poison");

	TArray<int32> Changed;
	const int64 BadHash = Layout.LayoutHash ^ static_cast<int64>(0xABCDEF);
	const bool bOk = FCrowdyStateCodec::Decode(Layout, BadHash, Blob, Dest, Changed);
	TestFalse(TEXT("mismatched hash dropped"), bOk);
	TestEqual(TEXT("no changed indices reported"), Changed.Num(), 0);

	TestEqual(TEXT("poisoned int survives"), Dest->RepInt, 999);
	TestEqual(TEXT("poisoned string survives"), Dest->RepString, FString(TEXT("poison")));
	return true;
}

// Untrusted-input guard: a valid keyframe blob truncated from the end, and the same blob with trailing
// garbage appended, both drop (return false) without crashing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateCodecTruncatedBlobDropsTest,
	"CrowdySDK.State.TruncatedBlobDrops", CrowdyStateCodecTestFlags)
bool FCrowdyStateCodecTruncatedBlobDropsTest::RunTest(const FString& Parameters)
{
	FCrowdyRepLayout Layout;
	TestTrue(TEXT("layout built"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyStateCodecTarget::StaticClass(), Layout));
	const int32 N = Layout.Properties.Num();

	UCrowdyStateCodecTarget* Source = NewObject<UCrowdyStateCodecTarget>();
	Source->RepInt = 3;
	Source->RepString = TEXT("some real content here");
	Source->RepVector = FVector(1.0, 2.0, 3.0);
	Source->RepQuantized = FVector_NetQuantize(4.0, 5.0, 6.0);
	TArray<uint8> Valid;
	FCrowdyStateCodec::Encode(Layout, Source, TBitArray<>(true, N), /*bKeyframe=*/true, Valid);
	TestTrue(TEXT("valid blob has body"), Valid.Num() > 8);

	// Sanity: the untouched valid blob decodes.
	{
		UCrowdyStateCodecTarget* Dest = NewObject<UCrowdyStateCodecTarget>();
		TArray<int32> Changed;
		TestTrue(TEXT("valid blob decodes"),
			FCrowdyStateCodec::Decode(Layout, Layout.LayoutHash, Valid, Dest, Changed));
	}

	// (a) Truncated: drop the last few bytes so a value mid-body runs short.
	{
		TArray<uint8> Truncated = Valid;
		Truncated.SetNum(Truncated.Num() - 4);
		UCrowdyStateCodecTarget* Dest = NewObject<UCrowdyStateCodecTarget>();
		TArray<int32> Changed;
		const bool bOk = FCrowdyStateCodec::Decode(Layout, Layout.LayoutHash, Truncated, Dest, Changed);
		TestFalse(TEXT("truncated blob dropped"), bOk);
	}

	// (b) Trailing garbage: append 3 bytes past a valid body; the trailing-bytes guard must reject it.
	{
		TArray<uint8> Garbaged = Valid;
		Garbaged.Add(0xDE);
		Garbaged.Add(0xAD);
		Garbaged.Add(0xBE);
		UCrowdyStateCodecTarget* Dest = NewObject<UCrowdyStateCodecTarget>();
		TArray<int32> Changed;
		const bool bOk = FCrowdyStateCodec::Decode(Layout, Layout.LayoutHash, Garbaged, Dest, Changed);
		TestFalse(TEXT("trailing-garbage blob dropped"), bOk);
	}

	return true;
}

// Untrusted-input guard: a hand-forged blob whose FString value carries a colossal length prefix must
// drop cleanly (return false) without attempting a multi-GB allocation or crashing. A plain FMemoryReader
// leaves ArMaxSerializeSize == 0, disabling the engine's own "string too large" cap; the codec's bounded
// reader restores it so the forged length is rejected at the length prefix, before any allocation.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateCodecForgedStringLengthDropsTest,
	"CrowdySDK.State.ForgedStringLengthDrops", CrowdyStateCodecTestFlags)
bool FCrowdyStateCodecForgedStringLengthDropsTest::RunTest(const FString& Parameters)
{
	FCrowdyRepLayout Layout;
	TestTrue(TEXT("layout built"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyStateCodecTarget::StaticClass(), Layout));
	const int32 N = Layout.Properties.Num();

	// Resolve the layout index of RepString so the forged blob names exactly that slot.
	int32 StringIndex = INDEX_NONE;
	for (int32 i = 0; i < N; ++i)
	{
		if (Layout.Properties[i].Property && Layout.Properties[i].Property->GetFName() == FName(TEXT("RepString")))
		{
			StringIndex = i;
			break;
		}
	}
	TestNotEqual(TEXT("RepString found in layout"), StringIndex, static_cast<int32>(INDEX_NONE));
	if (StringIndex == INDEX_NONE || StringIndex > 0x7F)
	{
		// The single-byte varint below assumes the index fits in one LEB128 byte, which it does for this
		// fixture; bail defensively rather than emit a malformed index if the layout ever grows past 127.
		return false;
	}

	// Hand-build: [version][index-list mode=1][count=1][index=StringIndex][forged int32 length 0x7FFFFFFF].
	// The forged length is the FString SaveNum a malicious peer would supply to drive AddUninitialized().
	TArray<uint8> Forged;
	Forged.Add(CrowdyStateBlobVersion);
	Forged.Add(1);                                 // selector mode: index-list
	Forged.Add(1);                                 // varint count = 1 (fits one byte)
	Forged.Add(static_cast<uint8>(StringIndex));   // varint index (fits one byte, guarded above)
	Forged.Add(0xFF);                              // int32 length prefix 0x7FFFFFFF, little-endian
	Forged.Add(0xFF);
	Forged.Add(0xFF);
	Forged.Add(0x7F);

	UCrowdyStateCodecTarget* Dest = NewObject<UCrowdyStateCodecTarget>();
	Dest->RepString = TEXT("untouched");
	TArray<int32> Changed;

	// The bounded reader pins ArMaxSerializeSize so the engine's FString load guard rejects the forged
	// length before any allocation. That guard emits a LogCore Error ("String is too large"); an
	// unhandled Error-level log fails the running automation test, so whitelist it here (0 = must fire >=1x).
	AddExpectedError(TEXT("String is too large"), EAutomationExpectedErrorFlags::Contains, 0);

	const bool bOk = FCrowdyStateCodec::Decode(Layout, Layout.LayoutHash, Forged, Dest, Changed);
	TestFalse(TEXT("forged string length dropped, no giant allocation, no crash"), bOk);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
