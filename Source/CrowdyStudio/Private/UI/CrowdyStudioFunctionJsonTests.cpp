// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "UI/CrowdyStudioFunctionJson.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyStudioFunctionJsonTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	TSharedPtr<FStudioPolicyRule> MakeRule(const FString& Type)
	{
		TSharedPtr<FStudioPolicyRule> Rule = MakeShared<FStudioPolicyRule>();
		Rule->Type = Type;
		return Rule;
	}

	// Assert one serialized field is present, is a string, and carries the expected value. Asking for the value
	// itself rather than through a typed getter keeps a dropped field a failure instead of a silently empty string,
	// and checking its type is what proves an id still travels as a string instead of collapsing to a number.
	void TestStudioJsonStringField(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Object,
		const TCHAR* FieldName, const FString& Expected)
	{
		if (!Test.TestTrue(FString::Printf(TEXT("The object carrying '%s' parsed back"), FieldName), Object.IsValid()))
		{
			return;
		}

		const TSharedPtr<FJsonValue> Value = Object->TryGetField(FieldName);
		if (!Test.TestTrue(FString::Printf(TEXT("'%s' is emitted"), FieldName), Value.IsValid()))
		{
			return;
		}

		Test.TestTrue(FString::Printf(TEXT("'%s' is emitted as a string"), FieldName), Value->Type == EJson::String);

		const FString ValueMessage = FString::Printf(TEXT("'%s' carries the authored value"), FieldName);
		Test.TestEqual(*ValueMessage, Value->AsString(), Expected);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioPolicyJsonRoundTripTest,
	"CrowdySDK.CrowdyStudio.PolicyJsonRoundTrip", CrowdyStudioFunctionJsonTestFlags)

bool FCrowdyStudioPolicyJsonRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyStudioFunctionJson;

	// Every rule kind the builder can show, so no leaf type can be dropped from the round trip unnoticed.
	TArray<TSharedPtr<FStudioPolicyRule>> Rules;
	Rules.Add(MakeRule(TEXT("is_host")));
	Rules.Add(MakeRule(TEXT("owner_of_self")));
	Rules.Add(MakeRule(TEXT("is_current_turn")));
	Rules.Add(MakeRule(TEXT("is_participant")));

	TSharedPtr<FStudioPolicyRule> Tier = MakeRule(TEXT("tier_feature"));
	Tier->Feature = TEXT("premium_combat");
	Rules.Add(Tier);

	TSharedPtr<FStudioPolicyRule> Group = MakeRule(TEXT("group_permission"));
	Group->GroupId = TEXT("9007199254740993");
	Group->Permission = TEXT("manage");
	Rules.Add(Group);

	// A grid gate needs both halves: the key says which permission on the grid, the id says which grid. A policy
	// that loses either is re-saved as a weaker gate than the one the author stored.
	TSharedPtr<FStudioPolicyRule> Grid = MakeRule(TEXT("grid_permission"));
	Grid->Key = TEXT("build");
	Grid->GridId = TEXT("9007199254740995");
	Rules.Add(Grid);

	TSharedPtr<FStudioPolicyRule> Condition = MakeRule(TEXT("condition"));
	Condition->Expression = TEXT("self.hp > 0");
	Rules.Add(Condition);

	const FString Json = PolicyToJson(TEXT("or"), Rules);
	TestFalse(TEXT("A policy with rules serializes to something"), Json.IsEmpty());

	TArray<TSharedPtr<FStudioPolicyRule>> Parsed;
	FString Connector;
	TestTrue(TEXT("The builder can represent what it just wrote"), ParsePolicyJson(Json, Parsed, Connector));
	TestEqual(TEXT("Connector survives the round trip"), Connector, FString(TEXT("or")));

	if (!TestEqual(TEXT("Rule count survives the round trip"), Parsed.Num(), Rules.Num()))
	{
		return false;
	}

	for (int32 Index = 0; Index < Rules.Num(); ++Index)
	{
		TestEqual(TEXT("Rule type survives"), Parsed[Index]->Type, Rules[Index]->Type);
		TestEqual(TEXT("Feature survives"), Parsed[Index]->Feature, Rules[Index]->Feature);
		TestEqual(TEXT("Group id survives as a string"), Parsed[Index]->GroupId, Rules[Index]->GroupId);
		TestEqual(TEXT("Permission survives"), Parsed[Index]->Permission, Rules[Index]->Permission);
		TestEqual(TEXT("Grid key survives"), Parsed[Index]->Key, Rules[Index]->Key);
		TestEqual(TEXT("Grid id survives as a string"), Parsed[Index]->GridId, Rules[Index]->GridId);
		TestEqual(TEXT("Expression survives"), Parsed[Index]->Expression, Rules[Index]->Expression);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioPolicyJsonEmptyTest,
	"CrowdySDK.CrowdyStudio.PolicyJsonEmpty", CrowdyStudioFunctionJsonTestFlags)

