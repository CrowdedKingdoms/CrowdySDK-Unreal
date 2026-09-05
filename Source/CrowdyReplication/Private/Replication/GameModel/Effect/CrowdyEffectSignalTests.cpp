// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Replication/GameModel/CrowdyGameModelMetaKeys.h"
#include "Replication/GameModel/CrowdyGameModelSubsystem.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyEffectSignalTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	TArray<uint8> AsciiBytes(const FString& Text)
	{
		TArray<uint8> Bytes;
		Bytes.Reserve(Text.Len());
		for (const TCHAR Char : Text)
		{
			Bytes.Add(static_cast<uint8>(Char));
		}
		return Bytes;
	}
}

// A signal lowers to a channel notification whose payload the receive side can split back apart. This is the pair
// that has to agree: authoring writes the payload, the decoder reads it, and a change to either alone breaks
// signals in a way no compile catches.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSignalRoundTripTest,
	"CrowdySDK.Replication.EffectSignalRoundTrip", CrowdyEffectSignalTestFlags)
bool FCrowdyEffectSignalRoundTripTest::RunTest(const FString& Parameters)
{
	const FCrowdyGameModelNotification Notif = UCrowdyEffect::BuildSignalNotification(TEXT("BossWave"));

	TestEqual(TEXT("signals use the channel carrier"), Notif.Kind, FString(TEXT("channel")));
	if (!TestEqual(TEXT("one payload arg"), Notif.Args.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("arg is the payload"), Notif.Args[0].Name, FString(TEXT("payload")));
	TestEqual(TEXT("payload concatenates the prefixed name with the server-injected container id"),
		Notif.Args[0].Expression, FString(TEXT("concat(\"csg:BossWave:\", $self_container_id)")));

	// What the server produces once it evaluates that expression.
	FString Name;
	FString ContainerId;
	const bool bDecoded = UCrowdyGameModelSubsystem::DecodeChannelSignal(
		AsciiBytes(TEXT("csg:BossWave:abc-123")), Name, ContainerId);

	if (TestTrue(TEXT("the authored payload decodes"), bDecoded))
	{
		TestEqual(TEXT("name round-trips"), Name, FString(TEXT("BossWave")));
		TestEqual(TEXT("container id round-trips"), ContainerId, FString(TEXT("abc-123")));
	}

	TestEqual(TEXT("handler name derives from the signal name"),
		UCrowdyEffect::MakeSignalHandlerName(TEXT("BossWave")), FName(TEXT("OnSignal_BossWave")));
	return true;
}

// The server may base64 the payload, exactly as it may for a model-changed notification, so both encodings decode.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSignalBase64Test,
	"CrowdySDK.Replication.EffectSignalBase64", CrowdyEffectSignalTestFlags)
bool FCrowdyEffectSignalBase64Test::RunTest(const FString& Parameters)
{
	const FString Encoded = FBase64::Encode(TEXT("csg:WaveOver:xyz-9"));

	FString Name;
	FString ContainerId;
	if (TestTrue(TEXT("base64 payload decodes"),
		UCrowdyGameModelSubsystem::DecodeChannelSignal(AsciiBytes(Encoded), Name, ContainerId)))
	{
		TestEqual(TEXT("name"), Name, FString(TEXT("WaveOver")));
		TestEqual(TEXT("container id"), ContainerId, FString(TEXT("xyz-9")));
	}
	return true;
}

// UCrowdyChannels skips Game Model frames so they never reach the reliable-RPC decoder, and it decides that with
// HasGameModelChannelPrefix. That test therefore has to recognize every encoding the two decoders accept: if it
// recognized fewer, a frame would be decoded by the Game Model plane AND mis-decoded as an RPC frame, which is the
// exact failure the skip exists to prevent. This locks the two sides together across both encodings.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelChannelPrefixMatchesDecodeTest,
	"CrowdySDK.Replication.GameModelChannelPrefixMatchesDecode", CrowdyEffectSignalTestFlags)
