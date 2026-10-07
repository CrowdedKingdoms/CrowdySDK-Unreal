#include "CrowdyExecTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyExecCodec.h"
#include "CrowdyExecInternal.h"
#include "CrowdyServerObjectDefinition.h"
#include "Misc/AutomationTest.h"
#include "Misc/OutputDeviceRedirector.h"
#include "NativeGameplayTags.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include <limits>

namespace CrowdyExecCodecTests
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	UE_DEFINE_GAMEPLAY_TAG_STATIC(TagBoss, "CrowdyExec.Test.Boss");

	// Golden bytes: the server code's own encoder produces exactly these for the same values.
	const TCHAR* ScalarsHex = TEXT("88a9624465666561746564c3a64865616c7468cd0f87a54c6576656cfda3426967cf0020000000000001a55370656564ca3fc00000a750726563697365cb3fb999999999999aa44e616d65a447726f6da55068617365a7456e7261676564");
	const TCHAR* ContainersHex = TEXT("84a4486974739301ccc8ce00011170a45461677391a5656c697465ae44616d6167654279506c6179657282a2703119a2703205a542797465739200ccff");
	const TCHAR* EngineHex = TEXT("84a54172656e6183a158cb3ff0000000000000a159cb4000000000000000a15acb4008000000000000a45768656ecf000001998c91f600a24964d9203031323334353637383961626364656630313233343536373839616263646566a54d61796265c0");
	const TCHAR* PushHex = TEXT("83a565706f6368cf000001998c91f600a373657107a66669656c647382a64865616c746800a9624465666561746564c3");
	const TCHAR* ReadHex = TEXT("84a8636f6e747261637401a565706f6368cf000001998c91f600a373657107a66669656c647383a64865616c746800a9624465666561746564c3a55068617365a546696e616c");
	const TCHAR* SnapshotHex = TEXT("82a8636f6e747261637401a5737461746582a64865616c7468cd0fa0a9624465666561746564c2");

	constexpr uint64 FixtureEpoch = 1759000000000ull;

	TArray<uint8> FromHex(const FString& Hex)
	{
		TArray<uint8> Bytes;
		Bytes.SetNumUninitialized(Hex.Len() / 2);
		HexToBytes(Hex, Bytes.GetData());
		return Bytes;
	}

	FString ToHex(TConstArrayView<uint8> Bytes)
	{
		return BytesToHex(Bytes.GetData(), Bytes.Num()).ToLower();
	}

	/** Builds a message by hand, for inputs no encoder would write. */
	struct FMessage
	{
		TArray<uint8> Bytes;
		FCrowdyExecWriter Writer{Bytes};

		FMessage& Map(int32 Count) { Writer.MapHeader(Count); return *this; }
		FMessage& Array(int32 Count) { Writer.ArrayHeader(Count); return *this; }
		FMessage& Key(const ANSICHAR* Text) { Writer.String(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Text), FCStringAnsi::Strlen(Text))); return *this; }
		FMessage& Str(const FString& Text)
		{
			FTCHARToUTF8 Utf8(*Text, Text.Len());
			Writer.String(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()));
			return *this;
		}
		FMessage& UInt(uint64 Value) { Writer.UInt(Value); return *this; }
		FMessage& Int(int64 Value) { Writer.Int(Value); return *this; }
		FMessage& Double(double Value) { Writer.Double(Value); return *this; }
		FMessage& Nil() { Writer.Nil(); return *this; }
		FMessage& Raw(const FString& Hex) { Bytes.Append(FromHex(Hex)); return *this; }
	};

	TStrongObjectPtr<UCrowdyServerObjectDefinition> MakeDefinition(UScriptStruct* State, const TArray<UScriptStruct*>& Params, const TArray<FName>& Watched)
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage()));
		Definition->TypeName = TEXT("test_boss");
		Definition->State = State;
		Definition->WatchedFields = Watched;
		for (UScriptStruct* Struct : Params)
		{
			FCrowdyServerFunction& Function = Definition->Functions.AddDefaulted_GetRef();
			Function.Name = *FString::Printf(TEXT("Call%d"), Definition->Functions.Num());
			Function.Params = Struct;
		}
		return Definition;
	}

	/** One definition that reaches every fixture struct, as a hub's state, params and replies would. */
	TStrongObjectPtr<UCrowdyServerObjectDefinition> MakeFixtureDefinition()
	{
		return MakeDefinition(FCrowdyExecTestBossState::StaticStruct(),
			{FCrowdyExecTestScalars::StaticStruct(), FCrowdyExecTestContainers::StaticStruct(), FCrowdyExecTestEngine::StaticStruct(),
				FCrowdyExecTestExtras::StaticStruct(), FCrowdyExecTestDepth2::StaticStruct(), FCrowdyExecTestHeavyHit::StaticStruct(),
				FCrowdyExecTestKeys::StaticStruct()},
			{TEXT("Health"), TEXT("bDefeated"), TEXT("Phase")});
	}

	/** The eight-link chain nests 17 levels, which a bake refuses, so its tables are written by hand as an older save has them. */
	TStrongObjectPtr<UCrowdyServerObjectDefinition> MakeDeepDefinition()
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(nullptr, {}, {});
		UScriptStruct* const Chain[] = {FCrowdyExecTestDepth8::StaticStruct(), FCrowdyExecTestDepth7::StaticStruct(), FCrowdyExecTestDepth6::StaticStruct(),
			FCrowdyExecTestDepth5::StaticStruct(), FCrowdyExecTestDepth4::StaticStruct(), FCrowdyExecTestDepth3::StaticStruct(),
			FCrowdyExecTestDepth2::StaticStruct(), FCrowdyExecTestDepth1::StaticStruct()};
		for (UScriptStruct* Struct : Chain)
		{
			FCrowdyExecBakedStruct& Baked = Definition->BakedStructs.AddDefaulted_GetRef();
			Baked.Struct = Struct;
			FCrowdyExecBakedField& Field = Baked.Fields.AddDefaulted_GetRef();
			Field.ServerName = TEXT("Items");
			Field.Property = TEXT("Items");
		}
		return Definition;
	}

	/** Eight struct-and-array links with the innermost array empty: sixteen levels. */
	FString DeepestAllowedHex()
	{
		FString Link;
		for (int32 Level = 0; Level < 8; ++Level)
		{
			Link += TEXT("81a54974656d7391");
		}
		return Link.LeftChop(2) + TEXT("90");
	}

	/** Counts the log lines that contain Text, as each is written. */
	struct FLogCounter : public FOutputDevice
	{
		explicit FLogCounter(const FString& InText)
			: Text(InText)
		{
			GLog->AddOutputDevice(this);
		}

		virtual ~FLogCounter() override
		{
			GLog->RemoveOutputDevice(this);
		}

		virtual void Serialize(const TCHAR* Line, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			Count += FCString::Strstr(Line, *Text) ? 1 : 0;
		}

		// Unbuffered, so the count is current as soon as the logging call returns.
		virtual bool CanBeUsedOnMultipleThreads() const override
		{
			return true;
		}

		FString Text;
		int32 Count = 0;
	};

	bool Bake(FAutomationTestBase& Test, UCrowdyServerObjectDefinition& Definition)
	{
		TArray<FString> Errors;
		const bool bBaked = Definition.Bake(Errors);
		Test.TestTrue(FString::Printf(TEXT("the definition bakes (%s)"), *FString::Join(Errors, TEXT("; "))), bBaked);
		return bBaked;
	}

	template <typename T>
	bool DecodeInto(const UCrowdyServerObjectDefinition& Definition, const TArray<uint8>& Bytes, T& Out, FString& OutError)
	{
		return CrowdyExec::Decode(Definition, *T::StaticStruct(), Bytes, &Out, OutError);
	}

	template <typename T>
	TArray<uint8> EncodeOf(FAutomationTestBase& Test, const UCrowdyServerObjectDefinition& Definition, const T& Value, TConstArrayView<FString> OnlyFields = {})
	{
		TArray<uint8> Bytes;
		FString Error;
		const bool bEncoded = CrowdyExec::Encode(Definition, T::StaticStruct(), &Value, Bytes, Error, OnlyFields);
		Test.TestTrue(FString::Printf(TEXT("%s encodes (%s)"), *T::StaticStruct()->GetName(), *Error), bEncoded);
		return Bytes;
	}

	bool ErrorIs(FAutomationTestBase& Test, const FString& Error, const FString& Expected)
	{
		return Test.TestTrue(FString::Printf(TEXT("the error is '%s' (got '%s')"), *Expected, *Error), Error.Equals(Expected, ESearchCase::CaseSensitive));
	}

	/** Decodes a message that must be refused, and checks the destination kept every value it had. */
	template <typename T>
	void ExpectRefusal(FAutomationTestBase& Test, const UCrowdyServerObjectDefinition& Definition, const T& Start, const TArray<uint8>& Bytes, const FString& Expected)
	{
		T Destination = Start;
		FString Error;
		Test.TestFalse(FString::Printf(TEXT("refused: %s"), *Expected), DecodeInto(Definition, Bytes, Destination, Error));
		ErrorIs(Test, Error, Expected);
		Test.TestTrue(FString::Printf(TEXT("the destination is untouched after: %s"), *Expected),
			T::StaticStruct()->CompareScriptStruct(&Destination, &Start, PPF_None));
	}

	FCrowdyExecTestHit MakeHit(int32 Damage, const TCHAR* Source)
	{
		FCrowdyExecTestHit Hit;
		Hit.Damage = Damage;
		Hit.Source = Source;
		return Hit;
	}

	FCrowdyServerFieldName FieldName(UScriptStruct* Struct, const TCHAR* Field, const TCHAR* ServerName)
	{
		FCrowdyServerFieldName Name;
		Name.Struct = Struct;
		Name.Field = Field;
		Name.ServerName = ServerName;
		return Name;
	}

	/** Sixteen levels: every link holds one element, and the innermost array is empty. */
	FCrowdyExecTestDepth8 DeepestAllowed()
	{
		FCrowdyExecTestDepth8 Top;
		FCrowdyExecTestDepth7& L7 = Top.Items.AddDefaulted_GetRef();
		FCrowdyExecTestDepth6& L6 = L7.Items.AddDefaulted_GetRef();
		FCrowdyExecTestDepth5& L5 = L6.Items.AddDefaulted_GetRef();
		FCrowdyExecTestDepth4& L4 = L5.Items.AddDefaulted_GetRef();
		FCrowdyExecTestDepth3& L3 = L4.Items.AddDefaulted_GetRef();
		FCrowdyExecTestDepth2& L2 = L3.Items.AddDefaulted_GetRef();
		L2.Items.AddDefaulted();
		return Top;
	}
}