bool FCrowdyStudioPolicyJsonEmptyTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyStudioFunctionJson;

	TArray<TSharedPtr<FStudioPolicyRule>> Parsed;
	FString Connector;

	TestTrue(TEXT("No policy at all is representable"), ParsePolicyJson(FString(), Parsed, Connector));
	TestEqual(TEXT("No policy yields no rules"), Parsed.Num(), 0);
	TestEqual(TEXT("No policy defaults to the and connector"), Connector, FString(TEXT("and")));

	TestTrue(TEXT("Whitespace-only policy is representable"), ParsePolicyJson(TEXT("   \n\t "), Parsed, Connector));
	TestEqual(TEXT("Whitespace-only policy yields no rules"), Parsed.Num(), 0);

	// An empty rule set clears the server's policy, so it must serialize to nothing rather than to an
	// empty group, which the server would read as a gate that nobody passes.
	TArray<TSharedPtr<FStudioPolicyRule>> NoRules;
	TestTrue(TEXT("No rules serializes to an empty string"), PolicyToJson(TEXT("and"), NoRules).IsEmpty());

	// A rule the user has not chosen a type for yet is dropped, not written as a typeless leaf.
	TArray<TSharedPtr<FStudioPolicyRule>> Blank;
	Blank.Add(MakeShared<FStudioPolicyRule>());
	TestTrue(TEXT("A typeless rule serializes to an empty string"), PolicyToJson(TEXT("and"), Blank).IsEmpty());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioPolicyJsonUnrepresentableTest,
	"CrowdySDK.CrowdyStudio.PolicyJsonUnrepresentable", CrowdyStudioFunctionJsonTestFlags)

bool FCrowdyStudioPolicyJsonUnrepresentableTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyStudioFunctionJson;

	TArray<TSharedPtr<FStudioPolicyRule>> Parsed;
	FString Connector;

	// Each of these carries meaning the flat builder cannot show. Reporting false keeps the original
	// JSON in the raw editor instead of quietly rewriting it into something weaker.
	const FString Nested = TEXT("{\"type\":\"and\",\"rules\":[{\"type\":\"or\",\"rules\":[{\"type\":\"is_host\"}]}]}");
	TestFalse(TEXT("A nested group is not representable"), ParsePolicyJson(Nested, Parsed, Connector));

	const FString Negated = TEXT("{\"type\":\"not\",\"rules\":[{\"type\":\"is_host\"}]}");
	TestFalse(TEXT("A not is not representable"), ParsePolicyJson(Negated, Parsed, Connector));

	const FString UnknownLeaf = TEXT("{\"type\":\"and\",\"rules\":[{\"type\":\"is_wearing_hat\"}]}");
	TestFalse(TEXT("An unknown rule type inside a group is not representable"), ParsePolicyJson(UnknownLeaf, Parsed, Connector));

	const FString UnknownRoot = TEXT("{\"type\":\"is_wearing_hat\"}");
	TestFalse(TEXT("An unknown bare rule type is not representable"), ParsePolicyJson(UnknownRoot, Parsed, Connector));

	const FString NotJson = TEXT("this is not json");
	TestFalse(TEXT("Unparseable text is not representable"), ParsePolicyJson(NotJson, Parsed, Connector));

	const FString NoType = TEXT("{\"rules\":[]}");
	TestFalse(TEXT("A policy with no type is not representable"), ParsePolicyJson(NoType, Parsed, Connector));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioPolicyJsonNumericIdsTest,
	"CrowdySDK.CrowdyStudio.PolicyJsonNumericIds", CrowdyStudioFunctionJsonTestFlags)

