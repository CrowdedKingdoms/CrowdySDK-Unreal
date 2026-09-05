// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/Kit/CrowdyWorldsimKitActions.h"

#if WITH_DEV_AUTOMATION_TESTS

#include <limits>

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Replication/GameModel/Kit/CrowdyWorldsimKitNames.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	// The unity build co-batches the kit test translation units, so both the flags constant and the serialize
	// helper carry a Worldsim-unique name to avoid a redefinition against another kit's identically-purposed helper.
	constexpr EAutomationTestFlags CrowdyWorldsimKitTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	FString CrowdyWorldsimSerializeObject(const TSharedPtr<FJsonObject>& Object)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
		return Out;
	}
}

// The type names and function names are derived from the type prefix exactly as the vendored worldsim blueprint
// derives them: <Prefix><Suffix> for a type, and the bare base (no prefix) or snake_case(prefix) + "_" + base
// (with a prefix) for a function. A drift here would address a name the deployed kit never created.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitWorldsimNamesFromPrefixTest,
	"CrowdySDK.Kit.WorldsimNamesFromPrefix", CrowdyWorldsimKitTestFlags)
bool FCrowdyKitWorldsimNamesFromPrefixTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("empty prefix -> WorldState"),
		CrowdyWorldsimKitNames::WorldStateTypeName(FString()), FString(TEXT("WorldState")));
	TestEqual(TEXT("empty prefix -> ResourceNode"),
		CrowdyWorldsimKitNames::ResourceNodeTypeName(FString()), FString(TEXT("ResourceNode")));
	TestEqual(TEXT("empty prefix -> Crop"),
		CrowdyWorldsimKitNames::CropTypeName(FString()), FString(TEXT("Crop")));
	TestEqual(TEXT("empty prefix -> WaveSpawner"),
		CrowdyWorldsimKitNames::WaveSpawnerTypeName(FString()), FString(TEXT("WaveSpawner")));

	TestEqual(TEXT("PascalCase prefix concats type"),
		CrowdyWorldsimKitNames::ResourceNodeTypeName(TEXT("Fire")), FString(TEXT("FireResourceNode")));
	TestEqual(TEXT("PascalCase prefix concats crop"),
		CrowdyWorldsimKitNames::CropTypeName(TEXT("MobEngine")), FString(TEXT("MobEngineCrop")));

	TestEqual(TEXT("empty prefix -> gather_node"),
		CrowdyWorldsimKitNames::GatherNodeFunctionName(FString()), FString(TEXT("gather_node")));
	TestEqual(TEXT("empty prefix -> harvest"),
		CrowdyWorldsimKitNames::HarvestFunctionName(FString()), FString(TEXT("harvest")));
	TestEqual(TEXT("empty prefix -> advance_time"),
		CrowdyWorldsimKitNames::AdvanceTimeFunctionName(FString()), FString(TEXT("advance_time")));
	TestEqual(TEXT("empty prefix -> set_weather"),
		CrowdyWorldsimKitNames::SetWeatherFunctionName(FString()), FString(TEXT("set_weather")));
	TestEqual(TEXT("empty prefix -> regen_node"),
		CrowdyWorldsimKitNames::RegenNodeFunctionName(FString()), FString(TEXT("regen_node")));
	TestEqual(TEXT("empty prefix -> grow_crop"),
		CrowdyWorldsimKitNames::GrowCropFunctionName(FString()), FString(TEXT("grow_crop")));
	TestEqual(TEXT("empty prefix -> spawn_wave"),
		CrowdyWorldsimKitNames::SpawnWaveFunctionName(FString()), FString(TEXT("spawn_wave")));

	TestEqual(TEXT("single-word prefix -> fire_gather_node"),
		CrowdyWorldsimKitNames::GatherNodeFunctionName(TEXT("Fire")), FString(TEXT("fire_gather_node")));
	TestEqual(TEXT("single-word prefix -> fire_harvest"),
		CrowdyWorldsimKitNames::HarvestFunctionName(TEXT("Fire")), FString(TEXT("fire_harvest")));
	TestEqual(TEXT("PascalCase prefix -> snake_case gather"),
		CrowdyWorldsimKitNames::GatherNodeFunctionName(TEXT("MobEngine")), FString(TEXT("mob_engine_gather_node")));
	TestEqual(TEXT("PascalCase prefix -> snake_case advance_time"),
		CrowdyWorldsimKitNames::AdvanceTimeFunctionName(TEXT("MobEngine")), FString(TEXT("mob_engine_advance_time")));

	// toSnakeCase parity with the kit: hyphen/space collapse to one underscore.
	TestEqual(TEXT("hyphen collapses"),
		CrowdyWorldsimKitNames::ToSnakeCase(TEXT("Fire-Storm")), FString(TEXT("fire_storm")));

	return true;
}