using namespace CrowdyExecCodecTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecGoldenScalarsTest, "CrowdySDK.CrowdyExec.GoldenScalarsRoundTrip", TestFlags)
bool FCrowdyExecGoldenScalarsTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeFixtureDefinition();
	if (!Bake(*this, *Definition))
	{
		return false;
	}
	FCrowdyExecTestScalars Value;
	FString Error;
	if (!TestTrue(FString::Printf(TEXT("the scalars fixture decodes (%s)"), *Error), DecodeInto(*Definition, FromHex(ScalarsHex), Value, Error)))
	{
		return false;
	}
	TestTrue(TEXT("bDefeated"), Value.bDefeated);
	TestEqual(TEXT("Health"), Value.Health, 3975);
	TestEqual(TEXT("Level"), Value.Level, -3);
	TestEqual(TEXT("Big keeps every bit above 2^53"), Value.Big, 9007199254740993ll);
	TestTrue(TEXT("Speed is exactly 1.5"), Value.Speed == 1.5f);
	TestTrue(TEXT("Precise is exactly 0.1"), Value.Precise == 0.1);
	TestTrue(TEXT("Name is Grom"), Value.Name.Equals(TEXT("Grom"), ESearchCase::CaseSensitive));
	TestEqual(TEXT("Phase is Enraged"), static_cast<int32>(Value.Phase), static_cast<int32>(ECrowdyExecTestPhase::Enraged));
	TestEqual(TEXT("re-encodes to the golden bytes"), ToHex(EncodeOf(*this, *Definition, Value)), FString(ScalarsHex));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecGoldenContainersTest, "CrowdySDK.CrowdyExec.GoldenContainersRoundTrip", TestFlags)
bool FCrowdyExecGoldenContainersTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeFixtureDefinition();
	if (!Bake(*this, *Definition))
	{
		return false;
	}
	FCrowdyExecTestContainers Value;
	FString Error;
	if (!TestTrue(FString::Printf(TEXT("the containers fixture decodes (%s)"), *Error), DecodeInto(*Definition, FromHex(ContainersHex), Value, Error)))
	{
		return false;
	}
	TestTrue(TEXT("Hits"), Value.Hits == TArray<int32>({1, 200, 70000}));
	TestTrue(TEXT("Tags holds elite only"), Value.Tags.Num() == 1 && Value.Tags.Contains(TEXT("elite")));
	TestTrue(TEXT("DamageByPlayer"), Value.DamageByPlayer.Num() == 2 && Value.DamageByPlayer.FindRef(TEXT("p1")) == 25 && Value.DamageByPlayer.FindRef(TEXT("p2")) == 5);
	TestTrue(TEXT("Bytes"), Value.Bytes == TArray<uint8>({0, 255}));

	// Built in reverse order, so matching the fixture proves the encoder sorts map keys.
	FCrowdyExecTestContainers Built;
	Built.Hits = {1, 200, 70000};
	Built.Tags.Add(TEXT("elite"));
	Built.DamageByPlayer.Add(TEXT("p2"), 5);
	Built.DamageByPlayer.Add(TEXT("p1"), 25);
	Built.Bytes = {0, 255};
	TestEqual(TEXT("encodes to the golden bytes"), ToHex(EncodeOf(*this, *Definition, Built)), FString(ContainersHex));

	// Neither insertion order nor its reverse is sorted, so only a real sort passes.
	FCrowdyExecTestContainers Shuffled;
	Shuffled.Tags.Add(TEXT("b"));
	Shuffled.Tags.Add(TEXT("c"));
	Shuffled.Tags.Add(TEXT("a"));
	TestTrue(TEXT("set elements are sorted by their bytes"), ToHex(EncodeOf(*this, *Definition, Shuffled)).Contains(TEXT("a45461677393a161a162a163")));

	FCrowdyExecTestContainers FromBinary;
	const TArray<uint8> Binary = FMessage().Map(1).Key("Bytes").Raw(TEXT("c402007f")).Bytes;
	TestTrue(TEXT("TArray<uint8> also accepts bin"), DecodeInto(*Definition, Binary, FromBinary, Error) && FromBinary.Bytes == TArray<uint8>({0, 127}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecGoldenEngineTest, "CrowdySDK.CrowdyExec.GoldenEngineRoundTrip", TestFlags)
bool FCrowdyExecGoldenEngineTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeFixtureDefinition();
	if (!Bake(*this, *Definition))
	{
		return false;
	}
	FCrowdyExecTestEngine Value;
	Value.Maybe = 5;
	FString Error;
	if (!TestTrue(FString::Printf(TEXT("the engine fixture decodes (%s)"), *Error), DecodeInto(*Definition, FromHex(EngineHex), Value, Error)))
	{
		return false;
	}
	const FGuid Id(0x01234567, 0x89abcdef, 0x01234567, 0x89abcdef);
	TestTrue(TEXT("Arena"), Value.Arena == FVector(1.0, 2.0, 3.0));
	TestEqual(TEXT("When is Unix milliseconds"), Value.When.GetTicks(), static_cast<int64>(FixtureEpoch) * ETimespan::TicksPerMillisecond + 621355968000000000ll);
	TestTrue(TEXT("Id"), Value.Id == Id);
	TestFalse(TEXT("Maybe is unset"), Value.Maybe.IsSet());
	TestEqual(TEXT("re-encodes to the golden bytes"), ToHex(EncodeOf(*this, *Definition, Value)), FString(EngineHex));

	FCrowdyExecTestEngine Hyphenated;
	const TArray<uint8> Bytes = FMessage().Map(1).Key("Id").Str(TEXT("01234567-89AB-CDEF-0123-456789ABCDEF")).Bytes;
	TestTrue(TEXT("a GUID in any case, with hyphens, decodes"), DecodeInto(*Definition, Bytes, Hyphenated, Error) && Hyphenated.Id == Id);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecGoldenStateMessagesTest, "CrowdySDK.CrowdyExec.GoldenStateMessages", TestFlags)
bool FCrowdyExecGoldenStateMessagesTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeFixtureDefinition();
	if (!Bake(*this, *Definition))
	{
		return false;
	}
	const TArray<FString> PushFields = {TEXT("Health"), TEXT("bDefeated")};
	const TArray<FString> ReadFields = {TEXT("Health"), TEXT("bDefeated"), TEXT("Phase")};

	FCrowdyExecTestBossState Pushed;
	Pushed.Phase = ECrowdyExecTestPhase::Enraged;
	FCrowdyExecStateHeader Header;
	TArray<FString> Fields;
	FString Error;
	TestTrue(FString::Printf(TEXT("the push decodes (%s)"), *Error),
		CrowdyExec::DecodeState(*Definition, ECrowdyExecStateMessage::Push, FromHex(PushHex), &Pushed, Header, Fields, Error));
	TestTrue(TEXT("push epoch and seq"), Header.Epoch == FixtureEpoch && Header.Seq == 7);
	TestTrue(TEXT("the push names the two values it carried"), Fields == PushFields);
	TestTrue(TEXT("the push applied its values"), Pushed.Health == 0 && Pushed.bDefeated);
	TestEqual(TEXT("a push leaves the values it does not carry"), static_cast<int32>(Pushed.Phase), static_cast<int32>(ECrowdyExecTestPhase::Enraged));

	FCrowdyExecTestBossState Guarded;
	const TArray<uint8> Unwatched = FMessage().Map(3).Key("epoch").UInt(1).Key("seq").UInt(1).Key("fields").Map(1).Key("Secret").UInt(5).Bytes;
	TestTrue(TEXT("a push naming an unwatched field decodes"),
		CrowdyExec::DecodeState(*Definition, ECrowdyExecStateMessage::Push, Unwatched, &Guarded, Header, Fields, Error));
	TestTrue(TEXT("but never writes it"), Guarded.Secret == 0 && Fields.IsEmpty());

	FMessage Push;
	Push.Map(3).Key("epoch").UInt(FixtureEpoch).Key("seq").UInt(7).Key("fields");
	Push.Bytes.Append(EncodeOf(*this, *Definition, Pushed, PushFields));
	TestEqual(TEXT("the push re-encodes to the golden bytes"), ToHex(Push.Bytes), FString(PushHex));

	FCrowdyExecTestBossState Read;
	Read.Secret = 9;
	TestTrue(FString::Printf(TEXT("the read decodes (%s)"), *Error),
		CrowdyExec::DecodeState(*Definition, ECrowdyExecStateMessage::Read, FromHex(ReadHex), &Read, Header, Fields, Error));
	TestTrue(TEXT("the read names every watched value"), Fields == ReadFields);
	TestTrue(TEXT("the read applied its values"), Read.Health == 0 && Read.bDefeated && Read.Phase == ECrowdyExecTestPhase::Final);
	TestEqual(TEXT("a read replaces the whole view"), Read.Secret, 0);

	FMessage ReadAgain;
	ReadAgain.Map(4).Key("contract").UInt(1).Key("epoch").UInt(FixtureEpoch).Key("seq").UInt(7).Key("fields");
	ReadAgain.Bytes.Append(EncodeOf(*this, *Definition, Read, ReadFields));
	TestEqual(TEXT("the read re-encodes to the golden bytes"), ToHex(ReadAgain.Bytes), FString(ReadHex));

	const TArray<uint8> Snapshot = FromHex(SnapshotHex);
	FCrowdyExecReader Reader(Snapshot);
	FCrowdyExecToken Token;
	const bool bEnvelope = Reader.Next(Token, Error) && Token.Count == 2 && Reader.Next(Token, Error) && Reader.Next(Token, Error) && Token.UInt == 1
		&& Reader.Next(Token, Error);
	FCrowdyExecTestBossState Restored;
	Restored.Health = 1;
	TestTrue(TEXT("the snapshot's state decodes"), bEnvelope
		&& CrowdyExec::Decode(*Definition, *FCrowdyExecTestBossState::StaticStruct(), TConstArrayView<uint8>(Snapshot).RightChop(Reader.Tell()), &Restored, Error));
	TestTrue(TEXT("the snapshot's values"), Restored.Health == 4000 && !Restored.bDefeated);
	FMessage SnapshotAgain;
	SnapshotAgain.Map(2).Key("contract").UInt(1).Key("state");
	SnapshotAgain.Bytes.Append(EncodeOf(*this, *Definition, Restored, PushFields));
	TestEqual(TEXT("the snapshot re-encodes to the golden bytes"), ToHex(SnapshotAgain.Bytes), FString(SnapshotHex));

	TArray<uint8> NoParams;
	TestTrue(TEXT("a function without params sends an empty map"), CrowdyExec::Encode(*Definition, nullptr, nullptr, NoParams, Error) && ToHex(NoParams) == TEXT("80"));

	FCrowdyExecTestBossState Unchanged;
	const TArray<uint8> NewerContract = FMessage().Map(4).Key("contract").UInt(2).Key("epoch").UInt(1).Key("seq").UInt(0).Key("fields").Map(0).Bytes;
	TestFalse(TEXT("a read from server code of another contract is refused"),
		CrowdyExec::DecodeState(*Definition, ECrowdyExecStateMessage::Read, NewerContract, &Unchanged, Header, Fields, Error));
	ErrorIs(*this, Error, TEXT("FCrowdyExecTestBossState: the server code was built for another SDK version (message format 2, this client reads 1); regenerate it and deploy it again"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDecodeAcceptsWiderFormsTest, "CrowdySDK.CrowdyExec.DecodeAcceptsWiderForms", TestFlags)
bool FCrowdyExecDecodeAcceptsWiderFormsTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeFixtureDefinition();
	if (!Bake(*this, *Definition))
	{
		return false;
	}
	FCrowdyExecTestScalars Value;
	FString Error;
	const TArray<uint8> Bytes = FMessage().Map(4).Key("Speed").UInt(2).Key("Precise").Int(-3).Key("Health").Raw(TEXT("cd0005")).Key("Level").Raw(TEXT("d30000000000000007")).Bytes;
	TestTrue(FString::Printf(TEXT("decodes (%s)"), *Error), DecodeInto(*Definition, Bytes, Value, Error));
	TestTrue(TEXT("an integer into a float field is exact"), Value.Speed == 2.f);
	TestTrue(TEXT("a negative integer into a double field is exact"), Value.Precise == -3.0);
	TestEqual(TEXT("uint16 where fixint would do"), Value.Health, 5);
	TestEqual(TEXT("int64 where fixint would do"), Value.Level, 7);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDecodeMissingAndUnknownFieldsTest, "CrowdySDK.CrowdyExec.DecodeMissingAndUnknownFields", TestFlags)
