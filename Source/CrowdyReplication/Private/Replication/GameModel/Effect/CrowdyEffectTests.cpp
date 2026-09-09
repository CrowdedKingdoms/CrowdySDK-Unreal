// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Replication/GameModel/Kit/CrowdyKitBlueprint.h"
#include "Replication/GameModel/Kit/CrowdyKitInventory.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Replication/GameModel/CrowdyAttributeRegistry.h"
#include "Replication/GameModel/CrowdyEffects.h"
#include "Replication/GameModel/CrowdyGameModelMetaKeys.h"
#include "Replication/GameModel/CrowdyGameModelTestTarget.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"
#include "Replication/GameModel/Effect/CrowdyEffectAuthoredSurface.h"
#include "Replication/GameModel/Effect/CrowdyEffectLowering.h"
#include "Replication/GameModel/Effect/CrowdyEffectParser.h"
#include "Replication/GameModel/Effect/CrowdyEffectSpec.h"
#include "Replication/GameModel/Effect/CrowdyEffectSpecBuilder.h"
#include "Replication/GameModel/Effect/CrowdyEffectSpecPrinter.h"
#include "Replication/GameModel/Effect/CrowdyGameModelFunctionMarshaller.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyEffectTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	FCrowdyAttributeDef MakeAttr(const TCHAR* Name, const TCHAR* Key, const TCHAR* ValueType,
		bool bHasClamp = false, double ClampMin = 0.0, double ClampMax = 0.0)
	{
		FCrowdyAttributeDef D;
		D.PropertyName = FName(Name);
		D.Key = Key;
		D.ValueType = ValueType;
		D.bHasClamp = bHasClamp;
		D.ClampMin = ClampMin;
		D.ClampMax = ClampMax;
		return D;
	}

	// A hero container with hp (clamp [0,100]), mana, str, level (all int) and title (string).
	FCrowdyEffectLoweringContext MakeHeroContext()
	{
		FCrowdyEffectLoweringContext Ctx;
		Ctx.FunctionName = TEXT("effect");
		Ctx.ContainerTypeName = TEXT("Hero");
		Ctx.Attributes.Add(MakeAttr(TEXT("Hp"), TEXT("hp"), TEXT("int"), true, 0.0, 100.0));
		Ctx.Attributes.Add(MakeAttr(TEXT("Mana"), TEXT("mana"), TEXT("int")));
		Ctx.Attributes.Add(MakeAttr(TEXT("Str"), TEXT("str"), TEXT("int")));
		Ctx.Attributes.Add(MakeAttr(TEXT("Level"), TEXT("level"), TEXT("int")));
		Ctx.Attributes.Add(MakeAttr(TEXT("Title"), TEXT("title"), TEXT("string")));
		return Ctx;
	}

	// A Minion source container: Power (int) exists only here, never on Hero; Hp exists on both so a
	// same-name-different-container case is available too.
	void AddMinionSource(FCrowdyEffectLoweringContext& Ctx)
	{
		Ctx.SourceContainerTypeName = TEXT("Minion");
		Ctx.SourceAttributes.Add(MakeAttr(TEXT("Power"), TEXT("power"), TEXT("int")));
		Ctx.SourceAttributes.Add(MakeAttr(TEXT("Hp"), TEXT("hp"), TEXT("int"), true, 0.0, 50.0));
	}

	// A declared parameter as the authoring surface produces one. Required-ness goes through the surface's own
	// predicate rather than being re-spelled here, because the two are not the same rule: an empty default makes a
	// parameter required everywhere except on a bool, whose checkbox never meant that. Pass bRequired to state it
	// outright instead.
	FCrowdyEffectParamDecl MakeParam(const TCHAR* Name, const TCHAR* ValueType, const TCHAR* DefaultJson = TEXT(""))
	{
		FCrowdyEffectParamDecl Decl;
		Decl.Name = Name;
		Decl.ValueType = ValueType;
		Decl.DefaultValueJson = DefaultJson;
		Decl.bRequired = UCrowdyEffect::IsLegacyRequiredEncoding(
			UCrowdyEffect::WireStringToValueType(Decl.ValueType), Decl.DefaultValueJson);
		return Decl;
	}

	// Parse + lower one script against a context, merging every diagnostic into one result.
	FCrowdyEffectLoweringResult LowerScript(const FString& Script, const FCrowdyEffectLoweringContext& Ctx)
	{
		FCrowdyEffectLoweringResult Result;
		FCrowdyEffectParseResult Parsed = FCrowdyEffectParser::Parse(Script);
		Result.Diagnostics.Append(Parsed.Diagnostics);
		if (Parsed.HasErrors())
		{
			return Result;
		}
		FCrowdyEffectLoweringResult Lowered = FCrowdyEffectLowering::Lower(Parsed.Program, Ctx);
		Result.Function = Lowered.Function;
		Result.Diagnostics.Append(Lowered.Diagnostics);
		Result.bSourceReferenced = Lowered.bSourceReferenced;
		return Result;
	}

	bool AnyDiagContains(const TArray<FCrowdyEffectDiagnostic>& Diags, ECrowdyEffectSeverity Severity, const TCHAR* Needle)
	{
		return Diags.ContainsByPredicate([Severity, Needle](const FCrowdyEffectDiagnostic& D)
		{
			return D.Severity == Severity && D.Message.Contains(Needle);
		});
	}

	// The first matching diagnostic, so a test can assert its reported line and column and not just its text.
	const FCrowdyEffectDiagnostic* FindDiag(
		const TArray<FCrowdyEffectDiagnostic>& Diags, ECrowdyEffectSeverity Severity, const TCHAR* Needle)
	{
		return Diags.FindByPredicate([Severity, Needle](const FCrowdyEffectDiagnostic& D)
		{
			return D.Severity == Severity && D.Message.Contains(Needle);
		});
	}

	FCrowdyEffectOperand AttrOperand(ECrowdyEffectRole Role, const TCHAR* Name)
	{
		FCrowdyEffectOperand O;
		O.Kind = ECrowdyEffectOperandKind::Attribute;
		O.Role = Role;
		O.Name = Name;
		return O;
	}

	FCrowdyEffectOperand MagOperand(const TCHAR* Name)
	{
		FCrowdyEffectOperand O;
		O.Kind = ECrowdyEffectOperandKind::Magnitude;
		O.Name = Name;
		return O;
	}

	FCrowdyEffectOperand NumOperand(const TCHAR* Literal)
	{
		FCrowdyEffectOperand O;
		O.Kind = ECrowdyEffectOperandKind::Number;
		O.Literal = Literal;
		return O;
	}

	FCrowdyEffectOperand ExprOperand(const TCHAR* Expression)
	{
		FCrowdyEffectOperand O;
		O.Kind = ECrowdyEffectOperandKind::Expression;
		O.Literal = Expression;
		return O;
	}

	FCrowdyEffectOperand IfOperand(const TCHAR* Cond, const TCHAR* Then, const TCHAR* Else)
	{
		FCrowdyEffectOperand O;
		O.Kind = ECrowdyEffectOperandKind::If;
		O.If.Condition = Cond;
		O.If.Then = Then;
		O.If.Else = Else;
		return O;
	}

	FCrowdyEffectOperand CallOperand(const TCHAR* Callee, const TArray<FString>& Args)
	{
		FCrowdyEffectOperand O;
		O.Kind = ECrowdyEffectOperandKind::Call;
		O.Call.Callee = Callee;
		O.Call.Args = Args;
		return O;
	}

	FCrowdyEffectTerm Term(ECrowdyEffectBinaryOp Op, const FCrowdyEffectOperand& Operand)
	{
		FCrowdyEffectTerm T;
		T.Op = Op;
		T.Operand = Operand;
		return T;
	}

	// A one-assignment spec: TargetRole.Attribute <Op> <single operand>. The commonest shape in these goldens.
	FCrowdyEffectSpec OneAssignmentSpec(
		ECrowdyEffectRole TargetRole, const TCHAR* Attribute, ECrowdyEffectAssignmentOp Op, const FCrowdyEffectOperand& Operand)
	{
		FCrowdyEffectSpec Spec;
		FCrowdyEffectAssignmentSpec Assignment;
		Assignment.TargetRole = TargetRole;
		Assignment.Attribute = Attribute;
		Assignment.Operator = Op;
		Assignment.Value.Add(Term(ECrowdyEffectBinaryOp::Add, Operand));
		Spec.Assignments.Add(Assignment);
		return Spec;
	}

	// Build + lower a structured spec through FCrowdyEffectSpecBuilder, merging diagnostics (mirrors LowerScript
	// for the text form, so a golden can compare the two surfaces directly).
	FCrowdyEffectLoweringResult LowerSpec(const FCrowdyEffectSpec& Spec, const FCrowdyEffectLoweringContext& Ctx)
	{
		FCrowdyEffectLoweringResult Result;
		FCrowdyEffectProgram Program = FCrowdyEffectSpecBuilder::BuildProgram(Spec, Result.Diagnostics);
		if (Result.HasErrors())
		{
			return Result;
		}
		FCrowdyEffectLoweringResult Lowered = FCrowdyEffectLowering::Lower(Program, Ctx);
		Result.Function = Lowered.Function;
		Result.Diagnostics.Append(Lowered.Diagnostics);
		Result.bSourceReferenced = Lowered.bSourceReferenced;
		return Result;
	}
}

// self.hp -= source.str : a cross-entity damage effect. The mutation reads the current hp minus the source's
// str, wrapped in hp's inherited clamp; a source_id container_ref param is injected; the default gate is
// is_participant (the effect touches a second entity, so owner_of_self would be wrong).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectDamageLowersTest,
	"CrowdySDK.Effect.DamageLowers", CrowdyEffectTestFlags)
bool FCrowdyEffectDamageLowersTest::RunTest(const FString& Parameters)
{
	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("self.hp -= source.str"), MakeHeroContext());

	TestFalse(TEXT("no errors"), R.HasErrors());
	if (TestEqual(TEXT("one mutation"), R.Function.Mutations.Num(), 1))
	{
		TestEqual(TEXT("target"), R.Function.Mutations[0].Target, FString(TEXT("self")));
		TestEqual(TEXT("property"), R.Function.Mutations[0].Property, FString(TEXT("hp")));
		TestEqual(TEXT("expression"), R.Function.Mutations[0].Expression,
			FString(TEXT("max(0, min(100, self.hp - (ref($source_id).str)))")));
	}
	if (TestEqual(TEXT("one injected param"), R.Function.Parameters.Num(), 1))
	{
		TestEqual(TEXT("source_id name"), R.Function.Parameters[0].Name, FString(TEXT("source_id")));
		TestEqual(TEXT("source_id type"), R.Function.Parameters[0].ValueType, FString(TEXT("container_ref")));
		TestTrue(TEXT("source_id required"), R.Function.Parameters[0].bRequired);
	}
	TestEqual(TEXT("default gate is is_participant"), R.Function.InvokePolicyJson,
		FString(TEXT("{\"type\":\"is_participant\"}")));
	return true;
}

// An attribute answers to its declared name and its server key, and to nothing else. A near miss on case
// used to resolve to the same attribute, so a line could read as though it touched two separate values.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectAttributeNameIsCaseSensitiveTest,
	"CrowdySDK.Effect.AttributeNameIsCaseSensitive", CrowdyEffectTestFlags)
bool FCrowdyEffectAttributeNameIsCaseSensitiveTest::RunTest(const FString& Parameters)
{
	// The fixture declares the property as "Hp", so its server key is "hp". Both spellings are exact.
	TestFalse(TEXT("the declared property name resolves"),
		LowerScript(TEXT("self.Hp -= 1"), MakeHeroContext()).HasErrors());
	TestFalse(TEXT("the server key resolves"),
		LowerScript(TEXT("self.hp -= 1"), MakeHeroContext()).HasErrors());

	// Neither the declared name nor the key, so it names no attribute at all.
	const FCrowdyEffectLoweringResult Mixed = LowerScript(TEXT("self.hP -= 1"), MakeHeroContext());
	TestTrue(TEXT("a mis-cased name is rejected"), Mixed.HasErrors());

	bool bSuggested = false;
	for (const FCrowdyEffectDiagnostic& D : Mixed.Diagnostics)
	{
		if (D.Message.Contains(TEXT("case-sensitive")) && D.Message.Contains(TEXT("'Hp'")))
		{
			bSuggested = true;
		}
	}
	TestTrue(TEXT("the error names the spelling that was meant"), bSuggested);

	// The reported bug in its original form: two spellings of one attribute on one line, where only one is
	// real. It must not compile as though both were the same value.
	TestTrue(TEXT("mixing two spellings of one attribute is rejected"),
		LowerScript(TEXT("self.Hp = self.hP + 1"), MakeHeroContext()).HasErrors());

	return true;
}

// self.mana -= $cost + require self.mana >= $cost : an explicit require overrides the self-target default
// (owner_of_self) with a condition leaf; the arithmetic comparison lowers verbatim inside the condition.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectCostLowersTest,
	"CrowdySDK.Effect.CostLowers", CrowdyEffectTestFlags)
bool FCrowdyEffectCostLowersTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.Magnitudes.Add(MakeParam(TEXT("cost"), TEXT("int")));

	const FCrowdyEffectLoweringResult R = LowerScript(
		TEXT("self.mana -= $cost\nrequire self.mana >= $cost"), Ctx);

	TestFalse(TEXT("no errors"), R.HasErrors());
	if (TestEqual(TEXT("one mutation"), R.Function.Mutations.Num(), 1))
	{
		TestEqual(TEXT("property"), R.Function.Mutations[0].Property, FString(TEXT("mana")));
		TestEqual(TEXT("expression (no clamp on mana)"), R.Function.Mutations[0].Expression,
			FString(TEXT("self.mana - ($cost)")));
	}
	// The inferred gate survives the require. A condition is checked against values the caller sends, so on its
	// own it keeps nobody out; before this was and-ed in, one "require self.mana >= $cost" made the function
	// callable by any account against anyone's container.
	TestEqual(TEXT("the authored condition is added to the inferred gate, not substituted for it"),
		R.Function.InvokePolicyJson,
		FString(TEXT("{\"type\":\"and\",\"rules\":[{\"type\":\"owner_of_self\"},")
			TEXT("{\"type\":\"condition\",\"expression\":\"self.mana >= $cost\"}]}")));
	if (TestEqual(TEXT("one param"), R.Function.Parameters.Num(), 1))
	{
		TestEqual(TEXT("cost param"), R.Function.Parameters[0].Name, FString(TEXT("cost")));
	}
	return true;
}

// self.str += $amount : a pure self-target buff defaults to owner_of_self.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectBuffLowersTest,
	"CrowdySDK.Effect.BuffLowers", CrowdyEffectTestFlags)
bool FCrowdyEffectBuffLowersTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.Magnitudes.Add(MakeParam(TEXT("amount"), TEXT("int")));

	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("self.str += $amount"), Ctx);

	TestFalse(TEXT("no errors"), R.HasErrors());
	if (TestEqual(TEXT("one mutation"), R.Function.Mutations.Num(), 1))
	{
		TestEqual(TEXT("expression"), R.Function.Mutations[0].Expression, FString(TEXT("self.str + ($amount)")));
	}
	TestEqual(TEXT("default gate is owner_of_self"), R.Function.InvokePolicyJson,
		FString(TEXT("{\"type\":\"owner_of_self\"}")));
	TestFalse(TEXT("no source_id injected on a pure-self effect"),
		R.Function.Parameters.ContainsByPredicate([](const FCrowdyGameModelFunctionParam& P) { return P.Name == TEXT("source_id"); }));
	return true;
}

// A multiplicative formula plus a raw(...) operand: the raw content splices in verbatim, the whole thing is
// clamp-wrapped, and minimal precedence-correct parenthesization is preserved.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectFormulaAndRawTest,
	"CrowdySDK.Effect.FormulaAndRaw", CrowdyEffectTestFlags)
bool FCrowdyEffectFormulaAndRawTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.Magnitudes.Add(MakeParam(TEXT("mult"), TEXT("float")));

	const FCrowdyEffectLoweringResult R = LowerScript(
		TEXT("self.hp = self.hp * $mult + raw(\"fn:level_bonus(self.level)\")"), Ctx);

	TestFalse(TEXT("no errors"), R.HasErrors());
	if (TestEqual(TEXT("one mutation"), R.Function.Mutations.Num(), 1))
	{
		// The raw() operand is parenthesized as an operand so its content cannot misgroup with the surrounding +.
		TestEqual(TEXT("expression"), R.Function.Mutations[0].Expression,
			FString(TEXT("max(0, min(100, self.hp * $mult + (fn:level_bonus(self.level))))")));
	}
	return true;
}

// A raw(...) whose content is a multi-term expression is parenthesized when combined with an operator, so the
// server never regroups it: 10 - raw("self.str - 1") must emit 10 - (self.str - 1), not 10 - self.str - 1.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectRawOperandGroupedTest,
	"CrowdySDK.Effect.RawOperandGrouped", CrowdyEffectTestFlags)
bool FCrowdyEffectRawOperandGroupedTest::RunTest(const FString& Parameters)
{
	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("self.mana = 10 - raw(\"self.str - 1\")"), MakeHeroContext());

	TestFalse(TEXT("no errors"), R.HasErrors());
	if (TestEqual(TEXT("one mutation"), R.Function.Mutations.Num(), 1))
	{
		TestEqual(TEXT("raw operand is parenthesized"), R.Function.Mutations[0].Expression,
			FString(TEXT("10 - (self.str - 1)")));
	}
	return true;
}

// Two assignment lines lower to two ordered mutations in one function (the server runs them transactionally).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectMultiWriteTransactionalTest,
	"CrowdySDK.Effect.MultiWriteTransactional", CrowdyEffectTestFlags)
bool FCrowdyEffectMultiWriteTransactionalTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.Magnitudes.Add(MakeParam(TEXT("dmg"), TEXT("int")));
	Ctx.Magnitudes.Add(MakeParam(TEXT("cost"), TEXT("int")));

	const FCrowdyEffectLoweringResult R = LowerScript(
		TEXT("self.hp -= $dmg\nself.mana -= $cost"), Ctx);

	TestFalse(TEXT("no errors"), R.HasErrors());
	if (TestEqual(TEXT("two ordered mutations"), R.Function.Mutations.Num(), 2))
	{
		TestEqual(TEXT("first is hp"), R.Function.Mutations[0].Property, FString(TEXT("hp")));
		TestEqual(TEXT("first expr"), R.Function.Mutations[0].Expression,
			FString(TEXT("max(0, min(100, self.hp - ($dmg)))")));
		TestEqual(TEXT("second is mana"), R.Function.Mutations[1].Property, FString(TEXT("mana")));
		TestEqual(TEXT("second expr"), R.Function.Mutations[1].Expression, FString(TEXT("self.mana - ($cost)")));
	}
	return true;
}

// require bare-keyword -> structured leaf; require comparison -> condition; multiple requires -> and-combined.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectRequireStructuredVsConditionTest,
	"CrowdySDK.Effect.RequireStructuredVsCondition", CrowdyEffectTestFlags)
bool FCrowdyEffectRequireStructuredVsConditionTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.Magnitudes.Add(MakeParam(TEXT("amount"), TEXT("int")));

	const FCrowdyEffectLoweringResult R = LowerScript(
		TEXT("self.str += $amount\nrequire host\nrequire self.str < 100"), Ctx);

	TestFalse(TEXT("no errors"), R.HasErrors());
	TestEqual(TEXT("two requires combine with and, keyword + condition"), R.Function.InvokePolicyJson,
		FString(TEXT("{\"type\":\"and\",\"rules\":[{\"type\":\"is_host\"},{\"type\":\"condition\",\"expression\":\"self.str < 100\"}]}")));
	return true;
}

// A value require must never be able to delete the inferred identity gate. This is the shape that made DealDamage
// forgeable: two ordinary argument validations replaced owner_of_self entirely, leaving a function any account
// could call against any container. Both directions are asserted, because either alone is satisfiable by a
// blanket rule: a value-only require KEEPS the gate, and an authored authority leaf REPLACES it, since choosing
// "require host" is a deliberate decision that and-ing owner_of_self onto would break.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectValueRequireKeepsIdentityGateTest,
	"CrowdySDK.Effect.ValueRequireKeepsIdentityGate", CrowdyEffectTestFlags)
bool FCrowdyEffectValueRequireKeepsIdentityGateTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.Magnitudes.Add(MakeParam(TEXT("damage"), TEXT("int")));

	// The DealDamage shape: two value guards and nothing about the caller.
	const FCrowdyEffectLoweringResult Guarded = LowerScript(
		TEXT("self.hp -= $damage\nrequire self.hp > 0\nrequire $damage > 0"), Ctx);
	TestFalse(TEXT("guarded: no errors"), Guarded.HasErrors());
	TestTrue(TEXT("the identity gate survives two value requires"),
		Guarded.Function.InvokePolicyJson.Contains(TEXT("\"owner_of_self\"")));
	TestTrue(TEXT("and the authored guards survive too"),
		Guarded.Function.InvokePolicyJson.Contains(TEXT("$damage > 0")));

	// The opposite direction: an authored authority leaf is a deliberate choice and is left alone.
	const FCrowdyEffectLoweringResult Host = LowerScript(
		TEXT("self.hp -= $damage\nrequire host\nrequire $damage > 0"), Ctx);
	TestFalse(TEXT("host: no errors"), Host.HasErrors());
	TestTrue(TEXT("an authored authority leaf is kept"),
		Host.Function.InvokePolicyJson.Contains(TEXT("\"is_host\"")));
	TestFalse(TEXT("and owner_of_self is NOT added on top of it"),
		Host.Function.InvokePolicyJson.Contains(TEXT("\"owner_of_self\"")));

	// An or with a caller-suppliable branch does not constrain the caller, so the gate is still added: the whole
	// point is that a caller who picks the right argument must not be able to satisfy the policy alone.
	const FCrowdyEffectLoweringResult Or = LowerScript(
		TEXT("self.hp -= $damage\nrequire host || $damage > 0"), Ctx);
	TestFalse(TEXT("or: no errors"), Or.HasErrors());
	TestTrue(TEXT("an or with a value branch still gets the identity gate"),
		Or.Function.InvokePolicyJson.Contains(TEXT("\"owner_of_self\"")));

	return true;
}

// require feature("...") lowers to a tier_feature leaf carrying the feature key.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectPolicyLeafArgsTest,
	"CrowdySDK.Effect.PolicyLeafArgs", CrowdyEffectTestFlags)
bool FCrowdyEffectPolicyLeafArgsTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.Magnitudes.Add(MakeParam(TEXT("amount"), TEXT("int")));

	const FCrowdyEffectLoweringResult Feature = LowerScript(
		TEXT("self.str += $amount\nrequire feature(\"premium_abilities\")"), Ctx);
	TestFalse(TEXT("feature: no errors"), Feature.HasErrors());
	// tier_feature gates the APP's tier, not the account calling, so the inferred gate is still added: a premium
	// ability is not a thing every player may use on every other player's container.
	TestEqual(TEXT("tier_feature is added to the inferred gate, because it constrains the app not the caller"),
		Feature.Function.InvokePolicyJson,
		FString(TEXT("{\"type\":\"and\",\"rules\":[{\"type\":\"owner_of_self\"},")
			TEXT("{\"type\":\"tier_feature\",\"feature\":\"premium_abilities\"}]}")));

	const FCrowdyEffectLoweringResult Grid = LowerScript(
		TEXT("self.str += $amount\nrequire grid_permission(\"update_voxel_data\", 7)"), Ctx);
	TestFalse(TEXT("grid: no errors"), Grid.HasErrors());
	TestEqual(TEXT("grid_permission leaf with gridId"), Grid.Function.InvokePolicyJson,
		FString(TEXT("{\"type\":\"grid_permission\",\"key\":\"update_voxel_data\",\"gridId\":\"7\"}")));
	return true;
}

// An unknown require keyword is a located error, not a silent pass-through.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectGateRefusalTest,
	"CrowdySDK.Effect.GateRefusal", CrowdyEffectTestFlags)
bool FCrowdyEffectGateRefusalTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.Magnitudes.Add(MakeParam(TEXT("dmg"), TEXT("int")));

	const FCrowdyEffectLoweringResult R = LowerScript(
		TEXT("self.hp -= $dmg\nrequire wizard"), Ctx);

	TestTrue(TEXT("has errors"), R.HasErrors());
	TestTrue(TEXT("names the unknown requirement"),
		AnyDiagContains(R.Diagnostics, ECrowdyEffectSeverity::Error, TEXT("unknown requirement 'wizard'")));
	return true;
}

// A clamp line is rejected at parse time: clamp bounds come only from the attribute's ClampMin/ClampMax.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectClampInEffectRejectedTest,
	"CrowdySDK.Effect.ClampInEffectRejected", CrowdyEffectTestFlags)
