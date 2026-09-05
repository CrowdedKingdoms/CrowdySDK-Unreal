#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Math/RandomStream.h"
#include "Messages/Channels/FChannelMessages.h"
#include "Serialization/CrowdyActorId.h"
#include "Utils/SerializationFunctionLibrary.h"

#include "CrowdyWireParitySupport.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyActorIdTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	/** 32 octets of text, the form an id takes in practice. */
	FCrowdyActorId TextActorId()
	{
		return FCrowdyActorId::FromAnsiLiteral("0123456789abcdef0123456789abcdef");
	}

	/** 32 octets that are not text: two binary GUIDs, which is what a server-originated event puts there. */
	TArray<uint8> BinaryActorIdOctets()
	{
		TArray<uint8> Octets;
		Octets.Reserve(FCrowdyActorId::NumOctets);
		for (int32 Index = 0; Index < FCrowdyActorId::NumOctets; ++Index)
		{
			Octets.Add(static_cast<uint8>(Index * 8 + 1));
		}

		// A run with no text reading at all: an embedded terminator, and an octet no UTF-8 sequence may start with.
		Octets[3] = 0x00;
		Octets[11] = 0xFF;
		Octets[27] = 0xC0;
		return Octets;
	}
}

// The whole value of a fixed-size id is that it cannot hold a run of any other length, so the parse either
// takes the text whole or refuses it. Truncating or padding would put a value on the wire that addresses a
// different actor while looking perfectly well formed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorIdLengthTest,
	"CrowdySDK.Wire.ActorIdRefusesAnythingButThirtyTwoOctets", CrowdyActorIdTestFlags)
bool FCrowdyActorIdLengthTest::RunTest(const FString& Parameters)
{
	const FString Exact = TEXT("0123456789abcdef0123456789abcdef");

	{
		FCrowdyActorId Id;
		TestTrue(TEXT("32 octets of text are accepted"), FCrowdyActorId::TryFromString(Exact, Id));
		TestTrue(TEXT("an accepted id is set"), Id.IsSet());
		TestEqual(TEXT("and reads back as the text it came from"), Id.ToString(), Exact);
	}

	{
		FCrowdyActorId Id = TextActorId();
		TestFalse(TEXT("31 characters are refused"),
			FCrowdyActorId::TryFromString(Exact.LeftChop(1), Id));
		TestFalse(TEXT("and a refusal leaves the id unset"), Id.IsSet());
	}

	{
		FCrowdyActorId Id = TextActorId();
		TestFalse(TEXT("33 characters are refused"), FCrowdyActorId::TryFromString(Exact + TEXT("0"), Id));
		TestFalse(TEXT("and a refusal leaves the id unset"), Id.IsSet());
	}

	{
		FCrowdyActorId Id = TextActorId();
		TestFalse(TEXT("empty text is refused"), FCrowdyActorId::TryFromString(FString(), Id));
		TestFalse(TEXT("and a refusal leaves the id unset"), Id.IsSet());
	}

	{
		// 32 characters that encode to 33 octets: the wire counts octets, so this is not an id even though
		// it is the right number of characters. This is the case a character-count check accepts.
		const FString OverLong = Exact.LeftChop(1) + FString::Chr(static_cast<TCHAR>(0x00E9));
		TestEqual(TEXT("the over-long value really is 32 characters"), OverLong.Len(), 32);

		FCrowdyActorId Id = TextActorId();
		TestFalse(TEXT("32 characters that encode to 33 octets are refused"),
			FCrowdyActorId::TryFromString(OverLong, Id));
		TestFalse(TEXT("and a refusal leaves the id unset"), Id.IsSet());
	}

	{
		TArray<uint8> Short;
		Short.SetNumZeroed(FCrowdyActorId::NumOctets - 1);
		Short[0] = 0x41;

		FCrowdyActorId Id = TextActorId();
		TestFalse(TEXT("a run of 31 octets is refused"), FCrowdyActorId::TryFromOctets(Short, Id));
		TestFalse(TEXT("and a refusal leaves the id unset"), Id.IsSet());
	}

	{
		FCrowdyActorId Id = TextActorId();
		TestFalse(TEXT("an empty run of octets is refused"),
			FCrowdyActorId::TryFromOctets(TConstArrayView<uint8>(), Id));
		TestFalse(TEXT("and a refusal leaves the id unset"), Id.IsSet());
	}

	return true;
}

