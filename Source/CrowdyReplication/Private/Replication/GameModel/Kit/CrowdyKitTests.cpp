// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "Replication/GameModel/CrowdyGameModelTestSupport.h"
#include "Replication/GameModel/Kit/CrowdyKitBlueprint.h"
#include "Replication/GameModel/Kit/CrowdyKitInventory.h"
#include "Replication/GameModel/Kit/CrowdyKitJson.h"
#include "Replication/GameModel/Kit/CrowdyKitPolicy.h"

// The golden literals below are copied byte for byte from the sibling SDK's tests/kit_test.cpp so the two
// blueprint builders emit identical wire text.

namespace
{
	constexpr EAutomationTestFlags CrowdyKitTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

// Mirrors kit_test.cpp testPolicyJson.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitPolicyJsonParityTest,
	"CrowdySDK.Replication.KitPolicyJsonParity", CrowdyKitTestFlags)
bool FCrowdyKitPolicyJsonParityTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyKit;

	TestEqual(TEXT("owner_of_self"), KitPolicyJson(OwnerOfSelfPolicy()), FString(TEXT("{\"type\":\"owner_of_self\"}")));
	TestEqual(TEXT("condition"), KitPolicyJson(ConditionPolicy(TEXT("$amount > 0"))),
		FString(TEXT("{\"expression\":\"$amount > 0\",\"type\":\"condition\"}")));
	TestEqual(TEXT("tier_feature"), KitPolicyJson(FeatureGate(TEXT("land_owner"))),
		FString(TEXT("{\"feature\":\"land_owner\",\"type\":\"tier_feature\"}")));

	const TArray<TSharedPtr<FJsonObject>> NullExtras = {TSharedPtr<FJsonObject>(), TSharedPtr<FJsonObject>()};
	TestEqual(TEXT("andPolicies collapses to a single rule"),
		KitPolicyJson(AndPolicies(OwnerOfSelfPolicy(), NullExtras)), FString(TEXT("{\"type\":\"owner_of_self\"}")));

	const TSharedPtr<FJsonObject> Combined = AndPolicies(OwnerOfSelfPolicy(), {FeatureGate(TEXT("vip"))});
	const TSharedPtr<FJsonObject> Parsed = ParseObject(KitPolicyJson(Combined));
	TestTrue(TEXT("combined parses"), Parsed.IsValid());
	TestEqual(TEXT("combined type"), Parsed->GetStringField(TEXT("type")), FString(TEXT("and")));
	TestEqual(TEXT("combined rules size"), Parsed->GetArrayField(TEXT("rules")).Num(), 2);

	return true;
}

// Mirrors kit_test.cpp testToSnakeCase.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitSnakeCaseParityTest,
	"CrowdySDK.Replication.KitSnakeCaseParity", CrowdyKitTestFlags)
bool FCrowdyKitSnakeCaseParityTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyKit;

	TestEqual(TEXT("Bank"), ToSnakeCase(TEXT("Bank")), FString(TEXT("bank")));
	TestEqual(TEXT("GuildBank"), ToSnakeCase(TEXT("GuildBank")), FString(TEXT("guild_bank")));
	TestEqual(TEXT("NPCSpawner"), ToSnakeCase(TEXT("NPCSpawner")), FString(TEXT("npcspawner")));
	TestEqual(TEXT("wave2Boss"), ToSnakeCase(TEXT("wave2Boss")), FString(TEXT("wave2_boss")));
	TestEqual(TEXT("with space-dash"), ToSnakeCase(TEXT("with space-dash")), FString(TEXT("with_space_dash")));

	return true;
}

// Mirrors kit_test.cpp testTrustedAuthority.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitTrustedAuthorityParityTest,
	"CrowdySDK.Replication.KitTrustedAuthorityParity", CrowdyKitTestFlags)
