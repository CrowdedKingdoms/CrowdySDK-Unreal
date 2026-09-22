#include "Replication/State/CrowdyStateTestTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyReplicationLog.h"
#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "Replication/State/CrowdyStateCodec.h"
#include "Replication/State/FCrowdyRepLayout.h"
#include "Subsystem/CrowdyStateHeartbeatAdvisoryTestTypes.h"
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

	// LEB128, matching the encoder's own varint writer, so a forged blob is malformed only where the test
	// means it to be.
	void AppendVarUInt(TArray<uint8>& Out, uint64 Value)
	{
		do
		{
			uint8 Byte = static_cast<uint8>(Value & 0x7Fu);
			Value >>= 7;
			if (Value != 0)
			{
				Byte |= 0x80u;
			}
			Out.Add(Byte);
		}
		while (Value != 0);
	}

	// A little-endian int32, the framing a memory archive writes for one.
	void AppendInt32(TArray<uint8>& Out, int32 Value)
	{
		const uint32 Bits = static_cast<uint32>(Value);
		Out.Add(static_cast<uint8>(Bits & 0xFFu));
		Out.Add(static_cast<uint8>((Bits >> 8) & 0xFFu));
		Out.Add(static_cast<uint8>((Bits >> 16) & 0xFFu));
		Out.Add(static_cast<uint8>((Bits >> 24) & 0xFFu));
	}

	// The header of an index-list blob naming exactly one slot.
	TArray<uint8> SingleSlotHeader(int32 LayoutIndex)
	{
		TArray<uint8> Blob;
		Blob.Add(CrowdyStateBlobVersion);
		Blob.Add(1);
		AppendVarUInt(Blob, 1);
		AppendVarUInt(Blob, static_cast<uint64>(LayoutIndex));
		return Blob;
	}

	int32 LayoutIndexOf(const FCrowdyRepLayout& Layout, const TCHAR* Name)
	{
		const FName Wanted(Name);
		for (int32 Index = 0; Index < Layout.Properties.Num(); ++Index)
		{
			const FProperty* Prop = Layout.Properties[Index].Property;
			if (Prop && Prop->GetFName() == Wanted)
			{
				return Index;
			}
		}
		return INDEX_NONE;
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

// Every header and selector guard in the decoder, one forged vector each. These are the guards a valid blob
// can never reach, so nothing that round-trips a real delta can see them: each vector below is malformed in
// exactly one place, and each would be ACCEPTED (or would read outside the layout) if its guard went away.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateCodecForgedHeaderDropsTest,
	"CrowdySDK.State.ForgedHeaderAndSelectorDrops", CrowdyStateCodecTestFlags)
