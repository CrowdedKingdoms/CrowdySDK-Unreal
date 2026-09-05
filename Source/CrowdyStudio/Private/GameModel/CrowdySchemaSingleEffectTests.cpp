// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "GameModel/CrowdySchemaSync.h"
#include "Model/CrowdyStudioTypes.h"
#include "CrowdyStudioSyncService.h" // ShouldReportDriftForPendingChannel, ShouldCacheStatusResult

// Helper names are uniquely prefixed (Se*) so they never collide with the sibling schema-sync test file's same-purpose
// builders under a unity build.
namespace
{
	constexpr EAutomationTestFlags SeTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	FCrowdyDesiredPropertyDef SeMakeDesiredProp(const FString& Key, const FString& ValueType, const FString& DefaultJson)
	{
		FCrowdyDesiredPropertyDef Prop;
		Prop.Key = Key;
		Prop.ValueType = ValueType;
		Prop.DefaultValueJson = DefaultJson;
		return Prop;
	}

	// A desired "Hero" container type with one int attribute, matching SeMakeServerType/Prop below.
	FCrowdyDesiredContainerType SeMakeDesiredHeroType()
	{
		FCrowdyDesiredContainerType Type;
		Type.TypeName = TEXT("Hero");
		Type.DisplayName = TEXT("Hero");
		Type.Props.Add(SeMakeDesiredProp(TEXT("health"), TEXT("int"), TEXT("75")));
		return Type;
	}

	FStudioContainerType SeMakeServerType(const FString& TypeName)
	{
		FStudioContainerType Type;
		Type.TypeName = TypeName;
		Type.DisplayName = TypeName;
		Type.InstantiableBy = TEXT("member");
		Type.DefaultPropertyVisibility = TEXT("public");
		return Type;
	}

	FStudioPropertyDef SeMakeServerProp(const FString& TypeName, const FString& Key, const FString& ValueType,
		const FString& DefaultJson)
	{
		FStudioPropertyDef Prop;
		Prop.ContainerTypeName = TypeName;
		Prop.Key = Key;
		Prop.ValueType = ValueType;
		Prop.DefaultValueJson = DefaultJson;
		Prop.Visibility = TEXT("public");
		Prop.Writable = TEXT("function");
		return Prop;
	}

	// The compiled-effect function shape FCrowdyEffectLowering produces for a simple self-write.
	FCrowdyGameModelFunctionInput SeMakeDesiredFunction()
	{
		FCrowdyGameModelFunctionInput Fn;
		Fn.Name = TEXT("take_damage");
		Fn.ContainerTypeName = TEXT("Hero");
		Fn.Description = TEXT("Applies damage to self.");
		Fn.InvokeScope = TEXT("player");
		Fn.InvokePolicyJson = TEXT("{\"type\":\"owner_of_self\"}");

		FCrowdyGameModelFunctionParam Param;
		Param.Name = TEXT("amount");
		Param.ValueType = TEXT("int");
		Param.bRequired = false;
		Param.DefaultValueJson = TEXT("10");
		Param.SortOrder = 0;
		Fn.Parameters.Add(Param);

		FCrowdyGameModelMutation Mutation;
		Mutation.Target = TEXT("self");
		Mutation.Property = TEXT("health");
		Mutation.Expression = TEXT("max(0, min(100, self.health - $amount))");
		Fn.Mutations.Add(Mutation);
		return Fn;
	}