// The header writes the id with no length and no padding, so the one thing that must be true whatever the id
// holds is that it occupies the same octets. If it did not, every field behind it would move and the payload
// could no longer be found by offset.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorIdHeaderWidthTest,
	"CrowdySDK.Wire.ActorIdHeaderIsAlwaysThirtyTwoOctets", CrowdyActorIdTestFlags)
bool FCrowdyActorIdHeaderWidthTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Payload = { 0xde, 0xad, 0xbe, 0xef };

	auto BuildMessage = [&Payload](const FCrowdyActorId& ActorId)
	{
		CrowdyWireParity::FParitySpatialMessage Message;
		Message.TypeOverride = ECrowdyMessageType::GENERIC_SPATIAL_1;
		Message.AppID = 7;
		Message.ChunkX = 1;
		Message.ChunkY = -2;
		Message.ChunkZ = 3;
		Message.ReplicationDistance = static_cast<ECrowdyReplicationDistance>(8);
		Message.DecayRate = static_cast<ECrowdyDecayRate>(1);
		Message.bContainsAuth = true;
		Message.UUID = ActorId;
		Message.PayloadBytes = Payload;
		return Message.Serialize();
	};

	const int32 ExpectedLength = CrowdySpatialHeader::Size + Payload.Num();

	const TArray<uint8> WithId = BuildMessage(TextActorId());
	const TArray<uint8> WithoutId = BuildMessage(FCrowdyActorId());
	const TArray<uint8> WithBinaryId = BuildMessage(FCrowdyActorId::FromOctets(BinaryActorIdOctets()));

	TestEqual(TEXT("a message with a text id is the length the layout describes"), WithId.Num(), ExpectedLength);
	TestEqual(TEXT("a message with no id at all is the same length"), WithoutId.Num(), ExpectedLength);
	TestEqual(TEXT("a message with a binary id is the same length"), WithBinaryId.Num(), ExpectedLength);

	// Everything outside the id's own octets has to be untouched by what the id holds, which is the property
	// that keeps the payload findable by offset.
	bool bSurroundingsMatch = true;
	for (int32 Index = 0; Index < ExpectedLength; ++Index)
	{
		if (Index >= CrowdySpatialHeader::ActorIdOffset
			&& Index < CrowdySpatialHeader::ActorIdOffset + FCrowdyActorId::NumOctets)
		{
			continue;
		}

		if (WithId[Index] != WithoutId[Index] || WithId[Index] != WithBinaryId[Index])
		{
			bSurroundingsMatch = false;
			AddError(FString::Printf(
				TEXT("the octet at offset %d changed with the actor id: 0x%02x with an id, 0x%02x without one, ")
				TEXT("0x%02x with a binary one"),
				Index, WithId[Index], WithoutId[Index], WithBinaryId[Index]));
			break;
		}
	}
	TestTrue(TEXT("nothing outside the id's octets depends on the id"), bSurroundingsMatch);

	// And the id's own octets are exactly what was put in.
	TestTrue(TEXT("the header carries the id's octets"),
		FMemory::Memcmp(WithId.GetData() + CrowdySpatialHeader::ActorIdOffset,
			TextActorId().Octets, FCrowdyActorId::NumOctets) == 0);

	return true;
}

// The 32 octets are text by convention only. A server-originated event writes two binary GUIDs there, so the
// value has to survive being carried without being read as text at any point.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorIdBinaryOctetsTest,
	"CrowdySDK.Wire.ActorIdCarriesNonTextOctetsVerbatim", CrowdyActorIdTestFlags)
bool FCrowdyActorIdBinaryOctetsTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Binary = BinaryActorIdOctets();

	{
		const FCrowdyActorId Id = FCrowdyActorId::FromOctets(Binary);

		TArray<uint8> Written;
		Id.AppendTo(Written);

		TestEqual(TEXT("appending an id writes its full width"), Written.Num(), FCrowdyActorId::NumOctets);
		TestTrue(TEXT("a binary id survives a round trip octet for octet"),
			Written == Binary);
	}

	{
		// The same value through the seam a received message meets it at: the sender region of a channel
		// notification, which is the layout whose id is routinely not text.
		TArray<uint8> Frame;
		USerializationFunctionLibrary::AppendValue<int64>(Frame, 42);
		Frame.Append(Binary);
		USerializationFunctionLibrary::AppendValue<uint16>(Frame, 4);
		Frame.Append(TArray<uint8>({ 0x01, 0x02, 0x03, 0x04 }));
		USerializationFunctionLibrary::AppendValue<int64>(Frame, 1700000000000LL);
		Frame.Add(9);

		FChannelMessageNotification Message;
		if (!TestTrue(TEXT("a channel notification with a binary sender decodes"),
			CrowdyWireParity::DecodeStrippedPayload(Message, Frame)))
		{
			return false;
		}

		TestTrue(TEXT("the decoded sender id is the octets that arrived"),
			Message.UUID == FCrowdyActorId::FromOctets(Binary));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorIdEqualityTest,
	"CrowdySDK.Wire.ActorIdEqualityAndHash", CrowdyActorIdTestFlags)