bool FCrowdyStateCodecForgedHeaderDropsTest::RunTest(const FString& Parameters)
{
	FCrowdyRepLayout Layout;
	TestTrue(TEXT("layout built"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyStateCodecTarget::StaticClass(), Layout));
	const int32 N = Layout.Properties.Num();
	if (!TestTrue(TEXT("layout is small enough for one-byte index varints"), N > 0 && N <= 0x7F))
	{
		return false;
	}

	const auto Decodes = [&](const TArray<uint8>& Blob) -> bool
	{
		UCrowdyStateCodecTarget* Dest = NewObject<UCrowdyStateCodecTarget>();
		TArray<int32> Changed;
		return FCrowdyStateCodec::Decode(Layout, Layout.LayoutHash, Blob, Dest, Changed);
	};

	// A well-formed single-slot delta, so every refusal below is attributable to the one thing it forges.
	UCrowdyStateCodecTarget* Source = NewObject<UCrowdyStateCodecTarget>();
	Source->RepInt = 4242;
	TBitArray<> Dirty(false, N);
	const int32 IntIndex = LayoutIndexOf(Layout, TEXT("RepInt"));
	if (!TestTrue(TEXT("RepInt is in the layout"), IntIndex != INDEX_NONE))
	{
		return false;
	}
	Dirty[IntIndex] = true;
	TArray<uint8> Valid;
	FCrowdyStateCodec::Encode(Layout, Source, Dirty, /*bKeyframe=*/false, Valid);
	TestTrue(TEXT("the control blob decodes"), Decodes(Valid));

	// A blob written by a peer on the previous body version must drop rather than be read positionally, so
	// bump the version byte and leave everything else alone.
	{
		TArray<uint8> Bad = Valid;
		Bad[0] = static_cast<uint8>(CrowdyStateBlobVersion + 1);
		TestFalse(TEXT("a blob on another body version drops"), Decodes(Bad));
	}

	// A selector mode outside {0, 1} names a framing this build does not have; without the range check it
	// falls into the index-list branch and is misread.
	{
		TArray<uint8> Bad = Valid;
		Bad[1] = 7;
		TestFalse(TEXT("an unknown selector mode drops"), Decodes(Bad));
	}

	// A blob that ends before the selector byte. Read against a layout with no slots there is no bitmask and
	// no value body to trip over afterwards, so this guard is the only thing between it and acceptance.
	{
		FCrowdyRepLayout EmptyLayout;
		EmptyLayout.OwnerClass = UCrowdyStateCodecTarget::StaticClass();
		EmptyLayout.LayoutHash = Layout.LayoutHash;
		TArray<uint8> Bad;
		Bad.Add(CrowdyStateBlobVersion);
		UCrowdyStateCodecTarget* Dest = NewObject<UCrowdyStateCodecTarget>();
		TArray<int32> Changed;
		TestFalse(TEXT("a blob ending before the selector mode drops"),
			FCrowdyStateCodec::Decode(EmptyLayout, EmptyLayout.LayoutHash, Bad, Dest, Changed));
	}

	// A bitmask cut short. The mask is read into a buffer sized from the LOCAL layout, so without the length
	// check the decoder would select slots from bytes it never read.
	{
		FCrowdyRepLayout WideLayout;
		TestTrue(TEXT("wide layout built"),
			FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyStateWideTarget::StaticClass(), WideLayout));
		TArray<uint8> Bad;
		Bad.Add(CrowdyStateBlobVersion);
		Bad.Add(0);
		Bad.Add(0xFF);
		Bad.Add(0xFF);
		Bad.Add(0xFF);
		UCrowdyStateWideTarget* Dest = NewObject<UCrowdyStateWideTarget>();
		TArray<int32> Changed;
		TestFalse(TEXT("a bitmask shorter than the layout drops"),
			FCrowdyStateCodec::Decode(WideLayout, WideLayout.LayoutHash, Bad, Dest, Changed));
	}

	// An index list whose count varint was never written. A truncated varint must be reported as malformed,
	// not read as a count of zero, which would make this an accepted delta carrying nothing.
	{
		TArray<uint8> Bad;
		Bad.Add(CrowdyStateBlobVersion);
		Bad.Add(1);
		TestFalse(TEXT("an index list with no count drops"), Decodes(Bad));
	}

	// An over-long varint: ten continuation bytes and a terminator, which encodes zero in eleven bytes. It is
	// rejected on its WIDTH, so a stream that keeps setting the high bit can never be shifted past the value.
	{
		TArray<uint8> Bad;
		Bad.Add(CrowdyStateBlobVersion);
		Bad.Add(1);
		for (int32 i = 0; i < 10; ++i)
		{
			Bad.Add(0x80);
		}
		Bad.Add(0x00);
		TestFalse(TEXT("an over-long count varint drops"), Decodes(Bad));
	}

	// A forged count far past the layout size. The count sizes the reservation before a single index is read,
	// so it is bounded against the layout rather than trusted.
	{
		TArray<uint8> Bad;
		Bad.Add(CrowdyStateBlobVersion);
		Bad.Add(1);
		AppendVarUInt(Bad, 0x7FFFFFFF);
		TestFalse(TEXT("an index-list count past the layout size drops"), Decodes(Bad));
	}

	// An index one past the last slot: the value loop indexes the layout by it, so this must never be
	// accepted.
	{
		TArray<uint8> Bad;
		Bad.Add(CrowdyStateBlobVersion);
		Bad.Add(1);
		AppendVarUInt(Bad, 1);
		AppendVarUInt(Bad, static_cast<uint64>(N));
		TestFalse(TEXT("an index past the last layout slot drops"), Decodes(Bad));
	}

	// A repeated index, and a descending pair, each carrying the value bytes its selector promises so the
	// body is otherwise well formed and ends exactly where it says it does. Without the ordering rule both
	// are complete, decodable deltas that write one slot twice; with it they are ambiguous and refused. The
	// uniform int32 layout is what lets the value bytes be written without depending on slot order.
	{
		FCrowdyRepLayout WideLayout;
		TestTrue(TEXT("wide layout built"),
			FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyStateWideTarget::StaticClass(), WideLayout));

		const auto WideDecodes = [&](const TArray<uint8>& Blob) -> bool
		{
			UCrowdyStateWideTarget* Dest = NewObject<UCrowdyStateWideTarget>();
			TArray<int32> Changed;
			return FCrowdyStateCodec::Decode(WideLayout, WideLayout.LayoutHash, Blob, Dest, Changed);
		};

		TArray<uint8> Repeated;
		Repeated.Add(CrowdyStateBlobVersion);
		Repeated.Add(1);
		AppendVarUInt(Repeated, 2);
		AppendVarUInt(Repeated, 5);
		AppendVarUInt(Repeated, 5);
		AppendInt32(Repeated, 111);
		AppendInt32(Repeated, 222);
		TestFalse(TEXT("a repeated index drops"), WideDecodes(Repeated));

		TArray<uint8> Descending;
		Descending.Add(CrowdyStateBlobVersion);
		Descending.Add(1);
		AppendVarUInt(Descending, 2);
		AppendVarUInt(Descending, 6);
		AppendVarUInt(Descending, 5);
		AppendInt32(Descending, 111);
		AppendInt32(Descending, 222);
		TestFalse(TEXT("a descending index pair drops"), WideDecodes(Descending));
	}

	// An enum value the sender never wrote. Both reflected shapes of an enum leaf are covered, because they
	// take different decode branches: an enum class owns a separate underlying integer property, while a
	// TEnumAsByte carries the enum on the byte property itself. Either one accepted on a short read would
	// leave the delta looking complete, since the blob ends exactly where the value should have started.
	{
		FCrowdyRepLayout EnumLayout;
		TestTrue(TEXT("enum layout built"), FCrowdyStateLayoutBuilder::BuildLayout(
			UCrowdyStateHeartbeatAdvisoryTarget::StaticClass(), EnumLayout));

		const auto EnumDecodes = [&](int32 SlotIndex) -> bool
		{
			const TArray<uint8> Bad = SingleSlotHeader(SlotIndex);
			UCrowdyStateHeartbeatAdvisoryTarget* Dest = NewObject<UCrowdyStateHeartbeatAdvisoryTarget>();
			TArray<int32> Changed;
			return FCrowdyStateCodec::Decode(EnumLayout, EnumLayout.LayoutHash, Bad, Dest, Changed);
		};

		const int32 StanceIndex = LayoutIndexOf(EnumLayout, TEXT("Stance"));
		const int32 MoodIndex = LayoutIndexOf(EnumLayout, TEXT("Mood"));
		if (TestTrue(TEXT("both enum shapes are in the layout"),
			StanceIndex != INDEX_NONE && MoodIndex != INDEX_NONE))
		{
			TestFalse(TEXT("an enum value that was never written drops"), EnumDecodes(StanceIndex));
			TestFalse(TEXT("a byte-backed enum value that was never written drops"), EnumDecodes(MoodIndex));
		}
	}

	// A layout slot whose property did not resolve. The blob is the valid one above; only the layout the
	// bytes are read against is degraded, which is what a baked table naming a property that has since been
	// renamed produces.
	{
		FCrowdyRepLayout Degraded = Layout;
		Degraded.Properties[IntIndex].Property = nullptr;
		UCrowdyStateCodecTarget* Dest = NewObject<UCrowdyStateCodecTarget>();
		TArray<int32> Changed;
		TestFalse(TEXT("a layout slot with no resolved property drops"),
			FCrowdyStateCodec::Decode(Degraded, Degraded.LayoutHash, Valid, Dest, Changed));
	}

	return true;
}

