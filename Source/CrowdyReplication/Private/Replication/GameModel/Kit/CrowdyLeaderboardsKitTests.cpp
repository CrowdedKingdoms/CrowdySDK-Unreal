// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/Kit/CrowdyLeaderboardsKitActions.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Replication/GameModel/Kit/CrowdyKitActionSupport.h"
#include "Replication/GameModel/Kit/CrowdyLeaderboardsKitNames.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyLeaderboardsKitTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	FString CrowdyLeaderboardsSerializeObject(const TSharedPtr<FJsonObject>& Object)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
		return Out;
	}

	FCrowdyLeaderboardEntry CrowdyLeaderboardsMakeEntry(int32 Score, const FString& ContainerId)
	{
		FCrowdyLeaderboardEntry Entry;
		Entry.ContainerId = ContainerId;
		Entry.Score = Score;
		return Entry;
	}
}

// The entry type name and the submit function name are derived from the type prefix exactly as the vendored
// leaderboards blueprint derives them: <Prefix>LeaderboardEntry for the type, and "submit_score" (no prefix) or
// snake_case(prefix) + "_submit_score" (with a prefix). A drift here would call a function/type the deployed kit
// never created.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitLeaderboardsTypeNameFromPrefixTest,
	"CrowdySDK.Kit.LeaderboardsTypeNameFromPrefix", CrowdyLeaderboardsKitTestFlags)
bool FCrowdyKitLeaderboardsTypeNameFromPrefixTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("empty prefix -> LeaderboardEntry"),
		CrowdyLeaderboardsKitNames::EntryTypeName(FString()), FString(TEXT("LeaderboardEntry")));
	TestEqual(TEXT("PascalCase prefix concats"),
		CrowdyLeaderboardsKitNames::EntryTypeName(TEXT("Fire")), FString(TEXT("FireLeaderboardEntry")));

	TestEqual(TEXT("empty prefix -> submit_score"),
		CrowdyLeaderboardsKitNames::SubmitScoreFunctionName(FString()), FString(TEXT("submit_score")));
	TestEqual(TEXT("single-word prefix -> lower_submit_score"),
		CrowdyLeaderboardsKitNames::SubmitScoreFunctionName(TEXT("Fire")), FString(TEXT("fire_submit_score")));
	TestEqual(TEXT("PascalCase prefix -> snake_case_submit_score"),
		CrowdyLeaderboardsKitNames::SubmitScoreFunctionName(TEXT("MobEngine")),
		FString(TEXT("mob_engine_submit_score")));

	// toSnakeCase parity with the kit: hyphen/space collapse to one underscore.
	TestEqual(TEXT("hyphen collapses"),
		CrowdyLeaderboardsKitNames::ToSnakeCase(TEXT("Fire-Storm")), FString(TEXT("fire_storm")));

	return true;
}

// submit_score's points is an int: on the wire it is a bare JSON number, not a string and not a nested object.
// This is the load-bearing param shape for a Submit Score.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitLeaderboardsSubmitParamsJsonTest,
	"CrowdySDK.Kit.LeaderboardsSubmitParamsJson", CrowdyLeaderboardsKitTestFlags)
bool FCrowdyKitLeaderboardsSubmitParamsJsonTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Params = UCrowdySubmitScoreAction::BuildSubmitScoreParams(1500);
	if (!TestNotNull(TEXT("params built"), Params.Get()))
	{
		return false;
	}

	const TSharedPtr<FJsonValue> Points = Params->TryGetField(TEXT("points"));
	if (TestNotNull(TEXT("points present"), Points.Get()))
	{
		TestEqual(TEXT("points is a JSON number"),
			static_cast<int32>(Points->Type), static_cast<int32>(EJson::Number));
	}

	TestEqual(TEXT("serialized shape"),
		CrowdyLeaderboardsSerializeObject(Params), FString(TEXT("{\"points\":1500}")));

	return true;
}

// The property keys and defaults must match the vendored leaderboards blueprint so the nodes read/write the exact
// properties the deployed kit created. A rename on either side would silently no-op.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitLeaderboardsNamesMatchKitTest,
	"CrowdySDK.Kit.LeaderboardsNamesMatchKit", CrowdyLeaderboardsKitTestFlags)