bool FCrowdyExecDecodeMissingAndUnknownFieldsTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeFixtureDefinition();
	if (!Bake(*this, *Definition))
	{
		return false;
	}
	FCrowdyExecTestBossState Value;
	Value.Health = 1;
	FString Error;
	TestTrue(TEXT("an empty map decodes"), DecodeInto(*Definition, FromHex(TEXT("80")), Value, Error));
	TestEqual(TEXT("a missing field takes the constructor's default, not zero or the old value"), Value.Health, 4000);

	const TArray<uint8> Unknown = FMessage().Map(2).Key("Nope").Array(2).UInt(1).Map(1).Key("a").UInt(2).Key("Health").UInt(7).Bytes;
	TestTrue(FString::Printf(TEXT("an unknown field is skipped (%s)"), *Error), DecodeInto(*Definition, Unknown, Value, Error));
	TestEqual(TEXT("the known field after it still decodes"), Value.Health, 7);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDecodeUnknownEnumNameTest, "CrowdySDK.CrowdyExec.DecodeUnknownEnumNameKeepsDefault", TestFlags)
bool FCrowdyExecDecodeUnknownEnumNameTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeFixtureDefinition();
	if (!Bake(*this, *Definition))
	{
		return false;
	}
	// A fresh name per run, since each unknown name is reported once per process.
	CrowdyExec::ResetUnknownEnumReportsForTest();
	const FString Unknown = TEXT("Nope") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	AddExpectedErrorPlain(FString::Printf(TEXT("FCrowdyExecTestScalars.Phase: '%s' is not a value of ECrowdyExecTestPhase"), *Unknown),
		EAutomationExpectedErrorFlags::Contains, 1);
	const TArray<uint8> Bytes = FMessage().Map(2).Key("Phase").Str(Unknown).Key("Health").UInt(3).Bytes;
	for (int32 Attempt = 0; Attempt < 2; ++Attempt)
	{
		FCrowdyExecTestScalars Value;
		Value.Phase = ECrowdyExecTestPhase::Final;
		FString Error;
		TestTrue(FString::Printf(TEXT("a newer server's enum value still decodes (%s)"), *Error), DecodeInto(*Definition, Bytes, Value, Error));
		TestEqual(TEXT("the field takes its default"), static_cast<int32>(Value.Phase), static_cast<int32>(ECrowdyExecTestPhase::Calm));
		TestEqual(TEXT("the rest of the message decodes"), Value.Health, 3);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDecodeRefusalsTest, "CrowdySDK.CrowdyExec.DecodeRefusalsNameTheField", TestFlags)
bool FCrowdyExecDecodeRefusalsTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeFixtureDefinition();
	if (!Bake(*this, *Definition))
	{
		return false;
	}
	FCrowdyExecTestScalars Scalars;
	Scalars.Health = 77;
	Scalars.Name = TEXT("Before");
	FCrowdyExecTestExtras Extras;
	Extras.Small = 9;
	Extras.Notes = {TEXT("kept")};

	ExpectRefusal(*this, *Definition, Scalars, FMessage().Map(1).Key("Health").Nil().Bytes,
		TEXT("FCrowdyExecTestScalars.Health: nil is accepted only for an optional field"));
	ExpectRefusal(*this, *Definition, Scalars, FMessage().Map(1).Key("Health").Double(1.5).Bytes,
		TEXT("FCrowdyExecTestScalars.Health: expected an integer, got a float"));
	ExpectRefusal(*this, *Definition, Scalars, FMessage().Map(1).Key("Speed").Double(std::numeric_limits<double>::quiet_NaN()).Bytes,
		TEXT("FCrowdyExecTestScalars.Speed: NaN and infinity are not accepted"));
	ExpectRefusal(*this, *Definition, Extras, FMessage().Map(1).Key("Small").UInt(300).Bytes,
		TEXT("FCrowdyExecTestExtras.Small: 300 is out of range for uint8"));
	ExpectRefusal(*this, *Definition, Extras, FMessage().Map(1).Key("Tiny").UInt(200).Bytes,
		TEXT("FCrowdyExecTestExtras.Tiny: 200 is out of range for int8"));
	ExpectRefusal(*this, *Definition, Extras, FMessage().Map(1).Key("Huge").Int(-1).Bytes,
		TEXT("FCrowdyExecTestExtras.Huge: -1 is out of range for uint64"));
	ExpectRefusal(*this, *Definition, Extras, FMessage().Map(1).Key("Tag").Str(TEXT("CrowdyExec.Test.Nope")).Bytes,
		TEXT("FCrowdyExecTestExtras.Tag: 'CrowdyExec.Test.Nope' is not a gameplay tag this build knows"));
	ExpectRefusal(*this, *Definition, Extras, FMessage().Map(1).Key("Path").Str(TEXT("not a path")).Bytes,
		TEXT("FCrowdyExecTestExtras.Path: 'not a path' is not an object path"));
	ExpectRefusal(*this, *Definition, Extras, FMessage().Map(1).Key("Mesh").Str(TEXT("not a path")).Bytes,
		TEXT("FCrowdyExecTestExtras.Mesh: 'not a path' is not an object path"));
	ExpectRefusal(*this, *Definition, Extras, FMessage().Map(1).Key("Attacks").Array(4).Map(0).Map(0).Map(0).Map(1).Key("Damage").Double(1.5).Bytes,
		TEXT("FCrowdyExecTestExtras.Attacks[3].Damage: expected an integer, got a float"));
	ExpectRefusal(*this, *Definition, Scalars, FMessage().Map(3).Key("Health").UInt(5).Key("Name").Str(TEXT("After")).Key("Phase").UInt(1).Bytes,
		TEXT("FCrowdyExecTestScalars.Phase: expected a string, got an integer"));
	ExpectRefusal(*this, *Definition, Scalars, FromHex(TEXT("c0")),
		TEXT("FCrowdyExecTestScalars: expected a map, got nil"));
	ExpectRefusal(*this, *Definition, Scalars, FromHex(TEXT("8000")),
		TEXT("FCrowdyExecTestScalars: unexpected bytes after the message"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDecodeLimitsTest, "CrowdySDK.CrowdyExec.DecodeEnforcesLimits", TestFlags)
bool FCrowdyExecDecodeLimitsTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeFixtureDefinition();
	TStrongObjectPtr<UCrowdyServerObjectDefinition> DeepDefinition = MakeDeepDefinition();
	if (!Bake(*this, *Definition))
	{
		return false;
	}
	FString Error;
	FString DeepPath = TEXT("FCrowdyExecTestDepth8");
	for (int32 Level = 0; Level < 8; ++Level)
	{
		DeepPath += TEXT(".Items[0]");
	}
	const FString Allowed = DeepestAllowedHex();
	const FString TooDeep = Allowed.LeftChop(2) + TEXT("9180");
	FCrowdyExecTestDepth8 Deep = DeepestAllowed();
	TestEqual(TEXT("16 levels of nesting encode"), ToHex(EncodeOf(*this, *DeepDefinition, Deep)), Allowed);
	FCrowdyExecTestDepth8 DeepBack;
	TestTrue(FString::Printf(TEXT("16 levels of nesting decode (%s)"), *Error), DecodeInto(*DeepDefinition, FromHex(Allowed), DeepBack, Error));
	ExpectRefusal(*this, *DeepDefinition, FCrowdyExecTestDepth8(), FromHex(TooDeep), DeepPath + TEXT(": nesting deeper than 16 levels"));
	Deep.Items[0].Items[0].Items[0].Items[0].Items[0].Items[0].Items[0].Items.Add(FVector::ZeroVector);
	TArray<uint8> TooDeepBytes;
	TestFalse(TEXT("17 levels of nesting do not encode"), CrowdyExec::Encode(*DeepDefinition, FCrowdyExecTestDepth8::StaticStruct(), &Deep, TooDeepBytes, Error));
	ErrorIs(*this, Error, DeepPath + TEXT(": nesting deeper than 16 levels"));

	auto NestedArrays = [](int32 Count)
	{
		FMessage Message;
		Message.Map(1).Key("Junk");
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Message.Array(1);
		}
		return Message.Nil().Bytes;
	};
	FCrowdyExecTestScalars Scalars;
	TestTrue(FString::Printf(TEXT("an unknown field 16 levels deep is skipped (%s)"), *Error), DecodeInto(*Definition, NestedArrays(15), Scalars, Error));
	ExpectRefusal(*this, *Definition, Scalars, NestedArrays(16), TEXT("FCrowdyExecTestScalars.Junk: nesting deeper than 16 levels"));

	FCrowdyExecTestExtras Extras;
	Extras.Small = 9;
	FMessage Wide;
	Wide.Map(1).Key("Notes").Array(4096);
	for (int32 Index = 0; Index < 4096; ++Index)
	{
		Wide.Str(TEXT(""));
	}
	TestTrue(FString::Printf(TEXT("4,096 elements decode (%s)"), *Error), DecodeInto(*Definition, Wide.Bytes, Extras, Error) && Extras.Notes.Num() == 4096);
	ExpectRefusal(*this, *Definition, Extras, FMessage().Map(1).Key("Notes").Raw(TEXT("dc1001")).Bytes,
		TEXT("FCrowdyExecTestExtras.Notes: a container of 4097 elements, over the limit of 4096"));

	const FString Longest = FString::ChrN(65535, TEXT('x'));
	TestTrue(FString::Printf(TEXT("a 65,535-byte string decodes (%s)"), *Error),
		DecodeInto(*Definition, FMessage().Map(1).Key("Notes").Array(1).Str(Longest).Bytes, Extras, Error) && Extras.Notes[0].Len() == 65535);
	ExpectRefusal(*this, *Definition, Extras, FMessage().Map(1).Key("Notes").Array(1).Raw(TEXT("db00010000")).Bytes,
		TEXT("FCrowdyExecTestExtras.Notes[0]: a string of 65536 bytes, over the limit of 65535"));
	ExpectRefusal(*this, *Definition, Extras, FMessage().Map(1).Key("Small").Raw(TEXT("d40000")).Bytes,
		TEXT("FCrowdyExecTestExtras.Small: an ext value, which is not accepted"));
	ExpectRefusal(*this, *Definition, Extras, FMessage().Map(1).Key("Notes").Array(2).Str(TEXT("a")).Bytes,
		TEXT("FCrowdyExecTestExtras.Notes[1]: the message ends early"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDecodeAllOrNothingTest, "CrowdySDK.CrowdyExec.DecodeIsAllOrNothing", TestFlags)
bool FCrowdyExecDecodeAllOrNothingTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeFixtureDefinition();
	if (!Bake(*this, *Definition))
	{
		return false;
	}
	FCrowdyExecTestScalars Value;
	Value.Health = 77;
	Value.Name = TEXT("Before");
	const TArray<uint8> Bytes = FMessage().Map(3).Key("Health").UInt(5).Key("Name").Str(TEXT("After")).Key("Phase").UInt(1).Bytes;
	FString Error;
	TestFalse(TEXT("a bad last field refuses the whole message"), DecodeInto(*Definition, Bytes, Value, Error));
	TestEqual(TEXT("a field decoded before the error is not applied"), Value.Health, 77);
	TestTrue(TEXT("nor is a string decoded before it"), Value.Name.Equals(TEXT("Before"), ESearchCase::CaseSensitive));

	FCrowdyExecTestBossState State;
	State.Health = 55;
	FCrowdyExecStateHeader Header;
	TArray<FString> Fields;
	const TArray<uint8> Push = FMessage().Map(3).Key("epoch").UInt(1).Key("fields").Map(2).Key("Health").UInt(3).Key("Phase").UInt(9).Key("seq").UInt(2).Bytes;
	TestFalse(TEXT("a push with a bad value is refused"), CrowdyExec::DecodeState(*Definition, ECrowdyExecStateMessage::Push, Push, &State, Header, Fields, Error));
	TestEqual(TEXT("and changes nothing"), State.Health, 55);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecEncodeRefusalsTest, "CrowdySDK.CrowdyExec.EncodeRefusalsNameTheField", TestFlags)
bool FCrowdyExecEncodeRefusalsTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeFixtureDefinition();
	if (!Bake(*this, *Definition))
	{
		return false;
	}
	auto ExpectEncodeRefusal = [this, &Definition](const UScriptStruct* Struct, const void* Value, const FString& Expected, TConstArrayView<FString> OnlyFields = {})
	{
		TArray<uint8> Bytes = {1};
		FString Error;
		TestFalse(FString::Printf(TEXT("refused: %s"), *Expected), CrowdyExec::Encode(*Definition, Struct, Value, Bytes, Error, OnlyFields));
		TestTrue(TEXT("nothing is left in the output"), Bytes.IsEmpty());
		return Error;
	};

	FCrowdyExecTestScalars Scalars;
	Scalars.Speed = std::numeric_limits<float>::infinity();
	ErrorIs(*this, ExpectEncodeRefusal(FCrowdyExecTestScalars::StaticStruct(), &Scalars, TEXT("infinity")),
		TEXT("FCrowdyExecTestScalars.Speed: NaN and infinity are not accepted"));
	Scalars.Speed = 0.f;
	Scalars.Phase = static_cast<ECrowdyExecTestPhase>(7);
	ErrorIs(*this, ExpectEncodeRefusal(FCrowdyExecTestScalars::StaticStruct(), &Scalars, TEXT("unnamed enum value")),
		TEXT("FCrowdyExecTestScalars.Phase: 7 is not a named value of ECrowdyExecTestPhase"));

	FCrowdyExecTestExtras Extras;
	Extras.Notes = {TEXT("fine"), FString::ChrN(65536, TEXT('x'))};
	ErrorIs(*this, ExpectEncodeRefusal(FCrowdyExecTestExtras::StaticStruct(), &Extras, TEXT("long string")),
		TEXT("FCrowdyExecTestExtras.Notes[1]: a string of 65536 bytes, over the limit of 65535"));
	Extras.Notes.Init(TEXT("x"), 4097);
	ErrorIs(*this, ExpectEncodeRefusal(FCrowdyExecTestExtras::StaticStruct(), &Extras, TEXT("wide array")),
		TEXT("FCrowdyExecTestExtras.Notes: 4097 elements, over the limit of 4096"));
	Extras.Notes.Init(FString::ChrN(65000, TEXT('x')), 17);
	const FString Oversized = ExpectEncodeRefusal(FCrowdyExecTestExtras::StaticStruct(), &Extras, TEXT("oversized payload"));
	TestTrue(FString::Printf(TEXT("the payload limit is named (%s)"), *Oversized),
		Oversized.StartsWith(TEXT("FCrowdyExecTestExtras: the message is ")) && Oversized.EndsWith(TEXT("bytes, over the limit of 1048576")));

	FCrowdyExecTestBossState State;
	const TArray<FString> Typo = {TEXT("health")};
	ErrorIs(*this, ExpectEncodeRefusal(FCrowdyExecTestBossState::StaticStruct(), &State, TEXT("unknown field"), Typo),
		TEXT("FCrowdyExecTestBossState: there is no field named 'health'"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecEngineTypesTest, "CrowdySDK.CrowdyExec.EngineTypesRoundTrip", TestFlags)
bool FCrowdyExecEngineTypesTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeFixtureDefinition();
	if (!Bake(*this, *Definition))
	{
		return false;
	}
	FCrowdyExecTestExtras Value;
	Value.Small = 255;
	Value.Tiny = -128;
	Value.Huge = MAX_uint64;
	Value.Rotation = FRotator(10.0, 20.0, 30.0);
	Value.Orientation = FQuat(0.5, 0.5, 0.5, 0.5);
	Value.Tint = FLinearColor(0.25f, 0.5f, 0.75f, 1.f);
	Value.Color = FColor(1, 2, 3, 4);
	Value.Cell = FIntPoint(3, -4);
	Value.Block = FIntVector(1, 2, 3);
	Value.Offset = FVector2D(1.5, -2.5);
	Value.Cooldown = FTimespan::FromMilliseconds(1500.0);
	Value.Tag = TagBoss;
	Value.Mesh = TSoftObjectPtr<UObject>(FSoftObjectPath(TEXT("/Game/Maps/Arena.Arena")));
	Value.Class = TSoftClassPtr<UObject>(FSoftObjectPath(TEXT("/Game/BP_Boss.BP_Boss_C")));
	Value.Path = FSoftObjectPath(TEXT("/Game/Sounds/Roar.Roar"));
	Value.ByIndex.Add(10, TEXT("b"));
	Value.ByIndex.Add(-1, TEXT("a"));
	Value.ByIndex.Add(3, TEXT("c"));
	Value.Phases.Add(ECrowdyExecTestPhase::Final);
	Value.Phases.Add(ECrowdyExecTestPhase::Calm);
	Value.Attacks = {MakeHit(12, TEXT("Axe")), MakeHit(7, TEXT("Bow"))};
	Value.Notes = {FString(TEXT("h")) + static_cast<TCHAR>(0x00E9) + TEXT("llo")};

	const TArray<uint8> Bytes = EncodeOf(*this, *Definition, Value);
	const FString Hex = ToHex(Bytes);
	TestTrue(TEXT("FColor travels R, G, B, A"), Hex.Contains(TEXT("a5436f6c6f7284a15201a14702a14203a14104")));
	TestTrue(TEXT("integer map keys are sorted by value"), Hex.Contains(TEXT("a74279496e64657883ffa16103a1630aa162")));
	TestTrue(TEXT("enum set elements are sorted in declaration order"), Hex.Contains(TEXT("a650686173657392a443616c6da546696e616c")));
	TestTrue(TEXT("a timespan is milliseconds"), Hex.Contains(TEXT("a8436f6f6c646f776ecd05dc")));
	TestTrue(TEXT("a tag is its name"), Hex.Contains(TEXT("a3546167b443726f776479457865632e546573742e426f7373")));

	FCrowdyExecTestExtras Decoded;
	FString Error;
	TestTrue(FString::Printf(TEXT("decodes (%s)"), *Error), DecodeInto(*Definition, Bytes, Decoded, Error));
	TestTrue(TEXT("every engine type round-trips exactly"), FCrowdyExecTestExtras::StaticStruct()->CompareScriptStruct(&Decoded, &Value, PPF_None));
	TestTrue(TEXT("the tag survives"), Decoded.Tag == TagBoss.GetTag());
	TestTrue(TEXT("the soft class path survives"), Decoded.Class.ToSoftObjectPath() == FSoftObjectPath(TEXT("/Game/BP_Boss.BP_Boss_C")));

	FCrowdyExecTestEngine Dated;
	Dated.When = FDateTime(621355968000000000ll + 12345678);
	FCrowdyExecTestEngine DatedBack;
	TestTrue(TEXT("a date decodes"), DecodeInto(*Definition, EncodeOf(*this, *Definition, Dated), DatedBack, Error));
	TestEqual(TEXT("a date keeps whole milliseconds only"), DatedBack.When.GetTicks(), 621355968000000000ll + 12340000);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDefinitionRefusesTypesTest, "CrowdySDK.CrowdyExec.DefinitionRefusesTypes", TestFlags)