bool FCrowdyEffectClampInEffectRejectedTest::RunTest(const FString& Parameters)
{
	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("clamp self.hp 0 100"), MakeHeroContext());

	TestTrue(TEXT("has errors"), R.HasErrors());
	TestTrue(TEXT("explains the clamp is inherited"),
		AnyDiagContains(R.Diagnostics, ECrowdyEffectSeverity::Error, TEXT("clamp bounds are inherited")));
	return true;
}

// An unknown attribute is a located error naming the attribute and the container type.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectUnknownAttributeTest,
	"CrowdySDK.Effect.UnknownAttribute", CrowdyEffectTestFlags)
bool FCrowdyEffectUnknownAttributeTest::RunTest(const FString& Parameters)
{
	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("self.stamina -= 5"), MakeHeroContext());

	TestTrue(TEXT("has errors"), R.HasErrors());
	TestTrue(TEXT("names the unknown attribute"),
		AnyDiagContains(R.Diagnostics, ECrowdyEffectSeverity::Error, TEXT("unknown attribute 'stamina'")));
	return true;
}

// A compound arithmetic operator on a string attribute is a located error.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectOperatorTypeMismatchTest,
	"CrowdySDK.Effect.OperatorTypeMismatch", CrowdyEffectTestFlags)
bool FCrowdyEffectOperatorTypeMismatchTest::RunTest(const FString& Parameters)
{
	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("self.title *= 2"), MakeHeroContext());

	TestTrue(TEXT("has errors"), R.HasErrors());
	TestTrue(TEXT("explains only int/float support the operator"),
		AnyDiagContains(R.Diagnostics, ECrowdyEffectSeverity::Error, TEXT("only int/float")));
	return true;
}

// An undeclared $param warns (matching the server's own static analysis) but still lowers.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectUndeclaredParamWarnsTest,
	"CrowdySDK.Effect.UndeclaredParamWarns", CrowdyEffectTestFlags)
bool FCrowdyEffectUndeclaredParamWarnsTest::RunTest(const FString& Parameters)
{
	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("self.hp -= $ghost"), MakeHeroContext());

	TestFalse(TEXT("no errors (an undeclared param is only a warning)"), R.HasErrors());
	TestTrue(TEXT("warns about the undeclared param"),
		AnyDiagContains(R.Diagnostics, ECrowdyEffectSeverity::Warning, TEXT("$ghost")));
	TestEqual(TEXT("still lowers the mutation"), R.Function.Mutations.Num(), 1);
	return true;
}

// An explicit ref($param) is cross-entity (default gate is_participant) but injects no source_id (the author
// declared the ref param themselves); the attribute after the ref is passed through verbatim.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectExplicitRefTest,
	"CrowdySDK.Effect.ExplicitRef", CrowdyEffectTestFlags)
bool FCrowdyEffectExplicitRefTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.Magnitudes.Add(MakeParam(TEXT("ally_id"), TEXT("container_ref")));

	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("self.hp += ref($ally_id).str"), Ctx);

	TestFalse(TEXT("no errors"), R.HasErrors());
	if (TestEqual(TEXT("one mutation"), R.Function.Mutations.Num(), 1))
	{
		TestEqual(TEXT("expression"), R.Function.Mutations[0].Expression,
			FString(TEXT("max(0, min(100, self.hp + (ref($ally_id).str)))")));
	}
	TestEqual(TEXT("default gate is is_participant (cross-entity)"), R.Function.InvokePolicyJson,
		FString(TEXT("{\"type\":\"is_participant\"}")));
	TestFalse(TEXT("no source_id injected for an explicit ref"),
		R.Function.Parameters.ContainsByPredicate([](const FCrowdyGameModelFunctionParam& P) { return P.Name == TEXT("source_id"); }));
	return true;
}

// End-to-end through the UCrowdyEffect asset: resolving the container class, discovering its attributes, and
// lowering, all in one Compile() call. The discovery target's health carries a [0,100] clamp. DiscoverForClass
// on that fixture logs the dual-plane reject + wrong-arity notify drop, so those are whitelisted.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectAssetCompileTest,
	"CrowdySDK.Effect.AssetCompile", CrowdyEffectTestFlags)
bool FCrowdyEffectAssetCompileTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("marked both CrowdyModel and CrowdyState"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedError(TEXT("must be parameterless"), EAutomationExpectedErrorFlags::Contains, 1);

	UCrowdyEffect* Effect = NewObject<UCrowdyEffect>(GetTransientPackage());
	Effect->ContainerClass = UCrowdyGameModelDiscoveryTarget::StaticClass();
	Effect->FunctionName = TEXT("take_damage");
	Effect->Source = ECrowdyEffectSource::Text;
	Effect->EffectScript = TEXT("self.health -= 5");

	const FCrowdyEffectLoweringResult R = Effect->Compile();

	TestFalse(TEXT("no errors"), R.HasErrors());
	TestEqual(TEXT("function name from the asset"), R.Function.Name, FString(TEXT("take_damage")));
	TestEqual(TEXT("container type from the class tag"), R.Function.ContainerTypeName, FString(TEXT("TestHero")));
	if (TestEqual(TEXT("one mutation"), R.Function.Mutations.Num(), 1))
	{
		TestEqual(TEXT("property"), R.Function.Mutations[0].Property, FString(TEXT("health")));
		TestEqual(TEXT("clamp inherited from the health attribute"), R.Function.Mutations[0].Expression,
			FString(TEXT("max(0, min(100, self.health - (5)))")));
	}
	TestEqual(TEXT("default gate owner_of_self"), R.Function.InvokePolicyJson,
		FString(TEXT("{\"type\":\"owner_of_self\"}")));
	return true;
}

// Pathological deeply-nested / very-long inputs must yield a located diagnostic, never a stack-overflow crash
// of the editor / cook. Covers the recursive-descent parser (deep parens, deep unary), the flat-chain case
// that parses iteratively into a deep AST, and a deep require policy chain.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectDeepInputRejectedTest,
	"CrowdySDK.Effect.DeepInputRejected", CrowdyEffectTestFlags)
bool FCrowdyEffectDeepInputRejectedTest::RunTest(const FString& Parameters)
{
	const FCrowdyEffectLoweringContext Ctx = MakeHeroContext();

	{
		FString Script = TEXT("self.hp = ");
		for (int32 I = 0; I < 5000; ++I) { Script += TEXT("("); }
		Script += TEXT("1");
		for (int32 I = 0; I < 5000; ++I) { Script += TEXT(")"); }
		TestTrue(TEXT("deep parens rejected, not crashed"), LowerScript(Script, Ctx).HasErrors());
	}
	{
		FString Script = TEXT("self.hp = 1");
		for (int32 I = 0; I < 8000; ++I) { Script += TEXT("+1"); }
		TestTrue(TEXT("long flat chain rejected, not crashed"), LowerScript(Script, Ctx).HasErrors());
	}
	{
		FString Script = TEXT("self.hp = ");
		for (int32 I = 0; I < 8000; ++I) { Script += TEXT("-"); }
		Script += TEXT("5");
		TestTrue(TEXT("deep unary rejected, not crashed"), LowerScript(Script, Ctx).HasErrors());
	}
	{
		FCrowdyEffectLoweringContext PolicyCtx = MakeHeroContext();
		PolicyCtx.Magnitudes.Add(MakeParam(TEXT("amount"), TEXT("int")));
		FString Script = TEXT("self.str += $amount\nrequire host");
		for (int32 I = 0; I < 8000; ++I) { Script += TEXT(" && host"); }
		TestTrue(TEXT("deep require chain rejected, not crashed"), LowerScript(Script, PolicyCtx).HasErrors());
	}
	return true;
}

// A magnitude may not claim a name the effect layer reserves (source_id and the server-injected condition
// variables), or the emitted parameter list collides with the injected source_id.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectReservedParamNameTest,
	"CrowdySDK.Effect.ReservedParamName", CrowdyEffectTestFlags)
bool FCrowdyEffectReservedParamNameTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.Magnitudes.Add(FCrowdyEffectParamDecl{ TEXT("source_id"), TEXT("int"), TEXT(""), TEXT("") });

	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("self.hp -= source.str"), Ctx);

	TestTrue(TEXT("has errors"), R.HasErrors());
	TestTrue(TEXT("names the reserved magnitude"),
		AnyDiagContains(R.Diagnostics, ECrowdyEffectSeverity::Error, TEXT("reserved")));
	return true;
}

// The four server-injected condition variables read cleanly in a require with no spurious undeclared-param
// warning (they cannot be declared as magnitudes and the server provides them).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectInjectedConditionVarsTest,
	"CrowdySDK.Effect.InjectedConditionVars", CrowdyEffectTestFlags)
bool FCrowdyEffectInjectedConditionVarsTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.Magnitudes.Add(MakeParam(TEXT("amount"), TEXT("int")));

	const FCrowdyEffectLoweringResult R = LowerScript(
		TEXT("self.str += $amount\nrequire $self_owner_id == $caller_user_id"), Ctx);

	TestFalse(TEXT("no errors"), R.HasErrors());
	TestEqual(TEXT("no diagnostics at all (no spurious undeclared-param warning)"), R.Diagnostics.Num(), 0);
	// An ownership check written as a condition still reads as a condition to the lowering, so the inferred gate
	// is added. That is stricter than what was authored and deliberately so: the author gets the gate they asked
	// for plus the one the shape implies, and neither can be lost by writing the other.
	TestEqual(TEXT("lowers to the injected-var condition, and-ed onto the inferred gate"),
		R.Function.InvokePolicyJson,
		FString(TEXT("{\"type\":\"and\",\"rules\":[{\"type\":\"owner_of_self\"},")
			TEXT("{\"type\":\"condition\",\"expression\":\"$self_owner_id == $caller_user_id\"}]}")));
	return true;
}

// An authored magnitude Description is carried through to the lowered function parameter (not silently dropped).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectMagnitudeDescriptionTest,
	"CrowdySDK.Effect.MagnitudeDescription", CrowdyEffectTestFlags)
bool FCrowdyEffectMagnitudeDescriptionTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.Magnitudes.Add(FCrowdyEffectParamDecl{ TEXT("amount"), TEXT("int"), TEXT("5"), TEXT("Base amount before scaling") });

	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("self.str += $amount"), Ctx);

	TestFalse(TEXT("no errors"), R.HasErrors());
	if (TestEqual(TEXT("one param"), R.Function.Parameters.Num(), 1))
	{
		TestEqual(TEXT("description preserved"), R.Function.Parameters[0].Description,
			FString(TEXT("Base amount before scaling")));
		TestFalse(TEXT("has a default, so not required"), R.Function.Parameters[0].bRequired);
	}
	return true;
}

// A string literal round-trips to re-lexable DSL: a decoded newline is re-encoded as \n, not a raw linefeed
// that the lexer (and server) would reject.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectStringLiteralRoundTripTest,
	"CrowdySDK.Effect.StringLiteralRoundTrip", CrowdyEffectTestFlags)
bool FCrowdyEffectStringLiteralRoundTripTest::RunTest(const FString& Parameters)
{
	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("self.title = \"a\\nb\""), MakeHeroContext());

	TestFalse(TEXT("no errors"), R.HasErrors());
	if (TestEqual(TEXT("one mutation"), R.Function.Mutations.Num(), 1))
	{
		// The literal's decoded linefeed comes back out as a backslash-n escape, inside quotes.
		TestEqual(TEXT("newline re-encoded"), R.Function.Mutations[0].Expression, FString(TEXT("\"a\\nb\"")));
	}
	return true;
}

// The structured picker form lowers to the IDENTICAL function input as the equivalent text: Target.hp -=
// Source.str produces the same mutation, clamp, source_id injection, and is_participant gate as
// "self.hp -= source.str". This is the "every front-end emits the identical AST" contract, and it doubles as
// the role-mapping (F6) guard: if Target/Source were mapped backwards, the target and expression would flip.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectStructuredDamageTest,
	"CrowdySDK.Effect.StructuredDamageMatchesText", CrowdyEffectTestFlags)
bool FCrowdyEffectStructuredDamageTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectSpec Spec;
	FCrowdyEffectAssignmentSpec Assignment;
	Assignment.TargetRole = ECrowdyEffectRole::Target; // -> self
	Assignment.Attribute = TEXT("hp");
	Assignment.Operator = ECrowdyEffectAssignmentOp::Subtract;
	Assignment.Value.Add(Term(ECrowdyEffectBinaryOp::Add, AttrOperand(ECrowdyEffectRole::Source, TEXT("str")))); // -> source
	Spec.Assignments.Add(Assignment);

	const FCrowdyEffectLoweringResult Structured = LowerSpec(Spec, MakeHeroContext());
	const FCrowdyEffectLoweringResult Text = LowerScript(TEXT("self.hp -= source.str"), MakeHeroContext());

	TestFalse(TEXT("structured lowers without error"), Structured.HasErrors());
	if (TestEqual(TEXT("one mutation"), Structured.Function.Mutations.Num(), 1)
		&& Text.Function.Mutations.Num() == 1)
	{
		TestEqual(TEXT("Target maps to self, not source"),
			Structured.Function.Mutations[0].Target, Text.Function.Mutations[0].Target);
		TestEqual(TEXT("same property"),
			Structured.Function.Mutations[0].Property, Text.Function.Mutations[0].Property);
		TestEqual(TEXT("Source maps to source; clamp + injection identical to text"),
			Structured.Function.Mutations[0].Expression, Text.Function.Mutations[0].Expression);
		// The concrete golden, so a regression that breaks BOTH paths identically is still caught.
		TestEqual(TEXT("target is self"), Structured.Function.Mutations[0].Target, FString(TEXT("self")));
		TestEqual(TEXT("expression golden"), Structured.Function.Mutations[0].Expression,
			FString(TEXT("max(0, min(100, self.hp - (ref($source_id).str)))")));
	}
	TestEqual(TEXT("same invoke policy (is_participant default)"),
		Structured.Function.InvokePolicyJson, Text.Function.InvokePolicyJson);
	TestEqual(TEXT("same param count (source_id injected once)"),
		Structured.Function.Parameters.Num(), Text.Function.Parameters.Num());
	return true;
}

// A compound right-hand side groups by the shared operator precedence: Target.hp -= Source.str + $power * 2
// must group as str + (power * 2), matching the text parser exactly (not (str + power) * 2). This proves the
// builder's precedence-climbing tree assembly agrees with the parser.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectStructuredPrecedenceTest,
	"CrowdySDK.Effect.StructuredCompoundPrecedence", CrowdyEffectTestFlags)
bool FCrowdyEffectStructuredPrecedenceTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.Magnitudes.Add(MakeParam(TEXT("power"), TEXT("int")));

	FCrowdyEffectSpec Spec;
	FCrowdyEffectAssignmentSpec Assignment;
	Assignment.TargetRole = ECrowdyEffectRole::Target;
	Assignment.Attribute = TEXT("hp");
	Assignment.Operator = ECrowdyEffectAssignmentOp::Subtract;
	Assignment.Value.Add(Term(ECrowdyEffectBinaryOp::Add, AttrOperand(ECrowdyEffectRole::Source, TEXT("str"))));
	Assignment.Value.Add(Term(ECrowdyEffectBinaryOp::Add, MagOperand(TEXT("power"))));
	Assignment.Value.Add(Term(ECrowdyEffectBinaryOp::Multiply, NumOperand(TEXT("2"))));
	Spec.Assignments.Add(Assignment);

	const FCrowdyEffectLoweringResult Structured = LowerSpec(Spec, Ctx);
	const FCrowdyEffectLoweringResult Text = LowerScript(TEXT("self.hp -= source.str + $power * 2"), Ctx);

	TestFalse(TEXT("no errors"), Structured.HasErrors());
	if (TestEqual(TEXT("one mutation"), Structured.Function.Mutations.Num(), 1)
		&& Text.Function.Mutations.Num() == 1)
	{
		TestEqual(TEXT("precedence-correct, identical to the text form"),
			Structured.Function.Mutations[0].Expression, Text.Function.Mutations[0].Expression);
	}
	return true;
}

// A structured comparison require lowers to the same condition leaf as the text: Target.mana >= $cost.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectStructuredCostRequireTest,
	"CrowdySDK.Effect.StructuredCostRequire", CrowdyEffectTestFlags)
bool FCrowdyEffectStructuredCostRequireTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.Magnitudes.Add(MakeParam(TEXT("cost"), TEXT("int")));

	FCrowdyEffectSpec Spec;
	FCrowdyEffectAssignmentSpec Assignment;
	Assignment.TargetRole = ECrowdyEffectRole::Target;
	Assignment.Attribute = TEXT("mana");
	Assignment.Operator = ECrowdyEffectAssignmentOp::Subtract;
	Assignment.Value.Add(Term(ECrowdyEffectBinaryOp::Add, MagOperand(TEXT("cost"))));
	Spec.Assignments.Add(Assignment);

	FCrowdyEffectRequireSpec Require;
	Require.Kind = ECrowdyEffectRequireKind::Comparison;
	Require.Left = AttrOperand(ECrowdyEffectRole::Target, TEXT("mana"));
	Require.Comparator = ECrowdyEffectComparator::GreaterOrEqual;
	Require.Right = MagOperand(TEXT("cost"));
	Spec.Requires.Add(Require);

	const FCrowdyEffectLoweringResult R = LowerSpec(Spec, Ctx);

	TestFalse(TEXT("no errors"), R.HasErrors());
	if (TestEqual(TEXT("one mutation"), R.Function.Mutations.Num(), 1))
	{
		TestEqual(TEXT("expression (no clamp on mana)"), R.Function.Mutations[0].Expression,
			FString(TEXT("self.mana - ($cost)")));
	}
	TestEqual(TEXT("comparison require lowers to a condition leaf, and-ed onto the inferred gate"),
		R.Function.InvokePolicyJson,
		FString(TEXT("{\"type\":\"and\",\"rules\":[{\"type\":\"owner_of_self\"},")
			TEXT("{\"type\":\"condition\",\"expression\":\"self.mana >= $cost\"}]}")));
	return true;
}

// A structured policy-keyword require lowers to the matching structured leaf (MyTurn -> is_current_turn),
// overriding the inferred default gate.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectStructuredKeywordRequireTest,
	"CrowdySDK.Effect.StructuredKeywordRequire", CrowdyEffectTestFlags)
bool FCrowdyEffectStructuredKeywordRequireTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectSpec Spec;
	FCrowdyEffectAssignmentSpec Assignment;
	Assignment.TargetRole = ECrowdyEffectRole::Target;
	Assignment.Attribute = TEXT("hp");
	Assignment.Operator = ECrowdyEffectAssignmentOp::Add;
	Assignment.Value.Add(Term(ECrowdyEffectBinaryOp::Add, NumOperand(TEXT("5"))));
	Spec.Assignments.Add(Assignment);

	FCrowdyEffectRequireSpec Require;
	Require.Kind = ECrowdyEffectRequireKind::Keyword;
	Require.Keyword = ECrowdyEffectPolicyKeyword::MyTurn;
	Spec.Requires.Add(Require);

	const FCrowdyEffectLoweringResult R = LowerSpec(Spec, MakeHeroContext());

	TestFalse(TEXT("no errors"), R.HasErrors());
	if (TestEqual(TEXT("one mutation"), R.Function.Mutations.Num(), 1))
	{
		TestEqual(TEXT("clamp-wrapped add"), R.Function.Mutations[0].Expression,
			FString(TEXT("max(0, min(100, self.hp + (5)))")));
	}
	TestEqual(TEXT("my_turn keyword lowers to the is_current_turn leaf"), R.Function.InvokePolicyJson,
		FString(TEXT("{\"type\":\"is_current_turn\"}")));
	return true;
}

// An assignment with no value terms is a structural error the builder reports (never a null-RHS crash).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectStructuredEmptyAssignmentTest,
	"CrowdySDK.Effect.StructuredEmptyAssignment", CrowdyEffectTestFlags)
bool FCrowdyEffectStructuredEmptyAssignmentTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectSpec Spec;
	FCrowdyEffectAssignmentSpec Assignment;
	Assignment.TargetRole = ECrowdyEffectRole::Target;
	Assignment.Attribute = TEXT("hp");
	Assignment.Operator = ECrowdyEffectAssignmentOp::Set;
	// No Value terms.
	Spec.Assignments.Add(Assignment);

	const FCrowdyEffectLoweringResult R = LowerSpec(Spec, MakeHeroContext());

	TestTrue(TEXT("an empty-value assignment is an error"), R.HasErrors());
	TestTrue(TEXT("the diagnostic names the missing value"),
		AnyDiagContains(R.Diagnostics, ECrowdyEffectSeverity::Error, TEXT("no value")));
	return true;
}

// A structured Number operand that is blank or non-numeric (the default state of a freshly-added term) is a
// structural error, matching the text parser which rejects a malformed number. Without this the two surfaces
// disagree on validity: the structured form would silently lower to "self.hp - ()" and pass IsDataValid.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectStructuredInvalidNumberTest,
	"CrowdySDK.Effect.StructuredInvalidNumber", CrowdyEffectTestFlags)
bool FCrowdyEffectStructuredInvalidNumberTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectSpec Spec;
	FCrowdyEffectAssignmentSpec Assignment;
	Assignment.TargetRole = ECrowdyEffectRole::Target;
	Assignment.Attribute = TEXT("hp");
	Assignment.Operator = ECrowdyEffectAssignmentOp::Subtract;
	Assignment.Value.Add(Term(ECrowdyEffectBinaryOp::Add, NumOperand(TEXT("")))); // the default, empty number
	Spec.Assignments.Add(Assignment);

	const FCrowdyEffectLoweringResult R = LowerSpec(Spec, MakeHeroContext());

	TestTrue(TEXT("a blank number operand is an error"), R.HasErrors());
	TestTrue(TEXT("the diagnostic names the invalid number"),
		AnyDiagContains(R.Diagnostics, ECrowdyEffectSeverity::Error, TEXT("not a valid number")));
	TestEqual(TEXT("no malformed mutation is emitted for the rejected assignment"), R.Function.Mutations.Num(), 0);
	return true;
}

// A selected notification carrier authors the matching model-changed notification naming the changed container
// via the server-injected $self_container_id, and adds no function param (nothing is passed in). The default
// carrier (None) emits no notification, so a raw effect lowering is byte-for-byte the baseline with no wiring.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectNotificationCarrierTest,
	"CrowdySDK.Effect.NotificationCarrier", CrowdyEffectTestFlags)
bool FCrowdyEffectNotificationCarrierTest::RunTest(const FString& Parameters)
{
	auto FindParam = [](const FCrowdyGameModelFunctionInput& Fn, const FString& Name) -> const FCrowdyGameModelFunctionParam*
	{
		return Fn.Parameters.FindByPredicate([&Name](const FCrowdyGameModelFunctionParam& P) { return P.Name == Name; });
	};
	auto FindArg = [](const FCrowdyGameModelNotification& N, const TCHAR* Name) -> const FCrowdyGameModelNotificationArg*
	{
		return N.Args.FindByPredicate([Name](const FCrowdyGameModelNotificationArg& A) { return A.Name == Name; });
	};

	// Default (None): no notify_id, no notification - the baseline lowering is unchanged.
	{
		FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
		const FCrowdyEffectLoweringResult R = LowerScript(TEXT("self.hp -= 10"), Ctx);
		TestFalse(TEXT("no errors (none)"), R.HasErrors());
		TestNull(TEXT("no notify_id param by default"), FindParam(R.Function, CrowdyGameModelMetaKeys::NotifyIdParam));
		TestEqual(TEXT("no notification by default"), R.Function.Notifications.Num(), 0);
	}

	// Channel: no function param, a channel notification whose payload = concat("cmc:", $self_container_id).
	{
		FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
		Ctx.NotificationCarrier = ECrowdyModelNotificationCarrier::Channel;
		const FCrowdyEffectLoweringResult R = LowerScript(TEXT("self.hp -= 10"), Ctx);
		TestFalse(TEXT("no errors (channel)"), R.HasErrors());
		TestNull(TEXT("no notify_id param authored"), FindParam(R.Function, CrowdyGameModelMetaKeys::NotifyIdParam));
		TestNull(TEXT("self_container_id is injected, never authored as a param"),
			FindParam(R.Function, CrowdyGameModelMetaKeys::SelfContainerIdParam));
		if (TestEqual(TEXT("one channel notification"), R.Function.Notifications.Num(), 1))
		{
			const FCrowdyGameModelNotification& N = R.Function.Notifications[0];
			TestEqual(TEXT("kind channel"), N.Kind, FString(TEXT("channel")));
			if (const FCrowdyGameModelNotificationArg* Payload = FindArg(N, TEXT("payload")))
			{
				TestTrue(TEXT("payload carries the model-changed prefix"),
					Payload->Expression.Contains(CrowdyGameModelMetaKeys::ModelChangedChannelPrefix));
				TestTrue(TEXT("payload references $self_container_id"),
					Payload->Expression.Contains(TEXT("$self_container_id")));
				// $self_container_id is already a string, so no to_string cast (which would otherwise be needed for a
				// container_ref); a stray to_string here would be a regression to the retired notify_id path.
				TestFalse(TEXT("no to_string cast on a string param"), Payload->Expression.Contains(TEXT("to_string(")));
			}
			else
			{
				AddError(TEXT("channel notification has no payload arg"));
			}
		}
	}

	// Spatial: no function param, a spatial notification carrying the id in state + the model-changed event_type,
	// so the existing SERVER_EVENT (139) receive path decodes it.
	{
		FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
		Ctx.NotificationCarrier = ECrowdyModelNotificationCarrier::Spatial;
		const FCrowdyEffectLoweringResult R = LowerScript(TEXT("self.hp -= 10"), Ctx);
		TestNull(TEXT("no notify_id param (spatial)"), FindParam(R.Function, CrowdyGameModelMetaKeys::NotifyIdParam));
		if (TestEqual(TEXT("one spatial notification"), R.Function.Notifications.Num(), 1))
		{
			const FCrowdyGameModelNotification& N = R.Function.Notifications[0];
			TestEqual(TEXT("kind spatial"), N.Kind, FString(TEXT("spatial")));
			if (const FCrowdyGameModelNotificationArg* State = FindArg(N, TEXT("state")))
			{
				TestTrue(TEXT("state references $self_container_id"),
					State->Expression.Contains(TEXT("$self_container_id")));
			}
			else
			{
				AddError(TEXT("spatial notification has no state arg"));
			}
		}
	}

	return true;
}