// The property keys and defaults must match the vendored worldsim blueprint so the nodes read/write the exact
// properties the deployed kit created. A rename on either side would silently no-op.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitWorldsimNamesMatchKitTest,
	"CrowdySDK.Kit.WorldsimNamesMatchKit", CrowdyWorldsimKitTestFlags)
bool FCrowdyKitWorldsimNamesMatchKitTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("time_of_day key"), FString(CrowdyWorldsimKitNames::Keys::TimeOfDay), FString(TEXT("time_of_day")));
	TestEqual(TEXT("day key"), FString(CrowdyWorldsimKitNames::Keys::Day), FString(TEXT("day")));
	TestEqual(TEXT("weather key"), FString(CrowdyWorldsimKitNames::Keys::Weather), FString(TEXT("weather")));
	TestEqual(TEXT("node_id key"), FString(CrowdyWorldsimKitNames::Keys::NodeId), FString(TEXT("node_id")));
	TestEqual(TEXT("resource_item_id key"),
		FString(CrowdyWorldsimKitNames::Keys::ResourceItemId), FString(TEXT("resource_item_id")));
	TestEqual(TEXT("amount key"), FString(CrowdyWorldsimKitNames::Keys::Amount), FString(TEXT("amount")));
	TestEqual(TEXT("max_amount key"), FString(CrowdyWorldsimKitNames::Keys::MaxAmount), FString(TEXT("max_amount")));
	TestEqual(TEXT("regen_rate key"), FString(CrowdyWorldsimKitNames::Keys::RegenRate), FString(TEXT("regen_rate")));
	TestEqual(TEXT("owner_user_id key"),
		FString(CrowdyWorldsimKitNames::Keys::OwnerUserId), FString(TEXT("owner_user_id")));
	TestEqual(TEXT("stage key"), FString(CrowdyWorldsimKitNames::Keys::Stage), FString(TEXT("stage")));
	TestEqual(TEXT("max_stage key"), FString(CrowdyWorldsimKitNames::Keys::MaxStage), FString(TEXT("max_stage")));
	TestEqual(TEXT("output_item_id key"),
		FString(CrowdyWorldsimKitNames::Keys::OutputItemId), FString(TEXT("output_item_id")));
	TestEqual(TEXT("output_qty key"), FString(CrowdyWorldsimKitNames::Keys::OutputQty), FString(TEXT("output_qty")));
	TestEqual(TEXT("next_wave_size key"),
		FString(CrowdyWorldsimKitNames::Keys::NextWaveSize), FString(TEXT("next_wave_size")));

	TestEqual(TEXT("max_amount default"), CrowdyWorldsimKitNames::Defaults::MaxAmount, 100);
	TestEqual(TEXT("regen_rate default"), CrowdyWorldsimKitNames::Defaults::RegenRate, 1);
	TestEqual(TEXT("max_stage default"), CrowdyWorldsimKitNames::Defaults::MaxStage, 3);
	TestEqual(TEXT("output_qty default"), CrowdyWorldsimKitNames::Defaults::OutputQty, 1);
	TestEqual(TEXT("next_wave_size default"), CrowdyWorldsimKitNames::Defaults::NextWaveSize, 5);

	return true;
}