	// A server function that matches a desired input field-for-field (the idempotency baseline).
	FStudioFunction SeMakeServerFunctionFrom(const FCrowdyGameModelFunctionInput& D)
	{
		FStudioFunction S;
		S.Name = D.Name;
		S.ContainerTypeName = D.ContainerTypeName;
		S.Description = D.Description;
		S.ReturnType = D.ReturnType;
		S.ReturnExpression = D.ReturnExpression;
		S.InvokeScope = D.InvokeScope;
		S.bAutonomousInvocable = D.bAutonomousInvocable;
		S.InvokePolicyJson = D.InvokePolicyJson;
		for (const FCrowdyGameModelFunctionParam& P : D.Parameters)
		{
			FStudioFunctionParam SP;
			SP.Name = P.Name;
			SP.ValueType = P.ValueType;
			SP.bRequired = P.bRequired;
			SP.DefaultValueJson = P.DefaultValueJson;
			SP.Description = P.Description;
			SP.SortOrder = P.SortOrder;
			S.Parameters.Add(SP);
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

	// The server snapshot for a Hero that already matches the desired type + function, plus UNRELATED server schema
	// (a Villain type and a cast_spell function) that a per-asset plan must ignore entirely.
	void SeMakeInSyncServerSnapshot(TArray<FStudioContainerType>& OutTypes,
		TMap<FString, TArray<FStudioPropertyDef>>& OutProps, TArray<FStudioFunction>& OutFunctions)
	{
		OutTypes.Add(SeMakeServerType(TEXT("Hero")));
		OutTypes.Add(SeMakeServerType(TEXT("Villain"))); // unrelated: never a prune candidate for a single-effect sync
		OutProps.Add(TEXT("Hero"), { SeMakeServerProp(TEXT("Hero"), TEXT("health"), TEXT("int"), TEXT("75")) });
		OutFunctions.Add(SeMakeServerFunctionFrom(SeMakeDesiredFunction()));

		FStudioFunction Unrelated;
		Unrelated.Name = TEXT("cast_spell");
		Unrelated.ContainerTypeName = TEXT("Villain");
		OutFunctions.Add(Unrelated);
	}
}

// A single-effect plan touches ONLY the effect's own function + container type: unrelated server types/functions are
// never upserted and never surface as prune candidates, and the server-only lists are always empty.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySingleEffectPlanScopesToOneFunctionTest,
	"CrowdySDK.GameModel.SingleEffectPlanScopesToOneFunction", SeTestFlags)
bool FCrowdySingleEffectPlanScopesToOneFunctionTest::RunTest(const FString& Parameters)
{
	TArray<FStudioContainerType> Types;
	TMap<FString, TArray<FStudioPropertyDef>> Props;
	TArray<FStudioFunction> Functions;
	SeMakeInSyncServerSnapshot(Types, Props, Functions);

	FCrowdySchemaDelta Delta;
	const FCrowdySchemaSyncReport Report = FCrowdySchemaSync::PlanForSingleEffect(
		SeMakeDesiredHeroType(), SeMakeDesiredFunction(), Types, Props, Functions, TArray<FString>(), Delta);

	TestTrue(TEXT("an in-sync effect plans no changes"), Delta.IsEmpty());
	TestEqual(TEXT("no upserts"), Report.UpsertCount(), 0);
	TestEqual(TEXT("no server-only types (the unrelated Villain type is never pruned)"), Delta.ServerOnlyTypes.Num(), 0);
	TestEqual(TEXT("no server-only functions (the unrelated cast_spell is never pruned)"), Delta.ServerOnlyFunctions.Num(), 0);
	TestEqual(TEXT("no server-only props"), Delta.ServerOnlyProps.Num(), 0);
	TestEqual(TEXT("the report surfaces no prune candidates"), Report.ServerOnlyCount(), 0);
	return true;
}

// A server function that differs from the compiled effect is planned as one update, with no prune candidates.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySingleEffectPlanDriftedTest,
	"CrowdySDK.GameModel.SingleEffectPlanDrifted", SeTestFlags)
bool FCrowdySingleEffectPlanDriftedTest::RunTest(const FString& Parameters)
{
	TArray<FStudioContainerType> Types;
	Types.Add(SeMakeServerType(TEXT("Hero")));
	TMap<FString, TArray<FStudioPropertyDef>> Props;
	Props.Add(TEXT("Hero"), { SeMakeServerProp(TEXT("Hero"), TEXT("health"), TEXT("int"), TEXT("75")) });

	FStudioFunction Drifted = SeMakeServerFunctionFrom(SeMakeDesiredFunction());
	Drifted.Description = TEXT("stale description"); // the only drift, so the type/props stay in sync

	TArray<FStudioFunction> Functions;
	Functions.Add(Drifted);

	FCrowdySchemaDelta Delta;
	const FCrowdySchemaSyncReport Report = FCrowdySchemaSync::PlanForSingleEffect(
		SeMakeDesiredHeroType(), SeMakeDesiredFunction(), Types, Props, Functions, TArray<FString>(), Delta);

	TestFalse(TEXT("a drifted effect plans a change"), Delta.IsEmpty());
	TestEqual(TEXT("one function update"), Report.FunctionsToUpdate, 1);
	TestEqual(TEXT("no function create"), Report.FunctionsToCreate, 0);
	TestEqual(TEXT("no prune candidates"), Report.ServerOnlyCount(), 0);
	return true;
}

// A server that already matches the compiled effect field-for-field plans zero upserts (idempotency).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySingleEffectPlanInSyncTest,
	"CrowdySDK.GameModel.SingleEffectPlanInSync", SeTestFlags)
bool FCrowdySingleEffectPlanInSyncTest::RunTest(const FString& Parameters)
{
	TArray<FStudioContainerType> Types;
	Types.Add(SeMakeServerType(TEXT("Hero")));
	TMap<FString, TArray<FStudioPropertyDef>> Props;
	Props.Add(TEXT("Hero"), { SeMakeServerProp(TEXT("Hero"), TEXT("health"), TEXT("int"), TEXT("75")) });
	TArray<FStudioFunction> Functions;
	Functions.Add(SeMakeServerFunctionFrom(SeMakeDesiredFunction()));

	FCrowdySchemaDelta Delta;
	const FCrowdySchemaSyncReport Report = FCrowdySchemaSync::PlanForSingleEffect(
		SeMakeDesiredHeroType(), SeMakeDesiredFunction(), Types, Props, Functions, TArray<FString>(), Delta);

	TestTrue(TEXT("plan is empty"), Delta.IsEmpty());
	TestEqual(TEXT("zero upserts"), Report.UpsertCount(), 0);
	return true;
}

