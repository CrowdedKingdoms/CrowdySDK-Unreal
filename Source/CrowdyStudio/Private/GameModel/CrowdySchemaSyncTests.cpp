// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "GameModel/CrowdySchemaSync.h"
#include "GameModel/CrowdySchemaSyncTestTarget.h"
#include "Model/CrowdyStudioTypes.h"
#include "Replication/GameModel/CrowdyAttributeRegistry.h"
#include "Replication/GameModel/CrowdyGameModelMetaKeys.h"

namespace
{
	constexpr EAutomationTestFlags CrowdySchemaSyncTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	FCrowdyDesiredContainerType MakeDesiredType(const FString& TypeName)
	{
		FCrowdyDesiredContainerType Type;
		Type.TypeName = TypeName;
		Type.DisplayName = TypeName;
		// InstantiableBy / DefaultVisibility keep their member/public defaults.
		return Type;
	}

	FCrowdyDesiredPropertyDef MakeDesiredProp(const FString& Key, const FString& ValueType, const FString& DefaultJson)
	{
		FCrowdyDesiredPropertyDef Prop;
		Prop.Key = Key;
		Prop.ValueType = ValueType;
		Prop.DefaultValueJson = DefaultJson;
		// Visibility / Writable keep their public/function defaults.
		return Prop;
	}

	FStudioContainerType MakeServerType(const FString& TypeName,
		const FString& InstantiableBy = TEXT("member"), const FString& Visibility = TEXT("public"))
	{
		FStudioContainerType Type;
		Type.TypeName = TypeName;
		Type.DisplayName = TypeName;
		Type.InstantiableBy = InstantiableBy;
		Type.DefaultPropertyVisibility = Visibility;
		return Type;
	}

	FStudioPropertyDef MakeServerProp(const FString& TypeName, const FString& Key, const FString& ValueType,
		const FString& DefaultJson, const FString& Visibility = TEXT("public"), const FString& Writable = TEXT("function"))
	{
		FStudioPropertyDef Prop;
		Prop.ContainerTypeName = TypeName;
		Prop.Key = Key;
		Prop.ValueType = ValueType;
		Prop.DefaultValueJson = DefaultJson;
		Prop.Visibility = Visibility;
		Prop.Writable = Writable;
		return Prop;
	}

	const FCrowdyDesiredPropertyDef* FindProp(const FCrowdyDesiredContainerType& Type, const TCHAR* Key)
	{
		return Type.Props.FindByPredicate([Key](const FCrowdyDesiredPropertyDef& P) { return P.Key == Key; });
	}

	FCrowdyGameModelFunctionParam MakeDesiredParam(const FString& Name, const FString& ValueType,
		bool bRequired, const FString& DefaultJson, int32 SortOrder)
	{
		FCrowdyGameModelFunctionParam Param;
		Param.Name = Name;
		Param.ValueType = ValueType;
		Param.bRequired = bRequired;
		Param.DefaultValueJson = DefaultJson;
		Param.SortOrder = SortOrder;
		return Param;
	}

	FStudioFunctionParam MakeServerParam(const FString& Name, const FString& ValueType,
		bool bRequired, const FString& DefaultJson, int32 SortOrder)
	{
		FStudioFunctionParam Param;
		Param.Name = Name;
		Param.ValueType = ValueType;
		Param.bRequired = bRequired;
		Param.DefaultValueJson = DefaultJson;
		Param.SortOrder = SortOrder;
		return Param;
	}

	FCrowdyGameModelMutation MakeMutation(const FString& Target, const FString& Property, const FString& Expression)
	{
		FCrowdyGameModelMutation Mutation;
		Mutation.Target = Target;
		Mutation.Property = Property;
		Mutation.Expression = Expression;
		return Mutation;
	}

	// A representative compiled-effect function: player-invoked, owner-gated, one defaulted param, one clamped
	// self-write (the shape FCrowdyEffectLowering produces for "self.health -= $amount").
	FCrowdyGameModelFunctionInput MakeDesiredFunction()
	{
		FCrowdyGameModelFunctionInput Fn;
		Fn.Name = TEXT("take_damage");
		Fn.ContainerTypeName = TEXT("Hero");
		Fn.Description = TEXT("Applies damage to self.");
		Fn.InvokeScope = TEXT("player");
		Fn.InvokePolicyJson = TEXT("{\"type\":\"owner_of_self\"}");
		Fn.Parameters.Add(MakeDesiredParam(TEXT("amount"), TEXT("int"), false, TEXT("10"), 0));
		Fn.Mutations.Add(MakeMutation(TEXT("self"), TEXT("health"), TEXT("max(0, min(100, self.health - $amount))")));
		return Fn;
	}

	// A server function that matches a desired input field-for-field (the idempotency baseline the tests perturb).
	FStudioFunction MakeServerFunctionFrom(const FCrowdyGameModelFunctionInput& D)
	{
		FStudioFunction S;
		S.Name = D.Name;
		S.ContainerTypeName = D.ContainerTypeName;
		S.Description = D.Description;
		S.ReturnType = D.ReturnType;
		S.ReturnExpression = D.ReturnExpression;
		S.InvokeScope = D.InvokeScope;
		S.InvokePolicyJson = D.InvokePolicyJson;
		for (const FCrowdyGameModelFunctionParam& P : D.Parameters)
		{
			S.Parameters.Add(MakeServerParam(P.Name, P.ValueType, P.bRequired, P.DefaultValueJson, P.SortOrder));
		}
		for (const FCrowdyGameModelMutation& M : D.Mutations)
		{
			FStudioFunctionMutation SM;
			SM.Target = M.Target;
			SM.Property = M.Property;
			SM.Expression = M.Expression;
			S.Mutations.Add(SM);
		}
		return S;
	}

	FCrowdyGameModelNotificationArg MakeNotifArg(const FString& Name, const FString& Expression)
	{
		FCrowdyGameModelNotificationArg Arg;
		Arg.Name = Name;
		Arg.Expression = Expression;
		return Arg;
	}

	// The SDK's own channel model-changed notification: payload = concat("cmc:", $self_container_id), optionally
	// addressed at the app's session channel by name (as InjectSessionChannelTarget produces).
	FCrowdyGameModelNotification MakeSdkChannelNotif(bool bAddressed)
	{
		FCrowdyGameModelNotification N;
		N.Kind = TEXT("channel");
		N.Args.Add(MakeNotifArg(TEXT("payload"),
			FString::Printf(TEXT("concat(\"%s\", $self_container_id)"), CrowdyGameModelMetaKeys::ModelChangedChannelPrefix)));
		if (bAddressed)
		{
			N.Args.Add(MakeNotifArg(TEXT("channel_name"),
				FString::Printf(TEXT("$%s"), CrowdyGameModelMetaKeys::SessionChannelNameParam)));
		}
		return N;
	}

	// The same notification as an app authored before channel_name existed: a literal channel id, which is exactly
	// what a model copied out of another app still carries and what the migration has to rewrite.
	FCrowdyGameModelNotification MakeLegacyChannelIdNotif(const FString& ChannelId = TEXT("777"))
	{
		FCrowdyGameModelNotification N = MakeSdkChannelNotif(/*bAddressed*/ false);
		N.Args.Add(MakeNotifArg(TEXT("channel_id"), ChannelId));
		return N;
	}

	// A non-SDK (seed/console-authored) notification: a spatial one whose event_type is NOT the reserved 60000.
	FCrowdyGameModelNotification MakeNonSdkNotif()
	{
		FCrowdyGameModelNotification N;
		N.Kind = TEXT("spatial");
		N.EmitAs = TEXT("server_event");
		N.Args.Add(MakeNotifArg(TEXT("event_type"), TEXT("42")));
		return N;
	}
}

// GatherContainerClasses must never return a test-only fixture: those carry meta=(CrowdyContainerTest) so a
// live "Sync Schema from Code" does not upsert them as bogus container types. Verifies the exclusion directly
// (the fixture is a valid CrowdyContainer with model attributes, yet excluded) and the predicate itself, then
// that the exclusion still holds through SelectContainerClasses, which is where the rule now lives.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySchemaSyncSkipsTestContainersTest,
	"CrowdySDK.GameModel.SchemaSyncSkipsTestContainers", CrowdySchemaSyncTestFlags)
bool FCrowdySchemaSyncSkipsTestContainersTest::RunTest(const FString& Parameters)
{
	UClass* TestFixture = UCrowdySchemaSyncTestTarget::StaticClass();

	TestTrue(TEXT("the fixture is recognized as a test container"),
		FCrowdyAttributeRegistry::IsTestContainer(TestFixture));
	TestFalse(TEXT("a plain class is not a test container"),
		FCrowdyAttributeRegistry::IsTestContainer(UObject::StaticClass()));

	const TArray<UClass*> Gathered = FCrowdySchemaSync::GatherContainerClasses();
	TestFalse(TEXT("the test fixture is not gathered for schema sync"), Gathered.Contains(TestFixture));

	const TArray<UClass*> Excluded = FCrowdySchemaSync::SelectContainerClasses(
		{ TestFixture, UCrowdySchemaSyncSignalsOnlyTarget::StaticClass() }, /*bExcludeTestContainers*/ true);
	TestEqual(TEXT("both test fixtures are excluded when the production rule is applied"), Excluded.Num(), 0);

	return true;
}

// A container whose whole contribution is functions (signals, timers, automations) declares no Server Owned
// attribute, because those are authored on effect assets rather than on the class. Gathering used to require an
// accepted attribute as well as the tag, which dropped such a container out of the desired schema entirely: its
// type was never created, its effects' functions had no type to bind to, and a type already on the server read as
// an orphan and was offered for prune. The tag alone declares a container.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySchemaSyncGathersAttributelessContainerTest,
	"CrowdySDK.GameModel.SchemaSyncGathersAttributelessContainer", CrowdySchemaSyncTestFlags)
bool FCrowdySchemaSyncGathersAttributelessContainerTest::RunTest(const FString& Parameters)
{
	UClass* SignalsOnly = UCrowdySchemaSyncSignalsOnlyTarget::StaticClass();

	// The fixture really is the shape under test: tagged, and carrying no accepted attribute at all.
	FString TypeName;
	TestTrue(TEXT("the fixture carries a CrowdyContainer tag"),
		FCrowdyAttributeRegistry::GetContainerTypeName(SignalsOnly, TypeName));
	TestEqual(TEXT("the fixture's container type name"), TypeName, FString(TEXT("SchemaSyncSignalsOnly")));
	TestFalse(TEXT("the fixture declares no Server Owned attributes"),
		FCrowdyAttributeRegistry::ClassHasModelAttributes(SignalsOnly));

	const TArray<UClass*> Selected = FCrowdySchemaSync::SelectContainerClasses(
		{ SignalsOnly }, /*bExcludeTestContainers*/ false);
	TestTrue(TEXT("an attribute-less declared container is gathered"), Selected.Contains(SignalsOnly));

	// It reflects into a real desired type carrying no properties, which is what the diff then upserts.
	const FCrowdyDesiredContainerType Desired = FCrowdySchemaSync::BuildDesiredForClass(SignalsOnly);
	TestEqual(TEXT("the desired type name"), Desired.TypeName, FString(TEXT("SchemaSyncSignalsOnly")));
	TestEqual(TEXT("the desired type carries no properties"), Desired.Props.Num(), 0);

	return true;
}

// The prune regression the gather miss caused: a declared container the sync omitted read as a server-only type,
// so Studio offered a live container type (and everything on it) for deletion. Once the type is in the desired
// set it is recognized as code-owned, whether or not it declares a single property.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySchemaSyncAttributelessTypeIsNotServerOnlyTest,
	"CrowdySDK.GameModel.SchemaSyncAttributelessTypeIsNotServerOnly", CrowdySchemaSyncTestFlags)
bool FCrowdySchemaSyncAttributelessTypeIsNotServerOnlyTest::RunTest(const FString& Parameters)
{
	const TArray<FCrowdyDesiredContainerType> Desired = { MakeDesiredType(TEXT("SchemaSyncSignalsOnly")) };
	const TArray<FStudioContainerType> CurrentTypes = { MakeServerType(TEXT("SchemaSyncSignalsOnly")) };

	const FCrowdySchemaDelta Delta = FCrowdySchemaSync::DiffSchema(Desired, CurrentTypes, {});

	TestEqual(TEXT("a declared attribute-less type is never a prune candidate"), Delta.ServerOnlyTypes.Num(), 0);
	TestEqual(TEXT("nothing changed, so no type upsert"), Delta.TypeUpserts.Num(), 0);
	TestEqual(TEXT("a type with no properties emits no property upsert"), Delta.PropUpserts.Num(), 0);

	// The same type absent from the desired set is exactly what the gather miss produced.
	const FCrowdySchemaDelta Missing = FCrowdySchemaSync::DiffSchema({}, CurrentTypes, {});
	TestEqual(TEXT("an undeclared type is a prune candidate"), Missing.ServerOnlyTypes.Num(), 1);

	return true;
}

// A channel notification authors no emitAs (BuildSignalNotification leaves it empty and the marshaller omits the
// field), so the server stores its own default and reports THAT back on the next read. Comparing the two with a
// plain != made every function carrying a channel notification report drift on every plan and re-upsert forever,
// which is what put the SDK's own __crowdy_touch_<type> in the pending list of almost every container type, and
// what let a server that appends on upsert accumulate duplicate signal notifications (one extra delivery of the
// same signal per sync). A blank side matches anything; two concrete values that differ are still drift.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySchemaSyncBlankEmitAsIsNotDriftTest,
	"CrowdySDK.GameModel.SchemaSyncBlankEmitAsIsNotDrift", CrowdySchemaSyncTestFlags)
bool FCrowdySchemaSyncBlankEmitAsIsNotDriftTest::RunTest(const FString& Parameters)
{
	// What the effect lowering plus the channel addressing author: a named channel notification and NO emitAs.
	FCrowdyGameModelFunctionInput Desired = MakeDesiredFunction();
	Desired.Notifications.Reset();
	Desired.Notifications.Add(MakeSdkChannelNotif(/*bAddressed*/ true));

	// What the server hands back: the same notification, with the default it applied for the omitted field.
	FStudioFunction Server = MakeServerFunctionFrom(Desired);
	Server.Notifications.Reset();
	FCrowdyGameModelNotification Echoed = MakeSdkChannelNotif(/*bAddressed*/ true);
	Echoed.EmitAs = TEXT("channel_message");
	Server.Notifications.Add(Echoed);

	FCrowdySchemaDelta Delta;
	FCrowdySchemaSync::DiffFunctions({ Desired }, { Server }, Delta);
	TestEqual(TEXT("a server-defaulted emitAs is not drift, so an unchanged function plans no upsert"),
		Delta.FunctionUpserts.Num(), 0);

	// Two concrete values that genuinely differ are still drift, so the tolerance cannot hide a real change.
	FStudioFunction Changed = Server;
	Changed.Notifications[0].EmitAs = TEXT("server_event");
	FCrowdyGameModelFunctionInput Spatial = Desired;
	Spatial.Notifications[0].EmitAs = TEXT("channel_message");

	FCrowdySchemaDelta ChangedDelta;
	FCrowdySchemaSync::DiffFunctions({ Spatial }, { Changed }, ChangedDelta);
	TestEqual(TEXT("two concrete emitAs values that differ still drive an upsert"),
		ChangedDelta.FunctionUpserts.Num(), 1);

	return true;
}

// A function_invoked trigger authors no writeSource (the field only means something for property writes), and the
// server defaults it and returns "any" (captured live). Reading that as drift planned a trigger update on every
// sync, and the server's trigger upsert CREATES a row rather than updating, so the churn duplicated one trigger
// 7x on a live app - and each duplicate fired the automation again per matched event, which is what buried the
// server's evaluator (4598 consecutive failures, circuit open, every player invoke timing out). Absence on either
// side must match anything; two concrete values that differ are still drift.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySchemaSyncServerDefaultedWriteSourceIsNotDriftTest,
	"CrowdySDK.GameModel.SchemaSyncServerDefaultedWriteSourceIsNotDrift", CrowdySchemaSyncTestFlags)
bool FCrowdySchemaSyncServerDefaultedWriteSourceIsNotDriftTest::RunTest(const FString& Parameters)
{
	FCrowdyGameModelAutomationTriggerInput Desired;
	Desired.AutomationName = TEXT("RegenerateCovers");
	Desired.OnEvent = TEXT("function_invoked");
	Desired.FunctionName = TEXT("InitiateBossWave");
	Desired.ContainerTypeName = TEXT("BP_TA_BossCharacter");
	// WriteSource deliberately unauthored; DebounceMs stays 0.

	FStudioAutomationTrigger Server;
	Server.AutomationName = Desired.AutomationName;
	Server.OnEvent = Desired.OnEvent;
	Server.FunctionName = Desired.FunctionName;
	Server.ContainerTypeName = Desired.ContainerTypeName;
	Server.WriteSource = TEXT("any"); // the server's default, echoed on read-back
	Server.DebounceMs = 0;

	FCrowdySchemaDelta Delta;
	FCrowdySchemaSync::DiffAutomations({}, { Desired }, {}, { Server }, {}, Delta);
	TestEqual(TEXT("a server-defaulted writeSource on an unauthored side plans no trigger upsert"),
		Delta.TriggerUpserts.Num(), 0);

	// Two concrete values that genuinely differ still drift, so the tolerance cannot hide a real change.
	FCrowdyGameModelAutomationTriggerInput Concrete = Desired;
	Concrete.WriteSource = TEXT("direct");
	FCrowdySchemaDelta ConcreteDelta;
	FCrowdySchemaSync::DiffAutomations({}, { Concrete }, {}, { Server }, {}, ConcreteDelta);
	TestEqual(TEXT("a genuinely different concrete writeSource still plans the update"),
		ConcreteDelta.TriggerUpserts.Num(), 1);

	return true;
}