// The quantized sub-blob's length prefix is the one untrusted length inside the value body, and the
// sub-decoder's own error flag is what catches a sub-blob that promised more than it holds.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateCodecForgedQuantizedLengthDropsTest,
	"CrowdySDK.State.ForgedQuantizedLengthDrops", CrowdyStateCodecTestFlags)
bool FCrowdyStateCodecForgedQuantizedLengthDropsTest::RunTest(const FString& Parameters)
{
	// ReadQuantizedVector NaN-checks an uninitialised vector on the zero sub-blob below; whether it logs depends on the stack.
	AddExpectedError(TEXT("ReadQuantizedVector: Value isn't finite"), EAutomationExpectedErrorFlags::Contains, -1);

	FCrowdyRepLayout Layout;
	TestTrue(TEXT("layout built"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyStateCodecTarget::StaticClass(), Layout));

	const int32 QuantIndex = LayoutIndexOf(Layout, TEXT("RepQuantized"));
	if (!TestTrue(TEXT("RepQuantized is in the layout"), QuantIndex != INDEX_NONE && QuantIndex <= 0x7F))
	{
		return false;
	}

	// Distinctive enough that a value read out of whatever follows the blob in memory cannot reproduce it.
	const FVector_NetQuantize Poison(1234.0, -5678.0, 9012.0);

	const auto DecodeWithPoison = [&](const TArray<uint8>& Blob, FVector_NetQuantize& OutAfter) -> bool
	{
		UCrowdyStateCodecTarget* Dest = NewObject<UCrowdyStateCodecTarget>();
		Dest->RepQuantized = Poison;
		TArray<int32> Changed;
		const bool bOk = FCrowdyStateCodec::Decode(Layout, Layout.LayoutHash, Blob, Dest, Changed);
		OutAfter = Dest->RepQuantized;
		return bOk;
	};

	// A length past the codec's ceiling, with that many bytes actually present, so nothing but the ceiling
	// itself stands between this and a decode that succeeds.
	{
		TArray<uint8> Bad = SingleSlotHeader(QuantIndex);
		const int32 Oversized = 5000;
		AppendInt32(Bad, Oversized);
		Bad.AddZeroed(Oversized);
		FVector_NetQuantize After;
		TestFalse(TEXT("a quantized length past the ceiling drops"), DecodeWithPoison(Bad, After));
	}

	// A length past the bytes that remain. The refusal has to happen BEFORE the read, which is what the
	// surviving poison shows: a decoder that read the promised length first would have written a value.
	{
		TArray<uint8> Bad = SingleSlotHeader(QuantIndex);
		AppendInt32(Bad, 100);
		Bad.AddZeroed(10);
		FVector_NetQuantize After;
		TestFalse(TEXT("a quantized length past the bytes remaining drops"), DecodeWithPoison(Bad, After));
		TestTrue(TEXT("nothing was read into the target for an out-of-range quantized length"),
			After.Equals(Poison, 0.0));
	}

	// A sub-blob shorter than the value it frames. The sub-reader's error flag is the only thing that sees
	// this: the outer stream is intact and ends exactly where it says it does.
	{
		TArray<uint8> Bad = SingleSlotHeader(QuantIndex);
		AppendInt32(Bad, 2);
		Bad.Add(0x00);
		Bad.Add(0x00);
		FVector_NetQuantize After;
		TestFalse(TEXT("a quantized sub-blob too short for its value drops"), DecodeWithPoison(Bad, After));
	}

	return true;
}

