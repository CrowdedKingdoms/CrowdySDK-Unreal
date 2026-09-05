// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "GameModel/CrowdyApplySelection.h"
#include "GameModel/CrowdyStudioFunctionMarshalling.h"
#include "Gql/CrowdyStudioQueries.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Replication/GameModel/CrowdyGameModelMetaKeys.h"
#include "Replication/GameModel/Effect/CrowdyGameModelFunctionMarshaller.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyApplySelectionTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Every fixture here carries the ApplyTest prefix. Adaptive unity merges this module's .cpp files into shared
	// translation units, so two anonymous-namespace helpers of one name in two files redefine each other.

	FCrowdySchemaTypeUpsert ApplyTestType(const FString& TypeName, bool bIsNew)
	{
		FCrowdySchemaTypeUpsert Upsert;
		Upsert.Type.TypeName = TypeName;
		Upsert.Type.DisplayName = TypeName;
		Upsert.bIsNew = bIsNew;
		return Upsert;
	}

	FCrowdySchemaPropUpsert ApplyTestProp(const FString& TypeName, const FString& Key, bool bIsNew)
	{
		FCrowdySchemaPropUpsert Upsert;
		Upsert.ContainerTypeName = TypeName;
		Upsert.Prop.Key = Key;
		Upsert.Prop.ValueType = TEXT("int");
		Upsert.bIsNew = bIsNew;
		return Upsert;
	}

	FCrowdySchemaFunctionUpsert ApplyTestFunction(const FString& TypeName, const FString& Name, bool bIsNew)
	{
		FCrowdySchemaFunctionUpsert Upsert;
		Upsert.Function.ContainerTypeName = TypeName;
		Upsert.Function.Name = Name;
		Upsert.bIsNew = bIsNew;
		return Upsert;
	}

	void ApplyTestAddMutation(
		FCrowdySchemaFunctionUpsert& Upsert, const FString& Target, const FString& Property, const FString& Expression)
	{
		FCrowdyGameModelMutation Mutation;
		Mutation.Target = Target;
		Mutation.Property = Property;
		Mutation.Expression = Expression;
		Upsert.Function.Mutations.Add(MoveTemp(Mutation));
	}

	void ApplyTestAddTimer(FCrowdySchemaFunctionUpsert& Upsert, const FString& FunctionName, const FString& DelayMs)
	{
		FCrowdyGameModelTimer Timer;
		Timer.FunctionName = FunctionName;
		Timer.DelayMsExpression = DelayMs;
		Upsert.Function.Timers.Add(MoveTemp(Timer));
	}

	FCrowdySchemaAutomationUpsert ApplyTestAutomation(
		const FString& Name, const FString& FunctionName, const FString& TargetTypeName, bool bIsNew)
	{
		FCrowdySchemaAutomationUpsert Upsert;
		Upsert.Automation.Name = Name;
		Upsert.Automation.FunctionName = FunctionName;
		Upsert.Automation.TargetMode = TEXT("type");
		Upsert.Automation.TargetTypeName = TargetTypeName;
		Upsert.bIsNew = bIsNew;
		return Upsert;
	}

	FCrowdySchemaTriggerUpsert ApplyTestTrigger(
		const FString& AutomationName, const FString& OnEvent, const FString& PropertyKey, bool bIsNew)
	{
		FCrowdySchemaTriggerUpsert Upsert;
		Upsert.Trigger.AutomationName = AutomationName;
		Upsert.Trigger.OnEvent = OnEvent;
		Upsert.Trigger.PropertyKey = PropertyKey;
		Upsert.bIsNew = bIsNew;
		return Upsert;
	}

	// The five pending arrays a plan holds, owned by the test so the plan input's pointers stay valid for as long as
	// the case does.
	struct FApplyTestPlan
	{
		int64 AppId = 4242;
		TArray<FCrowdySchemaTypeUpsert> Types;
		TArray<FCrowdySchemaPropUpsert> Props;
		TArray<FCrowdySchemaFunctionUpsert> Functions;
		TArray<FCrowdySchemaAutomationUpsert> Automations;
		TArray<FCrowdySchemaTriggerUpsert> Triggers;

		FCrowdyApplyPlanInput Input() const
		{
			FCrowdyApplyPlanInput In;
			In.AppId = AppId;
			In.Types = &Types;
			In.Props = &Props;
			In.Functions = &Functions;
			In.Automations = &Automations;
			In.Triggers = &Triggers;
			return In;
		}
	};

	const FCrowdyApplyUnit* ApplyTestFindUnit(
		const TArray<FCrowdyApplyUnit>& Units, ECrowdyApplyKind Kind, const FString& OwningType, const FString& Name)
	{
		for (const FCrowdyApplyUnit& Unit : Units)
		{
			if (Unit.Kind == Kind
				&& Unit.OwningType.Equals(OwningType, ESearchCase::CaseSensitive)
				&& Unit.Name.Equals(Name, ESearchCase::CaseSensitive))
			{
				return &Unit;
			}
		}
		return nullptr;
	}

	bool ApplyTestHasUnit(
		const TArray<FCrowdyApplyUnit>& Units, ECrowdyApplyKind Kind, const FString& OwningType, const FString& Name)
	{
		return ApplyTestFindUnit(Units, Kind, OwningType, Name) != nullptr;
	}

	int32 ApplyTestCountKind(const TArray<FCrowdyApplyUnit>& Units, ECrowdyApplyKind Kind)
	{
		int32 Count = 0;
		for (const FCrowdyApplyUnit& Unit : Units)
		{
			if (Unit.Kind == Kind)
			{
				++Count;
			}
		}
		return Count;
	}

	FString ApplyTestKeyOf(
		const TArray<FCrowdyApplyUnit>& Units, ECrowdyApplyKind Kind, const FString& OwningType, const FString& Name)
	{
		const FCrowdyApplyUnit* Unit = ApplyTestFindUnit(Units, Kind, OwningType, Name);
		return Unit ? Unit->IdentityKey() : FString();
	}

	TArray<FString> ApplyTestAllKeys(const TArray<FCrowdyApplyUnit>& Units)
	{
		TArray<FString> Keys;
		for (const FCrowdyApplyUnit& Unit : Units)
		{
			Keys.Add(Unit.IdentityKey());
		}
		return Keys;
	}

	// One server operation, as the walk issues it: the generated name and the variables, flattened to text so two
	// lists can be compared field for field rather than by pointer.
	struct FApplyTestOp
	{
		FString OperationName;
		FString Variables;
	};

	FString ApplyTestJsonText(const TSharedPtr<FJsonObject>& Object)
	{
		if (!Object.IsValid())
		{
			return FString();
		}
		FString Text;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
		return Text;
	}

	// The one op builder both lists in the order test go through, so what that test compares is the ORDER two
	// traversals produce and never two different ways of spelling an upsert.
	FApplyTestOp ApplyTestBuildOp(const FApplyTestPlan& Plan, ECrowdyApplyKind Kind, int32 Index)
	{
		FApplyTestOp Op;
		switch (Kind)
		{
		case ECrowdyApplyKind::Type:
		{
			const FCrowdySchemaTypeUpsert& Upsert = Plan.Types[Index];
			const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
			CrowdyStudioMarshalling::SetBigIntField(Input, TEXT("appId"), Plan.AppId);
			Input->SetStringField(TEXT("typeName"), Upsert.Type.TypeName);
			Input->SetStringField(TEXT("displayName"), Upsert.Type.DisplayName);
			Input->SetStringField(TEXT("instantiableBy"), Upsert.Type.InstantiableBy);
			Input->SetStringField(TEXT("defaultPropertyVisibility"), Upsert.Type.DefaultVisibility);
			const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
			Variables->SetObjectField(TEXT("input"), Input);
			Op.OperationName = TEXT("GameModelUpsertContainerType");
			Op.Variables = ApplyTestJsonText(Variables);
			break;
		}

		case ECrowdyApplyKind::Attribute:
		{
			const FCrowdySchemaPropUpsert& Upsert = Plan.Props[Index];
			const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
			CrowdyStudioMarshalling::SetBigIntField(Input, TEXT("appId"), Plan.AppId);
			Input->SetStringField(TEXT("containerTypeName"), Upsert.ContainerTypeName);
			Input->SetStringField(TEXT("key"), Upsert.Prop.Key);
			Input->SetStringField(TEXT("valueType"), Upsert.Prop.ValueType);
			CrowdyStudioMarshalling::SetOptionalStringField(Input, TEXT("defaultValueJson"), Upsert.Prop.DefaultValueJson);
			Input->SetStringField(TEXT("visibility"), Upsert.Prop.Visibility);
			Input->SetStringField(TEXT("writable"), Upsert.Prop.Writable);
			const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
			Variables->SetObjectField(TEXT("input"), Input);
			Op.OperationName = TEXT("GameModelUpsertPropertyDef");
			Op.Variables = ApplyTestJsonText(Variables);
			break;
		}

		case ECrowdyApplyKind::Function:
		{
			const TSharedPtr<FJsonObject> Input =
				CrowdyGameModelMarshalling::BuildFunctionUpsertInput(Plan.Functions[Index].Function, Plan.AppId);
			const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
			Variables->SetObjectField(TEXT("input"), Input);
			Op.OperationName = TEXT("GameModelUpsertFunction");
			Op.Variables = ApplyTestJsonText(Variables);
			break;
		}

		case ECrowdyApplyKind::Automation:
			Op.OperationName = TEXT("GameModelUpsertAutomation");
			Op.Variables = ApplyTestJsonText(
				CrowdyStudioGql::BuildAutomationUpsertVariables(Plan.Automations[Index].Automation, Plan.AppId));
			break;

		case ECrowdyApplyKind::Trigger:
			Op.OperationName = TEXT("GameModelUpsertAutomationTrigger");
			Op.Variables = ApplyTestJsonText(
				CrowdyStudioGql::BuildAutomationTriggerUpsertVariables(Plan.Triggers[Index].Trigger, Plan.AppId));
			break;

		default:
			break;
		}
		return Op;
	}

	// The op list the existing all-or-nothing apply builds: the five pending arrays, each walked whole, in the order
	// the walk declares them.
	TArray<FApplyTestOp> ApplyTestUnfilteredOps(const FApplyTestPlan& Plan)
	{
		TArray<FApplyTestOp> Ops;
		for (int32 Index = 0; Index < Plan.Types.Num(); ++Index)
		{
			Ops.Add(ApplyTestBuildOp(Plan, ECrowdyApplyKind::Type, Index));
		}
		for (int32 Index = 0; Index < Plan.Props.Num(); ++Index)
		{
			Ops.Add(ApplyTestBuildOp(Plan, ECrowdyApplyKind::Attribute, Index));
		}
		for (int32 Index = 0; Index < Plan.Functions.Num(); ++Index)
		{
			Ops.Add(ApplyTestBuildOp(Plan, ECrowdyApplyKind::Function, Index));
		}
		for (int32 Index = 0; Index < Plan.Automations.Num(); ++Index)
		{
			Ops.Add(ApplyTestBuildOp(Plan, ECrowdyApplyKind::Automation, Index));
		}
		for (int32 Index = 0; Index < Plan.Triggers.Num(); ++Index)
		{
			Ops.Add(ApplyTestBuildOp(Plan, ECrowdyApplyKind::Trigger, Index));
		}
		return Ops;
	}

	// The op list a selective apply builds: the closed set, in the order it came back, each unit addressed by its
	// kind and plan index.
	TArray<FApplyTestOp> ApplyTestSelectiveOps(const FApplyTestPlan& Plan, const TArray<FCrowdyApplyUnit>& ClosedSet)
	{
		TArray<FApplyTestOp> Ops;
		for (const FCrowdyApplyUnit& Unit : ClosedSet)
		{
			Ops.Add(ApplyTestBuildOp(Plan, Unit.Kind, Unit.PlanIndex));
		}
		return Ops;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyUnitIdentityKeepsItsScopeTest,
	"CrowdySDK.CrowdyStudio.ApplyUnitIdentityKeepsItsScope", CrowdyApplySelectionTestFlags)