// The server compiles a stored invoke policy and returns it with the compiled "ast" object written into the SAME
// JSON: an authored {"type":"condition","expression":"true"} reads back with an "ast" key beside it (captured from
// a live app). A plain semantic compare therefore reported drift for every function carrying a policy on every
// plan, forever - the SDK's own __crowdy_touch functions (an explicit policy is part of their definition) and
// every owner-gated effect showed as a permanent pending update. The policy compare must ignore the
// server-computed key while still catching a real policy change.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySchemaSyncServerCompiledPolicyIsNotDriftTest,
	"CrowdySDK.GameModel.SchemaSyncServerCompiledPolicyIsNotDrift", CrowdySchemaSyncTestFlags)
bool FCrowdySchemaSyncServerCompiledPolicyIsNotDriftTest::RunTest(const FString& Parameters)
{
	// What AppendReservedCollectionSchema authors, and the exact enriched form a live server returned for it.
	const FString Authored = TEXT("{\"type\":\"condition\",\"expression\":\"true\"}");
	const FString Enriched = TEXT(
		"{\"ast\":{\"type\":\"Literal\",\"value\":true,\"value_type\":\"bool\"},\"type\":\"condition\",\"expression\":\"true\"}");

	TestTrue(TEXT("the server's compiled ast is not drift"),
		FCrowdySchemaSync::InvokePolicyEquals(Authored, Enriched));
	TestTrue(TEXT("symmetric: an enriched desired side compares equal too"),
		FCrowdySchemaSync::InvokePolicyEquals(Enriched, Authored));
	TestFalse(TEXT("a genuinely different policy is still drift"),
		FCrowdySchemaSync::InvokePolicyEquals(TEXT("{\"type\":\"condition\",\"expression\":\"false\"}"), Enriched));
	TestFalse(TEXT("no policy vs a policy stays a real difference (an empty send clears the server's)"),
		FCrowdySchemaSync::InvokePolicyEquals(FString(), Enriched));

	// A policy is a tree: two require lines lower to an "and" whose rules nest one condition each, and the server
	// compiles EVERY node, so the ast appears inside the nested rules. The strip must walk the whole tree - a
	// top-level-only strip fixed single-condition policies and left every multi-requirement effect churning.
	const FString AuthoredTree = TEXT(
		"{\"type\":\"and\",\"rules\":["
		"{\"type\":\"condition\",\"expression\":\"self.capturestate == 1\"},"
		"{\"type\":\"condition\",\"expression\":\"self.presentplayercount > 0\"}]}");
	const FString EnrichedTree = TEXT(
		"{\"type\":\"and\",\"rules\":["
		"{\"ast\":{\"type\":\"Binary\",\"op\":\"==\"},\"type\":\"condition\",\"expression\":\"self.capturestate == 1\"},"
		"{\"ast\":{\"type\":\"Binary\",\"op\":\">\"},\"type\":\"condition\",\"expression\":\"self.presentplayercount > 0\"}]}");
	TestTrue(TEXT("asts nested inside a policy tree's rules are not drift"),
		FCrowdySchemaSync::InvokePolicyEquals(AuthoredTree, EnrichedTree));
	TestFalse(TEXT("a changed nested expression is still drift"),
		FCrowdySchemaSync::InvokePolicyEquals(
			AuthoredTree, EnrichedTree.Replace(TEXT("> 0"), TEXT("> 1"))));

	// End to end through DiffFunctions: a function whose only authored-vs-readback difference is the compiled ast
	// plans no upsert.
	FCrowdyGameModelFunctionInput Desired = MakeDesiredFunction();
	Desired.InvokePolicyJson = Authored;
	FStudioFunction Server = MakeServerFunctionFrom(Desired);
	Server.InvokePolicyJson = Enriched;

	FCrowdySchemaDelta Delta;
	FCrowdySchemaSync::DiffFunctions({ Desired }, { Server }, Delta);
	TestEqual(TEXT("an unchanged function whose policy readback carries the ast plans no upsert"),
		Delta.FunctionUpserts.Num(), 0);

	return true;
}

// BuildDesiredForClass reflects a CrowdyContainer class's Server Owned attributes into a desired container
// type + property defs, reading each attribute's CDO default via PropertyDefaultToJson (incl. the integral-
// float canonicalization and the JSON-quoted string). Covers the reflection half of the sync.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySchemaSyncDesiredMatchesClassTest,
	"CrowdySDK.GameModel.DesiredSchemaMatchesClass", CrowdySchemaSyncTestFlags)
bool FCrowdySchemaSyncDesiredMatchesClassTest::RunTest(const FString& Parameters)
{
	const FCrowdyDesiredContainerType Desired =
		FCrowdySchemaSync::BuildDesiredForClass(UCrowdySchemaSyncTestTarget::StaticClass());

	TestEqual(TEXT("type name from the CrowdyContainer tag"), Desired.TypeName, FString(TEXT("SchemaSyncHero")));
	TestEqual(TEXT("display name defaults to the type name"), Desired.DisplayName, FString(TEXT("SchemaSyncHero")));
	TestEqual(TEXT("instantiableBy defaults to member"), Desired.InstantiableBy, FString(TEXT("member")));
	TestEqual(TEXT("default visibility defaults to public"), Desired.DefaultVisibility, FString(TEXT("public")));
	TestEqual(TEXT("nine reflected attributes"), Desired.Props.Num(), 9);

	if (const FCrowdyDesiredPropertyDef* Health = FindProp(Desired, TEXT("health")))
	{
		TestEqual(TEXT("health value type"), Health->ValueType, FString(TEXT("int")));
		TestEqual(TEXT("health CDO default"), Health->DefaultValueJson, FString(TEXT("75")));
		TestEqual(TEXT("health visibility default"), Health->Visibility, FString(TEXT("public")));
		TestEqual(TEXT("health writable default"), Health->Writable, FString(TEXT("function")));
	}
	else
	{
		AddError(TEXT("health attribute missing"));
	}

	if (const FCrowdyDesiredPropertyDef* Speed = FindProp(Desired, TEXT("speed")))
	{
		TestEqual(TEXT("speed value type"), Speed->ValueType, FString(TEXT("float")));
		TestEqual(TEXT("fractional float default"), Speed->DefaultValueJson, FString(TEXT("3.5")));
	}
	if (const FCrowdyDesiredPropertyDef* Scale = FindProp(Desired, TEXT("scale")))
	{
		TestEqual(TEXT("integral float canonicalizes to integer text"), Scale->DefaultValueJson, FString(TEXT("2")));
	}
	if (const FCrowdyDesiredPropertyDef* Ready = FindProp(Desired, TEXT("bready")))
	{
		TestEqual(TEXT("bool value type"), Ready->ValueType, FString(TEXT("bool")));
		TestEqual(TEXT("bool default"), Ready->DefaultValueJson, FString(TEXT("true")));
		TestEqual(TEXT("the authored spelling keeps the case the key lost"),
			Ready->AuthoredName(), FString(TEXT("bReady")));
	}
	else
	{
		AddError(TEXT("bready attribute missing (the bool b-prefix leaks into the lowercased key)"));
	}
	if (const FCrowdyDesiredPropertyDef* Title = FindProp(Desired, TEXT("title")))
	{
		TestEqual(TEXT("string value type"), Title->ValueType, FString(TEXT("string")));
		TestEqual(TEXT("string default is JSON-quoted"), Title->DefaultValueJson, FString(TEXT("\"Squire\"")));
	}

	// A scalar array reflects as valueType "array" with its CDO default serialized by the codec.
	if (const FCrowdyDesiredPropertyDef* Loadout = FindProp(Desired, TEXT("loadout")))
	{
		TestEqual(TEXT("array value type"), Loadout->ValueType, FString(TEXT("array")));
		TestEqual(TEXT("array CDO default"), Loadout->DefaultValueJson, FString(TEXT("[10,20]")));
	}
	else
	{
		AddError(TEXT("loadout array attribute missing"));
	}

	// A container ref reflects as valueType "container_ref"; an unset reference carries no default.
	if (const FCrowdyDesiredPropertyDef* Equipped = FindProp(Desired, TEXT("equipped")))
	{
		TestEqual(TEXT("container_ref value type"), Equipped->ValueType, FString(TEXT("container_ref")));
		TestTrue(TEXT("unset ref has no default"), Equipped->DefaultValueJson.IsEmpty());
	}
	else
	{
		AddError(TEXT("equipped container_ref attribute missing"));
	}

	// A plain-struct object reflects as valueType "object" with its CDO default serialized by the codec (member
	// keys sorted, so "Power" before "Rank").
	if (const FCrowdyDesiredPropertyDef* Stats = FindProp(Desired, TEXT("stats")))
	{
		TestEqual(TEXT("object value type"), Stats->ValueType, FString(TEXT("object")));
		TestEqual(TEXT("object CDO default"), Stats->DefaultValueJson, FString(TEXT("{\"Power\":3,\"Rank\":\"gold\"}")));
	}
	else
	{
		AddError(TEXT("stats object attribute missing"));
	}

	// CrowdyKey override + CrowdyVisibility flow through BuildDesiredForClass into the desired property def; the
	// derived "secretscore" key never appears, and Writable keeps its "function" default.
	if (const FCrowdyDesiredPropertyDef* Secret = FindProp(Desired, TEXT("secret")))
	{
		TestEqual(TEXT("overridden-key value type"), Secret->ValueType, FString(TEXT("int")));
		TestEqual(TEXT("owner visibility flows to the property def"), Secret->Visibility, FString(TEXT("owner")));
		TestEqual(TEXT("writable keeps its function default"), Secret->Writable, FString(TEXT("function")));
		// The key an override pins says nothing about the property it was written on, and the derived key would
		// have lowercased that name anyway, so the authored spelling is carried rather than reconstructed later.
		TestEqual(TEXT("the authored property name is carried alongside the key"),
			Secret->AuthoredName(), FString(TEXT("SecretScore")));
	}
	else
	{
		AddError(TEXT("overridden-key 'secret' attribute missing"));
	}
	TestNull(TEXT("derived 'secretscore' key replaced by the override"), FindProp(Desired, TEXT("secretscore")));

	return true;
}

// A re-sync with no code change yields ZERO upserts even when the server's stored defaults are formatted
// differently (100 vs 100.0). This is the idempotency guard: the diff compares defaults SEMANTICALLY.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySchemaSyncResyncNoOpTest,
	"CrowdySDK.GameModel.ResyncIsNoOp", CrowdySchemaSyncTestFlags)
bool FCrowdySchemaSyncResyncNoOpTest::RunTest(const FString& Parameters)
{
	FCrowdyDesiredContainerType Desired = MakeDesiredType(TEXT("Hero"));
	Desired.Props.Add(MakeDesiredProp(TEXT("hp"), TEXT("int"), TEXT("100")));
	Desired.Props.Add(MakeDesiredProp(TEXT("speed"), TEXT("float"), TEXT("2.5")));

	TArray<FCrowdyDesiredContainerType> DesiredSchema;
	DesiredSchema.Add(Desired);

	TArray<FStudioContainerType> CurrentTypes;
	CurrentTypes.Add(MakeServerType(TEXT("Hero"))); // member / public, matches the desired defaults

	TMap<FString, TArray<FStudioPropertyDef>> CurrentProps;
	TArray<FStudioPropertyDef>& HeroProps = CurrentProps.Add(TEXT("Hero"));
	HeroProps.Add(MakeServerProp(TEXT("Hero"), TEXT("hp"), TEXT("int"), TEXT("100.0"))); // 100.0 == 100 semantically
	HeroProps.Add(MakeServerProp(TEXT("Hero"), TEXT("speed"), TEXT("float"), TEXT("2.5")));

	const FCrowdySchemaDelta Delta = FCrowdySchemaSync::DiffSchema(DesiredSchema, CurrentTypes, CurrentProps);

	TestTrue(TEXT("a no-change resync produces no upserts"), Delta.IsEmpty());
	TestEqual(TEXT("no type upserts"), Delta.TypeUpserts.Num(), 0);
	TestEqual(TEXT("no property upserts"), Delta.PropUpserts.Num(), 0);
	TestEqual(TEXT("no warnings when everything matches"), Delta.Warnings.Num(), 0);

	return true;
}

// The diff upserts exactly the changed entities and warns (never deletes) on value-type changes and
// server-only state: a new type, a changed default, a changed value type (+warning), a new property, a
// server-only property (+warning), and a server-only type (+warning).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySchemaSyncDiffDetectsChangeTest,
	"CrowdySDK.GameModel.SchemaDiffDetectsChange", CrowdySchemaSyncTestFlags)
bool FCrowdySchemaSyncDiffDetectsChangeTest::RunTest(const FString& Parameters)
{
	FCrowdyDesiredContainerType Hero = MakeDesiredType(TEXT("Hero"));
	Hero.Props.Add(MakeDesiredProp(TEXT("hp"), TEXT("int"), TEXT("120")));    // default changed 100 -> 120
	Hero.Props.Add(MakeDesiredProp(TEXT("mana"), TEXT("float"), TEXT("50"))); // value type changed int -> float (warning)
	Hero.Props.Add(MakeDesiredProp(TEXT("gold"), TEXT("int"), TEXT("0")));    // new property

	FCrowdyDesiredContainerType Chest = MakeDesiredType(TEXT("Chest"));       // new type
	Chest.Props.Add(MakeDesiredProp(TEXT("locked"), TEXT("bool"), TEXT("true")));

	TArray<FCrowdyDesiredContainerType> DesiredSchema;
	DesiredSchema.Add(Hero);
	DesiredSchema.Add(Chest);

	TArray<FStudioContainerType> CurrentTypes;
	CurrentTypes.Add(MakeServerType(TEXT("Hero")));  // exists, member/public -> no type upsert
	CurrentTypes.Add(MakeServerType(TEXT("Ghost"))); // server-only -> warning

	TMap<FString, TArray<FStudioPropertyDef>> CurrentProps;
	TArray<FStudioPropertyDef>& HeroProps = CurrentProps.Add(TEXT("Hero"));
	HeroProps.Add(MakeServerProp(TEXT("Hero"), TEXT("hp"), TEXT("int"), TEXT("100")));           // default differs
	HeroProps.Add(MakeServerProp(TEXT("Hero"), TEXT("mana"), TEXT("int"), TEXT("50")));          // value type differs -> warning
	HeroProps.Add(MakeServerProp(TEXT("Hero"), TEXT("legacy"), TEXT("string"), TEXT("\"x\""))); // server-only -> warning

	const FCrowdySchemaDelta Delta = FCrowdySchemaSync::DiffSchema(DesiredSchema, CurrentTypes, CurrentProps);

	// Chest is the only type upsert (new). Hero already matches its authored type fields.
	TestEqual(TEXT("one type upsert (the new Chest)"), Delta.TypeUpserts.Num(), 1);
	if (Delta.TypeUpserts.Num() == 1)
	{
		TestEqual(TEXT("Chest is the upserted type"), Delta.TypeUpserts[0].Type.TypeName, FString(TEXT("Chest")));
		TestTrue(TEXT("Chest is flagged new"), Delta.TypeUpserts[0].bIsNew);
	}

	// hp (default), mana (value type), gold (new), Chest.locked (new) = 4 property upserts.
	TestEqual(TEXT("four property upserts"), Delta.PropUpserts.Num(), 4);

	// mana value-type change + Hero.legacy server-only + Ghost server-only type = 3 warnings.
	TestEqual(TEXT("three warnings"), Delta.Warnings.Num(), 3);

	// The server-only entities are also surfaced as structured prune candidates.
	TestEqual(TEXT("one server-only type (Ghost)"), Delta.ServerOnlyTypes.Num(), 1);
	if (Delta.ServerOnlyTypes.Num() == 1)
	{
		TestEqual(TEXT("Ghost is the server-only type"), Delta.ServerOnlyTypes[0], FString(TEXT("Ghost")));
	}
	TestEqual(TEXT("one server-only prop (Hero.legacy)"), Delta.ServerOnlyProps.Num(), 1);
	if (Delta.ServerOnlyProps.Num() == 1)
	{
		TestEqual(TEXT("server-only prop type"), Delta.ServerOnlyProps[0].ContainerTypeName, FString(TEXT("Hero")));
		TestEqual(TEXT("server-only prop key"), Delta.ServerOnlyProps[0].Key, FString(TEXT("legacy")));
	}

	return true;
}