// A persistent decode scratch changes no outcome: the same blob decoded with one and without one produces
// the same changed set and the same values, and a scratch previously built for another layout rebuilds
// itself rather than being reused under the wrong slots.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateCodecScratchMatchesUnscratchedTest,
	"CrowdySDK.State.DecodeScratchMatchesUnscratchedDecode", CrowdyStateCodecTestFlags)
bool FCrowdyStateCodecScratchMatchesUnscratchedTest::RunTest(const FString& Parameters)
{
	FCrowdyRepLayout Layout;
	TestTrue(TEXT("layout built"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyStateCodecTarget::StaticClass(), Layout));
	FCrowdyRepLayout OtherLayout;
	TestTrue(TEXT("other layout built"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyStateWideTarget::StaticClass(), OtherLayout));

	const int32 N = Layout.Properties.Num();

	UCrowdyStateCodecTarget* Source = NewObject<UCrowdyStateCodecTarget>();
	Source->RepInt = 42;
	Source->RepBigInt = static_cast<int64>(-9000000001);
	Source->RepFloat = 1.25f;
	Source->RepDouble = -3.5;
	Source->bRepFlag = true;
	Source->RepByte = 200;
	Source->RepEnum = ECrowdyStateTestEnum::Gamma;
	Source->RepName = FName(TEXT("ScratchTag"));
	Source->RepString = TEXT("scratch equivalence");
	Source->RepVector = FVector(1.0, -2.5, 3.25);
	Source->RepRotator = FRotator(10.0, 20.0, 30.0);
	Source->RepQuantized = FVector_NetQuantize(12.0, -7.0, 3.0);

	TArray<uint8> Blob;
	FCrowdyStateCodec::Encode(Layout, Source, TBitArray<>(true, N), /*bKeyframe=*/true, Blob);

	// The scratch is deliberately built for a DIFFERENT layout first, so this also proves it rebuilds itself
	// rather than decoding the wrong slots into the block it happens to be holding.
	FCrowdyStateDecodeScratch Scratch;
	Scratch.EnsureForLayout(OtherLayout, OtherLayout.OwnerClass.Get());
	TestTrue(TEXT("scratch built for the other layout"), Scratch.IsReady());

	UCrowdyStateCodecTarget* Plain = NewObject<UCrowdyStateCodecTarget>();
	TArray<int32> PlainChanged;
	TestTrue(TEXT("unscratched decode succeeded"),
		FCrowdyStateCodec::Decode(Layout, Layout.LayoutHash, Blob, Plain, PlainChanged));

	UCrowdyStateCodecTarget* Scratched = NewObject<UCrowdyStateCodecTarget>();
	TArray<int32> ScratchedChanged;
	TestTrue(TEXT("scratched decode succeeded"),
		FCrowdyStateCodec::Decode(Layout, Layout.LayoutHash, Blob, Scratched, ScratchedChanged,
			/*OutPresentIndices=*/nullptr, &Scratch));

	if (TestEqual(TEXT("the same number of slots is reported changed"),
		ScratchedChanged.Num(), PlainChanged.Num()))
	{
		bool bSameSet = true;
		for (int32 i = 0; i < ScratchedChanged.Num(); ++i)
		{
			bSameSet = bSameSet && ScratchedChanged[i] == PlainChanged[i];
		}
		TestTrue(TEXT("the same slots are reported changed"), bSameSet);
	}
	TestEqual(TEXT("int32 agrees"), Scratched->RepInt, Plain->RepInt);
	TestEqual(TEXT("int64 agrees"), Scratched->RepBigInt, Plain->RepBigInt);
	TestEqual(TEXT("float agrees"), Scratched->RepFloat, Plain->RepFloat);
	TestEqual(TEXT("double agrees"), Scratched->RepDouble, Plain->RepDouble);
	TestEqual(TEXT("bool agrees"), Scratched->bRepFlag, Plain->bRepFlag);
	TestEqual(TEXT("byte agrees"), static_cast<int32>(Scratched->RepByte), static_cast<int32>(Plain->RepByte));
	TestTrue(TEXT("enum agrees"), Scratched->RepEnum == Plain->RepEnum);
	TestTrue(TEXT("name agrees"), Scratched->RepName == Plain->RepName);
	TestEqual(TEXT("string agrees"), Scratched->RepString, Plain->RepString);
	TestTrue(TEXT("plain vector agrees"), Scratched->RepVector.Equals(Plain->RepVector, 0.0));
	TestTrue(TEXT("rotator agrees"), Scratched->RepRotator.Equals(Plain->RepRotator, 0.0));
	TestTrue(TEXT("quantized vector agrees"), Scratched->RepQuantized.Equals(Plain->RepQuantized, 0.0));

	// The bytes really went through the block. Nothing above could tell a decode that used the scratch from
	// one that quietly ignored it, because using it is defined to change no outcome; the slot's own contents
	// are the only place that difference shows.
	{
		const int32 IntIndex = LayoutIndexOf(Layout, TEXT("RepInt"));
		const FIntProperty* IntProp = IntIndex == INDEX_NONE
			? nullptr : CastField<FIntProperty>(Layout.Properties[IntIndex].Property);
		if (TestNotNull(TEXT("RepInt resolved as an int property"), IntProp))
		{
			void* Slot = Scratch.SlotFor(IntIndex);
			if (TestNotNull(TEXT("the scratch has a slot for RepInt"), Slot))
			{
				TestEqual(TEXT("the decoded value passed through the scratch slot"),
					IntProp->GetPropertyValue(Slot), 42);
			}
		}
	}

	// The block holds the values it decoded, and knows which addresses are its own.
	TestTrue(TEXT("a slot address lies inside the block"), Scratch.Contains(Scratch.SlotFor(0)));
	TestFalse(TEXT("an unrelated address lies outside the block"), Scratch.Contains(Scratched));
	TestFalse(TEXT("a null address lies outside the block"), Scratch.Contains(nullptr));
	return true;
}

