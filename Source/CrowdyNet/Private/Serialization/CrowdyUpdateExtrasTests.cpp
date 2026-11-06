#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyWireParitySupport.h"
#include "HAL/IConsoleManager.h"
#include "Messages/Actor/CrowdyUpdateExtras.h"
#include "Messages/Actor/FActorUpdateNotificationMessage.h"
#include "Messages/Actor/FActorUpdateRequestMessage.h"
#include "Misc/AutomationTest.h"
#include "Serialization/CrowdyMessagePool.h"
#include "Utils/CrowdyNetActorStateWireTestTypes.h"
#include "Utils/UActorUpdatePayloadRegistry.h"

// Extras ride after the declared state, so the tests below build the frame by hand: a reader that stops at the
// state must decode it exactly as it decodes the same frame without them.
namespace
{
	constexpr EAutomationTestFlags CrowdyUpdateExtrasTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// The same struct and id SerializationFunctionLibraryActorStateTests registers; repeating it is harmless.
	constexpr FCrowdyTypeID ExtrasTestPayloadTypeID = 61234;

	TArray<uint8> MakeExtrasTestState(const int32 A, const int32 B)
	{
		UActorUpdatePayloadRegistry::Get()->RegisterStruct(FCrowdyNetActorStateWireTestPayload::StaticStruct(), ExtrasTestPayloadTypeID);
		TArray<uint8> State;
		AppendActorStateFraming(State, ExtrasTestPayloadTypeID, CrowdyActorStateWireTestClassID);
		State.Append(reinterpret_cast<const uint8*>(&A), sizeof(A));
		State.Append(reinterpret_cast<const uint8*>(&B), sizeof(B));
		return State;
	}

	// The spatial header, the declared state length, the state, whatever follows it, then the timestamp and sequence.
	TArray<uint8> MakeExtrasTestFrame(const TArray<uint8>& State, const TArray<uint8>& Trailing)
	{
		TArray<uint8> Frame;
		const int64 AppId = 7;
		const int64 Chunk = 0;
		Frame.Append(reinterpret_cast<const uint8*>(&AppId), sizeof(AppId));
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			Frame.Append(reinterpret_cast<const uint8*>(&Chunk), sizeof(Chunk));
		}
		Frame.Append({ 8, 0, 0 });
		Frame.Append(reinterpret_cast<const uint8*>(CrowdyWireParity::GoldenUuid()), 32);

		const int32 StateSize = State.Num();
		Frame.Append(reinterpret_cast<const uint8*>(&StateSize), sizeof(StateSize));
		Frame.Append(State);
		Frame.Append(Trailing);

