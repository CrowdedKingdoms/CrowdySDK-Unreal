// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/CrowdyGameModelSubsystem.h"
#include "Replication/GameModel/CrowdyGameModelTestTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "Network/GraphQL/FCrowdyGameApiCodec.h" // FCrowdyMutationApplied
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
// "container_ref". An array of a struct element and a plain (non-ref) struct stay unsupported (empty) they ride
// N2b, not this slice.
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

// Through the subsystem apply path: a pulled array value writes the live TArray and fires its OnRep once, an
// identical re-apply is silent (shared canonical cache), and a container_ref write fires its OnRep too.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelValueCodecApplyTest,
	"CrowdySDK.GameModel.RichApplyWritesArrayAndFiresOnRep", CrowdyModelCodecTestFlags)
bool FCrowdyModelValueCodecApplyTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	UCrowdyGameModelRichTarget* T = NewObject<UCrowdyGameModelRichTarget>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model) || !TestNotNull(TEXT("target created"), T))
	{
		return false;
	}

	const FGuid NetID = FGuid::NewGuid();

	const TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
	State->SetArrayField(TEXT("scores"), { Num(1), Num(2), Num(3) });
	Model->ApplyStateToContainer(NetID, T, State);
	TestEqual(TEXT("array member written"), T->Scores.Num(), 3);
	TestEqual(TEXT("array OnRep fired once"), T->ScoresOnRepCount, 1);

	// Re-apply identical values -> the canonical cache matches, no further OnRep.
	Model->ApplyStateToContainer(NetID, T, State);
	TestEqual(TEXT("array OnRep not re-fired on unchanged"), T->ScoresOnRepCount, 1);

	// Change one element -> OnRep fires again and the member reflects it.
	const TSharedPtr<FJsonObject> Changed = MakeShared<FJsonObject>();
	Changed->SetArrayField(TEXT("scores"), { Num(1), Num(2), Num(4) });
	Model->ApplyStateToContainer(NetID, T, Changed);
	TestEqual(TEXT("array OnRep fires on change"), T->ScoresOnRepCount, 2);
	TestEqual(TEXT("array element updated"), T->Scores.IsValidIndex(2) ? T->Scores[2] : -1, 4);

	// A container_ref pull writes the ref member and fires its OnRep.
	const TSharedPtr<FJsonObject> RefState = MakeShared<FJsonObject>();
	RefState->SetStringField(TEXT("equipped"), TEXT("weapon-1"));
	Model->ApplyStateToContainer(NetID, T, RefState);
	TestEqual(TEXT("ref member written"), T->Equipped.ModelId, FString(TEXT("weapon-1")));
	TestEqual(TEXT("ref OnRep fired once"), T->EquippedOnRepCount, 1);

	return true;
}

// A forged aggregate pull that the codec REJECTS must not fire OnRep, must not poison the diff cache, and must
// not broadcast a change: the member stays put and a later valid pull still applies. Locks the fold of the
// review's cross-lens finding (the apply seam must gate its notifications on an actual write, not on "is an
// attribute"). The codec logs the rejection at Warning level, which does not fail an automation run.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelValueCodecRejectedApplyTest,
	"CrowdySDK.GameModel.RichRejectedApplyDoesNotNotify", CrowdyModelCodecTestFlags)
