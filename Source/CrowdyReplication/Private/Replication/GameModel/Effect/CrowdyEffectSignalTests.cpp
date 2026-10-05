// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Replication/GameModel/CrowdyAttributeRegistry.h"
#include "Replication/GameModel/CrowdyGameModelMetaKeys.h"
#include "Replication/GameModel/CrowdyGameModelSubsystem.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"
#include "Replication/GameModel/Effect/CrowdyEffectAuthoredSurface.h"

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

	// Distinct helper names so this TU never collides with the surface/lowering test helpers in a unity build.
	FCrowdyEffectVocabulary MakeSignalVocabulary()
	{
		FCrowdyAttributeDef Charge;
		Charge.PropertyName = FName(TEXT("Charge"));
		Charge.Key = TEXT("charge");
		Charge.ValueType = TEXT("int");

		FCrowdyEffectVocabularyType Hero;
		Hero.TypeName = TEXT("SignalHero");
		Hero.Attributes.Add(Charge);

		const TArray<FCrowdyEffectVocabularyType> Types = { Hero };
		return CrowdyEffectAuthoredSurface::ResolveVocabularyFromTypes(Types, TEXT("SignalHero"), FString());
	}

	FCrowdyEffectAuthoredSurface MakeSignalSurface(const FString& Body, const TArray<FString>& SignalNames)
	{
		FCrowdyEffectAuthoredSurface Surface;
		Surface.EffectiveFunctionName = TEXT("signal_case");
		Surface.Source = ECrowdyEffectSource::Text;
		Surface.ScriptText = Body;
		Surface.InvokeScope = TEXT("player");
		Surface.Signals.Reserve(SignalNames.Num());
		for (const FString& Name : SignalNames)
		{
			FCrowdyEffectSignal Signal;
			Signal.Name = Name;
			Surface.Signals.Add(Signal);
		}
		return Surface;
	}

	// How many notifications carry a payload with this prefix. Case-sensitive on purpose: FString::Contains
	// defaults to case-insensitive, which would make "csg:" and "CSG:" the same answer.
	int32 CountSignalPayloads(const FCrowdyGameModelFunctionInput& Fn, const TCHAR* Prefix)
	{
		int32 Count = 0;
		for (const FCrowdyGameModelNotification& Notif : Fn.Notifications)
		{
			for (const FCrowdyGameModelNotificationArg& Arg : Notif.Args)
			{
				if (Arg.Name == TEXT("payload") && Arg.Expression.Contains(Prefix, ESearchCase::CaseSensitive))
				{
					++Count;
				}
			}
		}
		return Count;
	}

	bool HasErrorNaming(const TArray<FCrowdyEffectDiagnostic>& Diagnostics, const TCHAR* Fragment)
	{
		for (const FCrowdyEffectDiagnostic& Diagnostic : Diagnostics)
		{
			if (Diagnostic.Severity == ECrowdyEffectSeverity::Error
				&& Diagnostic.Message.Contains(Fragment, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
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

// UCrowdyChannels skips Game Model frames so they never reach the reliable-RPC decoder, and it decides that with
// HasGameModelChannelPrefix. That check therefore has to recognize every encoding the two decoders accept: if it
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

// A signal-only effect authors exactly one notification, the signal, even with a carrier selected: the model-changed
// hint is gated on the function writing something, and a function that writes nothing cannot have changed the model.
// This pins where signals are appended. Move that loop inside the lowering, or widen the empty-mutations gate, and a
// signal-only effect starts asking every peer to re-pull a container that did not move.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSignalOnlyAuthorsNoModelChangedTest,
	"CrowdySDK.Replication.EffectSignalOnlyAuthorsNoModelChanged", CrowdyEffectSignalTestFlags)
bool FCrowdyEffectSignalOnlyAuthorsNoModelChangedTest::RunTest(const FString& Parameters)
{
	const FCrowdyEffectAuthoredSurface Surface = MakeSignalSurface(FString(), { TEXT("SbxOnly") });
	const TArray<FCrowdyEffectDiagnostic> None;
	const FCrowdyEffectLoweringResult Result = CrowdyEffectAuthoredSurface::Compile(
		Surface, MakeSignalVocabulary(), ECrowdyModelNotificationCarrier::Channel,
		ECrowdyEffectFnCatalog::None, None);

	TestFalse(TEXT("an empty body with one signal compiles"), Result.HasErrors());
	TestEqual(TEXT("a signal-only effect writes nothing"), Result.Function.Mutations.Num(), 0);
	TestEqual(TEXT("exactly one notification is authored"), Result.Function.Notifications.Num(), 1);
	TestEqual(TEXT("and it is the signal"),
		CountSignalPayloads(Result.Function, CrowdyGameModelMetaKeys::SignalChannelPrefix), 1);
	TestEqual(TEXT("no model-changed hint rides with it"),
		CountSignalPayloads(Result.Function, CrowdyGameModelMetaKeys::ModelChangedChannelPrefix), 0);
	return true;
}

// A signal and the model-changed hint share one notifications array, and one upsert has to carry both. This is the
// shape that used to lose one of the two.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSignalBesideMutationsAuthorsBothTest,
	"CrowdySDK.Replication.EffectSignalBesideMutationsAuthorsBoth", CrowdyEffectSignalTestFlags)
bool FCrowdyEffectSignalBesideMutationsAuthorsBothTest::RunTest(const FString& Parameters)
{
	const FCrowdyEffectAuthoredSurface Surface =
		MakeSignalSurface(TEXT("self.charge = self.charge + 1"), { TEXT("SbxWithWrites") });
	const TArray<FCrowdyEffectDiagnostic> None;
	const FCrowdyEffectLoweringResult Result = CrowdyEffectAuthoredSurface::Compile(
		Surface, MakeSignalVocabulary(), ECrowdyModelNotificationCarrier::Channel,
		ECrowdyEffectFnCatalog::None, None);

	TestFalse(TEXT("a body plus a signal compiles"), Result.HasErrors());
	TestEqual(TEXT("the body's one write survives"), Result.Function.Mutations.Num(), 1);
	TestEqual(TEXT("two notifications"), Result.Function.Notifications.Num(), 2);
	TestEqual(TEXT("one of them is the signal"),
		CountSignalPayloads(Result.Function, CrowdyGameModelMetaKeys::SignalChannelPrefix), 1);
	TestEqual(TEXT("the other is the model-changed hint"),
		CountSignalPayloads(Result.Function, CrowdyGameModelMetaKeys::ModelChangedChannelPrefix), 1);
	return true;
}

// Two signals on one effect author one notification each, under their own names. A name-to-notification defect
// shows up here as one name arriving and the other never firing on any client.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSignalPairAuthorsOneEachTest,
	"CrowdySDK.Replication.EffectSignalPairAuthorsOneEach", CrowdyEffectSignalTestFlags)
bool FCrowdyEffectSignalPairAuthorsOneEachTest::RunTest(const FString& Parameters)
{
	const FCrowdyEffectAuthoredSurface Surface =
		MakeSignalSurface(TEXT("self.charge = self.charge + 1"), { TEXT("SbxAlpha"), TEXT("SbxBeta") });
	const TArray<FCrowdyEffectDiagnostic> None;
	const FCrowdyEffectLoweringResult Result = CrowdyEffectAuthoredSurface::Compile(
		Surface, MakeSignalVocabulary(), ECrowdyModelNotificationCarrier::Channel,
		ECrowdyEffectFnCatalog::None, None);

	TestFalse(TEXT("two signals beside a write compile"), Result.HasErrors());
	TestEqual(TEXT("the model-changed hint plus one notification per signal"),
		Result.Function.Notifications.Num(), 3);
	TestEqual(TEXT("two signal payloads"),
		CountSignalPayloads(Result.Function, CrowdyGameModelMetaKeys::SignalChannelPrefix), 2);
	TestEqual(TEXT("SbxAlpha is named exactly once"), CountSignalPayloads(Result.Function, TEXT("csg:SbxAlpha:")), 1);
	TestEqual(TEXT("SbxBeta is named exactly once"), CountSignalPayloads(Result.Function, TEXT("csg:SbxBeta:")), 1);
	return true;
}

// A bad signal name is an authoring-time error, because the name picks the handler function: a clean compile here
// would ship a signal that dispatches to nothing on every remote machine.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectInvalidSignalNameIsRejectedTest,
	"CrowdySDK.Replication.EffectInvalidSignalNameIsRejected", CrowdyEffectSignalTestFlags)