bool FCrowdyApplyUnitIdentityKeepsItsScopeTest::RunTest(const FString& Parameters)
{
	FApplyTestPlan Plan;
	Plan.Functions.Add(ApplyTestFunction(TEXT("Knight"), TEXT("regen"), true));
	Plan.Functions.Add(ApplyTestFunction(TEXT("Goblin"), TEXT("regen"), true));

	const TArray<FCrowdyApplyUnit> Units = CrowdyApplySelection::BuildUnits(Plan.Input());
	TestEqual(TEXT("both functions become units"), Units.Num(), 2);

	const FString KnightKey = ApplyTestKeyOf(Units, ECrowdyApplyKind::Function, TEXT("Knight"), TEXT("regen"));
	const FString GoblinKey = ApplyTestKeyOf(Units, ECrowdyApplyKind::Function, TEXT("Goblin"), TEXT("regen"));

	TestTrue(TEXT("the Knight function has a key"), !KnightKey.IsEmpty());
	TestTrue(TEXT("the Goblin function has a key"), !GoblinKey.IsEmpty());
	TestNotEqual(TEXT("one function name on two models is two identities"), KnightKey, GoblinKey);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyUnitIdentityIsCaseSensitiveTest,
	"CrowdySDK.CrowdyStudio.ApplyUnitIdentityIsCaseSensitive", CrowdyApplySelectionTestFlags)

bool FCrowdyApplyUnitIdentityIsCaseSensitiveTest::RunTest(const FString& Parameters)
{
	FCrowdyApplyUnit Upper;
	Upper.Kind = ECrowdyApplyKind::Attribute;
	Upper.OwningType = TEXT("Hero");
	Upper.Name = TEXT("hp");

	FCrowdyApplyUnit Lower = Upper;
	Lower.OwningType = TEXT("hero");

	TestFalse(TEXT("Hero and hero are not the same unit"), Upper == Lower);
	TestTrue(TEXT("Hero and hero compare unequal"), Upper != Lower);

	TArray<FCrowdyApplyUnit> Units;
	Units.Add(Upper);
	TestFalse(TEXT("an array holding Hero does not hold hero"), Units.Contains(Lower));

	FCrowdyApplyUnit UpperKey = Upper;
	FCrowdyApplyUnit LowerKey = Upper;
	LowerKey.Name = TEXT("HP");
	TestFalse(TEXT("hp and HP are not the same unit"), UpperKey == LowerKey);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyUnitIdentitySeparatesTwoTriggersOnOneAutomationTest,
	"CrowdySDK.CrowdyStudio.ApplyUnitIdentitySeparatesTwoTriggersOnOneAutomation", CrowdyApplySelectionTestFlags)

bool FCrowdyApplyUnitIdentitySeparatesTwoTriggersOnOneAutomationTest::RunTest(const FString& Parameters)
{
	FApplyTestPlan Plan;
	Plan.Triggers.Add(ApplyTestTrigger(TEXT("goblin_tick"), TEXT("property_changed"), TEXT("hp"), true));
	Plan.Triggers.Add(ApplyTestTrigger(TEXT("goblin_tick"), TEXT("property_changed"), TEXT("mana"), true));

	const TArray<FCrowdyApplyUnit> Units = CrowdyApplySelection::BuildUnits(Plan.Input());
	TestEqual(TEXT("both triggers become units"), Units.Num(), 2);
	TestNotEqual(TEXT("two triggers differing only in the property key are two identities"),
		Units[0].IdentityKey(), Units[1].IdentityKey());
	TestFalse(TEXT("the two trigger units are not equal"), Units[0] == Units[1]);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyTriggerIdentityMirrorsTheDiffKeyTest,
	"CrowdySDK.CrowdyStudio.ApplyTriggerIdentityMirrorsTheDiffKey", CrowdyApplySelectionTestFlags)