bool FCrowdyModelValueCodecRejectedApplyTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	UCrowdyGameModelRichTarget* T = NewObject<UCrowdyGameModelRichTarget>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model) || !TestNotNull(TEXT("target created"), T))
	{
		return false;
	}

	const FGuid NetID = FGuid::NewGuid();

	// Land a valid array first: member set, OnRep once.
	const TSharedPtr<FJsonObject> Good = MakeShared<FJsonObject>();
	Good->SetArrayField(TEXT("scores"), { Num(1), Num(2), Num(3) });
	Model->ApplyStateToContainer(NetID, T, Good);
	TestEqual(TEXT("valid pull applied"), T->Scores.Num(), 3);
	TestEqual(TEXT("valid pull fired OnRep once"), T->ScoresOnRepCount, 1);

	// A forged pull for the same key (wrong element type) is rejected by the codec.
	const TSharedPtr<FJsonObject> Forged = MakeShared<FJsonObject>();
	Forged->SetArrayField(TEXT("scores"), { Str(TEXT("x")), Str(TEXT("y")) });
	Model->ApplyStateToContainer(NetID, T, Forged);
	// The member is unchanged and NO extra OnRep fired: the rejected value never masqueraded as a change.
	TestEqual(TEXT("forged pull left member unchanged"), T->Scores.Num(), 3);
	TestEqual(TEXT("forged pull fired NO OnRep"), T->ScoresOnRepCount, 1);

	// The cache was not poisoned: re-pulling the ORIGINAL value is a silent no-op (still matches the cache). Had
	// the rejected value overwritten the cache, this re-pull would differ from the poison and fire OnRep again.
	Model->ApplyStateToContainer(NetID, T, Good);
	TestEqual(TEXT("original re-pull is a no-op (cache intact)"), T->ScoresOnRepCount, 1);

	// A genuinely new valid value still applies + fires, proving the seam is not wedged.
	const TSharedPtr<FJsonObject> Next = MakeShared<FJsonObject>();
	Next->SetArrayField(TEXT("scores"), { Num(9) });
	Model->ApplyStateToContainer(NetID, T, Next);
	TestEqual(TEXT("later valid pull applies"), T->Scores.Num(), 1);
	TestEqual(TEXT("later valid pull fires OnRep"), T->ScoresOnRepCount, 2);

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

// Through the subsystem apply path: an object pull writes the live struct member and fires its OnRep once; an
// identical re-apply built with SCRAMBLED key order is still a no-op (the sorted-key canonical cache), and a
// member change re-fires. This locks the JsonValueToCompactString object canonicalization.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelValueCodecObjectApplyTest,
	"CrowdySDK.GameModel.RichApplyWritesObjectAndFiresOnRep", CrowdyModelCodecTestFlags)
bool FCrowdyModelValueCodecObjectApplyTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	UCrowdyGameModelRichTarget* T = NewObject<UCrowdyGameModelRichTarget>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model) || !TestNotNull(TEXT("target created"), T))
	{
		return false;
	}

	const FGuid NetID = FGuid::NewGuid();

	// The same logical object, members inserted in declaration order.
	auto MakeStats = [](int32 Strength) -> TSharedPtr<FJsonObject>
	{
		const TSharedPtr<FJsonObject> Stats = MakeShared<FJsonObject>();
		Stats->SetNumberField(TEXT("Strength"), Strength);
		Stats->SetNumberField(TEXT("Focus"), 1.0);
		Stats->SetStringField(TEXT("Title"), TEXT("knight"));
		const TSharedPtr<FJsonObject> Offset = MakeShared<FJsonObject>();
		Offset->SetNumberField(TEXT("X"), 1.0);
		Offset->SetNumberField(TEXT("Y"), 2.0);
		Stats->SetObjectField(TEXT("Offset"), Offset);
		Stats->SetArrayField(TEXT("Ranks"), { Num(1), Num(2) });
		return Stats;
	};
	// The SAME logical object, members inserted in reverse order (a server re-serialization may reorder keys).
	auto MakeStatsScrambled = [](int32 Strength) -> TSharedPtr<FJsonObject>
	{
		const TSharedPtr<FJsonObject> Stats = MakeShared<FJsonObject>();
		Stats->SetArrayField(TEXT("Ranks"), { Num(1), Num(2) });
		const TSharedPtr<FJsonObject> Offset = MakeShared<FJsonObject>();
		Offset->SetNumberField(TEXT("Y"), 2.0);
		Offset->SetNumberField(TEXT("X"), 1.0);
		Stats->SetObjectField(TEXT("Offset"), Offset);
		Stats->SetStringField(TEXT("Title"), TEXT("knight"));
		Stats->SetNumberField(TEXT("Focus"), 1.0);
		Stats->SetNumberField(TEXT("Strength"), Strength);
		return Stats;
	};

	const TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
	State->SetObjectField(TEXT("stats"), MakeStats(5));
	Model->ApplyStateToContainer(NetID, T, State);
	TestEqual(TEXT("object member written"), T->Stats.Strength, 5);
	TestEqual(TEXT("nested member written"), T->Stats.Offset.Y, 2.0f);
	TestEqual(TEXT("object OnRep fired once"), T->StatsOnRepCount, 1);

	// Re-apply the same object with keys in a DIFFERENT order -> canonical cache matches, no further OnRep.
	const TSharedPtr<FJsonObject> Reordered = MakeShared<FJsonObject>();
	Reordered->SetObjectField(TEXT("stats"), MakeStatsScrambled(5));
	Model->ApplyStateToContainer(NetID, T, Reordered);
	TestEqual(TEXT("object OnRep not re-fired on unchanged (key-order-independent)"), T->StatsOnRepCount, 1);

	// Change one member -> OnRep fires again and the member reflects it.
	const TSharedPtr<FJsonObject> Changed = MakeShared<FJsonObject>();
	Changed->SetObjectField(TEXT("stats"), MakeStats(9));
	Model->ApplyStateToContainer(NetID, T, Changed);
	TestEqual(TEXT("object OnRep fires on change"), T->StatsOnRepCount, 2);
	TestEqual(TEXT("object member updated"), T->Stats.Strength, 9);

	return true;
}