bool FCrowdyStudioPolicyJsonNumericIdsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyStudioFunctionJson;

	// Ids travel as BigInt strings, but a hand-written policy may carry them as raw JSON numbers. Those
	// must still load into the builder, or opening such a function would silently drop the id.
	const FString Json = TEXT("{\"type\":\"and\",\"rules\":[{\"type\":\"group_permission\",\"groupId\":4242,\"permission\":\"manage\"},{\"type\":\"grid_permission\",\"key\":\"build\",\"gridId\":77}]}");

	TArray<TSharedPtr<FStudioPolicyRule>> Parsed;
	FString Connector;
	if (!TestTrue(TEXT("Numeric ids are representable"), ParsePolicyJson(Json, Parsed, Connector)))
	{
		return false;
	}
	if (!TestEqual(TEXT("Both rules loaded"), Parsed.Num(), 2))
	{
		return false;
	}

	TestEqual(TEXT("A numeric group id reads as its decimal string"), Parsed[0]->GroupId, FString(TEXT("4242")));
	TestEqual(TEXT("Permission still reads as a string"), Parsed[0]->Permission, FString(TEXT("manage")));
	TestEqual(TEXT("A numeric grid id reads as its decimal string"), Parsed[1]->GridId, FString(TEXT("77")));
	TestEqual(TEXT("Grid key still reads as a string"), Parsed[1]->Key, FString(TEXT("build")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioFunctionJsonEmptyListsTest,
	"CrowdySDK.CrowdyStudio.FunctionJsonEmptyLists", CrowdyStudioFunctionJsonTestFlags)

bool FCrowdyStudioFunctionJsonEmptyListsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyStudioFunctionJson;

	// A function with no parameters or no mutations sends an empty string, not "[]": the server reads
	// the empty string as "leave this alone" and an empty array as "replace it with nothing".
	TestEqual(TEXT("No parameters serializes to an empty string"), ParamsToJson(TArray<FStudioFunctionParam>()), FString());
	TestEqual(TEXT("No mutations serializes to an empty string"), MutationsToJson(TArray<FStudioFunctionMutation>()), FString());

	TArray<FStudioFunctionParam> Params;
	FStudioFunctionParam& Param = Params.AddDefaulted_GetRef();
	Param.Name = TEXT("Amount");
	Param.ValueType = TEXT("int");
	Param.bRequired = false;
	Param.SortOrder = 3;
	TArray<TSharedPtr<FJsonValue>> ParamValues;
	const TSharedRef<TJsonReader<>> ParamReader = TJsonReaderFactory<>::Create(ParamsToJson(Params));
	if (!TestTrue(TEXT("Serialized parameters parse back"), FJsonSerializer::Deserialize(ParamReader, ParamValues))
		|| !TestEqual(TEXT("One parameter round-trips"), ParamValues.Num(), 1))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> ParamObject = ParamValues[0]->AsObject();
	TestEqual(TEXT("Parameter name is emitted"), ParamObject->GetStringField(TEXT("name")), FString(TEXT("Amount")));
	TestEqual(TEXT("Parameter value type is emitted"), ParamObject->GetStringField(TEXT("valueType")), FString(TEXT("int")));
	TestTrue(TEXT("Parameter required flag is emitted"), ParamObject->HasField(TEXT("required")));
	TestFalse(TEXT("An optional parameter is not marked required"), ParamObject->GetBoolField(TEXT("required")));
	TestEqual(TEXT("Parameter sort order is emitted"), ParamObject->GetIntegerField(TEXT("sortOrder")), 3);
	TestFalse(TEXT("An unset default value is omitted entirely"), ParamObject->HasField(TEXT("defaultValueJson")));
	TestFalse(TEXT("An unset description is omitted entirely"), ParamObject->HasField(TEXT("description")));

	TArray<FStudioFunctionMutation> Mutations;
	FStudioFunctionMutation& Mutation = Mutations.AddDefaulted_GetRef();
	Mutation.Target = TEXT("self");
	Mutation.Property = TEXT("hp");
	Mutation.Expression = TEXT("hp - Amount");

	TArray<TSharedPtr<FJsonValue>> MutationValues;
	const TSharedRef<TJsonReader<>> MutationReader = TJsonReaderFactory<>::Create(MutationsToJson(Mutations));
	if (!TestTrue(TEXT("Serialized mutations parse back"), FJsonSerializer::Deserialize(MutationReader, MutationValues))
		|| !TestEqual(TEXT("One mutation round-trips"), MutationValues.Num(), 1))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> MutationObject = MutationValues[0]->AsObject();
	TestEqual(TEXT("Mutation target is emitted"), MutationObject->GetStringField(TEXT("target")), FString(TEXT("self")));
	TestEqual(TEXT("Mutation property is emitted"), MutationObject->GetStringField(TEXT("property")), FString(TEXT("hp")));
	TestEqual(TEXT("Mutation expression is emitted"), MutationObject->GetStringField(TEXT("expression")), FString(TEXT("hp - Amount")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioPolicyJsonFieldShapeTest,
	"CrowdySDK.CrowdyStudio.PolicyJsonFieldShape", CrowdyStudioFunctionJsonTestFlags)

bool FCrowdyStudioPolicyJsonFieldShapeTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyStudioFunctionJson;

	// A round trip on its own would still pass if a field were renamed on both sides, so the stored policy is
	// checked against the field names the server reads. The writer pretty-prints, so this reads parsed fields
	// rather than looking for text in the serialized string.
	TArray<TSharedPtr<FStudioPolicyRule>> Rules;

	TSharedPtr<FStudioPolicyRule> Tier = MakeRule(TEXT("tier_feature"));
	Tier->Feature = TEXT("premium_combat");
	Rules.Add(Tier);

	TSharedPtr<FStudioPolicyRule> Group = MakeRule(TEXT("group_permission"));
	Group->GroupId = TEXT("9007199254740993");
	Group->Permission = TEXT("manage");
	Rules.Add(Group);

	TSharedPtr<FStudioPolicyRule> Grid = MakeRule(TEXT("grid_permission"));
	Grid->Key = TEXT("build");
	Grid->GridId = TEXT("9007199254740995");
	Rules.Add(Grid);

	TSharedPtr<FStudioPolicyRule> Condition = MakeRule(TEXT("condition"));
	Condition->Expression = TEXT("self.hp > 0");
	Rules.Add(Condition);

	Rules.Add(MakeRule(TEXT("is_host")));

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(PolicyToJson(TEXT("or"), Rules));
	if (!TestTrue(TEXT("The serialized policy is valid JSON"),
		FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid()))
	{
		return false;
	}

	TestStudioJsonStringField(*this, Root, TEXT("type"), TEXT("or"));

	const TArray<TSharedPtr<FJsonValue>>* RuleValues = nullptr;
	if (!TestTrue(TEXT("The policy carries its rules array"), Root->TryGetArrayField(TEXT("rules"), RuleValues))
		|| !TestEqual(TEXT("Every authored rule is written"), RuleValues->Num(), Rules.Num()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> TierObject = (*RuleValues)[0]->AsObject();
	TestStudioJsonStringField(*this, TierObject, TEXT("type"), TEXT("tier_feature"));
	TestStudioJsonStringField(*this, TierObject, TEXT("feature"), TEXT("premium_combat"));
	TestEqual(TEXT("A tier rule writes its type and feature and nothing more"), TierObject->Values.Num(), 2);

	const TSharedPtr<FJsonObject> GroupObject = (*RuleValues)[1]->AsObject();
	TestStudioJsonStringField(*this, GroupObject, TEXT("type"), TEXT("group_permission"));
	TestStudioJsonStringField(*this, GroupObject, TEXT("groupId"), TEXT("9007199254740993"));
	TestStudioJsonStringField(*this, GroupObject, TEXT("permission"), TEXT("manage"));
	TestEqual(TEXT("A group rule writes its type, group id and permission and nothing more"),
		GroupObject->Values.Num(), 3);

	const TSharedPtr<FJsonObject> GridObject = (*RuleValues)[2]->AsObject();
	TestStudioJsonStringField(*this, GridObject, TEXT("type"), TEXT("grid_permission"));
	TestStudioJsonStringField(*this, GridObject, TEXT("key"), TEXT("build"));
	TestStudioJsonStringField(*this, GridObject, TEXT("gridId"), TEXT("9007199254740995"));
	TestEqual(TEXT("A grid rule writes its type, key and grid id and nothing more"), GridObject->Values.Num(), 3);

	const TSharedPtr<FJsonObject> ConditionObject = (*RuleValues)[3]->AsObject();
	TestStudioJsonStringField(*this, ConditionObject, TEXT("type"), TEXT("condition"));
	TestStudioJsonStringField(*this, ConditionObject, TEXT("expression"), TEXT("self.hp > 0"));
	TestEqual(TEXT("A condition rule writes its type and expression and nothing more"),
		ConditionObject->Values.Num(), 2);

	const TSharedPtr<FJsonObject> HostObject = (*RuleValues)[4]->AsObject();
	TestStudioJsonStringField(*this, HostObject, TEXT("type"), TEXT("is_host"));
	TestEqual(TEXT("A rule with no fields of its own writes only its type"), HostObject->Values.Num(), 1);

	// The optional halves are left out when blank rather than written empty, and a blank connector stores the same
	// and the builder shows.
	TArray<TSharedPtr<FStudioPolicyRule>> Sparse;

	TSharedPtr<FStudioPolicyRule> BareGroup = MakeRule(TEXT("group_permission"));
	BareGroup->GroupId = TEXT("4242");
	Sparse.Add(BareGroup);

	TSharedPtr<FStudioPolicyRule> BareGrid = MakeRule(TEXT("grid_permission"));
	BareGrid->Key = TEXT("build");
	Sparse.Add(BareGrid);

	TSharedPtr<FJsonObject> SparseRoot;
	const TSharedRef<TJsonReader<>> SparseReader = TJsonReaderFactory<>::Create(PolicyToJson(FString(), Sparse));
	if (!TestTrue(TEXT("The sparse policy is valid JSON"),
		FJsonSerializer::Deserialize(SparseReader, SparseRoot) && SparseRoot.IsValid()))
	{
		return false;
	}

	TestStudioJsonStringField(*this, SparseRoot, TEXT("type"), TEXT("and"));

	const TArray<TSharedPtr<FJsonValue>>* SparseRules = nullptr;
	if (!TestTrue(TEXT("The sparse policy carries its rules array"),
		SparseRoot->TryGetArrayField(TEXT("rules"), SparseRules))
		|| !TestEqual(TEXT("Both sparse rules are written"), SparseRules->Num(), 2))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> SparseGroupObject = (*SparseRules)[0]->AsObject();
	TestStudioJsonStringField(*this, SparseGroupObject, TEXT("groupId"), TEXT("4242"));
	TestFalse(TEXT("A group rule with no permission omits it"), SparseGroupObject->HasField(TEXT("permission")));

	const TSharedPtr<FJsonObject> SparseGridObject = (*SparseRules)[1]->AsObject();
	TestStudioJsonStringField(*this, SparseGridObject, TEXT("key"), TEXT("build"));
	TestFalse(TEXT("A grid rule with no grid id omits it"), SparseGridObject->HasField(TEXT("gridId")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioFunctionParamFieldsTest,
	"CrowdySDK.CrowdyStudio.FunctionParamFields", CrowdyStudioFunctionJsonTestFlags)

bool FCrowdyStudioFunctionParamFieldsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyStudioFunctionJson;

	// A fully authored parameter, so every optional half is checked in its written form too: a default value that
	// stops being sent leaves the server calling the function with nothing where the author put a value.
	FStudioFunctionParam Full;
	Full.Name = TEXT("Amount");
	Full.ValueType = TEXT("int");
	Full.bRequired = true;
	Full.DefaultValueJson = TEXT("{\"value\":5}");
	Full.Description = TEXT("How much health to remove");
	Full.SortOrder = 2;

	FStudioFunctionParam Minimal;
	Minimal.Name = TEXT("Source");
	Minimal.ValueType = TEXT("string");
	Minimal.bRequired = false;
	Minimal.SortOrder = 7;

	TArray<FStudioFunctionParam> Params;
	Params.Add(Full);
	Params.Add(Minimal);

	TArray<TSharedPtr<FJsonValue>> ParamValues;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ParamsToJson(Params));
	if (!TestTrue(TEXT("Serialized parameters parse back"), FJsonSerializer::Deserialize(Reader, ParamValues))
		|| !TestEqual(TEXT("Both parameters round-trip"), ParamValues.Num(), 2))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> FullObject = ParamValues[0]->AsObject();
	TestStudioJsonStringField(*this, FullObject, TEXT("name"), TEXT("Amount"));
	TestStudioJsonStringField(*this, FullObject, TEXT("valueType"), TEXT("int"));
	TestStudioJsonStringField(*this, FullObject, TEXT("defaultValueJson"), TEXT("{\"value\":5}"));
	TestStudioJsonStringField(*this, FullObject, TEXT("description"), TEXT("How much health to remove"));

	const TSharedPtr<FJsonValue> Required = FullObject->TryGetField(TEXT("required"));
	if (TestTrue(TEXT("A parameter emits its required flag"), Required.IsValid()))
	{
		TestTrue(TEXT("The required flag is emitted as a boolean"), Required->Type == EJson::Boolean);
		TestTrue(TEXT("A required parameter is marked required"), Required->AsBool());
	}

	const TSharedPtr<FJsonValue> SortOrder = FullObject->TryGetField(TEXT("sortOrder"));
	if (TestTrue(TEXT("A parameter emits its sort order"), SortOrder.IsValid()))
	{
		TestTrue(TEXT("The sort order is emitted as a number"), SortOrder->Type == EJson::Number);
		TestEqual(TEXT("The sort order carries the authored position"),
			static_cast<int32>(SortOrder->AsNumber()), 2);
	}

	TestEqual(TEXT("A fully authored parameter writes six fields and nothing more"), FullObject->Values.Num(), 6);

	// Order is the wire's only record of how the editor listed the rows, so the second parameter must be second.
	const TSharedPtr<FJsonObject> MinimalObject = ParamValues[1]->AsObject();
	TestStudioJsonStringField(*this, MinimalObject, TEXT("name"), TEXT("Source"));
	TestStudioJsonStringField(*this, MinimalObject, TEXT("valueType"), TEXT("string"));
	TestEqual(TEXT("A parameter with no default and no description writes four fields"),
		MinimalObject->Values.Num(), 4);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioPolicyLeafTypesTest,
	"CrowdySDK.CrowdyStudio.PolicyLeafTypes", CrowdyStudioFunctionJsonTestFlags)

bool FCrowdyStudioPolicyLeafTypesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyStudioFunctionJson;

	// A rule kind the builder stops recognizing fails quietly: every stored policy using it drops out of the guided
	// builder into the raw-JSON editor. Each kind is named here so losing one is a failure instead of a surprise.
	const TCHAR* const LeafTypes[] =
	{
		TEXT("owner_of_self"), TEXT("is_current_turn"), TEXT("is_host"), TEXT("is_participant"),
		TEXT("tier_feature"), TEXT("group_permission"), TEXT("grid_permission"), TEXT("condition")
	};

	for (const TCHAR* const LeafType : LeafTypes)
	{
		TestTrue(FString::Printf(TEXT("'%s' is a rule the guided builder can show"), LeafType),
			IsPolicyLeafType(LeafType));

		// The bare form: a stored policy that is a single rule with no surrounding connector.
		TArray<TSharedPtr<FStudioPolicyRule>> Parsed;
		FString Connector;
		const FString Bare = FString::Printf(TEXT("{\"type\":\"%s\"}"), LeafType);
		if (TestTrue(FString::Printf(TEXT("A bare '%s' policy loads into the builder"), LeafType),
			ParsePolicyJson(Bare, Parsed, Connector))
			&& TestEqual(FString::Printf(TEXT("A bare '%s' policy loads as one rule"), LeafType), Parsed.Num(), 1))
		{
			TestEqual(TEXT("The bare rule keeps its type"), Parsed[0]->Type, FString(LeafType));
			TestEqual(TEXT("A bare rule defaults to the and connector"), Connector, FString(TEXT("and")));
		}
	}

	// The connectors are groups rather than leaves, and a type the builder does not know stays in the raw editor.
	TestFalse(TEXT("The and connector is not a leaf"), IsPolicyLeafType(TEXT("and")));
	TestFalse(TEXT("The or connector is not a leaf"), IsPolicyLeafType(TEXT("or")));
	TestFalse(TEXT("A negation is not a leaf"), IsPolicyLeafType(TEXT("not")));
	TestFalse(TEXT("An unknown rule type is not a leaf"), IsPolicyLeafType(TEXT("is_wearing_hat")));
	TestFalse(TEXT("A rule with no type is not a leaf"), IsPolicyLeafType(FString()));

	return true;
}

#endif