// The gather function takes amount (int, a JSON number) and to_stack_id (container_ref, the bare stack
// container-id STRING). These are the load-bearing param shapes for a Gather Node.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitWorldsimGatherParamsJsonTest,
	"CrowdySDK.Kit.WorldsimGatherParamsJson", CrowdyWorldsimKitTestFlags)
bool FCrowdyKitWorldsimGatherParamsJsonTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Params = UCrowdyGatherNodeAction::BuildGatherParams(7, TEXT("stack-42"));
	if (!TestNotNull(TEXT("params built"), Params.Get()))
	{
		return false;
	}

	const TSharedPtr<FJsonValue> AmountField = Params->TryGetField(TEXT("amount"));
	if (TestNotNull(TEXT("amount present"), AmountField.Get()))
	{
		TestEqual(TEXT("amount is a JSON number"),
			static_cast<int32>(AmountField->Type), static_cast<int32>(EJson::Number));
		TestEqual(TEXT("amount value"), static_cast<int32>(AmountField->AsNumber()), 7);
	}

	const TSharedPtr<FJsonValue> StackField = Params->TryGetField(TEXT("to_stack_id"));
	if (TestNotNull(TEXT("to_stack_id present"), StackField.Get()))
	{
		TestEqual(TEXT("to_stack_id is a JSON string"),
			static_cast<int32>(StackField->Type), static_cast<int32>(EJson::String));
		TestEqual(TEXT("to_stack_id value"), StackField->AsString(), FString(TEXT("stack-42")));
	}

	// An all-digit stack id still round-trips as a string (never coerced to a number).
	const TSharedPtr<FJsonObject> NumericLike = UCrowdyGatherNodeAction::BuildGatherParams(3, TEXT("12345"));
	TestEqual(TEXT("serialized shape"), CrowdyWorldsimSerializeObject(NumericLike),
		FString(TEXT("{\"amount\":3,\"to_stack_id\":\"12345\"}")));

	return true;
}

// The harvest function takes only to_stack_id (container_ref, the bare stack container-id STRING).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitWorldsimHarvestParamsJsonTest,
	"CrowdySDK.Kit.WorldsimHarvestParamsJson", CrowdyWorldsimKitTestFlags)
bool FCrowdyKitWorldsimHarvestParamsJsonTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Params = UCrowdyHarvestCropAction::BuildHarvestParams(TEXT("stack-9"));
	if (!TestNotNull(TEXT("params built"), Params.Get()))
	{
		return false;
	}

	const TSharedPtr<FJsonValue> StackField = Params->TryGetField(TEXT("to_stack_id"));
	if (TestNotNull(TEXT("to_stack_id present"), StackField.Get()))
	{
		TestEqual(TEXT("to_stack_id is a JSON string"),
			static_cast<int32>(StackField->Type), static_cast<int32>(EJson::String));
		TestEqual(TEXT("to_stack_id value"), StackField->AsString(), FString(TEXT("stack-9")));
	}

	TestEqual(TEXT("serialized shape"), CrowdyWorldsimSerializeObject(Params),
		FString(TEXT("{\"to_stack_id\":\"stack-9\"}")));

	return true;
}

// The world-state parser reads time_of_day / day / weather from a flat pulled property map and threads the
// container id through. Weather defaults to "clear" when absent (mirroring the kit's default).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitWorldsimParseWorldStateTest,
	"CrowdySDK.Kit.WorldsimParseWorldState", CrowdyWorldsimKitTestFlags)
