// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Customizations/CrowdyEffectGraphNodeCustomizations.h"
#include "EdGraph/EdGraphPin.h"
#include "GameModel/CrowdyEffectDuplicateFunctionIndex.h"
#include "Graph/CrowdyEffectGraph.h"
#include "Graph/CrowdyEffectGraphNodeOptions.h"
#include "Graph/CrowdyEffectGraphNodes.h"
#include "Graph/CrowdyEffectGraphSchema.h"
#include "Graph/CrowdyEffectGraphTestContainer.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"
#include "Replication/GameModel/Effect/CrowdyEffectSpec.h"
#include "UObject/Package.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyGraphAuthoringTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	UCrowdyEffect* MakeEffectWithContainer()
	{
		UCrowdyEffect* Effect = NewObject<UCrowdyEffect>(GetTransientPackage());
		Effect->ContainerClass = UCrowdyEffectGraphTestContainer::StaticClass();
		return Effect;
	}

	CrowdyEffectFunctionCatalog::FDeclaredFunction MakeDeclaredFunction(
		const FString& FunctionName, const FString& ContainerTypeName, bool bAuthorsReturn,
		const FString& ReturnType = FString())
	{
		CrowdyEffectFunctionCatalog::FDeclaredFunction Declared;
		Declared.FunctionName = FunctionName;
		Declared.ContainerTypeName = ContainerTypeName;
		Declared.bAuthorsReturn = bAuthorsReturn;
		Declared.ReturnType = ReturnType;
		Declared.InvokeScope = TEXT("player");
		Declared.AssetPath = TEXT("/Test/") + FunctionName;
		return Declared;
	}

	// Installs a stubbed fn: callee catalog for the duration of a test and puts the real one back on the way out,
	// including on an early return. A picker test that consulted the real catalog would sweep and load every Crowdy
	// Effect asset in the project, which is both slow and contagious: any error an unrelated asset logs while
	// loading fails whichever test happened to trigger the sweep. Pass nullptr for "no catalog at all".
	struct FScopedCatalogHook
	{
		explicit FScopedCatalogHook(CrowdyEffectFunctionCatalog::FHook Hook)
		{
			CrowdyEffectFunctionCatalog::SetCatalogHook(MoveTemp(Hook));
		}

		~FScopedCatalogHook()
		{
			// Nothing reads back the installed hook, so the production one is restored by re-running the index's own
			// registration, which is idempotent and rebinds its asset-registry listeners with it.
			CrowdyEffectDuplicateFunctionIndex::Unregister();
			CrowdyEffectDuplicateFunctionIndex::Register();
		}
	};

	// A Result node inside a real graph, so pin allocation and node placement behave as they do in the editor.
	UCrowdyEffectGraphNode_Result* MakeAuthoringResultNode()
	{
		UCrowdyEffectGraph* Graph = NewObject<UCrowdyEffectGraph>(MakeEffectWithContainer());
		Graph->Schema = UCrowdyEffectGraphSchema::StaticClass();

		UCrowdyEffectGraphNode_Result* Result = NewObject<UCrowdyEffectGraphNode_Result>(Graph);
		Result->CreateNewGuid();
		Graph->Nodes.Add(Result);
		Result->NodePosX = 400;
		Result->AllocateDefaultPins();
		return Result;
	}

	FCrowdyEffectGraphWrite MakeAuthoringWrite(ECrowdyEffectRole Role, const FString& Attribute)
	{
		FCrowdyEffectGraphWrite Write;
		Write.TargetRole = Role;
		Write.Attribute = Attribute;
		Write.Op = ECrowdyEffectAssignmentOp::Add;
		return Write;
	}
}

// The pick-list option sources: container attribute keys, declared magnitudes, builtins, and their null / empty
// safety. These back every graph-node picker, so they are the load-bearing part to prove headlessly (the Slate
// combos themselves are an in-editor gate).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGraphNodeOptionSourcesTest,
	"CrowdySDK.Editor.GraphNodeOptionSources", CrowdyGraphAuthoringTestFlags)
