// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/CrowdyGameModelSubsystem.h"
#include "Replication/GameModel/CrowdyGameModelTestTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "Replication/GameModel/CrowdyAttributeRegistry.h"
#include "Replication/GameModel/CrowdyModelRef.h"
#include "Replication/GameModel/CrowdyModelValueCodec.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyModelCodecTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	FProperty* RichProp(const TCHAR* Name)
	{
		return UCrowdyGameModelRichTarget::StaticClass()->FindPropertyByName(FName(Name));
	}

	TSharedPtr<FJsonValue> Num(double Value) { return MakeShared<FJsonValueNumber>(Value); }
	TSharedPtr<FJsonValue> Str(const TCHAR* Value) { return MakeShared<FJsonValueString>(Value); }
	TSharedPtr<FJsonValue> Boolean(bool Value) { return MakeShared<FJsonValueBoolean>(Value); }
	TSharedPtr<FJsonValue> Arr(TArray<TSharedPtr<FJsonValue>> Values) { return MakeShared<FJsonValueArray>(MoveTemp(Values)); }
	TSharedPtr<FJsonValue> ObjVal(const TSharedPtr<FJsonObject>& Object) { return MakeShared<FJsonValueObject>(Object); }
}

// MapPropertyToValueType classifies the aggregate types: a scalar array is "array", an FCrowdyModelRef is
// "container_ref", a plain native struct is "object". An array of structs and a struct carrying an object reference
// stay unsupported (empty).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelValueCodecMappingTest,
	"CrowdySDK.GameModel.RichAttributeMapping", CrowdyModelCodecTestFlags)
bool FCrowdyModelValueCodecMappingTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("int array -> array"), FCrowdyAttributeRegistry::MapPropertyToValueType(RichProp(TEXT("Scores"))), FString(TEXT("array")));
	TestEqual(TEXT("string array -> array"), FCrowdyAttributeRegistry::MapPropertyToValueType(RichProp(TEXT("Tags"))), FString(TEXT("array")));
	TestEqual(TEXT("float array -> array"), FCrowdyAttributeRegistry::MapPropertyToValueType(RichProp(TEXT("Weights"))), FString(TEXT("array")));
	TestEqual(TEXT("bool array -> array"), FCrowdyAttributeRegistry::MapPropertyToValueType(RichProp(TEXT("Flags"))), FString(TEXT("array")));
	TestEqual(TEXT("model ref -> container_ref"), FCrowdyAttributeRegistry::MapPropertyToValueType(RichProp(TEXT("Equipped"))), FString(TEXT("container_ref")));

	// An array of a struct element is not a supported Server Owned array.
	TestTrue(TEXT("array of struct unsupported"), FCrowdyAttributeRegistry::MapPropertyToValueType(RichProp(TEXT("Positions"))).IsEmpty());

	// A plain native struct whose every member is in scope maps to "object".
	TestEqual(TEXT("plain native struct -> object"),
		FCrowdyAttributeRegistry::MapPropertyToValueType(RichProp(TEXT("Stats"))), FString(TEXT("object")));

	// A struct carrying an object-reference member is not a supported object (it stays unsupported).
	TestTrue(TEXT("struct with object-ref member unsupported"),
		FCrowdyAttributeRegistry::MapPropertyToValueType(RichProp(TEXT("BadObject"))).IsEmpty());

	// Discovery accepts exactly the six rich attributes (Positions and BadObject are unmarked).
	const TArray<FCrowdyAttributeDef> Defs =
		FCrowdyAttributeRegistry::DiscoverForClass(UCrowdyGameModelRichTarget::StaticClass());
	TestEqual(TEXT("six accepted rich attributes"), Defs.Num(), 6);

	return true;
}

// The codec round-trips each aggregate: encode a live value to canonical default JSON, decode server JSON back
// onto a fresh property. Integral floats canonicalize to integer text; an empty container ref sends no default.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelValueCodecRoundTripTest,
	"CrowdySDK.GameModel.RichValueCodecRoundTrip", CrowdyModelCodecTestFlags)
bool FCrowdyModelValueCodecRoundTripTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelRichTarget* T = NewObject<UCrowdyGameModelRichTarget>(GetTransientPackage());
	if (!TestNotNull(TEXT("target created"), T))
	{
		return false;
	}

	FProperty* ScoresProp = RichProp(TEXT("Scores"));
	FProperty* TagsProp = RichProp(TEXT("Tags"));
	FProperty* WeightsProp = RichProp(TEXT("Weights"));
	FProperty* FlagsProp = RichProp(TEXT("Flags"));
	FProperty* EquippedProp = RichProp(TEXT("Equipped"));

	// Encode: a live array reads back as canonical compact JSON, an integral float as integer text.
	T->Scores = { 1, 2, 3 };
	T->Tags = { TEXT("a"), TEXT("b") };
	T->Weights = { 1.5f, 2.0f };
	T->Flags = { true, false };

	FString Out;
	TestTrue(TEXT("int array encodes"), FCrowdyModelValueCodec::EncodePropertyDefaultToJson(ScoresProp, ScoresProp->ContainerPtrToValuePtr<void>(T), Out));
	TestEqual(TEXT("int array default text"), Out, FString(TEXT("[1,2,3]")));
	TestTrue(TEXT("string array encodes"), FCrowdyModelValueCodec::EncodePropertyDefaultToJson(TagsProp, TagsProp->ContainerPtrToValuePtr<void>(T), Out));
	TestEqual(TEXT("string array default text"), Out, FString(TEXT("[\"a\",\"b\"]")));
	TestTrue(TEXT("float array encodes"), FCrowdyModelValueCodec::EncodePropertyDefaultToJson(WeightsProp, WeightsProp->ContainerPtrToValuePtr<void>(T), Out));
	TestEqual(TEXT("float array default text (integral float -> int)"), Out, FString(TEXT("[1.5,2]")));
	TestTrue(TEXT("bool array encodes"), FCrowdyModelValueCodec::EncodePropertyDefaultToJson(FlagsProp, FlagsProp->ContainerPtrToValuePtr<void>(T), Out));
	TestEqual(TEXT("bool array default text"), Out, FString(TEXT("[true,false]")));

	// Encode: a set container ref is a JSON string; an empty ref sends no default.
	T->Equipped.ModelId = TEXT("sword-1");
	TestTrue(TEXT("set ref encodes"), FCrowdyModelValueCodec::EncodePropertyDefaultToJson(EquippedProp, EquippedProp->ContainerPtrToValuePtr<void>(T), Out));
	TestEqual(TEXT("ref default text"), Out, FString(TEXT("\"sword-1\"")));
	T->Equipped.ModelId.Reset();
	TestFalse(TEXT("empty ref sends no default"), FCrowdyModelValueCodec::EncodePropertyDefaultToJson(EquippedProp, EquippedProp->ContainerPtrToValuePtr<void>(T), Out));

	// Decode: server JSON writes onto the live members exactly like a scalar would, and reports success.
	TestTrue(TEXT("valid array decode returns true"), FCrowdyModelValueCodec::DecodeJsonToProperty(ScoresProp, ScoresProp->ContainerPtrToValuePtr<void>(T), Arr({ Num(4), Num(5) })));
	TestEqual(TEXT("int array decoded length"), T->Scores.Num(), 2);
	if (T->Scores.Num() == 2)
	{
		TestEqual(TEXT("int array element 0"), T->Scores[0], 4);
		TestEqual(TEXT("int array element 1"), T->Scores[1], 5);
	}

	FCrowdyModelValueCodec::DecodeJsonToProperty(TagsProp, TagsProp->ContainerPtrToValuePtr<void>(T), Arr({ Str(TEXT("x")), Str(TEXT("y")), Str(TEXT("z")) }));
	TestEqual(TEXT("string array decoded length"), T->Tags.Num(), 3);

	FCrowdyModelValueCodec::DecodeJsonToProperty(FlagsProp, FlagsProp->ContainerPtrToValuePtr<void>(T), Arr({ Boolean(false), Boolean(true) }));
	TestEqual(TEXT("bool array decoded length"), T->Flags.Num(), 2);
	if (T->Flags.Num() == 2)
	{
		TestFalse(TEXT("bool element 0"), T->Flags[0]);
		TestTrue(TEXT("bool element 1"), T->Flags[1]);
	}

	FCrowdyModelValueCodec::DecodeJsonToProperty(EquippedProp, EquippedProp->ContainerPtrToValuePtr<void>(T), Str(TEXT("shield-2")));
	TestEqual(TEXT("ref decoded"), T->Equipped.ModelId, FString(TEXT("shield-2")));

	// A shorter array replaces (not merges) the previous value.
	FCrowdyModelValueCodec::DecodeJsonToProperty(ScoresProp, ScoresProp->ContainerPtrToValuePtr<void>(T), Arr({ Num(9) }));
	TestEqual(TEXT("decode replaces, does not append"), T->Scores.Num(), 1);

	return true;
}