// A type update that changes instantiableBy / defaultPropertyVisibility discloses the transition as an
// access-control warning, so a hand-locked (admin/owner) server type being reset to the code defaults is
// visible in the plan before the user confirms, not a bare "~ update".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySchemaSyncTypeDriftWarnsTest,
	"CrowdySDK.GameModel.SchemaTypeDriftWarns", CrowdySchemaSyncTestFlags)
bool FCrowdySchemaSyncTypeDriftWarnsTest::RunTest(const FString& Parameters)
{
	FCrowdyDesiredContainerType Desired = MakeDesiredType(TEXT("Weapon")); // member / public (code defaults)

	TArray<FCrowdyDesiredContainerType> DesiredSchema;
	DesiredSchema.Add(Desired);

	TArray<FStudioContainerType> CurrentTypes;
	CurrentTypes.Add(MakeServerType(TEXT("Weapon"), TEXT("admin"), TEXT("owner"))); // hand-locked stricter

	TMap<FString, TArray<FStudioPropertyDef>> CurrentProps;
	CurrentProps.Add(TEXT("Weapon")); // no props either side

	const FCrowdySchemaDelta Delta = FCrowdySchemaSync::DiffSchema(DesiredSchema, CurrentTypes, CurrentProps);

	TestEqual(TEXT("the drifted type is upserted"), Delta.TypeUpserts.Num(), 1);
	TestEqual(TEXT("both access-control changes are disclosed"), Delta.Warnings.Num(), 2);
	const bool bInstWarned = Delta.Warnings.ContainsByPredicate([](const FString& W)
	{
		return W.Contains(TEXT("instantiableBy")) && W.Contains(TEXT("admin")) && W.Contains(TEXT("member"));
	});
	TestTrue(TEXT("instantiableBy loosening admin -> member is disclosed"), bInstWarned);

	return true;
}

// A property update that changes visibility / writable discloses the transition as an access-control warning
// (mirrors the type-level SchemaTypeDriftWarns), so a hand-hardened hidden/admin server property being reset to
// the code defaults (public/function) is visible in the plan before the user confirms, not a bare "~ update".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySchemaSyncPropDriftWarnsTest,
	"CrowdySDK.GameModel.SchemaPropDriftWarns", CrowdySchemaSyncTestFlags)
bool FCrowdySchemaSyncPropDriftWarnsTest::RunTest(const FString& Parameters)
{
	FCrowdyDesiredContainerType Hero = MakeDesiredType(TEXT("Hero"));
	Hero.Props.Add(MakeDesiredProp(TEXT("hp"), TEXT("int"), TEXT("100"))); // code defaults: public / function

	TArray<FCrowdyDesiredContainerType> DesiredSchema;
	DesiredSchema.Add(Hero);

	TArray<FStudioContainerType> CurrentTypes;
	CurrentTypes.Add(MakeServerType(TEXT("Hero"))); // member / public -> no type upsert

	TMap<FString, TArray<FStudioPropertyDef>> CurrentProps;
	TArray<FStudioPropertyDef>& HeroProps = CurrentProps.Add(TEXT("Hero"));
	// identical value type + default, but hand-hardened access control (hidden / admin)
	HeroProps.Add(MakeServerProp(TEXT("Hero"), TEXT("hp"), TEXT("int"), TEXT("100"), TEXT("hidden"), TEXT("admin")));

	const FCrowdySchemaDelta Delta = FCrowdySchemaSync::DiffSchema(DesiredSchema, CurrentTypes, CurrentProps);

	TestEqual(TEXT("the access-control-drifted property is upserted"), Delta.PropUpserts.Num(), 1);
	TestEqual(TEXT("both access-control changes are disclosed"), Delta.Warnings.Num(), 2);
	const bool bVisWarned = Delta.Warnings.ContainsByPredicate([](const FString& W)
	{
		return W.Contains(TEXT("visibility")) && W.Contains(TEXT("hidden")) && W.Contains(TEXT("public"));
	});
	TestTrue(TEXT("visibility widening hidden -> public is disclosed"), bVisWarned);
	const bool bWriteWarned = Delta.Warnings.ContainsByPredicate([](const FString& W)
	{
		return W.Contains(TEXT("writable")) && W.Contains(TEXT("admin")) && W.Contains(TEXT("function"));
	});
	TestTrue(TEXT("writable widening admin -> function is disclosed"), bWriteWarned);

	return true;
}

// JsonValueEquals compares JSON-value texts semantically, so server formatting drift is not read as a change.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySchemaSyncJsonValueEqualsTest,
	"CrowdySDK.GameModel.JsonValueEqualsSemantics", CrowdySchemaSyncTestFlags)
bool FCrowdySchemaSyncJsonValueEqualsTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("integral number formats compare equal"), FCrowdySchemaSync::JsonValueEquals(TEXT("100"), TEXT("100.0")));
	TestTrue(TEXT("equal bools"), FCrowdySchemaSync::JsonValueEquals(TEXT("true"), TEXT("true")));
	TestTrue(TEXT("equal strings"), FCrowdySchemaSync::JsonValueEquals(TEXT("\"hi\""), TEXT("\"hi\"")));
	TestTrue(TEXT("surrounding whitespace ignored"), FCrowdySchemaSync::JsonValueEquals(TEXT(" 100 "), TEXT("100")));
	TestTrue(TEXT("empty equals empty"), FCrowdySchemaSync::JsonValueEquals(TEXT(""), TEXT("")));

	TestFalse(TEXT("different numbers differ"), FCrowdySchemaSync::JsonValueEquals(TEXT("100"), TEXT("101")));
	TestFalse(TEXT("different strings differ"), FCrowdySchemaSync::JsonValueEquals(TEXT("\"a\""), TEXT("\"b\"")));
	TestFalse(TEXT("an absent default is not a present one"), FCrowdySchemaSync::JsonValueEquals(TEXT(""), TEXT("0")));

	// A genuinely fractional default within 1e-4 of an integer must NOT collapse to that integer: CanonicalNumber
	// compares against the exact rounded value, so a tiny float default is preserved and still diffs against 0.
	TestFalse(TEXT("a sub-1e-4 fractional default is not equal to the integer it rounds to"),
		FCrowdySchemaSync::JsonValueEquals(TEXT("0.0001"), TEXT("0")));
	TestTrue(TEXT("an exactly-integral float still canonicalizes to the integer"),
		FCrowdySchemaSync::JsonValueEquals(TEXT("2.0"), TEXT("2")));

	return true;
}

// A re-sync of an unchanged effect yields ZERO function upserts even when the server formats a param default
// differently (10 vs 10.0) and reformats the invoke policy (whitespace). The function-half idempotency guard:
// invokePolicyJson and param defaults are compared SEMANTICALLY, the rest structurally.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyFunctionSyncResyncNoOpTest,
	"CrowdySDK.GameModel.FunctionResyncIsNoOp", CrowdySchemaSyncTestFlags)
bool FCrowdyFunctionSyncResyncNoOpTest::RunTest(const FString& Parameters)
{
	const FCrowdyGameModelFunctionInput Desired = MakeDesiredFunction();

	FStudioFunction Server = MakeServerFunctionFrom(Desired);
	Server.Parameters[0].DefaultValueJson = TEXT("10.0");             // 10.0 == 10 semantically
	Server.InvokePolicyJson = TEXT("{ \"type\": \"owner_of_self\" }"); // reformatted, semantically identical

	TArray<FCrowdyGameModelFunctionInput> DesiredFns;
	DesiredFns.Add(Desired);
	TArray<FStudioFunction> CurrentFns;
	CurrentFns.Add(Server);

	FCrowdySchemaDelta Delta;
	FCrowdySchemaSync::DiffFunctions(DesiredFns, CurrentFns, Delta);

	TestEqual(TEXT("no function upserts on a semantic-equal resync"), Delta.FunctionUpserts.Num(), 0);
	TestEqual(TEXT("no server-only functions"), Delta.ServerOnlyFunctions.Num(), 0);
	TestEqual(TEXT("no warnings"), Delta.Warnings.Num(), 0);

	return true;
}

// A function with no server counterpart is a create upsert (bIsNew), with no warning and no server-only entry.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyFunctionSyncCreatesNewTest,
	"CrowdySDK.GameModel.FunctionDiffCreatesNew", CrowdySchemaSyncTestFlags)
bool FCrowdyFunctionSyncCreatesNewTest::RunTest(const FString& Parameters)
{
	TArray<FCrowdyGameModelFunctionInput> DesiredFns;
	DesiredFns.Add(MakeDesiredFunction());
	TArray<FStudioFunction> CurrentFns; // empty server

	FCrowdySchemaDelta Delta;
	FCrowdySchemaSync::DiffFunctions(DesiredFns, CurrentFns, Delta);

	TestEqual(TEXT("one function upsert"), Delta.FunctionUpserts.Num(), 1);
	if (Delta.FunctionUpserts.Num() == 1)
	{
		TestTrue(TEXT("flagged new"), Delta.FunctionUpserts[0].bIsNew);
		TestEqual(TEXT("the take_damage function"), Delta.FunctionUpserts[0].Function.Name, FString(TEXT("take_damage")));
	}
	TestEqual(TEXT("no warnings for a plain create"), Delta.Warnings.Num(), 0);
	TestEqual(TEXT("no server-only functions"), Delta.ServerOnlyFunctions.Num(), 0);

	return true;
}

// The diff upserts exactly the changed + new functions and warns (never deletes) on a server-only function: a
// changed mutation expression is an update, a brand-new function is a create, and a server function no effect
// authors is a server-only prune candidate (+warning).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyFunctionSyncDetectsChangeTest,
	"CrowdySDK.GameModel.FunctionDiffDetectsChange", CrowdySchemaSyncTestFlags)
bool FCrowdyFunctionSyncDetectsChangeTest::RunTest(const FString& Parameters)
{
	const FCrowdyGameModelFunctionInput Damage = MakeDesiredFunction(); // an UPDATE (server holds an old expression)

	FCrowdyGameModelFunctionInput Heal; // a NEW function
	Heal.Name = TEXT("heal");
	Heal.ContainerTypeName = TEXT("Hero");
	Heal.InvokeScope = TEXT("player");
	Heal.InvokePolicyJson = TEXT("{\"type\":\"owner_of_self\"}");
	Heal.Mutations.Add(MakeMutation(TEXT("self"), TEXT("health"), TEXT("min(100, self.health + 10)")));

	TArray<FCrowdyGameModelFunctionInput> DesiredFns;
	DesiredFns.Add(Damage);
	DesiredFns.Add(Heal);

	FStudioFunction ServerDamage = MakeServerFunctionFrom(Damage);
	ServerDamage.Mutations[0].Expression = TEXT("self.health - $amount"); // old, un-clamped expression -> changed
	FStudioFunction Legacy; // server-only, not authored by any effect
	Legacy.Name = TEXT("legacy_fn");
	Legacy.ContainerTypeName = TEXT("Hero");

	TArray<FStudioFunction> CurrentFns;
	CurrentFns.Add(ServerDamage);
	CurrentFns.Add(Legacy);

	FCrowdySchemaDelta Delta;
	FCrowdySchemaSync::DiffFunctions(DesiredFns, CurrentFns, Delta);

	TestEqual(TEXT("two upserts (damage update + heal create)"), Delta.FunctionUpserts.Num(), 2);
	const bool bDamageUpdate = Delta.FunctionUpserts.ContainsByPredicate([](const FCrowdySchemaFunctionUpsert& U)
	{
		return U.Function.Name == TEXT("take_damage") && !U.bIsNew;
	});
	const bool bHealCreate = Delta.FunctionUpserts.ContainsByPredicate([](const FCrowdySchemaFunctionUpsert& U)
	{
		return U.Function.Name == TEXT("heal") && U.bIsNew;
	});
	TestTrue(TEXT("take_damage is an update"), bDamageUpdate);
	TestTrue(TEXT("heal is a create"), bHealCreate);

	TestEqual(TEXT("one server-only function"), Delta.ServerOnlyFunctions.Num(), 1);
	if (Delta.ServerOnlyFunctions.Num() == 1)
	{
		TestEqual(TEXT("legacy_fn is the server-only function"), Delta.ServerOnlyFunctions[0].Name, FString(TEXT("legacy_fn")));
	}
	TestEqual(TEXT("one warning (the server-only function; the mutation-only change is silent)"), Delta.Warnings.Num(), 1);

	return true;
}

// Parameters are matched by NAME, so a server that returns them in a different array order is NOT a false diff;
// a genuine per-parameter change (here a sortOrder change) still upserts.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyFunctionSyncParamOrderTest,
	"CrowdySDK.GameModel.FunctionParamOrderIndependent", CrowdySchemaSyncTestFlags)
bool FCrowdyFunctionSyncParamOrderTest::RunTest(const FString& Parameters)
{
	FCrowdyGameModelFunctionInput Fn;
	Fn.Name = TEXT("cast");
	Fn.ContainerTypeName = TEXT("Hero");
	Fn.InvokeScope = TEXT("player");
	Fn.InvokePolicyJson = TEXT("{\"type\":\"owner_of_self\"}");
	Fn.Parameters.Add(MakeDesiredParam(TEXT("power"), TEXT("int"), true, TEXT(""), 0));
	Fn.Parameters.Add(MakeDesiredParam(TEXT("cost"), TEXT("int"), true, TEXT(""), 1));
	Fn.Mutations.Add(MakeMutation(TEXT("self"), TEXT("mana"), TEXT("self.mana - $cost")));

	TArray<FCrowdyGameModelFunctionInput> DesiredFns;
	DesiredFns.Add(Fn);

	// The server returns the SAME params in REVERSED array order -> must still be a no-op (name-keyed compare).
	FStudioFunction Reordered = MakeServerFunctionFrom(Fn);
	Reordered.Parameters.Swap(0, 1);
	TArray<FStudioFunction> ReorderedFns;
	ReorderedFns.Add(Reordered);

	FCrowdySchemaDelta NoOp;
	FCrowdySchemaSync::DiffFunctions(DesiredFns, ReorderedFns, NoOp);
	TestEqual(TEXT("reordered server params are not a false change"), NoOp.FunctionUpserts.Num(), 0);

	// A genuine per-parameter change (cost's sortOrder) is a real update.
	FStudioFunction Changed = MakeServerFunctionFrom(Fn);
	Changed.Parameters[1].SortOrder = 5; // 'cost' sortOrder 1 -> 5
	TArray<FStudioFunction> ChangedFns;
	ChangedFns.Add(Changed);

	FCrowdySchemaDelta Delta;
	FCrowdySchemaSync::DiffFunctions(DesiredFns, ChangedFns, Delta);
	TestEqual(TEXT("a genuine sortOrder change is an update"), Delta.FunctionUpserts.Num(), 1);

	return true;
}

// A multi-key invokePolicyJson (a compound and/or gate) whose OBJECT keys the server re-serialized in a
// different order is NOT a false change (the canonicalizer sorts object keys); a genuinely reordered RULES
// ARRAY still is a change (arrays are order-significant). Guards the idempotency of compound policies.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyFunctionSyncPolicyKeyOrderTest,
	"CrowdySDK.GameModel.FunctionPolicyKeyOrderNoOp", CrowdySchemaSyncTestFlags)
bool FCrowdyFunctionSyncPolicyKeyOrderTest::RunTest(const FString& Parameters)
{
	FCrowdyGameModelFunctionInput Desired = MakeDesiredFunction();
	Desired.InvokePolicyJson = TEXT("{\"type\":\"and\",\"rules\":[{\"type\":\"owner_of_self\"},{\"type\":\"is_participant\"}]}");

	TArray<FCrowdyGameModelFunctionInput> DesiredFns;
	DesiredFns.Add(Desired);

	// Same policy, top-level object keys reordered + reformatted -> must be semantically equal (no upsert).
	FStudioFunction Server = MakeServerFunctionFrom(Desired);
	Server.InvokePolicyJson = TEXT("{ \"rules\": [ { \"type\": \"owner_of_self\" }, { \"type\": \"is_participant\" } ], \"type\": \"and\" }");
	TArray<FStudioFunction> SameFns;
	SameFns.Add(Server);

	FCrowdySchemaDelta NoOp;
	FCrowdySchemaSync::DiffFunctions(DesiredFns, SameFns, NoOp);
	TestEqual(TEXT("a key-reordered compound policy is not a false change"), NoOp.FunctionUpserts.Num(), 0);

	// A genuinely reordered rules ARRAY (order-significant) IS a real change.
	FStudioFunction Reordered = MakeServerFunctionFrom(Desired);
	Reordered.InvokePolicyJson = TEXT("{\"type\":\"and\",\"rules\":[{\"type\":\"is_participant\"},{\"type\":\"owner_of_self\"}]}");
	TArray<FStudioFunction> ReorderedFns;
	ReorderedFns.Add(Reordered);

	FCrowdySchemaDelta Delta;
	FCrowdySchemaSync::DiffFunctions(DesiredFns, ReorderedFns, Delta);
	TestEqual(TEXT("a reordered rules array is a real change"), Delta.FunctionUpserts.Num(), 1);

	return true;
}