// The lowering reports whether the effect reads or writes source.<attr>, which the asset persists as
// bRequiresSource: a source read or a source write sets it; a pure-self effect (even one that reads an explicit
// ref, which uses a param not the Source object) does not.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSourceReferencedTest,
	"CrowdySDK.Effect.SourceReferenced", CrowdyEffectTestFlags)
bool FCrowdyEffectSourceReferencedTest::RunTest(const FString& Parameters)
{
	const FCrowdyEffectLoweringContext Ctx = MakeHeroContext();

	TestTrue(TEXT("source read -> requires source"), LowerScript(TEXT("self.hp -= source.str"), Ctx).bSourceReferenced);
	TestTrue(TEXT("source write -> requires source"), LowerScript(TEXT("source.hp -= 5"), Ctx).bSourceReferenced);
	TestFalse(TEXT("pure self -> no source"), LowerScript(TEXT("self.hp -= 5"), Ctx).bSourceReferenced);
	TestFalse(TEXT("magnitude only -> no source"), LowerScript(TEXT("self.hp -= $dmg"), Ctx).bSourceReferenced);
	// An explicit ref reads a container by a param, not by the Source object, so it does not require a Source.
	TestFalse(TEXT("explicit ref -> no source"), LowerScript(TEXT("self.hp -= ref($tid).str"), Ctx).bSourceReferenced);
	return true;
}

// A declared source container type of a DIFFERENT type from the target: a read of source.power (a Minion-only
// attribute, absent from Hero) resolves against the declared source schema and lowers cleanly.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectCrossTypeSourceReadResolvesTest,
	"CrowdySDK.Effect.CrossTypeSourceReadResolves", CrowdyEffectTestFlags)
bool FCrowdyEffectCrossTypeSourceReadResolvesTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	AddMinionSource(Ctx);

	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("self.hp -= source.power"), Ctx);

	TestFalse(TEXT("no errors reading a source-only attribute against the declared source schema"), R.HasErrors());
	if (TestEqual(TEXT("one mutation"), R.Function.Mutations.Num(), 1))
	{
		TestEqual(TEXT("expression reads the Minion-only attribute through the source ref"),
			R.Function.Mutations[0].Expression, FString(TEXT("max(0, min(100, self.hp - (ref($source_id).power)))")));
	}
	return true;
}

// A write to source.<attr> against a declared cross-type source resolves the same way a read does, and the
// emitted mutation targets ref($source_id) with the source schema's key, not the target's.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectCrossTypeSourceWriteEmitsRefSourceIdTest,
	"CrowdySDK.Effect.CrossTypeSourceWriteEmitsRefSourceId", CrowdyEffectTestFlags)
bool FCrowdyEffectCrossTypeSourceWriteEmitsRefSourceIdTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	AddMinionSource(Ctx);

	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("source.power -= 5"), Ctx);

	TestFalse(TEXT("no errors"), R.HasErrors());
	if (TestEqual(TEXT("one mutation"), R.Function.Mutations.Num(), 1))
	{
		TestEqual(TEXT("mutation targets the source ref"), R.Function.Mutations[0].Target, FString(TEXT("ref($source_id)")));
		TestEqual(TEXT("property is the Minion-only key"), R.Function.Mutations[0].Property, FString(TEXT("power")));
		TestEqual(TEXT("expression"), R.Function.Mutations[0].Expression,
			FString(TEXT("ref($source_id).power - (5)")));
	}
	return true;
}

// A source read that names nothing on the declared source schema is an error naming the SOURCE container type
// (Minion), never the target's (Hero). Naming the wrong container is the exact failure this change fixes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectCrossTypeSourceMissNamesSourceTypeTest,
	"CrowdySDK.Effect.CrossTypeSourceMissNamesSourceType", CrowdyEffectTestFlags)
bool FCrowdyEffectCrossTypeSourceMissNamesSourceTypeTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	AddMinionSource(Ctx);

	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("self.hp -= source.stamina"), Ctx);

	TestTrue(TEXT("has errors"), R.HasErrors());
	TestTrue(TEXT("names the source container type"),
		AnyDiagContains(R.Diagnostics, ECrowdyEffectSeverity::Error, TEXT("on container type 'Minion'")));
	TestFalse(TEXT("never names the target's container type"),
		AnyDiagContains(R.Diagnostics, ECrowdyEffectSeverity::Error, TEXT("on container type 'Hero'")));
	return true;
}

// An attribute that exists on the TARGET but not on a declared source schema must fail as source.<attr>: Hero
// has 'str', Minion does not, so source.str is unknown once a cross-type source is declared. This is the
// validation-tightening direction: before this change the same script silently read the target's own str.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectTargetOnlyAttributeFailsAsDeclaredSourceTest,
	"CrowdySDK.Effect.TargetOnlyAttributeFailsAsDeclaredSource", CrowdyEffectTestFlags)
bool FCrowdyEffectTargetOnlyAttributeFailsAsDeclaredSourceTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	AddMinionSource(Ctx);

	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("self.hp -= source.str"), Ctx);

	TestTrue(TEXT("a target-only attribute is rejected as a declared source read"), R.HasErrors());
	TestTrue(TEXT("names the unknown attribute against the source container type"),
		AnyDiagContains(R.Diagnostics, ECrowdyEffectSeverity::Error, TEXT("unknown attribute 'str' on container type 'Minion'")));
	return true;
}

// With no SourceContainerType declared (the empty-string default every existing asset has), source.<attr>
// still validates against the target's own attributes. Pinned against FCrowdyEffectDamageLowersTest's own
// golden so this is a genuine back-compat check, not a new expectation invented for this change.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectEmptySourceContainerTypeIsBackCompatTest,
	"CrowdySDK.Effect.EmptySourceContainerTypeIsBackCompat", CrowdyEffectTestFlags)
bool FCrowdyEffectEmptySourceContainerTypeIsBackCompatTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	TestTrue(TEXT("no cross-type source declared by default"), Ctx.SourceContainerTypeName.IsEmpty());
	TestTrue(TEXT("no source attribute list by default"), Ctx.SourceAttributes.IsEmpty());

	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("self.hp -= source.str"), Ctx);

	TestFalse(TEXT("no errors"), R.HasErrors());
	if (TestEqual(TEXT("one mutation"), R.Function.Mutations.Num(), 1))
	{
		// Same golden as FCrowdyEffectDamageLowersTest: an undeclared source type reads source.<attr> against
		// the target's own attributes exactly as it did before SourceContainerType existed.
		TestEqual(TEXT("expression matches the pre-existing (same-type) golden"), R.Function.Mutations[0].Expression,
			FString(TEXT("max(0, min(100, self.hp - (ref($source_id).str)))")));
	}
	return true;
}

// FirstSpellingSelf and FirstSpellingSource are independent maps, and stay independent once a cross-type
// source is declared: mixing spellings on self does not interact with mixing spellings on source, but mixing
// two spellings of the same Minion attribute on the SAME (source) base is still caught.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectMixedSpellingPolicedPerBaseWithSourceDeclaredTest,
	"CrowdySDK.Effect.MixedSpellingPolicedPerBaseWithSourceDeclared", CrowdyEffectTestFlags)
bool FCrowdyEffectMixedSpellingPolicedPerBaseWithSourceDeclaredTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	AddMinionSource(Ctx);

	// self spells hp as "hp" (the key); source spells its own Hp attribute two different ways ("Hp" then
	// "hp"). Both containers happen to declare an attribute of that name, but self and source are policed
	// independently, so the self spelling never shields or interferes with the source conflict.
	const FCrowdyEffectLoweringResult R = LowerScript(
		TEXT("self.hp -= 1\nsource.Hp = source.hp + 1"), Ctx);

	TestTrue(TEXT("mixing two spellings of the source's own attribute is still rejected"), R.HasErrors());
	TestTrue(TEXT("the diagnostic names it as a spelling conflict, not an unknown attribute"),
		AnyDiagContains(R.Diagnostics, ECrowdyEffectSeverity::Error, TEXT("are two spellings of the same attribute")));

	// The reverse shows the maps are genuinely independent: the same word spelled two different ways, once on
	// self and once on source, is not a cross-base conflict at all.
	FCrowdyEffectLoweringContext Independent = MakeHeroContext();
	AddMinionSource(Independent);
	const FCrowdyEffectLoweringResult Cross = LowerScript(TEXT("self.Hp -= 1\nsource.hp -= 1"), Independent);
	TestFalse(TEXT("one spelling on self and a different spelling on source is not a conflict"), Cross.HasErrors());

	return true;
}

// A declared source container type that names nothing in the project (a typo, or a container class since
// deleted) is an unconditional Error at line/col 0, even when the effect body never mentions source at all:
// the declaration itself is broken and must not ride along silently on the asset.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectUnresolvedDeclaredSourceTypeErrorsTest,
	"CrowdySDK.Effect.UnresolvedDeclaredSourceTypeErrors", CrowdyEffectTestFlags)
bool FCrowdyEffectUnresolvedDeclaredSourceTypeErrorsTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.SourceContainerTypeName = TEXT("NoSuchContainer");
	Ctx.bSourceContainerTypeUnresolved = true;

	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("self.hp -= 5"), Ctx);

	TestTrue(TEXT("an unresolvable declared source type is always an error"), R.HasErrors());
	const FCrowdyEffectDiagnostic* Diag = FindDiag(
		R.Diagnostics, ECrowdyEffectSeverity::Error, TEXT("not a known Game Model container type"));
	if (TestNotNull(TEXT("the diagnostic exists"), Diag))
	{
		TestTrue(TEXT("it names the unresolved type"), Diag->Message.Contains(TEXT("'NoSuchContainer'")));
		TestEqual(TEXT("it carries no source position (a setting, not a line of the body)"), Diag->Line, 0);
		TestEqual(TEXT("column likewise"), Diag->Col, 0);
	}
	return true;
}

// CollectUndeclaredParams returns the $params a program references that are neither declared nor reserved,
// de-duplicated, across both assignments and require conditions.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectCollectUndeclaredParamsTest,
	"CrowdySDK.Effect.CollectUndeclaredParams", CrowdyEffectTestFlags)
bool FCrowdyEffectCollectUndeclaredParamsTest::RunTest(const FString& Parameters)
{
	auto Undeclared = [](const TCHAR* Script, const TArray<FString>& Declared)
	{
		const FCrowdyEffectParseResult Parsed = FCrowdyEffectParser::Parse(Script);
		return FCrowdyEffectLowering::CollectUndeclaredParams(Parsed.Program, Declared);
	};

	const TArray<FString> OneDeclared = Undeclared(TEXT("self.hp -= $a + $b"), { TEXT("a") });
	if (TestEqual(TEXT("one undeclared"), OneDeclared.Num(), 1))
	{
		TestEqual(TEXT("the undeclared name"), OneDeclared[0], FString(TEXT("b")));
	}

	// A param used twice is reported once.
	const TArray<FString> Deduped = Undeclared(TEXT("self.hp -= $a + $a"), {});
	TestEqual(TEXT("deduped to one"), Deduped.Num(), 1);

	// A reserved name (source_id, the server-injected system params) is never a magnitude.
	TestEqual(TEXT("reserved excluded"), Undeclared(TEXT("self.hp -= $source_id"), {}).Num(), 0);
	TestEqual(TEXT("self_container_id reserved"), Undeclared(TEXT("self.hp -= $self_container_id"), {}).Num(), 0);

	// A require condition's params are collected too.
	const TArray<FString> FromRequire = Undeclared(TEXT("self.hp -= $a\nrequire self.mana >= $cost"), { TEXT("a") });
	if (TestEqual(TEXT("require param undeclared"), FromRequire.Num(), 1))
	{
		TestEqual(TEXT("require param name"), FromRequire[0], FString(TEXT("cost")));
	}

	// Everything declared -> nothing missing.
	TestEqual(TEXT("all declared -> none"), Undeclared(TEXT("self.hp -= $a + $b"), { TEXT("a"), TEXT("b") }).Num(), 0);
	return true;
}

// An old asset's legacy ValueType string folds into the typed enum exactly once: a magnitude with
// ValueType="float" and bTypeMigrated=false migrates to Float and stays there, even if a later stale string
// would suggest otherwise (the enum is authoritative once migrated).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectMagnitudeTypeMigrationTest,
	"CrowdySDK.Replication.EffectMagnitudeTypeMigration", CrowdyEffectTestFlags)
bool FCrowdyEffectMagnitudeTypeMigrationTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectMagnitude M;
	M.ValueType = TEXT("float");
	M.ValueTypeEnum = ECrowdyEffectValueType::Int; // the struct default, to be overwritten by migration
	M.bTypeMigrated = false;

	UCrowdyEffect::MigrateMagnitude(M);

	TestTrue(TEXT("enum derived from the legacy string"), M.ValueTypeEnum == ECrowdyEffectValueType::Float);
	TestTrue(TEXT("migration flag set"), M.bTypeMigrated);
	TestEqual(TEXT("legacy string mirrored from the enum"), M.ValueType, FString(TEXT("float")));

	// A second pass with a stale string must not clobber the migrated enum (idempotent, enum wins).
	M.ValueType = TEXT("int");
	UCrowdyEffect::MigrateMagnitude(M);
	TestTrue(TEXT("migrated enum is not re-derived"), M.ValueTypeEnum == ECrowdyEffectValueType::Float);
	TestEqual(TEXT("string re-mirrored to the enum"), M.ValueType, FString(TEXT("float")));
	return true;
}

// A magnitude name with surrounding whitespace is trimmed to the bare name on migration.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectMagnitudeNameTrimTest,
	"CrowdySDK.Replication.EffectMagnitudeNameTrim", CrowdyEffectTestFlags)
bool FCrowdyEffectMagnitudeNameTrimTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectMagnitude M;
	M.Name = TEXT("  base_power \t");
	M.bTypeMigrated = true; // isolate the trim from the type-migration branch

	UCrowdyEffect::MigrateMagnitude(M);

	TestEqual(TEXT("name trimmed"), M.Name, FString(TEXT("base_power")));
	return true;
}

// BuildInvokeParams keys typing off ValueTypeEnum: an int magnitude emits a JSON number, a string and a
// container_ref magnitude each emit a JSON string. This is the same shape the wire-string path produced.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectBuildInvokeParamsTypedEnumTest,
	"CrowdySDK.Replication.EffectBuildInvokeParamsTypedEnum", CrowdyEffectTestFlags)
bool FCrowdyEffectBuildInvokeParamsTypedEnumTest::RunTest(const FString& Parameters)
{
	UCrowdyEffect* Effect = NewObject<UCrowdyEffect>(GetTransientPackage());

	FCrowdyEffectMagnitude Dmg;
	Dmg.Name = TEXT("dmg");
	Dmg.ValueTypeEnum = ECrowdyEffectValueType::Int;
	Dmg.DefaultValueJson = TEXT("5");
	Dmg.bTypeMigrated = true;

	FCrowdyEffectMagnitude Label;
	Label.Name = TEXT("label");
	Label.ValueTypeEnum = ECrowdyEffectValueType::String;
	Label.DefaultValueJson = TEXT("\"sword\"");
	Label.bTypeMigrated = true;

	FCrowdyEffectMagnitude Ref;
	Ref.Name = TEXT("ally");
	Ref.ValueTypeEnum = ECrowdyEffectValueType::ContainerRef;
	Ref.DefaultValueJson = TEXT("\"abc123\"");
	Ref.bTypeMigrated = true;

	Effect->Magnitudes = { Dmg, Label, Ref };

	FString Error;
	const TSharedPtr<FJsonObject> Params =
		UCrowdyEffects::BuildInvokeParams(Effect, {}, 1.0f, /*bHasSource*/ false, FString(), Error);

	if (!TestNotNull(TEXT("params built"), Params.Get()))
	{
		return true;
	}
	TestTrue(TEXT("no error"), Error.IsEmpty());

	double NumberValue = 0.0;
	TestTrue(TEXT("dmg is a JSON number"), Params->TryGetNumberField(TEXT("dmg"), NumberValue));
	TestEqual(TEXT("dmg value"), NumberValue, 5.0);

	FString StringValue;
	TestTrue(TEXT("label is a JSON string"), Params->TryGetStringField(TEXT("label"), StringValue));
	TestEqual(TEXT("label value"), StringValue, FString(TEXT("sword")));

	FString RefValue;
	TestTrue(TEXT("ally is a JSON string"), Params->TryGetStringField(TEXT("ally"), RefValue));
	TestEqual(TEXT("ally value"), RefValue, FString(TEXT("abc123")));
	return true;
}

// An Expression operand is PARSED (not spliced verbatim like Raw), so it participates in precedence and lowers
// identically to the equivalent text form: an Expression "source.str + $power * 2" on the RHS produces the same
// clamp-wrapped, precedence-correct mutation as "self.hp -= source.str + $power * 2".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSpecExpressionOperandGoldenTest,
	"CrowdySDK.Effect.SpecExpressionOperandGolden", CrowdyEffectTestFlags)
bool FCrowdyEffectSpecExpressionOperandGoldenTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.Magnitudes.Add(MakeParam(TEXT("power"), TEXT("int")));

	const FCrowdyEffectSpec Spec = OneAssignmentSpec(
		ECrowdyEffectRole::Target, TEXT("hp"), ECrowdyEffectAssignmentOp::Subtract,
		ExprOperand(TEXT("source.str + $power * 2")));

	const FCrowdyEffectLoweringResult Structured = LowerSpec(Spec, Ctx);
	const FCrowdyEffectLoweringResult Text = LowerScript(TEXT("self.hp -= source.str + $power * 2"), Ctx);

	TestFalse(TEXT("no errors"), Structured.HasErrors());
	if (TestEqual(TEXT("one mutation"), Structured.Function.Mutations.Num(), 1) && Text.Function.Mutations.Num() == 1)
	{
		TestEqual(TEXT("identical to the text form"),
			Structured.Function.Mutations[0].Expression, Text.Function.Mutations[0].Expression);
		TestEqual(TEXT("expression golden"), Structured.Function.Mutations[0].Expression,
			FString(TEXT("max(0, min(100, self.hp - (ref($source_id).str + $power * 2)))")));
	}
	return true;
}

// An If operand lowers to a builtin if(Cond, Then, Else) call, and a Call operand lowers to its named builtin
// call; both are then clamp-wrapped by the target attribute exactly like any other RHS.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSpecIfCallGoldenTest,
	"CrowdySDK.Effect.SpecIfCallGolden", CrowdyEffectTestFlags)
bool FCrowdyEffectSpecIfCallGoldenTest::RunTest(const FString& Parameters)
{
	// If: self.hp = if(source.str > 10, 5, 0). Reads source, so a source_id is injected and the gate is cross-entity.
	{
		const FCrowdyEffectSpec Spec = OneAssignmentSpec(
			ECrowdyEffectRole::Target, TEXT("hp"), ECrowdyEffectAssignmentOp::Set,
			IfOperand(TEXT("source.str > 10"), TEXT("5"), TEXT("0")));

		const FCrowdyEffectLoweringResult R = LowerSpec(Spec, MakeHeroContext());
		TestFalse(TEXT("if: no errors"), R.HasErrors());
		if (TestEqual(TEXT("if: one mutation"), R.Function.Mutations.Num(), 1))
		{
			TestEqual(TEXT("if lowers to a clamp-wrapped if(...) call"), R.Function.Mutations[0].Expression,
				FString(TEXT("max(0, min(100, if(ref($source_id).str > 10, 5, 0)))")));
		}
	}

	// Call: self.hp = max(self.str, 10). Pure self, so no source_id and the default gate is owner_of_self.
	{
		const FCrowdyEffectSpec Spec = OneAssignmentSpec(
			ECrowdyEffectRole::Target, TEXT("hp"), ECrowdyEffectAssignmentOp::Set,
			CallOperand(TEXT("max"), { TEXT("self.str"), TEXT("10") }));

		const FCrowdyEffectLoweringResult R = LowerSpec(Spec, MakeHeroContext());
		TestFalse(TEXT("call: no errors"), R.HasErrors());
		if (TestEqual(TEXT("call: one mutation"), R.Function.Mutations.Num(), 1))
		{
			TestEqual(TEXT("call lowers to a clamp-wrapped builtin call"), R.Function.Mutations[0].Expression,
				FString(TEXT("max(0, min(100, max(self.str, 10)))")));
		}
		TestEqual(TEXT("call: default gate owner_of_self"), R.Function.InvokePolicyJson,
			FString(TEXT("{\"type\":\"owner_of_self\"}")));
	}
	return true;
}

// print(parse(x)) is stable: parsing a canonical EffectScript expression and printing the AST back yields the
// same canonical text, with minimal precedence-correct parenthesization.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSpecPrinterRoundTripTest,
	"CrowdySDK.Effect.SpecPrinterRoundTrip", CrowdyEffectTestFlags)
bool FCrowdyEffectSpecPrinterRoundTripTest::RunTest(const FString& Parameters)
{
	const TArray<FString> Canonical = {
		TEXT("self.str + $power * 2"),
		TEXT("10 - (self.str - 1)"),
		TEXT("max(self.str, 10)"),
		TEXT("source.hp >= $cost"),
		TEXT("self.level * (2 + 1)"),
		TEXT("-self.str"),
		TEXT("if(self.str > 10, 5, 0)"),
		TEXT("ref($ally_id).str + 1"),
	};

	for (const FString& Text : Canonical)
	{
		TArray<FCrowdyEffectDiagnostic> Diags;
		const TSharedPtr<FCrowdyEffectExpr> Parsed = FCrowdyEffectParser::ParseExpression(Text, Diags);
		if (TestTrue(FString::Printf(TEXT("parses: %s"), *Text), Parsed.IsValid()))
		{
			TestEqual(FString::Printf(TEXT("round-trips: %s"), *Text),
				FCrowdyEffectSpecPrinter::PrintExpression(Parsed), Text);
		}
	}
	return true;
}

// Byte-parity guard for the additive operand kinds: a spec built only from the pre-existing operand kinds
// (number / attribute / magnitude) lowers identically to the equivalent text. The expansion path must be a
// no-op when no Expression / If / Call operand is present.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSpecExistingShapeUnchangedTest,
	"CrowdySDK.Effect.SpecExistingShapeUnchanged", CrowdyEffectTestFlags)
bool FCrowdyEffectSpecExistingShapeUnchangedTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.Magnitudes.Add(MakeParam(TEXT("power"), TEXT("int")));

	FCrowdyEffectSpec Spec;
	FCrowdyEffectAssignmentSpec Assignment;
	Assignment.TargetRole = ECrowdyEffectRole::Target;
	Assignment.Attribute = TEXT("hp");
	Assignment.Operator = ECrowdyEffectAssignmentOp::Subtract;
	Assignment.Value.Add(Term(ECrowdyEffectBinaryOp::Add, AttrOperand(ECrowdyEffectRole::Source, TEXT("str"))));
	Assignment.Value.Add(Term(ECrowdyEffectBinaryOp::Add, MagOperand(TEXT("power"))));
	Spec.Assignments.Add(Assignment);

	const FCrowdyEffectLoweringResult Structured = LowerSpec(Spec, Ctx);
	const FCrowdyEffectLoweringResult Text = LowerScript(TEXT("self.hp -= source.str + $power"), Ctx);

	TestFalse(TEXT("no errors"), Structured.HasErrors());
	if (TestEqual(TEXT("one mutation"), Structured.Function.Mutations.Num(), 1) && Text.Function.Mutations.Num() == 1)
	{
		TestEqual(TEXT("existing-shape spec lowers identically to before"),
			Structured.Function.Mutations[0].Expression, Text.Function.Mutations[0].Expression);
	}
	TestEqual(TEXT("same invoke policy"), Structured.Function.InvokePolicyJson, Text.Function.InvokePolicyJson);
	TestEqual(TEXT("same param count"), Structured.Function.Parameters.Num(), Text.Function.Parameters.Num());
	return true;
}