bool FCrowdyGraphNodeOptionSourcesTest::RunTest(const FString&)
{
	// Attribute options resolve the effect's container class to its Server Owned attribute keys.
	{
		UCrowdyEffect* Effect = MakeEffectWithContainer();
		const TArray<FString> Attrs = CrowdyEffectGraphNodeOptions::AttributeOptions(Effect);
		TestTrue(TEXT("container attribute key 'health' is offered"), Attrs.Contains(TEXT("health")));
	}

	// No effect / no container class yields no attribute suggestions (the picker stays free-text, never locked).
	{
		TestEqual(TEXT("null effect offers no attributes"),
			CrowdyEffectGraphNodeOptions::AttributeOptions(nullptr).Num(), 0);

		UCrowdyEffect* Empty = NewObject<UCrowdyEffect>(GetTransientPackage());
		TestEqual(TEXT("effect with no container offers no attributes"),
			CrowdyEffectGraphNodeOptions::AttributeOptions(Empty).Num(), 0);
	}

	// Magnitude options are the effect's declared $params, blank names skipped.
	{
		UCrowdyEffect* Effect = NewObject<UCrowdyEffect>(GetTransientPackage());
		FCrowdyEffectMagnitude Power;
		Power.Name = TEXT("power");
		Effect->Magnitudes.Add(Power);
		FCrowdyEffectMagnitude Blank;
		Blank.Name = TEXT("  ");
		Effect->Magnitudes.Add(Blank);

		const TArray<FString> Mags = CrowdyEffectGraphNodeOptions::MagnitudeOptions(Effect);
		TestTrue(TEXT("declared magnitude 'power' is offered"), Mags.Contains(TEXT("power")));
		TestFalse(TEXT("a blank magnitude name is not offered"), Mags.Contains(TEXT("  ")));
	}

	// Builtins are the full documented catalog, not the narrower syntax-highlight subset: the common null-guard
	// 'coalesce' and 'len' must be offered even though they are not in CrowdyExpressionEditorModel::BuiltinNames.
	{
		const TArray<FString> Builtins = CrowdyEffectGraphNodeOptions::BuiltinCallOptions();
		TestTrue(TEXT("'clamp' is a builtin option"), Builtins.Contains(TEXT("clamp")));
		TestTrue(TEXT("'coalesce' is a builtin option"), Builtins.Contains(TEXT("coalesce")));
		TestTrue(TEXT("'len' is a builtin option"), Builtins.Contains(TEXT("len")));
		TestTrue(TEXT("'to_string' is a builtin option"), Builtins.Contains(TEXT("to_string")));
	}

	return true;
}

// The catalog-derived part of fn: call options: which other effects' returning functions are offered on the
// owning effect's own container type, and which are correctly excluded (no return authored, a different container
// type, or the effect naming itself). Driven directly through CrowdyEffectFunctionCatalog::SetCatalogHook with a
// stub rather than a real project-wide sweep, so the test is hermetic and never depends on what Crowdy Effect
// assets happen to exist in the project. Restores the real catalog hook on exit, including on an early return, so
// a failing assertion here cannot leak a stub into a later test.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGraphNodeFnCallCatalogTest,
	"CrowdySDK.Editor.GraphNodeFnCallCatalog", CrowdyGraphAuthoringTestFlags)