bool FCrowdyActorIdEqualityTest::RunTest(const FString& Parameters)
{
	const FCrowdyActorId First = TextActorId();
	const FCrowdyActorId Same = TextActorId();
	const FCrowdyActorId LastOctetDiffers = FCrowdyActorId::FromAnsiLiteral("0123456789abcdef0123456789abcdee");
	const FCrowdyActorId Unset;

	TestTrue(TEXT("two ids from the same octets are equal"), First == Same);
	TestTrue(TEXT("and hash the same"), GetTypeHash(First) == GetTypeHash(Same));

	TestTrue(TEXT("ids differing in their last octet are unequal"), First != LastOctetDiffers);

	TestTrue(TEXT("an id that was filled in is set"), First.IsSet());
	TestFalse(TEXT("an id that was never filled in is not set"), Unset.IsSet());
	TestTrue(TEXT("an unset id is unequal to a set one"), Unset != First);
	TestEqual(TEXT("and reads back as nothing at all"), Unset.ToString(), FString());

	// An id whose octets are all zero is what "never filled in" looks like, and there is no way to tell it
	// from an id someone deliberately zeroed. The wire has no representation for the difference either.
	FCrowdyActorId Zeroed = First;
	Zeroed.Reset();
	TestFalse(TEXT("resetting an id leaves it unset"), Zeroed.IsSet());
	TestTrue(TEXT("and equal to an id that was never filled in"), Zeroed == Unset);

	return true;
}

// The FGuid is a local lookup key derived from the octets, and every system that indexes actors by it depends
// on the derivation staying exactly what it is. This pins it, including the part of it that is lossy.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorIdGuidDerivationTest,
	"CrowdySDK.Wire.ActorIdGuidDerivationIsUnchanged", CrowdyActorIdTestFlags)
bool FCrowdyActorIdGuidDerivationTest::RunTest(const FString& Parameters)
{
	const FString Text = TEXT("0123456789abcdef0123456789abcdef");
	const FCrowdyActorId Id = TextActorId();

	const FGuid KeyFromOctets = USerializationFunctionLibrary::ToGuid(Id);
	const FGuid KeyFromText = USerializationFunctionLibrary::ToGuid(Text);

	// True of an id whose octets are all ASCII, which is every id written by convention, and only of
	// those. The run where the two readings part company is covered on its own below.
	TestEqual(TEXT("reading the octets and reading the text give the same key"), KeyFromOctets, KeyFromText);

	// The correspondence the entity system relies on: a key written back out as 32 hex digits reads back as
	// the same key.
	TestEqual(TEXT("a key round trips through its own 32 hex digits"),
		USerializationFunctionLibrary::ToGuid(KeyFromOctets.ToString(EGuidFormats::Digits)), KeyFromOctets);

	// Text that is not 32 octets describes no actor, so it derives no key rather than reading past its end.
	TestEqual(TEXT("text too short to be an id derives no key"),
		USerializationFunctionLibrary::ToGuid(FString(TEXT("short"))), FGuid());
	TestEqual(TEXT("empty text derives no key"),
		USerializationFunctionLibrary::ToGuid(FString()), FGuid());

	// The lossy part, pinned deliberately: a component holding anything outside the hexadecimal alphabet reads
	// as the largest value, so two ids that differ only there derive the same key and compare equal. The
	// octets still tell them apart; anything that changes this changes every derived key in the project.
	const FCrowdyActorId NonHexA = FCrowdyActorId::FromAnsiLiteral("ZZZZZZZZ0123456789abcdef01234567");
	const FCrowdyActorId NonHexB = FCrowdyActorId::FromAnsiLiteral("YYYYYYYY0123456789abcdef01234567");

	TestTrue(TEXT("the two ids are genuinely different"), NonHexA != NonHexB);
	TestEqual(TEXT("but a non-hexadecimal component collapses them onto the same key"),
		USerializationFunctionLibrary::ToGuid(NonHexA), USerializationFunctionLibrary::ToGuid(NonHexB));

	return true;
}

