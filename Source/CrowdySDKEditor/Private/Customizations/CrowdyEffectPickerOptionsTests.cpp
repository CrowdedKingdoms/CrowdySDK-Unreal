// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Customizations/CrowdyEffectPickerContainerTypeTestFixture.h"
#include "Customizations/CrowdyEffectPickerOptions.h"
#include "Customizations/CrowdyEffectPickerTestFixture.h"
#include "Replication/GameModel/CrowdyAttributeRegistry.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"
#include "Replication/GameModel/Effect/CrowdyEffectLowering.h"
#include "Replication/GameModel/Effect/CrowdyEffectSpec.h"
#include "Replication/GameModel/Effect/CrowdyEffectSpecBuilder.h"
#include "Replication/GameModel/Effect/CrowdyEffectStringifyTestSupport.h"
#include "Replication/GameModel/Effect/CrowdyGameModelFunctionInput.h"
#include "UObject/Package.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyEffectPickerTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

// AllowedAssignmentOps must mirror FCrowdyEffectLowering::LowerAssignment's own type gate exactly: numeric
// (int/float) attributes accept all five compound-assignment operators; bool/string/empty/unknown accept only
// Set (the lowering rejects any arithmetic operator on a non-numeric attribute). A drift between the two would
// either grey out a valid operator or offer one the compile step then rejects.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectPickerAllowedOpsTest,
	"CrowdySDK.GameModel.EffectPickerAllowedOps", CrowdyEffectPickerTestFlags)
bool FCrowdyEffectPickerAllowedOpsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyEffectPickerOptions;

	for (const FString& Numeric : { FString(TEXT("int")), FString(TEXT("float")) })
	{
		const TArray<ECrowdyEffectAssignmentOp> Ops = AllowedAssignmentOps(Numeric);
		TestEqual(*FString::Printf(TEXT("%s: five ops"), *Numeric), Ops.Num(), 5);
		TestTrue(*FString::Printf(TEXT("%s: includes Set"), *Numeric), Ops.Contains(ECrowdyEffectAssignmentOp::Set));
		TestTrue(*FString::Printf(TEXT("%s: includes Add"), *Numeric), Ops.Contains(ECrowdyEffectAssignmentOp::Add));
		TestTrue(*FString::Printf(TEXT("%s: includes Subtract"), *Numeric), Ops.Contains(ECrowdyEffectAssignmentOp::Subtract));
		TestTrue(*FString::Printf(TEXT("%s: includes Multiply"), *Numeric), Ops.Contains(ECrowdyEffectAssignmentOp::Multiply));
		TestTrue(*FString::Printf(TEXT("%s: includes Divide"), *Numeric), Ops.Contains(ECrowdyEffectAssignmentOp::Divide));
	}

	for (const FString& NonNumeric : { FString(TEXT("bool")), FString(TEXT("string")), FString(), FString(TEXT("container_ref")) })
	{
		const TArray<ECrowdyEffectAssignmentOp> Ops = AllowedAssignmentOps(NonNumeric);
		TestEqual(*FString::Printf(TEXT("'%s': exactly one op"), *NonNumeric), Ops.Num(), 1);
		TestTrue(*FString::Printf(TEXT("'%s': is Set"), *NonNumeric),
			Ops.Num() == 1 && Ops[0] == ECrowdyEffectAssignmentOp::Set);
	}

	return true;
}

// AttributesForClass + ValueTypeForAttribute against the local fixture: the discovered set matches the
// fixture's four CrowdyModel attributes, both a property-name and a server-key lookup resolve, the plain
// property never appears, and an unknown name or a null class returns "".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectPickerAttributeListingTest,
	"CrowdySDK.GameModel.EffectPickerAttributeListing", CrowdyEffectPickerTestFlags)
bool FCrowdyEffectPickerAttributeListingTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyEffectPickerOptions;

	const UClass* Class = UCrowdyEffectPickerTestTarget::StaticClass();
	const TArray<FCrowdyAttributeDef> Defs = AttributesForClass(Class);
	TestEqual(TEXT("four accepted attributes"), Defs.Num(), 4);

	TestEqual(TEXT("health by property name"), ValueTypeForAttribute(Class, TEXT("Health")), FString(TEXT("int")));
	TestEqual(TEXT("health by server key"), ValueTypeForAttribute(Class, TEXT("health")), FString(TEXT("int")));
	TestEqual(TEXT("speed"), ValueTypeForAttribute(Class, TEXT("Speed")), FString(TEXT("float")));
	TestEqual(TEXT("stunned"), ValueTypeForAttribute(Class, TEXT("bStunned")), FString(TEXT("bool")));
	TestEqual(TEXT("title"), ValueTypeForAttribute(Class, TEXT("Title")), FString(TEXT("string")));

	TestEqual(TEXT("plain property never an attribute"), ValueTypeForAttribute(Class, TEXT("PlainInt")), FString());
	TestEqual(TEXT("unknown name returns empty"), ValueTypeForAttribute(Class, TEXT("NoSuchThing")), FString());
	TestEqual(TEXT("null class returns empty"), ValueTypeForAttribute(nullptr, TEXT("Health")), FString());
	TestTrue(TEXT("null class returns no attributes"), AttributesForClass(nullptr).IsEmpty());

	return true;
}