bool FCrowdyKitWorldsimParseWorldStateTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
	State->SetNumberField(TEXT("time_of_day"), 13);
	State->SetNumberField(TEXT("day"), 4);
	State->SetStringField(TEXT("weather"), TEXT("storm"));

	const FCrowdyWorldState Parsed = UCrowdyGetWorldStateAction::ParseWorldState(State, TEXT("w-1"));
	TestEqual(TEXT("time_of_day"), Parsed.TimeOfDay, 13);
	TestEqual(TEXT("day"), Parsed.Day, 4);
	TestEqual(TEXT("weather"), Parsed.Weather, FString(TEXT("storm")));
	TestEqual(TEXT("container id threaded"), Parsed.ContainerId, FString(TEXT("w-1")));

	// Weather absent -> defaults to "clear"; the numeric fields default to 0.
	TSharedPtr<FJsonObject> Partial = MakeShared<FJsonObject>();
	Partial->SetNumberField(TEXT("time_of_day"), 6);
	const FCrowdyWorldState Defaulted = UCrowdyGetWorldStateAction::ParseWorldState(Partial, TEXT("w-2"));
	TestEqual(TEXT("time_of_day read"), Defaulted.TimeOfDay, 6);
	TestEqual(TEXT("day default"), Defaulted.Day, 0);
	TestEqual(TEXT("weather default clear"), Defaulted.Weather, FString(TEXT("clear")));

	// A null pulled state yields a defaulted struct that still carries the container id and the clear default.
	const FCrowdyWorldState Empty = UCrowdyGetWorldStateAction::ParseWorldState(nullptr, TEXT("w-3"));
	TestEqual(TEXT("null state container id"), Empty.ContainerId, FString(TEXT("w-3")));
	TestEqual(TEXT("null state weather clear"), Empty.Weather, FString(TEXT("clear")));

	return true;
}

// The resource-node parser reads the ints via the saturating clamp and the coordinates as floats, with the
// container id / display name threaded from the list row. A forged out-of-range amount saturates to int32 bounds
// and a non-finite coordinate coerces to 0, never invoking undefined narrowing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitWorldsimParseResourceNodeTest,
	"CrowdySDK.Kit.WorldsimParseResourceNode", CrowdyWorldsimKitTestFlags)
bool FCrowdyKitWorldsimParseResourceNodeTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
	State->SetStringField(TEXT("node_id"), TEXT("ore_01"));
	State->SetStringField(TEXT("resource_item_id"), TEXT("iron_ore"));
	State->SetNumberField(TEXT("amount"), 40);
	State->SetNumberField(TEXT("max_amount"), 100);
	State->SetNumberField(TEXT("regen_rate"), 2);
	State->SetNumberField(TEXT("x"), 12.5);
	State->SetNumberField(TEXT("y"), -3.25);
	State->SetNumberField(TEXT("z"), 0.0);

	const FCrowdyResourceNode Parsed =
		UCrowdyListResourceNodesAction::ParseResourceNode(State, TEXT("n-1"), TEXT("Iron Vein"));
	TestEqual(TEXT("container id threaded"), Parsed.ContainerId, FString(TEXT("n-1")));
	TestEqual(TEXT("display name threaded"), Parsed.DisplayName, FString(TEXT("Iron Vein")));
	TestEqual(TEXT("node_id"), Parsed.NodeId, FString(TEXT("ore_01")));
	TestEqual(TEXT("resource_item_id"), Parsed.ResourceItemId, FString(TEXT("iron_ore")));
	TestEqual(TEXT("amount"), Parsed.Amount, 40);
	TestEqual(TEXT("max_amount"), Parsed.MaxAmount, 100);
	TestEqual(TEXT("regen_rate"), Parsed.RegenRate, 2);
	TestEqual(TEXT("x float"), Parsed.X, 12.5f);
	TestEqual(TEXT("y float"), Parsed.Y, -3.25f);
	TestEqual(TEXT("z float"), Parsed.Z, 0.f);

	// A forged out-of-range amount saturates rather than narrowing; a non-finite coordinate coerces to 0.
	TSharedPtr<FJsonObject> Forged = MakeShared<FJsonObject>();
	Forged->SetNumberField(TEXT("amount"), 5.0e18);      // far above INT32_MAX
	Forged->SetNumberField(TEXT("max_amount"), -5.0e18); // far below INT32_MIN
	Forged->SetNumberField(TEXT("x"), std::numeric_limits<double>::infinity());
	const FCrowdyResourceNode ForgedNode =
		UCrowdyListResourceNodesAction::ParseResourceNode(Forged, TEXT("n-forged"), FString());
	TestEqual(TEXT("amount saturates to int32 max"), ForgedNode.Amount, MAX_int32);
	TestEqual(TEXT("max_amount saturates to int32 min"), ForgedNode.MaxAmount, MIN_int32);
	TestEqual(TEXT("non-finite x coerces to 0"), ForgedNode.X, 0.f);

	return true;
}