// A forged object pull that the codec REJECTS must not fire OnRep, must not poison the diff cache, and must not
// broadcast a change: the member stays put and a later valid pull still applies (the object analogue of the array
// reject-then-no-notify fold).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelValueCodecRejectedObjectApplyTest,
	"CrowdySDK.GameModel.RichRejectedObjectApplyDoesNotNotify", CrowdyModelCodecTestFlags)
bool FCrowdyModelValueCodecRejectedObjectApplyTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	UCrowdyGameModelRichTarget* T = NewObject<UCrowdyGameModelRichTarget>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model) || !TestNotNull(TEXT("target created"), T))
	{
		return false;
	}

	const FGuid NetID = FGuid::NewGuid();

	auto MakeStats = [](int32 Strength, const FString& Title) -> TSharedPtr<FJsonObject>
	{
		const TSharedPtr<FJsonObject> Stats = MakeShared<FJsonObject>();
		Stats->SetNumberField(TEXT("Strength"), Strength);
		Stats->SetNumberField(TEXT("Focus"), 1.0);
		Stats->SetStringField(TEXT("Title"), Title);
		const TSharedPtr<FJsonObject> Offset = MakeShared<FJsonObject>();
		Offset->SetNumberField(TEXT("X"), 0.0);
		Offset->SetNumberField(TEXT("Y"), 0.0);
		Stats->SetObjectField(TEXT("Offset"), Offset);
		Stats->SetArrayField(TEXT("Ranks"), { Num(1) });
		return Stats;
	};

	// Land a valid object first: member set, OnRep once.
	const TSharedPtr<FJsonObject> Good = MakeShared<FJsonObject>();
	Good->SetObjectField(TEXT("stats"), MakeStats(3, TEXT("valid")));
	Model->ApplyStateToContainer(NetID, T, Good);
	TestEqual(TEXT("valid object applied"), T->Stats.Strength, 3);
	TestEqual(TEXT("valid object fired OnRep once"), T->StatsOnRepCount, 1);

	// A forged object for the same key (Strength wrong-typed) is rejected by the codec.
	const TSharedPtr<FJsonObject> ForgedStats = MakeStats(0, TEXT("hacked"));
	ForgedStats->SetStringField(TEXT("Strength"), TEXT("boom")); // overwrite Strength with a string
	const TSharedPtr<FJsonObject> Forged = MakeShared<FJsonObject>();
	Forged->SetObjectField(TEXT("stats"), ForgedStats);
	Model->ApplyStateToContainer(NetID, T, Forged);
	TestEqual(TEXT("forged object left scalar unchanged"), T->Stats.Strength, 3);
	TestEqual(TEXT("forged object left string unchanged"), T->Stats.Title, FString(TEXT("valid")));
	TestEqual(TEXT("forged object fired NO OnRep"), T->StatsOnRepCount, 1);

	// The cache was not poisoned: re-pulling the ORIGINAL value is a silent no-op.
	const TSharedPtr<FJsonObject> Again = MakeShared<FJsonObject>();
	Again->SetObjectField(TEXT("stats"), MakeStats(3, TEXT("valid")));
	Model->ApplyStateToContainer(NetID, T, Again);
	TestEqual(TEXT("original re-pull is a no-op (cache intact)"), T->StatsOnRepCount, 1);

	// A genuinely new valid value still applies + fires, proving the seam is not wedged.
	const TSharedPtr<FJsonObject> Next = MakeShared<FJsonObject>();
	Next->SetObjectField(TEXT("stats"), MakeStats(8, TEXT("next")));
	Model->ApplyStateToContainer(NetID, T, Next);
	TestEqual(TEXT("later valid object applies"), T->Stats.Strength, 8);
	TestEqual(TEXT("later valid object fires OnRep"), T->StatsOnRepCount, 2);

	return true;
}