// The verb <-> operator map is a pure two-way relabel: every operator round-trips through its verb, the five
// verbs read as authored (Increase/Decrease/Set/Multiply/Divide), and a non-verb string resolves to nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectVerbOperatorMappingTest,
	"CrowdySDK.Editor.EffectVerbOperatorMapping", CrowdyEffectPickerTestFlags)
bool FCrowdyEffectVerbOperatorMappingTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyEffectPickerOptions;

	for (const ECrowdyEffectAssignmentOp Op : { ECrowdyEffectAssignmentOp::Set, ECrowdyEffectAssignmentOp::Add,
		ECrowdyEffectAssignmentOp::Subtract, ECrowdyEffectAssignmentOp::Multiply, ECrowdyEffectAssignmentOp::Divide })
	{
		ECrowdyEffectAssignmentOp RoundTripped = ECrowdyEffectAssignmentOp::Set;
		const bool bResolved = OpForVerbLabel(VerbLabel(Op).ToString(), RoundTripped);
		TestTrue(TEXT("verb resolves back to an operator"), bResolved);
		TestTrue(TEXT("operator round-trips through its verb"), bResolved && RoundTripped == Op);
	}

	TestEqual(TEXT("Add reads as Increase"), VerbLabel(ECrowdyEffectAssignmentOp::Add).ToString(), FString(TEXT("Increase")));
	TestEqual(TEXT("Subtract reads as Decrease"), VerbLabel(ECrowdyEffectAssignmentOp::Subtract).ToString(), FString(TEXT("Decrease")));
	TestEqual(TEXT("Set reads as Set"), VerbLabel(ECrowdyEffectAssignmentOp::Set).ToString(), FString(TEXT("Set")));
	TestEqual(TEXT("Multiply reads as Multiply"), VerbLabel(ECrowdyEffectAssignmentOp::Multiply).ToString(), FString(TEXT("Multiply")));
	TestEqual(TEXT("Divide reads as Divide"), VerbLabel(ECrowdyEffectAssignmentOp::Divide).ToString(), FString(TEXT("Divide")));

	// Case-insensitive resolve, and a non-verb string is not mistaken for a verb.
	ECrowdyEffectAssignmentOp Lowercased = ECrowdyEffectAssignmentOp::Set;
	TestTrue(TEXT("verb match is case-insensitive"), OpForVerbLabel(TEXT("decrease"), Lowercased));
	TestTrue(TEXT("lowercase decrease is Subtract"), Lowercased == ECrowdyEffectAssignmentOp::Subtract);

	ECrowdyEffectAssignmentOp Unchanged = ECrowdyEffectAssignmentOp::Divide;
	TestFalse(TEXT("a non-verb string does not resolve"), OpForVerbLabel(TEXT("Obliterate"), Unchanged));
	TestTrue(TEXT("OutOp is left untouched on no match"), Unchanged == ECrowdyEffectAssignmentOp::Divide);

	return true;
}

// The sentence phrasing helpers: a Target reads "Target's <attr>", a Source reads "<SourceRoleLabel>'s <attr>",
// and an empty / whitespace Source label falls back to "Source".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSentencePhrasingTest,
	"CrowdySDK.Editor.EffectSentencePhrasing", CrowdyEffectPickerTestFlags)
bool FCrowdyEffectSentencePhrasingTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyEffectPickerOptions;

	TestEqual(TEXT("target phrase"),
		TargetPhrase(ECrowdyEffectRole::Target, TEXT("hp"), TEXT("Attacker")), FString(TEXT("Target's hp")));
	TestEqual(TEXT("source phrase uses the custom label"),
		TargetPhrase(ECrowdyEffectRole::Source, TEXT("power"), TEXT("Attacker")), FString(TEXT("Attacker's power")));
	TestEqual(TEXT("empty source label falls back to Source"),
		TargetPhrase(ECrowdyEffectRole::Source, TEXT("power"), FString()), FString(TEXT("Source's power")));
	TestEqual(TEXT("whitespace source label falls back to Source"),
		TargetPhrase(ECrowdyEffectRole::Source, TEXT("power"), TEXT("   ")), FString(TEXT("Source's power")));

	TestEqual(TEXT("role word target"), RoleWord(ECrowdyEffectRole::Target, TEXT("Attacker")), FString(TEXT("Target")));
	TestEqual(TEXT("role word source"), RoleWord(ECrowdyEffectRole::Source, TEXT("Attacker")), FString(TEXT("Attacker")));

	return true;
}

// Golden: SourceRoleLabel is a display-only field, so it must never reach the lowered wire form. The picker test
// fixture is deliberately untagged (a CrowdyContainer tag would leak into a live schema sync), which makes
// UCrowdyEffect::Compile short-circuit before it lowers, so the whole-asset Compile path cannot exercise a real
// lowering here. Instead this drives the same direct lowering path the core uses: it lowers an effect whose
// SourceRoleLabel is a distinctive sentinel to a real, non-empty function, then scans the entire serialized
// function for that sentinel. The sentinel appearing nowhere proves the label leaks into no description, param,
// or mutation, while the injected source_id carrier surviving proves the Source role still lowers to the neutral
// AST carrier rather than the cosmetic label - so a future edit that routed the label into the wire form is caught.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSourceRoleLabelDoesNotAffectLoweringTest,
	"CrowdySDK.Editor.EffectSourceRoleLabelDoesNotAffectLowering", CrowdyEffectPickerTestFlags)
bool FCrowdyEffectSourceRoleLabelDoesNotAffectLoweringTest::RunTest(const FString& Parameters)
{
	// A one-step spec that reads the Source role: Target.hp -= Source.str. Reading source.str forces the lowering
	// to inject the neutral source_id carrier, which is exactly where a leaked cosmetic label would surface.
	FCrowdyEffectSpec Spec;
	{
		FCrowdyEffectAssignmentSpec Assignment;
		Assignment.TargetRole = ECrowdyEffectRole::Target;
		Assignment.Attribute = TEXT("hp");
		Assignment.Operator = ECrowdyEffectAssignmentOp::Subtract;
		FCrowdyEffectTerm Term;
		Term.Op = ECrowdyEffectBinaryOp::Add;
		Term.Operand.Kind = ECrowdyEffectOperandKind::Attribute;
		Term.Operand.Role = ECrowdyEffectRole::Source;
		Term.Operand.Name = TEXT("str");
		Assignment.Value.Add(Term);
		Spec.Assignments.Add(Assignment);
	}

	// A hand-built context so the lowering actually runs to a concrete function. hp carries a [0,100] clamp so the
	// mutation is a non-trivial expression rather than a bare copy.
	FCrowdyEffectLoweringContext Ctx;
	Ctx.FunctionName = TEXT("take_damage");
	Ctx.ContainerTypeName = TEXT("Hero");
	FCrowdyAttributeDef Hp;
	Hp.PropertyName = FName(TEXT("Hp"));
	Hp.Key = TEXT("hp");
	Hp.ValueType = TEXT("int");
	Hp.bHasClamp = true;
	Hp.ClampMin = 0.0;
	Hp.ClampMax = 100.0;
	Ctx.Attributes.Add(Hp);
	FCrowdyAttributeDef Str;
	Str.PropertyName = FName(TEXT("Str"));
	Str.Key = TEXT("str");
	Str.ValueType = TEXT("int");
	Ctx.Attributes.Add(Str);

	// Stamp a distinctive label on an asset holding this spec, then lower the very spec it carries. A value this
	// unusual could never legitimately appear in the wire form, so a byte-scan of the whole serialization is the
	// strongest structural proof the label leaks nowhere.
	const FString Sentinel = TEXT("Zqx_SourceRoleLabelSentinel_7788");

	UCrowdyEffect* Effect = NewObject<UCrowdyEffect>(GetTransientPackage());
	Effect->FunctionName = TEXT("take_damage");
	Effect->SourceRoleLabel = Sentinel;

	TArray<FCrowdyEffectDiagnostic> Diagnostics;
	const FCrowdyEffectProgram Program = FCrowdyEffectSpecBuilder::BuildProgram(Spec, Diagnostics);
	const FCrowdyEffectLoweringResult Lowered = FCrowdyEffectLowering::Lower(Program, Ctx);

	TestFalse(TEXT("spec lowers without error"), Lowered.HasErrors());
	TestEqual(TEXT("spec lowers to one concrete mutation"), Lowered.Function.Mutations.Num(), 1);

	const FString Serialized = CrowdyStringifyResult(Lowered);

	TestTrue(TEXT("the Source role lowers to the neutral source_id carrier"),
		Serialized.Contains(TEXT("source_id"), ESearchCase::CaseSensitive));
	TestFalse(TEXT("SourceRoleLabel appears nowhere in the lowered output"),
		Serialized.Contains(Sentinel, ESearchCase::CaseSensitive));

	return true;
}