// Decoding treats every value as forged: a non-array for an array property, an over-length array, a
// wrong-typed or nested element, and a non-string for a container_ref are each rejected WHOLE (the live value
// is left untouched, never half-written). Rejections log a warning, which does not fail the test.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelValueCodecForgedInputTest,
	"CrowdySDK.GameModel.RichValueCodecForgedInput", CrowdyModelCodecTestFlags)
bool FCrowdyModelValueCodecForgedInputTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelRichTarget* T = NewObject<UCrowdyGameModelRichTarget>(GetTransientPackage());
	if (!TestNotNull(TEXT("target created"), T))
	{
		return false;
	}

	FProperty* ScoresProp = RichProp(TEXT("Scores"));
	FProperty* EquippedProp = RichProp(TEXT("Equipped"));
	void* ScoresAddr = ScoresProp->ContainerPtrToValuePtr<void>(T);

	// Baseline the live value so every rejection can assert "unchanged".
	T->Scores = { 42 };

	// A non-array value for an array property. A rejected decode reports false so the caller notifies nothing.
	TestFalse(TEXT("non-array decode returns false"), FCrowdyModelValueCodec::DecodeJsonToProperty(ScoresProp, ScoresAddr, Num(7)));
	TestEqual(TEXT("non-array rejected: length unchanged"), T->Scores.Num(), 1);
	TestEqual(TEXT("non-array rejected: value unchanged"), T->Scores.IsValidIndex(0) ? T->Scores[0] : -1, 42);

	// A wrong-typed element (a string in an int array) rejects the whole value.
	FCrowdyModelValueCodec::DecodeJsonToProperty(ScoresProp, ScoresAddr, Arr({ Num(1), Str(TEXT("nope")) }));
	TestEqual(TEXT("wrong-element rejected whole: unchanged"), T->Scores.Num(), 1);

	// A nested-array element is not a scalar -> rejected whole (also caps depth at one).
	FCrowdyModelValueCodec::DecodeJsonToProperty(ScoresProp, ScoresAddr, Arr({ Arr({ Num(1) }) }));
	TestEqual(TEXT("nested-array element rejected whole: unchanged"), T->Scores.Num(), 1);

	// An over-length array is rejected whole, never truncated.
	TArray<TSharedPtr<FJsonValue>> TooMany;
	TooMany.Reserve(FCrowdyModelValueCodec::MaxArrayElements + 1);
	for (int32 Index = 0; Index <= FCrowdyModelValueCodec::MaxArrayElements; ++Index)
	{
		TooMany.Add(Num(0));
	}
	FCrowdyModelValueCodec::DecodeJsonToProperty(ScoresProp, ScoresAddr, Arr(MoveTemp(TooMany)));
	TestEqual(TEXT("over-length rejected whole: unchanged"), T->Scores.Num(), 1);

	// An array exactly at the cap is accepted (boundary is inclusive) and reports success.
	TArray<TSharedPtr<FJsonValue>> AtCap;
	AtCap.Reserve(FCrowdyModelValueCodec::MaxArrayElements);
	for (int32 Index = 0; Index < FCrowdyModelValueCodec::MaxArrayElements; ++Index)
	{
		AtCap.Add(Num(1));
	}
	TestTrue(TEXT("at-cap decode returns true"), FCrowdyModelValueCodec::DecodeJsonToProperty(ScoresProp, ScoresAddr, Arr(MoveTemp(AtCap))));
	TestEqual(TEXT("at-cap accepted"), T->Scores.Num(), FCrowdyModelValueCodec::MaxArrayElements);

	// A huge forged number decodes without undefined behavior: the double -> int64 cast saturates instead of
	// invoking UB, so the element is accepted and the array has one element (no crash, no garbage-length).
	TestTrue(TEXT("huge-number element decodes (saturating cast)"), FCrowdyModelValueCodec::DecodeJsonToProperty(ScoresProp, ScoresAddr, Arr({ Num(1e19) })));
	TestEqual(TEXT("huge-number decoded to one element"), T->Scores.Num(), 1);

	// A non-string value for a container_ref leaves the reference untouched and reports false.
	T->Equipped.ModelId = TEXT("keep-me");
	TestFalse(TEXT("ref non-string decode returns false"), FCrowdyModelValueCodec::DecodeJsonToProperty(EquippedProp, EquippedProp->ContainerPtrToValuePtr<void>(T), Num(5)));
	TestEqual(TEXT("ref non-string rejected: unchanged"), T->Equipped.ModelId, FString(TEXT("keep-me")));

	return true;
}