bool FCrowdyGameModelChannelPrefixMatchesDecodeTest::RunTest(const FString& Parameters)
{
	const FString Payloads[] =
	{
		TEXT("csg:BossWave:abc-123"),
		TEXT("cmc:abc-123"),
	};

	for (const FString& Plain : Payloads)
	{
		FString Name;
		FString Id;
		const TArray<uint8> Raw = AsciiBytes(Plain);
		const TArray<uint8> Encoded = AsciiBytes(FBase64::Encode(Plain));

		const bool bRawDecodes = UCrowdyGameModelSubsystem::DecodeChannelSignal(Raw, Name, Id)
			|| UCrowdyGameModelSubsystem::DecodeChannelModelChangedId(Raw, Id);
		const bool bEncodedDecodes = UCrowdyGameModelSubsystem::DecodeChannelSignal(Encoded, Name, Id)
			|| UCrowdyGameModelSubsystem::DecodeChannelModelChangedId(Encoded, Id);

		TestTrue(FString::Printf(TEXT("'%s' decodes raw"), *Plain), bRawDecodes);
		TestTrue(FString::Printf(TEXT("'%s' decodes base64"), *Plain), bEncodedDecodes);

		TestTrue(FString::Printf(TEXT("the skip test claims raw '%s'"), *Plain),
			CrowdyGameModelMetaKeys::HasGameModelChannelPrefix(Raw));
		TestTrue(FString::Printf(TEXT("the skip test claims base64 '%s'"), *Plain),
			CrowdyGameModelMetaKeys::HasGameModelChannelPrefix(Encoded));
	}

	// The converse: ordinary channel traffic must stay claimable by the RPC decoder, or reliable RPC breaks.
	TestFalse(TEXT("chat is not a Game Model frame"),
		CrowdyGameModelMetaKeys::HasGameModelChannelPrefix(AsciiBytes(TEXT("hello world"))));
	TestFalse(TEXT("an empty payload is not a Game Model frame"),
		CrowdyGameModelMetaKeys::HasGameModelChannelPrefix(TArray<uint8>()));
	TestFalse(TEXT("base64 chat is not a Game Model frame"),
		CrowdyGameModelMetaKeys::HasGameModelChannelPrefix(AsciiBytes(FBase64::Encode(TEXT("hello world")))));

	return true;
}

// The signal decoder and the model-changed decoder share one opcode, so each must refuse the other's frames and
// both must refuse ordinary channel traffic. A cross-claim here would either swallow re-pulls or fire phantom
// signals.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSignalRejectsForeignPayloadsTest,
	"CrowdySDK.Replication.EffectSignalRejectsForeignPayloads", CrowdyEffectSignalTestFlags)
bool FCrowdyEffectSignalRejectsForeignPayloadsTest::RunTest(const FString& Parameters)
{
	FString Name;
	FString ContainerId;

	TestFalse(TEXT("a model-changed frame is not a signal"),
		UCrowdyGameModelSubsystem::DecodeChannelSignal(AsciiBytes(TEXT("cmc:abc-123")), Name, ContainerId));
	TestFalse(TEXT("chat is not a signal"),
		UCrowdyGameModelSubsystem::DecodeChannelSignal(AsciiBytes(TEXT("hello world")), Name, ContainerId));
	TestFalse(TEXT("an empty payload is not a signal"),
		UCrowdyGameModelSubsystem::DecodeChannelSignal(TArray<uint8>(), Name, ContainerId));

	// And the reverse direction: a signal frame must not read as a model-changed re-pull, or every signal would
	// also cost a pull.
	FString ModelChangedId;
	TestFalse(TEXT("a signal frame is not a model-changed notification"),
		UCrowdyGameModelSubsystem::DecodeChannelModelChangedId(
			AsciiBytes(TEXT("csg:BossWave:abc-123")), ModelChangedId));
	return true;
}