// The sync AUTHORS the SDK's own model-changed notification and PRESERVES a non-SDK
// (seed/console-authored) one. An effect that authors no notification (carrier None) removes the server's SDK
// notification but keeps the non-SDK one - the effect, not the server, is the source of truth for the SDK's own.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyFunctionSyncPreservesNotificationsTest,
	"CrowdySDK.GameModel.FunctionPreservesNotifications", CrowdySchemaSyncTestFlags)
bool FCrowdyFunctionSyncPreservesNotificationsTest::RunTest(const FString& Parameters)
{
	const FCrowdyGameModelFunctionInput Desired = MakeDesiredFunction(); // carrier None: authors no notifications

	FStudioFunction Server = MakeServerFunctionFrom(Desired);
	Server.Mutations[0].Expression = TEXT("self.health - $amount"); // force an update
	Server.Notifications.Add(MakeSdkChannelNotif(/*bAddressed*/ true)); // the SDK's own, should be removed
	Server.Notifications.Add(MakeNonSdkNotif());                            // a seed/console one, should be kept

	TArray<FCrowdyGameModelFunctionInput> DesiredFns;
	DesiredFns.Add(Desired);
	TArray<FStudioFunction> CurrentFns;
	CurrentFns.Add(Server);

	FCrowdySchemaDelta Delta;
	FCrowdySchemaSync::DiffFunctions(DesiredFns, CurrentFns, Delta);

	TestEqual(TEXT("one update upsert"), Delta.FunctionUpserts.Num(), 1);
	if (Delta.FunctionUpserts.Num() == 1)
	{
		const FCrowdyGameModelFunctionInput& Upserted = Delta.FunctionUpserts[0].Function;
		if (TestEqual(TEXT("only the non-SDK notification remains"), Upserted.Notifications.Num(), 1))
		{
			TestEqual(TEXT("the preserved one is the non-SDK spatial"), Upserted.Notifications[0].Kind, FString(TEXT("spatial")));
			TestEqual(TEXT("with its own event_type"), Upserted.Notifications[0].Args[0].Expression, FString(TEXT("42")));
		}
	}

	return true;
}

// An effect's signals and its model-changed hint share one notifications array, so a notification the sync cannot
// author must not take the ones it CAN author down with it. With a Spatial carrier (unauthorable) plus a signal
// (a channel notification, authorable), the signal is still written to the server and the spatial is preserved.
// Before this, the whole set was discarded and the signal silently never reached the server, so it never fired.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyFunctionAuthorsSignalBesideSpatialTest,
	"CrowdySDK.GameModel.FunctionAuthorsSignalBesideSpatial", CrowdySchemaSyncTestFlags)
bool FCrowdyFunctionAuthorsSignalBesideSpatialTest::RunTest(const FString& Parameters)
{
	// The SDK's spatial model-changed notification: real, SDK-owned, and not authorable by this sync.
	FCrowdyGameModelNotification SdkSpatial;
	SdkSpatial.Kind = TEXT("spatial");
	SdkSpatial.EmitAs = TEXT("server_event");
	SdkSpatial.Args.Add(MakeNotifArg(TEXT("event_type"),
		FString::FromInt(CrowdyGameModelMetaKeys::ModelChangedEventType)));

	// A signal: a channel notification carrying a destination, so it IS authorable.
	FCrowdyGameModelNotification Signal;
	Signal.Kind = TEXT("channel");
	Signal.Args.Add(MakeNotifArg(TEXT("payload"),
		FString::Printf(TEXT("concat(\"%sBossWave:\", $self_container_id)"),
			CrowdyGameModelMetaKeys::SignalChannelPrefix)));
	Signal.Args.Add(MakeNotifArg(TEXT("channel_name"),
		FString::Printf(TEXT("$%s"), CrowdyGameModelMetaKeys::SessionChannelNameParam)));

	FCrowdyGameModelFunctionInput Desired = MakeDesiredFunction();
	Desired.Notifications.Add(SdkSpatial);
	Desired.Notifications.Add(Signal);

	// The server has the spatial already, but not the signal.
	FStudioFunction Server = MakeServerFunctionFrom(Desired);
	Server.Notifications.Add(SdkSpatial);

	TArray<FCrowdyGameModelFunctionInput> DesiredFns;
	DesiredFns.Add(Desired);
	TArray<FStudioFunction> CurrentFns;
	CurrentFns.Add(Server);

	FCrowdySchemaDelta Delta;
	FCrowdySchemaSync::DiffFunctions(DesiredFns, CurrentFns, Delta);

	if (TestEqual(TEXT("authoring the signal is a real change"), Delta.FunctionUpserts.Num(), 1))
	{
		const TArray<FCrowdyGameModelNotification>& Upserted = Delta.FunctionUpserts[0].Function.Notifications;
		const bool bHasSignal = Upserted.ContainsByPredicate(
			[](const FCrowdyGameModelNotification& N)
			{
				return N.Kind == TEXT("channel") && N.Args.ContainsByPredicate(
					[](const FCrowdyGameModelNotificationArg& A)
					{
						return A.Name == TEXT("payload")
							&& A.Expression.Contains(CrowdyGameModelMetaKeys::SignalChannelPrefix);
					});
			});
		const bool bHasSpatial = Upserted.ContainsByPredicate(
			[](const FCrowdyGameModelNotification& N) { return N.Kind == TEXT("spatial"); });

		TestTrue(TEXT("the signal is authored despite the unauthorable spatial"), bHasSignal);
		TestTrue(TEXT("the unauthorable spatial is preserved, not wiped"), bHasSpatial);
		TestEqual(TEXT("exactly those two"), Upserted.Num(), 2);
	}

	// With nothing authorable at all, the server's notifications are preserved verbatim rather than deleted.
	FCrowdyGameModelFunctionInput SpatialOnly = MakeDesiredFunction();
	SpatialOnly.Notifications.Add(SdkSpatial);
	FStudioFunction SpatialServer = MakeServerFunctionFrom(SpatialOnly);
	SpatialServer.Notifications.Add(SdkSpatial);

	TArray<FCrowdyGameModelFunctionInput> SpatialDesiredFns;
	SpatialDesiredFns.Add(SpatialOnly);
	TArray<FStudioFunction> SpatialCurrentFns;
	SpatialCurrentFns.Add(SpatialServer);

	FCrowdySchemaDelta SpatialDelta;
	FCrowdySchemaSync::DiffFunctions(SpatialDesiredFns, SpatialCurrentFns, SpatialDelta);
	TestEqual(TEXT("an unauthorable-only function is not churned into an upsert"),
		SpatialDelta.FunctionUpserts.Num(), 0);

	return true;
}

// IsSdkOwnedNotification recognizes exactly the two model-changed shapes the lowering authors (a channel
// payload carrying the cmc: prefix, a spatial notification stamping the reserved 60000 event_type) and nothing else.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNotificationIsSdkOwnedTest,
	"CrowdySDK.GameModel.NotificationIsSdkOwned", CrowdySchemaSyncTestFlags)
bool FCrowdyNotificationIsSdkOwnedTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("channel cmc payload is SDK-owned"), FCrowdySchemaSync::IsSdkOwnedNotification(MakeSdkChannelNotif(true)));
	TestTrue(TEXT("channel cmc payload (no id yet) is SDK-owned"), FCrowdySchemaSync::IsSdkOwnedNotification(MakeSdkChannelNotif(false)));

	FCrowdyGameModelNotification SdkSpatial;
	SdkSpatial.Kind = TEXT("spatial");
	SdkSpatial.Args.Add(MakeNotifArg(TEXT("event_type"), FString::FromInt(CrowdyGameModelMetaKeys::ModelChangedEventType)));
	TestTrue(TEXT("spatial 60000 is SDK-owned"), FCrowdySchemaSync::IsSdkOwnedNotification(SdkSpatial));

	// Not SDK-owned: a spatial with a different event_type, or a channel whose payload lacks the prefix.
	TestFalse(TEXT("spatial 42 is not SDK-owned"), FCrowdySchemaSync::IsSdkOwnedNotification(MakeNonSdkNotif()));
	FCrowdyGameModelNotification PlainChannel;
	PlainChannel.Kind = TEXT("channel");
	PlainChannel.Args.Add(MakeNotifArg(TEXT("payload"), TEXT("\"chat hello\"")));
	TestFalse(TEXT("a chat channel payload is not SDK-owned"), FCrowdySchemaSync::IsSdkOwnedNotification(PlainChannel));

	return true;
}

// InjectSessionChannelTarget addresses SDK channel notifications by NAME, never with a resolved id, and never
// touches a non-SDK notification.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNotificationInjectChannelTargetTest,
	"CrowdySDK.GameModel.NotificationInjectChannelTarget", CrowdySchemaSyncTestFlags)
bool FCrowdyNotificationInjectChannelTargetTest::RunTest(const FString& Parameters)
{
	auto ArgOf = [](const FCrowdyGameModelNotification& N, const TCHAR* Name) -> FString
	{
		const FCrowdyGameModelNotificationArg* A = N.Args.FindByPredicate(
			[Name](const FCrowdyGameModelNotificationArg& X) { return X.Name == Name; });
		return A ? A->Expression : FString();
	};
	// Spelled out, NOT built from the constant the production line uses. The server injects this exact token; both
	// sides deriving it from one symbol would let a typo in that symbol keep this test green while the emitted
	// channel_name references a param the server never injects, which is silent by construction.
	const FString ExpectedName = TEXT("$session_channel_name");
	TestEqual(TEXT("the injected param keeps its wire spelling"),
		FString(CrowdyGameModelMetaKeys::SessionChannelNameParam), FString(TEXT("session_channel_name")));

	FCrowdyGameModelFunctionInput Fn;
	Fn.Name = TEXT("f");
	Fn.Notifications.Add(MakeSdkChannelNotif(/*bAddressed*/ false));
	Fn.Notifications.Add(MakeNonSdkNotif());
	TArray<FCrowdyGameModelFunctionInput> Fns;
	Fns.Add(Fn);

	// The destination is the injected system param, not anything the client resolved, and it needs no live server
	// read to produce: an app with no session channel yet still gets a complete, authorable notification.
	FCrowdySchemaSync::InjectSessionChannelTarget(Fns);
	TestEqual(TEXT("channel_name injected"), ArgOf(Fns[0].Notifications[0], TEXT("channel_name")), ExpectedName);
	TestEqual(TEXT("no channel_id authored"), ArgOf(Fns[0].Notifications[0], TEXT("channel_id")), FString());
	TestEqual(TEXT("non-SDK notification untouched"), Fns[0].Notifications[1].Args.Num(), 1);

	// Re-injecting replaces (idempotent, no duplicate arg), so a re-plan is a true no-op.
	FCrowdySchemaSync::InjectSessionChannelTarget(Fns);
	TestEqual(TEXT("channel_name not duplicated"), Fns[0].Notifications[0].Args.Num(), 2);
	TestEqual(TEXT("channel_name unchanged"), ArgOf(Fns[0].Notifications[0], TEXT("channel_name")), ExpectedName);

	return true;
}

// The migration: a notification carrying a literal channel id (what an app recreated or moved between orgs is left
// holding) is REWRITTEN to name the channel, and the stale id is removed rather than left beside the name. The
// server takes exactly one destination, so leaving both would author a mutation it refuses.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNotificationLiteralChannelIdIsReplacedTest,
	"CrowdySDK.GameModel.NotificationLiteralChannelIdIsReplaced", CrowdySchemaSyncTestFlags)
bool FCrowdyNotificationLiteralChannelIdIsReplacedTest::RunTest(const FString& Parameters)
{
	FCrowdyGameModelFunctionInput Fn;
	Fn.Name = TEXT("copied_from_another_app");
	Fn.Notifications.Add(MakeLegacyChannelIdNotif(TEXT("83847407127552")));
	TArray<FCrowdyGameModelFunctionInput> Fns;
	Fns.Add(Fn);

	// The stale literal makes the notification authorable today, so nothing downstream would ever flag it: the
	// removal here is the whole migration.
	TestTrue(TEXT("the legacy shape is authorable before the migration"),
		FCrowdySchemaSync::AnyFunctionNeedsSessionChannel(Fns));

	FCrowdySchemaSync::InjectSessionChannelTarget(Fns);

	const FCrowdyGameModelNotification& Migrated = Fns[0].Notifications[0];
	TestEqual(TEXT("exactly payload + channel_name survive"), Migrated.Args.Num(), 2);
	TestFalse(TEXT("the stale channel_id is gone"), Migrated.Args.ContainsByPredicate(
		[](const FCrowdyGameModelNotificationArg& A) { return A.Name == TEXT("channel_id"); }));
	TestTrue(TEXT("the channel is named"), Migrated.Args.ContainsByPredicate(
		[](const FCrowdyGameModelNotificationArg& A)
		{
			return A.Name == TEXT("channel_name") && A.Expression == TEXT("$session_channel_name");
		}));

	return true;
}

// AnyFunctionNeedsSessionChannel is true exactly when a function declares an SDK-owned channel notification (the
// signal the apply path uses to decide whether a missing session channel is worth auto-creating). It matches
// InjectSessionChannelTarget's filter: a non-SDK channel notification, a spatial one, or none do NOT count.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyFunctionNeedsSessionChannelTest,
	"CrowdySDK.GameModel.FunctionNeedsSessionChannel", CrowdySchemaSyncTestFlags)
bool FCrowdyFunctionNeedsSessionChannelTest::RunTest(const FString& Parameters)
{
	// No functions -> nothing needs the channel.
	TestFalse(TEXT("empty set needs no channel"), FCrowdySchemaSync::AnyFunctionNeedsSessionChannel({}));

	// A function with only a non-SDK notification does not need the session channel.
	FCrowdyGameModelFunctionInput NonSdk;
	NonSdk.Name = TEXT("plain");
	NonSdk.Notifications.Add(MakeNonSdkNotif());
	TArray<FCrowdyGameModelFunctionInput> NonSdkFns;
	NonSdkFns.Add(NonSdk);
	TestFalse(TEXT("a non-SDK notification does not need the channel"), FCrowdySchemaSync::AnyFunctionNeedsSessionChannel(NonSdkFns));

	// A function with an SDK channel notification needs the channel, whether or not the id is resolved yet.
	FCrowdyGameModelFunctionInput Unresolved;
	Unresolved.Name = TEXT("effect");
	Unresolved.Notifications.Add(MakeSdkChannelNotif(/*bAddressed*/ false));
	TArray<FCrowdyGameModelFunctionInput> UnresolvedFns;
	UnresolvedFns.Add(Unresolved);
	TestTrue(TEXT("an unresolved SDK channel notification needs the channel"), FCrowdySchemaSync::AnyFunctionNeedsSessionChannel(UnresolvedFns));

	FCrowdyGameModelFunctionInput Resolved;
	Resolved.Name = TEXT("effect2");
	Resolved.Notifications.Add(MakeSdkChannelNotif(/*bAddressed*/ true));
	TArray<FCrowdyGameModelFunctionInput> ResolvedFns;
	ResolvedFns.Add(Resolved);
	TestTrue(TEXT("a resolved SDK channel notification still needs the channel"), FCrowdySchemaSync::AnyFunctionNeedsSessionChannel(ResolvedFns));

	// Mixed: one plain function plus one SDK-channel function -> needs the channel.
	TArray<FCrowdyGameModelFunctionInput> Mixed;
	Mixed.Add(NonSdk);
	Mixed.Add(Resolved);
	TestTrue(TEXT("a mixed set with one SDK channel notification needs the channel"), FCrowdySchemaSync::AnyFunctionNeedsSessionChannel(Mixed));

	return true;
}

// A resolved channel notification is AUTHORED on the upsert, a re-plan is a no-op, and an unresolved
// channel (no id) is NOT authored - it warns and leaves the server's notifications untouched.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyFunctionSyncAuthorsChannelNotificationTest,
	"CrowdySDK.GameModel.FunctionAuthorsChannelNotification", CrowdySchemaSyncTestFlags)