bool FCrowdyApplyTriggerIdentityMirrorsTheDiffKeyTest::RunTest(const FString& Parameters)
{
	// The schema diff keys a trigger on the automation name, the event, and the function, model and property-key
	// filters, and treats the write source as a tunable that updates a trigger rather than as a different trigger.
	// A unit key finer than that drops a ticked trigger whenever the write source changes across a re-plan, for an
	// entity sitting right there in the plan; a coarser one folds two distinct triggers into one and sends the
	// wrong one. Matching the diff's field list exactly is the only reading with neither failure.
	FApplyTestPlan WriteSourcePlan;
	FCrowdySchemaTriggerUpsert AnyWrite =
		ApplyTestTrigger(TEXT("goblin_tick"), TEXT("property_changed"), TEXT("hp"), true);
	AnyWrite.Trigger.ContainerTypeName = TEXT("Knight");
	AnyWrite.Trigger.WriteSource = TEXT("any");
	WriteSourcePlan.Triggers.Add(MoveTemp(AnyWrite));

	FCrowdySchemaTriggerUpsert DirectWrite =
		ApplyTestTrigger(TEXT("goblin_tick"), TEXT("property_changed"), TEXT("hp"), true);
	DirectWrite.Trigger.ContainerTypeName = TEXT("Knight");
	DirectWrite.Trigger.WriteSource = TEXT("direct");
	WriteSourcePlan.Triggers.Add(MoveTemp(DirectWrite));

	const TArray<FCrowdyApplyUnit> WriteSourceUnits = CrowdyApplySelection::BuildUnits(WriteSourcePlan.Input());
	if (WriteSourceUnits.Num() != 2)
	{
		AddError(TEXT("the fixture did not produce the two trigger units the case is about"));
		return false;
	}
	TestEqual(TEXT("the write source is not part of a trigger's discriminator"),
		WriteSourceUnits[0].Discriminator, WriteSourceUnits[1].Discriminator);
	TestEqual(TEXT("two triggers differing only in the write source are one unit"),
		WriteSourceUnits[0].IdentityKey(), WriteSourceUnits[1].IdentityKey());
	TestTrue(TEXT("the two trigger units compare equal"), WriteSourceUnits[0] == WriteSourceUnits[1]);

	// The model filter IS in the diff's key, so two triggers differing only in it are two entities.
	FApplyTestPlan ModelPlan;
	FCrowdySchemaTriggerUpsert OnKnight =
		ApplyTestTrigger(TEXT("goblin_tick"), TEXT("property_changed"), TEXT("hp"), true);
	OnKnight.Trigger.ContainerTypeName = TEXT("Knight");
	ModelPlan.Triggers.Add(MoveTemp(OnKnight));

	FCrowdySchemaTriggerUpsert OnGoblin =
		ApplyTestTrigger(TEXT("goblin_tick"), TEXT("property_changed"), TEXT("hp"), true);
	OnGoblin.Trigger.ContainerTypeName = TEXT("Goblin");
	ModelPlan.Triggers.Add(MoveTemp(OnGoblin));

	const TArray<FCrowdyApplyUnit> ModelUnits = CrowdyApplySelection::BuildUnits(ModelPlan.Input());
	if (ModelUnits.Num() != 2)
	{
		AddError(TEXT("the fixture did not produce the two trigger units the case is about"));
		return false;
	}
	TestNotEqual(TEXT("the model filter is part of a trigger's discriminator"),
		ModelUnits[0].Discriminator, ModelUnits[1].Discriminator);
	TestNotEqual(TEXT("two triggers differing only in the model filter are two units"),
		ModelUnits[0].IdentityKey(), ModelUnits[1].IdentityKey());
	TestFalse(TEXT("the two trigger units do not compare equal"), ModelUnits[0] == ModelUnits[1]);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyUnitScopeAnswersUndeterminedSeparatelyTest,
	"CrowdySDK.CrowdyStudio.ApplyUnitScopeAnswersUndeterminedSeparately", CrowdyApplySelectionTestFlags)

bool FCrowdyApplyUnitScopeAnswersUndeterminedSeparatelyTest::RunTest(const FString& Parameters)
{
	FApplyTestPlan Plan;
	Plan.Functions.Add(ApplyTestFunction(FString(), TEXT("regen"), true));
	Plan.Functions.Add(ApplyTestFunction(TEXT("Knight"), TEXT("take_damage"), true));
	Plan.Automations.Add(ApplyTestAutomation(TEXT("goblin_tick"), TEXT("regen"), FString(), true));

	const TArray<FCrowdyApplyUnit> Units = CrowdyApplySelection::BuildUnits(Plan.Input());

	const FCrowdyApplyUnit* Unscoped = ApplyTestFindUnit(Units, ECrowdyApplyKind::Function, FString(), TEXT("regen"));
	const FCrowdyApplyUnit* Scoped =
		ApplyTestFindUnit(Units, ECrowdyApplyKind::Function, TEXT("Knight"), TEXT("take_damage"));
	const FCrowdyApplyUnit* Automation =
		ApplyTestFindUnit(Units, ECrowdyApplyKind::Automation, FString(), TEXT("goblin_tick"));

	if (!Unscoped || !Scoped || !Automation)
	{
		AddError(TEXT("the fixture did not produce the three units the case is about"));
		return false;
	}

	TestTrue(TEXT("a function with no model is Undetermined"), Unscoped->Scope() == ECrowdyApplyScope::Undetermined);
	TestFalse(TEXT("a function with no model has no determined scope"), Unscoped->HasDeterminedScope());
	TestTrue(TEXT("a function with a model is Determined"), Scoped->Scope() == ECrowdyApplyScope::Determined);
	TestTrue(TEXT("an automation carries no scope at all"), Automation->Scope() == ECrowdyApplyScope::NotScoped);
	TestTrue(TEXT("an automation still has a determined scope"), Automation->HasDeterminedScope());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyUnitWithUndeterminedScopeIsNotSelectableTest,
	"CrowdySDK.CrowdyStudio.ApplyUnitWithUndeterminedScopeIsNotSelectable", CrowdyApplySelectionTestFlags)

