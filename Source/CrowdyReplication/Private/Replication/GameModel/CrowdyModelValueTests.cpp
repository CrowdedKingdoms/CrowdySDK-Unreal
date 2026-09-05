// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Replication/GameModel/CrowdyGameModel.h"
#include "Replication/GameModel/CrowdyGameModelSessionTypes.h" // FCrowdyCollectionItem
#include "Replication/GameModel/CrowdyModelValue.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyModelValueTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

// UCrowdyModelValue::As* decode a lone serialized JSON value (the OldValueJson/NewValueJson a change hands out) to a
// typed value, returning the supplied default on an empty, malformed, or wrong-typed value. Pure parse, never crashes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelValueAsTypedTest,
	"CrowdySDK.Replication.ModelValueAsTyped", CrowdyModelValueTestFlags)
bool FCrowdyModelValueAsTypedTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("int number"), UCrowdyModelValue::AsInt(TEXT("5"), 0), 5);
	TestEqual(TEXT("int malformed -> default"), UCrowdyModelValue::AsInt(TEXT("bad"), 9), 9);
	TestEqual(TEXT("int empty -> default"), UCrowdyModelValue::AsInt(FString(), 3), 3);
	// A forged out-of-range number saturates to the int32 bounds rather than an undefined narrowing cast.
	TestEqual(TEXT("int huge -> clamps to max"), UCrowdyModelValue::AsInt(TEXT("1e30"), 0), MAX_int32);
	TestEqual(TEXT("int huge negative -> clamps to min"), UCrowdyModelValue::AsInt(TEXT("-1e30"), 0), MIN_int32);

	TestEqual(TEXT("float number"), UCrowdyModelValue::AsFloat(TEXT("1.5"), 0.0f), 1.5f);
	TestEqual(TEXT("float wrong type -> default"), UCrowdyModelValue::AsFloat(TEXT("true"), 4.0f), 4.0f);

	TestTrue(TEXT("bool true"), UCrowdyModelValue::AsBool(TEXT("true"), false));
	TestTrue(TEXT("bool empty -> default"), UCrowdyModelValue::AsBool(FString(), true));
	TestFalse(TEXT("bool from number -> default"), UCrowdyModelValue::AsBool(TEXT("1"), false));

	TestEqual(TEXT("string content"), UCrowdyModelValue::AsString(TEXT("\"hi\""), FString(TEXT("d"))), FString(TEXT("hi")));
	TestEqual(TEXT("string from number -> default"), UCrowdyModelValue::AsString(TEXT("5"), FString(TEXT("d"))), FString(TEXT("d")));

	return true;
}

// The struct-taking collection getters pull the item's StateJson off the struct and read a scalar field, matching the
// StateJson overloads' default/mismatch behaviour.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCollectionItemTypedGettersTest,
	"CrowdySDK.Replication.CollectionItemTypedGetters", CrowdyModelValueTestFlags)
bool FCrowdyCollectionItemTypedGettersTest::RunTest(const FString& Parameters)
{
	FCrowdyCollectionItem Item;
	Item.ContainerId = TEXT("c1");
	Item.TypeName = TEXT("item");
	Item.StateJson = TEXT("{\"qty\":3,\"name\":\"sword\"}");

	TestEqual(TEXT("int field"), UCrowdyGameModel::GetItemFieldInt(Item, TEXT("qty"), 0), 3);
	TestEqual(TEXT("string field"), UCrowdyGameModel::GetItemFieldString(Item, TEXT("name"), FString()), FString(TEXT("sword")));
	TestEqual(TEXT("missing key -> default"), UCrowdyGameModel::GetItemFieldInt(Item, TEXT("missing"), 7), 7);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