bool FCrowdyFunctionSyncAuthorsChannelNotificationTest::RunTest(const FString& Parameters)
{
	// An effect that authors a resolved channel notification, against a server function that has none: an update
	// that authors it (the definition matches otherwise, so the notification alone drives the upsert).
	FCrowdyGameModelFunctionInput Desired = MakeDesiredFunction();
	Desired.Notifications.Add(MakeSdkChannelNotif(/*bAddressed*/ true));

	TArray<FCrowdyGameModelFunctionInput> DesiredFns;
	DesiredFns.Add(Desired);
	TArray<FStudioFunction> CurrentFns;
	CurrentFns.Add(MakeServerFunctionFrom(Desired)); // server has the function but NO notifications

	FCrowdySchemaDelta Delta;
	FCrowdySchemaSync::DiffFunctions(DesiredFns, CurrentFns, Delta);
	if (TestEqual(TEXT("notification-only diff is one upsert"), Delta.FunctionUpserts.Num(), 1))
	{
		TestEqual(TEXT("the channel notification is authored"), Delta.FunctionUpserts[0].Function.Notifications.Num(), 1);
	}

	// Re-plan against a server that now carries the authored notification: a genuine no-op.
	FStudioFunction Synced = MakeServerFunctionFrom(Desired);
	Synced.Notifications.Add(MakeSdkChannelNotif(/*bAddressed*/ true));
	TArray<FStudioFunction> SyncedFns;
	SyncedFns.Add(Synced);
	FCrowdySchemaDelta NoOp;
	FCrowdySchemaSync::DiffFunctions(DesiredFns, SyncedFns, NoOp);
	TestEqual(TEXT("re-plan authors nothing new"), NoOp.FunctionUpserts.Num(), 0);

	// A channel notification with NO destination: the sync does not author it, warns, and leaves the server untouched.
	FCrowdyGameModelFunctionInput Unaddressed = MakeDesiredFunction();
	Unaddressed.Notifications.Add(MakeSdkChannelNotif(/*bAddressed*/ false));
	TArray<FCrowdyGameModelFunctionInput> UnaddressedFns;
	UnaddressedFns.Add(Unaddressed);
	TArray<FStudioFunction> ServerNoNotif;
	ServerNoNotif.Add(MakeServerFunctionFrom(Unaddressed)); // matches otherwise, no notifications
	FCrowdySchemaDelta UnaddressedDelta;
	FCrowdySchemaSync::DiffFunctions(UnaddressedFns, ServerNoNotif, UnaddressedDelta);
	TestEqual(TEXT("an unaddressed channel authors no upsert"), UnaddressedDelta.FunctionUpserts.Num(), 0);
	const bool bWarned = UnaddressedDelta.Warnings.ContainsByPredicate(
		[](const FString& W) { return W.Contains(TEXT("cannot address")); });
	TestTrue(TEXT("an unaddressed channel warns"), bWarned);

	// BOTH destinations at once is refused the same way. The server takes exactly one, so authoring the pair would
	// be rejected at the mutation with nothing earlier to point at; a gate accepting "either present" would let it
	// through and mask a failure to strip the stale id.
	FCrowdyGameModelFunctionInput Both = MakeDesiredFunction();
	FCrowdyGameModelNotification BothArgs = MakeSdkChannelNotif(/*bAddressed*/ true);
	BothArgs.Args.Add(MakeNotifArg(TEXT("channel_id"), TEXT("777")));
	Both.Notifications.Add(BothArgs);
	TArray<FCrowdyGameModelFunctionInput> BothFns;
	BothFns.Add(Both);
	TArray<FStudioFunction> ServerForBoth;
	ServerForBoth.Add(MakeServerFunctionFrom(Both));
	FCrowdySchemaDelta BothDelta;
	FCrowdySchemaSync::DiffFunctions(BothFns, ServerForBoth, BothDelta);
	TestEqual(TEXT("two destinations author no upsert"), BothDelta.FunctionUpserts.Num(), 0);

	return true;
}

// A server function whose name is in RecognizedFunctionNames (owned by an effect that was SKIPPED this plan, e.g. an
// unmigrated flipped effect) is protected: it is NOT offered for prune even though it is not in the desired set. A
// genuine orphan (recognized by nothing) is still a prune candidate. Guards against S4's migration skip turning a
// live server function into a prune-delete.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyFunctionSyncProtectsRecognizedTest,
	"CrowdySDK.GameModel.FunctionDiffProtectsRecognized", CrowdySchemaSyncTestFlags)
bool FCrowdyFunctionSyncProtectsRecognizedTest::RunTest(const FString& Parameters)
{
	// Desired holds nothing (every effect this plan was skipped, or there are none).
	TArray<FCrowdyGameModelFunctionInput> DesiredFns;

	// The server has two functions: one owned by a skipped effect (recognized), one a true orphan.
	FStudioFunction Skipped;
	Skipped.Name = TEXT("take_damage"); // an effect asset owns this name but was skipped (unmigrated)
	Skipped.ContainerTypeName = TEXT("Hero");
	FStudioFunction Orphan;
	Orphan.Name = TEXT("legacy_fn"); // no effect owns this
	Orphan.ContainerTypeName = TEXT("Hero");

	TArray<FStudioFunction> CurrentFns;
	CurrentFns.Add(Skipped);
	CurrentFns.Add(Orphan);

	TSet<FString> Recognized;
	Recognized.Add(TEXT("take_damage")); // the skipped effect's function name is still recognized

	FCrowdySchemaDelta Delta;
	FCrowdySchemaSync::DiffFunctions(DesiredFns, CurrentFns, Delta, Recognized);

	if (TestEqual(TEXT("one server-only function"), Delta.ServerOnlyFunctions.Num(), 1))
	{
		TestEqual(TEXT("only the orphan is prunable"), Delta.ServerOnlyFunctions[0].Name, FString(TEXT("legacy_fn")));
	}
	TestFalse(TEXT("the recognized function is not a prune candidate"),
		Delta.HasServerOnlyFunction(TEXT("take_damage")));

	// Without the recognized set (the default arg), both are prune candidates - guards against the protection
	// silently becoming a no-op.
	FCrowdySchemaDelta Unprotected;
	FCrowdySchemaSync::DiffFunctions(DesiredFns, CurrentFns, Unprotected);
	TestEqual(TEXT("without protection both are prunable"), Unprotected.ServerOnlyFunctions.Num(), 2);

	return true;
}

// The reserved collection name helpers: the touch-function name lowercases + prefixes its type (so the runtime,
// handed the parent's type by the caller, and the schema sync, reading the CrowdyContainer tag, agree regardless
// of casing), and the reserved-key / reserved-function predicates recognize exactly the reserved forms.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCollectionReservedNamesTest,
	"CrowdySDK.GameModel.CollectionReservedNames", CrowdySchemaSyncTestFlags)
bool FCrowdyCollectionReservedNamesTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("touch name lowercases the type and prefixes it"),
		CrowdyGameModelMetaKeys::CollectionTouchFunctionName(TEXT("Inventory")), FString(TEXT("__crowdy_touch_inventory")));
	TestEqual(TEXT("the touch name is caller-casing-independent"),
		CrowdyGameModelMetaKeys::CollectionTouchFunctionName(TEXT("inventory")),
		CrowdyGameModelMetaKeys::CollectionTouchFunctionName(TEXT("Inventory")));

	TestTrue(TEXT("crowdy_rev is a reserved key"), CrowdyGameModelMetaKeys::IsReservedCollectionKey(TEXT("crowdy_rev")));
	TestFalse(TEXT("a normal key is not reserved"), CrowdyGameModelMetaKeys::IsReservedCollectionKey(TEXT("health")));
	TestTrue(TEXT("a touch name is a reserved function"),
		CrowdyGameModelMetaKeys::IsReservedCollectionFunctionName(TEXT("__crowdy_touch_inventory")));
	TestFalse(TEXT("a designer function is not reserved"),
		CrowdyGameModelMetaKeys::IsReservedCollectionFunctionName(TEXT("take_damage")));

	return true;
}

// AppendReservedCollectionSchema adds a reserved crowdy_rev int (default 0, public, function-writable) to every
// desired type and a per-type __crowdy_touch_<type> function (self.crowdy_rev + 1, no params, an SDK-owned channel
// notification naming the parent via $self_container_id, always-true policy) that is recognized (never pruned).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCollectionSchemaProvisionedTest,
	"CrowdySDK.GameModel.CollectionSchemaProvisioned", CrowdySchemaSyncTestFlags)
bool FCrowdyCollectionSchemaProvisionedTest::RunTest(const FString& Parameters)
{
	TArray<FCrowdyDesiredContainerType> Types;
	Types.Add(MakeDesiredType(TEXT("Inventory")));
	Types.Add(MakeDesiredType(TEXT("Chest")));

	TArray<FCrowdyGameModelFunctionInput> Functions;
	TSet<FString> Recognized;
	TArray<FString> Warnings;
	FCrowdySchemaSync::AppendReservedCollectionSchema(Types, Functions, Recognized, Warnings);

	TestEqual(TEXT("no collision warnings for clean types"), Warnings.Num(), 0);

	for (const FCrowdyDesiredContainerType& T : Types)
	{
		const FCrowdyDesiredPropertyDef* Rev = FindProp(T, TEXT("crowdy_rev"));
		if (TestNotNull(*FString::Printf(TEXT("%s has crowdy_rev"), *T.TypeName), Rev))
		{
			TestEqual(TEXT("crowdy_rev is int"), Rev->ValueType, FString(TEXT("int")));
			TestEqual(TEXT("crowdy_rev default 0"), Rev->DefaultValueJson, FString(TEXT("0")));
			TestEqual(TEXT("crowdy_rev is public so a pull observes the bump"), Rev->Visibility, FString(TEXT("public")));
			TestEqual(TEXT("crowdy_rev is function-writable"), Rev->Writable, FString(TEXT("function")));
		}
	}

	TestEqual(TEXT("one touch function per type"), Functions.Num(), 2);

	const FString InvTouch = CrowdyGameModelMetaKeys::CollectionTouchFunctionName(TEXT("Inventory"));
	const FCrowdyGameModelFunctionInput* Touch = Functions.FindByPredicate(
		[&InvTouch](const FCrowdyGameModelFunctionInput& F) { return F.Name == InvTouch; });
	if (TestNotNull(TEXT("the inventory touch function exists"), Touch))
	{
		TestEqual(TEXT("bound to its container type"), Touch->ContainerTypeName, FString(TEXT("Inventory")));
		TestEqual(TEXT("player scope"), Touch->InvokeScope, FString(TEXT("player")));
		TestFalse(TEXT("has an invoke policy (idempotent, not empty)"), Touch->InvokePolicyJson.IsEmpty());
		if (TestEqual(TEXT("one mutation"), Touch->Mutations.Num(), 1))
		{
			TestEqual(TEXT("bumps crowdy_rev on self"), Touch->Mutations[0].Property, FString(TEXT("crowdy_rev")));
			TestEqual(TEXT("bump target is self"), Touch->Mutations[0].Target, FString(TEXT("self")));
			TestTrue(TEXT("bump coalesces an unset counter (legacy-container safe)"),
				Touch->Mutations[0].Expression.Contains(TEXT("coalesce")));
		}
		// The touch needs no params: the notification names the parent via the server-injected $self_container_id.
		TestEqual(TEXT("no params (server injects $self_container_id)"), Touch->Parameters.Num(), 0);
		if (TestEqual(TEXT("one notification"), Touch->Notifications.Num(), 1))
		{
			TestTrue(TEXT("the touch notification is SDK-owned (channel-id-injectable, prune diff)"),
				FCrowdySchemaSync::IsSdkOwnedNotification(Touch->Notifications[0]));
			if (const FCrowdyGameModelNotificationArg* Payload = Touch->Notifications[0].Args.FindByPredicate(
					[](const FCrowdyGameModelNotificationArg& A) { return A.Name == TEXT("payload"); }))
			{
				TestTrue(TEXT("payload names the parent via $self_container_id"),
					Payload->Expression.Contains(TEXT("$self_container_id")));
				TestFalse(TEXT("no to_string cast on a string param"),
					Payload->Expression.Contains(TEXT("to_string(")));
			}
		}
	}
	TestTrue(TEXT("the touch name is recognized so it is never pruned"), Recognized.Contains(InvTouch));

	return true;
}

// A re-sync of a provisioned type is a genuine no-op: the server already has crowdy_rev and the touch function
// (with its resolved channel notification), so neither the schema diff nor the function diff upserts anything, and
// the recognized touch name keeps it off the prune list. The collection idempotency guard.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCollectionSchemaResyncNoOpTest,
	"CrowdySDK.GameModel.CollectionSchemaResyncIsNoOp", CrowdySchemaSyncTestFlags)
bool FCrowdyCollectionSchemaResyncNoOpTest::RunTest(const FString& Parameters)
{
	TArray<FCrowdyDesiredContainerType> Types;
	Types.Add(MakeDesiredType(TEXT("Inventory")));
	TArray<FCrowdyGameModelFunctionInput> Functions;
	TSet<FString> Recognized;
	TArray<FString> Warnings;
	FCrowdySchemaSync::AppendReservedCollectionSchema(Types, Functions, Recognized, Warnings);

	// Address the channel the way the controller does before diffing, so the desired notification is complete.
	FCrowdySchemaSync::InjectSessionChannelTarget(Functions);

	TArray<FStudioContainerType> CurrentTypes;
	CurrentTypes.Add(MakeServerType(TEXT("Inventory")));
	TMap<FString, TArray<FStudioPropertyDef>> CurrentProps;
	TArray<FStudioPropertyDef>& InvProps = CurrentProps.Add(TEXT("Inventory"));
	InvProps.Add(MakeServerProp(TEXT("Inventory"), TEXT("crowdy_rev"), TEXT("int"), TEXT("0")));

	const FCrowdySchemaDelta SchemaDelta = FCrowdySchemaSync::DiffSchema(Types, CurrentTypes, CurrentProps);
	TestEqual(TEXT("crowdy_rev does not re-upsert on resync"), SchemaDelta.PropUpserts.Num(), 0);

	// The server carries the touch function field-for-field, including the resolved channel notification.
	FStudioFunction ServerTouch = MakeServerFunctionFrom(Functions[0]);
	ServerTouch.Notifications = Functions[0].Notifications;
	TArray<FStudioFunction> CurrentFns;
	CurrentFns.Add(ServerTouch);

	FCrowdySchemaDelta FnDelta;
	FCrowdySchemaSync::DiffFunctions(Functions, CurrentFns, FnDelta, Recognized);
	TestEqual(TEXT("the touch function does not re-upsert on resync"), FnDelta.FunctionUpserts.Num(), 0);
	TestEqual(TEXT("the touch function is not offered for prune"), FnDelta.ServerOnlyFunctions.Num(), 0);

	return true;
}

// A designer attribute that already resolves to the reserved crowdy_rev key blocks that ONE type's collection
// plumbing (no duplicate crowdy_rev, no touch function) with a warning, while a clean sibling type is still fully
// provisioned. The plan-time collision error.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCollectionSchemaCollisionTest,
	"CrowdySDK.GameModel.CollectionSchemaCollisionSkips", CrowdySchemaSyncTestFlags)
bool FCrowdyCollectionSchemaCollisionTest::RunTest(const FString& Parameters)
{
	FCrowdyDesiredContainerType Colliding = MakeDesiredType(TEXT("Ledger"));
	Colliding.Props.Add(MakeDesiredProp(TEXT("crowdy_rev"), TEXT("int"), TEXT("7"))); // designer claimed the reserved key

	TArray<FCrowdyDesiredContainerType> Types;
	Types.Add(MakeDesiredType(TEXT("Inventory"))); // clean
	Types.Add(Colliding);
	TArray<FCrowdyGameModelFunctionInput> Functions;
	TSet<FString> Recognized;
	TArray<FString> Warnings;
	FCrowdySchemaSync::AppendReservedCollectionSchema(Types, Functions, Recognized, Warnings);

	// The colliding type keeps only its own crowdy_rev (never a second) and gets no touch function.
	const FCrowdyDesiredContainerType& LedgerOut = Types[1];
	int32 RevCount = 0;
	for (const FCrowdyDesiredPropertyDef& P : LedgerOut.Props)
	{
		if (P.Key == TEXT("crowdy_rev")) { ++RevCount; }
	}
	TestEqual(TEXT("no duplicate crowdy_rev on the colliding type"), RevCount, 1);
	if (const FCrowdyDesiredPropertyDef* Rev = FindProp(LedgerOut, TEXT("crowdy_rev")))
	{
		TestEqual(TEXT("the designer's crowdy_rev is untouched"), Rev->DefaultValueJson, FString(TEXT("7")));
	}
	const FString LedgerTouch = CrowdyGameModelMetaKeys::CollectionTouchFunctionName(TEXT("Ledger"));
	TestFalse(TEXT("no touch function for the colliding type"),
		Functions.ContainsByPredicate([&LedgerTouch](const FCrowdyGameModelFunctionInput& F) { return F.Name == LedgerTouch; }));

	// The clean sibling type is still fully provisioned.
	const FString InvTouch = CrowdyGameModelMetaKeys::CollectionTouchFunctionName(TEXT("Inventory"));
	TestTrue(TEXT("the clean type still gets its touch"),
		Functions.ContainsByPredicate([&InvTouch](const FCrowdyGameModelFunctionInput& F) { return F.Name == InvTouch; }));
	TestNotNull(TEXT("the clean type still gets crowdy_rev"), FindProp(Types[0], TEXT("crowdy_rev")));

	TestTrue(TEXT("the collision is warned"),
		Warnings.ContainsByPredicate([](const FString& W) { return W.Contains(TEXT("Ledger")); }));

	return true;
}

// A server container type whose name carries a deployed Game Kit's type prefix is kit-owned schema, not a code
// orphan: the diff keeps it OFF the server-only prune list (a warning notes it is left in place). A type matching
// no recognized prefix is still a prune candidate, and without any recognized prefixes the kit types are prunable
// too (guards against the protection silently becoming a no-op).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySchemaSyncProtectsKitTypesTest,
	"CrowdySDK.Studio.SchemaSyncProtectsKitTypes", CrowdySchemaSyncTestFlags)