// The Automation "On Property Key" picker must write the attribute's server KEY, not its property name: the
// property-change trigger's propertyKey rides straight to the server with no name/key resolution on the way
// (unlike an attribute reference inside the effect body, which ResolveAttr accepts spelled either way), and the
// schema sync upserts every property definition under its key. A def whose PropertyName and Key deliberately
// differ proves the picker's value-to-write helper picks the key, not the name.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectAutomationPropertyKeyPicksServerKeyTest,
	"CrowdySDK.GameModel.EffectAutomationPropertyKeyPicksServerKey", CrowdyEffectPickerTestFlags)
bool FCrowdyEffectAutomationPropertyKeyPicksServerKeyTest::RunTest(const FString& Parameters)
{
	FCrowdyAttributeDef Def;
	Def.PropertyName = FName(TEXT("MaxHealthPoints"));
	Def.Key = TEXT("hp_max"); // a meta=(CrowdyKey=...) override distinct from the property name
	Def.ValueType = TEXT("int");

	const FString Written = CrowdyEffectPickerOptions::PropertyKeyForAutomationTrigger(Def);
	TestEqual(TEXT("writes the server key"), Written, FString(TEXT("hp_max")));
	TestNotEqual(TEXT("never writes the raw property name"), Written, Def.PropertyName.ToString());

	return true;
}

// The Attribute picker (an assignment step's target, or an Attribute operand) must also write the server KEY, not
// the declared property name: the expression editor's autocomplete (GetExpressionAttributeNames) and every other
// attribute reference already resolve through the key, and lowering treats a key and a property name as two
// different spellings of the same attribute, so mixing them inside one effect is a hard error. A def whose
// PropertyName and Key deliberately differ proves the picker's value-to-write helper picks the key, not the name;
// a def with no key assigned falls back to the property name rather than writing an empty reference.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectAttributePickerWritesServerKeyTest,
	"CrowdySDK.GameModel.EffectAttributePickerWritesServerKey", CrowdyEffectPickerTestFlags)
bool FCrowdyEffectAttributePickerWritesServerKeyTest::RunTest(const FString& Parameters)
{
	FCrowdyAttributeDef Def;
	Def.PropertyName = FName(TEXT("Hp"));
	Def.Key = TEXT("hp");
	Def.ValueType = TEXT("int");

	const FString Written = CrowdyEffectPickerOptions::AttributePickerWrittenValue(Def);
	TestEqual(TEXT("writes the server key"), Written, Def.Key);
	// A key and its property name differ only in case here, and FString comparison ignores case by default,
	// so this has to be an explicitly case-sensitive check to mean anything.
	TestTrue(TEXT("never writes the raw property name when a key differs"),
		!Written.Equals(Def.PropertyName.ToString(), ESearchCase::CaseSensitive));

	FCrowdyAttributeDef DefNoKey;
	DefNoKey.PropertyName = FName(TEXT("Speed"));
	DefNoKey.ValueType = TEXT("float");
	TestEqual(TEXT("falls back to the property name when no key is assigned"),
		CrowdyEffectPickerOptions::AttributePickerWrittenValue(DefNoKey), DefNoKey.PropertyName.ToString());

	return true;
}

// The body editor's autocomplete deliberately does the OPPOSITE of the picker: it offers the DECLARED name. A body
// is hand-authored against the container class the author is reading, so completing to the lowercase server key
// puts the tool at odds with everything they type themselves, and mixing the two inside one effect is a hard
// lowering error. The two helpers disagreeing is the point, so both are pinned here together.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectScriptCompletionOffersDeclaredNameTest,
	"CrowdySDK.GameModel.EffectScriptCompletionOffersDeclaredName", CrowdyEffectPickerTestFlags)