// A slot in a persistent scratch is returned to the property's default before each decode, so a value the
// sender did not carry reads as the default and never as what the previous delta left in that slot.
// FCrowdyStatePartialNetStruct carries its second member only while the first is positive, so the second
// delta below says nothing about it: the value that lands must be 0, and the slot must therefore report no
// change against a target that is already 0.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateCodecScratchSlotIsResetTest,
	"CrowdySDK.State.DecodeScratchSlotIsResetBetweenDeltas", CrowdyStateCodecTestFlags)
bool FCrowdyStateCodecScratchSlotIsResetTest::RunTest(const FString& Parameters)
{
	FCrowdyRepLayout Layout;
	TestTrue(TEXT("layout built"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyStatePartialNetTarget::StaticClass(), Layout));
	const int32 N = Layout.Properties.Num();
	if (!TestEqual(TEXT("the fixture carries exactly one replicated property"), N, 1))
	{
		return false;
	}

	FCrowdyStateDecodeScratch Scratch;

	// First delta: Wire positive, so Conditional rides the wire and lands in the slot.
	UCrowdyStatePartialNetTarget* FirstSource = NewObject<UCrowdyStatePartialNetTarget>();
	FirstSource->RepPartial.Wire = 5;
	FirstSource->RepPartial.Conditional = 7;
	TArray<uint8> FirstBlob;
	FCrowdyStateCodec::Encode(Layout, FirstSource, TBitArray<>(true, N), /*bKeyframe=*/true, FirstBlob);

	UCrowdyStatePartialNetTarget* FirstDest = NewObject<UCrowdyStatePartialNetTarget>();
	TArray<int32> FirstChanged;
	TestTrue(TEXT("first delta decodes"),
		FCrowdyStateCodec::Decode(Layout, Layout.LayoutHash, FirstBlob, FirstDest, FirstChanged,
			/*OutPresentIndices=*/nullptr, &Scratch));
	TestEqual(TEXT("first delta carried Wire"), FirstDest->RepPartial.Wire, 5);
	TestEqual(TEXT("first delta carried Conditional"), FirstDest->RepPartial.Conditional, 7);

	// Second delta over the SAME scratch: Wire is zero, so Conditional is not carried at all.
	UCrowdyStatePartialNetTarget* SecondSource = NewObject<UCrowdyStatePartialNetTarget>();
	SecondSource->RepPartial.Wire = 0;
	SecondSource->RepPartial.Conditional = 99;
	TArray<uint8> SecondBlob;
	FCrowdyStateCodec::Encode(Layout, SecondSource, TBitArray<>(true, N), /*bKeyframe=*/true, SecondBlob);

	UCrowdyStatePartialNetTarget* SecondDest = NewObject<UCrowdyStatePartialNetTarget>();
	TArray<int32> SecondChanged;
	TestTrue(TEXT("second delta decodes"),
		FCrowdyStateCodec::Decode(Layout, Layout.LayoutHash, SecondBlob, SecondDest, SecondChanged,
			/*OutPresentIndices=*/nullptr, &Scratch));

	TestEqual(TEXT("the uncarried member reads as the default, not as the previous delta's value"),
		SecondDest->RepPartial.Conditional, 0);
	TestEqual(TEXT("the carried member reads as sent"), SecondDest->RepPartial.Wire, 0);
	TestEqual(TEXT("a delta equal to the target's own value reports no change"), SecondChanged.Num(), 0);
	return true;
}