// The grid and permission reads take a closed set of literals for their overlap mode and axis. The server reports a
// bad one at upload, which is late and easy to miss in a warnings blob, so the lowering flags a literal it can check.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectGridLiteralArgsTest,
	"CrowdySDK.Effect.GridBuiltinLiteralArgsAreChecked", CrowdyEffectTestFlags)
bool FCrowdyEffectGridLiteralArgsTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();

	auto WarnsAbout = [this, &Ctx](const TCHAR* Script, const TCHAR* Fragment)
	{
		const FCrowdyEffectLoweringResult Result = LowerScript(Script, Ctx);
		for (const FCrowdyEffectDiagnostic& Diagnostic : Result.Diagnostics)
		{
			if (Diagnostic.Severity == ECrowdyEffectSeverity::Warning && Diagnostic.Message.Contains(Fragment))
			{
				return true;
			}
		}
		return false;
	};

	TestTrue(TEXT("a bad overlap mode is flagged"),
		WarnsAbout(TEXT("self.level = grid_at(1, 2, 3, \"nearest\")"), TEXT("expects a mode")));
	TestTrue(TEXT("a bad axis is flagged"),
		WarnsAbout(TEXT("self.level = grid_min(7, \"w\")"), TEXT("expects an axis")));

	TestFalse(TEXT("a valid overlap mode is not flagged"),
		WarnsAbout(TEXT("self.level = grid_at(1, 2, 3, \"smallest\")"), TEXT("expects a mode")));
	TestFalse(TEXT("a valid axis is not flagged"),
		WarnsAbout(TEXT("self.level = grid_min(7, \"z\")"), TEXT("expects an axis")));
	TestFalse(TEXT("omitting the optional mode is not flagged"),
		WarnsAbout(TEXT("self.level = grid_at(1, 2, 3)"), TEXT("expects a mode")));

	// Only a literal can be checked; a value known at run time must be left alone rather than guessed at.
	Ctx.Magnitudes.Add(MakeParam(TEXT("mode"), TEXT("string")));
	TestFalse(TEXT("a parameter in the mode position is left alone"),
		WarnsAbout(TEXT("self.level = grid_at(1, 2, 3, $mode)"), TEXT("expects a mode")));

	// The caller's id is server-injected, so a permission read must not report it as an undeclared parameter.
	const FCrowdyEffectLoweringResult Permission = LowerScript(
		TEXT("self.level = to_int(has_grid_permission($caller_user_id, \"access\"))"), Ctx);
	for (const FCrowdyEffectDiagnostic& Diagnostic : Permission.Diagnostics)
	{
		TestFalse(TEXT("the injected caller id is not reported as undeclared"),
			Diagnostic.Message.Contains(TEXT("caller_user_id")));
	}
	return true;
}

// An attribute answers to two spellings, the declared property name and the lowercased server key, and either one
// used on its own is legal. Using both inside one effect is not: the script then reads as though two separate
// values were involved while only one exists, which is what made "self.PulseIndex = self.pulseindex + 1" compile.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectAttributeSpellingConsistencyTest,
	"CrowdySDK.Effect.AttributeSpellingMustBeConsistent", CrowdyEffectTestFlags)
bool FCrowdyEffectAttributeSpellingConsistencyTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.Attributes.Add(MakeAttr(TEXT("PulseIndex"), TEXT("pulseindex"), TEXT("int")));

	const FCrowdyEffectLoweringResult Mixed = LowerScript(TEXT("self.PulseIndex = self.pulseindex + 1"), Ctx);
	TestTrue(TEXT("mixing the declared name and the server key is rejected"), Mixed.HasErrors());
	if (const FCrowdyEffectDiagnostic* D =
		FindDiag(Mixed.Diagnostics, ECrowdyEffectSeverity::Error, TEXT("two spellings of the same attribute")))
	{
		TestTrue(TEXT("names the spelling written second"), D->Message.Contains(TEXT("'pulseindex'")));
		TestTrue(TEXT("names the spelling it was first written with"), D->Message.Contains(TEXT("'PulseIndex'")));
		TestEqual(TEXT("reported on the line of the second spelling"), D->Line, 1);
		TestEqual(TEXT("reported at the column of the second spelling"), D->Col, 19);
	}
	else
	{
		AddError(TEXT("no spelling-consistency error was reported"));
	}

	// Either spelling used throughout stays legal, so the all-lowercase server-key form keeps compiling.
	TestFalse(TEXT("the declared name used throughout compiles"),
		LowerScript(TEXT("self.PulseIndex = self.PulseIndex + 1"), Ctx).HasErrors());
	TestFalse(TEXT("the server key used throughout compiles"),
		LowerScript(TEXT("self.pulseindex = self.pulseindex + 1"), Ctx).HasErrors());

	// The rule spans the whole effect, not one line, and one attribute stays one attribute whichever role reads it.
	TestTrue(TEXT("two spellings across two lines are rejected"),
		LowerScript(TEXT("self.Hp -= 1\nself.hp -= 1"), Ctx).HasErrors());

	// self.<attr> and source.<attr> name attributes on two different containers, so each base is policed on its
	// own: reading source.Hp and writing self.hp in one effect names two different values and is not a conflict.
	TestFalse(TEXT("self and source are policed independently, so differing spellings across them are legal"),
		LowerScript(TEXT("self.hp -= source.Hp"), Ctx).HasErrors());
	TestFalse(TEXT("a require on source.Hp and a write to self.hp are unrelated spellings"),
		LowerScript(TEXT("require source.Hp > 0\nself.hp = 1"), Ctx).HasErrors());

	// Two different attributes each spelled its own way is not a conflict.
	TestFalse(TEXT("different attributes may use different spellings"),
		LowerScript(TEXT("self.Hp -= 1\nself.mana -= 1"), Ctx).HasErrors());

	// The return is part of the same effect, so it is policed the same way a statement is. Both orders are checked:
	// the return reads the second spelling, and the return reads the first.
	TestTrue(TEXT("a write and a return spelling one attribute two ways is rejected"),
		LowerScript(TEXT("self.hp += 1\nreturn self.Hp"), Ctx).HasErrors());
	TestTrue(TEXT("and the other way round"),
		LowerScript(TEXT("self.Hp += 1\nreturn self.hp"), Ctx).HasErrors());
	TestFalse(TEXT("one spelling throughout, including the return, compiles"),
		LowerScript(TEXT("self.Hp += 1\nreturn self.Hp"), Ctx).HasErrors());
	return true;
}

// Writes run in order and each sees the previous result, so a plain '=' that lands on an attribute nothing has read
// since the last write makes that earlier write do nothing at all.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectOverwrittenWriteWarnsTest,
	"CrowdySDK.Effect.OverwrittenWriteWarns", CrowdyEffectTestFlags)
bool FCrowdyEffectOverwrittenWriteWarnsTest::RunTest(const FString& Parameters)
{
	const FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	auto FlagsDeadWrite = [&Ctx](const TCHAR* Script)
	{
		return AnyDiagContains(
			LowerScript(Script, Ctx).Diagnostics, ECrowdyEffectSeverity::Warning, TEXT("has no effect"));
	};

	const FCrowdyEffectLoweringResult Overwritten = LowerScript(TEXT("self.hp = 5\nself.hp = 10"), Ctx);
	TestFalse(TEXT("a dead write is a warning, not an error"), Overwritten.HasErrors());
	if (const FCrowdyEffectDiagnostic* D =
		FindDiag(Overwritten.Diagnostics, ECrowdyEffectSeverity::Warning, TEXT("has no effect")))
	{
		TestTrue(TEXT("names the attribute"), D->Message.Contains(TEXT("'hp'")));
		TestEqual(TEXT("reported on the overwriting line"), D->Line, 2);
		TestEqual(TEXT("reported at the start of the overwriting statement"), D->Col, 1);
	}
	else
	{
		AddError(TEXT("no overwritten-write warning was reported"));
	}

	TestFalse(TEXT("a write that something reads before the overwrite is not flagged"),
		FlagsDeadWrite(TEXT("self.hp = 5\nself.mana = self.hp\nself.hp = 10")));
	TestFalse(TEXT("accumulating compound writes are not flagged"),
		FlagsDeadWrite(TEXT("self.hp += 1\nself.hp += 1")));
	TestFalse(TEXT("a compound write reads the attribute first, so the set before it is not flagged"),
		FlagsDeadWrite(TEXT("self.hp = 5\nself.hp += 1")));
	// A raw escape is spliced through unparsed, so it could read anything; an uncertain case must not warn.
	TestFalse(TEXT("an opaque raw escape between the writes suppresses the warning"),
		FlagsDeadWrite(TEXT("self.hp = 5\nself.mana = raw(\"self.hp\")\nself.hp = 10")));
	TestFalse(TEXT("writes to different attributes are unrelated"),
		FlagsDeadWrite(TEXT("self.hp = 5\nself.mana = 10")));
	// A require folds into the invoke policy and is evaluated by the server before any mutation runs, so its
	// position between two writes carries no meaning: it must not suppress the dead-store warning.
	TestTrue(TEXT("a require between two writes does not suppress the warning"),
		FlagsDeadWrite(TEXT("self.hp = 5\nrequire self.hp > 0\nself.hp = 10")));
	return true;
}

// A magnitude the effect never mentions still becomes a function parameter every caller has to supply, which is
// almost always a leftover from an edit. It is a warning, not an error: the effect itself is still valid.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectUnusedMagnitudeWarnsTest,
	"CrowdySDK.Effect.UnusedMagnitudeWarns", CrowdyEffectTestFlags)
bool FCrowdyEffectUnusedMagnitudeWarnsTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.Magnitudes.Add(MakeParam(TEXT("dmg"), TEXT("int")));
	Ctx.Magnitudes.Add(MakeParam(TEXT("spare"), TEXT("int")));

	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("self.hp -= $dmg"), Ctx);

	TestFalse(TEXT("an unused magnitude is a warning, not an error"), R.HasErrors());
	TestTrue(TEXT("warns about the magnitude nothing references"),
		AnyDiagContains(R.Diagnostics, ECrowdyEffectSeverity::Warning, TEXT("'spare' is declared but never used")));
	TestFalse(TEXT("does not warn about the magnitude the effect uses"),
		AnyDiagContains(R.Diagnostics, ECrowdyEffectSeverity::Warning, TEXT("'dmg' is declared but never used")));
	TestEqual(TEXT("the unused magnitude is still emitted as a parameter"), R.Function.Parameters.Num(), 2);

	TestFalse(TEXT("a magnitude read only by a require counts as used"),
		AnyDiagContains(LowerScript(TEXT("self.hp -= $dmg\nrequire self.mana >= $spare"), Ctx).Diagnostics,
			ECrowdyEffectSeverity::Warning, TEXT("'spare' is declared but never used")));
	return true;
}

// raw(...) content is spliced through without being parsed, so a magnitude used only there is still used and must
// not be reported as dead.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectRawOnlyMagnitudeIsUsedTest,
	"CrowdySDK.Effect.RawOnlyMagnitudeCountsAsUsed", CrowdyEffectTestFlags)
bool FCrowdyEffectRawOnlyMagnitudeIsUsedTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.Magnitudes.Add(MakeParam(TEXT("mult"), TEXT("float")));

	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("self.hp = raw(\"self.hp * $mult\")"), Ctx);

	TestFalse(TEXT("no errors"), R.HasErrors());
	TestFalse(TEXT("a magnitude mentioned only inside raw(...) is not reported unused"),
		AnyDiagContains(R.Diagnostics, ECrowdyEffectSeverity::Warning, TEXT("'mult' is declared but never used")));
	return true;
}

// A literal zero divisor cannot produce a value, so it is rejected where it is written rather than at upload.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectDivideByLiteralZeroTest,
	"CrowdySDK.Effect.DivideByLiteralZeroRejected", CrowdyEffectTestFlags)
bool FCrowdyEffectDivideByLiteralZeroTest::RunTest(const FString& Parameters)
{
	const FCrowdyEffectLoweringContext Ctx = MakeHeroContext();

	const FCrowdyEffectLoweringResult Divide = LowerScript(TEXT("self.hp = self.mana / 0"), Ctx);
	TestTrue(TEXT("dividing by a literal zero is rejected"), Divide.HasErrors());
	if (const FCrowdyEffectDiagnostic* D =
		FindDiag(Divide.Diagnostics, ECrowdyEffectSeverity::Error, TEXT("division by zero")))
	{
		TestEqual(TEXT("reported on the line of the zero"), D->Line, 1);
		TestEqual(TEXT("reported at the column of the zero"), D->Col, 23);
	}
	else
	{
		AddError(TEXT("no division-by-zero error was reported"));
	}

	TestTrue(TEXT("a zero written with a decimal point is still zero"),
		LowerScript(TEXT("self.hp = self.mana / 0.0"), Ctx).HasErrors());
	TestTrue(TEXT("modulo by a literal zero is rejected"),
		LowerScript(TEXT("self.hp = self.mana % 0"), Ctx).HasErrors());
	TestFalse(TEXT("a non-zero divisor is fine"),
		LowerScript(TEXT("self.hp = self.mana / 2"), Ctx).HasErrors());
	return true;
}

// The compound '/=' divides too, but its divisor is the whole right-hand side rather than one side of a binary
// operator, so it needs its own check.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectCompoundDivideByZeroTest,
	"CrowdySDK.Effect.CompoundDivideByZeroRejected", CrowdyEffectTestFlags)
bool FCrowdyEffectCompoundDivideByZeroTest::RunTest(const FString& Parameters)
{
	const FCrowdyEffectLoweringContext Ctx = MakeHeroContext();

	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("self.hp /= 0"), Ctx);
	TestTrue(TEXT("'/= 0' is rejected"), R.HasErrors());
	if (const FCrowdyEffectDiagnostic* D =
		FindDiag(R.Diagnostics, ECrowdyEffectSeverity::Error, TEXT("division by zero")))
	{
		TestEqual(TEXT("reported on the line of the zero"), D->Line, 1);
		TestEqual(TEXT("reported at the column of the zero"), D->Col, 12);
	}
	else
	{
		AddError(TEXT("no division-by-zero error was reported"));
	}

	TestFalse(TEXT("a non-zero divisor is fine"), LowerScript(TEXT("self.hp /= 2"), Ctx).HasErrors());
	return true;
}

// A literal an int attribute cannot hold would be silently mangled by whatever reads it, so it is rejected at the
// literal. A float attribute holds magnitudes an int cannot, so the check must not apply there.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectIntLiteralOutOfRangeTest,
	"CrowdySDK.Effect.IntLiteralOutOfRangeRejected", CrowdyEffectTestFlags)
bool FCrowdyEffectIntLiteralOutOfRangeTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.Attributes.Add(MakeAttr(TEXT("Ratio"), TEXT("ratio"), TEXT("float")));

	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("self.level = 99999999999999999999"), Ctx);
	TestTrue(TEXT("a literal too large for an int attribute is rejected"), R.HasErrors());
	if (const FCrowdyEffectDiagnostic* D =
		FindDiag(R.Diagnostics, ECrowdyEffectSeverity::Error, TEXT("does not fit in the int attribute")))
	{
		TestTrue(TEXT("names the attribute"), D->Message.Contains(TEXT("'level'")));
		TestEqual(TEXT("reported on the line of the literal"), D->Line, 1);
		TestEqual(TEXT("reported at the column of the literal"), D->Col, 14);
	}
	else
	{
		AddError(TEXT("no out-of-range literal error was reported"));
	}

	TestTrue(TEXT("the same literal subtracted is rejected too"),
		LowerScript(TEXT("self.level -= 99999999999999999999"), Ctx).HasErrors());
	TestFalse(TEXT("an ordinary literal is accepted"),
		LowerScript(TEXT("self.level = 42"), Ctx).HasErrors());
	TestFalse(TEXT("a float attribute is not range-checked as an int"),
		LowerScript(TEXT("self.ratio = 99999999999999999999"), Ctx).HasErrors());
	return true;
}

// The int range is checked exactly at its edges, including the extra step the negative range has.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectIntLiteralBoundaryTest,
	"CrowdySDK.Effect.IntLiteralRangeBoundaryIsExact", CrowdyEffectTestFlags)
bool FCrowdyEffectIntLiteralBoundaryTest::RunTest(const FString& Parameters)
{
	const FCrowdyEffectLoweringContext Ctx = MakeHeroContext();

	TestFalse(TEXT("the largest positive int literal is accepted"),
		LowerScript(TEXT("self.level = 9223372036854775807"), Ctx).HasErrors());
	TestTrue(TEXT("one past it is rejected"),
		LowerScript(TEXT("self.level = 9223372036854775808"), Ctx).HasErrors());

	TestFalse(TEXT("the smallest negative int literal is accepted"),
		LowerScript(TEXT("self.level = -9223372036854775808"), Ctx).HasErrors());
	TestTrue(TEXT("one past that is rejected"),
		LowerScript(TEXT("self.level = -9223372036854775809"), Ctx).HasErrors());

	TestFalse(TEXT("leading zeros do not make a literal look longer than it is"),
		LowerScript(TEXT("self.level = 0000000000000000000042"), Ctx).HasErrors());
	return true;
}

// An effect that writes and then answers with the value it wrote: the house style for a return, and the shape
// every shipped kit function uses.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectReturnMutateAndReadTest,
	"CrowdySDK.Effect.ReturnAfterMutationLowers", CrowdyEffectTestFlags)
bool FCrowdyEffectReturnMutateAndReadTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.Magnitudes.Add(MakeParam(TEXT("amount"), TEXT("int")));

	const FCrowdyEffectLoweringResult R = LowerScript(
		TEXT("self.hp -= $amount\nreturn self.hp"), Ctx);

	TestFalse(TEXT("no errors"), R.HasErrors());
	TestEqual(TEXT("the return expression is key-resolved"), R.Function.ReturnExpression, FString(TEXT("self.hp")));
	TestEqual(TEXT("the type is read off the returned attribute"), R.Function.ReturnType, FString(TEXT("int")));
	TestEqual(TEXT("one mutation is still emitted"), R.Function.Mutations.Num(), 1);
	if (R.Function.Mutations.Num() == 1)
	{
		// The write is still clamp-wrapped; the return, which writes nothing, is not.
		TestEqual(TEXT("the write keeps its clamp"), R.Function.Mutations[0].Expression,
			FString(TEXT("max(0, min(100, self.hp - ($amount)))")));
	}
	TestFalse(TEXT("no untyped-return warning when the type is inferable"),
		AnyDiagContains(R.Diagnostics, ECrowdyEffectSeverity::Warning, TEXT("declares no return type")));
	return true;
}

// A question with no writes at all: the case that is unauthorable without a return, since an effect with no
// mutations otherwise produces nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectReturnQueryOnlyTest,
	"CrowdySDK.Effect.ReturnOnlyQueryEffectLowers", CrowdyEffectTestFlags)
bool FCrowdyEffectReturnQueryOnlyTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.Magnitudes.Add(MakeParam(TEXT("cost"), TEXT("int")));
	Ctx.ReturnType = TEXT("bool");

	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("return self.mana >= $cost"), Ctx);

	TestFalse(TEXT("no errors"), R.HasErrors());
	TestEqual(TEXT("no mutations"), R.Function.Mutations.Num(), 0);
	TestEqual(TEXT("the declared type is used verbatim"), R.Function.ReturnType, FString(TEXT("bool")));
	TestEqual(TEXT("the comparison lowers as an expression"), R.Function.ReturnExpression,
		FString(TEXT("self.mana >= $cost")));
	TestEqual(TEXT("the default owner gate still applies"), R.Function.InvokePolicyJson,
		FString(TEXT("{\"type\":\"owner_of_self\"}")));
	TestFalse(TEXT("a magnitude read only by the return is not reported unused"),
		AnyDiagContains(R.Diagnostics, ECrowdyEffectSeverity::Warning, TEXT("never used in this effect")));
	return true;
}

// A return that reads source.<attr> is what makes an effect cross-entity, so it has to inject source_id and widen
// the default gate exactly as a statement would.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectReturnFromSourceTest,
	"CrowdySDK.Effect.ReturnFromSourceIsCrossEntity", CrowdyEffectTestFlags)
bool FCrowdyEffectReturnFromSourceTest::RunTest(const FString& Parameters)
{
	const FCrowdyEffectLoweringContext Ctx = MakeHeroContext();

	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("return source.str"), Ctx);

	TestFalse(TEXT("no errors"), R.HasErrors());
	TestTrue(TEXT("the effect is reported as needing a source"), R.bSourceReferenced);
	TestEqual(TEXT("the source read is rewritten as a ref"), R.Function.ReturnExpression,
		FString(TEXT("ref($source_id).str")));
	TestEqual(TEXT("source_id is injected"), R.Function.Parameters.Num(), 1);
	if (R.Function.Parameters.Num() == 1)
	{
		TestEqual(TEXT("named source_id"), R.Function.Parameters[0].Name, FString(TEXT("source_id")));
	}
	TestEqual(TEXT("the default gate widens to participant"), R.Function.InvokePolicyJson,
		FString(TEXT("{\"type\":\"is_participant\"}")));
	return true;
}

// A shared formula: internal scope, no entry point of its own, and no invoke policy, since there is no player
// route to gate.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectInternalHelperTest,
	"CrowdySDK.Effect.InternalHelperLowersWithNoPolicy", CrowdyEffectTestFlags)
bool FCrowdyEffectInternalHelperTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.FunctionName = TEXT("xp_for_level");
	Ctx.InvokeScope = TEXT("internal");
	Ctx.ReturnType = TEXT("int");

	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("return self.level * self.level * 100"), Ctx);

	TestFalse(TEXT("no errors"), R.HasErrors());
	TestEqual(TEXT("the scope reaches the function"), R.Function.InvokeScope, FString(TEXT("internal")));
	TestTrue(TEXT("no invoke policy is inferred"), R.Function.InvokePolicyJson.IsEmpty());
	TestEqual(TEXT("no mutations"), R.Function.Mutations.Num(), 0);
	TestEqual(TEXT("the formula lowers with its own grouping"), R.Function.ReturnExpression,
		FString(TEXT("self.level * self.level * 100")));

	// An author who writes a require means it, so an explicit policy is still honoured on an internal function.
	const FCrowdyEffectLoweringResult Explicit =
		LowerScript(TEXT("require host\nreturn self.level"), Ctx);
	TestEqual(TEXT("an explicit require still lowers to a policy"), Explicit.Function.InvokePolicyJson,
		FString(TEXT("{\"type\":\"is_host\"}")));
	return true;
}

// Internal scope with nothing to answer and no automation to run it is unreachable and inert. The autonomous case
// is deliberately exempt: a trusted server-side function is reached by its automation, not by a fn: call, and that
// pairing ships in the inventory kit.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectInternalWithoutReturnTest,
	"CrowdySDK.Effect.InternalWithoutReturnRejected", CrowdyEffectTestFlags)
bool FCrowdyEffectInternalWithoutReturnTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.InvokeScope = TEXT("internal");

	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("self.hp -= 1"), Ctx);
	TestTrue(TEXT("an unreachable internal effect is rejected"),
		AnyDiagContains(R.Diagnostics, ECrowdyEffectSeverity::Error, TEXT("callable only from other effects")));

	FCrowdyEffectLoweringContext Autonomous = Ctx;
	Autonomous.bAutonomousInvocable = true;
	const FCrowdyEffectLoweringResult AutoResult = LowerScript(TEXT("self.hp -= 1"), Autonomous);
	TestFalse(TEXT("an automation entry point may be internal with no return"), AutoResult.HasErrors());
	TestTrue(TEXT("the autonomous flag reaches the function"), AutoResult.Function.bAutonomousInvocable);

	TestFalse(TEXT("a return makes the internal effect reachable"),
		LowerScript(TEXT("self.hp -= 1\nreturn self.hp"), Ctx).HasErrors());
	return true;
}

// One effect answers with exactly one value, so a second return is rejected against the line of the first rather
// than silently replacing it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectDuplicateReturnTest,
	"CrowdySDK.Effect.DuplicateReturnRejected", CrowdyEffectTestFlags)