bool FCrowdyGraphNodeFnCallCatalogTest::RunTest(const FString&)
{
	// A null effect offers no options, catalog or not.
	TestEqual(TEXT("a null effect offers no fn: options"),
		CrowdyEffectGraphNodeOptions::FnCallOptions(nullptr).Num(), 0);

	// With no catalog hook registered at all, the function offers nothing and does not crash: IsCatalogAvailable()
	// is what gates the catalog-derived half.
	{
		const FScopedCatalogHook NoCatalog(nullptr);

		UCrowdyEffect* Effect = MakeEffectWithContainer();
		const TArray<FString> Names = CrowdyEffectGraphNodeOptions::FnCallOptions(Effect);
		TestEqual(TEXT("with no catalog hook nothing is offered"), Names.Num(), 0);
	}

	// The test fixture's container class carries the CrowdyContainer tag "GraphAssetTestHero" (see
	// CrowdyEffectGraphTestContainer.h). The stub below deliberately ignores the queried container type and always
	// returns the full mixed-type list, so it is FnCallOptions' own container-type check, not the hook's scoping,
	// that has to exclude the cross-type entry.
	const FScopedCatalogHook StubCatalog(
		[](const FString&) -> TArray<CrowdyEffectFunctionCatalog::FDeclaredFunction>
		{
			TArray<CrowdyEffectFunctionCatalog::FDeclaredFunction> Out;
			Out.Add(MakeDeclaredFunction(TEXT("compute_bonus_from_ally"), TEXT("GraphAssetTestHero"), true, TEXT("float")));
			Out.Add(MakeDeclaredFunction(TEXT("bare_attribute_return"), TEXT("GraphAssetTestHero"), true, FString()));
			Out.Add(MakeDeclaredFunction(TEXT("side_effect_only"), TEXT("GraphAssetTestHero"), false, TEXT("int")));
			Out.Add(MakeDeclaredFunction(TEXT("own_function"), TEXT("GraphAssetTestHero"), true, TEXT("int")));
			Out.Add(MakeDeclaredFunction(TEXT("cross_type_return"), TEXT("SomeOtherContainerType"), true, TEXT("int")));

			// An effect that both writes and returns is still a legal callee: the picker offers it, and the
			// lowering is what warns that its writes will not run at the call site.
			CrowdyEffectFunctionCatalog::FDeclaredFunction Mutating =
				MakeDeclaredFunction(TEXT("mutate_and_return"), TEXT("GraphAssetTestHero"), true, TEXT("int"));
			Mutating.bHasMutations = true;
			Out.Add(Mutating);
			return Out;
		});

	UCrowdyEffect* Effect = MakeEffectWithContainer();
	Effect->FunctionName = TEXT("own_function");

	const TArray<FString> Names = CrowdyEffectGraphNodeOptions::FnCallOptions(Effect);

	TestTrue(TEXT("a returning effect on the same container type is offered"),
		Names.Contains(TEXT("compute_bonus_from_ally")));
	TestTrue(TEXT("an effect that authors a return but declares no return type is offered"),
		Names.Contains(TEXT("bare_attribute_return")));
	TestFalse(TEXT("an effect that returns nothing is not offered"),
		Names.Contains(TEXT("side_effect_only")));
	TestFalse(TEXT("an effect on a different container type is not offered"),
		Names.Contains(TEXT("cross_type_return")));
	TestFalse(TEXT("the owning effect's own function name is not offered to itself"),
		Names.Contains(TEXT("own_function")));
	TestTrue(TEXT("an effect that writes and returns is still offered; the lowering warns about the writes"),
		Names.Contains(TEXT("mutate_and_return")));

	// An effect with no container class resolves to an empty container type name, so it must skip the catalog
	// query entirely rather than ask with an empty key. This test's stub ignores the queried type and always
	// returns its fixed list, so a leak here would show up as those names being offered anyway.
	{
		UCrowdyEffect* NoContainerEffect = NewObject<UCrowdyEffect>(GetTransientPackage());
		const TArray<FString> NoContainerNames = CrowdyEffectGraphNodeOptions::FnCallOptions(NoContainerEffect);
		TestEqual(TEXT("an effect with no container class offers nothing, even with a catalog registered"),
			NoContainerNames.Num(), 0);
	}

	return true;
}

// OwningEffect walks a node's outer chain (node -> graph -> effect), the resolution every picker relies on to reach
// the container class and magnitudes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGraphNodeOwningEffectTest,
	"CrowdySDK.Editor.GraphNodeOwningEffect", CrowdyGraphAuthoringTestFlags)
bool FCrowdyGraphNodeOwningEffectTest::RunTest(const FString&)
{
	UCrowdyEffect* Effect = MakeEffectWithContainer();
	UCrowdyEffectGraph* Graph = NewObject<UCrowdyEffectGraph>(Effect);
	UCrowdyEffectGraphNode_Attribute* Node = NewObject<UCrowdyEffectGraphNode_Attribute>(Graph);

	TestEqual(TEXT("OwningEffect resolves the node's owning effect"),
		CrowdyEffectGraphNodeOptions::OwningEffect(Node), static_cast<const UCrowdyEffect*>(Effect));

	UCrowdyEffectGraphNode_Attribute* Orphan = NewObject<UCrowdyEffectGraphNode_Attribute>(GetTransientPackage());
	TestNull(TEXT("a node with no owning effect resolves null"),
		CrowdyEffectGraphNodeOptions::OwningEffect(Orphan));
	return true;
}

// The Result node's write / condition pins read as plain instructions, not operator glyphs.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGraphResultPinLabelsTest,
	"CrowdySDK.Editor.GraphResultPinLabels", CrowdyGraphAuthoringTestFlags)