// An effect whose function is absent server-side plans a create for the type, its props, and the function.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySingleEffectPlanServerMissingTest,
	"CrowdySDK.GameModel.SingleEffectPlanServerMissing", SeTestFlags)
bool FCrowdySingleEffectPlanServerMissingTest::RunTest(const FString& Parameters)
{
	const TArray<FStudioContainerType> Types;                 // nothing on the server yet
	const TMap<FString, TArray<FStudioPropertyDef>> Props;
	const TArray<FStudioFunction> Functions;

	FCrowdySchemaDelta Delta;
	const FCrowdySchemaSyncReport Report = FCrowdySchemaSync::PlanForSingleEffect(
		SeMakeDesiredHeroType(), SeMakeDesiredFunction(), Types, Props, Functions, TArray<FString>(), Delta);

	TestFalse(TEXT("a missing effect plans changes"), Delta.IsEmpty());
	TestEqual(TEXT("one container type create"), Report.TypesToCreate, 1);
	TestEqual(TEXT("one property create"), Report.PropsToCreate, 1);
	TestEqual(TEXT("one function create"), Report.FunctionsToCreate, 1);
	TestEqual(TEXT("no prune candidates"), Report.ServerOnlyCount(), 0);
	return true;
}

// A broken / unmigrated effect (the caller passes an empty function + type it could not compile) plans nothing and
// never surfaces a prune candidate, so it is guarded rather than synced.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySingleEffectPlanUnmigratedGuardTest,
	"CrowdySDK.GameModel.SingleEffectPlanUnmigratedGuard", SeTestFlags)
bool FCrowdySingleEffectPlanUnmigratedGuardTest::RunTest(const FString& Parameters)
{
	// A real server with unrelated schema present, to prove an unsyncable effect never touches (or prunes) it.
	TArray<FStudioContainerType> Types;
	TMap<FString, TArray<FStudioPropertyDef>> Props;
	TArray<FStudioFunction> Functions;
	SeMakeInSyncServerSnapshot(Types, Props, Functions);

	FCrowdySchemaDelta Delta;
	const FCrowdySchemaSyncReport Report = FCrowdySchemaSync::PlanForSingleEffect(
		FCrowdyDesiredContainerType(), FCrowdyGameModelFunctionInput(), Types, Props, Functions, TArray<FString>(), Delta);

	TestTrue(TEXT("a guarded effect plans nothing"), Delta.IsEmpty());
	TestEqual(TEXT("zero upserts"), Report.UpsertCount(), 0);
	TestEqual(TEXT("no prune candidates"), Report.ServerOnlyCount(), 0);
	return true;
}

// An effect whose function is already on the server (empty delta) but whose app session channel is not yet provisioned
// still has real sync work: a status read must report drift, not "in sync", because the sync will create the channel.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySingleEffectPendingChannelStatusTest,
	"CrowdySDK.GameModel.SingleEffectPendingChannelStatus", SeTestFlags)
bool FCrowdySingleEffectPendingChannelStatusTest::RunTest(const FString& Parameters)
{
	using CrowdyStudioSyncService::ShouldReportDriftForPendingChannel;

	TestTrue(TEXT("empty delta + a channel-needing function + no session channel -> report drift"),
		ShouldReportDriftForPendingChannel(true, true, 0));
	TestFalse(TEXT("an already-resolved session channel -> genuinely in sync"),
		ShouldReportDriftForPendingChannel(true, true, 42));
	TestFalse(TEXT("no function needs the channel -> in sync regardless"),
		ShouldReportDriftForPendingChannel(true, false, 0));
	TestFalse(TEXT("a non-empty delta is drift by the normal path, not this helper's concern"),
		ShouldReportDriftForPendingChannel(false, true, 0));
	return true;
}

// A status read caches its result only when the effect's status epoch is unchanged since kickoff: if a sync or an edit
// bumped it while the read was in flight, the read's value is stale and must not overwrite the fresher cached status.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySingleEffectStatusEpochGuardTest,
	"CrowdySDK.GameModel.SingleEffectStatusEpochGuard", SeTestFlags)
bool FCrowdySingleEffectStatusEpochGuardTest::RunTest(const FString& Parameters)
{
	using CrowdyStudioSyncService::ShouldCacheStatusResult;

	TestTrue(TEXT("an unchanged epoch caches the status result"), ShouldCacheStatusResult(3, 3));
	TestFalse(TEXT("a bumped epoch drops the stale status write"), ShouldCacheStatusResult(3, 4));
	TestFalse(TEXT("epoch 0 vs 1 drops the write (a sync landed during the first read)"), ShouldCacheStatusResult(0, 1));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