bool FCrowdyEffectDuplicateReturnTest::RunTest(const FString& Parameters)
{
	const FCrowdyEffectLoweringContext Ctx = MakeHeroContext();

	const FCrowdyEffectParseResult Parsed =
		FCrowdyEffectParser::Parse(TEXT("self.hp -= 1\nreturn self.hp\nreturn self.mana"));
	TestTrue(TEXT("a second return is an error"), Parsed.HasErrors());
	if (const FCrowdyEffectDiagnostic* D =
		FindDiag(Parsed.Diagnostics, ECrowdyEffectSeverity::Error, TEXT("already returns a value")))
	{
		TestTrue(TEXT("points back at the first return"), D->Message.Contains(TEXT("line 2")));
		TestEqual(TEXT("reported on the second return's line"), D->Line, 3);
	}
	else
	{
		AddError(TEXT("no duplicate-return error was reported"));
	}
	TestTrue(TEXT("the first return is the one kept"),
		Parsed.Program.ReturnExpr.IsValid() && Parsed.Program.ReturnExpr->Attr == TEXT("hp"));

	// The parser resyncs after a bad line, so the statement following a rejected return still parses.
	const FCrowdyEffectParseResult After =
		FCrowdyEffectParser::Parse(TEXT("return self.hp\nreturn self.mana\nself.level += 1"));
	TestEqual(TEXT("the line after a rejected return still parses"), After.Program.Statements.Num(), 1);
	return true;
}

// The return expression resolves attribute names through the same resolver a right-hand side uses, so a name that
// is not an attribute fails at author time instead of on the server.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectReturnUnknownAttributeTest,
	"CrowdySDK.Effect.ReturnUnknownAttributeRejected", CrowdyEffectTestFlags)
bool FCrowdyEffectReturnUnknownAttributeTest::RunTest(const FString& Parameters)
{
	const FCrowdyEffectLoweringContext Ctx = MakeHeroContext();

	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("return self.stamina"), Ctx);
	TestTrue(TEXT("an unknown attribute in a return is rejected"),
		AnyDiagContains(R.Diagnostics, ECrowdyEffectSeverity::Error, TEXT("unknown attribute 'stamina'")));

	TestTrue(TEXT("a mis-cased name is named as the near miss"),
		AnyDiagContains(LowerScript(TEXT("return self.HP"), Ctx).Diagnostics,
			ECrowdyEffectSeverity::Error, TEXT("did you mean 'Hp'")));
	return true;
}

// A return hands its value to anyone who may invoke, which is not the path the server's read visibility filters,
// so a non-public attribute leaving through one is worth saying out loud.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectReturnVisibilityWarningTest,
	"CrowdySDK.Effect.ReturnOfNonPublicAttributeWarns", CrowdyEffectTestFlags)
bool FCrowdyEffectReturnVisibilityWarningTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	FCrowdyAttributeDef Secret = MakeAttr(TEXT("SecretSeed"), TEXT("secret_seed"), TEXT("int"));
	Secret.Visibility = TEXT("hidden");
	Ctx.Attributes.Add(Secret);
	FCrowdyAttributeDef OwnerOnly = MakeAttr(TEXT("Gold"), TEXT("gold"), TEXT("int"));
	OwnerOnly.Visibility = TEXT("owner");
	Ctx.Attributes.Add(OwnerOnly);

	const FCrowdyEffectLoweringResult Hidden = LowerScript(TEXT("return self.secret_seed"), Ctx);
	TestFalse(TEXT("a disclosure is a warning, not a hard failure"), Hidden.HasErrors());
	TestTrue(TEXT("a hidden attribute in a return warns"),
		AnyDiagContains(Hidden.Diagnostics, ECrowdyEffectSeverity::Warning, TEXT("hidden-visible")));

	// Buried in arithmetic counts too: the value still leaves the server.
	TestTrue(TEXT("an owner attribute inside an expression warns"),
		AnyDiagContains(LowerScript(TEXT("return self.gold + 1"), Ctx).Diagnostics,
			ECrowdyEffectSeverity::Warning, TEXT("owner-visible")));

	TestFalse(TEXT("a public attribute does not warn"),
		AnyDiagContains(LowerScript(TEXT("return self.hp"), Ctx).Diagnostics,
			ECrowdyEffectSeverity::Warning, TEXT("-visible")));

	// Reading it into a write is the ordinary case the server already guards; only the return discloses.
	TestFalse(TEXT("writing from a hidden attribute does not warn"),
		AnyDiagContains(LowerScript(TEXT("self.hp = self.secret_seed"), Ctx).Diagnostics,
			ECrowdyEffectSeverity::Warning, TEXT("-visible")));
	return true;
}

// The return type is optional to the server, so an undeclared one that cannot be read off an attribute is a
// warning: the value still arrives, just untyped.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectReturnTypeDeclarationTest,
	"CrowdySDK.Effect.ReturnTypeDeclarationWarnings", CrowdyEffectTestFlags)
bool FCrowdyEffectReturnTypeDeclarationTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.Magnitudes.Add(MakeParam(TEXT("cost"), TEXT("int")));

	const FCrowdyEffectLoweringResult Untyped = LowerScript(TEXT("return self.mana >= $cost"), Ctx);
	TestFalse(TEXT("an untyped return is not an error"), Untyped.HasErrors());
	TestTrue(TEXT("an expression return with no declared type warns"),
		AnyDiagContains(Untyped.Diagnostics, ECrowdyEffectSeverity::Warning, TEXT("declares no return type")));
	TestTrue(TEXT("and is left undeclared on the wire"), Untyped.Function.ReturnType.IsEmpty());

	// A declared type always wins over the attribute's own, so an author can widen an int read to a float answer.
	FCrowdyEffectLoweringContext Declared = Ctx;
	Declared.ReturnType = TEXT("float");
	TestEqual(TEXT("a declared type overrides the inferred one"),
		LowerScript(TEXT("return self.hp"), Declared).Function.ReturnType, FString(TEXT("float")));

	// The inverse: a type declared on an effect that answers with nothing.
	const FCrowdyEffectLoweringResult NoReturn = LowerScript(TEXT("self.hp -= 1"), Declared);
	TestFalse(TEXT("a declared type with no return is not an error"), NoReturn.HasErrors());
	TestTrue(TEXT("but it warns"),
		AnyDiagContains(NoReturn.Diagnostics, ECrowdyEffectSeverity::Warning, TEXT("returns nothing")));
	TestTrue(TEXT("and nothing is declared on the wire"), NoReturn.Function.ReturnType.IsEmpty());

	// An aggregate attribute has no return-type spelling, so it falls back to undeclared rather than emitting one
	// the server has never been shown.
	FCrowdyEffectLoweringContext WithArray = MakeHeroContext();
	WithArray.Attributes.Add(MakeAttr(TEXT("Tags"), TEXT("tags"), TEXT("array")));
	const FCrowdyEffectLoweringResult Aggregate = LowerScript(TEXT("return self.tags"), WithArray);
	TestTrue(TEXT("an array attribute return declares no type"), Aggregate.Function.ReturnType.IsEmpty());
	TestTrue(TEXT("and warns about it"),
		AnyDiagContains(Aggregate.Diagnostics, ECrowdyEffectSeverity::Warning, TEXT("declares no return type")));
	return true;
}

// An unrecognized scope is a programming error in a front-end, not something an author can type, so it fails
// loudly rather than reaching the server as a rejected upsert.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectInvalidInvokeScopeTest,
	"CrowdySDK.Effect.InvalidInvokeScopeRejected", CrowdyEffectTestFlags)
bool FCrowdyEffectInvalidInvokeScopeTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.InvokeScope = TEXT("admin");

	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("self.hp -= 1"), Ctx);
	TestTrue(TEXT("an unknown scope is rejected"),
		AnyDiagContains(R.Diagnostics, ECrowdyEffectSeverity::Error, TEXT("is not an invoke scope")));
	TestEqual(TEXT("and the function falls back to the player scope"), R.Function.InvokeScope,
		FString(TEXT("player")));

	FCrowdyEffectLoweringContext Server = MakeHeroContext();
	Server.InvokeScope = TEXT("server");
	TestEqual(TEXT("server scope passes through"),
		LowerScript(TEXT("self.hp -= 1"), Server).Function.InvokeScope, FString(TEXT("server")));
	return true;
}

// The legacy autonomous-only flag folds into Callable From only where it was actually live: it was hidden and
// inert unless the effect also ran automatically, so honouring it on its own would close an effect players can
// invoke today.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectCallableFromMigrationTest,
	"CrowdySDK.Effect.CallableFromMigratesLegacyFlag", CrowdyEffectTestFlags)
bool FCrowdyEffectCallableFromMigrationTest::RunTest(const FString& Parameters)
{
	using EScope = ECrowdyEffectCallableFrom;
	auto Migrated = [](EScope Current, bool bFlag, bool bAuto)
	{
		return static_cast<int32>(UCrowdyEffect::MigrateCallableFrom(Current, bFlag, bAuto));
	};

	TestEqual(TEXT("the live legacy pair becomes server-only"),
		Migrated(EScope::Players, true, true), static_cast<int32>(EScope::ServerOnly));
	TestEqual(TEXT("the flag alone changes nothing"),
		Migrated(EScope::Players, true, false), static_cast<int32>(EScope::Players));
	TestEqual(TEXT("an ordinary automatic effect stays open to players"),
		Migrated(EScope::Players, false, true), static_cast<int32>(EScope::Players));
	TestEqual(TEXT("an effect with neither is untouched"),
		Migrated(EScope::Players, false, false), static_cast<int32>(EScope::Players));

	// Idempotent: re-running it on an already-migrated value is a no-op, and it never overwrites a scope the
	// author has since chosen.
	TestEqual(TEXT("re-running it on the migrated value is stable"),
		Migrated(EScope::ServerOnly, true, true), static_cast<int32>(EScope::ServerOnly));
	TestEqual(TEXT("an authored internal scope survives the legacy flag"),
		Migrated(EScope::OtherEffectsOnly, false, true), static_cast<int32>(EScope::OtherEffectsOnly));

	TestEqual(TEXT("players maps to the server's player scope"),
		UCrowdyEffect::CallableFromToWireString(EScope::Players), FString(TEXT("player")));
	TestEqual(TEXT("server only maps to server"),
		UCrowdyEffect::CallableFromToWireString(EScope::ServerOnly), FString(TEXT("server")));
	TestEqual(TEXT("other effects only maps to internal"),
		UCrowdyEffect::CallableFromToWireString(EScope::OtherEffectsOnly), FString(TEXT("internal")));

	TestTrue(TEXT("no declared return type is the empty wire string"),
		UCrowdyEffect::ReturnTypeToWireString(ECrowdyEffectReturnType::None).IsEmpty());
	TestEqual(TEXT("int maps to int"),
		UCrowdyEffect::ReturnTypeToWireString(ECrowdyEffectReturnType::Int), FString(TEXT("int")));
	TestEqual(TEXT("string maps to string"),
		UCrowdyEffect::ReturnTypeToWireString(ECrowdyEffectReturnType::String), FString(TEXT("string")));
	return true;
}

// PostLoad is where the migration actually runs on a real asset, and NewObject never calls it, so the pure mapping
// test above cannot see a regression in the block that invokes it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectPostLoadMigrationTest,
	"CrowdySDK.Effect.PostLoadAppliesCallableFromMigration", CrowdyEffectTestFlags)
bool FCrowdyEffectPostLoadMigrationTest::RunTest(const FString& Parameters)
{
	UCrowdyEffect* Effect = NewObject<UCrowdyEffect>();
	Effect->bAutonomousOnly = true;
	Effect->bRunAutomatically = true;

	Effect->PostLoad();
	TestEqual(TEXT("the legacy pair migrates on load"), static_cast<int32>(Effect->CallableFrom),
		static_cast<int32>(ECrowdyEffectCallableFrom::ServerOnly));
	TestTrue(TEXT("and records that it migrated"), Effect->bInvokeScopeMigrated);

	// The author changes their mind. A second load must not stomp the scope back, which is what the guard is for.
	Effect->CallableFrom = ECrowdyEffectCallableFrom::Players;
	Effect->PostLoad();
	TestEqual(TEXT("a later load leaves an authored scope alone"), static_cast<int32>(Effect->CallableFrom),
		static_cast<int32>(ECrowdyEffectCallableFrom::Players));

	// The flag without the automation was inert before the control existed, so it stays inert.
	UCrowdyEffect* Inert = NewObject<UCrowdyEffect>();
	Inert->bAutonomousOnly = true;
	Inert->PostLoad();
	TestEqual(TEXT("the flag alone does not close the effect"), static_cast<int32>(Inert->CallableFrom),
		static_cast<int32>(ECrowdyEffectCallableFrom::Players));
	return true;
}

// A question changes nothing, so it must not tell every peer that a container changed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectQueryEffectSkipsNotificationTest,
	"CrowdySDK.Effect.QueryEffectAuthorsNoNotification", CrowdyEffectTestFlags)
bool FCrowdyEffectQueryEffectSkipsNotificationTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.NotificationCarrier = ECrowdyModelNotificationCarrier::Channel;
	Ctx.ReturnType = TEXT("bool");

	TestEqual(TEXT("a return-only effect authors no model-changed notification"),
		LowerScript(TEXT("return self.mana > 0"), Ctx).Function.Notifications.Num(), 0);
	TestEqual(TEXT("a policy-gate-only effect authors none either"),
		LowerScript(TEXT("require owner"), Ctx).Function.Notifications.Num(), 0);
	TestEqual(TEXT("an effect that writes still authors one"),
		LowerScript(TEXT("self.hp -= 1\nreturn self.hp"), Ctx).Function.Notifications.Num(), 1);
	return true;
}

// A helper an automation may run has a caller, so it gets the gate that caller passes. Emitting nothing would send
// an explicit null that clears whatever policy the server function already carried.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectInternalAutomationPolicyTest,
	"CrowdySDK.Effect.InternalAutomationGetsAutomationPolicy", CrowdyEffectTestFlags)
bool FCrowdyEffectInternalAutomationPolicyTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.InvokeScope = TEXT("internal");
	Ctx.bAutonomousInvocable = true;

	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("self.hp -= 1"), Ctx);
	TestFalse(TEXT("no errors"), R.HasErrors());
	TestEqual(TEXT("the automation gate is emitted"), R.Function.InvokePolicyJson,
		FString(TEXT("{\"type\":\"is_automation\"}")));
	TestTrue(TEXT("and the pairing is flagged as undocumented"),
		AnyDiagContains(R.Diagnostics, ECrowdyEffectSeverity::Warning, TEXT("documented scope for an automation")));

	// A pure fn: helper has no caller to gate, so it still emits nothing.
	FCrowdyEffectLoweringContext Helper = MakeHeroContext();
	Helper.InvokeScope = TEXT("internal");
	Helper.ReturnType = TEXT("int");
	TestTrue(TEXT("a pure helper still emits no policy"),
		LowerScript(TEXT("return self.level"), Helper).Function.InvokePolicyJson.IsEmpty());

	// An explicit require still wins over either inference.
	Ctx.ReturnType = TEXT("int");
	TestEqual(TEXT("an explicit require overrides the automation gate"),
		LowerScript(TEXT("require host\nreturn self.level"), Ctx).Function.InvokePolicyJson,
		FString(TEXT("{\"type\":\"is_host\"}")));
	return true;
}

// The disclosure warning is the only author-time control over the new return route, so it must not be dodged by
// re-spelling the read.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectReturnDisclosureCoverageTest,
	"CrowdySDK.Effect.ReturnDisclosureCoversEveryRead", CrowdyEffectTestFlags)
bool FCrowdyEffectReturnDisclosureCoverageTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	FCrowdyAttributeDef Gold = MakeAttr(TEXT("Gold"), TEXT("gold"), TEXT("int"));
	Gold.Visibility = TEXT("owner");
	Ctx.Attributes.Add(Gold);
	Ctx.Magnitudes.Add(MakeParam(TEXT("target_id"), TEXT("container_ref")));

	// ref($source_id).gold is the exact string a source.gold read lowers to, so it cannot be the quiet spelling.
	TestTrue(TEXT("a read through an explicit ref warns"),
		AnyDiagContains(LowerScript(TEXT("return ref($target_id).gold"), Ctx).Diagnostics,
			ECrowdyEffectSeverity::Warning, TEXT("owner-visible")));

	TestTrue(TEXT("a raw escape mentioning the key warns"),
		AnyDiagContains(LowerScript(TEXT("return raw(\"self.gold\")"), Ctx).Diagnostics,
			ECrowdyEffectSeverity::Warning, TEXT("raw expression mentioning 'gold'")));

	TestTrue(TEXT("returning grid state warns"),
		AnyDiagContains(LowerScript(TEXT("return grid_at(1, 2, 3, \"first\")"), Ctx).Diagnostics,
			ECrowdyEffectSeverity::Warning, TEXT("reads grid state")));

	// A grid read that only gates a policy is not a disclosure: nothing leaves the server.
	TestFalse(TEXT("a grid read in a require does not warn"),
		AnyDiagContains(LowerScript(TEXT("require grid_at(1, 2, 3, \"first\") == 0"), Ctx).Diagnostics,
			ECrowdyEffectSeverity::Warning, TEXT("reads grid state")));
	return true;
}

// A declared type the caller would decode wrongly is worth saying out loud, and an unknown attribute must not be
// answered with advice about return types.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectReturnTypeMismatchTest,
	"CrowdySDK.Effect.ReturnTypeMismatchWarns", CrowdyEffectTestFlags)
bool FCrowdyEffectReturnTypeMismatchTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.ReturnType = TEXT("int");

	TestTrue(TEXT("declaring int while returning a string attribute warns"),
		AnyDiagContains(LowerScript(TEXT("return self.title"), Ctx).Diagnostics,
			ECrowdyEffectSeverity::Warning, TEXT("but returns 'title'")));
	TestFalse(TEXT("declaring int while returning an int attribute does not"),
		AnyDiagContains(LowerScript(TEXT("return self.hp"), Ctx).Diagnostics,
			ECrowdyEffectSeverity::Warning, TEXT("but returns")));

	// Widening an int answer to float is a deliberate choice, so it stays silent.
	FCrowdyEffectLoweringContext Widened = MakeHeroContext();
	Widened.ReturnType = TEXT("float");
	TestFalse(TEXT("widening an int to float does not warn"),
		AnyDiagContains(LowerScript(TEXT("return self.hp"), Widened).Diagnostics,
			ECrowdyEffectSeverity::Warning, TEXT("but returns")));

	// A front-end that passes a type outside the vocabulary is a programming error, not something to ship.
	FCrowdyEffectLoweringContext Bogus = MakeHeroContext();
	Bogus.ReturnType = TEXT("container_ref");
	const FCrowdyEffectLoweringResult BogusResult = LowerScript(TEXT("return self.hp"), Bogus);
	TestTrue(TEXT("an unsupported return type is rejected"),
		AnyDiagContains(BogusResult.Diagnostics, ECrowdyEffectSeverity::Error, TEXT("is not a return type")));
	TestTrue(TEXT("and nothing is declared on the wire"), BogusResult.Function.ReturnType.IsEmpty());

	// The unknown-attribute error already names the fix; a second warning would point at the wrong one.
	TestFalse(TEXT("an unknown attribute does not also warn about the return type"),
		AnyDiagContains(LowerScript(TEXT("return self.stamina"), MakeHeroContext()).Diagnostics,
			ECrowdyEffectSeverity::Warning, TEXT("declares no return type")));
	return true;
}

namespace
{
	// Renders a parsed value with its object members in name order. The two policy emitters write the same members
	// in different orders, and neither order is meaningful, so comparing them needs the names sorted; array order
	// is left alone, since a policy's rule order is part of what it says.
	FString StableJson(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return TEXT("null");
		}
		switch (Value->Type)
		{
		case EJson::Object:
		{
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			TArray<TPair<FString, TSharedPtr<FJsonValue>>> Members;
			for (const auto& Pair : Object->Values)
			{
				Members.Emplace(FString(*Pair.Key), Pair.Value);
			}
			Members.Sort([](const TPair<FString, TSharedPtr<FJsonValue>>& A,
				const TPair<FString, TSharedPtr<FJsonValue>>& B) { return A.Key < B.Key; });
			FString Out = TEXT("{");
			for (int32 Index = 0; Index < Members.Num(); ++Index)
			{
				if (Index > 0)
				{
					Out += TEXT(",");
				}
				Out += TEXT("\"") + Members[Index].Key + TEXT("\":") + StableJson(Members[Index].Value);
			}
			return Out + TEXT("}");
		}
		case EJson::Array:
		{
			const TArray<TSharedPtr<FJsonValue>>& Elements = Value->AsArray();
			FString Out = TEXT("[");
			for (int32 Index = 0; Index < Elements.Num(); ++Index)
			{
				if (Index > 0)
				{
					Out += TEXT(",");
				}
				Out += StableJson(Elements[Index]);
			}
			return Out + TEXT("]");
		}
		case EJson::String:
		{
			// Re-escaping matters: the parser removed the escapes, so a string containing a quote would otherwise
			// re-render as extra members and two different objects could canonicalize to the same text.
			FString Escaped = Value->AsString();
			Escaped.ReplaceInline(TEXT("\\"), TEXT("\\\\"), ESearchCase::CaseSensitive);
			Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""), ESearchCase::CaseSensitive);
			return TEXT("\"") + Escaped + TEXT("\"");
		}
		case EJson::Number:
		{
			// SanitizeFloat renders 1 and 1.0 alike and loses precision past 2^53, so an integral value is printed
			// as an integer instead.
			const double Number = Value->AsNumber();
			if (FMath::IsFinite(Number) && Number == FMath::RoundToDouble(Number) && FMath::Abs(Number) < 1e15)
			{
				return FString::Printf(TEXT("%lld"), static_cast<int64>(Number));
			}
			return FString::SanitizeFloat(Number);
		}
		case EJson::Boolean: return Value->AsBool() ? TEXT("true") : TEXT("false");
		default:             return TEXT("null");
		}
	}

	// A policy JSON string rendered so two emitters' output compares on content rather than member order.
	FString CanonicalPolicyJson(const FString& In)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(In);
		if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
		{
			return In;
		}
		return StableJson(MakeShared<FJsonValueObject>(Object));
	}

	// The inventory kit's ItemStack attributes, so an effect can be authored against the same vocabulary the
	// hand-built kit function speaks.
	FCrowdyEffectLoweringContext MakeItemStackContext()
	{
		FCrowdyEffectLoweringContext Ctx;
		Ctx.ContainerTypeName = CrowdyKit::InventoryNames().StackType;
		Ctx.Attributes.Add(MakeAttr(TEXT("ItemId"), TEXT("item_id"), TEXT("string")));
		Ctx.Attributes.Add(MakeAttr(TEXT("OwnerUserId"), TEXT("owner_user_id"), TEXT("int")));
		Ctx.Attributes.Add(MakeAttr(TEXT("Quantity"), TEXT("quantity"), TEXT("int")));
		Ctx.Attributes.Add(MakeAttr(TEXT("Slot"), TEXT("slot"), TEXT("int")));
		Ctx.Magnitudes.Add(MakeParam(TEXT("amount"), TEXT("int")));
		return Ctx;
	}

	const TSharedPtr<FJsonObject>* FindKitFunction(const FCrowdyKitBlueprint& Blueprint, const FString& Name)
	{
		return Blueprint.Functions.FindByPredicate([&Name](const TSharedPtr<FJsonObject>& Fn)
		{
			return Fn.IsValid() && CrowdyKit::BlueprintField(Fn, TEXT("name")) == Name;
		});
	}
}

// The parity golden: the same two functions the inventory kit hand-builds as raw JSON, re-authored as EffectScript,
// must lower field for field to what the kit ships. The kit is the only Game Model function shape this SDK has that
// is known to work against a real server, so it is the reference a return has to reproduce.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectKitParityGoldenTest,
	"CrowdySDK.Effect.InventoryKitParityGolden", CrowdyEffectTestFlags)