		const int64 Timestamp = 1234;
		Frame.Append(reinterpret_cast<const uint8*>(&Timestamp), sizeof(Timestamp));
		Frame.Add(9);
		return Frame;
	}

	TArray<uint8> MakeProbeExtras(const int64 Nonce)
	{
		TArray<uint8> Extras;
		CrowdyUpdateExtras::AppendContinuousEntry(Extras, 0,
			TConstArrayView<uint8>(reinterpret_cast<const uint8*>(&Nonce), sizeof(Nonce)));
		return Extras;
	}

	bool ExtrasBytesEqual(const TConstArrayView<uint8> A, const TConstArrayView<uint8> B)
	{
		return A.Num() == B.Num() && (A.Num() == 0 || FMemory::Memcmp(A.GetData(), B.GetData(), A.Num()) == 0);
	}

	// Sets crowdy.net.receive.updateextras for one scope and puts the previous value back.
	struct FScopedReceiveExtras
	{
		IConsoleVariable* Variable = nullptr;
		int32 Previous = 1;

		explicit FScopedReceiveExtras(const bool bEnabled)
		{
			Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("crowdy.net.receive.updateextras"));
			if (Variable)
			{
				Previous = Variable->GetInt();
				Variable->Set(bEnabled ? 1 : 0, ECVF_SetByCode);
			}
		}

		~FScopedReceiveExtras()
		{
			if (Variable)
			{
				Variable->Set(Previous, ECVF_SetByCode);
			}
		}
	};

	void TestSameStateAs(FAutomationTestBase& Test, const FActorUpdateNotificationMessage& Plain, const FActorUpdateNotificationMessage& WithExtras)
	{
		Test.TestEqual(TEXT("StateSize"), WithExtras.StateSize, Plain.StateSize);
		// Only the length: the views point into frames that are gone by now.
		Test.TestEqual(TEXT("the state view stops at the declared state"), WithExtras.StateView.Num(), Plain.StateView.Num());
		Test.TestEqual(TEXT("PayloadTypeID"), static_cast<int32>(WithExtras.PayloadTypeID), static_cast<int32>(Plain.PayloadTypeID));
		Test.TestEqual(TEXT("PayloadClassID"), static_cast<int64>(WithExtras.PayloadClassID), static_cast<int64>(Plain.PayloadClassID));
		Test.TestEqual(TEXT("Timestamp"), WithExtras.Timestamp, Plain.Timestamp);
		Test.TestEqual(TEXT("SequenceNumber"), static_cast<int32>(WithExtras.SequenceNumber), static_cast<int32>(Plain.SequenceNumber));

		const FCrowdyNetActorStateWireTestPayload* A = Plain.State.GetPtr<FCrowdyNetActorStateWireTestPayload>();
		const FCrowdyNetActorStateWireTestPayload* B = WithExtras.State.GetPtr<FCrowdyNetActorStateWireTestPayload>();
		if (!Test.TestTrue(TEXT("both decode the state"), A && B))
		{
			return;
		}
		Test.TestEqual(TEXT("state field A"), B->A, A->A);
		Test.TestEqual(TEXT("state field B"), B->B, A->B);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyUpdateExtrasEntriesRoundTripTest,
	"CrowdySDK.Wire.UpdateExtras.EntriesRoundTrip", CrowdyUpdateExtrasTestFlags)
bool FCrowdyUpdateExtrasEntriesRoundTripTest::RunTest(const FString&)
{
	TArray<uint8> Extras;
	const TArray<uint8> Unknown = { 1, 2, 3 };
	const TArray<uint8> Values = { 0xAB, 0xCD };
	TestTrue(TEXT("an entry of a type this build does not know is appended"), CrowdyUpdateExtras::AppendEntry(Extras, 7, Unknown));
	TestTrue(TEXT("a Continuous entry is appended"), CrowdyUpdateExtras::AppendContinuousEntry(Extras, 0x1122334455667788LL, Values));
	TestTrue(TEXT("an empty entry is appended"), CrowdyUpdateExtras::AppendEntry(Extras, 9, {}));
	TestEqual(TEXT("three headers and their bodies"), Extras.Num(), 3 * CrowdyUpdateExtras::EntryHeaderOctets + 3 + 8 + 2);

	TArray<uint16> Types;
	TestTrue(TEXT("they read back as entries"), CrowdyUpdateExtras::ForEachEntry(Extras, [&Types](const uint16 Type, TConstArrayView<uint8>) { Types.Add(Type); }));
	TestTrue(TEXT("in the order they were written"), Types == TArray<uint16>({ 7, CrowdyUpdateExtras::ContinuousType, 9 }));

	int64 LayoutHash = 0;
	TConstArrayView<uint8> ReadValues;
	const TConstArrayView<uint8> Body = CrowdyUpdateExtras::FindEntry(Extras, CrowdyUpdateExtras::ContinuousType);
	TestTrue(TEXT("the Continuous entry splits"), CrowdyUpdateExtras::ReadContinuousEntry(Body, LayoutHash, ReadValues));
	TestEqual(TEXT("its layout hash"), LayoutHash, (int64)0x1122334455667788LL);
	TestTrue(TEXT("its values"), ExtrasBytesEqual(ReadValues, Values));
	TestTrue(TEXT("a type that is absent finds nothing"), CrowdyUpdateExtras::FindEntry(Extras, 42).IsEmpty());

	const int32 Before = Extras.Num();
	TArray<uint8> TooLong;
	TooLong.SetNumZeroed(MAX_uint16 + 1);
	TestFalse(TEXT("type 0 is refused"), CrowdyUpdateExtras::AppendEntry(Extras, 0, Unknown));
	TestFalse(TEXT("a body over 65535 octets is refused"), CrowdyUpdateExtras::AppendEntry(Extras, 7, TooLong));
	TestEqual(TEXT("and a refusal writes nothing"), Extras.Num(), Before);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyUpdateExtrasMalformedTest,
	"CrowdySDK.Wire.UpdateExtras.Malformed", CrowdyUpdateExtrasTestFlags)