// A component is judged after all eight of its octets rather than at the first one outside the alphabet, so
// where the bad octet sits must not change the answer. Only the component holding it may be affected, and it
// must read as the largest value wherever in that component it appears.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorIdGuidRejectsAtAnyPositionTest,
	"CrowdySDK.Wire.ActorIdGuidRejectsANonHexOctetAtAnyPosition", CrowdyActorIdTestFlags)
bool FCrowdyActorIdGuidRejectsAtAnyPositionTest::RunTest(const FString& Parameters)
{
	const FGuid Expected(0x01234567, 0xFFFFFFFF, 0x01234567, 0x89abcdef);

	for (int32 Position = 0; Position < 8; ++Position)
	{
		char Text[FCrowdyActorId::NumOctets + 1] = "0123456789abcdef0123456789abcdef";

		// The second component, so a component either side of it is left hexadecimal and can show that the
		// rejection stayed where it belongs instead of spreading.
		Text[8 + Position] = 'Z';

		const FGuid Key = USerializationFunctionLibrary::ToGuid(FCrowdyActorId::FromAnsiLiteral(Text));

		TestEqual(FString::Printf(TEXT("a non-hexadecimal octet at position %d rejects its whole component"),
			Position), Key, Expected);
	}

	// One octet outside the alphabet in each of two components rejects both, not just the first reached.
	char Both[FCrowdyActorId::NumOctets + 1] = "0123456789abcdef0123456789abcdef";
	Both[3] = 'Z';
	Both[20] = 'Z';

	TestEqual(TEXT("a bad octet in two components rejects both"),
		USerializationFunctionLibrary::ToGuid(FCrowdyActorId::FromAnsiLiteral(Both)),
		FGuid(0xFFFFFFFF, 0x89abcdef, 0xFFFFFFFF, 0x89abcdef));

	return true;
}

// The key is derived from the octets, never from a rendering of them. The two readings agree only while
// every octet is ASCII, so a run that is not text is what says the derivation still reads the octets: an
// id that survives being read as octets is lost entirely by the same value going through text.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorIdGuidFromNonTextOctetsTest,
	"CrowdySDK.Wire.ActorIdGuidDerivationDoesNotGoThroughText", CrowdyActorIdTestFlags)
bool FCrowdyActorIdGuidFromNonTextOctetsTest::RunTest(const FString& Parameters)
{
	// 26 hexadecimal characters, then an octet no UTF-8 sequence may start with and five octets that can
	// only be continuations of one. The key is read in four components of eight octets, so the components
	// here are "01234567", "89abcdef", "01234567", and a last one holding "89" and then those six octets.
	TArray<uint8> Octets;
	Octets.Append(reinterpret_cast<const uint8*>("0123456789abcdef0123456789"), 26);
	Octets.Append({ 0xFF, 0x80, 0x81, 0x82, 0x83, 0x84 });

	if (!TestEqual(TEXT("the vector really is a full-width id"), Octets.Num(), FCrowdyActorId::NumOctets))
	{
		return false;
	}

	const FCrowdyActorId Id = FCrowdyActorId::FromOctets(Octets);

	// The first three components are hexadecimal and read as themselves. The last holds octets outside the
	// hexadecimal alphabet, and a component that is not hexadecimal reads as the largest value there is.
	TestEqual(TEXT("the key comes off the octets as the alphabet describes"),
		USerializationFunctionLibrary::ToGuid(Id), FGuid(0x01234567, 0x89abcdef, 0x01234567, 0xFFFFFFFF));

	// The same octets read as text do not come back. An octet that cannot begin a UTF-8 sequence becomes
	// one replacement character, and the continuation octets behind it are consumed into that same
	// replacement, so the text is shorter than the id it came from and is no longer 32 octets at all.
	const FString AsText = Id.ToString();
	TestTrue(TEXT("text does not carry these octets back"),
		FCrowdyActorId::FromStringOrUnset(AsText) != Id);
	TestFalse(TEXT("so the same id read as text derives no key at all"),
		USerializationFunctionLibrary::ToGuid(AsText).IsValid());

	return true;
}

// The in-place writers exist so a header costs one allocation instead of one per field, so they have to put
// down exactly the octets the allocating version does.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorIdAppendValueTest,
	"CrowdySDK.Wire.AppendValueMatchesSerializeValue", CrowdyActorIdTestFlags)
