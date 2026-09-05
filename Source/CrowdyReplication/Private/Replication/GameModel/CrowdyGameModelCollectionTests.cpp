// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Replication/GameModel/CrowdyGameModel.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyCollectionTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

// UCrowdyGameModel::GetItem* read a scalar out of a collection item's state JSON (from Get Collection With Items'
// State), returning the supplied default on a missing key, a type mismatch, or malformed/empty JSON. Pure parse.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCollectionItemGettersTest,
	"CrowdySDK.GameModel.CollectionItemGetters", CrowdyCollectionTestFlags)
bool FCrowdyCollectionItemGettersTest::RunTest(const FString& Parameters)
{
	const FString State = TEXT("{\"quantity\":5,\"weight\":2.5,\"equipped\":true,\"item_id\":\"sword\"}");

	TestEqual(TEXT("int field"), UCrowdyGameModel::GetItemInt(State, TEXT("quantity"), 0), 5);
	TestEqual(TEXT("float field"), UCrowdyGameModel::GetItemFloat(State, TEXT("weight"), 0.0f), 2.5f);
	TestTrue(TEXT("bool field"), UCrowdyGameModel::GetItemBool(State, TEXT("equipped"), false));
	TestEqual(TEXT("string field"), UCrowdyGameModel::GetItemString(State, TEXT("item_id"), FString()), FString(TEXT("sword")));

	// A missing key returns the default.
	TestEqual(TEXT("missing key -> default"), UCrowdyGameModel::GetItemInt(State, TEXT("nope"), 99), 99);
	// A type mismatch returns the default (item_id is a string, read as an int).
	TestEqual(TEXT("type mismatch -> default"), UCrowdyGameModel::GetItemInt(State, TEXT("item_id"), 42), 42);
	// A bool read of a number is a mismatch.
	TestFalse(TEXT("number read as bool -> default"), UCrowdyGameModel::GetItemBool(State, TEXT("quantity"), false));

	// Empty and malformed JSON both return the default (never crash).
	TestEqual(TEXT("empty JSON -> default"), UCrowdyGameModel::GetItemInt(FString(), TEXT("quantity"), 7), 7);
	TestEqual(TEXT("malformed JSON -> default"), UCrowdyGameModel::GetItemInt(TEXT("{not valid"), TEXT("quantity"), 7), 7);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