bool FCrowdySchemaSyncProtectsKitTypesTest::RunTest(const FString& Parameters)
{
	FCrowdyDesiredContainerType Hero = MakeDesiredType(TEXT("Hero"));
	Hero.Props.Add(MakeDesiredProp(TEXT("hp"), TEXT("int"), TEXT("100")));
	TArray<FCrowdyDesiredContainerType> DesiredSchema;
	DesiredSchema.Add(Hero);

	TArray<FStudioContainerType> CurrentTypes;
	CurrentTypes.Add(MakeServerType(TEXT("Hero")));               // code-owned, matches
	CurrentTypes.Add(MakeServerType(TEXT("GoblinCombatant")));    // kit-owned (prefix "Goblin")
	CurrentTypes.Add(MakeServerType(TEXT("GoblinStatusEffect"))); // kit-owned (prefix "Goblin")
	CurrentTypes.Add(MakeServerType(TEXT("Ghost")));             // a true orphan

	TMap<FString, TArray<FStudioPropertyDef>> CurrentProps;
	TArray<FStudioPropertyDef>& HeroProps = CurrentProps.Add(TEXT("Hero"));
	HeroProps.Add(MakeServerProp(TEXT("Hero"), TEXT("hp"), TEXT("int"), TEXT("100")));

	TSet<FString> Prefixes;
	Prefixes.Add(TEXT("Goblin"));

	const FCrowdySchemaDelta Delta = FCrowdySchemaSync::DiffSchema(DesiredSchema, CurrentTypes, CurrentProps, Prefixes);
	if (TestEqual(TEXT("only the non-kit orphan is a prune candidate"), Delta.ServerOnlyTypes.Num(), 1))
	{
		TestEqual(TEXT("Ghost is the only server-only type"), Delta.ServerOnlyTypes[0], FString(TEXT("Ghost")));
	}
	TestFalse(TEXT("a kit type is not offered for prune"), Delta.ServerOnlyTypes.Contains(TEXT("GoblinCombatant")));

	// Without recognized prefixes, all three non-code types are prune candidates.
	const FCrowdySchemaDelta Unprotected = FCrowdySchemaSync::DiffSchema(DesiredSchema, CurrentTypes, CurrentProps);
	TestEqual(TEXT("without protection every non-code type is prunable"), Unprotected.ServerOnlyTypes.Num(), 3);

	return true;
}

// A server function whose name carries a deployed Game Kit's function-name prefix (toSnakeCase(prefix) + "_") is
// kit-owned, so the diff keeps it off the prune list; a function matching no recognized prefix is still a candidate.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySchemaSyncProtectsKitFunctionsTest,
	"CrowdySDK.Studio.SchemaSyncProtectsKitFunctions", CrowdySchemaSyncTestFlags)
bool FCrowdySchemaSyncProtectsKitFunctionsTest::RunTest(const FString& Parameters)
{
	TArray<FCrowdyGameModelFunctionInput> DesiredFns; // nothing authored in code

	FStudioFunction KitAttack;
	KitAttack.Name = TEXT("goblin_attack"); // kit function for prefix "Goblin"
	FStudioFunction KitApply;
	KitApply.Name = TEXT("goblin_apply_effect");
	FStudioFunction Orphan;
	Orphan.Name = TEXT("legacy_fn"); // no kit, no effect owns this

	TArray<FStudioFunction> CurrentFns;
	CurrentFns.Add(KitAttack);
	CurrentFns.Add(KitApply);
	CurrentFns.Add(Orphan);

	TSet<FString> Prefixes;
	Prefixes.Add(TEXT("Goblin"));

	FCrowdySchemaDelta Delta;
	FCrowdySchemaSync::DiffFunctions(DesiredFns, CurrentFns, Delta, TSet<FString>(), Prefixes);
	if (TestEqual(TEXT("only the non-kit orphan is a prune candidate"), Delta.ServerOnlyFunctions.Num(), 1))
	{
		TestEqual(TEXT("legacy_fn is the only server-only function"), Delta.ServerOnlyFunctions[0].Name, FString(TEXT("legacy_fn")));
	}
	TestFalse(TEXT("a kit function is not offered for prune"), Delta.HasServerOnlyFunction(TEXT("goblin_attack")));

	// Without recognized prefixes, every server function is a prune candidate.
	FCrowdySchemaDelta Unprotected;
	FCrowdySchemaSync::DiffFunctions(DesiredFns, CurrentFns, Unprotected);
	TestEqual(TEXT("without protection every server function is prunable"), Unprotected.ServerOnlyFunctions.Num(), 3);

	return true;
}

// A composed kit function can be verb-first (e.g. a lock's 'open_guild_hall'), so its NAME does not carry the kit
// prefix even though its container TYPE ('GuildHall') is kit-owned. Recognizing it by its owning type keeps it off
// the prune list; a same-named function on a non-kit type is still a prune candidate.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySchemaSyncProtectsKitLockFunctionsTest,
	"CrowdySDK.Studio.SchemaSyncProtectsKitLockFunctions", CrowdySchemaSyncTestFlags)
bool FCrowdySchemaSyncProtectsKitLockFunctionsTest::RunTest(const FString& Parameters)
{
	TArray<FCrowdyGameModelFunctionInput> DesiredFns; // nothing authored in code

	// A verb-first access-control function on the kit's GuildHall type: its name has no 'guild_' prefix.
	FStudioFunction OpenHall;
	OpenHall.Name = TEXT("open_guild_hall");
	OpenHall.ContainerTypeName = TEXT("GuildHall");

	// A same-named function on a non-kit type: not kit-owned, so it stays a prune candidate.
	FStudioFunction OpenProfile;
	OpenProfile.Name = TEXT("open_player_profile");
	OpenProfile.ContainerTypeName = TEXT("PlayerProfile");

	TArray<FStudioFunction> CurrentFns;
	CurrentFns.Add(OpenHall);
	CurrentFns.Add(OpenProfile);

	TSet<FString> Prefixes;
	Prefixes.Add(TEXT("Guild"));

	FCrowdySchemaDelta Delta;
	FCrowdySchemaSync::DiffFunctions(DesiredFns, CurrentFns, Delta, TSet<FString>(), Prefixes);

	TestFalse(TEXT("a verb-first function on a kit type is not offered for prune"),
		Delta.HasServerOnlyFunction(TEXT("open_guild_hall")));
	if (TestEqual(TEXT("only the non-kit-type function is a prune candidate"), Delta.ServerOnlyFunctions.Num(), 1))
	{
		TestEqual(TEXT("open_player_profile is the only server-only function"),
			Delta.ServerOnlyFunctions[0].Name, FString(TEXT("open_player_profile")));
		TestEqual(TEXT("the candidate carries the model the diff identified it by"),
			Delta.ServerOnlyFunctions[0].ContainerTypeName, FString(TEXT("PlayerProfile")));
	}

	return true;
}

// A non-kit server type/function is still pruned even when recognized prefixes are present, and the defensive
// prop-level protection keeps a server-only property off the prune list when its OWNING code type also matches a
// kit prefix (a kit generally owns its whole type) while a normal code type's server-only prop is still surfaced.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySchemaSyncNonKitTypeStillPrunedTest,
	"CrowdySDK.Studio.SchemaSyncNonKitTypeStillPruned", CrowdySchemaSyncTestFlags)
bool FCrowdySchemaSyncNonKitTypeStillPrunedTest::RunTest(const FString& Parameters)
{
	FCrowdyDesiredContainerType Hero = MakeDesiredType(TEXT("Hero"));
	Hero.Props.Add(MakeDesiredProp(TEXT("hp"), TEXT("int"), TEXT("100")));
	FCrowdyDesiredContainerType Keep = MakeDesiredType(TEXT("GoblinKeep")); // a code type whose name matches the prefix
	Keep.Props.Add(MakeDesiredProp(TEXT("gold"), TEXT("int"), TEXT("0")));

	TArray<FCrowdyDesiredContainerType> DesiredSchema;
	DesiredSchema.Add(Hero);
	DesiredSchema.Add(Keep);

	TArray<FStudioContainerType> CurrentTypes;
	CurrentTypes.Add(MakeServerType(TEXT("Hero")));
	CurrentTypes.Add(MakeServerType(TEXT("GoblinKeep")));
	CurrentTypes.Add(MakeServerType(TEXT("Dragon")));    // a non-kit orphan
	CurrentTypes.Add(MakeServerType(TEXT("GoblinNest"))); // kit-owned (prefix "Goblin")

	TMap<FString, TArray<FStudioPropertyDef>> CurrentProps;
	TArray<FStudioPropertyDef>& HeroProps = CurrentProps.Add(TEXT("Hero"));
	HeroProps.Add(MakeServerProp(TEXT("Hero"), TEXT("hp"), TEXT("int"), TEXT("100")));
	HeroProps.Add(MakeServerProp(TEXT("Hero"), TEXT("legacy"), TEXT("string"), TEXT("\"x\""))); // server-only -> prunable
	TArray<FStudioPropertyDef>& KeepProps = CurrentProps.Add(TEXT("GoblinKeep"));
	KeepProps.Add(MakeServerProp(TEXT("GoblinKeep"), TEXT("gold"), TEXT("int"), TEXT("0")));
	KeepProps.Add(MakeServerProp(TEXT("GoblinKeep"), TEXT("stale"), TEXT("int"), TEXT("0"))); // protected (kit-prefixed type)

	TSet<FString> Prefixes;
	Prefixes.Add(TEXT("Goblin"));

	const FCrowdySchemaDelta Delta = FCrowdySchemaSync::DiffSchema(DesiredSchema, CurrentTypes, CurrentProps, Prefixes);

	if (TestEqual(TEXT("the non-kit type is still pruned, the kit type is not"), Delta.ServerOnlyTypes.Num(), 1))
	{
		TestEqual(TEXT("Dragon is the only server-only type"), Delta.ServerOnlyTypes[0], FString(TEXT("Dragon")));
	}
	if (TestEqual(TEXT("only the normal type's server-only prop is surfaced"), Delta.ServerOnlyProps.Num(), 1))
	{
		TestEqual(TEXT("the surfaced prop's type"), Delta.ServerOnlyProps[0].ContainerTypeName, FString(TEXT("Hero")));
		TestEqual(TEXT("the surfaced prop's key"), Delta.ServerOnlyProps[0].Key, FString(TEXT("legacy")));
	}

	return true;
}

// The snake-case transform and the recognition predicates: ToKitSnakeCase mirrors the vendored kit's rule, the
// type match is a case-sensitive prefix, the function match uses toSnakeCase(prefix) + "_", and an empty prefix
// never matches (it must not swallow the whole schema).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySchemaSyncSnakeCasePrefixMatchTest,
	"CrowdySDK.Studio.SchemaSyncSnakeCasePrefixMatch", CrowdySchemaSyncTestFlags)
bool FCrowdySchemaSyncSnakeCasePrefixMatchTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("a single word lowercases"), FCrowdySchemaSync::ToKitSnakeCase(TEXT("Goblin")), FString(TEXT("goblin")));
	TestEqual(TEXT("PascalCase splits on the case boundary"),
		FCrowdySchemaSync::ToKitSnakeCase(TEXT("WeeklyBoard")), FString(TEXT("weekly_board")));
	TestEqual(TEXT("the Guild default lowercases"), FCrowdySchemaSync::ToKitSnakeCase(TEXT("Guild")), FString(TEXT("guild")));

	TSet<FString> Prefixes;
	Prefixes.Add(TEXT("Goblin"));

	TestTrue(TEXT("a kit type name matches its prefix"),
		FCrowdySchemaSync::IsRecognizedKitTypeName(TEXT("GoblinCombatant"), Prefixes));
	TestFalse(TEXT("a non-kit type does not match"),
		FCrowdySchemaSync::IsRecognizedKitTypeName(TEXT("Dragon"), Prefixes));
	TestFalse(TEXT("the prefix match is case-sensitive"),
		FCrowdySchemaSync::IsRecognizedKitTypeName(TEXT("goblinCombatant"), Prefixes));

	TestTrue(TEXT("a kit function name matches the snake prefix"),
		FCrowdySchemaSync::IsRecognizedKitFunctionName(TEXT("goblin_attack"), Prefixes));
	TestFalse(TEXT("an unprefixed function does not match"),
		FCrowdySchemaSync::IsRecognizedKitFunctionName(TEXT("attack"), Prefixes));

	// An empty prefix must never match (it would protect the entire server schema from prune).
	TSet<FString> Empty;
	Empty.Add(FString());
	TestFalse(TEXT("an empty prefix matches no type"), FCrowdySchemaSync::IsRecognizedKitTypeName(TEXT("Anything"), Empty));
	TestFalse(TEXT("an empty prefix matches no function"), FCrowdySchemaSync::IsRecognizedKitFunctionName(TEXT("anything"), Empty));

	return true;
}

// The exact-name layer: an empty-prefix kit seeds a bare type name ("Combatant") that no prefix rule can recognize.
// With that name in RecognizedKitTypeNames the diff keeps it off the prune list; a name absent from the set (a real
// hand-authored orphan) is still a prune candidate. This is the gap the prefix layer alone left open.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySchemaSyncProtectsExactKitTypeNamesTest,
	"CrowdySDK.Studio.SchemaSyncProtectsExactKitTypeNames", CrowdySchemaSyncTestFlags)
bool FCrowdySchemaSyncProtectsExactKitTypeNamesTest::RunTest(const FString& Parameters)
{
	FCrowdyDesiredContainerType Hero = MakeDesiredType(TEXT("Hero"));
	Hero.Props.Add(MakeDesiredProp(TEXT("hp"), TEXT("int"), TEXT("100")));
	TArray<FCrowdyDesiredContainerType> DesiredSchema;
	DesiredSchema.Add(Hero);

	TArray<FStudioContainerType> CurrentTypes;
	CurrentTypes.Add(MakeServerType(TEXT("Hero")));          // code-owned, matches
	CurrentTypes.Add(MakeServerType(TEXT("Combatant")));     // empty-prefix kit type, protected by exact name
	CurrentTypes.Add(MakeServerType(TEXT("StatusEffect")));  // empty-prefix kit type, protected by exact name
	CurrentTypes.Add(MakeServerType(TEXT("PlayerProfile"))); // a true hand-authored orphan

	TMap<FString, TArray<FStudioPropertyDef>> CurrentProps;
	TArray<FStudioPropertyDef>& HeroProps = CurrentProps.Add(TEXT("Hero"));
	HeroProps.Add(MakeServerProp(TEXT("Hero"), TEXT("hp"), TEXT("int"), TEXT("100")));

	// No prefixes (the empty-prefix kit contributes none); protection rides the exact type names only.
	TSet<FString> KitTypeNames;
	KitTypeNames.Add(TEXT("Combatant"));
	KitTypeNames.Add(TEXT("StatusEffect"));

	const FCrowdySchemaDelta Delta = FCrowdySchemaSync::DiffSchema(
		DesiredSchema, CurrentTypes, CurrentProps, TSet<FString>(), KitTypeNames);

	if (TestEqual(TEXT("only the hand-authored orphan is a prune candidate"), Delta.ServerOnlyTypes.Num(), 1))
	{
		TestEqual(TEXT("PlayerProfile is the only server-only type"), Delta.ServerOnlyTypes[0], FString(TEXT("PlayerProfile")));
	}
	TestFalse(TEXT("an exact-named kit type is not offered for prune"), Delta.ServerOnlyTypes.Contains(TEXT("Combatant")));

	// Without the exact names, the two kit types plus the orphan are all prunable (guards against a silent no-op).
	const FCrowdySchemaDelta Unprotected = FCrowdySchemaSync::DiffSchema(DesiredSchema, CurrentTypes, CurrentProps);
	TestEqual(TEXT("without exact-name protection every non-code type is prunable"), Unprotected.ServerOnlyTypes.Num(), 3);

	return true;
}

// The exact-name layer for functions: an empty-prefix kit's bare "attack" carries no recognizable prefix. With it in
// RecognizedKitFunctionNames the diff keeps it off the prune list; an unrecognized function is still a prune candidate.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySchemaSyncProtectsExactKitFunctionNamesTest,
	"CrowdySDK.Studio.SchemaSyncProtectsExactKitFunctionNames", CrowdySchemaSyncTestFlags)