bool FCrowdyKitLeaderboardsNamesMatchKitTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("owner_user_id key"),
		FString(CrowdyLeaderboardsKitNames::Keys::OwnerUserId), FString(TEXT("owner_user_id")));
	TestEqual(TEXT("board_id key"),
		FString(CrowdyLeaderboardsKitNames::Keys::BoardId), FString(TEXT("board_id")));
	TestEqual(TEXT("score key"), FString(CrowdyLeaderboardsKitNames::Keys::Score), FString(TEXT("score")));
	TestEqual(TEXT("season key"), FString(CrowdyLeaderboardsKitNames::Keys::Season), FString(TEXT("season")));
	TestEqual(TEXT("rank key"), FString(CrowdyLeaderboardsKitNames::Keys::Rank), FString(TEXT("rank")));

	TestEqual(TEXT("entry type suffix"),
		FString(CrowdyLeaderboardsKitNames::EntryTypeSuffix), FString(TEXT("LeaderboardEntry")));

	TestEqual(TEXT("score default"), CrowdyLeaderboardsKitNames::Defaults::Score, 0);
	TestEqual(TEXT("season default"), CrowdyLeaderboardsKitNames::Defaults::Season, 1);
	TestEqual(TEXT("rank default"), CrowdyLeaderboardsKitNames::Defaults::Rank, 0);

	return true;
}

// The entry parser reads board_id/score/season/rank out of a flat pulled property map and threads the row header
// (container id, display name, owner) through. Missing keys leave the struct defaults (season starts at 1).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitLeaderboardsParseEntryTest,
	"CrowdySDK.Kit.LeaderboardsParseEntry", CrowdyLeaderboardsKitTestFlags)
bool FCrowdyKitLeaderboardsParseEntryTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
	State->SetStringField(TEXT("board_id"), TEXT("weekly_kills"));
	State->SetNumberField(TEXT("score"), 4200);
	State->SetNumberField(TEXT("season"), 3);
	State->SetNumberField(TEXT("rank"), 7);

	const FCrowdyLeaderboardEntry Parsed =
		UCrowdyGetLeaderboardAction::ParseLeaderboardEntry(State, TEXT("c-1"), TEXT("Aria"), TEXT("99"));
	TestEqual(TEXT("board id"), Parsed.BoardId, FString(TEXT("weekly_kills")));
	TestEqual(TEXT("score"), Parsed.Score, 4200);
	TestEqual(TEXT("season"), Parsed.Season, 3);
	TestEqual(TEXT("rank"), Parsed.Rank, 7);
	TestEqual(TEXT("container id threaded"), Parsed.ContainerId, FString(TEXT("c-1")));
	TestEqual(TEXT("display name threaded"), Parsed.DisplayName, FString(TEXT("Aria")));
	TestEqual(TEXT("owner threaded"), Parsed.OwnerUserId, FString(TEXT("99")));

	// A brand-new entry with only board_id present; the unread stats stay at their struct defaults.
	TSharedPtr<FJsonObject> Fresh = MakeShared<FJsonObject>();
	Fresh->SetStringField(TEXT("board_id"), TEXT("weekly_kills"));
	const FCrowdyLeaderboardEntry New =
		UCrowdyGetLeaderboardAction::ParseLeaderboardEntry(Fresh, TEXT("c-2"), FString(), FString());
	TestEqual(TEXT("score default when absent"), New.Score, 0);
	TestEqual(TEXT("season default when absent"), New.Season, 1);
	TestEqual(TEXT("rank default when absent"), New.Rank, 0);

	return true;
}

// A forged/out-of-range score must saturate to the int32 bounds, not invoke undefined double-to-int narrowing (a
// garbage sentinel on MSVC). The server never sends these; a hostile/broken peer or replay could.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitLeaderboardsParseEntrySaturatesTest,
	"CrowdySDK.Kit.LeaderboardsParseEntrySaturates", CrowdyLeaderboardsKitTestFlags)
bool FCrowdyKitLeaderboardsParseEntrySaturatesTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Huge = MakeShared<FJsonObject>();
	Huge->SetNumberField(TEXT("score"), 5.0e18);   // far above INT32_MAX
	Huge->SetNumberField(TEXT("season"), -5.0e18); // far below INT32_MIN
	Huge->SetNumberField(TEXT("rank"), 3.0e9);     // above INT32_MAX (2147483647)

	const FCrowdyLeaderboardEntry Parsed =
		UCrowdyGetLeaderboardAction::ParseLeaderboardEntry(Huge, TEXT("c-forged"), FString(), FString());
	TestEqual(TEXT("score saturates to int32 max"), Parsed.Score, MAX_int32);
	TestEqual(TEXT("season saturates to int32 min"), Parsed.Season, MIN_int32);
	TestEqual(TEXT("rank saturates to int32 max"), Parsed.Rank, MAX_int32);

	// An in-range value is unaffected by the clamp.
	TSharedPtr<FJsonObject> InRange = MakeShared<FJsonObject>();
	InRange->SetNumberField(TEXT("score"), 250);
	const FCrowdyLeaderboardEntry Ok =
		UCrowdyGetLeaderboardAction::ParseLeaderboardEntry(InRange, TEXT("c-ok"), FString(), FString());
	TestEqual(TEXT("in-range score passes through"), Ok.Score, 250);

	return true;
}