bool FCrowdyEffectKitParityGoldenTest::RunTest(const FString& Parameters)
{
	const FCrowdyKitInventoryNames Names = CrowdyKit::InventoryNames();
	const FCrowdyKitBlueprint Kit = CrowdyKit::InventoryBlueprint();

	struct FCase
	{
		FString FunctionName;
		FString Description;
		FString Script;
	};
	const TArray<FCase> Cases = {
		{ Names.GrantFn, TEXT("Add items to a stack the caller owns."),
			TEXT("self.quantity = self.quantity + max(0, $amount)\nreturn self.quantity") },
		// The kit's guard is one condition holding both terms. A top-level '&&' in a require lowers to two separate
		// policy rules instead, which gates identically but is a different tree, so the operand escape is what
		// reproduces the kit's exact shape here. The equivalence of the plain spelling is asserted below.
		{ Names.ConsumeFn, TEXT("Spend items from a stack; refuses to overdraw."),
			TEXT("require owner\nrequire raw(\"$amount > 0 && self.quantity >= $amount\")\n"
				"self.quantity = self.quantity - $amount\nreturn self.quantity") }
	};

	for (const FCase& Case : Cases)
	{
		const TSharedPtr<FJsonObject>* KitFn = FindKitFunction(Kit, Case.FunctionName);
		if (!KitFn)
		{
			AddError(FString::Printf(TEXT("the inventory kit no longer ships '%s'"), *Case.FunctionName));
			continue;
		}

		FCrowdyEffectLoweringContext Ctx = MakeItemStackContext();
		Ctx.FunctionName = Case.FunctionName;
		Ctx.Description = Case.Description;

		const FCrowdyEffectLoweringResult R = LowerScript(Case.Script, Ctx);
		if (!TestFalse(FString::Printf(TEXT("'%s' compiles clean"), *Case.FunctionName), R.HasErrors()))
		{
			continue;
		}
		const FCrowdyGameModelFunctionInput& Fn = R.Function;

		TestEqual(TEXT("name"), Fn.Name, CrowdyKit::BlueprintField(*KitFn, TEXT("name")));
		TestEqual(TEXT("container type"), Fn.ContainerTypeName,
			CrowdyKit::BlueprintField(*KitFn, TEXT("containerTypeName")));
		TestEqual(TEXT("description"), Fn.Description, CrowdyKit::BlueprintField(*KitFn, TEXT("description")));
		TestEqual(TEXT("return type"), Fn.ReturnType, CrowdyKit::BlueprintField(*KitFn, TEXT("returnType")));
		TestEqual(TEXT("return expression"), Fn.ReturnExpression,
			CrowdyKit::BlueprintField(*KitFn, TEXT("returnExpression")));
		TestEqual(TEXT("invoke policy"), CanonicalPolicyJson(Fn.InvokePolicyJson),
			CanonicalPolicyJson(CrowdyKit::BlueprintField(*KitFn, TEXT("invokePolicyJson"))));

		// The kit declares no scope and no autonomous flag on either of these, which is the server's player default.
		TestTrue(TEXT("the kit declares no invoke scope"), CrowdyKit::BlueprintField(*KitFn, TEXT("invokeScope")).IsEmpty());
		TestEqual(TEXT("so the effect stays player-scoped"), Fn.InvokeScope, FString(TEXT("player")));
		TestFalse(TEXT("and is not autonomous-invocable"), Fn.bAutonomousInvocable);

		const TArray<TSharedPtr<FJsonValue>>* KitParams = nullptr;
		// A missing array has to fail rather than skip the comparison: silently asserting nothing about the
		// parameters or the mutations is exactly the false green this golden exists to prevent.
		if (TestTrue(TEXT("the kit function declares parameters"),
			(*KitFn)->TryGetArrayField(TEXT("parameters"), KitParams) && KitParams != nullptr))
		{
			if (TestEqual(TEXT("parameter count"), Fn.Parameters.Num(), KitParams->Num()))
			{
				for (int32 Index = 0; Index < Fn.Parameters.Num(); ++Index)
				{
					const TSharedPtr<FJsonObject> KitParam = (*KitParams)[Index]->AsObject();
					TestEqual(TEXT("parameter name"), Fn.Parameters[Index].Name,
						CrowdyKit::BlueprintField(KitParam, TEXT("name")));
					TestEqual(TEXT("parameter type"), Fn.Parameters[Index].ValueType,
						CrowdyKit::BlueprintField(KitParam, TEXT("valueType")));
					bool bKitRequired = false;
					KitParam->TryGetBoolField(TEXT("required"), bKitRequired);
					TestEqual(TEXT("parameter required"), Fn.Parameters[Index].bRequired, bKitRequired);
				}
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* KitMutations = nullptr;
		if (TestTrue(TEXT("the kit function declares mutations"),
			(*KitFn)->TryGetArrayField(TEXT("mutations"), KitMutations) && KitMutations != nullptr))
		{
			if (TestEqual(TEXT("mutation count"), Fn.Mutations.Num(), KitMutations->Num()))
			{
				for (int32 Index = 0; Index < Fn.Mutations.Num(); ++Index)
				{
					const TSharedPtr<FJsonObject> KitMutation = (*KitMutations)[Index]->AsObject();
					TestEqual(TEXT("mutation target"), Fn.Mutations[Index].Target,
						CrowdyKit::BlueprintField(KitMutation, TEXT("target")));
					TestEqual(TEXT("mutation property"), Fn.Mutations[Index].Property,
						CrowdyKit::BlueprintField(KitMutation, TEXT("property")));
					TestEqual(TEXT("mutation expression"), Fn.Mutations[Index].Expression,
						CrowdyKit::BlueprintField(KitMutation, TEXT("expression")));
				}
			}
		}

		TestEqual(TEXT("no notifications"), Fn.Notifications.Num(), 0);
		TestEqual(TEXT("no timers"), Fn.Timers.Num(), 0);
	}

	// The plain spelling of the same guard: everything but the policy tree still matches the kit byte for byte, and
	// the policy differs only by splitting one condition into two rules of the same and-gate.
	FCrowdyEffectLoweringContext Plain = MakeItemStackContext();
	Plain.FunctionName = Names.ConsumeFn;
	const FCrowdyEffectLoweringResult PlainResult = LowerScript(
		TEXT("require owner\nrequire $amount > 0 && self.quantity >= $amount\n"
			"self.quantity = self.quantity - $amount\nreturn self.quantity"), Plain);
	TestFalse(TEXT("the plain spelling compiles clean"), PlainResult.HasErrors());
	TestEqual(TEXT("with the same return"), PlainResult.Function.ReturnExpression, FString(TEXT("self.quantity")));
	TestEqual(TEXT("and the same guard, split across two rules"), PlainResult.Function.InvokePolicyJson,
		FString(TEXT("{\"type\":\"and\",\"rules\":[{\"type\":\"owner_of_self\"},{\"type\":\"and\",\"rules\":"
			"[{\"type\":\"condition\",\"expression\":\"$amount > 0\"},"
			"{\"type\":\"condition\",\"expression\":\"self.quantity >= $amount\"}]}]}")));
	return true;
}

// A structured spec with bHasReturn set and an Attribute operand lowers to the same ReturnType / ReturnExpression
// as the equivalent EffectScript text, matching the pattern the existing structured-versus-text goldens use.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSpecReturnMatchesTextTest,
	"CrowdySDK.Effect.SpecReturnMatchesText", CrowdyEffectTestFlags)
bool FCrowdyEffectSpecReturnMatchesTextTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.Magnitudes.Add(MakeParam(TEXT("amount"), TEXT("int")));

	FCrowdyEffectSpec Spec = OneAssignmentSpec(
		ECrowdyEffectRole::Target, TEXT("hp"), ECrowdyEffectAssignmentOp::Subtract, MagOperand(TEXT("amount")));
	Spec.bHasReturn = true;
	Spec.Return = AttrOperand(ECrowdyEffectRole::Target, TEXT("hp"));

	const FCrowdyEffectLoweringResult Structured = LowerSpec(Spec, Ctx);
	const FCrowdyEffectLoweringResult Text = LowerScript(TEXT("self.hp -= $amount\nreturn self.hp"), Ctx);

	TestFalse(TEXT("no errors"), Structured.HasErrors());
	TestEqual(TEXT("structured return expression matches text"),
		Structured.Function.ReturnExpression, Text.Function.ReturnExpression);
	TestEqual(TEXT("structured return type matches text"),
		Structured.Function.ReturnType, Text.Function.ReturnType);
	TestEqual(TEXT("both read self.hp"), Structured.Function.ReturnExpression, FString(TEXT("self.hp")));
	return true;
}

// bHasReturn left false leaves the program's ReturnExpr slot unset and the lowered function carries no return
// fields at all: the additive path must be a no-op when a spec does not opt in.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSpecNoReturnLeavesLoweringUnchangedTest,
	"CrowdySDK.Effect.SpecNoReturnLeavesLoweringUnchanged", CrowdyEffectTestFlags)
bool FCrowdyEffectSpecNoReturnLeavesLoweringUnchangedTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.Magnitudes.Add(MakeParam(TEXT("amount"), TEXT("int")));

	FCrowdyEffectSpec Spec = OneAssignmentSpec(
		ECrowdyEffectRole::Target, TEXT("hp"), ECrowdyEffectAssignmentOp::Subtract, MagOperand(TEXT("amount")));
	// bHasReturn defaults to false; Spec.Return is left as its untouched default (a Number with an empty literal).

	TArray<FCrowdyEffectDiagnostic> Diags;
	const FCrowdyEffectProgram Program = FCrowdyEffectSpecBuilder::BuildProgram(Spec, Diags);
	TestFalse(TEXT("no diagnostics from the unused default operand"), Program.ReturnExpr.IsValid());

	const FCrowdyEffectLoweringResult Structured = LowerSpec(Spec, Ctx);
	const FCrowdyEffectLoweringResult Text = LowerScript(TEXT("self.hp -= $amount"), Ctx);

	TestFalse(TEXT("no errors"), Structured.HasErrors());
	TestTrue(TEXT("no return type on the wire"), Structured.Function.ReturnType.IsEmpty());
	TestTrue(TEXT("no return expression on the wire"), Structured.Function.ReturnExpression.IsEmpty());
	TestEqual(TEXT("byte-identical mutation to before"),
		Structured.Function.Mutations[0].Expression, Text.Function.Mutations[0].Expression);
	return true;
}

// A return operand that cannot build (an attribute operand with no attribute name) records a diagnostic instead of
// silently producing no return: bHasReturn was true, so the author asked for one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSpecInvalidReturnRecordsDiagnosticTest,
	"CrowdySDK.Effect.SpecInvalidReturnRecordsDiagnostic", CrowdyEffectTestFlags)
bool FCrowdyEffectSpecInvalidReturnRecordsDiagnosticTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectSpec Spec = OneAssignmentSpec(
		ECrowdyEffectRole::Target, TEXT("hp"), ECrowdyEffectAssignmentOp::Subtract, NumOperand(TEXT("1")));
	Spec.bHasReturn = true;
	Spec.Return = AttrOperand(ECrowdyEffectRole::Target, TEXT("")); // no attribute name

	TArray<FCrowdyEffectDiagnostic> Diags;
	const FCrowdyEffectProgram Program = FCrowdyEffectSpecBuilder::BuildProgram(Spec, Diags);

	TestFalse(TEXT("the return slot is left unset"), Program.ReturnExpr.IsValid());
	TestTrue(TEXT("a diagnostic is recorded for the bad return operand"),
		AnyDiagContains(Diags, ECrowdyEffectSeverity::Error, TEXT("no attribute name")));

	// A raw operand with no text is the one shape that used to build a perfectly valid node emitting nothing. That
	// reads downstream as an authored, blank answer: the type is still declared, every "you declared a return but
	// wrote none" diagnostic is skipped because a return does exist, and the empty expression is sent to the server
	// as an explicit null that clears whatever the function returned before.
	FCrowdyEffectSpec RawSpec = OneAssignmentSpec(
		ECrowdyEffectRole::Target, TEXT("hp"), ECrowdyEffectAssignmentOp::Subtract, NumOperand(TEXT("1")));
	RawSpec.bHasReturn = true;
	RawSpec.Return.Kind = ECrowdyEffectOperandKind::Raw;
	RawSpec.Return.Literal = TEXT("   ");

	TArray<FCrowdyEffectDiagnostic> RawDiags;
	const FCrowdyEffectProgram RawProgram = FCrowdyEffectSpecBuilder::BuildProgram(RawSpec, RawDiags);

	TestFalse(TEXT("an empty raw return leaves the slot unset"), RawProgram.ReturnExpr.IsValid());
	TestTrue(TEXT("and is reported rather than emitted as a blank answer"),
		AnyDiagContains(RawDiags, ECrowdyEffectSeverity::Error, TEXT("a raw operand has no expression text")));

	// A raw operand that does carry text is still spliced through untouched.
	RawSpec.Return.Literal = TEXT("self.hp + 1");
	TArray<FCrowdyEffectDiagnostic> GoodRawDiags;
	TestTrue(TEXT("a raw return with text still builds"),
		FCrowdyEffectSpecBuilder::BuildProgram(RawSpec, GoodRawDiags).ReturnExpr.IsValid());
	return true;
}

// The graph compiler emits Expression-kind operands (an EffectScript string produced from the wired graph), so
// that is the kind the real S2 path exercises: an Expression return builds and lowers like any other expression.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSpecExpressionReturnLowersTest,
	"CrowdySDK.Effect.SpecExpressionReturnLowers", CrowdyEffectTestFlags)
bool FCrowdyEffectSpecExpressionReturnLowersTest::RunTest(const FString& Parameters)
{
	const FCrowdyEffectLoweringContext Ctx = MakeHeroContext();

	FCrowdyEffectSpec Spec;
	Spec.bHasReturn = true;
	Spec.Return = ExprOperand(TEXT("self.mana >= self.level"));

	const FCrowdyEffectLoweringResult Structured = LowerSpec(Spec, Ctx);
	const FCrowdyEffectLoweringResult Text = LowerScript(TEXT("return self.mana >= self.level"), Ctx);

	TestFalse(TEXT("no errors"), Structured.HasErrors());
	TestEqual(TEXT("no mutations"), Structured.Function.Mutations.Num(), 0);
	TestEqual(TEXT("expression-kind return matches the equivalent text"),
		Structured.Function.ReturnExpression, Text.Function.ReturnExpression);
	TestEqual(TEXT("same declared/inferred type"), Structured.Function.ReturnType, Text.Function.ReturnType);
	return true;
}

// What an editor needs to underline a problem as it is typed. The parser alone cannot find any of these: they all
// need the container's attribute vocabulary, so an editor that only parses reports a body as clean when it is not.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectDiagnoseScriptBodyTest,
	"CrowdySDK.Effect.DiagnoseScriptBodyFindsSemanticErrors", CrowdyEffectTestFlags)
bool FCrowdyEffectDiagnoseScriptBodyTest::RunTest(const FString& Parameters)
{
	// The discovery fixture deliberately carries two rejected properties and re-logs them on every discovery pass,
	// so the count is "one or more" rather than a fixed number.
	AddExpectedError(TEXT("marked both CrowdyModel and CrowdyState"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("must be parameterless"), EAutomationExpectedErrorFlags::Contains, 0);

	UCrowdyEffect* Effect = NewObject<UCrowdyEffect>(GetTransientPackage());
	Effect->ContainerClass = UCrowdyGameModelDiscoveryTarget::StaticClass();
	Effect->Source = ECrowdyEffectSource::Text;

	// The fixture declares "Health", whose server key is "health". Both spellings resolve on their own, so nothing
	// short of the whole-effect check notices that one effect used both.
	TestTrue(TEXT("mixing the server key and the declared name is reported"),
		AnyDiagContains(Effect->DiagnoseScriptBody(TEXT("self.health += 1\nreturn self.Health")),
			ECrowdyEffectSeverity::Error, TEXT("two spellings of the same attribute")));

	TestFalse(TEXT("one spelling throughout is clean"),
		AnyDiagContains(Effect->DiagnoseScriptBody(TEXT("self.Health += 1\nreturn self.Health")),
			ECrowdyEffectSeverity::Error, TEXT("two spellings")));

	TestTrue(TEXT("an attribute the container does not declare is reported"),
		AnyDiagContains(Effect->DiagnoseScriptBody(TEXT("self.stamina += 1")),
			ECrowdyEffectSeverity::Error, TEXT("unknown attribute")));

	// A body mid-edit does not parse, and the parser already says so. Lowering a half-built program on top would
	// bury that under errors about statements the author has not finished typing.
	TestEqual(TEXT("an unparseable body is left to the parser"),
		Effect->DiagnoseScriptBody(TEXT("self.Health +=")).Num(), 0);

	// Without a container class there is no vocabulary, so every attribute would look unknown. Report nothing
	// rather than covering the body in errors the author cannot act on.
	UCrowdyEffect* Unbound = NewObject<UCrowdyEffect>(GetTransientPackage());
	Unbound->Source = ECrowdyEffectSource::Text;
	TestEqual(TEXT("no container class means no semantic answer"),
		Unbound->DiagnoseScriptBody(TEXT("self.Health += 1")).Num(), 0);
	return true;
}

namespace
{
	// One entry in a stubbed catalog. The two flags are independent: a query effect returns and writes nothing, an
	// ordinary effect writes and returns nothing, and a mutate-then-return effect does both.
	struct FKnownCallee
	{
		FString Name;
		bool bAuthorsReturn = false;
		bool bHasMutations = false;
	};

	// Builds a FnCalleeLookup that answers only for the given entries; any other queried name is unknown to the
	// project (the lookup returns false), matching how a real catalog treats a hand-authored server function or a
	// kit function that is not a Crowdy Effect asset.
	TFunction<bool(const FString&, FCrowdyEffectFnCallee&)> MakeFnCalleeLookup(const TArray<FKnownCallee>& Known)
	{
		return [Known](const FString& Name, FCrowdyEffectFnCallee& OutCallee) -> bool
		{
			for (const FKnownCallee& Entry : Known)
			{
				if (Entry.Name.Equals(Name, ESearchCase::CaseSensitive))
				{
					OutCallee.FunctionName = Name;
					OutCallee.bAuthorsReturn = Entry.bAuthorsReturn;
					OutCallee.bHasMutations = Entry.bHasMutations;
					return true;
				}
			}
			return false;
		};
	}
}

// A fn: call naming a function the project knows about that authors a return produces no diagnostic: there is
// something for the call to read.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectFnCallKnownReturningTest,
	"CrowdySDK.Effect.FnCallToKnownReturningFunctionIsSilent", CrowdyEffectTestFlags)
bool FCrowdyEffectFnCallKnownReturningTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.FnCalleeLookup = MakeFnCalleeLookup({ { TEXT("Bonus"), true } });

	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("self.hp += fn:Bonus()"), Ctx);

	TestFalse(TEXT("no errors"), R.HasErrors());
	TestEqual(TEXT("no diagnostics at all: the callee returns a value"), R.Diagnostics.Num(), 0);
	return true;
}

// A fn: call naming a function the project knows about but which authors no return warns exactly once, and
// never as an error: the server decides whether the call is allowed, the SDK only warns that it has nothing
// to read.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectFnCallKnownReturnlessTest,
	"CrowdySDK.Effect.FnCallToKnownReturnlessFunctionWarnsOnce", CrowdyEffectTestFlags)
bool FCrowdyEffectFnCallKnownReturnlessTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.FnCalleeLookup = MakeFnCalleeLookup({ { TEXT("SilentHelper"), false } });

	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("self.hp += fn:SilentHelper()"), Ctx);

	TestFalse(TEXT("never an error: the server decides, the SDK only warns"), R.HasErrors());
	int32 WarningCount = 0;
	for (const FCrowdyEffectDiagnostic& D : R.Diagnostics)
	{
		if (D.Severity == ECrowdyEffectSeverity::Warning && D.Message.Contains(TEXT("SilentHelper")))
		{
			++WarningCount;
		}
	}
	TestEqual(TEXT("exactly one warning for the one call site"), WarningCount, 1);
	return true;
}

// A fn: call naming a function the project has never heard of is not itself a problem: it may legitimately name
// a hand-authored server function or a kit function that is not a Crowdy Effect asset, so it produces no
// diagnostic. Getting this backwards would warn on every kit-calling effect in the project.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectFnCallUnknownNameTest,
	"CrowdySDK.Effect.FnCallToUnknownNameProducesNoDiagnostic", CrowdyEffectTestFlags)
bool FCrowdyEffectFnCallUnknownNameTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.FnCalleeLookup = MakeFnCalleeLookup({ { TEXT("KnownOther"), true } });

	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("self.hp += fn:HandAuthoredKitFunction()"), Ctx);

	TestFalse(TEXT("no errors"), R.HasErrors());
	TestEqual(TEXT("no diagnostics: an unknown name is not evidence of a mistake"), R.Diagnostics.Num(), 0);
	return true;
}

// With no catalog available at all (no editor, a cooked build, a bare unit test that never sets FnCalleeLookup),
// lowering must emit no fn: diagnostics whatsoever, even for a program that is full of fn: calls.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectFnCallNoCatalogTest,
	"CrowdySDK.Effect.FnCallWithNoCatalogProducesNoDiagnostic", CrowdyEffectTestFlags)
bool FCrowdyEffectFnCallNoCatalogTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext(); // FnCalleeLookup deliberately left unset.
	Ctx.ReturnType = TEXT("int"); // avoids the unrelated untyped-return warning so the count below is exact.

	const FCrowdyEffectLoweringResult R = LowerScript(
		TEXT("self.hp += fn:A()\nrequire fn:B()\nreturn fn:C()"), Ctx);

	TestFalse(TEXT("no errors"), R.HasErrors());
	TestEqual(TEXT("no diagnostics: no catalog means no fn: diagnostics at all"), R.Diagnostics.Num(), 0);
	return true;
}

// A fn: call written inside a require condition is covered exactly like one in a mutation's right-hand side.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectFnCallInRequireTest,
	"CrowdySDK.Effect.FnCallInRequireConditionWarns", CrowdyEffectTestFlags)
bool FCrowdyEffectFnCallInRequireTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.FnCalleeLookup = MakeFnCalleeLookup({ { TEXT("CanClaim"), false } });

	const FCrowdyEffectLoweringResult R = LowerScript(
		TEXT("self.hp -= 1\nrequire fn:CanClaim()"), Ctx);

	TestFalse(TEXT("no errors"), R.HasErrors());
	TestTrue(TEXT("the require's fn: call is checked too"),
		AnyDiagContains(R.Diagnostics, ECrowdyEffectSeverity::Warning, TEXT("CanClaim")));
	return true;
}

// A fn: call nested inside another call's arguments is still visited: coverage is not limited to the top-level
// expression of a statement.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectFnCallNestedInArgsTest,
	"CrowdySDK.Effect.FnCallNestedInCallArgsWarns", CrowdyEffectTestFlags)
bool FCrowdyEffectFnCallNestedInArgsTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.FnCalleeLookup = MakeFnCalleeLookup({ { TEXT("Bonus"), false } });

	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("self.hp -= max(1, fn:Bonus())"), Ctx);

	TestFalse(TEXT("no errors"), R.HasErrors());
	TestTrue(TEXT("a fn: call nested inside a builtin's arguments is checked"),
		AnyDiagContains(R.Diagnostics, ECrowdyEffectSeverity::Warning, TEXT("Bonus")));
	return true;
}

// A fn: call written as the return expression itself is covered.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectFnCallInReturnTest,
	"CrowdySDK.Effect.FnCallInReturnExpressionWarns", CrowdyEffectTestFlags)
bool FCrowdyEffectFnCallInReturnTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.ReturnType = TEXT("int");
	Ctx.FnCalleeLookup = MakeFnCalleeLookup({ { TEXT("Foo"), false } });

	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("return fn:Foo()"), Ctx);

	TestFalse(TEXT("no errors"), R.HasErrors());
	TestTrue(TEXT("the return expression's own fn: call is checked"),
		AnyDiagContains(R.Diagnostics, ECrowdyEffectSeverity::Warning, TEXT("Foo")));
	return true;
}

// The warning is located at the line the fn: call was actually written on, the same way every other located
// diagnostic in this file is.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectFnCallWarningLocationTest,
	"CrowdySDK.Effect.FnCallWarningLocatedAtCallLine", CrowdyEffectTestFlags)
bool FCrowdyEffectFnCallWarningLocationTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.FnCalleeLookup = MakeFnCalleeLookup({ { TEXT("Foo"), false } });

	const FCrowdyEffectLoweringResult R = LowerScript(
		TEXT("self.hp -= 1\nself.mana -= fn:Foo()"), Ctx);

	TestFalse(TEXT("no errors"), R.HasErrors());
	if (const FCrowdyEffectDiagnostic* D = FindDiag(R.Diagnostics, ECrowdyEffectSeverity::Warning, TEXT("Foo")))
	{
		TestEqual(TEXT("located on the line the call was written on"), D->Line, 2);
	}
	else
	{
		AddError(TEXT("no fn: return-value warning was reported"));
	}
	return true;
}

// Adding this diagnostic must change no lowered bytes: a fn: call still emits the identical "fn:Name(args)"
// expression whether or not a catalog is available, and whether or not the callee returns anything.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectFnCallLoweredOutputUnchangedTest,
	"CrowdySDK.Effect.FnCallLoweringUnchangedByCatalog", CrowdyEffectTestFlags)