bool FCrowdyExecDefinitionRefusesTypesTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(FCrowdyExecTestRefused::StaticStruct(), {}, {});
	TArray<FString> Errors;
	TestFalse(TEXT("a struct with unsupported fields does not bake"), Definition->Bake(Errors));
	const TArray<FString> Expected = {
		TEXT("FCrowdyExecTestRefused.Target: object references are not supported; use a soft reference"),
		TEXT("FCrowdyExecTestRefused.Label: text is not supported; use FString"),
		TEXT("FCrowdyExecTestRefused.Payload: FInstancedStruct is not supported"),
		TEXT("FCrowdyExecTestRefused.Fixed: fixed-size arrays are not supported; use TArray"),
		TEXT("FCrowdyExecTestRefused.Weak: object references are not supported; use a soft reference"),
		TEXT("FCrowdyExecTestRefused.Objects: object references are not supported; use a soft reference"),
		TEXT("FCrowdyExecTestRefused.Ratios: set elements and map keys must be strings, names, integers or enums"),
		TEXT("FCrowdyExecTestRefused.TagSet: FGameplayTagContainer is not supported; use TArray<FGameplayTag>")};
	TestEqual(TEXT("one error per refused field, none for the supported one"), Errors.Num(), Expected.Num());
	for (const FString& Line : Expected)
	{
		TestTrue(FString::Printf(TEXT("reported: %s (got: %s)"), *Line, *FString::Join(Errors, TEXT(" | "))),
			Errors.ContainsByPredicate([&Line](const FString& Candidate) { return Candidate.Equals(Line, ESearchCase::CaseSensitive); }));
	}
	TestTrue(TEXT("a failed bake leaves no tables"), Definition->BakedStructs.IsEmpty() && Definition->BakedEnums.IsEmpty());

	FCrowdyExecTestRefused Value;
	TArray<uint8> Bytes;
	FString Error;
	TestFalse(TEXT("an unbaked definition encodes nothing"), CrowdyExec::Encode(*Definition, FCrowdyExecTestRefused::StaticStruct(), &Value, Bytes, Error));
	ErrorIs(*this, Error, FString::Printf(TEXT("%s has no field tables; fix the problems it reports and save it"), *Definition->GetName()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDefinitionNamesTest, "CrowdySDK.CrowdyExec.DefinitionChecksNames", TestFlags)
bool FCrowdyExecDefinitionNamesTest::RunTest(const FString& Parameters)
{
	auto MethodOf = [](const TCHAR* Name)
	{
		FCrowdyServerFunction Function;
		Function.Name = Name;
		return Function.GetMethodName();
	};
	TestEqual(TEXT("AttackBoss"), MethodOf(TEXT("AttackBoss")), FString(TEXT("attack_boss")));
	TestEqual(TEXT("HTTPGet"), MethodOf(TEXT("HTTPGet")), FString(TEXT("http_get")));
	TestEqual(TEXT("Hit2Boss"), MethodOf(TEXT("Hit2Boss")), FString(TEXT("hit2_boss")));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Broken = MakeDefinition(FCrowdyExecTestBossState::StaticStruct(), {FCrowdyExecTestScalars::StaticStruct()}, {TEXT("Health"), TEXT("Nope")});
	Broken->TypeName = TEXT("Bad-Name");
	Broken->Functions.AddDefaulted_GetRef().Name = TEXT("Read");
	FCrowdyServerFunction& Platform = Broken->Functions.AddDefaulted_GetRef();
	Platform.Name = TEXT("Timer");
	Platform.ServerName = TEXT("$timer");
	Broken->Functions.AddDefaulted_GetRef().Name = TEXT("AttackBoss");
	Broken->Functions.AddDefaulted_GetRef().Name = TEXT("Attack_Boss");
	Broken->FieldNames.Add(FieldName(FCrowdyExecTestScalars::StaticStruct(), TEXT("Level"), TEXT("HEALTH")));
	Broken->FieldNames.Add(FieldName(FCrowdyExecTestScalars::StaticStruct(), TEXT("Nope"), TEXT("Gone")));
	FCrowdyServerEnumValueName& Stray = Broken->EnumValueNames.AddDefaulted_GetRef();
	Stray.Enum = StaticEnum<ECrowdyExecTestPhase>();
	Stray.Value = TEXT("Missing");
	Stray.ServerName = TEXT("Lost");
	TArray<FString> Errors;
	TestFalse(TEXT("a definition with bad names does not bake"), Broken->Bake(Errors));
	const TArray<FString> Expected = {
		TEXT("Type Name 'Bad-Name' must be lowercase letters, digits and underscores, start with a letter, and be at most 48 characters"),
		TEXT("Function Read: 'read' is reserved"),
		TEXT("Function Timer: '$timer' cannot be its server name, which must be lowercase letters, digits and underscores, start with a letter, and be at most 64 characters"),
		TEXT("Function Attack_Boss: another function is also named 'attack_boss'"),
		TEXT("FCrowdyExecTestScalars.Level: another field of FCrowdyExecTestScalars is also named 'HEALTH'; server names must differ in more than case"),
		TEXT("Watched field 'Nope' is not a field of FCrowdyExecTestBossState"),
		TEXT("Server Names: FCrowdyExecTestScalars has no field 'Nope'"),
		TEXT("Server Names: ECrowdyExecTestPhase has no value 'Missing'")};
	TestEqual(TEXT("one error per problem"), Errors.Num(), Expected.Num());
	for (const FString& Line : Expected)
	{
		TestTrue(FString::Printf(TEXT("reported: %s (got: %s)"), *Line, *FString::Join(Errors, TEXT(" | "))),
			Errors.ContainsByPredicate([&Line](const FString& Candidate) { return Candidate.Equals(Line, ESearchCase::CaseSensitive); }));
	}

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Renamed = MakeFixtureDefinition();
	Renamed->FieldNames.Add(FieldName(FCrowdyExecTestScalars::StaticStruct(), TEXT("Health"), TEXT("HP")));
	FCrowdyServerEnumValueName& Angry = Renamed->EnumValueNames.AddDefaulted_GetRef();
	Angry.Enum = StaticEnum<ECrowdyExecTestPhase>();
	Angry.Value = TEXT("Enraged");
	Angry.ServerName = TEXT("Angry");
	if (!Bake(*this, *Renamed))
	{
		return false;
	}
	FCrowdyExecTestScalars Scalars;
	Scalars.Phase = ECrowdyExecTestPhase::Enraged;
	const FString Hex = ToHex(EncodeOf(*this, *Renamed, Scalars));
	TestTrue(TEXT("a field override renames the key"), Hex.Contains(TEXT("a2485000")) && !Hex.Contains(TEXT("a64865616c7468")));
	TestTrue(TEXT("an enum value override renames the value"), Hex.Contains(TEXT("a5416e677279")));

	const FCrowdyExecBakedStruct* Heavy = Renamed->BakedStructs.FindByPredicate([](const FCrowdyExecBakedStruct& Baked) { return Baked.Struct == FCrowdyExecTestHeavyHit::StaticStruct(); });
	TestTrue(TEXT("a derived struct's fields travel base first"), Heavy && Heavy->Fields.Num() == 3 && Heavy->Fields[0].ServerName == TEXT("Damage")
		&& Heavy->Fields[1].ServerName == TEXT("Source") && Heavy->Fields[2].ServerName == TEXT("Stagger"));
	const FCrowdyExecBakedStruct& State = Renamed->BakedStructs[0];
	TestTrue(TEXT("the watched flags are baked"), State.Struct == FCrowdyExecTestBossState::StaticStruct() && State.Fields[0].bWatched && State.Fields[1].bWatched
		&& State.Fields[2].bWatched && !State.Fields[3].bWatched);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDefinitionStaleTablesTest, "CrowdySDK.CrowdyExec.DefinitionDetectsStaleTables", TestFlags)
bool FCrowdyExecDefinitionStaleTablesTest::RunTest(const FString& Parameters)
{
	FCrowdyExecTestBossState State;
	TArray<uint8> Bytes;
	FString Error;

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Grown = MakeFixtureDefinition();
	if (!Bake(*this, *Grown))
	{
		return false;
	}
	Grown->BakedStructs[0].Fields.Pop();
	TestFalse(TEXT("a struct with a field the tables have not seen is refused"), CrowdyExec::Encode(*Grown, FCrowdyExecTestBossState::StaticStruct(), &State, Bytes, Error));
	ErrorIs(*this, Error, FString::Printf(TEXT("FCrowdyExecTestBossState has changed since %s was saved; save the definition again"), *Grown->GetName()));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Moved = MakeFixtureDefinition();
	if (!Bake(*this, *Moved))
	{
		return false;
	}
	Moved->BakedStructs[0].Fields[0].Property = TEXT("Gone");
	TestFalse(TEXT("a field the struct no longer has is refused"), CrowdyExec::Encode(*Moved, FCrowdyExecTestBossState::StaticStruct(), &State, Bytes, Error));
	ErrorIs(*this, Error, FString::Printf(TEXT("FCrowdyExecTestBossState has no field Health any more; save %s again"), *Moved->GetName()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDecodeForgedContainersTest, "CrowdySDK.CrowdyExec.DecodeRefusesForgedInput", TestFlags)
bool FCrowdyExecDecodeForgedContainersTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeFixtureDefinition();
	if (!Bake(*this, *Definition))
	{
		return false;
	}
	FCrowdyExecTestScalars Scalars;
	Scalars.Health = 77;
	ExpectRefusal(*this, *Definition, Scalars, FMessage().Map(2).Key("Health").UInt(1).Key("Health").UInt(2).Bytes,
		TEXT("FCrowdyExecTestScalars: the field Health appears twice"));

	FCrowdyExecTestContainers Containers;
	Containers.Hits = {9};
	ExpectRefusal(*this, *Definition, Containers, FMessage().Map(1).Key("Tags").Array(2).Str(TEXT("elite")).Str(TEXT("elite")).Bytes,
		TEXT("FCrowdyExecTestContainers.Tags: two keys collide: 'elite' (Unreal compares these keys without case)"));
	ExpectRefusal(*this, *Definition, Containers, FMessage().Map(1).Key("Tags").Array(2).Str(TEXT("elite")).Str(TEXT("Elite")).Bytes,
		TEXT("FCrowdyExecTestContainers.Tags: two keys collide: 'Elite' (Unreal compares these keys without case)"));
	ExpectRefusal(*this, *Definition, Containers, FMessage().Map(1).Key("DamageByPlayer").Map(2).Key("p1").UInt(1).Key("P1").UInt(2).Bytes,
		TEXT("FCrowdyExecTestContainers.DamageByPlayer: two keys collide: 'P1' (Unreal compares these keys without case)"));
	// The same spelling twice: a game build shows a name in the case it was first created with.
	ExpectRefusal(*this, *Definition, FCrowdyExecTestKeys(), FMessage().Map(1).Key("ByName").Map(2).Key("crowdy_exec_dup").UInt(1).Key("crowdy_exec_dup").UInt(2).Bytes,
		TEXT("FCrowdyExecTestKeys.ByName: two keys collide: 'crowdy_exec_dup' (Unreal compares these keys without case)"));
	FCrowdyExecTestExtras Extras;
	Extras.Small = 9;
	ExpectRefusal(*this, *Definition, Extras, FMessage().Map(1).Key("ByIndex").Map(2).UInt(3).Str(TEXT("a")).UInt(3).Str(TEXT("b")).Bytes,
		TEXT("FCrowdyExecTestExtras.ByIndex: the key 3 appears twice"));
	ExpectRefusal(*this, *Definition, Extras, FMessage().Map(1).Key("Phases").Array(2).Str(TEXT("Calm")).Str(TEXT("Calm")).Bytes,
		TEXT("FCrowdyExecTestExtras.Phases: the key Calm appears twice"));

	ExpectRefusal(*this, *Definition, Extras, FMessage().Map(1).Key("Notes").Raw(TEXT("dc0100")).Bytes,
		TEXT("FCrowdyExecTestExtras.Notes: a container of 256 elements, more than the 0 bytes left in the message"));
	ExpectRefusal(*this, *Definition, Extras, FMessage().Map(1).Key("Path").Str(TEXT("/Game/") + FString::ChrN(1018, TEXT('x'))).Bytes,
		TEXT("FCrowdyExecTestExtras.Path: an object path of 1024 characters, over the limit of 1023"));

	// Every row but the last holds 4,096 elements; the rows' own array adds one element per row.
	auto Grid = [](int32 Rows, int32 LastRow)
	{
		FMessage Message;
		Message.Map(1).Key("Items").Array(Rows);
		for (int32 Row = 0; Row < Rows; ++Row)
		{
			const int32 Cells = Row + 1 < Rows ? CrowdyExec::MaxElements : LastRow;
			Message.Map(1).Key("Items").Array(Cells);
			for (int32 Cell = 0; Cell < Cells; ++Cell)
			{
				Message.Map(0);
			}
		}
		return Message.Bytes;
	};
	FCrowdyExecTestDepth2 Full;
	FString Error;
	TestTrue(FString::Printf(TEXT("65,536 elements in one message decode (%s)"), *Error), DecodeInto(*Definition, Grid(16, 4080), Full, Error) && Full.Items.Num() == 16);
	ExpectRefusal(*this, *Definition, FCrowdyExecTestDepth2(), Grid(16, 4081),
		TEXT("FCrowdyExecTestDepth2.Items[15].Items: more than 65536 elements in one message"));

	TArray<uint8> Oversized = FromHex(TEXT("80"));
	Oversized.SetNumZeroed(CrowdyExec::MaxPayloadBytes + 1);
	ExpectRefusal(*this, *Definition, Scalars, Oversized, TEXT("FCrowdyExecTestScalars: the message is 1048577 bytes, over the limit of 1048576"));
	FCrowdyExecTestBossState State;
	State.Health = 55;
	FCrowdyExecStateHeader Header;
	TArray<FString> Fields;
	TestFalse(TEXT("an oversized state message is refused"),
		CrowdyExec::DecodeState(*Definition, ECrowdyExecStateMessage::Push, Oversized, &State, Header, Fields, Error));
	ErrorIs(*this, Error, TEXT("FCrowdyExecTestBossState: the message is 1048577 bytes, over the limit of 1048576"));
	TestEqual(TEXT("and changes nothing"), State.Health, 55);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDecodeDropsUnknownEnumEntriesTest, "CrowdySDK.CrowdyExec.DecodeDropsUnknownEnumEntries", TestFlags)
bool FCrowdyExecDecodeDropsUnknownEnumEntriesTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeFixtureDefinition();
	if (!Bake(*this, *Definition))
	{
		return false;
	}
	CrowdyExec::ResetUnknownEnumReportsForTest();
	const TCHAR* Reports[] = {TEXT("FCrowdyExecTestKeys.HitByPhase: 'Gone1'"), TEXT("FCrowdyExecTestKeys.PhaseList: 'Gone2'"),
		TEXT("FCrowdyExecTestKeys.MaybePhase: 'Gone3'"), TEXT("FCrowdyExecTestExtras.Phases: 'Gone4'"), TEXT("FCrowdyExecTestBossState.Phase: 'Gone5'")};
	for (const TCHAR* Report : Reports)
	{
		AddExpectedErrorPlain(Report, EAutomationExpectedErrorFlags::Contains, 1);
	}

	// The unknown key follows a struct value, so its report must name the map, not the struct's last field.
	FMessage Message;
	Message.Map(3).Key("HitByPhase").Map(2).Key("Calm").Map(1).Key("Damage").UInt(1).Key("Gone1").Map(1).Key("Damage").UInt(2);
	Message.Key("PhaseList").Array(3).Str(TEXT("Calm")).Str(TEXT("Gone2")).Str(TEXT("Final"));
	Message.Key("MaybePhase").Str(TEXT("Gone3"));
	FCrowdyExecTestKeys Keys;
	FString Error;
	TestTrue(FString::Printf(TEXT("decodes (%s)"), *Error), DecodeInto(*Definition, Message.Bytes, Keys, Error));
	TestTrue(TEXT("a map entry with an unknown enum key is dropped"),
		Keys.HitByPhase.Num() == 1 && Keys.HitByPhase.FindRef(ECrowdyExecTestPhase::Calm).Damage == 1);
	TestTrue(TEXT("an array element with an unknown enum name is dropped"),
		Keys.PhaseList == TArray<ECrowdyExecTestPhase>({ECrowdyExecTestPhase::Calm, ECrowdyExecTestPhase::Final}));
	TestFalse(TEXT("an optional with an unknown enum name stays unset"), Keys.MaybePhase.IsSet());

	FCrowdyExecTestExtras Extras;
	TestTrue(TEXT("a set element with an unknown enum name is dropped"),
		DecodeInto(*Definition, FMessage().Map(1).Key("Phases").Array(2).Str(TEXT("Final")).Str(TEXT("Gone4")).Bytes, Extras, Error)
		&& Extras.Phases.Num() == 1 && Extras.Phases.Contains(ECrowdyExecTestPhase::Final));

	FCrowdyExecTestBossState State;
	State.Phase = ECrowdyExecTestPhase::Enraged;
	FCrowdyExecStateHeader Header;
	TArray<FString> Fields;
	const TArray<uint8> Push = FMessage().Map(3).Key("epoch").UInt(1).Key("seq").UInt(2).Key("fields").Map(2).Key("Health").UInt(5).Key("Phase").Str(TEXT("Gone5")).Bytes;
	TestTrue(FString::Printf(TEXT("the push decodes (%s)"), *Error),
		CrowdyExec::DecodeState(*Definition, ECrowdyExecStateMessage::Push, Push, &State, Header, Fields, Error));
	TestTrue(TEXT("the push reports only the value it applied"), Fields == TArray<FString>({TEXT("Health")}));
	TestEqual(TEXT("the value with the unknown name keeps what it had"), static_cast<int32>(State.Phase), static_cast<int32>(ECrowdyExecTestPhase::Enraged));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecUnknownEnumReportsTest, "CrowdySDK.CrowdyExec.UnknownEnumReportsAreBounded", TestFlags)
bool FCrowdyExecUnknownEnumReportsTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeFixtureDefinition();
	if (!Bake(*this, *Definition))
	{
		return false;
	}
	CrowdyExec::ResetUnknownEnumReportsForTest();
	const FString Unsafe = FString::ChrN(1, TCHAR(1)) + FString::ChrN(99, TEXT('y'));
	const FString Shown = TEXT("?") + FString::ChrN(63, TEXT('y')) + TEXT("...");
	AddExpectedErrorPlain(FString::Printf(TEXT("FCrowdyExecTestScalars.Phase: '%s' is not a value"), *Shown), EAutomationExpectedErrorFlags::Contains, 1);
	FCrowdyExecTestScalars Scalars;
	FString Error;
	TestTrue(TEXT("a long name with a control character decodes"), DecodeInto(*Definition, FMessage().Map(1).Key("Phase").Str(Unsafe).Bytes, Scalars, Error));

	auto UnknownNames = [](const TCHAR* Prefix, int32 Count)
	{
		FMessage Message;
		Message.Map(1).Key("PhaseList").Array(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Message.Str(FString::Printf(TEXT("%s%d"), Prefix, Index));
		}
		return Message.Bytes;
	};
	CrowdyExec::ResetUnknownEnumReportsForTest();
	FLogCounter Reported(TEXT("FCrowdyExecTestKeys.PhaseList: '"));
	FLogCounter Stopped(TEXT("Further unknown enum values are not reported"));
	AddExpectedErrorPlain(TEXT("FCrowdyExecTestKeys.PhaseList: '"), EAutomationExpectedErrorFlags::Contains, 256);
	AddExpectedErrorPlain(TEXT("Further unknown enum values are not reported"), EAutomationExpectedErrorFlags::Contains, 1);
	FCrowdyExecTestKeys Keys;
	TestTrue(TEXT("300 unknown names decode"), DecodeInto(*Definition, UnknownNames(TEXT("Cap"), 300), Keys, Error) && Keys.PhaseList.IsEmpty());
	TestTrue(TEXT("and so do more"), DecodeInto(*Definition, UnknownNames(TEXT("Late"), 5), Keys, Error));
	TestEqual(TEXT("256 unknown names are reported"), Reported.Count, 256);
	TestEqual(TEXT("then one line says the rest are not"), Stopped.Count, 1);
	CrowdyExec::ResetUnknownEnumReportsForTest();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecEncodeKeyOrderTest, "CrowdySDK.CrowdyExec.EncodeKeyOrderAndNoneNames", TestFlags)
bool FCrowdyExecEncodeKeyOrderTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeFixtureDefinition();
	FCrowdyServerEnumValueName& Renamed = Definition->EnumValueNames.AddDefaulted_GetRef();
	Renamed.Enum = StaticEnum<ECrowdyExecTestPhase>();
	Renamed.Value = TEXT("Calm");
	Renamed.ServerName = TEXT("Zzz");
	if (!Bake(*this, *Definition))
	{
		return false;
	}
	// Calm travels as Zzz, so name order and declaration order differ; neither insertion order nor its reverse is sorted.
	FCrowdyExecTestExtras Extras;
	Extras.Phases.Add(ECrowdyExecTestPhase::Final);
	Extras.Phases.Add(ECrowdyExecTestPhase::Calm);
	Extras.Phases.Add(ECrowdyExecTestPhase::Enraged);
	TestTrue(TEXT("enum set elements travel in declaration order"),
		ToHex(EncodeOf(*this, *Definition, Extras)).Contains(TEXT("a650686173657393a35a7a7aa7456e7261676564a546696e616c")));
	FCrowdyExecTestKeys Keys;
	Keys.HitByPhase.Add(ECrowdyExecTestPhase::Final, MakeHit(1, TEXT("")));
	Keys.HitByPhase.Add(ECrowdyExecTestPhase::Calm, MakeHit(2, TEXT("")));
	TestTrue(TEXT("enum map keys travel in declaration order"), ToHex(EncodeOf(*this, *Definition, Keys)).Contains(
		TEXT("aa4869744279506861736582a35a7a7a82a644616d61676502a6536f75726365a0a546696e616c82a644616d61676501a6536f75726365a0")));

	FCrowdyExecTestHit Nameless;
	TestEqual(TEXT("FName None travels as an empty string"), ToHex(EncodeOf(*this, *Definition, Nameless)), FString(TEXT("82a644616d61676500a6536f75726365a0")));
	FCrowdyExecTestKeys NoneKey;
	NoneKey.ByName.Add(NAME_None, 1);
	TestTrue(TEXT("so does a None map key"), ToHex(EncodeOf(*this, *Definition, NoneKey)).EndsWith(TEXT("a642794e616d6581a001")));
	FCrowdyExecTestHit Back = MakeHit(0, TEXT("Before"));
	FString Error;
	TestTrue(TEXT("and an empty string decodes to None"), DecodeInto(*Definition, EncodeOf(*this, *Definition, Nameless), Back, Error) && Back.Source.IsNone());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecStateMessageChecksTest, "CrowdySDK.CrowdyExec.StateMessageChecks", TestFlags)