// Leaderboard reads have no server-side ORDER BY, so ranking is client-side: sort by score descending, then stamp
// 1-based positions. A tie must keep the listed order (stable), so a read is deterministic between calls.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitLeaderboardsRankEntriesTest,
	"CrowdySDK.Kit.LeaderboardsRankEntries", CrowdyLeaderboardsKitTestFlags)
bool FCrowdyKitLeaderboardsRankEntriesTest::RunTest(const FString& Parameters)
{
	TArray<FCrowdyLeaderboardEntry> Entries;
	Entries.Add(CrowdyLeaderboardsMakeEntry(50, TEXT("mid")));
	Entries.Add(CrowdyLeaderboardsMakeEntry(90, TEXT("top")));
	Entries.Add(CrowdyLeaderboardsMakeEntry(50, TEXT("mid-tie")));  // same score as "mid", listed after it
	Entries.Add(CrowdyLeaderboardsMakeEntry(10, TEXT("low")));

	UCrowdyGetLeaderboardAction::RankEntries(Entries);

	if (!TestEqual(TEXT("entry count preserved"), Entries.Num(), 4))
	{
		return false;
	}
	TestEqual(TEXT("best score first"), Entries[0].ContainerId, FString(TEXT("top")));
	TestEqual(TEXT("first tie keeps listed order"), Entries[1].ContainerId, FString(TEXT("mid")));
	TestEqual(TEXT("second tie after first"), Entries[2].ContainerId, FString(TEXT("mid-tie")));
	TestEqual(TEXT("lowest last"), Entries[3].ContainerId, FString(TEXT("low")));

	TestEqual(TEXT("position 1"), Entries[0].Position, 1);
	TestEqual(TEXT("position 2"), Entries[1].Position, 2);
	TestEqual(TEXT("position 3"), Entries[2].Position, 3);
	TestEqual(TEXT("position 4"), Entries[3].Position, 4);

	return true;
}

// A string property's ValueJson must stay valid JSON even when the string carries characters that are special in a
// JSON string literal. A crafted value with a quote, a backslash, and control characters must escape rather than
// break out of the literal, and must round-trip back to the original text through a real JSON parse.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitLeaderboardsStringValueJsonEscapesTest,
	"CrowdySDK.Kit.LeaderboardsStringValueJsonEscapes", CrowdyLeaderboardsKitTestFlags)
bool FCrowdyKitLeaderboardsStringValueJsonEscapesTest::RunTest(const FString& Parameters)
{
	const FString Crafted = FString(TEXT("a\"b\\c")) + TEXT("\n") + TEXT("\t") + FString::Chr(0x01) + TEXT("z");
	const FString ValueJson = CrowdyKitActionSupport::MakeStringValueJson(Crafted);

	// Parse it back as the value of a one-field object; a malformed escape would fail the parse or corrupt the text.
	const FString Wrapped = FString::Printf(TEXT("{\"v\":%s}"), *ValueJson);
	TSharedPtr<FJsonObject> Parsed;
	const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Wrapped);
	if (!TestTrue(TEXT("escaped ValueJson parses as valid JSON"), FJsonSerializer::Deserialize(Reader, Parsed)
		&& Parsed.IsValid()))
	{
		return false;
	}
	FString RoundTripped;
	TestTrue(TEXT("value field present"), Parsed->TryGetStringField(TEXT("v"), RoundTripped));
	TestEqual(TEXT("string round-trips unchanged"), RoundTripped, Crafted);

	// A plain identifier is wrapped in quotes with no escaping surprises.
	TestEqual(TEXT("plain string is quoted"),
		CrowdyKitActionSupport::MakeStringValueJson(TEXT("weekly_kills")), FString(TEXT("\"weekly_kills\"")));

	return true;
}

#endif