bool FCrowdyKitTrustedAuthorityParityTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyKit;

	TSharedPtr<FJsonObject> Fn = MakeShared<FJsonObject>();
	ApplyTrustedAuthority(Fn, FCrowdyTrustedAuthority::Server());
	TestEqual(TEXT("server scope"), Fn->GetStringField(TEXT("invokeScope")), FString(TEXT("server")));
	const TSharedPtr<FJsonObject> ServerPolicy = ParseObject(Fn->GetStringField(TEXT("invokePolicyJson")));
	TestTrue(TEXT("server policy parses"), ServerPolicy.IsValid());
	TestEqual(TEXT("server policy type"), ServerPolicy->GetStringField(TEXT("type")), FString(TEXT("allow")));

	TSharedPtr<FJsonObject> Fn2 = MakeShared<FJsonObject>();
	ApplyTrustedAuthority(Fn2, FCrowdyTrustedAuthority::Automation(), TEXT("self.hp > 0"));
	TestTrue(TEXT("autonomous"), Fn2->GetBoolField(TEXT("autonomousInvocable")));
	const TSharedPtr<FJsonObject> AutoPolicy = ParseObject(Fn2->GetStringField(TEXT("invokePolicyJson")));
	TestTrue(TEXT("auto policy parses"), AutoPolicy.IsValid());
	TestEqual(TEXT("auto policy type"), AutoPolicy->GetStringField(TEXT("type")), FString(TEXT("and")));
	const TArray<TSharedPtr<FJsonValue>>& AutoRules = AutoPolicy->GetArrayField(TEXT("rules"));
	TestEqual(TEXT("auto rules size"), AutoRules.Num(), 2);
	TestEqual(TEXT("auto rule 0 type"), AutoRules[0]->AsObject()->GetStringField(TEXT("type")), FString(TEXT("is_automation")));
	TestEqual(TEXT("auto rule 1 expression"), AutoRules[1]->AsObject()->GetStringField(TEXT("expression")),
		FString(TEXT("self.hp > 0")));

	return true;
}

// Mirrors kit_test.cpp testInventoryBlueprintShape.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitInventoryBlueprintParityTest,
	"CrowdySDK.Replication.KitInventoryBlueprintParity", CrowdyKitTestFlags)
bool FCrowdyKitInventoryBlueprintParityTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyKit;

	FCrowdyInventoryBlueprintOptions Options;
	Options.TypePrefix = TEXT("Bank");
	Options.MaxSlots = 10;
	Options.SlotCount = 8;
	const FCrowdyKitBlueprint Bp = InventoryBlueprint(Options);

	TestEqual(TEXT("name"), Bp.Name, FString(TEXT("BankInventory")));
	TestEqual(TEXT("container types"), Bp.ContainerTypes.Num(), 2);
	TestEqual(TEXT("property definitions"), Bp.PropertyDefinitions.Num(), 5);
	TestEqual(TEXT("functions"), Bp.Functions.Num(), 4);

	const FCrowdyKitInventoryNames Names = InventoryNames(TEXT("Bank"));
	TestEqual(TEXT("stack type"), Names.StackType, FString(TEXT("BankItemStack")));
	TestEqual(TEXT("grant fn"), Names.GrantFn, FString(TEXT("bank_grant_stack")));
	TestEqual(TEXT("transfer fn"), Names.TransferFn, FString(TEXT("bank_transfer_stack")));

	TestEqual(TEXT("consume name"), Bp.Functions[1]->GetStringField(TEXT("name")), FString(TEXT("bank_consume_stack")));
	const TSharedPtr<FJsonObject> ConsumePolicy = ParseObject(Bp.Functions[1]->GetStringField(TEXT("invokePolicyJson")));
	TestTrue(TEXT("consume policy parses"), ConsumePolicy.IsValid());
	const TArray<TSharedPtr<FJsonValue>>& ConsumeRules = ConsumePolicy->GetArrayField(TEXT("rules"));
	TestEqual(TEXT("consume guard"), ConsumeRules[1]->AsObject()->GetStringField(TEXT("expression")),
		FString(TEXT("$amount > 0 && self.quantity >= $amount")));

	const TArray<TSharedPtr<FJsonValue>>& MoveMutations = Bp.Functions[2]->GetArrayField(TEXT("mutations"));
	TestEqual(TEXT("move clamp"), MoveMutations[0]->AsObject()->GetStringField(TEXT("expression")),
		FString(TEXT("clamp($to_slot, 0, 7)")));

	TestEqual(TEXT("max_slots default"), Bp.PropertyDefinitions[0]->GetStringField(TEXT("defaultValueJson")),
		FString(TEXT("10")));

	return true;
}