bool FCrowdySchemaSyncProtectsExactKitFunctionNamesTest::RunTest(const FString& Parameters)
{
	TArray<FCrowdyGameModelFunctionInput> DesiredFns; // nothing authored in code

	FStudioFunction KitAttack;
	KitAttack.Name = TEXT("attack"); // empty-prefix kit function, protected by exact name
	FStudioFunction KitAdvance;
	KitAdvance.Name = TEXT("advance_time");
	FStudioFunction Orphan;
	Orphan.Name = TEXT("legacy_fn"); // no kit, no effect owns this

	TArray<FStudioFunction> CurrentFns;
	CurrentFns.Add(KitAttack);
	CurrentFns.Add(KitAdvance);
	CurrentFns.Add(Orphan);

	TSet<FString> KitFunctionNames;
	KitFunctionNames.Add(TEXT("attack"));
	KitFunctionNames.Add(TEXT("advance_time"));

	// No prefixes: protection rides the exact function names only.
	FCrowdySchemaDelta Delta;
	FCrowdySchemaSync::DiffFunctions(DesiredFns, CurrentFns, Delta, TSet<FString>(), TSet<FString>(), KitFunctionNames);
	if (TestEqual(TEXT("only the unrecognized function is a prune candidate"), Delta.ServerOnlyFunctions.Num(), 1))
	{
		TestEqual(TEXT("legacy_fn is the only server-only function"), Delta.ServerOnlyFunctions[0].Name, FString(TEXT("legacy_fn")));
	}
	TestFalse(TEXT("an exact-named kit function is not offered for prune"), Delta.HasServerOnlyFunction(TEXT("attack")));

	// Without the exact names, every server function is a prune candidate.
	FCrowdySchemaDelta Unprotected;
	FCrowdySchemaSync::DiffFunctions(DesiredFns, CurrentFns, Unprotected);
	TestEqual(TEXT("without exact-name protection every server function is prunable"), Unprotected.ServerOnlyFunctions.Num(), 3);

	return true;
}

// DetectDuplicateNames is the pure core behind both GatherDesiredFunctions and GatherDesiredAutomations' duplicate
// guard. Two assets claiming one (scope, name) pair is a conflict carrying both asset paths, in input order; a
// third, unrelated name with a single author is not a conflict at all.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDuplicateNameDetectionFindsConflictTest,
	"CrowdySDK.GameModel.DuplicateNameDetectionFindsConflict", CrowdySchemaSyncTestFlags)
bool FCrowdyDuplicateNameDetectionFindsConflictTest::RunTest(const FString& Parameters)
{
	TArray<FCrowdySchemaNameCandidate> Candidates;
	Candidates.Add({ TEXT("/Game/Effects/Heal.Heal"), TEXT("Hero"), TEXT("heal") });
	Candidates.Add({ TEXT("/Game/Effects/TakeDamage.TakeDamage"), TEXT("Hero"), TEXT("take_damage") });
	Candidates.Add({ TEXT("/Game/Effects/Heal_Copy.Heal_Copy"), TEXT("Hero"), TEXT("heal") });

	const TArray<FCrowdySchemaNameConflict> Conflicts = FCrowdySchemaSync::DetectDuplicateNames(Candidates);

	if (TestEqual(TEXT("exactly one conflicting name"), Conflicts.Num(), 1))
	{
		TestEqual(TEXT("the conflicting name is 'heal'"), Conflicts[0].Name, FString(TEXT("heal")));
		TestEqual(TEXT("the conflict carries its container type"), Conflicts[0].Scope, FString(TEXT("Hero")));
		if (TestEqual(TEXT("two authoring assets"), Conflicts[0].AssetPaths.Num(), 2))
		{
			TestEqual(TEXT("first author in input order"), Conflicts[0].AssetPaths[0], FString(TEXT("/Game/Effects/Heal.Heal")));
			TestEqual(TEXT("second author in input order"), Conflicts[0].AssetPaths[1], FString(TEXT("/Game/Effects/Heal_Copy.Heal_Copy")));
		}
	}

	return true;
}

// Every name authored by exactly one asset must never be reported: a schema sync with no duplicated effect names
// must see zero conflicts.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDuplicateNameDetectionUniqueNamesPassThroughTest,
	"CrowdySDK.GameModel.DuplicateNameDetectionUniqueNamesPassThrough", CrowdySchemaSyncTestFlags)
bool FCrowdyDuplicateNameDetectionUniqueNamesPassThroughTest::RunTest(const FString& Parameters)
{
	TArray<FCrowdySchemaNameCandidate> Candidates;
	Candidates.Add({ TEXT("/Game/Effects/Heal.Heal"), TEXT("Hero"), TEXT("heal") });
	Candidates.Add({ TEXT("/Game/Effects/TakeDamage.TakeDamage"), TEXT("Hero"), TEXT("take_damage") });
	Candidates.Add({ TEXT("/Game/Effects/Stun.Stun"), TEXT("Hero"), TEXT("stun") });

	const TArray<FCrowdySchemaNameConflict> Conflicts = FCrowdySchemaSync::DetectDuplicateNames(Candidates);
	TestEqual(TEXT("no conflicts when every name has a single author"), Conflicts.Num(), 0);

	return true;
}

// The server scopes a model function by its container type, so "take_damage" on Hero and "take_damage" on Monster
// are two distinct functions and a per-container naming convention must keep working: no conflict. The same name
// twice on ONE container type is still a conflict. An app-wide name (an automation) passes an empty scope, which
// groups on the name alone and conflicts regardless of which effect's container type it came from.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDuplicateNameDetectionScopedByContainerTypeTest,
	"CrowdySDK.GameModel.DuplicateNameDetectionScopedByContainerType", CrowdySchemaSyncTestFlags)
bool FCrowdyDuplicateNameDetectionScopedByContainerTypeTest::RunTest(const FString& Parameters)
{
	TArray<FCrowdySchemaNameCandidate> AcrossTypes;
	AcrossTypes.Add({ TEXT("/Game/Effects/HeroDamage.HeroDamage"), TEXT("Hero"), TEXT("take_damage") });
	AcrossTypes.Add({ TEXT("/Game/Effects/MonsterDamage.MonsterDamage"), TEXT("Monster"), TEXT("take_damage") });

	TestEqual(TEXT("the same function name on two container types is not a conflict"),
		FCrowdySchemaSync::DetectDuplicateNames(AcrossTypes).Num(), 0);

	TArray<FCrowdySchemaNameCandidate> SameType;
	SameType.Add({ TEXT("/Game/Effects/HeroDamage.HeroDamage"), TEXT("Hero"), TEXT("take_damage") });
	SameType.Add({ TEXT("/Game/Effects/HeroDamage_Copy.HeroDamage_Copy"), TEXT("Hero"), TEXT("take_damage") });

	const TArray<FCrowdySchemaNameConflict> SameTypeConflicts = FCrowdySchemaSync::DetectDuplicateNames(SameType);
	if (TestEqual(TEXT("the same function name twice on one container type is a conflict"), SameTypeConflicts.Num(), 1))
	{
		TestEqual(TEXT("both assets are named"), SameTypeConflicts[0].AssetPaths.Num(), 2);
		TestEqual(TEXT("the conflict is scoped to Hero"), SameTypeConflicts[0].Scope, FString(TEXT("Hero")));
	}

	// An automation name is unique app-wide, so its candidates carry no scope and still collide across types.
	TArray<FCrowdySchemaNameCandidate> AppWide;
	AppWide.Add({ TEXT("/Game/Effects/HeroTick.HeroTick"), FString(), TEXT("tick") });
	AppWide.Add({ TEXT("/Game/Effects/MonsterTick.MonsterTick"), FString(), TEXT("tick") });

	TestEqual(TEXT("an unscoped name collides regardless of the authoring effects' types"),
		FCrowdySchemaSync::DetectDuplicateNames(AppWide).Num(), 1);

	return true;
}

// ScopedNameKey is the shared identity for every scoped-name decision the sync makes: the duplicate grouping,
// DiffFunctions' current-function lookup, and the gathers' exclusion sets. Distinct (scope, name) pairs must
// produce distinct keys, including pairs a separator-less concatenation would collide.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyScopedNameKeyIsUnambiguousTest,
	"CrowdySDK.GameModel.ScopedNameKeyIsUnambiguous", CrowdySchemaSyncTestFlags)
bool FCrowdyScopedNameKeyIsUnambiguousTest::RunTest(const FString& Parameters)
{
	TestNotEqual(TEXT("the same name on two container types keys differently"),
		FCrowdySchemaSync::ScopedNameKey(TEXT("Hero"), TEXT("take_damage")),
		FCrowdySchemaSync::ScopedNameKey(TEXT("Monster"), TEXT("take_damage")));

	TestNotEqual(TEXT("two names on one container type key differently"),
		FCrowdySchemaSync::ScopedNameKey(TEXT("Hero"), TEXT("take_damage")),
		FCrowdySchemaSync::ScopedNameKey(TEXT("Hero"), TEXT("heal")));

	// A boundary that a plain concatenation would collapse onto one key.
	TestNotEqual(TEXT("the scope/name boundary is unambiguous"),
		FCrowdySchemaSync::ScopedNameKey(TEXT("Hero"), TEXT("take_damage")),
		FCrowdySchemaSync::ScopedNameKey(TEXT("Herotake"), TEXT("_damage")));

	TestEqual(TEXT("the same pair keys identically"),
		FCrowdySchemaSync::ScopedNameKey(TEXT("Hero"), TEXT("heal")),
		FCrowdySchemaSync::ScopedNameKey(TEXT("Hero"), TEXT("heal")));

	TestEqual(TEXT("an app-wide name keys on the name alone"),
		FCrowdySchemaSync::ScopedNameKey(FString(), TEXT("tick")),
		FCrowdySchemaSync::ScopedNameKey(FString(), TEXT("tick")));

	return true;
}

// A name authored by three or more assets is one conflict carrying every authoring asset path, not one conflict
// per pair.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDuplicateNameDetectionThreeWayConflictTest,
	"CrowdySDK.GameModel.DuplicateNameDetectionThreeWayConflict", CrowdySchemaSyncTestFlags)
bool FCrowdyDuplicateNameDetectionThreeWayConflictTest::RunTest(const FString& Parameters)
{
	TArray<FCrowdySchemaNameCandidate> Candidates;
	Candidates.Add({ TEXT("/Game/Effects/A.A"), TEXT("Hero"), TEXT("heal") });
	Candidates.Add({ TEXT("/Game/Effects/B.B"), TEXT("Hero"), TEXT("heal") });
	Candidates.Add({ TEXT("/Game/Effects/C.C"), TEXT("Hero"), TEXT("heal") });

	const TArray<FCrowdySchemaNameConflict> Conflicts = FCrowdySchemaSync::DetectDuplicateNames(Candidates);

	if (TestEqual(TEXT("exactly one conflicting name"), Conflicts.Num(), 1))
	{
		TestEqual(TEXT("all three assets recorded as authors"), Conflicts[0].AssetPaths.Num(), 3);
	}

	return true;
}

// DiffFunctions identifies a server function by (container type, name), so two server functions sharing a name on
// different container types are compared against the right desired function. Keyed on the name alone, one of the
// two overwrites the other in the lookup and an unchanged effect reads as a container rebind (a spurious upsert
// that would move the server function onto the wrong type).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyFunctionDiffScopedByContainerTypeTest,
	"CrowdySDK.GameModel.FunctionDiffScopedByContainerType", CrowdySchemaSyncTestFlags)
bool FCrowdyFunctionDiffScopedByContainerTypeTest::RunTest(const FString& Parameters)
{
	FCrowdyGameModelFunctionInput MonsterDamage = MakeDesiredFunction();
	MonsterDamage.ContainerTypeName = TEXT("Monster");

	FCrowdyGameModelFunctionInput HeroDamage = MakeDesiredFunction(); // stays on Hero

	// The server holds BOTH, Monster first, so a name-keyed lookup would keep the Hero one for both desired entries.
	TArray<FStudioFunction> CurrentFns;
	CurrentFns.Add(MakeServerFunctionFrom(MonsterDamage));
	CurrentFns.Add(MakeServerFunctionFrom(HeroDamage));

	TArray<FCrowdyGameModelFunctionInput> DesiredFns;
	DesiredFns.Add(MonsterDamage);
	DesiredFns.Add(HeroDamage);

	FCrowdySchemaDelta NoOp;
	FCrowdySchemaSync::DiffFunctions(DesiredFns, CurrentFns, NoOp);

	TestEqual(TEXT("two in-sync same-named functions on different types are not a change"), NoOp.FunctionUpserts.Num(), 0);
	TestEqual(TEXT("neither is offered for prune"), NoOp.ServerOnlyFunctions.Num(), 0);
	TestEqual(TEXT("no rebind warning"), NoOp.Warnings.Num(), 0);

	// A genuine change on ONE of them is still an update, and only that one.
	TArray<FStudioFunction> DriftedFns;
	FStudioFunction DriftedMonster = MakeServerFunctionFrom(MonsterDamage);
	DriftedMonster.Mutations[0].Expression = TEXT("self.health - $amount");
	DriftedFns.Add(DriftedMonster);
	DriftedFns.Add(MakeServerFunctionFrom(HeroDamage));

	FCrowdySchemaDelta Delta;
	FCrowdySchemaSync::DiffFunctions(DesiredFns, DriftedFns, Delta);

	if (TestEqual(TEXT("only the drifted one is an update"), Delta.FunctionUpserts.Num(), 1))
	{
		TestEqual(TEXT("the Monster function is the one upserted"),
			Delta.FunctionUpserts[0].Function.ContainerTypeName, FString(TEXT("Monster")));
		TestFalse(TEXT("it is an update, not a create"), Delta.FunctionUpserts[0].bIsNew);
	}

	return true;
}

// A function whose container type genuinely changed still matches the single server function of that name, so a
// rebind is an update (with its disclosure warning) rather than a create that strands the old definition.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyFunctionDiffRebindIsAnUpdateTest,
	"CrowdySDK.GameModel.FunctionDiffRebindIsAnUpdate", CrowdySchemaSyncTestFlags)
bool FCrowdyFunctionDiffRebindIsAnUpdateTest::RunTest(const FString& Parameters)
{
	const FCrowdyGameModelFunctionInput OnHero = MakeDesiredFunction(); // server still has it on Hero

	FCrowdyGameModelFunctionInput Rebound = OnHero;
	Rebound.ContainerTypeName = TEXT("Monster");

	TArray<FCrowdyGameModelFunctionInput> DesiredFns;
	DesiredFns.Add(Rebound);
	TArray<FStudioFunction> CurrentFns;
	CurrentFns.Add(MakeServerFunctionFrom(OnHero));

	FCrowdySchemaDelta Delta;
	FCrowdySchemaSync::DiffFunctions(DesiredFns, CurrentFns, Delta);

	if (TestEqual(TEXT("the rebind is one upsert"), Delta.FunctionUpserts.Num(), 1))
	{
		TestFalse(TEXT("an update, not a create"), Delta.FunctionUpserts[0].bIsNew);
	}
	TestEqual(TEXT("the old definition is not stranded as a prune candidate"), Delta.ServerOnlyFunctions.Num(), 0);
	const bool bDisclosed = Delta.Warnings.ContainsByPredicate([](const FString& W)
	{
		return W.Contains(TEXT("rebinds container type"));
	});
	TestTrue(TEXT("the rebind is disclosed"), bDisclosed);

	return true;
}

// MarkDuplicateConflictWarning + SplitDuplicateConflictWarnings round-trip: a marked warning is promoted into
// StatusNote (naming the conflict) and its marker is stripped before it reaches the plain warning list; an
// ordinary (unmarked) warning is left completely untouched.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDuplicateConflictWarningPromotedToStatusNoteTest,
	"CrowdySDK.GameModel.DuplicateConflictWarningPromotedToStatusNote", CrowdySchemaSyncTestFlags)
bool FCrowdyDuplicateConflictWarningPromotedToStatusNoteTest::RunTest(const FString& Parameters)
{
	const FString ConflictMessage = TEXT("Function name 'heal' is authored by 2 effects: A, B.");

	TArray<FString> InWarnings;
	InWarnings.Add(TEXT("an ordinary advisory warning"));
	InWarnings.Add(FCrowdySchemaSync::MarkDuplicateConflictWarning(ConflictMessage));

	TArray<FString> PlainWarnings;
	FString StatusNote;
	FCrowdySchemaSync::SplitDuplicateConflictWarnings(InWarnings, PlainWarnings, StatusNote);

	TestTrue(TEXT("the ordinary warning survives untouched"), PlainWarnings.Contains(TEXT("an ordinary advisory warning")));
	TestTrue(TEXT("the conflict message (marker stripped) is still visible in the warning list"),
		PlainWarnings.Contains(ConflictMessage));
	TestFalse(TEXT("StatusNote is non-empty when a conflict was marked"), StatusNote.IsEmpty());
	TestTrue(TEXT("StatusNote names the conflict"), StatusNote.Contains(ConflictMessage));

	return true;
}

// No marked warning must leave StatusNote empty and the warning list byte-for-byte unchanged: an ordinary plan
// (no duplicate names) must never show the "plan blocked" banner.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNoDuplicateConflictLeavesStatusNoteEmptyTest,
	"CrowdySDK.GameModel.NoDuplicateConflictLeavesStatusNoteEmpty", CrowdySchemaSyncTestFlags)