// The block is built for, and validated against, the class it is HANDED, never one it re-derives from the
// layout. Every assertion below is a sentinel the layout's own class could not produce: a null owner that
// must refuse to build, and a foreign owner that must force a rebuild even though the layout is unchanged.
// A block that resolved the layout's class for itself would build in the first case and keep its contents in
// the second, so what is pinned is which class the decision reads, not merely that a block came back.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateCodecScratchUsesGivenOwnerTest,
	"CrowdySDK.State.DecodeScratchUsesTheOwnerItIsGiven", CrowdyStateCodecTestFlags)
bool FCrowdyStateCodecScratchUsesGivenOwnerTest::RunTest(const FString& Parameters)
{
	FCrowdyRepLayout Layout;
	TestTrue(TEXT("layout built"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyStateCodecTarget::StaticClass(), Layout));
	const UClass* Owner = Layout.OwnerClass.Get();
	if (!TestNotNull(TEXT("the layout names a live owner class"), Owner))
	{
		return false;
	}

	const int32 IntIndex = LayoutIndexOf(Layout, TEXT("RepInt"));
	const FIntProperty* IntProp = IntIndex == INDEX_NONE
		? nullptr : CastField<FIntProperty>(Layout.Properties[IntIndex].Property);
	if (!TestNotNull(TEXT("RepInt resolved as an int property"), IntProp))
	{
		return false;
	}

	// No owner, so nothing is built: the layout still names its class, and a block that asked the layout
	// instead of reading this argument would be ready here.
	FCrowdyStateDecodeScratch Scratch;
	Scratch.EnsureForLayout(Layout, nullptr);
	TestFalse(TEXT("a null owner builds no block"), Scratch.IsReady());
	TestFalse(TEXT("and the block describes no layout"), Scratch.DescribesLayout(Layout));

	Scratch.EnsureForLayout(Layout, Owner);
	if (!TestTrue(TEXT("the layout's own owner builds the block"), Scratch.IsReady()))
	{
		return false;
	}
	TestTrue(TEXT("the block describes the layout it was built for"), Scratch.DescribesLayout(Layout));

	// A value written into a slot survives exactly as long as the block does, so it says whether a later call
	// rebuilt: a rebuild constructs every slot afresh and the sentinel is gone.
	void* Slot = Scratch.SlotFor(IntIndex);
	if (!TestNotNull(TEXT("the block has a slot for RepInt"), Slot))
	{
		return false;
	}
	constexpr int32 Sentinel = 0x5EEDBEEF;
	IntProp->SetPropertyValue(Slot, Sentinel);

	Scratch.EnsureForLayout(Layout, Owner);
	TestEqual(TEXT("the same owner keeps the block it already built"),
		IntProp->GetPropertyValue(Scratch.SlotFor(IntIndex)), Sentinel);

	// A different live class with the same layout in hand: the block must rebuild, which only a check that
	// reads the owner it was handed can decide.
	const UClass* Foreign = UCrowdyStateWideTarget::StaticClass();
	Scratch.EnsureForLayout(Layout, Foreign);
	TestTrue(TEXT("a foreign owner leaves a block behind"), Scratch.IsReady());
	TestTrue(TEXT("a foreign owner rebuilds the block rather than reusing it"),
		IntProp->GetPropertyValue(Scratch.SlotFor(IntIndex)) != Sentinel);
	return true;
}

// The delta-decode scope nests inside a scope that is itself measured, so leaving it on costs two
// timestamps per received delta and makes a before-and-after reading of the enclosing scope compare two
// different instrumentations. Its default is the only thing keeping those readings comparable, and a
// default is exactly the kind of value a profiling session flips and forgets.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateDecodeScopeIsOffByDefaultTest,
	"CrowdySDK.State.DecodeScopeIsOffByDefault", CrowdyStateCodecTestFlags)
bool FCrowdyStateDecodeScopeIsOffByDefaultTest::RunTest(const FString& Parameters)
{
	IConsoleVariable* const Cvar = IConsoleManager::Get().FindConsoleVariable(TEXT("crowdy.state.scopes"));

	if (!TestNotNull(TEXT("crowdy.state.scopes exists"), Cvar))
	{
		return false;
	}

	// Nothing in this suite sets it, so what it reads here is the value it was registered with. Asserted
	// through the codec's own accessor as well, so a gate reading a different variable than the one checked
	// above cannot pass.
	TestEqual(TEXT("it reads zero in a session that never set it"), Cvar->GetInt(), 0);
	TestFalse(TEXT("and the decode path sees it off"), CrowdyReplicationProfile::StateScopes());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