bool FCrowdyGraphResultPinLabelsTest::RunTest(const FString&)
{
	UCrowdyEffectGraph* Graph = NewObject<UCrowdyEffectGraph>(GetTransientPackage());
	UCrowdyEffectGraphNode_Result* Result = NewObject<UCrowdyEffectGraphNode_Result>(Graph);

	FCrowdyEffectGraphWrite AddHp;
	AddHp.TargetRole = ECrowdyEffectRole::Target;
	AddHp.Attribute = TEXT("hp");
	AddHp.Op = ECrowdyEffectAssignmentOp::Add;
	Result->Writes.Add(AddHp);

	FCrowdyEffectGraphWrite SetSourceMana;
	SetSourceMana.TargetRole = ECrowdyEffectRole::Source;
	SetSourceMana.Attribute = TEXT("mana");
	SetSourceMana.Op = ECrowdyEffectAssignmentOp::Set;
	Result->Writes.Add(SetSourceMana);

	FCrowdyEffectGraphRequire PlainCondition;
	Result->Requires.Add(PlainCondition);

	FCrowdyEffectGraphRequire NotedCondition;
	NotedCondition.Note = TEXT("target is alive");
	Result->Requires.Add(NotedCondition);

	Result->AllocateDefaultPins();

	const UEdGraphPin* Write0 = Result->FindPin(UCrowdyEffectGraphNode_Result::WritePinName(0), EGPD_Input);
	const UEdGraphPin* Write1 = Result->FindPin(UCrowdyEffectGraphNode_Result::WritePinName(1), EGPD_Input);
	const UEdGraphPin* Require0 = Result->FindPin(UCrowdyEffectGraphNode_Result::RequirePinName(0), EGPD_Input);
	const UEdGraphPin* Require1 = Result->FindPin(UCrowdyEffectGraphNode_Result::RequirePinName(1), EGPD_Input);

	if (!Write0 || !Write1 || !Require0 || !Require1)
	{
		AddError(TEXT("expected write and condition pins were not created"));
		return false;
	}

	TestEqual(TEXT("Target Add write reads as 'Add to hp'"),
		Write0->PinFriendlyName.ToString(), FString(TEXT("Add to hp")));
	TestEqual(TEXT("Source Set write reads as 'Set Source's mana'"),
		Write1->PinFriendlyName.ToString(), FString(TEXT("Set Source's mana")));
	TestEqual(TEXT("a note-less condition pin reads as 'Only if...'"),
		Require0->PinFriendlyName.ToString(), FString(TEXT("Only if...")));
	TestEqual(TEXT("a noted condition pin reads as 'Only if: <note>'"),
		Require1->PinFriendlyName.ToString(), FString(TEXT("Only if: target is alive")));
	return true;
}

// When the one-click return shortcut is offered. It answers with the single write it would read back, and stands
// down whenever that write is ambiguous, unnamed, or the effect already answers with something.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGraphReturnDefaultAvailabilityTest,
	"CrowdySDK.Editor.GraphReturnDefaultAvailability", CrowdyGraphAuthoringTestFlags)