bool FCrowdyNoDuplicateConflictLeavesStatusNoteEmptyTest::RunTest(const FString& Parameters)
{
	TArray<FString> InWarnings;
	InWarnings.Add(TEXT("a plain warning with nothing to do with duplicates"));

	TArray<FString> PlainWarnings;
	FString StatusNote;
	FCrowdySchemaSync::SplitDuplicateConflictWarnings(InWarnings, PlainWarnings, StatusNote);

	TestTrue(TEXT("StatusNote stays empty with no conflict"), StatusNote.IsEmpty());
	if (TestEqual(TEXT("the warning list is unchanged"), PlainWarnings.Num(), 1))
	{
		TestEqual(TEXT("same single warning"), PlainWarnings[0], FString(TEXT("a plain warning with nothing to do with duplicates")));
	}

	return true;
}

// BuildReport is the sole caller of SplitDuplicateConflictWarnings: a duplicate-name conflict reported through
// GatherDesiredFunctions' warning channel must reach the report's durable StatusNote banner, not just its
// ordinary Warnings list, so the Studio panel shows it above the counts.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyBuildReportPromotesDuplicateConflictTest,
	"CrowdySDK.GameModel.BuildReportPromotesDuplicateConflictToStatusNote", CrowdySchemaSyncTestFlags)
bool FCrowdyBuildReportPromotesDuplicateConflictTest::RunTest(const FString& Parameters)
{
	const FString ConflictMessage =
		TEXT("Function name 'heal' is authored by 2 effects: /Game/A.A, /Game/B.B. Give each a unique function name.");

	TArray<FString> ExtraWarnings;
	ExtraWarnings.Add(FCrowdySchemaSync::MarkDuplicateConflictWarning(ConflictMessage));

	const FCrowdySchemaSyncReport Report = FCrowdySchemaSync::BuildReport(FCrowdySchemaDelta(), ExtraWarnings, /*bApplied*/ false);

	TestFalse(TEXT("StatusNote carries the conflict banner"), Report.StatusNote.IsEmpty());
	TestTrue(TEXT("StatusNote names the conflicting function"), Report.StatusNote.Contains(ConflictMessage));
	TestTrue(TEXT("the stripped message is still visible in Warnings"), Report.Warnings.Contains(ConflictMessage));

	return true;
}

namespace
{
	// One effect's compile facts as the sweep would have produced them, for the pure selection rules below. Named
	// apart from the other fixtures in this file (and from the plan-cache tests' own) because adaptive unity merges
	// this module's translation units and two anonymous-namespace helpers sharing a name redefine each other.
	FCrowdyEffectPlanRecord MakeAuthorshipRecord(
		const FString& AssetPath, const FString& TypeName, const FString& FunctionName)
	{
		FCrowdyEffectPlanRecord Record;
		Record.AssetPath = AssetPath;
		Record.TargetTypeName = TypeName;
		Record.EffectiveFunctionName = FunctionName;
		Record.Function.Name = FunctionName;
		Record.Function.ContainerTypeName = TypeName;
		Record.Function.InvokeScope = TEXT("player");
		// One write, because a function that changes nothing is refused by the selection: a fixture standing in for a
		// compiled effect has to carry something the server would actually do.
		Record.Function.Mutations.Add({ TEXT("self"), TEXT("hp"), TEXT("(self.hp - 1)") });
		return Record;
	}
}

// An effect that compiled and was accepted into the plan is attributed as an author, not as a skip: this is the entry
// that answers "which asset declares this function" for a function that really is on the server.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySchemaAuthorshipCompiledEffectIsNotSkippedTest,
	"CrowdySDK.CrowdyStudio.SchemaAuthorshipCompiledEffectIsNotSkipped", CrowdySchemaSyncTestFlags)
bool FCrowdySchemaAuthorshipCompiledEffectIsNotSkippedTest::RunTest(const FString& Parameters)
{
	const TArray<FCrowdyEffectPlanRecord> Records =
		{ MakeAuthorshipRecord(TEXT("/Game/FX/Hit.Hit"), TEXT("Combatant"), TEXT("take_damage")) };

	TArray<FString> Warnings;
	TSet<FString> RecognizedNames;
	TArray<FCrowdySchemaAuthorship> Authorship;
	const TArray<FCrowdyGameModelFunctionInput> Functions =
		FCrowdySchemaSync::SelectDesiredFunctions(Records, Warnings, RecognizedNames, Authorship);

	TestEqual(TEXT("the function is planned"), Functions.Num(), 1);
	if (TestEqual(TEXT("one authorship entry"), Authorship.Num(), 1))
	{
		TestFalse(TEXT("an accepted effect is not skipped"), Authorship[0].bSkipped);
		TestFalse(TEXT("an accepted effect is not conflicted"), Authorship[0].bConflicted);
		TestEqual(TEXT("scoped by its container type"), Authorship[0].Scope, FString(TEXT("Combatant")));
		TestEqual(TEXT("named by its function"), Authorship[0].Name, FString(TEXT("take_damage")));
		TestEqual(TEXT("attributed to its asset"), Authorship[0].AssetPath, FString(TEXT("/Game/FX/Hit.Hit")));
	}
	return true;
}

// An effect whose only job is a require gate plus a timer writes nothing, and it must still sync: refusing it would
// break the shape a delayed completion is authored in. This is the carve-out that stops the empty-function guard from
// being written as "no mutations", which is the obvious and wrong way to express it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySchemaEmptyFunctionGuardKeepsTimerOnlyEffectTest,
	"CrowdySDK.CrowdyStudio.SchemaEmptyFunctionGuardKeepsTimerOnlyEffect", CrowdySchemaSyncTestFlags)
bool FCrowdySchemaEmptyFunctionGuardKeepsTimerOnlyEffectTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectPlanRecord TimerOnly =
		MakeAuthorshipRecord(TEXT("/Game/FX/BeginCapture.BeginCapture"), TEXT("Camp"), TEXT("begin_capture"));
	TimerOnly.Function.Mutations.Reset();
	FCrowdyGameModelTimer Timer;
	Timer.FunctionName = TEXT("complete_capture");
	Timer.DelayMsExpression = TEXT("30000");
	TimerOnly.Function.Timers.Add(Timer);

	TestFalse(TEXT("a function that arms a timer is not empty"),
		FCrowdySchemaSync::FunctionDoesNothing(TimerOnly.Function));

	FCrowdyEffectPlanRecord Returns =
		MakeAuthorshipRecord(TEXT("/Game/FX/ReadHp.ReadHp"), TEXT("Camp"), TEXT("read_hp"));
	Returns.Function.Mutations.Reset();
	Returns.Function.ReturnExpression = TEXT("self.hp");
	TestFalse(TEXT("a function that only answers with a value is not empty"),
		FCrowdySchemaSync::FunctionDoesNothing(Returns.Function));

	FCrowdyEffectPlanRecord Nothing =
		MakeAuthorshipRecord(TEXT("/Game/FX/Empty.Empty"), TEXT("Camp"), TEXT("does_nothing"));
	Nothing.Function.Mutations.Reset();
	// A parameter and an invoke policy change no state on their own, so neither rescues an otherwise empty function.
	FCrowdyGameModelFunctionParam Param;
	Param.Name = TEXT("amount");
	Param.ValueType = TEXT("int");
	Nothing.Function.Parameters.Add(MoveTemp(Param));
	Nothing.Function.InvokePolicyJson = TEXT("{\"type\":\"owner_of_self\"}");
	TestTrue(TEXT("a function with only params and a policy is empty"),
		FCrowdySchemaSync::FunctionDoesNothing(Nothing.Function));

	TArray<FString> Warnings;
	TSet<FString> RecognizedNames;
	TArray<FCrowdySchemaAuthorship> Authorship;
	const TArray<FCrowdyGameModelFunctionInput> Functions = FCrowdySchemaSync::SelectDesiredFunctions(
		{ TimerOnly, Returns, Nothing }, Warnings, RecognizedNames, Authorship);

	if (TestEqual(TEXT("both effects that do something are planned"), Functions.Num(), 2))
	{
		TestEqual(TEXT("the timer-only effect is one of them"), Functions[0].Name, FString(TEXT("begin_capture")));
		TestEqual(TEXT("the query effect is the other"), Functions[1].Name, FString(TEXT("read_hp")));
	}
	return true;
}

// A non-compiling effect still has to be attributed, and attributed to a MODEL: without the scope, a reader is told a
// name was skipped but not which model it belongs to, which is exactly the case where that matters most.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySchemaAuthorshipBrokenEffectKeepsItsScopeTest,
	"CrowdySDK.CrowdyStudio.SchemaAuthorshipBrokenEffectKeepsItsScope", CrowdySchemaSyncTestFlags)
bool FCrowdySchemaAuthorshipBrokenEffectKeepsItsScopeTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectPlanRecord Broken =
		MakeAuthorshipRecord(TEXT("/Game/FX/Broken.Broken"), TEXT("Combatant"), TEXT("take_damage"));
	Broken.bCompileFailed = true;
	Broken.FirstCompileError = TEXT("line 2: unknown attribute 'helth'");
	// A failed compile produces no function at all, which is what the selection has to fall back from.
	Broken.Function = FCrowdyGameModelFunctionInput();

	// An effect that compiles cleanly to a function with no writes, no timers, no notifications and no return value.
	// Sending it would replace the server's function of the same name with an empty body, so it is skipped for a
	// different reason than the broken one and still has to be attributed.
	FCrowdyEffectPlanRecord Blank =
		MakeAuthorshipRecord(TEXT("/Game/FX/Empty.Empty"), TEXT("Combatant"), TEXT("old_hit"));
	Blank.Function.Mutations.Reset();

	TArray<FString> Warnings;
	TSet<FString> RecognizedNames;
	TArray<FCrowdySchemaAuthorship> Authorship;
	const TArray<FCrowdyGameModelFunctionInput> Functions =
		FCrowdySchemaSync::SelectDesiredFunctions({ Broken, Blank }, Warnings, RecognizedNames, Authorship);

	TestEqual(TEXT("neither broken effect is planned"), Functions.Num(), 0);
	if (TestEqual(TEXT("both are still attributed"), Authorship.Num(), 2))
	{
		TestTrue(TEXT("a non-compiling effect is marked skipped"), Authorship[0].bSkipped);
		TestEqual(TEXT("a non-compiling effect keeps its container type as its scope"),
			Authorship[0].Scope, FString(TEXT("Combatant")));
		TestEqual(TEXT("a non-compiling effect keeps its effective function name"),
			Authorship[0].Name, FString(TEXT("take_damage")));

		TestTrue(TEXT("an effect that compiles to nothing is marked skipped"), Authorship[1].bSkipped);
		TestEqual(TEXT("an effect that compiles to nothing keeps its container type as its scope"),
			Authorship[1].Scope, FString(TEXT("Combatant")));
	}
	return true;
}

// A pair claimed twice is excluded for BOTH authors, so both entries have to say so. Marking only the second one seen
// would read as "this asset lost to that one", which is not what happens: neither is synced.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySchemaAuthorshipDuplicatePairMarksBothAuthorsTest,
	"CrowdySDK.CrowdyStudio.SchemaAuthorshipDuplicatePairMarksBothAuthors", CrowdySchemaSyncTestFlags)
bool FCrowdySchemaAuthorshipDuplicatePairMarksBothAuthorsTest::RunTest(const FString& Parameters)
{
	const TArray<FCrowdyEffectPlanRecord> Records = {
		MakeAuthorshipRecord(TEXT("/Game/FX/HitA.HitA"), TEXT("Combatant"), TEXT("take_damage")),
		MakeAuthorshipRecord(TEXT("/Game/FX/HitB.HitB"), TEXT("Combatant"), TEXT("take_damage")),
		// The same name on a different container type is two distinct server functions, so it is not a conflict.
		MakeAuthorshipRecord(TEXT("/Game/FX/HitC.HitC"), TEXT("Structure"), TEXT("take_damage"))
	};

	TArray<FString> Warnings;
	TSet<FString> RecognizedNames;
	TArray<FCrowdySchemaAuthorship> Authorship;
	const TArray<FCrowdyGameModelFunctionInput> Functions =
		FCrowdySchemaSync::SelectDesiredFunctions(Records, Warnings, RecognizedNames, Authorship);

	TestEqual(TEXT("only the unconflicted function is planned"), Functions.Num(), 1);
	if (TestEqual(TEXT("every author is attributed"), Authorship.Num(), 3))
	{
		TestTrue(TEXT("the first author is marked conflicted"), Authorship[0].bConflicted);
		TestTrue(TEXT("the second author is marked conflicted too"), Authorship[1].bConflicted);
		TestTrue(TEXT("a conflicted author is also skipped"), Authorship[0].bSkipped);
		TestTrue(TEXT("a conflicted author is also skipped"), Authorship[1].bSkipped);
		TestFalse(TEXT("the same name on another container type is not conflicted"), Authorship[2].bConflicted);
		TestFalse(TEXT("the same name on another container type is not skipped"), Authorship[2].bSkipped);
	}
	return true;
}

// An automation name is unique app-wide, so its authorship carries an EMPTY scope, and a duplicate name is likewise
// excluded for every author rather than for whichever asset the sweep reached second.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySchemaAuthorshipDuplicateAutomationMarksBothAuthorsTest,
	"CrowdySDK.CrowdyStudio.SchemaAuthorshipDuplicateAutomationMarksBothAuthors", CrowdySchemaSyncTestFlags)
bool FCrowdySchemaAuthorshipDuplicateAutomationMarksBothAuthorsTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectPlanRecord First =
		MakeAuthorshipRecord(TEXT("/Game/FX/TickA.TickA"), TEXT("Combatant"), TEXT("regen_a"));
	First.bHasAutomation = true;
	First.Automation.Name = TEXT("regen");
	First.Automation.FunctionName = TEXT("regen_a");
	First.Automation.ActionKind = TEXT("model_function");

	FCrowdyEffectPlanRecord Second =
		MakeAuthorshipRecord(TEXT("/Game/FX/TickB.TickB"), TEXT("Combatant"), TEXT("regen_b"));
	Second.bHasAutomation = true;
	Second.Automation.Name = TEXT("regen");
	Second.Automation.FunctionName = TEXT("regen_b");
	Second.Automation.ActionKind = TEXT("model_function");

	// A plain effect that does not run automatically: it authors no automation, so it must not appear on this side
	// at all. Inventing an entry for it would put an automation on screen that does not exist.
	const FCrowdyEffectPlanRecord Plain =
		MakeAuthorshipRecord(TEXT("/Game/FX/Hit.Hit"), TEXT("Combatant"), TEXT("take_damage"));

	TArray<FString> Warnings;
	TSet<FString> RecognizedNames;
	TArray<FCrowdyGameModelAutomationTriggerInput> Triggers;
	TArray<FCrowdySchemaAuthorship> Authorship;
	const TArray<FCrowdyGameModelAutomationInput> Automations =
		FCrowdySchemaSync::SelectDesiredAutomations(
			{ First, Second, Plain }, Warnings, RecognizedNames, Triggers, Authorship);

	TestEqual(TEXT("no conflicting automation is planned"), Automations.Num(), 0);
	if (TestEqual(TEXT("only the two automation authors are attributed"), Authorship.Num(), 2))
	{
		TestTrue(TEXT("the first author is marked conflicted"), Authorship[0].bConflicted);
		TestTrue(TEXT("the second author is marked conflicted too"), Authorship[1].bConflicted);
		TestTrue(TEXT("an automation authorship scope is empty (the name is app-wide)"),
			Authorship[0].Scope.IsEmpty() && Authorship[1].Scope.IsEmpty());
		TestEqual(TEXT("named by the automation"), Authorship[0].Name, FString(TEXT("regen")));
	}
	return true;
}

// A designer effect claiming the SDK's reserved collection-touch prefix is excluded from the plan, and the entry has
// to say so: it is the only thing that explains why a function the asset clearly declares is nowhere on the server.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySchemaAuthorshipReservedNameIsSkippedTest,
	"CrowdySDK.CrowdyStudio.SchemaAuthorshipReservedNameIsSkipped", CrowdySchemaSyncTestFlags)
bool FCrowdySchemaAuthorshipReservedNameIsSkippedTest::RunTest(const FString& Parameters)
{
	const FString ReservedName =
		FString(CrowdyGameModelMetaKeys::CollectionTouchFunctionPrefix) + TEXT("combatant");

	TArray<FString> Warnings;
	TSet<FString> RecognizedNames;
	TArray<FCrowdySchemaAuthorship> Authorship;
	const TArray<FCrowdyGameModelFunctionInput> Functions = FCrowdySchemaSync::SelectDesiredFunctions(
		{ MakeAuthorshipRecord(TEXT("/Game/FX/Sneaky.Sneaky"), TEXT("Combatant"), ReservedName) },
		Warnings, RecognizedNames, Authorship);

	TestEqual(TEXT("a reserved-name effect is not planned"), Functions.Num(), 0);
	if (TestEqual(TEXT("it is still attributed"), Authorship.Num(), 1))
	{
		TestTrue(TEXT("a reserved-name effect is marked skipped"), Authorship[0].bSkipped);
		TestEqual(TEXT("it keeps its container type as its scope"),
			Authorship[0].Scope, FString(TEXT("Combatant")));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