// The payload arrives over the network and its name selects a function to call, so a forged frame must not be able
// to name something the authoring side could never produce.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSignalRejectsForgedNamesTest,
	"CrowdySDK.Replication.EffectSignalRejectsForgedNames", CrowdyEffectSignalTestFlags)
bool FCrowdyEffectSignalRejectsForgedNamesTest::RunTest(const FString& Parameters)
{
	FString Name;
	FString ContainerId;

	TestFalse(TEXT("a name with a path separator is refused"),
		UCrowdyGameModelSubsystem::DecodeChannelSignal(AsciiBytes(TEXT("csg:Some/Path:id")), Name, ContainerId));
	TestFalse(TEXT("a name with spaces is refused"),
		UCrowdyGameModelSubsystem::DecodeChannelSignal(AsciiBytes(TEXT("csg:Destroy Actor:id")), Name, ContainerId));
	TestFalse(TEXT("a leading-digit name is refused"),
		UCrowdyGameModelSubsystem::DecodeChannelSignal(AsciiBytes(TEXT("csg:1Bad:id")), Name, ContainerId));
	TestFalse(TEXT("an empty name is refused"),
		UCrowdyGameModelSubsystem::DecodeChannelSignal(AsciiBytes(TEXT("csg::id")), Name, ContainerId));
	TestFalse(TEXT("a missing container id is refused"),
		UCrowdyGameModelSubsystem::DecodeChannelSignal(AsciiBytes(TEXT("csg:BossWave:")), Name, ContainerId));
	TestFalse(TEXT("no separator at all is refused"),
		UCrowdyGameModelSubsystem::DecodeChannelSignal(AsciiBytes(TEXT("csg:BossWave")), Name, ContainerId));
	return true;
}

// A container id may contain colons, so the split takes the FIRST separator after the prefix and leaves the rest
// of the payload intact as the id.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSignalSplitsOnFirstSeparatorTest,
	"CrowdySDK.Replication.EffectSignalSplitsOnFirstSeparator", CrowdyEffectSignalTestFlags)
bool FCrowdyEffectSignalSplitsOnFirstSeparatorTest::RunTest(const FString& Parameters)
{
	FString Name;
	FString ContainerId;
	if (TestTrue(TEXT("decodes"), UCrowdyGameModelSubsystem::DecodeChannelSignal(
		AsciiBytes(TEXT("csg:BossWave:urn:uuid:abc")), Name, ContainerId)))
	{
		TestEqual(TEXT("name stops at the first separator"), Name, FString(TEXT("BossWave")));
		TestEqual(TEXT("the whole remainder is the id"), ContainerId, FString(TEXT("urn:uuid:abc")));
	}
	return true;
}

// Signal names are constrained because they become a function name.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSignalNameValidationTest,
	"CrowdySDK.Replication.EffectSignalNameValidation", CrowdyEffectSignalTestFlags)
bool FCrowdyEffectSignalNameValidationTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("letters"), UCrowdyEffect::IsValidSignalName(TEXT("BossWave")));
	TestTrue(TEXT("underscores and digits after the first character"),
		UCrowdyEffect::IsValidSignalName(TEXT("wave_2_start")));
	TestTrue(TEXT("surrounding whitespace is trimmed, not rejected"),
		UCrowdyEffect::IsValidSignalName(TEXT("  BossWave  ")));

	TestFalse(TEXT("empty"), UCrowdyEffect::IsValidSignalName(TEXT("")));
	TestFalse(TEXT("leading digit"), UCrowdyEffect::IsValidSignalName(TEXT("2Fast")));
	TestFalse(TEXT("separator character"), UCrowdyEffect::IsValidSignalName(TEXT("Boss:Wave")));
	TestFalse(TEXT("space"), UCrowdyEffect::IsValidSignalName(TEXT("Boss Wave")));
	TestFalse(TEXT("punctuation"), UCrowdyEffect::IsValidSignalName(TEXT("Boss-Wave")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