bool FCrowdyUpdateExtrasMalformedTest::RunTest(const FString&)
{
	TestTrue(TEXT("no extras is well formed"), CrowdyUpdateExtras::IsWellFormed({}));

	TArray<uint8> Valid;
	CrowdyUpdateExtras::AppendEntry(Valid, 7, TArray<uint8>({ 1, 2, 3 }));
	TestTrue(TEXT("one whole entry is well formed"), CrowdyUpdateExtras::IsWellFormed(Valid));

	TestFalse(TEXT("a header cut short"), CrowdyUpdateExtras::IsWellFormed(TArray<uint8>({ 7, 0, 3 })));
	TestFalse(TEXT("a body one octet short of its length"), CrowdyUpdateExtras::IsWellFormed(TConstArrayView<uint8>(Valid).LeftChop(1)));
	TestFalse(TEXT("a type 0 entry"), CrowdyUpdateExtras::IsWellFormed(TArray<uint8>({ 0, 0, 0, 0 })));
	TestFalse(TEXT("a forged length of 65535 over three octets"), CrowdyUpdateExtras::IsWellFormed(TArray<uint8>({ 7, 0, 0xFF, 0xFF, 1, 2, 3 })));

	TArray<uint8> ThenJunk = Valid;
	ThenJunk.Add(0xFF);
	TestFalse(TEXT("a whole entry followed by a stray octet"), CrowdyUpdateExtras::IsWellFormed(ThenJunk));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyUpdateExtrasFitBesideStateTest,
	"CrowdySDK.Wire.UpdateExtras.FitBesideState", CrowdyUpdateExtrasTestFlags)