// Mirrors kit_test.cpp testMergeBlueprints and testComposeBlueprints. UE ports the throwing merge to a bool +
// OutErrors contract; the collision message still names the kind so the assertion below can find "container type".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitMergeDuplicateRejectionTest,
	"CrowdySDK.Replication.KitMergeDuplicateRejection", CrowdyKitTestFlags)
bool FCrowdyKitMergeDuplicateRejectionTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyKit;

	FCrowdyInventoryBlueprintOptions BankOptions;
	BankOptions.TypePrefix = TEXT("Bank");

	FCrowdyMergedBlueprints Out;
	TArray<FString> Errors;
	const bool bMerged = MergeBlueprints(TEXT("42"),
		{InventoryBlueprint(), InventoryBlueprint(BankOptions)}, TEXT("sess-1"), Out, Errors);
	TestTrue(TEXT("merge succeeds"), bMerged);
	TestTrue(TEXT("seed input valid"), Out.SeedInput.IsValid());
	TestEqual(TEXT("seed appId"), Out.SeedInput->GetStringField(TEXT("appId")), FString(TEXT("42")));
	TestEqual(TEXT("seed sessionId"), Out.SeedInput->GetStringField(TEXT("sessionId")), FString(TEXT("sess-1")));
	TestEqual(TEXT("seed container types"), Out.SeedInput->GetArrayField(TEXT("containerTypes")).Num(), 4);
	TestEqual(TEXT("seed functions"), Out.SeedInput->GetArrayField(TEXT("functions")).Num(), 8);

	FCrowdyMergedBlueprints Out2;
	TArray<FString> Errors2;
	const bool bMerged2 = MergeBlueprints(TEXT("42"),
		{InventoryBlueprint(), InventoryBlueprint()}, FString(), Out2, Errors2);
	TestFalse(TEXT("duplicate merge fails"), bMerged2);
	bool bMentionsContainerType = false;
	for (const FString& Error : Errors2)
	{
		if (Error.Contains(TEXT("container type")))
		{
			bMentionsContainerType = true;
			break;
		}
	}
	TestTrue(TEXT("error names the kind"), bMentionsContainerType);

	FCrowdyInventoryBlueprintOptions XOptions;
	XOptions.TypePrefix = TEXT("X");
	const FCrowdyKitBlueprint Composite = ComposeBlueprints(TEXT("bundle"),
		{InventoryBlueprint(), InventoryBlueprint(XOptions)});
	TestEqual(TEXT("composite name"), Composite.Name, FString(TEXT("bundle")));
	TestEqual(TEXT("composite container types"), Composite.ContainerTypes.Num(), 4);
	TestEqual(TEXT("composite functions"), Composite.Functions.Num(), 8);

	return true;
}

// Exercises the canonical serializer directly: keys emit in sorted order, condensed, and an integral number
// field carries no decimal point.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitCanonicalJsonSortedKeysTest,
	"CrowdySDK.Replication.KitCanonicalJsonSortedKeys", CrowdyKitTestFlags)
bool FCrowdyKitCanonicalJsonSortedKeysTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("type"), TEXT("condition"));
	Object->SetStringField(TEXT("expression"), TEXT("x"));
	Object->SetNumberField(TEXT("amount"), 10);

	const FString Canonical = CrowdyKitJson::Canonical(Object);
	TestEqual(TEXT("sorted, condensed, integer number"), Canonical,
		FString(TEXT("{\"amount\":10,\"expression\":\"x\",\"type\":\"condition\"}")));
	TestFalse(TEXT("no decimal point"), Canonical.Contains(TEXT("10.0")));

	// A non-integral value serializes at 17 significant digits, byte-matching the sibling SDK's double dump
	// (printf %.17g), not a shortest-round-trip form.
	TSharedPtr<FJsonObject> Fractional = MakeShared<FJsonObject>();
	Fractional->SetNumberField(TEXT("rate"), 0.1);
	TestEqual(TEXT("fractional matches %.17g"), CrowdyKitJson::Canonical(Fractional),
		FString(TEXT("{\"rate\":0.10000000000000001}")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