bool FCrowdyApplyUnitWithUndeterminedScopeIsNotSelectableTest::RunTest(const FString& Parameters)
{
	FCrowdyApplyUnit Unscoped;
	Unscoped.Kind = ECrowdyApplyKind::Function;
	Unscoped.Name = TEXT("regen");

	FString Reason;
	TestFalse(TEXT("a function with no model cannot be ticked"), CrowdyApplySelection::CanSelect(Unscoped, Reason));
	TestTrue(TEXT("the refusal says why"), !Reason.IsEmpty());

	FCrowdyApplyUnit Scoped = Unscoped;
	Scoped.OwningType = TEXT("Knight");
	FString ScopedReason = TEXT("stale");
	TestTrue(TEXT("a function with a model can be ticked"), CrowdyApplySelection::CanSelect(Scoped, ScopedReason));
	TestTrue(TEXT("an allowed unit carries no reason"), ScopedReason.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyClosureAddsTheNewTypeAnAttributeNeedsTest,
	"CrowdySDK.CrowdyStudio.ApplyClosureAddsTheNewTypeAnAttributeNeeds", CrowdyApplySelectionTestFlags)

bool FCrowdyApplyClosureAddsTheNewTypeAnAttributeNeedsTest::RunTest(const FString& Parameters)
{
	FApplyTestPlan Plan;
	Plan.Types.Add(ApplyTestType(TEXT("Knight"), true));
	Plan.Props.Add(ApplyTestProp(TEXT("Knight"), TEXT("hp"), true));

	const TArray<FCrowdyApplyUnit> Units = CrowdyApplySelection::BuildUnits(Plan.Input());
	TArray<FString> Selected;
	Selected.Add(ApplyTestKeyOf(Units, ECrowdyApplyKind::Attribute, TEXT("Knight"), TEXT("hp")));

	const FCrowdyApplyPlan Result = CrowdyApplySelection::BuildPlan(Plan.Input(), Selected);

	TestTrue(TEXT("the model the attribute needs is sent"),
		ApplyTestHasUnit(Result.ClosedSet, ECrowdyApplyKind::Type, FString(), TEXT("Knight")));
	TestEqual(TEXT("exactly one addition"), Result.Additions.Num(), 1);
	TestTrue(TEXT("the plan can be sent"), Result.bSendable);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyClosureOmitsATypeThatIsOnlyAnUpdateTest,
	"CrowdySDK.CrowdyStudio.ApplyClosureOmitsATypeThatIsOnlyAnUpdate", CrowdyApplySelectionTestFlags)

bool FCrowdyApplyClosureOmitsATypeThatIsOnlyAnUpdateTest::RunTest(const FString& Parameters)
{
	FApplyTestPlan Plan;
	Plan.Types.Add(ApplyTestType(TEXT("Knight"), false));
	Plan.Props.Add(ApplyTestProp(TEXT("Knight"), TEXT("hp"), true));

	const TArray<FCrowdyApplyUnit> Units = CrowdyApplySelection::BuildUnits(Plan.Input());
	TArray<FString> Selected;
	Selected.Add(ApplyTestKeyOf(Units, ECrowdyApplyKind::Attribute, TEXT("Knight"), TEXT("hp")));

	const FCrowdyApplyPlan Result = CrowdyApplySelection::BuildPlan(Plan.Input(), Selected);

	TestFalse(TEXT("a model the server already has is not dragged in"),
		ApplyTestHasUnit(Result.ClosedSet, ECrowdyApplyKind::Type, FString(), TEXT("Knight")));
	TestEqual(TEXT("only the ticked attribute is sent"), Result.ClosedSet.Num(), 1);
	TestEqual(TEXT("nothing was added"), Result.Additions.Num(), 0);
	TestTrue(TEXT("the plan can be sent"), Result.bSendable);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyClosureOmitsAPrerequisiteTheServerAlreadyHasTest,
	"CrowdySDK.CrowdyStudio.ApplyClosureOmitsAPrerequisiteTheServerAlreadyHas", CrowdyApplySelectionTestFlags)

bool FCrowdyApplyClosureOmitsAPrerequisiteTheServerAlreadyHasTest::RunTest(const FString& Parameters)
{
	// The model the function is bound to is absent from the plan entirely, which is what a delta looks like when
	// the server is already correct about it.
	FApplyTestPlan Plan;
	Plan.Functions.Add(ApplyTestFunction(TEXT("Knight"), TEXT("take_damage"), true));

	const TArray<FCrowdyApplyUnit> Units = CrowdyApplySelection::BuildUnits(Plan.Input());
	TArray<FString> Selected;
	Selected.Add(ApplyTestKeyOf(Units, ECrowdyApplyKind::Function, TEXT("Knight"), TEXT("take_damage")));

	const FCrowdyApplyPlan Result = CrowdyApplySelection::BuildPlan(Plan.Input(), Selected);

	TestEqual(TEXT("only the ticked function is sent"), Result.ClosedSet.Num(), 1);
	TestEqual(TEXT("nothing was added"), Result.Additions.Num(), 0);
	TestEqual(TEXT("nothing was unresolvable"), Result.Unresolvable.Num(), 0);
	TestTrue(TEXT("an absent prerequisite is not a refusal"), Result.Refusal == ECrowdyApplyRefusal::None);
	TestTrue(TEXT("the plan can be sent"), Result.bSendable);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyClosureAddsTheNewAttributeAFunctionWritesTest,
	"CrowdySDK.CrowdyStudio.ApplyClosureAddsTheNewAttributeAFunctionWrites", CrowdyApplySelectionTestFlags)

bool FCrowdyApplyClosureAddsTheNewAttributeAFunctionWritesTest::RunTest(const FString& Parameters)
{
	FApplyTestPlan Plan;
	Plan.Props.Add(ApplyTestProp(TEXT("Knight"), TEXT("hp"), true));
	Plan.Props.Add(ApplyTestProp(TEXT("Goblin"), TEXT("hp"), true));

	FCrowdySchemaFunctionUpsert Function = ApplyTestFunction(TEXT("Knight"), TEXT("take_damage"), true);
	ApplyTestAddMutation(Function, TEXT("self"), TEXT("hp"), TEXT("self.hp - 1"));
	Plan.Functions.Add(MoveTemp(Function));

	const TArray<FCrowdyApplyUnit> Units = CrowdyApplySelection::BuildUnits(Plan.Input());
	TArray<FString> Selected;
	Selected.Add(ApplyTestKeyOf(Units, ECrowdyApplyKind::Function, TEXT("Knight"), TEXT("take_damage")));

	const FCrowdyApplyPlan Result = CrowdyApplySelection::BuildPlan(Plan.Input(), Selected);

	TestTrue(TEXT("the key the function writes on its own model is sent"),
		ApplyTestHasUnit(Result.ClosedSet, ECrowdyApplyKind::Attribute, TEXT("Knight"), TEXT("hp")));
	TestFalse(TEXT("the same key on another model is left alone"),
		ApplyTestHasUnit(Result.ClosedSet, ECrowdyApplyKind::Attribute, TEXT("Goblin"), TEXT("hp")));
	TestEqual(TEXT("exactly one addition"), Result.Additions.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyClosureAddsEveryCandidateForACrossModelWriteTest,
	"CrowdySDK.CrowdyStudio.ApplyClosureAddsEveryCandidateForACrossModelWrite", CrowdyApplySelectionTestFlags)

bool FCrowdyApplyClosureAddsEveryCandidateForACrossModelWriteTest::RunTest(const FString& Parameters)
{
	FApplyTestPlan Plan;
	Plan.Props.Add(ApplyTestProp(TEXT("Knight"), TEXT("hp"), true));
	Plan.Props.Add(ApplyTestProp(TEXT("Goblin"), TEXT("hp"), true));
	Plan.Props.Add(ApplyTestProp(TEXT("Goblin"), TEXT("mana"), true));

	FCrowdySchemaFunctionUpsert Function = ApplyTestFunction(TEXT("Knight"), TEXT("take_damage"), true);
	ApplyTestAddMutation(Function, TEXT("ref($target)"), TEXT("hp"), TEXT("0"));
	Plan.Functions.Add(MoveTemp(Function));

	const TArray<FCrowdyApplyUnit> Units = CrowdyApplySelection::BuildUnits(Plan.Input());
	TArray<FString> Selected;
	Selected.Add(ApplyTestKeyOf(Units, ECrowdyApplyKind::Function, TEXT("Knight"), TEXT("take_damage")));

	const FCrowdyApplyPlan Result = CrowdyApplySelection::BuildPlan(Plan.Input(), Selected);

	TestTrue(TEXT("hp on Knight is sent"),
		ApplyTestHasUnit(Result.ClosedSet, ECrowdyApplyKind::Attribute, TEXT("Knight"), TEXT("hp")));
	TestTrue(TEXT("hp on Goblin is sent too, because nothing narrows the write down"),
		ApplyTestHasUnit(Result.ClosedSet, ECrowdyApplyKind::Attribute, TEXT("Goblin"), TEXT("hp")));
	TestFalse(TEXT("an unrelated key is not dragged in"),
		ApplyTestHasUnit(Result.ClosedSet, ECrowdyApplyKind::Attribute, TEXT("Goblin"), TEXT("mana")));
	TestEqual(TEXT("both candidates are named as additions"), Result.Additions.Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyClosureAddsTheFunctionAnAutomationRunsTest,
	"CrowdySDK.CrowdyStudio.ApplyClosureAddsTheFunctionAnAutomationRuns", CrowdyApplySelectionTestFlags)

bool FCrowdyApplyClosureAddsTheFunctionAnAutomationRunsTest::RunTest(const FString& Parameters)
{
	FApplyTestPlan Plan;
	Plan.Functions.Add(ApplyTestFunction(TEXT("Goblin"), TEXT("regen"), true));
	Plan.Automations.Add(ApplyTestAutomation(TEXT("goblin_tick"), TEXT("regen"), FString(), true));

	const TArray<FCrowdyApplyUnit> Units = CrowdyApplySelection::BuildUnits(Plan.Input());
	TArray<FString> Selected;
	Selected.Add(ApplyTestKeyOf(Units, ECrowdyApplyKind::Automation, FString(), TEXT("goblin_tick")));

	const FCrowdyApplyPlan Result = CrowdyApplySelection::BuildPlan(Plan.Input(), Selected);

	TestTrue(TEXT("the function the automation runs is sent"),
		ApplyTestHasUnit(Result.ClosedSet, ECrowdyApplyKind::Function, TEXT("Goblin"), TEXT("regen")));
	TestEqual(TEXT("exactly one addition"), Result.Additions.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyClosureAddsTheAutomationATriggerFiresTest,
	"CrowdySDK.CrowdyStudio.ApplyClosureAddsTheAutomationATriggerFires", CrowdyApplySelectionTestFlags)

bool FCrowdyApplyClosureAddsTheAutomationATriggerFiresTest::RunTest(const FString& Parameters)
{
	FApplyTestPlan Plan;
	Plan.Automations.Add(ApplyTestAutomation(TEXT("goblin_tick"), FString(), FString(), true));
	Plan.Triggers.Add(ApplyTestTrigger(TEXT("goblin_tick"), TEXT("property_changed"), TEXT("hp"), true));

	const TArray<FCrowdyApplyUnit> Units = CrowdyApplySelection::BuildUnits(Plan.Input());
	const FCrowdyApplyUnit* Trigger = ApplyTestFindUnit(Units, ECrowdyApplyKind::Trigger, FString(), TEXT("goblin_tick"));
	if (!Trigger)
	{
		AddError(TEXT("the fixture did not produce a trigger unit"));
		return false;
	}

	TArray<FString> Selected;
	Selected.Add(Trigger->IdentityKey());

	const FCrowdyApplyPlan Result = CrowdyApplySelection::BuildPlan(Plan.Input(), Selected);

	TestTrue(TEXT("the automation the trigger fires is sent"),
		ApplyTestHasUnit(Result.ClosedSet, ECrowdyApplyKind::Automation, FString(), TEXT("goblin_tick")));
	TestEqual(TEXT("exactly one addition"), Result.Additions.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyClosureFollowsATimersCalleeTest,
	"CrowdySDK.CrowdyStudio.ApplyClosureFollowsATimersCallee", CrowdyApplySelectionTestFlags)

bool FCrowdyApplyClosureFollowsATimersCalleeTest::RunTest(const FString& Parameters)
{
	FApplyTestPlan Plan;
	FCrowdySchemaFunctionUpsert Armer = ApplyTestFunction(TEXT("Knight"), TEXT("take_damage"), true);
	ApplyTestAddTimer(Armer, TEXT("bleed_tick"), TEXT("1000"));
	Plan.Functions.Add(MoveTemp(Armer));
	Plan.Functions.Add(ApplyTestFunction(TEXT("Knight"), TEXT("bleed_tick"), true));

	const TArray<FCrowdyApplyUnit> Units = CrowdyApplySelection::BuildUnits(Plan.Input());
	TArray<FString> Selected;
	Selected.Add(ApplyTestKeyOf(Units, ECrowdyApplyKind::Function, TEXT("Knight"), TEXT("take_damage")));

	const FCrowdyApplyPlan Result = CrowdyApplySelection::BuildPlan(Plan.Input(), Selected);

	TestTrue(TEXT("the function the timer arms is sent"),
		ApplyTestHasUnit(Result.ClosedSet, ECrowdyApplyKind::Function, TEXT("Knight"), TEXT("bleed_tick")));
	TestEqual(TEXT("exactly one addition"), Result.Additions.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyClosureFollowsAnFnCallInAnExpressionTest,
	"CrowdySDK.CrowdyStudio.ApplyClosureFollowsAnFnCallInAnExpression", CrowdyApplySelectionTestFlags)

bool FCrowdyApplyClosureFollowsAnFnCallInAnExpressionTest::RunTest(const FString& Parameters)
{
	FApplyTestPlan Plan;
	FCrowdySchemaFunctionUpsert Caller = ApplyTestFunction(TEXT("Knight"), TEXT("take_damage"), true);
	Caller.Function.ReturnExpression = TEXT("fn:armour_of(self) + 1");
	Plan.Functions.Add(MoveTemp(Caller));
	Plan.Functions.Add(ApplyTestFunction(TEXT("Knight"), TEXT("armour_of"), true));

	const TArray<FCrowdyApplyUnit> Units = CrowdyApplySelection::BuildUnits(Plan.Input());
	TArray<FString> Selected;
	Selected.Add(ApplyTestKeyOf(Units, ECrowdyApplyKind::Function, TEXT("Knight"), TEXT("take_damage")));

	const FCrowdyApplyPlan Result = CrowdyApplySelection::BuildPlan(Plan.Input(), Selected);

	TestTrue(TEXT("a callee named only inside the returned value is sent"),
		ApplyTestHasUnit(Result.ClosedSet, ECrowdyApplyKind::Function, TEXT("Knight"), TEXT("armour_of")));
	TestEqual(TEXT("exactly one addition"), Result.Additions.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyClosureCalleeScanMatchesWholeNamesOnlyTest,
	"CrowdySDK.CrowdyStudio.ApplyClosureCalleeScanMatchesWholeNamesOnly", CrowdyApplySelectionTestFlags)

bool FCrowdyApplyClosureCalleeScanMatchesWholeNamesOnlyTest::RunTest(const FString& Parameters)
{
	FApplyTestPlan Plan;
	FCrowdySchemaFunctionUpsert Caller = ApplyTestFunction(TEXT("Knight"), TEXT("take_damage"), true);
	Caller.Function.ReturnExpression = TEXT("fn:regen_all(self)");
	Plan.Functions.Add(MoveTemp(Caller));
	Plan.Functions.Add(ApplyTestFunction(TEXT("Knight"), TEXT("regen"), true));
	Plan.Functions.Add(ApplyTestFunction(TEXT("Knight"), TEXT("regen_all"), true));

	const TArray<FCrowdyApplyUnit> Units = CrowdyApplySelection::BuildUnits(Plan.Input());

	const TArray<FString> Callees =
		CrowdyApplySelection::FunctionCallees(Plan.Functions[0].Function);
	TestEqual(TEXT("one callee is found"), Callees.Num(), 1);
	if (Callees.Num() == 1)
	{
		TestEqual(TEXT("the callee is the whole name"), Callees[0], FString(TEXT("regen_all")));
	}

	TArray<FString> Selected;
	Selected.Add(ApplyTestKeyOf(Units, ECrowdyApplyKind::Function, TEXT("Knight"), TEXT("take_damage")));
	const FCrowdyApplyPlan Result = CrowdyApplySelection::BuildPlan(Plan.Input(), Selected);

	TestTrue(TEXT("the whole-name callee is sent"),
		ApplyTestHasUnit(Result.ClosedSet, ECrowdyApplyKind::Function, TEXT("Knight"), TEXT("regen_all")));
	TestFalse(TEXT("the shorter name it starts with is not"),
		ApplyTestHasUnit(Result.ClosedSet, ECrowdyApplyKind::Function, TEXT("Knight"), TEXT("regen")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyClosureIgnoresTriggerFiltersTest,
	"CrowdySDK.CrowdyStudio.ApplyClosureIgnoresTriggerFilters", CrowdyApplySelectionTestFlags)

bool FCrowdyApplyClosureIgnoresTriggerFiltersTest::RunTest(const FString& Parameters)
{
	FApplyTestPlan Plan;
	Plan.Types.Add(ApplyTestType(TEXT("Knight"), true));
	Plan.Props.Add(ApplyTestProp(TEXT("Knight"), TEXT("hp"), true));
	Plan.Functions.Add(ApplyTestFunction(TEXT("Knight"), TEXT("take_damage"), true));
	Plan.Automations.Add(ApplyTestAutomation(TEXT("goblin_tick"), FString(), FString(), true));

	FCrowdySchemaTriggerUpsert Trigger = ApplyTestTrigger(TEXT("goblin_tick"), TEXT("property_changed"), TEXT("hp"), true);
	Trigger.Trigger.FunctionName = TEXT("take_damage");
	Trigger.Trigger.ContainerTypeName = TEXT("Knight");
	Plan.Triggers.Add(MoveTemp(Trigger));

	const TArray<FCrowdyApplyUnit> Units = CrowdyApplySelection::BuildUnits(Plan.Input());
	const FCrowdyApplyUnit* TriggerUnit =
		ApplyTestFindUnit(Units, ECrowdyApplyKind::Trigger, TEXT("Knight"), TEXT("goblin_tick"));
	if (!TriggerUnit)
	{
		AddError(TEXT("the fixture did not produce a trigger unit"));
		return false;
	}

	TArray<FString> Selected;
	Selected.Add(TriggerUnit->IdentityKey());
	const FCrowdyApplyPlan Result = CrowdyApplySelection::BuildPlan(Plan.Input(), Selected);

	TestTrue(TEXT("the automation the trigger fires is sent"),
		ApplyTestHasUnit(Result.ClosedSet, ECrowdyApplyKind::Automation, FString(), TEXT("goblin_tick")));
	TestFalse(TEXT("the function filter is not a prerequisite"),
		ApplyTestHasUnit(Result.ClosedSet, ECrowdyApplyKind::Function, TEXT("Knight"), TEXT("take_damage")));
	TestFalse(TEXT("the model filter is not a prerequisite"),
		ApplyTestHasUnit(Result.ClosedSet, ECrowdyApplyKind::Type, FString(), TEXT("Knight")));
	TestFalse(TEXT("the property filter is not a prerequisite"),
		ApplyTestHasUnit(Result.ClosedSet, ECrowdyApplyKind::Attribute, TEXT("Knight"), TEXT("hp")));
	TestEqual(TEXT("only the automation was added"), Result.Additions.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyClosureRefusesAnUnresolvablePrerequisiteTest,
	"CrowdySDK.CrowdyStudio.ApplyClosureRefusesAnUnresolvablePrerequisite", CrowdyApplySelectionTestFlags)

bool FCrowdyApplyClosureRefusesAnUnresolvablePrerequisiteTest::RunTest(const FString& Parameters)
{
	// The only candidate for the function the automation runs is one whose owning model never resolved. Adding it
	// writes an upsert nobody can say is correct; omitting it may write an automation naming a function the server
	// does not have, so the closure refuses instead of picking one of the two wrong answers.
	FApplyTestPlan Plan;
	Plan.Functions.Add(ApplyTestFunction(FString(), TEXT("regen"), true));
	Plan.Automations.Add(ApplyTestAutomation(TEXT("goblin_tick"), TEXT("regen"), FString(), true));

	const TArray<FCrowdyApplyUnit> Units = CrowdyApplySelection::BuildUnits(Plan.Input());
	TArray<FString> Selected;
	Selected.Add(ApplyTestKeyOf(Units, ECrowdyApplyKind::Automation, FString(), TEXT("goblin_tick")));

	const FCrowdyApplyPlan Result = CrowdyApplySelection::BuildPlan(Plan.Input(), Selected);

	TestTrue(TEXT("the refusal names the unresolvable prerequisite"),
		Result.Refusal == ECrowdyApplyRefusal::UnresolvablePrerequisite);
	TestFalse(TEXT("the plan cannot be sent"), Result.bSendable);
	TestEqual(TEXT("the unresolvable unit is named"), Result.Unresolvable.Num(), 1);
	TestTrue(TEXT("the unresolvable unit is the function with no model"),
		ApplyTestHasUnit(Result.Unresolvable, ECrowdyApplyKind::Function, FString(), TEXT("regen")));
	TestFalse(TEXT("the unresolvable function is not sent anyway"),
		ApplyTestHasUnit(Result.ClosedSet, ECrowdyApplyKind::Function, FString(), TEXT("regen")));
	TestTrue(TEXT("the blocked reason is spelled out"), !Result.Sheet.BlockedReason.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyClosureNamesWhatForcedEachAdditionTest,
	"CrowdySDK.CrowdyStudio.ApplyClosureNamesWhatForcedEachAddition", CrowdyApplySelectionTestFlags)

bool FCrowdyApplyClosureNamesWhatForcedEachAdditionTest::RunTest(const FString& Parameters)
{
	// Two links deep: the ticked trigger forces the automation, and the automation forces the function. Both
	// additions must name the TRIGGER, which is the row the reader actually ticked.
	FApplyTestPlan Plan;
	Plan.Functions.Add(ApplyTestFunction(TEXT("Goblin"), TEXT("regen"), true));
	Plan.Automations.Add(ApplyTestAutomation(TEXT("goblin_tick"), TEXT("regen"), FString(), true));
	Plan.Triggers.Add(ApplyTestTrigger(TEXT("goblin_tick"), TEXT("property_changed"), TEXT("hp"), true));

	const TArray<FCrowdyApplyUnit> Units = CrowdyApplySelection::BuildUnits(Plan.Input());
	const FCrowdyApplyUnit* Trigger = ApplyTestFindUnit(Units, ECrowdyApplyKind::Trigger, FString(), TEXT("goblin_tick"));
	if (!Trigger)
	{
		AddError(TEXT("the fixture did not produce a trigger unit"));
		return false;
	}

	TArray<FString> Selected;
	Selected.Add(Trigger->IdentityKey());
	const FCrowdyApplyPlan Result = CrowdyApplySelection::BuildPlan(Plan.Input(), Selected);

	TestEqual(TEXT("two things were added"), Result.Additions.Num(), 2);
	for (const FCrowdyApplyAddition& Addition : Result.Additions)
	{
		TestTrue(TEXT("every addition names what forced it"), !Addition.ForcedByKey.IsEmpty());
		TestTrue(TEXT("every addition names a unit the reader ticked"),
			Result.SelectedKeys.Contains(Addition.ForcedByKey));
		TestEqual(TEXT("the forcing unit is the ticked trigger"), Addition.ForcedByKey, Trigger->IdentityKey());
		TestTrue(TEXT("every addition says why in a sentence"), !Addition.Because.IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyClosureIsIdempotentTest,
	"CrowdySDK.CrowdyStudio.ApplyClosureIsIdempotent", CrowdyApplySelectionTestFlags)

bool FCrowdyApplyClosureIsIdempotentTest::RunTest(const FString& Parameters)
{
	// One function reached from two ticked automations, so the closure meets it twice. It must enter the set once.
	FApplyTestPlan Plan;
	Plan.Functions.Add(ApplyTestFunction(TEXT("Goblin"), TEXT("regen"), true));
	Plan.Automations.Add(ApplyTestAutomation(TEXT("goblin_tick"), TEXT("regen"), FString(), true));
	Plan.Automations.Add(ApplyTestAutomation(TEXT("goblin_slow_tick"), TEXT("regen"), FString(), true));

	const TArray<FCrowdyApplyUnit> Units = CrowdyApplySelection::BuildUnits(Plan.Input());
	TArray<FString> Selected;
	Selected.Add(ApplyTestKeyOf(Units, ECrowdyApplyKind::Automation, FString(), TEXT("goblin_tick")));
	Selected.Add(ApplyTestKeyOf(Units, ECrowdyApplyKind::Automation, FString(), TEXT("goblin_slow_tick")));

	const FCrowdyApplyPlan First = CrowdyApplySelection::BuildPlan(Plan.Input(), Selected);
	TestEqual(TEXT("three units are sent"), First.ClosedSet.Num(), 3);
	TestEqual(TEXT("the shared function is added once"), First.Additions.Num(), 1);

	TArray<FString> AdditionKeys;
	for (const FCrowdyApplyAddition& Addition : First.Additions)
	{
		TestFalse(TEXT("no unit is added twice"), AdditionKeys.Contains(Addition.Unit.IdentityKey()));
		AdditionKeys.Add(Addition.Unit.IdentityKey());
	}

	// Closing an already-closed set changes nothing at all.
	const FCrowdyApplyPlan Second = CrowdyApplySelection::BuildPlan(Plan.Input(), ApplyTestAllKeys(First.ClosedSet));
	TestEqual(TEXT("the second pass sends the same number of units"), Second.ClosedSet.Num(), First.ClosedSet.Num());
	TestEqual(TEXT("the second pass adds nothing"), Second.Additions.Num(), 0);
	for (int32 Index = 0; Index < First.ClosedSet.Num() && Index < Second.ClosedSet.Num(); ++Index)
	{
		TestEqual(TEXT("the order is unchanged"),
			Second.ClosedSet[Index].IdentityKey(), First.ClosedSet[Index].IdentityKey());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyClosureTerminatesOnAFunctionCycleTest,
	"CrowdySDK.CrowdyStudio.ApplyClosureTerminatesOnAFunctionCycle", CrowdyApplySelectionTestFlags)

bool FCrowdyApplyClosureTerminatesOnAFunctionCycleTest::RunTest(const FString& Parameters)
{
	// Two functions that call each other. The prerequisite graph is cyclic, so a walk that recursed instead of
	// checking membership would never come back.
	FApplyTestPlan Plan;
	FCrowdySchemaFunctionUpsert First = ApplyTestFunction(TEXT("Knight"), TEXT("ping"), true);
	First.Function.ReturnExpression = TEXT("fn:pong(self)");
	Plan.Functions.Add(MoveTemp(First));

	FCrowdySchemaFunctionUpsert Second = ApplyTestFunction(TEXT("Knight"), TEXT("pong"), true);
	Second.Function.ReturnExpression = TEXT("fn:ping(self)");
	Plan.Functions.Add(MoveTemp(Second));

	const TArray<FCrowdyApplyUnit> Units = CrowdyApplySelection::BuildUnits(Plan.Input());
	TArray<FString> Selected;
	Selected.Add(ApplyTestKeyOf(Units, ECrowdyApplyKind::Function, TEXT("Knight"), TEXT("ping")));

	const FCrowdyApplyPlan Result = CrowdyApplySelection::BuildPlan(Plan.Input(), Selected);

	TestEqual(TEXT("both halves of the cycle are sent, once each"), Result.ClosedSet.Num(), 2);
	TestEqual(TEXT("only the other half is an addition"), Result.Additions.Num(), 1);
	TestTrue(TEXT("the callee is sent"),
		ApplyTestHasUnit(Result.ClosedSet, ECrowdyApplyKind::Function, TEXT("Knight"), TEXT("pong")));
	TestTrue(TEXT("a cycle is not a refusal"), Result.Refusal == ECrowdyApplyRefusal::None);
	TestTrue(TEXT("the plan can be sent"), Result.bSendable);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyClosureTreatsASelfReferenceAsSatisfiedTest,
	"CrowdySDK.CrowdyStudio.ApplyClosureTreatsASelfReferenceAsSatisfied", CrowdyApplySelectionTestFlags)

bool FCrowdyApplyClosureTreatsASelfReferenceAsSatisfiedTest::RunTest(const FString& Parameters)
{
	// A function that re-arms itself names itself as a prerequisite. It is already in the set, so it is satisfied,
	// never missing and never added a second time.
	FApplyTestPlan Plan;
	FCrowdySchemaFunctionUpsert Ticker = ApplyTestFunction(TEXT("Knight"), TEXT("bleed_tick"), true);
	ApplyTestAddTimer(Ticker, TEXT("bleed_tick"), TEXT("1000"));
	Ticker.Function.ReturnExpression = TEXT("fn:bleed_tick(self)");
	Plan.Functions.Add(MoveTemp(Ticker));

	// The self-edge is really there: the scan reports the function naming itself, and the closure still answers
	// "already satisfied" rather than "missing".
	const TArray<FString> Callees = CrowdyApplySelection::FunctionCallees(Plan.Functions[0].Function);
	TestEqual(TEXT("the function names itself"), Callees.Num(), 1);
	if (Callees.Num() == 1)
	{
		TestEqual(TEXT("the callee is the function itself"), Callees[0], FString(TEXT("bleed_tick")));
	}

	const TArray<FCrowdyApplyUnit> Units = CrowdyApplySelection::BuildUnits(Plan.Input());
	TArray<FString> Selected;
	Selected.Add(ApplyTestKeyOf(Units, ECrowdyApplyKind::Function, TEXT("Knight"), TEXT("bleed_tick")));

	const FCrowdyApplyPlan Result = CrowdyApplySelection::BuildPlan(Plan.Input(), Selected);

	TestEqual(TEXT("the function is sent once"), Result.ClosedSet.Num(), 1);
	TestEqual(TEXT("a self-reference adds nothing"), Result.Additions.Num(), 0);
	TestEqual(TEXT("a self-reference is not unresolvable"), Result.Unresolvable.Num(), 0);
	TestTrue(TEXT("the plan can be sent"), Result.bSendable);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyClosurePullsTheSdkWiringWithItsModelTest,
	"CrowdySDK.CrowdyStudio.ApplyClosurePullsTheSdkWiringWithItsModel", CrowdyApplySelectionTestFlags)

bool FCrowdyApplyClosurePullsTheSdkWiringWithItsModelTest::RunTest(const FString& Parameters)
{
	const FString TouchName = CrowdyGameModelMetaKeys::CollectionTouchFunctionName(TEXT("Knight"));

	FApplyTestPlan Plan;
	Plan.Props.Add(ApplyTestProp(TEXT("Knight"), TEXT("hp"), true));
	Plan.Props.Add(ApplyTestProp(TEXT("Knight"), CrowdyGameModelMetaKeys::CollectionRevKey, true));
	Plan.Props.Add(ApplyTestProp(TEXT("Goblin"), CrowdyGameModelMetaKeys::CollectionRevKey, true));
	Plan.Functions.Add(ApplyTestFunction(TEXT("Knight"), TouchName, true));

	const TArray<FCrowdyApplyUnit> Units = CrowdyApplySelection::BuildUnits(Plan.Input());
	const TArray<FCrowdyApplyUnit> Selectable = CrowdyApplySelection::SelectableUnits(Units);

	TestEqual(TEXT("the SDK's own wiring is never offered as a row"), Selectable.Num(), 1);
	TestTrue(TEXT("only the designer's attribute is offered"),
		ApplyTestHasUnit(Selectable, ECrowdyApplyKind::Attribute, TEXT("Knight"), TEXT("hp")));

	TArray<FString> Selected;
	Selected.Add(ApplyTestKeyOf(Units, ECrowdyApplyKind::Attribute, TEXT("Knight"), TEXT("hp")));
	const FCrowdyApplyPlan Result = CrowdyApplySelection::BuildPlan(Plan.Input(), Selected);

	const FCrowdyApplyUnit* Rev = ApplyTestFindUnit(
		Result.ClosedSet, ECrowdyApplyKind::Attribute, TEXT("Knight"), CrowdyGameModelMetaKeys::CollectionRevKey);
	const FCrowdyApplyUnit* Touch =
		ApplyTestFindUnit(Result.ClosedSet, ECrowdyApplyKind::Function, TEXT("Knight"), TouchName);

	TestTrue(TEXT("the revision attribute goes with its model"), Rev != nullptr);
	TestTrue(TEXT("the touch function goes with its model"), Touch != nullptr);
	if (Rev && Touch)
	{
		TestTrue(TEXT("the revision attribute is reserved"), Rev->bReserved);
		TestTrue(TEXT("the touch function is reserved"), Touch->bReserved);
	}
	TestFalse(TEXT("another model's wiring is left alone"),
		ApplyTestHasUnit(
			Result.ClosedSet, ECrowdyApplyKind::Attribute, TEXT("Goblin"), CrowdyGameModelMetaKeys::CollectionRevKey));
	TestEqual(TEXT("wiring is never listed as an addition"), Result.Additions.Num(), 0);
	TestEqual(TEXT("the sheet counts two reserved units"), Result.CountReserved(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyOrderMatchesTheFullApplyByteForByteTest,
	"CrowdySDK.CrowdyStudio.ApplyOrderMatchesTheFullApplyByteForByte", CrowdyApplySelectionTestFlags)

bool FCrowdyApplyOrderMatchesTheFullApplyByteForByteTest::RunTest(const FString& Parameters)
{
	FApplyTestPlan Plan;
	Plan.Types.Add(ApplyTestType(TEXT("Knight"), true));
	Plan.Types.Add(ApplyTestType(TEXT("Goblin"), true));
	Plan.Props.Add(ApplyTestProp(TEXT("Knight"), TEXT("hp"), true));
	Plan.Props.Add(ApplyTestProp(TEXT("Goblin"), TEXT("hp"), true));
	Plan.Props.Add(ApplyTestProp(TEXT("Knight"), TEXT("mana"), true));

	FCrowdySchemaFunctionUpsert Function = ApplyTestFunction(TEXT("Knight"), TEXT("take_damage"), true);
	ApplyTestAddMutation(Function, TEXT("self"), TEXT("hp"), TEXT("self.hp - 1"));
	Plan.Functions.Add(MoveTemp(Function));
	Plan.Functions.Add(ApplyTestFunction(TEXT("Goblin"), TEXT("regen"), true));

	Plan.Automations.Add(ApplyTestAutomation(TEXT("goblin_tick"), TEXT("regen"), TEXT("Goblin"), true));
	Plan.Triggers.Add(ApplyTestTrigger(TEXT("goblin_tick"), TEXT("property_changed"), TEXT("hp"), true));

	const TArray<FCrowdyApplyUnit> Units = CrowdyApplySelection::BuildUnits(Plan.Input());
	const FCrowdyApplyPlan Result = CrowdyApplySelection::BuildPlan(Plan.Input(), ApplyTestAllKeys(Units));

	TestEqual(TEXT("everything selected sends everything"), Result.ClosedSet.Num(), Units.Num());

	const TArray<FApplyTestOp> Unfiltered = ApplyTestUnfilteredOps(Plan);
	const TArray<FApplyTestOp> Selective = ApplyTestSelectiveOps(Plan, Result.ClosedSet);

	TestEqual(TEXT("the two op lists are the same length"), Selective.Num(), Unfiltered.Num());
	if (Selective.Num() != Unfiltered.Num())
	{
		return false;
	}

	for (int32 Index = 0; Index < Unfiltered.Num(); ++Index)
	{
		TestEqual(
			FString::Printf(TEXT("op %d has the same operation name"), Index),
			Selective[Index].OperationName, Unfiltered[Index].OperationName);
		TestEqual(
			FString::Printf(TEXT("op %d has the same variables"), Index),
			Selective[Index].Variables, Unfiltered[Index].Variables);
	}

	// The list itself, stated once so a reordering is visible here as well as in the ops above.
	const TArray<ECrowdyApplyKind>& Order = CrowdyApplySelection::ApplyOrder();
	TestEqual(TEXT("the apply order has five kinds"), Order.Num(), 5);
	if (Order.Num() == 5)
	{
		TestTrue(TEXT("models first"), Order[0] == ECrowdyApplyKind::Type);
		TestTrue(TEXT("attributes second"), Order[1] == ECrowdyApplyKind::Attribute);
		TestTrue(TEXT("functions third"), Order[2] == ECrowdyApplyKind::Function);
		TestTrue(TEXT("automations fourth"), Order[3] == ECrowdyApplyKind::Automation);
		TestTrue(TEXT("triggers last"), Order[4] == ECrowdyApplyKind::Trigger);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyOrderWithinAKindKeepsThePlanOrderTest,
	"CrowdySDK.CrowdyStudio.ApplyOrderWithinAKindKeepsThePlanOrder", CrowdyApplySelectionTestFlags)

bool FCrowdyApplyOrderWithinAKindKeepsThePlanOrderTest::RunTest(const FString& Parameters)
{
	// Names deliberately out of alphabetical order, so a set that had been sorted by name would read differently
	// from one that kept the plan's own order.
	FApplyTestPlan Plan;
	Plan.Props.Add(ApplyTestProp(TEXT("Knight"), TEXT("zeal"), true));
	Plan.Props.Add(ApplyTestProp(TEXT("Knight"), TEXT("armour"), true));
	Plan.Props.Add(ApplyTestProp(TEXT("Knight"), TEXT("mana"), true));

	const TArray<FCrowdyApplyUnit> Units = CrowdyApplySelection::BuildUnits(Plan.Input());
	const FCrowdyApplyPlan Result = CrowdyApplySelection::BuildPlan(Plan.Input(), ApplyTestAllKeys(Units));

	TestEqual(TEXT("all three attributes are sent"), ApplyTestCountKind(Result.ClosedSet, ECrowdyApplyKind::Attribute), 3);

	int32 Previous = INDEX_NONE;
	for (const FCrowdyApplyUnit& Unit : Result.ClosedSet)
	{
		if (Unit.Kind != ECrowdyApplyKind::Attribute)
		{
			continue;
		}
		TestTrue(TEXT("attributes stay in ascending plan order"), Unit.PlanIndex > Previous);
		Previous = Unit.PlanIndex;
	}

	if (Result.ClosedSet.Num() > 0)
	{
		TestEqual(TEXT("the first sent attribute is the plan's first"), Result.ClosedSet[0].Name, FString(TEXT("zeal")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplySelectionSurvivesAReplanByIdentityTest,
	"CrowdySDK.CrowdyStudio.ApplySelectionSurvivesAReplanByIdentity", CrowdyApplySelectionTestFlags)

bool FCrowdyApplySelectionSurvivesAReplanByIdentityTest::RunTest(const FString& Parameters)
{
	FApplyTestPlan Before;
	Before.Props.Add(ApplyTestProp(TEXT("Knight"), TEXT("hp"), true));
	Before.Props.Add(ApplyTestProp(TEXT("Knight"), TEXT("mana"), true));
	Before.Props.Add(ApplyTestProp(TEXT("Goblin"), TEXT("hp"), true));

	const TArray<FCrowdyApplyUnit> OldUnits = CrowdyApplySelection::BuildUnits(Before.Input());
	TArray<FString> Selected;
	Selected.Add(ApplyTestKeyOf(OldUnits, ECrowdyApplyKind::Attribute, TEXT("Knight"), TEXT("mana")));
	Selected.Add(ApplyTestKeyOf(OldUnits, ECrowdyApplyKind::Attribute, TEXT("Goblin"), TEXT("hp")));

	// The re-plan rebuilds every array: mana is gone, Goblin's hp moved to a different index, and a new entry
	// arrived in front of both.
	FApplyTestPlan After;
	After.Props.Add(ApplyTestProp(TEXT("Goblin"), TEXT("mana"), true));
	After.Props.Add(ApplyTestProp(TEXT("Goblin"), TEXT("hp"), true));
	After.Props.Add(ApplyTestProp(TEXT("Knight"), TEXT("hp"), true));

	const TArray<FCrowdyApplyUnit> NewUnits = CrowdyApplySelection::BuildUnits(After.Input());
	TArray<FString> Dropped;
	const TArray<FString> Kept = CrowdyApplySelection::ReconcileSelection(Selected, NewUnits, Dropped);

	TestEqual(TEXT("one key survived"), Kept.Num(), 1);
	TestEqual(TEXT("one key went"), Dropped.Num(), 1);
	if (Kept.Num() == 1)
	{
		TestEqual(TEXT("the surviving key is the one the new plan still holds"),
			Kept[0], ApplyTestKeyOf(NewUnits, ECrowdyApplyKind::Attribute, TEXT("Goblin"), TEXT("hp")));
	}
	if (Dropped.Num() == 1)
	{
		TestEqual(TEXT("the dropped key is the one that vanished"),
			Dropped[0], ApplyTestKeyOf(OldUnits, ECrowdyApplyKind::Attribute, TEXT("Knight"), TEXT("mana")));
	}

	// Nothing was mapped onto a neighbour: the key that survived still names Goblin's hp, not the entry that took
	// its old position.
	const FCrowdyApplyPlan Result = CrowdyApplySelection::BuildPlan(After.Input(), Selected);
	TestEqual(TEXT("only the surviving unit is sent"), Result.ClosedSet.Num(), 1);
	TestTrue(TEXT("the surviving unit is Goblin's hp"),
		ApplyTestHasUnit(Result.ClosedSet, ECrowdyApplyKind::Attribute, TEXT("Goblin"), TEXT("hp")));
	TestEqual(TEXT("the dropped key is reported on the plan"), Result.DroppedKeys.Num(), 1);
	TestTrue(TEXT("a partial drop is not a refusal"), Result.Refusal == ECrowdyApplyRefusal::None);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