bool FCrowdyUpdateExtrasFitBesideStateTest::RunTest(const FString&)
{
	const int32 Room = CrowdyUpdateExtras::MaxPayloadOctets - static_cast<int32>(sizeof(int32)) - 100;
	TestTrue(TEXT("extras that fill the datagram exactly fit"), CrowdyUpdateExtras::FitsBesideState(100, Room));
	TestFalse(TEXT("one octet more does not"), CrowdyUpdateExtras::FitsBesideState(100, Room + 1));
	TestTrue(TEXT("no extras always fit"), CrowdyUpdateExtras::FitsBesideState(CrowdyUpdateExtras::MaxPayloadOctets - 4, 0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyUpdateExtrasRequestSendsThemAfterTheStateTest,
	"CrowdySDK.Wire.UpdateExtras.RequestSendsThemAfterTheState", CrowdyUpdateExtrasTestFlags)
bool FCrowdyUpdateExtrasRequestSendsThemAfterTheStateTest::RunTest(const FString&)
{
	const TArray<uint8> State = MakeExtrasTestState(5, 6);
	const TArray<uint8> Extras = MakeProbeExtras(77);

	FActorUpdateRequestMessage Request;
	Request.StateBytes = State;
	Request.StateSize = State.Num();
	Request.ExtrasBytes = Extras;

	TArray<uint8> Expected;
	const int32 StateSize = State.Num();
	Expected.Append(reinterpret_cast<const uint8*>(&StateSize), sizeof(StateSize));
	Expected.Append(State);
	Expected.Append(Extras);

	const TArray<uint8> Sent = Request.Serialize();
	TestTrue(TEXT("the message ends with the length, the state, then the extras"),
		Sent.Num() >= Expected.Num() && ExtrasBytesEqual(TConstArrayView<uint8>(Sent).Right(Expected.Num()), Expected));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyUpdateExtrasOlderReaderIgnoresThemTest,
	"CrowdySDK.Wire.UpdateExtras.OlderReaderIgnoresThem", CrowdyUpdateExtrasTestFlags)
bool FCrowdyUpdateExtrasOlderReaderIgnoresThemTest::RunTest(const FString&)
{
	const TArray<uint8> State = MakeExtrasTestState(11, 22);
	const TArray<uint8> Extras = MakeProbeExtras(4242);

	FActorUpdateNotificationMessage Plain;
	if (!TestTrue(TEXT("the frame without extras decodes"), CrowdyWireParity::DecodeStrippedPayload(Plain, MakeExtrasTestFrame(State, {}))))
	{
		return false;
	}
	TestTrue(TEXT("and carries none"), Plain.Extras.IsEmpty());

	{
		const FScopedReceiveExtras Older(false);
		FActorUpdateNotificationMessage Message;
		if (!TestTrue(TEXT("an older reader decodes the frame with extras"), CrowdyWireParity::DecodeStrippedPayload(Message, MakeExtrasTestFrame(State, Extras))))
		{
			return false;
		}
		TestSameStateAs(*this, Plain, Message);
		TestTrue(TEXT("and sees no extras"), Message.Extras.IsEmpty());
	}

	FActorUpdateNotificationMessage Message;
	if (!TestTrue(TEXT("a newer reader decodes it too"), CrowdyWireParity::DecodeStrippedPayload(Message, MakeExtrasTestFrame(State, Extras))))
	{
		return false;
	}
	TestSameStateAs(*this, Plain, Message);
	TestTrue(TEXT("and keeps the extras exactly"), ExtrasBytesEqual(Message.Extras, Extras));
	TestTrue(TEXT("the payload bytes a subscriber reads stop at the state"), ExtrasBytesEqual(Message.GetPayloadBytes(), State));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyUpdateExtrasMalformedKeepsTheStateTest,
	"CrowdySDK.Wire.UpdateExtras.MalformedKeepsTheState", CrowdyUpdateExtrasTestFlags)
bool FCrowdyUpdateExtrasMalformedKeepsTheStateTest::RunTest(const FString&)
{
	const TArray<uint8> State = MakeExtrasTestState(3, 4);

	FActorUpdateNotificationMessage Plain;
	if (!TestTrue(TEXT("the frame without extras decodes"), CrowdyWireParity::DecodeStrippedPayload(Plain, MakeExtrasTestFrame(State, {}))))
	{
		return false;
	}

	const int64 MalformedBefore = CrowdyUpdateExtras::GetMalformedCount();
	FActorUpdateNotificationMessage Message;
	if (!TestTrue(TEXT("a frame whose extras do not read as entries still decodes"),
		CrowdyWireParity::DecodeStrippedPayload(Message, MakeExtrasTestFrame(State, TArray<uint8>({ 1, 0, 200 })))))
	{
		return false;
	}
	TestSameStateAs(*this, Plain, Message);
	TestTrue(TEXT("the malformed extras are dropped whole"), Message.Extras.IsEmpty());
	TestEqual(TEXT("and counted once"), CrowdyUpdateExtras::GetMalformedCount(), MalformedBefore + 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyUpdateExtrasPoolReuseClearsThemTest,
	"CrowdySDK.Wire.UpdateExtras.PoolReuseClearsThem", CrowdyUpdateExtrasTestFlags)
bool FCrowdyUpdateExtrasPoolReuseClearsThemTest::RunTest(const FString&)
{
	TCrowdyMessagePool<FActorUpdateNotificationMessage> Pool;
	{
		const TSharedRef<FActorUpdateNotificationMessage, ESPMode::ThreadSafe> First = Pool.Acquire();
		First->Extras = MakeProbeExtras(5);
	}

	const TSharedRef<FActorUpdateNotificationMessage, ESPMode::ThreadSafe> Second = Pool.Acquire();
	TestEqual(TEXT("the slot was reused"), Pool.GetReuseCount(), (int64)1);
	TestTrue(TEXT("and the reused message carries no earlier extras"), Second->Extras.IsEmpty());
	return true;
}

#endif