bool FCrowdyGraphReturnDefaultAvailabilityTest::RunTest(const FString&)
{
	// Exactly one named write: offered, naming that write's role and attribute (trimmed).
	{
		UCrowdyEffectGraphNode_Result* Result = MakeAuthoringResultNode();
		Result->Writes.Add(MakeAuthoringWrite(ECrowdyEffectRole::Source, TEXT("  mana  ")));

		const CrowdyEffectReturnDefault::FPlan Planned = CrowdyEffectReturnDefault::Plan(Result);
		TestTrue(TEXT("one named write offers the shortcut"), Planned.bAvailable);
		TestEqual(TEXT("the shortcut reads back the write's attribute"), Planned.Attribute, FString(TEXT("mana")));
		TestEqual(TEXT("the shortcut keeps the write's role"),
			static_cast<int32>(Planned.Role), static_cast<int32>(ECrowdyEffectRole::Source));
		TestTrue(TEXT("the shortcut names the attribute to the author"),
			Planned.Message.ToString().Contains(TEXT("mana")));
	}

	// No writes at all: there is nothing to read back.
	{
		UCrowdyEffectGraphNode_Result* Result = MakeAuthoringResultNode();
		TestFalse(TEXT("no writes does not offer the shortcut"),
			CrowdyEffectReturnDefault::Plan(Result).bAvailable);
	}

	// Two writes: which one the caller wants is the author's decision.
	{
		UCrowdyEffectGraphNode_Result* Result = MakeAuthoringResultNode();
		Result->Writes.Add(MakeAuthoringWrite(ECrowdyEffectRole::Target, TEXT("hp")));
		Result->Writes.Add(MakeAuthoringWrite(ECrowdyEffectRole::Target, TEXT("shield")));
		TestFalse(TEXT("two writes do not offer the shortcut"),
			CrowdyEffectReturnDefault::Plan(Result).bAvailable);
	}

	// A write with no attribute named yet has nothing to point at.
	{
		UCrowdyEffectGraphNode_Result* Result = MakeAuthoringResultNode();
		Result->Writes.Add(MakeAuthoringWrite(ECrowdyEffectRole::Target, TEXT("   ")));
		TestFalse(TEXT("a blank attribute does not offer the shortcut"),
			CrowdyEffectReturnDefault::Plan(Result).bAvailable);
	}

	// Turning the return on without wiring anything is the state an author reaches by ticking the checkbox first.
	// Nothing is returned yet and the graph does not compile, so the shortcut has to still be on offer.
	{
		UCrowdyEffectGraphNode_Result* Result = MakeAuthoringResultNode();
		Result->Writes.Add(MakeAuthoringWrite(ECrowdyEffectRole::Target, TEXT("hp")));
		Result->bReturnsValue = true;
		Result->ReconstructNode();
		TestTrue(TEXT("a return that is switched on but unwired still offers the shortcut"),
			CrowdyEffectReturnDefault::Plan(Result).bAvailable);

		// And applying it from that state authors the return rather than refusing.
		TestTrue(TEXT("the shortcut applies from a switched-on but unwired return"),
			CrowdyEffectReturnDefault::Apply(Result));
	}

	// Already returning something: the shortcut would overwrite an answer the author chose.
	{
		UCrowdyEffectGraphNode_Result* Result = MakeAuthoringResultNode();
		Result->Writes.Add(MakeAuthoringWrite(ECrowdyEffectRole::Target, TEXT("hp")));
		TestTrue(TEXT("the shortcut applies once"), CrowdyEffectReturnDefault::Apply(Result));
		TestFalse(TEXT("an effect that already returns a wired value does not offer the shortcut"),
			CrowdyEffectReturnDefault::Plan(Result).bAvailable);
	}

	// No node selected at all.
	{
		TestFalse(TEXT("a null node does not offer the shortcut"),
			CrowdyEffectReturnDefault::Plan(nullptr).bAvailable);
	}

	return true;
}

// Applying the shortcut: the Return pin has to exist before anything can be wired into it, so the node is rebuilt
// between turning the return on and finding the pin. The wire, the spawned read, and its placement are what the
// author sees, so all three are pinned here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGraphReturnDefaultApplyTest,
	"CrowdySDK.Editor.GraphReturnDefaultApply", CrowdyGraphAuthoringTestFlags)