bool FCrowdyEffectInvalidSignalNameIsRejectedTest::RunTest(const FString& Parameters)
{
	const FCrowdyEffectAuthoredSurface Surface =
		MakeSignalSurface(TEXT("self.charge = self.charge + 1"), { TEXT("9bad name") });
	const TArray<FCrowdyEffectDiagnostic> None;
	const FCrowdyEffectLoweringResult Result = CrowdyEffectAuthoredSurface::Compile(
		Surface, MakeSignalVocabulary(), ECrowdyModelNotificationCarrier::Channel,
		ECrowdyEffectFnCatalog::None, None);

	TestTrue(TEXT("the compile reports an error"), Result.HasErrors());
	TestTrue(TEXT("and the error names the signal"), HasErrorNaming(Result.Diagnostics, TEXT("9bad name")));
	TestEqual(TEXT("no signal notification is authored for it"),
		CountSignalPayloads(Result.Function, CrowdyGameModelMetaKeys::SignalChannelPrefix), 0);
	return true;
}

// The same signal twice would author two identical notifications, so every handler would run twice per invocation
// and a counter would read double for a reason nothing names.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectDuplicateSignalIsRejectedTest,
	"CrowdySDK.Replication.EffectDuplicateSignalIsRejected", CrowdyEffectSignalTestFlags)
bool FCrowdyEffectDuplicateSignalIsRejectedTest::RunTest(const FString& Parameters)
{
	const FCrowdyEffectAuthoredSurface Surface =
		MakeSignalSurface(TEXT("self.charge = self.charge + 1"), { TEXT("SbxAlpha"), TEXT("SbxAlpha") });
	const TArray<FCrowdyEffectDiagnostic> None;
	const FCrowdyEffectLoweringResult Result = CrowdyEffectAuthoredSurface::Compile(
		Surface, MakeSignalVocabulary(), ECrowdyModelNotificationCarrier::Channel,
		ECrowdyEffectFnCatalog::None, None);

	TestTrue(TEXT("the compile reports an error"), Result.HasErrors());
	TestTrue(TEXT("and the error names the duplicated signal"),
		HasErrorNaming(Result.Diagnostics, TEXT("declared more than once")));
	TestEqual(TEXT("the duplicate authors nothing, so the name appears once"),
		CountSignalPayloads(Result.Function, TEXT("csg:SbxAlpha:")), 1);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