bool FCrowdyEffectScriptCompletionOffersDeclaredNameTest::RunTest(const FString& Parameters)
{
	FCrowdyAttributeDef Def;
	Def.PropertyName = FName(TEXT("Health"));
	Def.Key = TEXT("health");
	Def.ValueType = TEXT("int");

	// FString comparison ignores case by default, which is exactly the distinction being made, so every check here
	// is explicitly case-sensitive or it proves nothing.
	const FString Offered = CrowdyEffectPickerOptions::AttributeNameForScriptCompletion(Def);
	TestTrue(TEXT("offers the declared name"),
		Offered.Equals(Def.PropertyName.ToString(), ESearchCase::CaseSensitive));
	TestFalse(TEXT("and not the lowercased server key"), Offered.Equals(Def.Key, ESearchCase::CaseSensitive));
	TestFalse(TEXT("so it disagrees with the picker on purpose"),
		Offered.Equals(CrowdyEffectPickerOptions::AttributePickerWrittenValue(Def), ESearchCase::CaseSensitive));

	// A CrowdyKey override makes the two genuinely different words, not just different casing.
	FCrowdyAttributeDef Renamed;
	Renamed.PropertyName = FName(TEXT("MaxHealth"));
	Renamed.Key = TEXT("hp_max");
	TestEqual(TEXT("a renamed key still completes to the declared name"),
		CrowdyEffectPickerOptions::AttributeNameForScriptCompletion(Renamed), FString(TEXT("MaxHealth")));

	// A hand-built def with no name still has to complete to something usable.
	FCrowdyAttributeDef KeyOnly;
	KeyOnly.Key = TEXT("hp");
	TestEqual(TEXT("falls back to the key when there is no declared name"),
		CrowdyEffectPickerOptions::AttributeNameForScriptCompletion(KeyOnly), FString(TEXT("hp")));
	return true;
}

// IsTransientReflectionClassName's prefix gate: every reflection-artifact prefix the schema sync's own class
// sweep skips (SKEL_/REINST_/TRASHCLASS_/PLACEHOLDER-) is recognized, a plain class name is not, and the check
// is prefix-anchored (a name that merely contains one of the words, but does not start with it, is not skipped).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectTransientReflectionClassNameTest,
	"CrowdySDK.Editor.EffectTransientReflectionClassName", CrowdyEffectPickerTestFlags)
bool FCrowdyEffectTransientReflectionClassNameTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyEffectPickerOptions;

	for (const FString& Transient : { FString(TEXT("SKEL_Hero")), FString(TEXT("REINST_Hero_C_0")),
		FString(TEXT("TRASHCLASS_Hero")), FString(TEXT("PLACEHOLDER-Hero")) })
	{
		TestTrue(*FString::Printf(TEXT("'%s' is transient"), *Transient), IsTransientReflectionClassName(Transient));
	}

	TestFalse(TEXT("a plain class name is not transient"), IsTransientReflectionClassName(TEXT("Hero")));
	TestFalse(TEXT("the prefix must anchor at the start"), IsTransientReflectionClassName(TEXT("MySKEL_Hero")));

	return true;
}

// IsOfferableContainerType: null is never offerable; an untagged class (the picker's own attribute fixture,
// which carries CrowdyModel attributes but no CrowdyContainer tag) is never offerable; and a class carrying
// both a CrowdyContainer tag AND CrowdyContainerTest is excluded specifically BECAUSE it is a test fixture, not
// merely because it lacks a tag (proving the test-container exclusion is checked, not just tag presence).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectOfferableContainerTypeTest,
	"CrowdySDK.GameModel.EffectOfferableContainerType", CrowdyEffectPickerTestFlags)
bool FCrowdyEffectOfferableContainerTypeTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyEffectPickerOptions;

	FString OutTypeName;
	TestFalse(TEXT("a null class is never offerable"), IsOfferableContainerType(nullptr, OutTypeName));

	OutTypeName.Reset();
	TestFalse(TEXT("an untagged class is never offerable"),
		IsOfferableContainerType(UCrowdyEffectPickerTestTarget::StaticClass(), OutTypeName));
	TestTrue(TEXT("OutTypeName is left untouched when not offerable"), OutTypeName.IsEmpty());

	const UClass* TestFixtureClass = UCrowdyEffectPickerContainerTypeTestFixture::StaticClass();
	TestTrue(TEXT("the fixture does carry a CrowdyContainer tag"),
		FCrowdyAttributeRegistry::GetContainerTypeName(TestFixtureClass, OutTypeName));
	OutTypeName.Reset();
	TestFalse(TEXT("a CrowdyContainerTest-tagged class is excluded despite carrying a real tag"),
		IsOfferableContainerType(TestFixtureClass, OutTypeName));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