bool FCrowdyExecStateMessageChecksTest::RunTest(const FString& Parameters)
{
	FCrowdyExecStateHeader Header;
	TArray<FString> Fields;
	FString Error;

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Deep = MakeDeepDefinition();
	Deep->State = FCrowdyExecTestDepth8::StaticStruct();
	Deep->BakedStructs[0].Fields[0].bWatched = true;
	FMessage Nested;
	Nested.Map(3).Key("epoch").UInt(1).Key("seq").UInt(1).Key("fields").Raw(DeepestAllowedHex());
	FCrowdyExecTestDepth8 Top;
	TestTrue(FString::Printf(TEXT("the State struct nests 16 levels below the envelope (%s)"), *Error),
		CrowdyExec::DecodeState(*Deep, ECrowdyExecStateMessage::Push, Nested.Bytes, &Top, Header, Fields, Error) && Top.Items.Num() == 1);
	FMessage Junk;
	Junk.Map(3).Key("epoch").UInt(1).Key("seq").UInt(1).Key("junk");
	for (int32 Level = 0; Level < 16; ++Level)
	{
		Junk.Array(1);
	}
	Junk.Nil();
	TestTrue(FString::Printf(TEXT("an unknown envelope key 16 levels deep is skipped (%s)"), *Error),
		CrowdyExec::DecodeState(*Deep, ECrowdyExecStateMessage::Push, Junk.Bytes, &Top, Header, Fields, Error));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeFixtureDefinition();
	if (!Bake(*this, *Definition))
	{
		return false;
	}
	FCrowdyExecTestBossState State;
	const TArray<uint8> Early = FMessage().Map(4).Key("contract").UInt(2).Key("fields").Map(1).Key("Health").Double(1.5)
		.Key("epoch").UInt(1).Key("seq").UInt(0).Bytes;
	TestFalse(TEXT("another contract is refused before a later key fails"),
		CrowdyExec::DecodeState(*Definition, ECrowdyExecStateMessage::Read, Early, &State, Header, Fields, Error));
	ErrorIs(*this, Error, TEXT("FCrowdyExecTestBossState: the server code was built for another SDK version (message format 2, this client reads 1); regenerate it and deploy it again"));
	const TArray<uint8> Textual = FMessage().Map(3).Key("contract").Str(TEXT("1")).Key("epoch").UInt(1).Key("seq").UInt(0).Bytes;
	TestFalse(TEXT("a contract that is not an integer is refused"),
		CrowdyExec::DecodeState(*Definition, ECrowdyExecStateMessage::Read, Textual, &State, Header, Fields, Error));
	ErrorIs(*this, Error, TEXT("FCrowdyExecTestBossState: the server code was built for another SDK version (message format unknown, this client reads 1); regenerate it and deploy it again"));

	FString Resolved;
	TestNotNull(TEXT("the tables resolve"), Definition->ResolveLayout(Resolved));
	Definition->State = FCrowdyExecTestScalars::StaticStruct();
	FCrowdyExecTestScalars Scalars;
	Scalars.Health = 3;
	TestFalse(TEXT("a State struct swapped after the tables were resolved is refused"),
		CrowdyExec::DecodeState(*Definition, ECrowdyExecStateMessage::Push, FromHex(PushHex), &Scalars, Header, Fields, Error));
	ErrorIs(*this, Error, FString::Printf(TEXT("%s: the State struct changed after its field tables were built; save it again"), *Definition->GetName()));
	TestEqual(TEXT("and nothing is written"), Scalars.Health, 3);
	Definition->State = nullptr;
	TestFalse(TEXT("a cleared State struct is refused"),
		CrowdyExec::DecodeState(*Definition, ECrowdyExecStateMessage::Push, FromHex(PushHex), &State, Header, Fields, Error));
	ErrorIs(*this, Error, FString::Printf(TEXT("%s has no State struct"), *Definition->GetName()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDefinitionTableChecksTest, "CrowdySDK.CrowdyExec.DefinitionChecksTables", TestFlags)
bool FCrowdyExecDefinitionTableChecksTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UCrowdyServerObjectDefinition> TooDeep = MakeDefinition(FCrowdyExecTestBossState::StaticStruct(), {FCrowdyExecTestDepth8::StaticStruct()}, {});
	TArray<FString> Errors;
	TestFalse(TEXT("a Params struct nesting 17 levels does not bake"), TooDeep->Bake(Errors));
	TestTrue(FString::Printf(TEXT("the depth is the only problem (got: %s)"), *FString::Join(Errors, TEXT(" | "))),
		Errors.Num() == 1 && Errors[0].Equals(TEXT("FCrowdyExecTestDepth8 nests structs and containers more than 16 levels deep"), ESearchCase::CaseSensitive));
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Shallower = MakeDefinition(FCrowdyExecTestBossState::StaticStruct(), {FCrowdyExecTestDepth7::StaticStruct()}, {});
	Bake(*this, *Shallower);

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeFixtureDefinition();
	if (!Bake(*this, *Definition))
	{
		return false;
	}
	const FCrowdyExecBakedStruct* KeysTable = Definition->BakedStructs.FindByPredicate([](const FCrowdyExecBakedStruct& Baked) { return Baked.Struct == FCrowdyExecTestKeys::StaticStruct(); });
	TestTrue(TEXT("a transient field is not in the table"), KeysTable && KeysTable->Fields.Num() == 4
		&& !KeysTable->Fields.ContainsByPredicate([](const FCrowdyExecBakedField& Field) { return Field.Property == TEXT("Cache"); }));
	FCrowdyExecTestKeys Keys;
	Keys.Cache = 5;
	TestTrue(TEXT("nor on the wire"), ToHex(EncodeOf(*this, *Definition, Keys)).StartsWith(TEXT("84")));

	auto PhaseTable = [](UCrowdyServerObjectDefinition& Owner)
	{
		return Owner.BakedEnums.FindByPredicate([](const FCrowdyExecBakedEnum& Baked) { return Baked.Enum == StaticEnum<ECrowdyExecTestPhase>(); });
	};
	const FCrowdyExecBakedEnum* PhaseEnum = PhaseTable(*Definition);
	TestTrue(TEXT("each value's enumerator is baked"), PhaseEnum && PhaseEnum->EnumeratorNames == TArray<FName>({TEXT("ECrowdyExecTestPhase::Calm"),
		TEXT("ECrowdyExecTestPhase::Enraged"), TEXT("ECrowdyExecTestPhase::Final")}));
	FCrowdyExecTestScalars Scalars;
	TArray<uint8> Bytes;
	FString Error;
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Renumbered = MakeFixtureDefinition();
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Removed = MakeFixtureDefinition();
	if (!Bake(*this, *Renumbered) || !Bake(*this, *Removed) || !PhaseTable(*Renumbered) || !PhaseTable(*Removed))
	{
		return false;
	}
	PhaseTable(*Renumbered)->Values[1] = 7;
	TestFalse(TEXT("an enum whose values were renumbered is refused"), CrowdyExec::Encode(*Renumbered, FCrowdyExecTestScalars::StaticStruct(), &Scalars, Bytes, Error));
	ErrorIs(*this, Error, FString::Printf(TEXT("ECrowdyExecTestPhase has changed since %s was saved; save the definition again"), *Renumbered->GetName()));
	PhaseTable(*Removed)->EnumeratorNames[2] = TEXT("ECrowdyExecTestPhase::Gone");
	TestFalse(TEXT("an enum that lost a value is refused"), CrowdyExec::Encode(*Removed, FCrowdyExecTestScalars::StaticStruct(), &Scalars, Bytes, Error));
	ErrorIs(*this, Error, FString::Printf(TEXT("ECrowdyExecTestPhase has changed since %s was saved; save the definition again"), *Removed->GetName()));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Guessed = MakeFixtureDefinition();
	if (!Bake(*this, *Guessed))
	{
		return false;
	}
	Guessed->BakedStructs[0].Fields[0].PropertyGuid = FGuid::NewGuid();
	TestTrue(TEXT("a GUID the struct has no record of falls back to the baked name"),
		CrowdyExec::FindResolvedPropertyForTest(*Guessed, *FCrowdyExecTestBossState::StaticStruct(), TEXT("Health"))
		== FindFProperty<FProperty>(FCrowdyExecTestBossState::StaticStruct(), TEXT("Health")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecStateHeaderTest, "CrowdySDK.CrowdyExec.StateHeaderAndEnvelopeKeys", TestFlags)
bool FCrowdyExecStateHeaderTest::RunTest(const FString& Parameters)
{
	FCrowdyExecStateHeader Header;
	FString Error;
	TestTrue(FString::Printf(TEXT("a push's header reads (%s)"), *Error), CrowdyExec::ReadStateHeader(FromHex(PushHex), Header, Error));
	TestTrue(TEXT("its epoch and seq"), Header.Epoch == FixtureEpoch && Header.Seq == 7);
	Header = FCrowdyExecStateHeader();
	TestTrue(FString::Printf(TEXT("a read's header reads (%s)"), *Error), CrowdyExec::ReadStateHeader(FromHex(ReadHex), Header, Error));
	TestTrue(TEXT("its epoch and seq"), Header.Epoch == FixtureEpoch && Header.Seq == 7);
	const TArray<uint8> BadFields = FMessage().Map(3).Key("fields").Map(1).Key("Health").Double(1.5).Key("seq").UInt(4).Key("epoch").UInt(3).Bytes;
	TestTrue(TEXT("the fields are skipped, not decoded"), CrowdyExec::ReadStateHeader(BadFields, Header, Error) && Header.Epoch == 3 && Header.Seq == 4);

	auto ExpectHeaderRefusal = [this](const TArray<uint8>& Bytes, const FString& Expected)
	{
		FCrowdyExecStateHeader Kept;
		Kept.Epoch = 99;
		FString Refusal;
		TestFalse(FString::Printf(TEXT("refused: %s"), *Expected), CrowdyExec::ReadStateHeader(Bytes, Kept, Refusal));
		ErrorIs(*this, Refusal, Expected);
		TestTrue(TEXT("and the header is untouched"), Kept.Epoch == 99);
	};
	ExpectHeaderRefusal(FMessage().Map(3).Key("epoch").UInt(1).Key("seq").UInt(2).Key("epoch").UInt(3).Bytes, TEXT("the envelope key epoch appears twice"));
	ExpectHeaderRefusal(FMessage().Map(1).Key("epoch").UInt(1).Bytes, TEXT("the message carries no epoch or seq"));
	ExpectHeaderRefusal(FMessage().Map(2).Key("epoch").Str(TEXT("1")).Key("seq").UInt(2).Bytes, TEXT("expected an unsigned integer, got a string"));
	ExpectHeaderRefusal(FromHex(TEXT("c0")), TEXT("expected a map, got nil"));
	TArray<uint8> Oversized = FromHex(PushHex);
	Oversized.SetNumZeroed(CrowdyExec::MaxPayloadBytes + 1);
	ExpectHeaderRefusal(Oversized, TEXT("the message is 1048577 bytes, over the limit of 1048576"));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeFixtureDefinition();
	if (!Bake(*this, *Definition))
	{
		return false;
	}
	FCrowdyExecTestBossState State;
	State.Health = 55;
	TArray<FString> Fields;
	const TArray<uint8> TwoFields = FMessage().Map(4).Key("epoch").UInt(1).Key("seq").UInt(2)
		.Key("fields").Map(1).Key("Health").UInt(3).Key("fields").Map(1).Key("Health").UInt(4).Bytes;
	TestFalse(TEXT("a state message naming fields twice is refused"),
		CrowdyExec::DecodeState(*Definition, ECrowdyExecStateMessage::Push, TwoFields, &State, Header, Fields, Error));
	ErrorIs(*this, Error, TEXT("FCrowdyExecTestBossState: the envelope key fields appears twice"));
	TestEqual(TEXT("and changes nothing"), State.Health, 55);
	const TArray<uint8> TwoSeqs = FMessage().Map(3).Key("epoch").UInt(1).Key("seq").UInt(2).Key("seq").UInt(3).Bytes;
	TestFalse(TEXT("so is one naming seq twice"),
		CrowdyExec::DecodeState(*Definition, ECrowdyExecStateMessage::Push, TwoSeqs, &State, Header, Fields, Error));
	ErrorIs(*this, Error, TEXT("FCrowdyExecTestBossState: the envelope key seq appears twice"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecSafeTextTest, "CrowdySDK.CrowdyExec.ServerTextIsMadeSafe", TestFlags)
bool FCrowdyExecSafeTextTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("control and direction-override characters are replaced"),
		CrowdyExec::SafeText(FString(TEXT("a\r\nb")) + TCHAR(0x202E) + TEXT("c") + TCHAR(0x2066) + TEXT("d") + TCHAR(0x85), 64), FString(TEXT("a??b?c?d?")));
	TestEqual(TEXT("a cut is marked"), CrowdyExec::SafeText(TEXT("abcdef"), 3), FString(TEXT("abc...")));
	TestEqual(TEXT("short text is kept"), CrowdyExec::SafeText(TEXT("abc"), 3), FString(TEXT("abc")));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeFixtureDefinition();
	if (!Bake(*this, *Definition))
	{
		return false;
	}
	ExpectRefusal(*this, *Definition, FCrowdyExecTestEngine(), FMessage().Map(1).Key("Id").Str(TEXT("bad\nguid")).Bytes,
		TEXT("FCrowdyExecTestEngine.Id: 'bad?guid' is not a GUID"));
	FCrowdyExecTestExtras Extras;
	ExpectRefusal(*this, *Definition, Extras, FMessage().Map(1).Key("Tag").Str(FString(TEXT("x")) + TCHAR(0x202E) + TEXT("y")).Bytes,
		TEXT("FCrowdyExecTestExtras.Tag: 'x?y' is not a gameplay tag this build knows"));
	ExpectRefusal(*this, *Definition, Extras, FMessage().Map(1).Key("Path").Str(TEXT("bad\rpath")).Bytes,
		TEXT("FCrowdyExecTestExtras.Path: 'bad?path' is not an object path"));
	ExpectRefusal(*this, *Definition, Extras, FMessage().Map(1).Key("Path").Str(FString::ChrN(70, TEXT('/'))).Bytes,
		FString::Printf(TEXT("FCrowdyExecTestExtras.Path: '%s...' is not an object path"), *FString::ChrN(64, TEXT('/'))));
	FMessage Junk;
	Junk.Map(1).Key("Ju\nnk");
	for (int32 Level = 0; Level < 16; ++Level)
	{
		Junk.Array(1);
	}
	ExpectRefusal(*this, *Definition, FCrowdyExecTestScalars(), Junk.Nil().Bytes, TEXT("FCrowdyExecTestScalars.Ju?nk: nesting deeper than 16 levels"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecNewNameBudgetTest, "CrowdySDK.CrowdyExec.NewNamesAreBudgeted", TestFlags)
bool FCrowdyExecNewNameBudgetTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeFixtureDefinition();
	if (!Bake(*this, *Definition))
	{
		return false;
	}
	// Fresh names each run, 33 characters each: the budget has room for exactly one.
	auto FreshName = []() { return TEXT("N") + FGuid::NewGuid().ToString(EGuidFormats::Digits); };
	const int64 SpentBefore = CrowdyExec::NewNameCharsSpentForTest();
	CrowdyExec::ResetNewNameBudgetForTest(CrowdyExec::MaxNewNameChars - 40);
	AddExpectedErrorPlain(TEXT("The server sent more new names than this client accepts"), EAutomationExpectedErrorFlags::Contains, 1);
	FCrowdyExecTestHit Hit;
	FString Error;
	const FString First = FreshName();
	TestTrue(FString::Printf(TEXT("a new name within the budget decodes (%s)"), *Error),
		DecodeInto(*Definition, FMessage().Map(1).Key("Source").Str(First).Bytes, Hit, Error) && Hit.Source == FName(*First));

	const FString Spent = FreshName();
	TestTrue(FString::Printf(TEXT("a message with a new name past the budget still decodes (%s)"), *Error),
		DecodeInto(*Definition, FMessage().Map(2).Key("Source").Str(Spent).Key("Damage").UInt(9).Bytes, Hit, Error));
	TestTrue(TEXT("the new name reads as None"), Hit.Source.IsNone());
	TestEqual(TEXT("the rest of the message applies"), Hit.Damage, 9);
	TestTrue(TEXT("and the name table did not gain it"), FName(*Spent, FNAME_Find).IsNone());

	FCrowdyExecTestKeys Keys;
	const TArray<uint8> ByName = FMessage().Map(1).Key("ByName").Map(3).Str(FreshName()).UInt(1).Str(TEXT("Damage")).UInt(2).Str(FreshName()).UInt(3).Bytes;
	TestTrue(FString::Printf(TEXT("a map with new keys past the budget still decodes (%s)"), *Error), DecodeInto(*Definition, ByName, Keys, Error));
	TestEqual(TEXT("each entry whose key was skipped is dropped, so two of them do not collide as None"), Keys.ByName.Num(), 1);
	const int32* Kept = Keys.ByName.Find(FName(TEXT("Damage")));
	TestTrue(TEXT("and the entry whose key exists is kept"), Kept && *Kept == 2);

	const FString Fresh = FreshName();
	const FSoftObjectPath Known(TEXT("/Script/CoreUObject.Object"));
	FCrowdyExecTestExtras Extras;
	Extras.Path = Known;
	TestTrue(FString::Printf(TEXT("a path of new names past the budget still decodes (%s)"), *Error),
		DecodeInto(*Definition, FMessage().Map(1).Key("Path").Str(TEXT("/Game/") + Fresh + TEXT(".") + Fresh).Bytes, Extras, Error));
	TestTrue(TEXT("and reads as an empty path"), Extras.Path.IsNull());
	Extras.Path = Known;
	TestTrue(FString::Printf(TEXT("a path whose asset name is new still decodes (%s)"), *Error),
		DecodeInto(*Definition, FMessage().Map(1).Key("Path").Str(TEXT("/Script/CoreUObject.") + Fresh).Bytes, Extras, Error));
	TestTrue(TEXT("and reads as an empty path too"), Extras.Path.IsNull());

	TestTrue(TEXT("a name that already exists costs nothing"),
		DecodeInto(*Definition, FMessage().Map(1).Key("Source").Str(TEXT("Damage")).Bytes, Hit, Error) && Hit.Source == FName(TEXT("Damage")));
	TestTrue(TEXT("nor does a path whose names exist"),
		DecodeInto(*Definition, FMessage().Map(1).Key("Path").Str(TEXT("/Script/CoreUObject.Object")).Bytes, Extras, Error)
		&& Extras.Path == FSoftObjectPath(TEXT("/Script/CoreUObject.Object")));
	CrowdyExec::ResetNewNameBudgetForTest(SpentBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecNoneCostsNoBudgetTest, "CrowdySDK.CrowdyExec.NoneNameCostsNoBudget", TestFlags)
bool FCrowdyExecNoneCostsNoBudgetTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeFixtureDefinition();
	if (!Bake(*this, *Definition))
	{
		return false;
	}
	// Room for one 33-character name; ten Nones would use the rest of it if they were charged.
	const FString Fresh = TEXT("N") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const int64 SpentBefore = CrowdyExec::NewNameCharsSpentForTest();
	CrowdyExec::ResetNewNameBudgetForTest(CrowdyExec::MaxNewNameChars - 40);
	FCrowdyExecTestHit Hit;
	FString Error;
	for (int32 Round = 0; Round < 10; ++Round)
	{
		Hit.Source = FName(TEXT("Damage"));
		TestTrue(FString::Printf(TEXT("None decodes (%s)"), *Error),
			DecodeInto(*Definition, FMessage().Map(1).Key("Source").Str(TEXT("None")).Bytes, Hit, Error) && Hit.Source.IsNone());
	}
	TestEqual(TEXT("and spends nothing"), CrowdyExec::NewNameCharsSpentForTest(), CrowdyExec::MaxNewNameChars - 40);
	TestTrue(TEXT("a name that exists still decodes"),
		DecodeInto(*Definition, FMessage().Map(1).Key("Source").Str(TEXT("Damage")).Bytes, Hit, Error) && Hit.Source == FName(TEXT("Damage")));
	TestTrue(TEXT("and a new name that fits is admitted"),
		DecodeInto(*Definition, FMessage().Map(1).Key("Source").Str(Fresh).Bytes, Hit, Error) && Hit.Source == FName(*Fresh));
	CrowdyExec::ResetNewNameBudgetForTest(SpentBefore);
	return true;
}

#if WITH_CASE_PRESERVING_NAME
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCaseVariantIsBudgetedTest, "CrowdySDK.CrowdyExec.NameCaseVariantIsBudgeted", TestFlags)
bool FCrowdyExecCaseVariantIsBudgetedTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeFixtureDefinition();
	if (!Bake(*this, *Definition))
	{
		return false;
	}
	// The held name is made here; its lower-case spelling is new to the name table, which keeps every spelling.
	const FString Held = TEXT("N") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FName HeldName(*Held);
	const FString Variant = Held.ToLower();
	const int64 SpentBefore = CrowdyExec::NewNameCharsSpentForTest();
	CrowdyExec::ResetNewNameBudgetForTest(CrowdyExec::MaxNewNameChars);
	AddExpectedErrorPlain(TEXT("The server sent more new names than this client accepts"), EAutomationExpectedErrorFlags::Contains, 1);
	FCrowdyExecTestHit Hit;
	FString Error;
	TestTrue(FString::Printf(TEXT("the held spelling decodes with the budget spent (%s)"), *Error),
		DecodeInto(*Definition, FMessage().Map(1).Key("Source").Str(Held).Bytes, Hit, Error) && Hit.Source == HeldName);
	TestTrue(FString::Printf(TEXT("a message with a case variant still decodes (%s)"), *Error),
		DecodeInto(*Definition, FMessage().Map(1).Key("Source").Str(Variant).Bytes, Hit, Error));
	TestTrue(TEXT("but the variant would store a new spelling, so it reads as None"), Hit.Source.IsNone());
	CrowdyExec::ResetNewNameBudgetForTest(SpentBefore);
	return true;
}
#endif

#endif