bool FCrowdyGraphReturnDefaultApplyTest::RunTest(const FString&)
{
	UCrowdyEffectGraphNode_Result* Result = MakeAuthoringResultNode();
	Result->Writes.Add(MakeAuthoringWrite(ECrowdyEffectRole::Source, TEXT("  mana  ")));
	Result->ReconstructNode();

	TestNull(TEXT("no Return pin exists before the shortcut runs"),
		Result->FindPin(UCrowdyEffectGraphNode_Result::ReturnPinName(), EGPD_Input));

	TestTrue(TEXT("the shortcut applies"), CrowdyEffectReturnDefault::Apply(Result));
	TestTrue(TEXT("the node now returns a value"), Result->bReturnsValue);
	TestTrue(TEXT("the Return pin has a stable identity"), Result->ReturnPinId.IsValid());

	UEdGraphPin* ReturnPin = Result->FindPin(UCrowdyEffectGraphNode_Result::ReturnPinName(), EGPD_Input);
	if (!ReturnPin)
	{
		AddError(TEXT("the Return pin was not created"));
		return false;
	}
	if (ReturnPin->LinkedTo.Num() != 1 || !ReturnPin->LinkedTo[0])
	{
		AddError(TEXT("the Return pin was not wired to exactly one value"));
		return false;
	}

	UCrowdyEffectGraphNode_Attribute* Read =
		Cast<UCrowdyEffectGraphNode_Attribute>(ReturnPin->LinkedTo[0]->GetOwningNodeUnchecked());
	if (!Read)
	{
		AddError(TEXT("the Return pin is not driven by an attribute read"));
		return false;
	}

	TestEqual(TEXT("the read names the write's attribute"), Read->Attribute, FString(TEXT("mana")));
	TestEqual(TEXT("the read keeps the write's role"),
		static_cast<int32>(Read->Role), static_cast<int32>(ECrowdyEffectRole::Source));
	TestTrue(TEXT("the read sits to the left of the Result node"), Read->NodePosX < Result->NodePosX);
	TestTrue(TEXT("the read is part of the graph"),
		Result->GetGraph()->Nodes.Contains(static_cast<UEdGraphNode*>(Read)));

	// The wire is matched by the pin's stable identity, so a later details-panel edit does not drop it.
	Result->Writes.Add(MakeAuthoringWrite(ECrowdyEffectRole::Target, TEXT("hp")));
	Result->ReconstructNode();
	const UEdGraphPin* AfterEdit = Result->FindPin(UCrowdyEffectGraphNode_Result::ReturnPinName(), EGPD_Input);
	if (!AfterEdit)
	{
		AddError(TEXT("the Return pin did not survive a rebuild"));
		return false;
	}
	TestEqual(TEXT("the returned value survives adding another write"), AfterEdit->LinkedTo.Num(), 1);

	// A second run is refused rather than replacing the answer the author now has.
	TestFalse(TEXT("the shortcut is refused once the effect returns"), CrowdyEffectReturnDefault::Apply(Result));
	return true;
}

// Compile's FnCatalog choice. The catalog contributes advisory warnings ONLY, so a catalog-free compile must
// produce the identical lowered function and the identical error verdict, minus the fn: warnings; that identity
// is what lets every warning-discarding caller (the schema-sync plan, a Blueprint node expansion, a
// bRequiresSource refresh) skip the catalog, and with it the project-wide asset load a catalog cache miss forces.
// Driven through a stub hook so the test is hermetic and never sweeps the real project.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectCompileFnCatalogChoiceTest,
	"CrowdySDK.Editor.EffectCompileFnCatalogChoice", CrowdyGraphAuthoringTestFlags)