// A deeply-nested server value runs through the apply seam's canonicalizer (JsonValueToCompactString) without
// overflowing the C++ stack: past its recursion bound the canonicalizer hands the deep subtree to the engine's
// iterative serializer. Uses a depth well above the bound but modest enough that the test's own DOM teardown is
// safe. Returning here (no crash) is the assertion.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelValueCodecDeepValueTest,
	"CrowdySDK.GameModel.RichCanonicalizeDeepValueDoesNotCrash", CrowdyModelCodecTestFlags)
bool FCrowdyModelValueCodecDeepValueTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	UCrowdyGameModelRichTarget* T = NewObject<UCrowdyGameModelRichTarget>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model) || !TestNotNull(TEXT("target created"), T))
	{
		return false;
	}

	// A ~200-deep nested array [[[ ... 1 ... ]]] under a key the target does not declare. The apply seam
	// canonicalizes every value before any attribute check, so this drives the canonicalizer past its fallback.
	TSharedPtr<FJsonValue> Deep = Num(1);
	for (int32 Index = 0; Index < 200; ++Index)
	{
		Deep = Arr({ Deep });
	}
	const TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
	State->SetField(TEXT("not_an_attribute"), Deep);

	Model->ApplyStateToContainer(FGuid::NewGuid(), T, State);
	TestTrue(TEXT("deeply-nested value canonicalized without a stack overflow"), true);

	return true;
}

// A forged deeply-nested mutation value (the double-encoded server newValueJson) is rejected before it is
// re-parsed, so its deep DOM is never built and cannot overflow the stack on teardown. Covers the second-layer
// ParseJsonValueString re-parse in ApplyMutationsToContainer, below the transport-boundary guard.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelValueCodecDeepMutationTest,
	"CrowdySDK.GameModel.RichDeepMutationValueDoesNotCrash", CrowdyModelCodecTestFlags)
bool FCrowdyModelValueCodecDeepMutationTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	UCrowdyGameModelRichTarget* T = NewObject<UCrowdyGameModelRichTarget>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model) || !TestNotNull(TEXT("target created"), T))
	{
		return false;
	}

	// A ~5000-deep bracket string as the mutation's newValueJson: without the pre-scan this re-parse would build a
	// deep DOM whose recursive teardown overflows the stack.
	FString DeepJson;
	for (int32 Index = 0; Index < 5000; ++Index)
	{
		DeepJson += TEXT("[");
	}
	DeepJson += TEXT("1");
	for (int32 Index = 0; Index < 5000; ++Index)
	{
		DeepJson += TEXT("]");
	}

	FCrowdyMutationApplied Mutation;
	Mutation.Key = TEXT("scores");
	Mutation.NewValueJson = DeepJson;

	Model->ApplyMutationsToContainer(FGuid::NewGuid(), T, { Mutation });
	TestTrue(TEXT("deep mutation value rejected without a stack overflow"), true);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