// The object codec round-trips a plain-struct attribute: encode a live struct to canonical sorted-key JSON
// (integral float member as integer text, nested struct + scalar array inline), then decode a server object whose
// keys arrive in a DIFFERENT order (one lower-cased, since FJsonObject lookup is case-insensitive) back onto a
// fresh struct.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelValueCodecObjectRoundTripTest,
	"CrowdySDK.GameModel.RichValueCodecObjectRoundTrip", CrowdyModelCodecTestFlags)
bool FCrowdyModelValueCodecObjectRoundTripTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelRichTarget* T = NewObject<UCrowdyGameModelRichTarget>(GetTransientPackage());
	if (!TestNotNull(TEXT("target created"), T))
	{
		return false;
	}

	FProperty* StatsProp = RichProp(TEXT("Stats"));
	void* StatsAddr = StatsProp->ContainerPtrToValuePtr<void>(T);

	T->Stats.Strength = 7;
	T->Stats.Focus = 0.5f;
	T->Stats.Title = TEXT("hero");
	T->Stats.Offset.X = 1.5f;
	T->Stats.Offset.Y = 2.0f;
	T->Stats.Ranks = { 3, 4 };

	FString Out;
	TestTrue(TEXT("object encodes"), FCrowdyModelValueCodec::EncodePropertyDefaultToJson(StatsProp, StatsAddr, Out));
	TestEqual(TEXT("object default text (sorted keys, integral float -> int)"), Out,
		FString(TEXT("{\"Focus\":0.5,\"Offset\":{\"X\":1.5,\"Y\":2},\"Ranks\":[3,4],\"Strength\":7,\"Title\":\"hero\"}")));

	// Decode a server object whose keys arrive in a different order (and one lower-cased) onto a fresh struct.
	T->Stats = FCrowdyGameModelTestStats();
	const TSharedPtr<FJsonObject> In = MakeShared<FJsonObject>();
	In->SetStringField(TEXT("Title"), TEXT("wizard"));
	In->SetNumberField(TEXT("strength"), 12); // lower-cased key: FJsonObject lookup is case-insensitive
	In->SetNumberField(TEXT("Focus"), 0.25);
	const TSharedPtr<FJsonObject> InOffset = MakeShared<FJsonObject>();
	InOffset->SetNumberField(TEXT("X"), 3.0);
	InOffset->SetNumberField(TEXT("Y"), 4.5);
	In->SetObjectField(TEXT("Offset"), InOffset);
	In->SetArrayField(TEXT("Ranks"), { Num(5), Num(6), Num(7) });

	TestTrue(TEXT("valid object decode returns true"),
		FCrowdyModelValueCodec::DecodeJsonToProperty(StatsProp, StatsAddr, ObjVal(In)));
	TestEqual(TEXT("scalar member decoded (case-insensitive key)"), T->Stats.Strength, 12);
	TestEqual(TEXT("float member decoded"), T->Stats.Focus, 0.25f);
	TestEqual(TEXT("string member decoded"), T->Stats.Title, FString(TEXT("wizard")));
	TestEqual(TEXT("nested struct member X decoded"), T->Stats.Offset.X, 3.0f);
	TestEqual(TEXT("nested struct member Y decoded"), T->Stats.Offset.Y, 4.5f);
	TestEqual(TEXT("array member decoded length"), T->Stats.Ranks.Num(), 3);
	if (T->Stats.Ranks.Num() == 3)
	{
		TestEqual(TEXT("array member last element"), T->Stats.Ranks[2], 7);
	}

	return true;
}