bool FCrowdyActorIdAppendValueTest::RunTest(const FString& Parameters)
{
	auto CheckValue = [this](const TCHAR* What, auto Value)
	{
		using ValueType = decltype(Value);

		const TArray<uint8> Expected = USerializationFunctionLibrary::SerializeValue<ValueType>(Value);

		TArray<uint8> Appended;
		USerializationFunctionLibrary::AppendValue<ValueType>(Appended, Value);
		TestTrue(FString::Printf(TEXT("AppendValue matches SerializeValue for %s"), What), Appended == Expected);

		TArray<uint8> Written;
		Written.SetNumUninitialized(static_cast<int32>(sizeof(ValueType)));
		USerializationFunctionLibrary::WriteValue<ValueType>(Written.GetData(), Value);
		TestTrue(FString::Printf(TEXT("WriteValue matches SerializeValue for %s"), What), Written == Expected);

		// Appending has to leave what is already there alone, since every message body appends onto its header.
		TArray<uint8> Existing = { 0xAA, 0xBB };
		USerializationFunctionLibrary::AppendValue<ValueType>(Existing, Value);
		TestEqual(FString::Printf(TEXT("appending %s grows the array by its own width"), What),
			Existing.Num(), 2 + static_cast<int32>(sizeof(ValueType)));
		TestTrue(FString::Printf(TEXT("appending %s disturbs nothing before it"), What),
			Existing[0] == 0xAA && Existing[1] == 0xBB);
	};

	CheckValue(TEXT("int64"), static_cast<int64>(1234567890123LL));
	CheckValue(TEXT("a negative int64"), static_cast<int64>(-1234567890123LL));
	CheckValue(TEXT("int32"), static_cast<int32>(1234567));
	CheckValue(TEXT("a negative int32"), static_cast<int32>(-1234567));
	CheckValue(TEXT("int16"), static_cast<int16>(12345));
	CheckValue(TEXT("a negative int16"), static_cast<int16>(-12345));
	CheckValue(TEXT("uint16"), static_cast<uint16>(54321));
	CheckValue(TEXT("uint8"), static_cast<uint8>(200));

	return true;
}

// FromGuid replaces a string build plus a UTF-8 round trip on the send path, so what it owes is not that it
// is reasonable but that it is the SAME 32 octets the string route produced. The expectations here are
// literals, never the string route's own output, so a change to both at once still shows up.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorIdFromGuidTest,
	"CrowdySDK.Wire.ActorIdFromGuidMatchesTheDigitsString", CrowdyActorIdTestFlags)
bool FCrowdyActorIdFromGuidTest::RunTest(const FString& Parameters)
{
	// Every nibble value appears, so a wrong digit table, a swapped word or a reversed nibble order all move
	// at least one octet.
	TestTrue(TEXT("the digits are written in word order, high nibble first, uppercase"),
		FCrowdyActorId::FromGuid(FGuid(0x01234567, 0x89ABCDEF, 0xFEDCBA98, 0x76543210))
			== FCrowdyActorId::FromAnsiLiteral("0123456789ABCDEFFEDCBA9876543210"));

	// The one every event with no named target sends. An unset guid is 32 '0' CHARACTERS, not 32 zero
	// octets, so an id built from it is set; treating it as absent would shorten nothing but would change
	// what the relay reads as the target.
	const FCrowdyActorId FromUnset = FCrowdyActorId::FromGuid(FGuid());
	TestTrue(TEXT("an unset guid writes 32 zero characters"),
		FromUnset == FCrowdyActorId::FromAnsiLiteral("00000000000000000000000000000000"));
	TestTrue(TEXT("which is a set id, since the octets are not zero"), FromUnset.IsSet());

	// A high bit in every word, so a signed shift or a sign-extended word would show.
	TestTrue(TEXT("the top of the range writes all F"),
		FCrowdyActorId::FromGuid(FGuid(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF))
			== FCrowdyActorId::FromAnsiLiteral("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"));

	// The property the wire actually depends on, over values nobody chose: the two routes agree octet for
	// octet, so replacing one with the other moves no byte.
	FRandomStream Stream(20260903);
	for (int32 Trial = 0; Trial < 64; ++Trial)
	{
		const FGuid Value(Stream.GetUnsignedInt(), Stream.GetUnsignedInt(),
			Stream.GetUnsignedInt(), Stream.GetUnsignedInt());

		const FCrowdyActorId Direct = FCrowdyActorId::FromGuid(Value);
		const FCrowdyActorId ViaString = FCrowdyActorId::FromStringOrUnset(Value.ToString(EGuidFormats::Digits));

		if (!TestTrue(*FString::Printf(TEXT("guid %s takes the same octets either way (direct '%s', string '%s')"),
				*Value.ToString(EGuidFormats::Digits), *Direct.ToString(), *ViaString.ToString()),
			Direct == ViaString))
		{
			return false;
		}
	}

	return true;
}

#endif