bool FCrowdyEffectFnCallLoweredOutputUnchangedTest::RunTest(const FString& Parameters)
{
	const FCrowdyEffectLoweringResult NoCatalog =
		LowerScript(TEXT("self.hp -= fn:Bonus(1, 2)"), MakeHeroContext());

	FCrowdyEffectLoweringContext WithCatalogCtx = MakeHeroContext();
	WithCatalogCtx.FnCalleeLookup = MakeFnCalleeLookup({ { TEXT("Bonus"), false } });
	const FCrowdyEffectLoweringResult WithCatalog = LowerScript(TEXT("self.hp -= fn:Bonus(1, 2)"), WithCatalogCtx);

	TestFalse(TEXT("no errors without a catalog"), NoCatalog.HasErrors());
	TestFalse(TEXT("no errors with a catalog"), WithCatalog.HasErrors());
	if (TestEqual(TEXT("one mutation either way"), NoCatalog.Function.Mutations.Num(), 1)
		&& WithCatalog.Function.Mutations.Num() == 1)
	{
		TestEqual(TEXT("the lowered expression is byte-identical whether or not a catalog is wired up"),
			NoCatalog.Function.Mutations[0].Expression, WithCatalog.Function.Mutations[0].Expression);
		TestEqual(TEXT("the golden fn: call expression"), WithCatalog.Function.Mutations[0].Expression,
			FString(TEXT("max(0, min(100, self.hp - (fn:Bonus(1, 2))))")));
	}
	TestTrue(TEXT("the catalog run additionally warns (proving the check actually ran)"),
		AnyDiagContains(WithCatalog.Diagnostics, ECrowdyEffectSeverity::Warning, TEXT("Bonus")));
	TestEqual(TEXT("the no-catalog run has no diagnostics"), NoCatalog.Diagnostics.Num(), 0);
	return true;
}

// A fn: call reads the callee's return value and runs none of its writes, which nothing at the call site reveals.
// A callee that both returns and writes is therefore warned about even though the call does answer with a value.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectFnCallMutatingCalleeTest,
	"CrowdySDK.Effect.FnCallToMutatingFunctionWarnsWritesDoNotRun", CrowdyEffectTestFlags)
bool FCrowdyEffectFnCallMutatingCalleeTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.FnCalleeLookup = MakeFnCalleeLookup({ { TEXT("ApplyDamage"), true, true } });

	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("self.hp += fn:ApplyDamage()"), Ctx);

	TestFalse(TEXT("never an error: the call is legal, it just does less than it looks like"), R.HasErrors());
	TestTrue(TEXT("the author is told the callee's writes do not run here"),
		AnyDiagContains(R.Diagnostics, ECrowdyEffectSeverity::Warning, TEXT("is not changed here")));
	TestFalse(TEXT("no returnless warning: this callee does return a value"),
		AnyDiagContains(R.Diagnostics, ECrowdyEffectSeverity::Warning, TEXT("returns nothing")));
	return true;
}

// The two fn: complaints are independent, so a callee that neither returns nor can write here earns both lines
// rather than one masking the other.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectFnCallReturnlessMutatingCalleeTest,
	"CrowdySDK.Effect.FnCallToReturnlessMutatingFunctionWarnsBoth", CrowdyEffectTestFlags)
bool FCrowdyEffectFnCallReturnlessMutatingCalleeTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.FnCalleeLookup = MakeFnCalleeLookup({ { TEXT("Heal"), false, true } });

	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("self.hp += fn:Heal()"), Ctx);

	TestFalse(TEXT("never an error"), R.HasErrors());
	TestTrue(TEXT("it has nothing to read"),
		AnyDiagContains(R.Diagnostics, ECrowdyEffectSeverity::Warning, TEXT("returns nothing")));
	TestTrue(TEXT("and its writes do not run here"),
		AnyDiagContains(R.Diagnostics, ECrowdyEffectSeverity::Warning, TEXT("is not changed here")));
	return true;
}

// A callee that returns a value and writes nothing is exactly what fn: is for, so it stays silent. This is the
// case that would break if the mutation warning ever keyed off something other than the callee's own writes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectFnCallPureQueryCalleeTest,
	"CrowdySDK.Effect.FnCallToPureQueryFunctionIsSilent", CrowdyEffectTestFlags)
bool FCrowdyEffectFnCallPureQueryCalleeTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.FnCalleeLookup = MakeFnCalleeLookup({ { TEXT("XpForLevel"), true, false } });

	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("self.hp += fn:XpForLevel()"), Ctx);

	TestFalse(TEXT("no errors"), R.HasErrors());
	TestEqual(TEXT("no diagnostics: a pure returning helper is the shape fn: exists for"), R.Diagnostics.Num(), 0);
	return true;
}

// What the project-wide function catalog reports about every effect asset comes from this one function, so the
// two flags are pinned per authoring surface here rather than only through a stubbed catalog. Both answers come
// off the same program the compiler lowers, so this also pins that the surfaces agree with each other. Text is
// covered; the Graph surface goes through the editor graph compiler and is covered by the editor tests.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectAuthoredShapeTest,
	"CrowdySDK.Effect.AuthoredShapeReadsTheSurface", CrowdyEffectTestFlags)
bool FCrowdyEffectAuthoredShapeTest::RunTest(const FString& Parameters)
{
	UCrowdyEffect* Effect = NewObject<UCrowdyEffect>(GetTransientPackage());
	Effect->Source = ECrowdyEffectSource::Text;

	Effect->EffectScript = TEXT("self.Health += 1");
	FCrowdyEffectAuthoredShape Shape = Effect->GetAuthoredShape();
	TestFalse(TEXT("a write-only effect authors no return"), Shape.bAuthorsReturn);
	TestTrue(TEXT("a write-only effect has mutations"), Shape.bHasMutations);

	// The query shape: it answers a question and changes nothing, which is what makes it a fn: callee worth having.
	Effect->EffectScript = TEXT("return self.Health");
	Shape = Effect->GetAuthoredShape();
	TestTrue(TEXT("a query effect authors a return"), Shape.bAuthorsReturn);
	TestFalse(TEXT("a query effect has no mutations"), Shape.bHasMutations);

	Effect->EffectScript = TEXT("self.Health += 1\nreturn self.Health");
	Shape = Effect->GetAuthoredShape();
	TestTrue(TEXT("a mutate-and-return effect authors a return"), Shape.bAuthorsReturn);
	TestTrue(TEXT("a mutate-and-return effect has mutations"), Shape.bHasMutations);

	// A require is not a mutation: it folds into the invoke policy and writes nothing.
	Effect->EffectScript = TEXT("require is_owner\nreturn self.Health");
	Shape = Effect->GetAuthoredShape();
	TestFalse(TEXT("a require alone is not a mutation"), Shape.bHasMutations);

	// A body mid-edit declares nothing a caller can rely on, and says so without erroring.
	Effect->EffectScript = TEXT("self.Health +=");
	Shape = Effect->GetAuthoredShape();
	TestFalse(TEXT("an unparseable body authors no return"), Shape.bAuthorsReturn);
	return true;
}

// A fresh effect defaults to Text, not to a value whose enum name no longer exists. The Structured mode was
// retired and its enumerator removed; if the default ever regressed to some other surviving value, every new
// effect asset would silently author against the wrong surface with no assignments or requires to notice by.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSourceDefaultsToTextTest,
	"CrowdySDK.Effect.SourceDefaultsToText", CrowdyEffectTestFlags)
bool FCrowdyEffectSourceDefaultsToTextTest::RunTest(const FString& Parameters)
{
	const UCrowdyEffect* Cdo = GetDefault<UCrowdyEffect>();
	TestTrue(TEXT("a fresh CDO defaults to Source == Text"), Cdo->Source == ECrowdyEffectSource::Text);
	return true;
}

// A fn: call runs none of the callee's side effects, not merely none of its writes. An effect whose only job is to
// fire a signal still does nothing when reached through fn:, so it counts.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectAuthoredShapeSignalOnlyTest,
	"CrowdySDK.Effect.AuthoredShapeCountsASignalAsASideEffect", CrowdyEffectTestFlags)
bool FCrowdyEffectAuthoredShapeSignalOnlyTest::RunTest(const FString& Parameters)
{
	UCrowdyEffect* Effect = NewObject<UCrowdyEffect>(GetTransientPackage());
	Effect->Source = ECrowdyEffectSource::Text;
	Effect->EffectScript = TEXT("return self.Health");

	TestFalse(TEXT("with no signal it is a pure query"), Effect->GetAuthoredShape().bHasMutations);

	FCrowdyEffectSignal Signal;
	Signal.Name = TEXT("Damaged");
	Effect->Signals.Add(Signal);
	TestTrue(TEXT("a signal is a side effect a fn: call does not get"), Effect->GetAuthoredShape().bHasMutations);
	return true;
}

// Same for a timer: arming one is a side effect, and a fn: call arms nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectAuthoredShapeTimerOnlyTest,
	"CrowdySDK.Effect.AuthoredShapeCountsATimerAsASideEffect", CrowdyEffectTestFlags)
bool FCrowdyEffectAuthoredShapeTimerOnlyTest::RunTest(const FString& Parameters)
{
	UCrowdyEffect* Effect = NewObject<UCrowdyEffect>(GetTransientPackage());
	Effect->Source = ECrowdyEffectSource::Text;
	Effect->EffectScript = TEXT("return self.Health");

	TestFalse(TEXT("with no timer it is a pure query"), Effect->GetAuthoredShape().bHasMutations);

	FCrowdyEffectTimer Timer;
	Timer.FunctionName = TEXT("Respawn");
	Effect->Timers.Add(Timer);
	TestTrue(TEXT("a timer is a side effect a fn: call does not get"), Effect->GetAuthoredShape().bHasMutations);
	return true;
}

// A bool tuning parameter the designer picked from the dropdown and then left alone still lowers as an authored
// default of false. The bool editor is a checkbox, which draws an unauthored default and a stored false
// identically, so an empty one is a value the designer believes they authored; unnormalized it lowers as required
// and the apply is refused, which is why ticking and unticking the box "fixes" the parameter.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectBoolMagnitudeUntouchedDefaultsToFalseTest,
	"CrowdySDK.Effect.BoolMagnitudeUntouchedDefaultsToFalse", CrowdyEffectTestFlags)
bool FCrowdyEffectBoolMagnitudeUntouchedDefaultsToFalseTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectMagnitude Flag;
	Flag.Name = TEXT("flag");
	Flag.ValueTypeEnum = ECrowdyEffectValueType::Bool;
	Flag.bTypeMigrated = true;

	UCrowdyEffect::MigrateMagnitude(Flag);

	TestEqual(TEXT("an untouched bool default normalizes to false"), Flag.DefaultValueJson, FString(TEXT("false")));
	TestEqual(TEXT("the wire type is bool, not int"), Flag.ValueType, FString(TEXT("bool")));

	// An authored true survives, so the normalization cannot be a blanket overwrite of every bool default.
	FCrowdyEffectMagnitude Authored = Flag;
	Authored.DefaultValueJson = TEXT("true");
	UCrowdyEffect::MigrateMagnitude(Authored);
	TestEqual(TEXT("an authored true is not clobbered"), Authored.DefaultValueJson, FString(TEXT("true")));

	// Declared exactly as the authored surface declares it: the resolved wire type, the normalized default, and the
	// migrated required flag rather than a value the test picked.
	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.Magnitudes.Add(FCrowdyEffectParamDecl{ Flag.Name, Flag.ValueType, Flag.DefaultValueJson, Flag.Description,
		UCrowdyEffect::ResolveMagnitudeRequired(Flag) });

	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("self.hp -= 1"), Ctx);
	TestFalse(TEXT("no errors"), R.HasErrors());

	const FCrowdyGameModelFunctionParam* Param = R.Function.Parameters.FindByPredicate(
		[](const FCrowdyGameModelFunctionParam& P) { return P.Name.Equals(TEXT("flag"), ESearchCase::CaseSensitive); });
	if (!TestNotNull(TEXT("the bool parameter lowered"), Param))
	{
		return true;
	}
	TestFalse(TEXT("a bool parameter is never required"), Param->bRequired);
	TestEqual(TEXT("lowered wire type is bool"), Param->ValueType, FString(TEXT("bool")));
	TestEqual(TEXT("lowered default is false"), Param->DefaultValueJson, FString(TEXT("false")));

	// Key presence is the load-bearing fact: the marshaller OMITS defaultValueJson when the value is empty, so a
	// change that never actually stored anything would still read back as "false" from an absent server field.
	const TSharedPtr<FJsonObject> Upsert = CrowdyGameModelMarshalling::BuildFunctionUpsertInput(R.Function, 42);
	if (!TestTrue(TEXT("upsert input built"), Upsert.IsValid()))
	{
		return true;
	}
	const TArray<TSharedPtr<FJsonValue>>* ParamsArray = nullptr;
	if (!TestTrue(TEXT("parameters array present"), Upsert->TryGetArrayField(TEXT("parameters"), ParamsArray)))
	{
		return true;
	}

	bool bFoundFlag = false;
	for (const TSharedPtr<FJsonValue>& Entry : *ParamsArray)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!Entry.IsValid() || !Entry->TryGetObject(Object))
		{
			continue;
		}
		FString Name;
		if (!(*Object)->TryGetStringField(TEXT("name"), Name) || !Name.Equals(TEXT("flag"), ESearchCase::CaseSensitive))
		{
			continue;
		}
		bFoundFlag = true;
		TestTrue(TEXT("defaultValueJson key reaches the wire"), (*Object)->HasField(TEXT("defaultValueJson")));

		FString MarshalledDefault;
		(*Object)->TryGetStringField(TEXT("defaultValueJson"), MarshalledDefault);
		TestEqual(TEXT("marshalled default is false"), MarshalledDefault, FString(TEXT("false")));

		bool bMarshalledRequired = true;
		(*Object)->TryGetBoolField(TEXT("required"), bMarshalledRequired);
		TestFalse(TEXT("marshalled as not required"), bMarshalledRequired);

		FString MarshalledType;
		(*Object)->TryGetStringField(TEXT("valueType"), MarshalledType);
		TestEqual(TEXT("marshalled wire type is bool"), MarshalledType, FString(TEXT("bool")));
	}
	TestTrue(TEXT("the flag parameter reached the wire"), bFoundFlag);
	return true;
}

// The apply path for the same untouched bool: no overrides, no curve, and it must send JSON false instead of
// being refused as a required magnitude nobody supplied.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectBoolMagnitudeAppliesWithoutOverrideTest,
	"CrowdySDK.Effect.BoolMagnitudeAppliesWithoutOverride", CrowdyEffectTestFlags)
bool FCrowdyEffectBoolMagnitudeAppliesWithoutOverrideTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectMagnitude Flag;
	Flag.Name = TEXT("flag");
	Flag.ValueTypeEnum = ECrowdyEffectValueType::Bool;
	Flag.bTypeMigrated = true;
	// What PostLoad runs over every magnitude of a loaded asset.
	UCrowdyEffect::MigrateMagnitude(Flag);

	UCrowdyEffect* Effect = NewObject<UCrowdyEffect>(GetTransientPackage());
	Effect->Magnitudes = { Flag };

	FString Error;
	const TSharedPtr<FJsonObject> Params =
		UCrowdyEffects::BuildInvokeParams(Effect, {}, 1.0f, /*bHasSource*/ false, FString(), Error);

	if (!TestNotNull(TEXT("params built with no override supplied"), Params.Get()))
	{
		AddError(FString::Printf(TEXT("the apply was refused: %s"), *Error));
		return true;
	}
	TestTrue(TEXT("no error"), Error.IsEmpty());

	bool bValue = true;
	TestTrue(TEXT("flag is a JSON boolean"), Params->TryGetBoolField(TEXT("flag"), bValue));
	TestFalse(TEXT("flag sends false"), bValue);
	return true;
}

namespace
{
	// One parameter object out of a marshalled function upsert, so a test can assert on the KEYS the wire carries
	// and not only on their values. Null when no parameter of that name was marshalled.
	TSharedPtr<FJsonObject> CrowdyEffectTestsFindMarshalledParam(const TSharedPtr<FJsonObject>& Upsert,
		const TCHAR* Name)
	{
		const TArray<TSharedPtr<FJsonValue>>* Params = nullptr;
		if (!Upsert.IsValid() || !Upsert->TryGetArrayField(TEXT("parameters"), Params) || !Params)
		{
			return nullptr;
		}
		for (const TSharedPtr<FJsonValue>& Entry : *Params)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!Entry.IsValid() || !Entry->TryGetObject(Object) || !Object)
			{
				continue;
			}
			FString ParamName;
			if ((*Object)->TryGetStringField(TEXT("name"), ParamName)
				&& ParamName.Equals(Name, ESearchCase::CaseSensitive))
			{
				return *Object;
			}
		}
		return nullptr;
	}

	// A magnitude as it was serialized before required-ness had a field: the value type in the legacy wire string,
	// neither migration flag set, and required-ness spelled only by whether a default was authored.
	FCrowdyEffectMagnitude CrowdyEffectTestsMakeLegacyMagnitude(const TCHAR* Name, const TCHAR* WireType,
		const TCHAR* DefaultJson)
	{
		FCrowdyEffectMagnitude M;
		M.Name = Name;
		M.ValueType = WireType;
		M.DefaultValueJson = DefaultJson;
		return M;
	}
}

// Required-ness used to be spelled as an empty default, which meant one field carried two independent facts. A
// legacy magnitude derives the explicit flag once at load, and the lowering then reads the flag instead of
// re-deriving it from the text.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectMagnitudeRequiredMigratesTest,
	"CrowdySDK.Effect.MagnitudeRequiredMigratesFromEmptyDefault", CrowdyEffectTestFlags)
bool FCrowdyEffectMagnitudeRequiredMigratesTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectMagnitude Legacy = CrowdyEffectTestsMakeLegacyMagnitude(TEXT("amount"), TEXT("int"), TEXT(""));
	UCrowdyEffect::MigrateMagnitude(Legacy);
	TestTrue(TEXT("an empty non-bool default migrates to required"), Legacy.bRequired);
	TestTrue(TEXT("and records that the answer has been derived"), Legacy.bRequiredMigrated);
	TestTrue(TEXT("without inventing a default for it"), Legacy.DefaultValueJson.IsEmpty());

	// The control: an authored default migrates to OPTIONAL, so the derivation cannot be a blanket "required".
	FCrowdyEffectMagnitude Defaulted = CrowdyEffectTestsMakeLegacyMagnitude(TEXT("power"), TEXT("int"), TEXT("5"));
	UCrowdyEffect::MigrateMagnitude(Defaulted);
	TestFalse(TEXT("an authored default migrates to optional"), Defaulted.bRequired);
	TestEqual(TEXT("and keeps the authored default"), Defaulted.DefaultValueJson, FString(TEXT("5")));

	// Both reach the wire through the authoring surface, which is what the asset's Compile() feeds the lowering.
	UCrowdyEffect* Effect = NewObject<UCrowdyEffect>(GetTransientPackage());
	Effect->Magnitudes = { Legacy, Defaulted };

	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.Magnitudes = CrowdyEffectAuthoredSurface::FromEffectSettings(*Effect).Magnitudes;

	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("self.str += $amount + $power"), Ctx);
	TestFalse(TEXT("no errors"), R.HasErrors());

	const FCrowdyGameModelFunctionParam* Amount = R.Function.Parameters.FindByPredicate(
		[](const FCrowdyGameModelFunctionParam& P) { return P.Name.Equals(TEXT("amount"), ESearchCase::CaseSensitive); });
	const FCrowdyGameModelFunctionParam* Power = R.Function.Parameters.FindByPredicate(
		[](const FCrowdyGameModelFunctionParam& P) { return P.Name.Equals(TEXT("power"), ESearchCase::CaseSensitive); });
	if (!TestNotNull(TEXT("the migrated magnitude lowered"), Amount)
		|| !TestNotNull(TEXT("the defaulted magnitude lowered"), Power))
	{
		return true;
	}
	TestTrue(TEXT("it still lowers as required"), Amount->bRequired);
	TestFalse(TEXT("and the defaulted one still does not"), Power->bRequired);
	TestEqual(TEXT("the default survives lowering"), Power->DefaultValueJson, FString(TEXT("5")));
	return true;
}

// The one type the old encoding could never speak for: a bool, whose checkbox draws an empty default and a stored
// false identically. It migrates to optional with an explicit false, which is the behaviour a required-ness flag
// must not quietly undo now that a bool CAN say it is required.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectBoolMagnitudeMigratesAsOptionalTest,
	"CrowdySDK.Effect.BoolMagnitudeMigratesAsOptional", CrowdyEffectTestFlags)
bool FCrowdyEffectBoolMagnitudeMigratesAsOptionalTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectMagnitude Flag = CrowdyEffectTestsMakeLegacyMagnitude(TEXT("flag"), TEXT("bool"), TEXT(""));
	UCrowdyEffect::MigrateMagnitude(Flag);
	TestFalse(TEXT("an empty bool default migrates to optional"), Flag.bRequired);
	TestEqual(TEXT("with the explicit false its checkbox cannot express"), Flag.DefaultValueJson,
		FString(TEXT("false")));

	// The control: the very same empty default on a string IS required, so the exemption is the bool's alone and
	// not a migration that answers "optional" for everything.
	FCrowdyEffectMagnitude Text = CrowdyEffectTestsMakeLegacyMagnitude(TEXT("note"), TEXT("string"), TEXT(""));
	UCrowdyEffect::MigrateMagnitude(Text);
	TestTrue(TEXT("the same empty default on a string migrates to required"), Text.bRequired);

	// Read back through the resolver an unmigrated magnitude goes through, so a bool built in code (never loaded,
	// never edited) answers the same way rather than falling back to the emptiness rule.
	const FCrowdyEffectMagnitude Unmigrated =
		CrowdyEffectTestsMakeLegacyMagnitude(TEXT("flag"), TEXT("bool"), TEXT(""));
	TestFalse(TEXT("an unmigrated bool resolves as optional"), UCrowdyEffect::ResolveMagnitudeRequired(Unmigrated));
	const FCrowdyEffectMagnitude UnmigratedText =
		CrowdyEffectTestsMakeLegacyMagnitude(TEXT("note"), TEXT("string"), TEXT(""));
	TestTrue(TEXT("an unmigrated string with no default resolves as required"),
		UCrowdyEffect::ResolveMagnitudeRequired(UnmigratedText));
	return true;
}

// Migration runs on every load, so deriving the flag has to be a one-time fold, not a rule re-applied over the
// current state: a designer who marks a defaulted parameter required must still find it required next session.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectMagnitudeRequiredMigrationIdempotentTest,
	"CrowdySDK.Effect.MagnitudeRequiredMigrationIsIdempotent", CrowdyEffectTestFlags)
bool FCrowdyEffectMagnitudeRequiredMigrationIdempotentTest::RunTest(const FString& Parameters)
{
	const TArray<FCrowdyEffectMagnitude> Legacy = {
		CrowdyEffectTestsMakeLegacyMagnitude(TEXT("amount"), TEXT("int"), TEXT("")),
		CrowdyEffectTestsMakeLegacyMagnitude(TEXT("power"), TEXT("int"), TEXT("5")),
		CrowdyEffectTestsMakeLegacyMagnitude(TEXT("flag"), TEXT("bool"), TEXT("")),
		CrowdyEffectTestsMakeLegacyMagnitude(TEXT("note"), TEXT("string"), TEXT(""))
	};

	for (const FCrowdyEffectMagnitude& Original : Legacy)
	{
		FCrowdyEffectMagnitude Once = Original;
		UCrowdyEffect::MigrateMagnitude(Once);
		FCrowdyEffectMagnitude Twice = Once;
		UCrowdyEffect::MigrateMagnitude(Twice);

		TestEqual(FString::Printf(TEXT("'%s' required is stable"), *Original.Name), Twice.bRequired, Once.bRequired);
		TestEqual(FString::Printf(TEXT("'%s' default is stable"), *Original.Name), Twice.DefaultValueJson,
			Once.DefaultValueJson);
	}

	// The control for the guard flag: a designer marks a defaulted parameter required after it migrated, and a
	// later load must not re-derive "it has a default, so it is optional" over that decision.
	FCrowdyEffectMagnitude Authored = CrowdyEffectTestsMakeLegacyMagnitude(TEXT("power"), TEXT("int"), TEXT("5"));
	UCrowdyEffect::MigrateMagnitude(Authored);
	Authored.bRequired = true;
	UCrowdyEffect::MigrateMagnitude(Authored);
	TestTrue(TEXT("a required flag set after migration survives the next load"), Authored.bRequired);

	// And the same in the other direction: an empty default no longer forces required back on.
	FCrowdyEffectMagnitude Relaxed = CrowdyEffectTestsMakeLegacyMagnitude(TEXT("amount"), TEXT("int"), TEXT(""));
	UCrowdyEffect::MigrateMagnitude(Relaxed);
	Relaxed.bRequired = false;
	UCrowdyEffect::MigrateMagnitude(Relaxed);
	TestFalse(TEXT("an optional parameter with no default stays optional"), Relaxed.bRequired);
	return true;
}

// The case the old encoding made unauthorable: a REQUIRED bool. It lowers as required and carries no default, so
// the marshaller omits defaultValueJson entirely rather than telling the server both at once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectRequiredBoolIsAuthorableTest,
	"CrowdySDK.Effect.RequiredBoolLowersWithNoDefault", CrowdyEffectTestFlags)