// The object codec treats every value as forged and rejects WHOLE: a non-object value, a wrong-typed member, and
// a missing member each leave the live struct untouched (never half-written) and report false.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelValueCodecObjectForgedInputTest,
	"CrowdySDK.GameModel.RichValueCodecObjectForgedInput", CrowdyModelCodecTestFlags)
bool FCrowdyModelValueCodecObjectForgedInputTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelRichTarget* T = NewObject<UCrowdyGameModelRichTarget>(GetTransientPackage());
	if (!TestNotNull(TEXT("target created"), T))
	{
		return false;
	}

	FProperty* StatsProp = RichProp(TEXT("Stats"));
	void* StatsAddr = StatsProp->ContainerPtrToValuePtr<void>(T);

	// Baseline the live struct so every rejection can assert "unchanged".
	T->Stats.Strength = 99;
	T->Stats.Title = TEXT("baseline");

	// A complete, valid object body so the ONLY reason to reject is the mutation each case introduces.
	auto MakeComplete = []() -> TSharedPtr<FJsonObject>
	{
		const TSharedPtr<FJsonObject> Stats = MakeShared<FJsonObject>();
		Stats->SetNumberField(TEXT("Strength"), 1);
		Stats->SetNumberField(TEXT("Focus"), 1.0);
		Stats->SetStringField(TEXT("Title"), TEXT("changed"));
		const TSharedPtr<FJsonObject> Offset = MakeShared<FJsonObject>();
		Offset->SetNumberField(TEXT("X"), 0.0);
		Offset->SetNumberField(TEXT("Y"), 0.0);
		Stats->SetObjectField(TEXT("Offset"), Offset);
		Stats->SetArrayField(TEXT("Ranks"), { Num(1) });
		return Stats;
	};

	// A non-object value for an object property is rejected whole.
	TestFalse(TEXT("non-object decode returns false"),
		FCrowdyModelValueCodec::DecodeJsonToProperty(StatsProp, StatsAddr, Num(5)));
	TestEqual(TEXT("non-object rejected: scalar unchanged"), T->Stats.Strength, 99);
	TestEqual(TEXT("non-object rejected: string unchanged"), T->Stats.Title, FString(TEXT("baseline")));

	// A member of the wrong type (Strength as a string) rejects the whole object.
	const TSharedPtr<FJsonObject> WrongType = MakeComplete();
	WrongType->SetStringField(TEXT("Strength"), TEXT("nope"));
	TestFalse(TEXT("wrong-typed member decode returns false"),
		FCrowdyModelValueCodec::DecodeJsonToProperty(StatsProp, StatsAddr, ObjVal(WrongType)));
	TestEqual(TEXT("wrong-typed member rejected whole: scalar unchanged"), T->Stats.Strength, 99);
	TestEqual(TEXT("wrong-typed member rejected whole: string unchanged"), T->Stats.Title, FString(TEXT("baseline")));

	// A missing member (no "Strength") rejects the whole object.
	const TSharedPtr<FJsonObject> Missing = MakeComplete();
	Missing->RemoveField(TEXT("Strength"));
	TestFalse(TEXT("missing member decode returns false"),
		FCrowdyModelValueCodec::DecodeJsonToProperty(StatsProp, StatsAddr, ObjVal(Missing)));
	TestEqual(TEXT("missing member rejected whole: scalar unchanged"), T->Stats.Strength, 99);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