bool FCrowdyEffectCompileFnCatalogChoiceTest::RunTest(const FString&)
{
	// A known callee that returns nothing: the one shape guaranteed to draw a catalog warning at its call site.
	const FScopedCatalogHook StubCatalog(
		[](const FString&) -> TArray<CrowdyEffectFunctionCatalog::FDeclaredFunction>
		{
			TArray<CrowdyEffectFunctionCatalog::FDeclaredFunction> Out;
			Out.Add(MakeDeclaredFunction(TEXT("side_effect_only"), TEXT("GraphAssetTestHero"), /*bAuthorsReturn*/ false));
			return Out;
		});

	// The probe exercises every authored surface the lowering emits: a require (invoke policy), a magnitude (a
	// parameter), a mutation whose expression embeds the fn: call, a bare-attribute return (return expression +
	// attribute-derived return type), and a description. The wider the function, the less a None-only lowering
	// regression can hide behind fields that are empty on both sides.
	UCrowdyEffect* Effect = MakeEffectWithContainer();
	Effect->FunctionName = TEXT("catalog_probe");
	Effect->Description = TEXT("catalog probe effect");
	Effect->Source = ECrowdyEffectSource::Text;
	FCrowdyEffectMagnitude Power;
	Power.Name = TEXT("power");
	Effect->Magnitudes.Add(Power);
	Effect->EffectScript = TEXT("require self.health > 0\nself.health += fn:side_effect_only() + $power\nreturn self.health");

	const FCrowdyEffectLoweringResult WithCatalog = Effect->Compile(ECrowdyEffectFnCatalog::Project);
	const FCrowdyEffectLoweringResult WithoutCatalog = Effect->Compile(ECrowdyEffectFnCatalog::None);

	const auto HasFnCalleeWarning = [](const FCrowdyEffectLoweringResult& R)
	{
		return R.Diagnostics.ContainsByPredicate([](const FCrowdyEffectDiagnostic& D)
		{
			return D.Severity == ECrowdyEffectSeverity::Warning && D.Message.Contains(TEXT("side_effect_only"));
		});
	};

	// The catalog path really consulted the stub; without that, the None side proves nothing.
	TestTrue(TEXT("Project consults the catalog and warns about the returnless callee"), HasFnCalleeWarning(WithCatalog));
	TestFalse(TEXT("None never consults the catalog, so the advisory warning is absent"), HasFnCalleeWarning(WithoutCatalog));

	// The advisory warning is the ONLY difference: same error verdict, same lowered function, field for field.
	TestEqual(TEXT("error verdict is identical"), WithoutCatalog.HasErrors(), WithCatalog.HasErrors());
	TestFalse(TEXT("the probe effect compiles cleanly either way"), WithCatalog.HasErrors());
	TestEqual(TEXT("function name matches"), WithoutCatalog.Function.Name, WithCatalog.Function.Name);
	TestEqual(TEXT("container type matches"),
		WithoutCatalog.Function.ContainerTypeName, WithCatalog.Function.ContainerTypeName);
	TestEqual(TEXT("description matches"), WithoutCatalog.Function.Description, WithCatalog.Function.Description);
	TestEqual(TEXT("invoke scope matches"), WithoutCatalog.Function.InvokeScope, WithCatalog.Function.InvokeScope);
	TestEqual(TEXT("autonomous-invocable matches"),
		WithoutCatalog.Function.bAutonomousInvocable, WithCatalog.Function.bAutonomousInvocable);
	TestEqual(TEXT("return type matches"), WithoutCatalog.Function.ReturnType, WithCatalog.Function.ReturnType);
	TestEqual(TEXT("return expression matches"),
		WithoutCatalog.Function.ReturnExpression, WithCatalog.Function.ReturnExpression);
	TestEqual(TEXT("invoke policy matches"),
		WithoutCatalog.Function.InvokePolicyJson, WithCatalog.Function.InvokePolicyJson);
	TestTrue(TEXT("the probe's require actually lowered a policy, so the compare above compares something"),
		!WithCatalog.Function.InvokePolicyJson.IsEmpty());

	if (TestEqual(TEXT("mutation count matches"),
		WithoutCatalog.Function.Mutations.Num(), WithCatalog.Function.Mutations.Num())
		&& TestTrue(TEXT("the probe authored a mutation"), WithCatalog.Function.Mutations.Num() > 0))
	{
		TestEqual(TEXT("mutation target matches"),
			WithoutCatalog.Function.Mutations[0].Target, WithCatalog.Function.Mutations[0].Target);
		TestEqual(TEXT("mutation property matches"),
			WithoutCatalog.Function.Mutations[0].Property, WithCatalog.Function.Mutations[0].Property);
		TestEqual(TEXT("mutation expression (the field embedding the fn: call) matches"),
			WithoutCatalog.Function.Mutations[0].Expression, WithCatalog.Function.Mutations[0].Expression);
	}
	if (TestEqual(TEXT("parameter count matches"),
		WithoutCatalog.Function.Parameters.Num(), WithCatalog.Function.Parameters.Num())
		&& TestTrue(TEXT("the probe's magnitude lowered a parameter"), WithCatalog.Function.Parameters.Num() > 0))
	{
		TestEqual(TEXT("parameter name matches"),
			WithoutCatalog.Function.Parameters[0].Name, WithCatalog.Function.Parameters[0].Name);
	}
	TestEqual(TEXT("notification count matches"),
		WithoutCatalog.Function.Notifications.Num(), WithCatalog.Function.Notifications.Num());
	TestEqual(TEXT("timer count matches"),
		WithoutCatalog.Function.Timers.Num(), WithCatalog.Function.Timers.Num());
	TestEqual(TEXT("automation presence matches"),
		WithoutCatalog.Automation.IsSet(), WithCatalog.Automation.IsSet());
	TestEqual(TEXT("trigger presence matches"),
		WithoutCatalog.Trigger.IsSet(), WithCatalog.Trigger.IsSet());
	TestEqual(TEXT("source-referenced verdict matches"),
		WithoutCatalog.bSourceReferenced, WithCatalog.bSourceReferenced);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