bool FCrowdyEffectRequiredBoolIsAuthorableTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectMagnitude Flag;
	Flag.Name = TEXT("flag");
	Flag.ValueTypeEnum = ECrowdyEffectValueType::Bool;
	Flag.bTypeMigrated = true;
	Flag.bRequired = true;
	Flag.bRequiredMigrated = true;
	// A default authored before the box was ticked. It still reaches the wire: required-ness is its own field,
	// and blanking the default here would ask the upsert to clear one the server holds, which it cannot do.
	Flag.DefaultValueJson = TEXT("true");

	// Migration leaves a required bool alone rather than normalizing a default onto it.
	UCrowdyEffect::MigrateMagnitude(Flag);
	TestTrue(TEXT("a required bool stays required through migration"), Flag.bRequired);

	UCrowdyEffect* Effect = NewObject<UCrowdyEffect>(GetTransientPackage());
	Effect->Magnitudes = { Flag };

	FCrowdyEffectLoweringContext Ctx = MakeHeroContext();
	Ctx.Magnitudes = CrowdyEffectAuthoredSurface::FromEffectSettings(*Effect).Magnitudes;

	const FCrowdyEffectLoweringResult R = LowerScript(TEXT("self.hp -= 1"), Ctx);
	TestFalse(TEXT("no errors"), R.HasErrors());

	const FCrowdyGameModelFunctionParam* Param = R.Function.Parameters.FindByPredicate(
		[](const FCrowdyGameModelFunctionParam& P) { return P.Name.Equals(TEXT("flag"), ESearchCase::CaseSensitive); });
	if (!TestNotNull(TEXT("the bool parameter lowered"), Param))
	{
		return true;
	}
	TestTrue(TEXT("a bool can now say it is required"), Param->bRequired);

	// Key PRESENCE is the load-bearing fact. The marshaller omits an empty defaultValueJson rather than sending a
	// null, so a lowering that blanked the default would plan a clear the wire cannot carry: the server would keep
	// its old value and every later plan would compute the same difference again, forever.
	TestEqual(TEXT("the authored default still lowers"), Param->DefaultValueJson, FString(TEXT("true")));

	const TSharedPtr<FJsonObject> Marshalled =
		CrowdyEffectTestsFindMarshalledParam(CrowdyGameModelMarshalling::BuildFunctionUpsertInput(R.Function, 42),
			TEXT("flag"));
	if (!TestTrue(TEXT("the flag parameter reached the wire"), Marshalled.IsValid()))
	{
		return true;
	}
	TestTrue(TEXT("a required parameter still carries its default key"),
		Marshalled->HasField(TEXT("defaultValueJson")));
	bool bMarshalledRequired = false;
	Marshalled->TryGetBoolField(TEXT("required"), bMarshalledRequired);
	TestTrue(TEXT("marshalled as required"), bMarshalledRequired);

	// The control, and the whole point of the pair: required-ness moves independently of the default, so the two
	// variants differ in exactly one marshalled field and neither of them can ask for an unsendable clear.
	FCrowdyEffectMagnitude Optional = Flag;
	Optional.bRequired = false;
	Effect->Magnitudes = { Optional };
	Ctx.Magnitudes = CrowdyEffectAuthoredSurface::FromEffectSettings(*Effect).Magnitudes;

	const FCrowdyEffectLoweringResult Relaxed = LowerScript(TEXT("self.hp -= 1"), Ctx);
	const TSharedPtr<FJsonObject> RelaxedParam =
		CrowdyEffectTestsFindMarshalledParam(
			CrowdyGameModelMarshalling::BuildFunctionUpsertInput(Relaxed.Function, 42), TEXT("flag"));
	if (TestTrue(TEXT("the optional flag reached the wire"), RelaxedParam.IsValid()))
	{
		TestTrue(TEXT("an optional bool keeps its default key"), RelaxedParam->HasField(TEXT("defaultValueJson")));
		FString RelaxedDefault;
		RelaxedParam->TryGetStringField(TEXT("defaultValueJson"), RelaxedDefault);
		TestEqual(TEXT("carrying the authored value"), RelaxedDefault, FString(TEXT("true")));

		bool bRelaxedRequired = true;
		RelaxedParam->TryGetBoolField(TEXT("required"), bRelaxedRequired);
		TestFalse(TEXT("and differing from the required variant only in the required field"), bRelaxedRequired);
	}
	return true;
}

// The apply-time gate reads the flag, not the text: a required parameter is demanded even when a default is still
// stored beside it, and an optional one with no default at all is omitted rather than refused.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectApplyDemandsByRequiredFlagTest,
	"CrowdySDK.Effect.ApplyDemandsByRequiredFlag", CrowdyEffectTestFlags)
bool FCrowdyEffectApplyDemandsByRequiredFlagTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectMagnitude Required;
	Required.Name = TEXT("amount");
	Required.ValueTypeEnum = ECrowdyEffectValueType::Int;
	Required.ValueType = TEXT("int");
	Required.DefaultValueJson = TEXT("5");
	Required.bRequired = true;
	Required.bTypeMigrated = true;
	Required.bRequiredMigrated = true;

	UCrowdyEffect* Demanding = NewObject<UCrowdyEffect>(GetTransientPackage());
	Demanding->Magnitudes = { Required };

	FString Error;
	TestNull(TEXT("a required magnitude is demanded despite the default stored beside it"),
		UCrowdyEffects::BuildInvokeParams(Demanding, {}, 1.0f, false, FString(), Error).Get());
	TestFalse(TEXT("with an error to show for it"), Error.IsEmpty());

	// An override satisfies it, so the refusal above is about the missing value and not about the flag itself.
	TMap<FName, FString> Overrides;
	Overrides.Add(FName(TEXT("amount")), TEXT("42"));
	Error.Reset();
	const TSharedPtr<FJsonObject> Supplied =
		UCrowdyEffects::BuildInvokeParams(Demanding, Overrides, 1.0f, false, FString(), Error);
	if (TestNotNull(TEXT("an override supplies it"), Supplied.Get()))
	{
		TestEqual(TEXT("carrying the supplied value"), static_cast<int32>(Supplied->GetNumberField(TEXT("amount"))), 42);
	}

	// The control: the same magnitude with the same stored default, optional, sends that default.
	FCrowdyEffectMagnitude Optional = Required;
	Optional.bRequired = false;
	UCrowdyEffect* Lenient = NewObject<UCrowdyEffect>(GetTransientPackage());
	Lenient->Magnitudes = { Optional };
	Error.Reset();
	const TSharedPtr<FJsonObject> Defaulted =
		UCrowdyEffects::BuildInvokeParams(Lenient, {}, 1.0f, false, FString(), Error);
	if (TestNotNull(TEXT("an optional magnitude with the same default is sent"), Defaulted.Get()))
	{
		TestEqual(TEXT("carrying that default"), static_cast<int32>(Defaulted->GetNumberField(TEXT("amount"))), 5);
	}

	// An optional magnitude with no default is omitted (the server applies the parameter's own), while the same
	// magnitude marked required is refused: emptiness no longer decides either answer.
	FCrowdyEffectMagnitude Sparse;
	Sparse.Name = TEXT("note");
	Sparse.ValueTypeEnum = ECrowdyEffectValueType::String;
	Sparse.ValueType = TEXT("string");
	Sparse.bTypeMigrated = true;
	Sparse.bRequiredMigrated = true;

	UCrowdyEffect* Sparser = NewObject<UCrowdyEffect>(GetTransientPackage());
	Sparser->Magnitudes = { Sparse };
	Error.Reset();
	const TSharedPtr<FJsonObject> Omitted =
		UCrowdyEffects::BuildInvokeParams(Sparser, {}, 1.0f, false, FString(), Error);
	if (TestNotNull(TEXT("an optional magnitude with no default is not refused"), Omitted.Get()))
	{
		TestFalse(TEXT("its key is omitted rather than sent empty"), Omitted->HasField(TEXT("note")));
	}

	Sparse.bRequired = true;
	Sparser->Magnitudes = { Sparse };
	Error.Reset();
	TestNull(TEXT("the same magnitude marked required is refused"),
		UCrowdyEffects::BuildInvokeParams(Sparser, {}, 1.0f, false, FString(), Error).Get());
	TestFalse(TEXT("with an error to show for it"), Error.IsEmpty());
	return true;
}

namespace
{
	// A whole marshalled upsert as text, so a comparison sees which KEYS each side wrote and not only the values of
	// the keys both happened to write. An omitted defaultValueJson is invisible to any field-by-field check.
	FString CrowdyEffectTestsMarshalToText(const FCrowdyGameModelFunctionInput& Fn)
	{
		FString Text;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
		FJsonSerializer::Serialize(CrowdyGameModelMarshalling::BuildFunctionUpsertInput(Fn, 42).ToSharedRef(), Writer);
		return Text;
	}
}

// The two routes one effect reaches the wire by: the asset, loaded and migrated, and the payload it stamped on
// itself, read back without loading anything. A schema plan uses whichever is available per package, so two
// machines planning the same project must produce the same upsert. Compared as marshalled TEXT, because the
// divergence this pins was a missing key rather than a differing value.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectStoredPayloadMatchesAssetPathTest,
	"CrowdySDK.Effect.StoredPayloadMarshalsLikeTheAssetPath", CrowdyEffectTestFlags)
bool FCrowdyEffectStoredPayloadMatchesAssetPathTest::RunTest(const FString& Parameters)
{
	// A bool authored before required-ness had a field of its own: the legacy wire string, no default, and neither
	// migration flag. It is the case the two routes used to answer differently for.
	const FCrowdyEffectMagnitude Authored =
		CrowdyEffectTestsMakeLegacyMagnitude(TEXT("flag"), TEXT("bool"), TEXT(""));

	UCrowdyEffect* Effect = NewObject<UCrowdyEffect>(GetTransientPackage());

	// Route one: the asset as a load leaves it.
	FCrowdyEffectMagnitude Migrated = Authored;
	UCrowdyEffect::MigrateMagnitude(Migrated);
	Effect->Magnitudes = { Migrated };
	const FCrowdyEffectAuthoredSurface FromAsset = CrowdyEffectAuthoredSurface::FromEffectSettings(*Effect);

	// Route two: the payload a build that predates the required key stamped, read back with no asset in hand. Taking
	// it off the UNMIGRATED magnitude is what makes it a genuine legacy payload rather than a copy of route one.
	Effect->Magnitudes = { Authored };
	FString LegacyJson = CrowdyEffectAuthoredSurface::ToJson(CrowdyEffectAuthoredSurface::FromEffectSettings(*Effect));
	TestTrue(TEXT("the payload carried a required key to begin with"),
		LegacyJson.Contains(TEXT("\"r\":true")) || LegacyJson.Contains(TEXT("\"r\":false")));
	LegacyJson = LegacyJson.Replace(TEXT(",\"r\":true"), TEXT(""));
	LegacyJson = LegacyJson.Replace(TEXT(",\"r\":false"), TEXT(""));
	TestFalse(TEXT("no boolean required key survives the strip"),
		LegacyJson.Contains(TEXT("\"r\":true")) || LegacyJson.Contains(TEXT("\"r\":false")));

	FCrowdyEffectAuthoredSurface FromTag;
	if (!TestTrue(TEXT("the stripped payload parses"),
		CrowdyEffectAuthoredSurface::FromJson(LegacyJson, FromTag)))
	{
		return true;
	}

	const FString Body = TEXT("self.hp -= if($flag, 2, 1)");

	FCrowdyEffectLoweringContext AssetCtx = MakeHeroContext();
	AssetCtx.Magnitudes = FromAsset.Magnitudes;
	const FCrowdyEffectLoweringResult AssetResult = LowerScript(Body, AssetCtx);

	FCrowdyEffectLoweringContext TagCtx = MakeHeroContext();
	TagCtx.Magnitudes = FromTag.Magnitudes;
	const FCrowdyEffectLoweringResult TagResult = LowerScript(Body, TagCtx);

	TestFalse(TEXT("the asset route compiles"), AssetResult.HasErrors());
	TestFalse(TEXT("the stored-payload route compiles"), TagResult.HasErrors());

	TestEqual(TEXT("both routes marshal byte-identically, key presence included"),
		CrowdyEffectTestsMarshalToText(TagResult.Function), CrowdyEffectTestsMarshalToText(AssetResult.Function));

	// The control: the equality above would also hold if BOTH routes had dropped the key, so name the value each
	// side has to be carrying. An optional bool's checkbox cannot draw "no default", so false is the only honest one.
	const TSharedPtr<FJsonObject> TaggedParam = CrowdyEffectTestsFindMarshalledParam(
		CrowdyGameModelMarshalling::BuildFunctionUpsertInput(TagResult.Function, 42), TEXT("flag"));
	if (TestTrue(TEXT("the flag parameter reached the wire"), TaggedParam.IsValid()))
	{
		FString TaggedDefault;
		TestTrue(TEXT("the stored-payload route carries a default at all"),
			TaggedParam->TryGetStringField(TEXT("defaultValueJson"), TaggedDefault));
		TestEqual(TEXT("and it is the explicit false the asset route writes"), TaggedDefault, FString(TEXT("false")));
	}

	// A payload written before that normalization existed describes a different schema, so it must not read as this
	// build's. That is what lets the retag command find and rewrite it: it selects on the version not matching.
	TestFalse(TEXT("a version 1 payload is no longer current"),
		CrowdyEffectAuthoredSurface::IsCurrentTagVersion(TEXT("1")));
	return true;
}

// Optional means "omit it and this value is used", so with no default the declaration names no value at all, and
// no later upsert can fix it: an empty defaultValueJson is omitted rather than sent as a clear.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectOptionalWithNoDefaultIsRefusedTest,
	"CrowdySDK.Effect.OptionalMagnitudeWithNoDefaultIsRefused", CrowdyEffectTestFlags)
bool FCrowdyEffectOptionalWithNoDefaultIsRefusedTest::RunTest(const FString& Parameters)
{
	const FString Body = TEXT("self.hp -= $amount");

	FCrowdyEffectParamDecl Offending;
	Offending.Name = TEXT("amount");
	Offending.ValueType = TEXT("int");
	Offending.bRequired = false;

	FCrowdyEffectLoweringContext BadCtx = MakeHeroContext();
	BadCtx.Magnitudes = { Offending };
	const FCrowdyEffectLoweringResult Bad = LowerScript(Body, BadCtx);
	TestTrue(TEXT("an optional magnitude with no default is refused"), Bad.HasErrors());

	// Two controls, one per way out of the shape, so the error cannot be a blanket rejection of the magnitude.
	FCrowdyEffectParamDecl Defaulted = Offending;
	Defaulted.DefaultValueJson = TEXT("1");
	FCrowdyEffectLoweringContext DefaultedCtx = MakeHeroContext();
	DefaultedCtx.Magnitudes = { Defaulted };
	TestFalse(TEXT("authoring a default resolves it"), LowerScript(Body, DefaultedCtx).HasErrors());

	FCrowdyEffectParamDecl Demanded = Offending;
	Demanded.bRequired = true;
	FCrowdyEffectLoweringContext DemandedCtx = MakeHeroContext();
	DemandedCtx.Magnitudes = { Demanded };
	TestFalse(TEXT("marking it required resolves it too"), LowerScript(Body, DemandedCtx).HasErrors());
	return true;
}

// An override is a JSON literal, and no value type spells one as the empty string. The container_ref route is how
// this arrives in practice: a wired pin holding an object with no bound container.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectEmptyOverrideIsRefusedTest,
	"CrowdySDK.Effect.EmptyOverrideIsRefused", CrowdyEffectTestFlags)
bool FCrowdyEffectEmptyOverrideIsRefusedTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectMagnitude Ref;
	Ref.Name = TEXT("ally_id");
	Ref.ValueTypeEnum = ECrowdyEffectValueType::ContainerRef;
	Ref.ValueType = TEXT("container_ref");
	Ref.bTypeMigrated = true;
	Ref.bRequired = false;
	Ref.bRequiredMigrated = true;
	Ref.DefaultValueJson = TEXT("\"authored-container\"");

	UCrowdyEffect* Effect = NewObject<UCrowdyEffect>(GetTransientPackage());
	Effect->Magnitudes = { Ref };

	TMap<FName, FString> EmptyOverride;
	EmptyOverride.Add(FName(TEXT("ally_id")), FString());

	FString Error;
	TestNull(TEXT("an empty override is refused rather than dropped"),
		UCrowdyEffects::BuildInvokeParams(Effect, EmptyOverride, 1.0f, false, FString(), Error).Get());
	TestFalse(TEXT("with an error to show for it"), Error.IsEmpty());

	// The control: the same magnitude with no override at all still sends its authored default, so the refusal is
	// about the empty value supplied and not about the parameter.
	Error.Reset();
	const TSharedPtr<FJsonObject> Defaulted =
		UCrowdyEffects::BuildInvokeParams(Effect, {}, 1.0f, false, FString(), Error);
	if (TestNotNull(TEXT("omitting the override sends the authored default"), Defaulted.Get()))
	{
		FString Sent;
		Defaulted->TryGetStringField(TEXT("ally_id"), Sent);
		TestEqual(TEXT("carrying the authored container"), Sent, FString(TEXT("authored-container")));
	}

	// And a real override still arrives, so the guard cannot be rejecting every override.
	TMap<FName, FString> RealOverride;
	RealOverride.Add(FName(TEXT("ally_id")), TEXT("live-container"));
	Error.Reset();
	const TSharedPtr<FJsonObject> Supplied =
		UCrowdyEffects::BuildInvokeParams(Effect, RealOverride, 1.0f, false, FString(), Error);
	if (TestNotNull(TEXT("a non-empty override is accepted"), Supplied.Get()))
	{
		FString Sent;
		Supplied->TryGetStringField(TEXT("ally_id"), Sent);
		TestEqual(TEXT("carrying the supplied container"), Sent, FString(TEXT("live-container")));
	}
	return true;
}

// bRequired is EditAnywhere and BlueprintReadWrite while its migration flag is hidden, so code and Blueprint set
// the one they can see. Reading only the flag would discard what they said whenever a default was present.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectExplicitRequiredNeedsNoMigrationFlagTest,
	"CrowdySDK.Effect.ExplicitRequiredNeedsNoMigrationFlag", CrowdyEffectTestFlags)
bool FCrowdyEffectExplicitRequiredNeedsNoMigrationFlagTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectMagnitude Stated;
	Stated.Name = TEXT("amount");
	Stated.ValueTypeEnum = ECrowdyEffectValueType::Int;
	Stated.ValueType = TEXT("int");
	Stated.bTypeMigrated = true;
	Stated.DefaultValueJson = TEXT("5");
	Stated.bRequired = true;
	// Deliberately NOT set: this is the magnitude a caller who touched only the visible field leaves behind.
	Stated.bRequiredMigrated = false;

	TestTrue(TEXT("an explicitly required magnitude resolves as required without the migration flag"),
		UCrowdyEffect::ResolveMagnitudeRequired(Stated));

	// It survives migration rather than being derived away by the default sitting beside it.
	FCrowdyEffectMagnitude Loaded = Stated;
	UCrowdyEffect::MigrateMagnitude(Loaded);
	TestTrue(TEXT("and still required once migrated"), Loaded.bRequired);

	// The control: the same magnitude that never said anything derives from the legacy encoding exactly as before,
	// so the change cannot be "everything with a default is now required".
	FCrowdyEffectMagnitude Silent = Stated;
	Silent.bRequired = false;
	TestFalse(TEXT("a magnitude that states nothing and has a default is optional"),
		UCrowdyEffect::ResolveMagnitudeRequired(Silent));
	Silent.DefaultValueJson.Reset();
	TestTrue(TEXT("and required once its default is gone"), UCrowdyEffect::ResolveMagnitudeRequired(Silent));
	return true;
}

// One definition of the legacy encoding, reached from the asset and from a stored payload alike. A wire type
// spelled with stray whitespace used to be a bool under one reader and not under the other.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectLegacyRequiredEncodingIsOneRuleTest,
	"CrowdySDK.Effect.LegacyRequiredEncodingIsOneRule", CrowdyEffectTestFlags)
bool FCrowdyEffectLegacyRequiredEncodingIsOneRuleTest::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("a bool with no default is not required"), UCrowdyEffect::IsLegacyRequiredEncoding(
		ECrowdyEffectValueType::Bool, FString()));
	TestTrue(TEXT("an int with no default is"), UCrowdyEffect::IsLegacyRequiredEncoding(
		ECrowdyEffectValueType::Int, FString()));
	TestFalse(TEXT("an int with a default is not"), UCrowdyEffect::IsLegacyRequiredEncoding(
		ECrowdyEffectValueType::Int, TEXT("5")));

	// A padded wire type reaches the same answer through both readers, which is the whole point of there being one
	// predicate: the asset path trims, and a hand-edited or older payload can carry the padding.
	FCrowdyEffectAuthoredSurface Surface;
	Surface.Magnitudes.Add({ TEXT("flag"), TEXT(" bool "), FString(), FString(), false });
	FString Json = CrowdyEffectAuthoredSurface::ToJson(Surface);
	Json = Json.Replace(TEXT(",\"r\":true"), TEXT("")).Replace(TEXT(",\"r\":false"), TEXT(""));

	FCrowdyEffectAuthoredSurface Parsed;
	if (TestTrue(TEXT("the padded payload parses"), CrowdyEffectAuthoredSurface::FromJson(Json, Parsed))
		&& TestEqual(TEXT("its one magnitude survives"), Parsed.Magnitudes.Num(), 1))
	{
		TestFalse(TEXT("a padded bool is a bool to the fallback too"), Parsed.Magnitudes[0].bRequired);
		TestEqual(TEXT("and is normalized to the explicit false"), Parsed.Magnitudes[0].DefaultValueJson,
			FString(TEXT("false")));
	}

	// The normalizer reads the RESOLVED value type, so a magnitude that names bool only in its legacy string is
	// normalized as well. Its sibling resolver has always done this; the two now agree.
	FCrowdyEffectMagnitude Unmigrated = CrowdyEffectTestsMakeLegacyMagnitude(TEXT("flag"), TEXT("bool"), TEXT(""));
	UCrowdyEffect::EnsureBoolDefault(Unmigrated);
	TestEqual(TEXT("an unmigrated bool is normalized off its legacy type string"), Unmigrated.DefaultValueJson,
		FString(TEXT("false")));

	// The control: a required bool keeps no default, so the normalizer is not writing false over everything.
	FCrowdyEffectMagnitude Demanded = CrowdyEffectTestsMakeLegacyMagnitude(TEXT("flag"), TEXT("bool"), TEXT(""));
	Demanded.bRequired = true;
	UCrowdyEffect::EnsureBoolDefault(Demanded);
	TestTrue(TEXT("a required bool is left with no default"), Demanded.DefaultValueJson.IsEmpty());
	return true;
}

// Changing a magnitude's value type discards its default, because the old one was canonical for the old type. The
// pair it lands on must never be the one shape lowering refuses.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectValueTypeChangeLandsOnAValidPairTest,
	"CrowdySDK.Effect.ValueTypeChangeLandsOnAValidPair", CrowdyEffectTestFlags)
bool FCrowdyEffectValueTypeChangeLandsOnAValidPairTest::RunTest(const FString& Parameters)
{
	bool bRequired = true;
	FString DefaultJson = TEXT("5");
	UCrowdyEffect::ApplyValueTypeChangeDefaults(ECrowdyEffectValueType::Bool, bRequired, DefaultJson);
	TestFalse(TEXT("picking Bool clears Required"), bRequired);
	TestEqual(TEXT("and fills the default its checkbox cannot draw"), DefaultJson, FString(TEXT("false")));

	// The reverse switch: the bool default is meaningless for the new type, so it goes, and required-ness follows it.
	UCrowdyEffect::ApplyValueTypeChangeDefaults(ECrowdyEffectValueType::String, bRequired, DefaultJson);
	TestTrue(TEXT("switching away from Bool makes the parameter required"), bRequired);
	TestTrue(TEXT("with no default left over from the old type"), DefaultJson.IsEmpty());

	// The property that matters across every type, stated as one loop rather than trusted from the two cases above.
	for (const ECrowdyEffectValueType Type : { ECrowdyEffectValueType::Int, ECrowdyEffectValueType::Float,
		ECrowdyEffectValueType::Bool, ECrowdyEffectValueType::String, ECrowdyEffectValueType::ContainerRef })
	{
		bool bLanded = false;
		FString Landed;
		UCrowdyEffect::ApplyValueTypeChangeDefaults(Type, bLanded, Landed);
		TestFalse(FString::Printf(TEXT("'%s' never lands optional with no default"),
			*UCrowdyEffect::ValueTypeToWireString(Type)), !bLanded && Landed.IsEmpty());
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