// The crop parser reads stage / max_stage / output_qty via the saturating clamp and the output item string, with
// the container id / display name / owner threaded from the list row, and computes bReady (max_stage > 0 and
// stage >= max_stage). A forged out-of-range value saturates.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitWorldsimParseCropTest,
	"CrowdySDK.Kit.WorldsimParseCrop", CrowdyWorldsimKitTestFlags)
bool FCrowdyKitWorldsimParseCropTest::RunTest(const FString& Parameters)
{
	// A grown crop: stage has reached max_stage, so bReady is true.
	TSharedPtr<FJsonObject> Grown = MakeShared<FJsonObject>();
	Grown->SetNumberField(TEXT("stage"), 3);
	Grown->SetNumberField(TEXT("max_stage"), 3);
	Grown->SetNumberField(TEXT("output_qty"), 2);
	Grown->SetStringField(TEXT("output_item_id"), TEXT("wheat"));

	const FCrowdyCrop ReadyCrop =
		UCrowdyListCropsAction::ParseCrop(Grown, TEXT("c-1"), TEXT("Wheat Plot"), TEXT("555"));
	TestEqual(TEXT("container id threaded"), ReadyCrop.ContainerId, FString(TEXT("c-1")));
	TestEqual(TEXT("display name threaded"), ReadyCrop.DisplayName, FString(TEXT("Wheat Plot")));
	TestEqual(TEXT("owner threaded"), ReadyCrop.OwnerUserId, FString(TEXT("555")));
	TestEqual(TEXT("stage"), ReadyCrop.Stage, 3);
	TestEqual(TEXT("max_stage"), ReadyCrop.MaxStage, 3);
	TestEqual(TEXT("output_qty"), ReadyCrop.OutputQty, 2);
	TestEqual(TEXT("output_item_id"), ReadyCrop.OutputItemId, FString(TEXT("wheat")));
	TestTrue(TEXT("grown crop is ready"), ReadyCrop.bReady);

	// A growing crop: stage below max_stage, so bReady is false.
	TSharedPtr<FJsonObject> Growing = MakeShared<FJsonObject>();
	Growing->SetNumberField(TEXT("stage"), 1);
	Growing->SetNumberField(TEXT("max_stage"), 3);
	const FCrowdyCrop GrowingCrop =
		UCrowdyListCropsAction::ParseCrop(Growing, TEXT("c-2"), FString(), FString());
	TestFalse(TEXT("growing crop is not ready"), GrowingCrop.bReady);

	// max_stage 0 is never ready even at stage 0 (the max_stage > 0 guard).
	TSharedPtr<FJsonObject> Unset = MakeShared<FJsonObject>();
	const FCrowdyCrop UnsetCrop = UCrowdyListCropsAction::ParseCrop(Unset, TEXT("c-3"), FString(), FString());
	TestFalse(TEXT("max_stage 0 is not ready"), UnsetCrop.bReady);

	// A forged out-of-range stage saturates rather than narrowing.
	TSharedPtr<FJsonObject> Forged = MakeShared<FJsonObject>();
	Forged->SetNumberField(TEXT("stage"), 5.0e18);
	Forged->SetNumberField(TEXT("max_stage"), 3);
	const FCrowdyCrop ForgedCrop = UCrowdyListCropsAction::ParseCrop(Forged, TEXT("c-forged"), FString(), FString());
	TestEqual(TEXT("stage saturates to int32 max"), ForgedCrop.Stage, MAX_int32);
	TestTrue(TEXT("saturated stage is ready"), ForgedCrop.bReady);

	return true;
}

#endif
