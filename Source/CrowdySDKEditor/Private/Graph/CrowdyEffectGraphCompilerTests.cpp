// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Graph/CrowdyEffectGraph.h"
#include "Graph/CrowdyEffectGraphCompiler.h"
#include "Graph/CrowdyEffectGraphNodes.h"
#include "Graph/CrowdyEffectGraphTestContainer.h"
#include "Replication/GameModel/CrowdyAttributeRegistry.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"
#include "Replication/GameModel/Effect/CrowdyEffectLowering.h"
#include "Replication/GameModel/Effect/CrowdyEffectParser.h"
#include "Replication/GameModel/Effect/CrowdyEffectSpec.h"
#include "Replication/GameModel/Effect/CrowdyEffectStringifyTestSupport.h"
#include "Replication/GameModel/Effect/CrowdyGameModelFunctionInput.h"
#include "UObject/Package.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyEffectGraphTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// The vocabulary both the graph and the equivalent text validate against: a clamped int hp, a plain int armor,
	// and an int attack the source is read through, plus the tuning magnitudes the graphs reference.
	FCrowdyEffectLoweringContext MakeContext()
	{
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

		FCrowdyAttributeDef Armor;
		Armor.PropertyName = FName(TEXT("Armor"));
		Armor.Key = TEXT("armor");
		Armor.ValueType = TEXT("int");
		Ctx.Attributes.Add(Armor);

		FCrowdyAttributeDef Attack;
		Attack.PropertyName = FName(TEXT("Attack"));
		Attack.Key = TEXT("attack");
		Attack.ValueType = TEXT("int");
		Ctx.Attributes.Add(Attack);

		FCrowdyAttributeDef Alive;
		Alive.PropertyName = FName(TEXT("Alive"));
		Alive.Key = TEXT("alive");
		Alive.ValueType = TEXT("bool");
		Ctx.Attributes.Add(Alive);

		// Required, which is what an int with no default means on the authoring surface; an optional parameter with
		// no default is a shape lowering refuses, so leaving the flag off would fail every case here for a reason
		// that has nothing to do with the graph.
		for (const TCHAR* MagName : { TEXT("power"), TEXT("a"), TEXT("b") })
		{
			FCrowdyEffectParamDecl Param;
			Param.Name = MagName;
			Param.ValueType = TEXT("int");
			Param.bRequired = true;
			Ctx.Magnitudes.Add(Param);
		}

		return Ctx;
	}

	template <typename TNode>
	TNode* AddNode(UCrowdyEffectGraph* Graph)
	{
		TNode* Node = NewObject<TNode>(Graph);
		Node->CreateNewGuid();
		Graph->Nodes.Add(Node);
		return Node;
	}

	void Link(UEdGraphNode* FromNode, FName FromPinName, UEdGraphNode* ToNode, FName ToPinName)
	{
		UEdGraphPin* Out = FromNode ? FromNode->FindPin(FromPinName, EGPD_Output) : nullptr;
		UEdGraphPin* In = ToNode ? ToNode->FindPin(ToPinName, EGPD_Input) : nullptr;
		if (Out && In)
		{
			Out->MakeLinkTo(In);
		}
	}

	// self.hp -= ($power + source.attack)
	UCrowdyEffectGraph* BuildDamageGraph()
	{
		UCrowdyEffectGraph* Graph = NewObject<UCrowdyEffectGraph>(GetTransientPackage());

		UCrowdyEffectGraphNode_Tuning* Power = AddNode<UCrowdyEffectGraphNode_Tuning>(Graph);
		Power->ParamName = TEXT("power");
		Power->AllocateDefaultPins();

		UCrowdyEffectGraphNode_Attribute* Attack = AddNode<UCrowdyEffectGraphNode_Attribute>(Graph);
		Attack->Role = ECrowdyEffectRole::Source;
		Attack->Attribute = TEXT("attack");
		Attack->AllocateDefaultPins();

		UCrowdyEffectGraphNode_BinaryOp* Add = AddNode<UCrowdyEffectGraphNode_BinaryOp>(Graph);
		Add->Op = ECrowdyEffectGraphArithOp::Add;
		Add->AllocateDefaultPins();

		UCrowdyEffectGraphNode_Result* Result = AddNode<UCrowdyEffectGraphNode_Result>(Graph);
		FCrowdyEffectGraphWrite Write;
		Write.TargetRole = ECrowdyEffectRole::Target;
		Write.Attribute = TEXT("hp");
		Write.Op = ECrowdyEffectAssignmentOp::Subtract;
		Result->Writes.Add(Write);
		Result->AllocateDefaultPins();

		Link(Power, UCrowdyEffectGraphNode_Tuning::OutputPinName(), Add, UCrowdyEffectGraphNode_BinaryOp::InputAPinName());
		Link(Attack, UCrowdyEffectGraphNode_Attribute::OutputPinName(), Add, UCrowdyEffectGraphNode_BinaryOp::InputBPinName());
		Link(Add, UCrowdyEffectGraphNode_BinaryOp::OutputPinName(), Result, UCrowdyEffectGraphNode_Result::WritePinName(0));

		return Graph;
	}

	// A Result node with one Set/Add/Subtract... write to self.hp, ready for a value to be wired into WritePinName(0).
	UCrowdyEffectGraphNode_Result* MakeResultWithHpWrite(UCrowdyEffectGraph* Graph, ECrowdyEffectAssignmentOp Op)
	{
		UCrowdyEffectGraphNode_Result* Result = AddNode<UCrowdyEffectGraphNode_Result>(Graph);
		FCrowdyEffectGraphWrite Write;
		Write.TargetRole = ECrowdyEffectRole::Target;
		Write.Attribute = TEXT("hp");
		Write.Op = Op;
		Result->Writes.Add(Write);
		Result->AllocateDefaultPins();
		return Result;
	}

	UCrowdyEffectGraphNode_Attribute* MakeAttribute(UCrowdyEffectGraph* Graph, ECrowdyEffectRole Role, const TCHAR* Attr)
	{
		UCrowdyEffectGraphNode_Attribute* Node = AddNode<UCrowdyEffectGraphNode_Attribute>(Graph);
		Node->Role = Role;
		Node->Attribute = Attr;
		Node->AllocateDefaultPins();
		return Node;
	}

	UCrowdyEffectGraphNode_Tuning* MakeTuning(UCrowdyEffectGraph* Graph, const TCHAR* Param)
	{
		UCrowdyEffectGraphNode_Tuning* Node = AddNode<UCrowdyEffectGraphNode_Tuning>(Graph);
		Node->ParamName = Param;
		Node->AllocateDefaultPins();
		return Node;
	}

	UCrowdyEffectGraphNode_Constant* MakeConstant(
		UCrowdyEffectGraph* Graph, ECrowdyEffectGraphConstantType Type, const TCHAR* Literal)
	{
		UCrowdyEffectGraphNode_Constant* Node = AddNode<UCrowdyEffectGraphNode_Constant>(Graph);
		Node->ConstantType = Type;
		Node->Literal = Literal;
		Node->AllocateDefaultPins();
		return Node;
	}

	// Asserts a graph and the equivalent EffectScript text lower to a byte / structurally identical function input.
	void CheckParity(FAutomationTestBase& Test, UCrowdyEffectGraph* Graph, const TCHAR* EffectText)
	{
		const FCrowdyEffectLoweringContext Ctx = MakeContext();

		const FString FromGraph = CrowdyStringifyResult(FCrowdyEffectGraphCompiler::CompileToFunction(Graph, Ctx));

		const FCrowdyEffectParseResult Parsed = FCrowdyEffectParser::Parse(EffectText);
		Test.TestFalse(TEXT("the equivalent text parses without error"), Parsed.HasErrors());
		const FString FromText = CrowdyStringifyResult(FCrowdyEffectLowering::Lower(Parsed.Program, Ctx));

		Test.TestEqual(TEXT("the graph and the equivalent text lower identically"), FromGraph, FromText);
	}

	// Every diagnostic as severity and message only. A graph has no source text, so its expressions reach the
	// lowering as fragments parsed on their own, and a position inside a fragment counts from the fragment rather
	// than from a line that does not exist. The position is the only part that cannot be compared across the two
	// surfaces, so it is the only part dropped: which diagnostics fire, how many, and what they say all still have
	// to match.
	FString StringifyDiagnosticsWithoutPositions(const TArray<FCrowdyEffectDiagnostic>& Diagnostics)
	{
		FString S;
		for (const FCrowdyEffectDiagnostic& D : Diagnostics)
		{
			S += FString::Printf(TEXT("<%s|%s>"),
				D.Severity == ECrowdyEffectSeverity::Error ? TEXT("error") : TEXT("warning"), *D.Message);
		}
		return S;
	}

	/**
	 * The same parity assertion for an effect whose diagnostics carry a source position: the lowered function is
	 * compared byte for byte, and the diagnostics are compared on everything except where they point.
	 */
	void CheckFunctionParity(FAutomationTestBase& Test, UCrowdyEffectGraph* Graph, const TCHAR* EffectText)
	{
		const FCrowdyEffectLoweringContext Ctx = MakeContext();

		const FCrowdyEffectLoweringResult GraphResult = FCrowdyEffectGraphCompiler::CompileToFunction(Graph, Ctx);

		const FCrowdyEffectParseResult Parsed = FCrowdyEffectParser::Parse(EffectText);
		Test.TestFalse(TEXT("the equivalent text parses without error"), Parsed.HasErrors());
		const FCrowdyEffectLoweringResult TextResult = FCrowdyEffectLowering::Lower(Parsed.Program, Ctx);

		Test.TestEqual(TEXT("the graph and the equivalent text lower to the same function"),
			CrowdyStringifyFunction(GraphResult.Function), CrowdyStringifyFunction(TextResult.Function));
		Test.TestEqual(TEXT("and agree on whether a source is required"),
			GraphResult.bSourceReferenced, TextResult.bSourceReferenced);
		Test.TestEqual(TEXT("and say the same things to the author"),
			StringifyDiagnosticsWithoutPositions(GraphResult.Diagnostics),
			StringifyDiagnosticsWithoutPositions(TextResult.Diagnostics));
	}

	// True when any diagnostic of the given severity mentions Needle, so a test can assert a warning fires on both
	// surfaces without depending on the position it is reported at.
	bool AnyDiagnosticContains(
		const TArray<FCrowdyEffectDiagnostic>& Diagnostics, ECrowdyEffectSeverity Severity, const TCHAR* Needle)
	{
		return Diagnostics.ContainsByPredicate([Severity, Needle](const FCrowdyEffectDiagnostic& D)
		{
			return D.Severity == Severity && D.Message.Contains(Needle);
		});
	}
}

// A graph writing self.hp -= ($power + source.attack) compiles all the way through to a concrete function: one
// mutation, the source_id param the cross-entity read injects, and no error diagnostics.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectGraphCompilesToFunctionTest,
	"CrowdySDK.Editor.GraphCompilesToFunction", CrowdyEffectGraphTestFlags)
bool FCrowdyEffectGraphCompilesToFunctionTest::RunTest(const FString& Parameters)
{
	UCrowdyEffectGraph* Graph = BuildDamageGraph();
	const FCrowdyEffectLoweringResult Result = FCrowdyEffectGraphCompiler::CompileToFunction(Graph, MakeContext());

	TestFalse(TEXT("graph compiles without error"), Result.HasErrors());
	TestEqual(TEXT("one mutation is produced"), Result.Function.Mutations.Num(), 1);
	TestTrue(TEXT("the cross-entity read is detected"), Result.bSourceReferenced);
	TestTrue(TEXT("a source_id param is injected"),
		Result.Function.Parameters.ContainsByPredicate([](const FCrowdyGameModelFunctionParam& P)
		{
			return P.Name == TEXT("source_id") && P.ValueType == TEXT("container_ref");
		}));

	return true;
}

// The load-bearing parity test: the SAME effect authored as a graph and as EffectScript text lowers to a byte /
// structurally identical function input.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectGraphMatchesEffectScriptTextTest,
	"CrowdySDK.Editor.GraphMatchesEffectScriptText", CrowdyEffectGraphTestFlags)
bool FCrowdyEffectGraphMatchesEffectScriptTextTest::RunTest(const FString& Parameters)
{
	const FCrowdyEffectLoweringContext Ctx = MakeContext();

	UCrowdyEffectGraph* Graph = BuildDamageGraph();
	const FString FromGraph = CrowdyStringifyResult(FCrowdyEffectGraphCompiler::CompileToFunction(Graph, Ctx));

	const FCrowdyEffectParseResult Parsed = FCrowdyEffectParser::Parse(TEXT("self.hp -= ($power + source.attack)"));
	TestFalse(TEXT("the text parses without error"), Parsed.HasErrors());
	const FString FromText = CrowdyStringifyResult(FCrowdyEffectLowering::Lower(Parsed.Program, Ctx));

	TestEqual(TEXT("the graph and the equivalent text lower identically"), FromGraph, FromText);

	return true;
}

// EmitExpr always fully parenthesizes a binary subexpression, so a graph 2 * ($a + $b) preserves precedence and
// lowers identically to the text 2 * ($a + $b).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectGraphBinaryOpParenthesizedTest,
	"CrowdySDK.Editor.GraphBinaryOpParenthesized", CrowdyEffectGraphTestFlags)
bool FCrowdyEffectGraphBinaryOpParenthesizedTest::RunTest(const FString& Parameters)
{
	const FCrowdyEffectLoweringContext Ctx = MakeContext();

	UCrowdyEffectGraph* Graph = NewObject<UCrowdyEffectGraph>(GetTransientPackage());

	UCrowdyEffectGraphNode_Constant* Two = AddNode<UCrowdyEffectGraphNode_Constant>(Graph);
	Two->ConstantType = ECrowdyEffectGraphConstantType::Number;
	Two->Literal = TEXT("2");
	Two->AllocateDefaultPins();

	UCrowdyEffectGraphNode_Tuning* A = AddNode<UCrowdyEffectGraphNode_Tuning>(Graph);
	A->ParamName = TEXT("a");
	A->AllocateDefaultPins();

	UCrowdyEffectGraphNode_Tuning* B = AddNode<UCrowdyEffectGraphNode_Tuning>(Graph);
	B->ParamName = TEXT("b");
	B->AllocateDefaultPins();

	UCrowdyEffectGraphNode_BinaryOp* Add = AddNode<UCrowdyEffectGraphNode_BinaryOp>(Graph);
	Add->Op = ECrowdyEffectGraphArithOp::Add;
	Add->AllocateDefaultPins();

	UCrowdyEffectGraphNode_BinaryOp* Mul = AddNode<UCrowdyEffectGraphNode_BinaryOp>(Graph);
	Mul->Op = ECrowdyEffectGraphArithOp::Multiply;
	Mul->AllocateDefaultPins();

	UCrowdyEffectGraphNode_Result* Result = AddNode<UCrowdyEffectGraphNode_Result>(Graph);
	FCrowdyEffectGraphWrite Write;
	Write.TargetRole = ECrowdyEffectRole::Target;
	Write.Attribute = TEXT("hp");
	Write.Op = ECrowdyEffectAssignmentOp::Set;
	Result->Writes.Add(Write);
	Result->AllocateDefaultPins();

	Link(A, UCrowdyEffectGraphNode_Tuning::OutputPinName(), Add, UCrowdyEffectGraphNode_BinaryOp::InputAPinName());
	Link(B, UCrowdyEffectGraphNode_Tuning::OutputPinName(), Add, UCrowdyEffectGraphNode_BinaryOp::InputBPinName());
	Link(Two, UCrowdyEffectGraphNode_Constant::OutputPinName(), Mul, UCrowdyEffectGraphNode_BinaryOp::InputAPinName());
	Link(Add, UCrowdyEffectGraphNode_BinaryOp::OutputPinName(), Mul, UCrowdyEffectGraphNode_BinaryOp::InputBPinName());
	Link(Mul, UCrowdyEffectGraphNode_BinaryOp::OutputPinName(), Result, UCrowdyEffectGraphNode_Result::WritePinName(0));

	const FString FromGraph = CrowdyStringifyResult(FCrowdyEffectGraphCompiler::CompileToFunction(Graph, Ctx));

	const FCrowdyEffectParseResult Parsed = FCrowdyEffectParser::Parse(TEXT("self.hp = 2 * ($a + $b)"));
	TestFalse(TEXT("the text parses without error"), Parsed.HasErrors());
	const FString FromText = CrowdyStringifyResult(FCrowdyEffectLowering::Lower(Parsed.Program, Ctx));

	TestEqual(TEXT("full parenthesization preserves precedence"), FromGraph, FromText);

	return true;
}

// A Result write pin with nothing wired is an Error diagnostic, never a crash.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectGraphUnconnectedRequiredPinDiagnosticTest,
	"CrowdySDK.Editor.GraphUnconnectedRequiredPinDiagnostic", CrowdyEffectGraphTestFlags)
bool FCrowdyEffectGraphUnconnectedRequiredPinDiagnosticTest::RunTest(const FString& Parameters)
{
	UCrowdyEffectGraph* Graph = NewObject<UCrowdyEffectGraph>(GetTransientPackage());

	UCrowdyEffectGraphNode_Result* Result = AddNode<UCrowdyEffectGraphNode_Result>(Graph);
	FCrowdyEffectGraphWrite Write;
	Write.TargetRole = ECrowdyEffectRole::Target;
	Write.Attribute = TEXT("hp");
	Write.Op = ECrowdyEffectAssignmentOp::Set;
	Result->Writes.Add(Write);
	Result->AllocateDefaultPins();
	// The single write input pin is left unconnected.

	TArray<FCrowdyEffectDiagnostic> Diagnostics;
	const FCrowdyEffectSpec Spec = FCrowdyEffectGraphCompiler::CompileToSpec(Graph, Diagnostics);

	TestEqual(TEXT("no assignment is emitted for the unconnected write"), Spec.Assignments.Num(), 0);
	TestTrue(TEXT("an error diagnostic is recorded"),
		Diagnostics.ContainsByPredicate([](const FCrowdyEffectDiagnostic& D)
		{
			return D.Severity == ECrowdyEffectSeverity::Error;
		}));

	return true;
}

// The modulo arithmetic operator (%), which the picker's term operator set does not carry, round-trips through the graph.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectGraphModuloTest,
	"CrowdySDK.Editor.GraphModuloOperator", CrowdyEffectGraphTestFlags)
bool FCrowdyEffectGraphModuloTest::RunTest(const FString& Parameters)
{
	UCrowdyEffectGraph* Graph = NewObject<UCrowdyEffectGraph>(GetTransientPackage());

	UCrowdyEffectGraphNode_Attribute* Hp = MakeAttribute(Graph, ECrowdyEffectRole::Target, TEXT("hp"));
	UCrowdyEffectGraphNode_Tuning* Power = MakeTuning(Graph, TEXT("power"));

	UCrowdyEffectGraphNode_BinaryOp* Mod = AddNode<UCrowdyEffectGraphNode_BinaryOp>(Graph);
	Mod->Op = ECrowdyEffectGraphArithOp::Modulo;
	Mod->AllocateDefaultPins();

	UCrowdyEffectGraphNode_Result* Result = MakeResultWithHpWrite(Graph, ECrowdyEffectAssignmentOp::Set);

	Link(Hp, UCrowdyEffectGraphNode_Attribute::OutputPinName(), Mod, UCrowdyEffectGraphNode_BinaryOp::InputAPinName());
	Link(Power, UCrowdyEffectGraphNode_Tuning::OutputPinName(), Mod, UCrowdyEffectGraphNode_BinaryOp::InputBPinName());
	Link(Mod, UCrowdyEffectGraphNode_BinaryOp::OutputPinName(), Result, UCrowdyEffectGraphNode_Result::WritePinName(0));

	CheckParity(*this, Graph, TEXT("self.hp = self.hp % $power"));
	return true;
}

// A ternary if() fed by a comparison lowers identically to the text if(self.hp > 0, self.hp, 0).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectGraphIfAndCompareTest,
	"CrowdySDK.Editor.GraphIfAndCompare", CrowdyEffectGraphTestFlags)
bool FCrowdyEffectGraphIfAndCompareTest::RunTest(const FString& Parameters)
{
	UCrowdyEffectGraph* Graph = NewObject<UCrowdyEffectGraph>(GetTransientPackage());

	UCrowdyEffectGraphNode_Attribute* HpCond = MakeAttribute(Graph, ECrowdyEffectRole::Target, TEXT("hp"));
	UCrowdyEffectGraphNode_Constant* ZeroCond = MakeConstant(Graph, ECrowdyEffectGraphConstantType::Number, TEXT("0"));

	UCrowdyEffectGraphNode_Compare* Cmp = AddNode<UCrowdyEffectGraphNode_Compare>(Graph);
	Cmp->Comparator = ECrowdyEffectComparator::Greater;
	Cmp->AllocateDefaultPins();

	UCrowdyEffectGraphNode_Attribute* HpThen = MakeAttribute(Graph, ECrowdyEffectRole::Target, TEXT("hp"));
	UCrowdyEffectGraphNode_Constant* ZeroElse = MakeConstant(Graph, ECrowdyEffectGraphConstantType::Number, TEXT("0"));

	UCrowdyEffectGraphNode_If* If = AddNode<UCrowdyEffectGraphNode_If>(Graph);
	If->AllocateDefaultPins();

	UCrowdyEffectGraphNode_Result* Result = MakeResultWithHpWrite(Graph, ECrowdyEffectAssignmentOp::Set);

	Link(HpCond, UCrowdyEffectGraphNode_Attribute::OutputPinName(), Cmp, UCrowdyEffectGraphNode_Compare::InputAPinName());
	Link(ZeroCond, UCrowdyEffectGraphNode_Constant::OutputPinName(), Cmp, UCrowdyEffectGraphNode_Compare::InputBPinName());
	Link(Cmp, UCrowdyEffectGraphNode_Compare::OutputPinName(), If, UCrowdyEffectGraphNode_If::ConditionPinName());
	Link(HpThen, UCrowdyEffectGraphNode_Attribute::OutputPinName(), If, UCrowdyEffectGraphNode_If::ThenPinName());
	Link(ZeroElse, UCrowdyEffectGraphNode_Constant::OutputPinName(), If, UCrowdyEffectGraphNode_If::ElsePinName());
	Link(If, UCrowdyEffectGraphNode_If::OutputPinName(), Result, UCrowdyEffectGraphNode_Result::WritePinName(0));

	CheckParity(*this, Graph, TEXT("self.hp = if(self.hp > 0, self.hp, 0)"));
	return true;
}

// A boolean built from a not, an and, and two comparisons preserves precedence (comparison binds tighter than &&,
// unary tighter still), matching the text !(self.armor < 0) && self.attack > 0.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectGraphLogicAndUnaryTest,
	"CrowdySDK.Editor.GraphLogicAndUnary", CrowdyEffectGraphTestFlags)
bool FCrowdyEffectGraphLogicAndUnaryTest::RunTest(const FString& Parameters)
{
	UCrowdyEffectGraph* Graph = NewObject<UCrowdyEffectGraph>(GetTransientPackage());

	UCrowdyEffectGraphNode_Attribute* Armor = MakeAttribute(Graph, ECrowdyEffectRole::Target, TEXT("armor"));
	UCrowdyEffectGraphNode_Constant* ZeroA = MakeConstant(Graph, ECrowdyEffectGraphConstantType::Number, TEXT("0"));
	UCrowdyEffectGraphNode_Compare* CmpArmor = AddNode<UCrowdyEffectGraphNode_Compare>(Graph);
	CmpArmor->Comparator = ECrowdyEffectComparator::Less;
	CmpArmor->AllocateDefaultPins();

	UCrowdyEffectGraphNode_Unary* Not = AddNode<UCrowdyEffectGraphNode_Unary>(Graph);
	Not->Op = ECrowdyEffectGraphUnaryOp::Not;
	Not->AllocateDefaultPins();

	UCrowdyEffectGraphNode_Attribute* Attack = MakeAttribute(Graph, ECrowdyEffectRole::Target, TEXT("attack"));
	UCrowdyEffectGraphNode_Constant* ZeroB = MakeConstant(Graph, ECrowdyEffectGraphConstantType::Number, TEXT("0"));
	UCrowdyEffectGraphNode_Compare* CmpAttack = AddNode<UCrowdyEffectGraphNode_Compare>(Graph);
	CmpAttack->Comparator = ECrowdyEffectComparator::Greater;
	CmpAttack->AllocateDefaultPins();

	UCrowdyEffectGraphNode_Logic* And = AddNode<UCrowdyEffectGraphNode_Logic>(Graph);
	And->Op = ECrowdyEffectGraphLogicOp::And;
	And->AllocateDefaultPins();

	UCrowdyEffectGraphNode_Attribute* HpThen = MakeAttribute(Graph, ECrowdyEffectRole::Target, TEXT("hp"));
	UCrowdyEffectGraphNode_Constant* ZeroElse = MakeConstant(Graph, ECrowdyEffectGraphConstantType::Number, TEXT("0"));
	UCrowdyEffectGraphNode_If* If = AddNode<UCrowdyEffectGraphNode_If>(Graph);
	If->AllocateDefaultPins();

	UCrowdyEffectGraphNode_Result* Result = MakeResultWithHpWrite(Graph, ECrowdyEffectAssignmentOp::Set);

	Link(Armor, UCrowdyEffectGraphNode_Attribute::OutputPinName(), CmpArmor, UCrowdyEffectGraphNode_Compare::InputAPinName());
	Link(ZeroA, UCrowdyEffectGraphNode_Constant::OutputPinName(), CmpArmor, UCrowdyEffectGraphNode_Compare::InputBPinName());
	Link(CmpArmor, UCrowdyEffectGraphNode_Compare::OutputPinName(), Not, UCrowdyEffectGraphNode_Unary::InputPinName());
	Link(Attack, UCrowdyEffectGraphNode_Attribute::OutputPinName(), CmpAttack, UCrowdyEffectGraphNode_Compare::InputAPinName());
	Link(ZeroB, UCrowdyEffectGraphNode_Constant::OutputPinName(), CmpAttack, UCrowdyEffectGraphNode_Compare::InputBPinName());
	Link(Not, UCrowdyEffectGraphNode_Unary::OutputPinName(), And, UCrowdyEffectGraphNode_Logic::InputAPinName());
	Link(CmpAttack, UCrowdyEffectGraphNode_Compare::OutputPinName(), And, UCrowdyEffectGraphNode_Logic::InputBPinName());
	Link(And, UCrowdyEffectGraphNode_Logic::OutputPinName(), If, UCrowdyEffectGraphNode_If::ConditionPinName());
	Link(HpThen, UCrowdyEffectGraphNode_Attribute::OutputPinName(), If, UCrowdyEffectGraphNode_If::ThenPinName());
	Link(ZeroElse, UCrowdyEffectGraphNode_Constant::OutputPinName(), If, UCrowdyEffectGraphNode_If::ElsePinName());
	Link(If, UCrowdyEffectGraphNode_If::OutputPinName(), Result, UCrowdyEffectGraphNode_Result::WritePinName(0));

	CheckParity(*this, Graph, TEXT("self.hp = if(!(self.armor < 0) && self.attack > 0, self.hp, 0)"));
	return true;
}

// A builtin call with three arguments (clamp) lowers identically to the text clamp(self.hp - $power, 0, 100).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectGraphBuiltinCallTest,
	"CrowdySDK.Editor.GraphBuiltinCall", CrowdyEffectGraphTestFlags)
bool FCrowdyEffectGraphBuiltinCallTest::RunTest(const FString& Parameters)
{
	UCrowdyEffectGraph* Graph = NewObject<UCrowdyEffectGraph>(GetTransientPackage());

	UCrowdyEffectGraphNode_Attribute* Hp = MakeAttribute(Graph, ECrowdyEffectRole::Target, TEXT("hp"));
	UCrowdyEffectGraphNode_Tuning* Power = MakeTuning(Graph, TEXT("power"));
	UCrowdyEffectGraphNode_BinaryOp* Sub = AddNode<UCrowdyEffectGraphNode_BinaryOp>(Graph);
	Sub->Op = ECrowdyEffectGraphArithOp::Subtract;
	Sub->AllocateDefaultPins();

	UCrowdyEffectGraphNode_Constant* Zero = MakeConstant(Graph, ECrowdyEffectGraphConstantType::Number, TEXT("0"));
	UCrowdyEffectGraphNode_Constant* Hundred = MakeConstant(Graph, ECrowdyEffectGraphConstantType::Number, TEXT("100"));

	UCrowdyEffectGraphNode_Call* Clamp = AddNode<UCrowdyEffectGraphNode_Call>(Graph);
	Clamp->Callee = TEXT("clamp");
	Clamp->ArgCount = 3;
	Clamp->AllocateDefaultPins();

	UCrowdyEffectGraphNode_Result* Result = MakeResultWithHpWrite(Graph, ECrowdyEffectAssignmentOp::Set);

	Link(Hp, UCrowdyEffectGraphNode_Attribute::OutputPinName(), Sub, UCrowdyEffectGraphNode_BinaryOp::InputAPinName());
	Link(Power, UCrowdyEffectGraphNode_Tuning::OutputPinName(), Sub, UCrowdyEffectGraphNode_BinaryOp::InputBPinName());
	Link(Sub, UCrowdyEffectGraphNode_BinaryOp::OutputPinName(), Clamp, UCrowdyEffectGraphNode_Call::ArgPinName(0));
	Link(Zero, UCrowdyEffectGraphNode_Constant::OutputPinName(), Clamp, UCrowdyEffectGraphNode_Call::ArgPinName(1));
	Link(Hundred, UCrowdyEffectGraphNode_Constant::OutputPinName(), Clamp, UCrowdyEffectGraphNode_Call::ArgPinName(2));
	Link(Clamp, UCrowdyEffectGraphNode_Call::OutputPinName(), Result, UCrowdyEffectGraphNode_Result::WritePinName(0));

	CheckParity(*this, Graph, TEXT("self.hp = clamp(self.hp - $power, 0, 100)"));
	return true;
}

// An authored server-function call (fn:) with a cross-entity read lowers identically to the equivalent text.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectGraphFnCallTest,
	"CrowdySDK.Editor.GraphFnCall", CrowdyEffectGraphTestFlags)
bool FCrowdyEffectGraphFnCallTest::RunTest(const FString& Parameters)
{
	UCrowdyEffectGraph* Graph = NewObject<UCrowdyEffectGraph>(GetTransientPackage());

	UCrowdyEffectGraphNode_Tuning* Power = MakeTuning(Graph, TEXT("power"));
	UCrowdyEffectGraphNode_Attribute* Attack = MakeAttribute(Graph, ECrowdyEffectRole::Source, TEXT("attack"));

	UCrowdyEffectGraphNode_Call* Fn = AddNode<UCrowdyEffectGraphNode_Call>(Graph);
	Fn->Callee = TEXT("compute_damage");
	Fn->bIsFnCall = true;
	Fn->ArgCount = 2;
	Fn->AllocateDefaultPins();

	UCrowdyEffectGraphNode_Result* Result = MakeResultWithHpWrite(Graph, ECrowdyEffectAssignmentOp::Set);

	Link(Power, UCrowdyEffectGraphNode_Tuning::OutputPinName(), Fn, UCrowdyEffectGraphNode_Call::ArgPinName(0));
	Link(Attack, UCrowdyEffectGraphNode_Attribute::OutputPinName(), Fn, UCrowdyEffectGraphNode_Call::ArgPinName(1));
	Link(Fn, UCrowdyEffectGraphNode_Call::OutputPinName(), Result, UCrowdyEffectGraphNode_Result::WritePinName(0));

	CheckParity(*this, Graph, TEXT("self.hp = fn:compute_damage($power, source.attack)"));
	return true;
}

// A ref-read (ref("abc").hp) inside a coalesce, exercising the ReadRef node and a string constant id.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectGraphReadRefTest,
	"CrowdySDK.Editor.GraphReadRef", CrowdyEffectGraphTestFlags)
bool FCrowdyEffectGraphReadRefTest::RunTest(const FString& Parameters)
{
	UCrowdyEffectGraph* Graph = NewObject<UCrowdyEffectGraph>(GetTransientPackage());

	UCrowdyEffectGraphNode_Constant* Id = MakeConstant(Graph, ECrowdyEffectGraphConstantType::String, TEXT("abc"));
	UCrowdyEffectGraphNode_ReadRef* Ref = AddNode<UCrowdyEffectGraphNode_ReadRef>(Graph);
	Ref->Attribute = TEXT("hp");
	Ref->AllocateDefaultPins();

	UCrowdyEffectGraphNode_Constant* Zero = MakeConstant(Graph, ECrowdyEffectGraphConstantType::Number, TEXT("0"));
	UCrowdyEffectGraphNode_Call* Coalesce = AddNode<UCrowdyEffectGraphNode_Call>(Graph);
	Coalesce->Callee = TEXT("coalesce");
	Coalesce->ArgCount = 2;
	Coalesce->AllocateDefaultPins();

	UCrowdyEffectGraphNode_Result* Result = MakeResultWithHpWrite(Graph, ECrowdyEffectAssignmentOp::Set);

	Link(Id, UCrowdyEffectGraphNode_Constant::OutputPinName(), Ref, UCrowdyEffectGraphNode_ReadRef::IdPinName());
	Link(Ref, UCrowdyEffectGraphNode_ReadRef::OutputPinName(), Coalesce, UCrowdyEffectGraphNode_Call::ArgPinName(0));
	Link(Zero, UCrowdyEffectGraphNode_Constant::OutputPinName(), Coalesce, UCrowdyEffectGraphNode_Call::ArgPinName(1));
	Link(Coalesce, UCrowdyEffectGraphNode_Call::OutputPinName(), Result, UCrowdyEffectGraphNode_Result::WritePinName(0));

	CheckParity(*this, Graph, TEXT("self.hp = coalesce(ref(\"abc\").hp, 0)"));
	return true;
}

// A null constant inside coalesce lowers identically to the text coalesce(null, 0).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectGraphNullConstantTest,
	"CrowdySDK.Editor.GraphNullConstant", CrowdyEffectGraphTestFlags)
bool FCrowdyEffectGraphNullConstantTest::RunTest(const FString& Parameters)
{
	UCrowdyEffectGraph* Graph = NewObject<UCrowdyEffectGraph>(GetTransientPackage());

	UCrowdyEffectGraphNode_Constant* Null = MakeConstant(Graph, ECrowdyEffectGraphConstantType::Null, TEXT(""));
	UCrowdyEffectGraphNode_Constant* Zero = MakeConstant(Graph, ECrowdyEffectGraphConstantType::Number, TEXT("0"));

	UCrowdyEffectGraphNode_Call* Coalesce = AddNode<UCrowdyEffectGraphNode_Call>(Graph);
	Coalesce->Callee = TEXT("coalesce");
	Coalesce->ArgCount = 2;
	Coalesce->AllocateDefaultPins();

	UCrowdyEffectGraphNode_Result* Result = MakeResultWithHpWrite(Graph, ECrowdyEffectAssignmentOp::Set);

	Link(Null, UCrowdyEffectGraphNode_Constant::OutputPinName(), Coalesce, UCrowdyEffectGraphNode_Call::ArgPinName(0));
	Link(Zero, UCrowdyEffectGraphNode_Constant::OutputPinName(), Coalesce, UCrowdyEffectGraphNode_Call::ArgPinName(1));
	Link(Coalesce, UCrowdyEffectGraphNode_Call::OutputPinName(), Result, UCrowdyEffectGraphNode_Result::WritePinName(0));

	CheckParity(*this, Graph, TEXT("self.hp = coalesce(null, 0)"));
	return true;
}

// A unary negate lowers identically to the text -$power.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectGraphUnaryNegateTest,
	"CrowdySDK.Editor.GraphUnaryNegate", CrowdyEffectGraphTestFlags)
bool FCrowdyEffectGraphUnaryNegateTest::RunTest(const FString& Parameters)
{
	UCrowdyEffectGraph* Graph = NewObject<UCrowdyEffectGraph>(GetTransientPackage());

	UCrowdyEffectGraphNode_Tuning* Power = MakeTuning(Graph, TEXT("power"));
	UCrowdyEffectGraphNode_Unary* Negate = AddNode<UCrowdyEffectGraphNode_Unary>(Graph);
	Negate->Op = ECrowdyEffectGraphUnaryOp::Negate;
	Negate->AllocateDefaultPins();

	UCrowdyEffectGraphNode_Result* Result = MakeResultWithHpWrite(Graph, ECrowdyEffectAssignmentOp::Set);

	Link(Power, UCrowdyEffectGraphNode_Tuning::OutputPinName(), Negate, UCrowdyEffectGraphNode_Unary::InputPinName());
	Link(Negate, UCrowdyEffectGraphNode_Unary::OutputPinName(), Result, UCrowdyEffectGraphNode_Result::WritePinName(0));

	CheckParity(*this, Graph, TEXT("self.hp = -$power"));
	return true;
}

// A Compare node (self.armor >= $power) wired into a Result condition pin lowers identically to a 'require' line: the
// compiler unwraps a Compare straight into its comparison require, so it matches "require self.armor >= $power".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectGraphComparisonConditionTest,
	"CrowdySDK.Editor.GraphComparisonCondition", CrowdyEffectGraphTestFlags)
bool FCrowdyEffectGraphComparisonConditionTest::RunTest(const FString& Parameters)
{
	UCrowdyEffectGraph* Graph = NewObject<UCrowdyEffectGraph>(GetTransientPackage());

	UCrowdyEffectGraphNode_Tuning* PowerWrite = MakeTuning(Graph, TEXT("power"));
	UCrowdyEffectGraphNode_Attribute* Armor = MakeAttribute(Graph, ECrowdyEffectRole::Target, TEXT("armor"));
	UCrowdyEffectGraphNode_Tuning* PowerCond = MakeTuning(Graph, TEXT("power"));

	UCrowdyEffectGraphNode_Compare* Cmp = AddNode<UCrowdyEffectGraphNode_Compare>(Graph);
	Cmp->Comparator = ECrowdyEffectComparator::GreaterOrEqual;
	Cmp->AllocateDefaultPins();

	UCrowdyEffectGraphNode_Result* Result = AddNode<UCrowdyEffectGraphNode_Result>(Graph);
	FCrowdyEffectGraphWrite Write;
	Write.TargetRole = ECrowdyEffectRole::Target;
	Write.Attribute = TEXT("hp");
	Write.Op = ECrowdyEffectAssignmentOp::Subtract;
	Result->Writes.Add(Write);
	Result->Requires.Add(FCrowdyEffectGraphRequire());
	Result->AllocateDefaultPins();

	Link(PowerWrite, UCrowdyEffectGraphNode_Tuning::OutputPinName(), Result, UCrowdyEffectGraphNode_Result::WritePinName(0));
	Link(Armor, UCrowdyEffectGraphNode_Attribute::OutputPinName(), Cmp, UCrowdyEffectGraphNode_Compare::InputAPinName());
	Link(PowerCond, UCrowdyEffectGraphNode_Tuning::OutputPinName(), Cmp, UCrowdyEffectGraphNode_Compare::InputBPinName());
	Link(Cmp, UCrowdyEffectGraphNode_Compare::OutputPinName(), Result, UCrowdyEffectGraphNode_Result::RequirePinName(0));

	CheckParity(*this, Graph, TEXT("self.hp -= $power\nrequire self.armor >= $power"));
	return true;
}

// A non-comparison boolean condition (a bare bool attribute) is required to hold via "<expr> == true".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectGraphBooleanConditionTest,
	"CrowdySDK.Editor.GraphBooleanCondition", CrowdyEffectGraphTestFlags)
bool FCrowdyEffectGraphBooleanConditionTest::RunTest(const FString& Parameters)
{
	UCrowdyEffectGraph* Graph = NewObject<UCrowdyEffectGraph>(GetTransientPackage());

	UCrowdyEffectGraphNode_Tuning* Power = MakeTuning(Graph, TEXT("power"));
	UCrowdyEffectGraphNode_Attribute* Alive = MakeAttribute(Graph, ECrowdyEffectRole::Target, TEXT("alive"));

	UCrowdyEffectGraphNode_Result* Result = AddNode<UCrowdyEffectGraphNode_Result>(Graph);
	FCrowdyEffectGraphWrite Write;
	Write.TargetRole = ECrowdyEffectRole::Target;
	Write.Attribute = TEXT("hp");
	Write.Op = ECrowdyEffectAssignmentOp::Subtract;
	Result->Writes.Add(Write);
	Result->Requires.Add(FCrowdyEffectGraphRequire());
	Result->AllocateDefaultPins();

	Link(Power, UCrowdyEffectGraphNode_Tuning::OutputPinName(), Result, UCrowdyEffectGraphNode_Result::WritePinName(0));
	Link(Alive, UCrowdyEffectGraphNode_Attribute::OutputPinName(), Result, UCrowdyEffectGraphNode_Result::RequirePinName(0));

	// The bare-boolean require wraps to "self.alive == true"; assert it not only matches the text form but actually
	// lowers cleanly (a bool compared to a bool literal is a valid gate), so the wrap is real, not parity-with-a-break.
	const FCrowdyEffectLoweringResult Compiled = FCrowdyEffectGraphCompiler::CompileToFunction(Graph, MakeContext());
	TestFalse(TEXT("the bare-boolean condition graph compiles without error"), Compiled.HasErrors());

	CheckParity(*this, Graph, TEXT("self.hp -= $power\nrequire self.alive == true"));
	return true;
}

// A policy-keyword require condition (owner) lowers identically to a 'require owner' line after the assignment.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectGraphKeywordConditionTest,
	"CrowdySDK.Editor.GraphKeywordCondition", CrowdyEffectGraphTestFlags)
bool FCrowdyEffectGraphKeywordConditionTest::RunTest(const FString& Parameters)
{
	UCrowdyEffectGraph* Graph = NewObject<UCrowdyEffectGraph>(GetTransientPackage());

	UCrowdyEffectGraphNode_Tuning* Power = MakeTuning(Graph, TEXT("power"));

	UCrowdyEffectGraphNode_Result* Result = AddNode<UCrowdyEffectGraphNode_Result>(Graph);
	FCrowdyEffectGraphWrite Write;
	Write.TargetRole = ECrowdyEffectRole::Target;
	Write.Attribute = TEXT("hp");
	Write.Op = ECrowdyEffectAssignmentOp::Subtract;
	Result->Writes.Add(Write);
	Result->KeywordConditions.Add(ECrowdyEffectPolicyKeyword::Owner);
	Result->AllocateDefaultPins();

	Link(Power, UCrowdyEffectGraphNode_Tuning::OutputPinName(), Result, UCrowdyEffectGraphNode_Result::WritePinName(0));

	CheckParity(*this, Graph, TEXT("self.hp -= $power\nrequire owner"));
	return true;
}

// A condition whose boolean input is unconnected is an Error diagnostic and emits no require, never a crash; the
// assignment still compiles.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectGraphUnconnectedConditionDiagnosticTest,
	"CrowdySDK.Editor.GraphUnconnectedConditionDiagnostic", CrowdyEffectGraphTestFlags)
bool FCrowdyEffectGraphUnconnectedConditionDiagnosticTest::RunTest(const FString& Parameters)
{
	UCrowdyEffectGraph* Graph = NewObject<UCrowdyEffectGraph>(GetTransientPackage());

	UCrowdyEffectGraphNode_Tuning* PowerWrite = MakeTuning(Graph, TEXT("power"));

	UCrowdyEffectGraphNode_Result* Result = AddNode<UCrowdyEffectGraphNode_Result>(Graph);
	FCrowdyEffectGraphWrite Write;
	Write.TargetRole = ECrowdyEffectRole::Target;
	Write.Attribute = TEXT("hp");
	Write.Op = ECrowdyEffectAssignmentOp::Subtract;
	Result->Writes.Add(Write);
	Result->Requires.Add(FCrowdyEffectGraphRequire());
	Result->AllocateDefaultPins();

	// The write is wired; the condition's boolean input is left unconnected.
	Link(PowerWrite, UCrowdyEffectGraphNode_Tuning::OutputPinName(), Result, UCrowdyEffectGraphNode_Result::WritePinName(0));

	TArray<FCrowdyEffectDiagnostic> Diagnostics;
	const FCrowdyEffectSpec Spec = FCrowdyEffectGraphCompiler::CompileToSpec(Graph, Diagnostics);

	TestEqual(TEXT("the assignment is still emitted"), Spec.Assignments.Num(), 1);
	TestEqual(TEXT("no require is emitted for the unconnected condition"), Spec.Requires.Num(), 0);
	TestTrue(TEXT("an error diagnostic is recorded"),
		Diagnostics.ContainsByPredicate([](const FCrowdyEffectDiagnostic& D)
		{
			return D.Severity == ECrowdyEffectSeverity::Error;
		}));

	return true;
}

// A string constant with an embedded line feed round-trips: the graph escapes it as \n so it reparses to the same
// string value the escaped text "a\nb" produces, instead of a raw newline terminating the literal.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectGraphStringNewlineTest,
	"CrowdySDK.Editor.GraphStringNewlineEscaped", CrowdyEffectGraphTestFlags)
bool FCrowdyEffectGraphStringNewlineTest::RunTest(const FString& Parameters)
{
	UCrowdyEffectGraph* Graph = NewObject<UCrowdyEffectGraph>(GetTransientPackage());

	UCrowdyEffectGraphNode_Constant* Str = MakeConstant(Graph, ECrowdyEffectGraphConstantType::String, TEXT("a\nb"));
	UCrowdyEffectGraphNode_Call* Len = AddNode<UCrowdyEffectGraphNode_Call>(Graph);
	Len->Callee = TEXT("len");
	Len->ArgCount = 1;
	Len->AllocateDefaultPins();
	UCrowdyEffectGraphNode_Result* Result = MakeResultWithHpWrite(Graph, ECrowdyEffectAssignmentOp::Set);

	Link(Str, UCrowdyEffectGraphNode_Constant::OutputPinName(), Len, UCrowdyEffectGraphNode_Call::ArgPinName(0));
	Link(Len, UCrowdyEffectGraphNode_Call::OutputPinName(), Result, UCrowdyEffectGraphNode_Result::WritePinName(0));

	CheckParity(*this, Graph, TEXT("self.hp = len(\"a\\nb\")"));
	return true;
}

// Helper: build a graph whose single value node feeds self.hp's write, then compile and assert an error with no
// assignment emitted. Used by the degenerate-value tests below.
namespace
{
	void ExpectValueNodeError(FAutomationTestBase& Test, UCrowdyEffectGraph* Graph, UEdGraphNode* ValueNode, FName ValueNodeOutputPin)
	{
		UCrowdyEffectGraphNode_Result* Result = MakeResultWithHpWrite(Graph, ECrowdyEffectAssignmentOp::Set);
		Link(ValueNode, ValueNodeOutputPin, Result, UCrowdyEffectGraphNode_Result::WritePinName(0));

		TArray<FCrowdyEffectDiagnostic> Diagnostics;
		const FCrowdyEffectSpec Spec = FCrowdyEffectGraphCompiler::CompileToSpec(Graph, Diagnostics);

		Test.TestEqual(TEXT("no assignment is emitted"), Spec.Assignments.Num(), 0);
		Test.TestTrue(TEXT("an error diagnostic is recorded"),
			Diagnostics.ContainsByPredicate([](const FCrowdyEffectDiagnostic& D)
			{
				return D.Severity == ECrowdyEffectSeverity::Error;
			}));
	}
}

// A string constant with a carriage return cannot be represented and is an Error diagnostic, never malformed output.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectGraphStringCarriageReturnTest,
	"CrowdySDK.Editor.GraphStringCarriageReturnRejected", CrowdyEffectGraphTestFlags)
bool FCrowdyEffectGraphStringCarriageReturnTest::RunTest(const FString& Parameters)
{
	UCrowdyEffectGraph* Graph = NewObject<UCrowdyEffectGraph>(GetTransientPackage());
	UCrowdyEffectGraphNode_Constant* Str = MakeConstant(Graph, ECrowdyEffectGraphConstantType::String, TEXT("a\rb"));
	ExpectValueNodeError(*this, Graph, Str, UCrowdyEffectGraphNode_Constant::OutputPinName());
	return true;
}

// A number constant holding a non-numeric value (a compound expression) is rejected rather than spliced verbatim.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectGraphInvalidNumberTest,
	"CrowdySDK.Editor.GraphInvalidNumberRejected", CrowdyEffectGraphTestFlags)
bool FCrowdyEffectGraphInvalidNumberTest::RunTest(const FString& Parameters)
{
	UCrowdyEffectGraph* Graph = NewObject<UCrowdyEffectGraph>(GetTransientPackage());
	UCrowdyEffectGraphNode_Constant* Num = MakeConstant(Graph, ECrowdyEffectGraphConstantType::Number, TEXT("1+2"));
	ExpectValueNodeError(*this, Graph, Num, UCrowdyEffectGraphNode_Constant::OutputPinName());
	return true;
}

// A tuning node with a blank $param name emits an Error diagnostic instead of the malformed bare "$".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectGraphBlankTuningNameTest,
	"CrowdySDK.Editor.GraphBlankTuningNameRejected", CrowdyEffectGraphTestFlags)
bool FCrowdyEffectGraphBlankTuningNameTest::RunTest(const FString& Parameters)
{
	UCrowdyEffectGraph* Graph = NewObject<UCrowdyEffectGraph>(GetTransientPackage());
	UCrowdyEffectGraphNode_Tuning* Tuning = AddNode<UCrowdyEffectGraphNode_Tuning>(Graph);
	Tuning->ParamName = TEXT("");
	Tuning->AllocateDefaultPins();
	ExpectValueNodeError(*this, Graph, Tuning, UCrowdyEffectGraphNode_Tuning::OutputPinName());
	return true;
}

// Two Result nodes are ambiguous: the compiler records an Error rather than silently dropping the second.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectGraphDuplicateResultTest,
	"CrowdySDK.Editor.GraphDuplicateResultRejected", CrowdyEffectGraphTestFlags)
bool FCrowdyEffectGraphDuplicateResultTest::RunTest(const FString& Parameters)
{
	UCrowdyEffectGraph* Graph = NewObject<UCrowdyEffectGraph>(GetTransientPackage());

	UCrowdyEffectGraphNode_Tuning* Power = MakeTuning(Graph, TEXT("power"));
	UCrowdyEffectGraphNode_Result* First = MakeResultWithHpWrite(Graph, ECrowdyEffectAssignmentOp::Set);
	Link(Power, UCrowdyEffectGraphNode_Tuning::OutputPinName(), First, UCrowdyEffectGraphNode_Result::WritePinName(0));

	// A second Result node the compiler must flag rather than ignore.
	MakeResultWithHpWrite(Graph, ECrowdyEffectAssignmentOp::Set);

	TArray<FCrowdyEffectDiagnostic> Diagnostics;
	FCrowdyEffectGraphCompiler::CompileToSpec(Graph, Diagnostics);

	TestTrue(TEXT("an error diagnostic is recorded for the duplicate Result"),
		Diagnostics.ContainsByPredicate([](const FCrowdyEffectDiagnostic& D)
		{
			return D.Severity == ECrowdyEffectSeverity::Error;
		}));
	return true;
}

// A Result node with no writes and no conditions is an Error diagnostic, not a silently do-nothing effect.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectGraphEmptyResultTest,
	"CrowdySDK.Editor.GraphEmptyResultRejected", CrowdyEffectGraphTestFlags)
bool FCrowdyEffectGraphEmptyResultTest::RunTest(const FString& Parameters)
{
	UCrowdyEffectGraph* Graph = NewObject<UCrowdyEffectGraph>(GetTransientPackage());
	UCrowdyEffectGraphNode_Result* Result = AddNode<UCrowdyEffectGraphNode_Result>(Graph);
	Result->AllocateDefaultPins();

	TArray<FCrowdyEffectDiagnostic> Diagnostics;
	const FCrowdyEffectSpec Spec = FCrowdyEffectGraphCompiler::CompileToSpec(Graph, Diagnostics);

	TestEqual(TEXT("no assignment is emitted"), Spec.Assignments.Num(), 0);
	TestTrue(TEXT("an error diagnostic is recorded"),
		Diagnostics.ContainsByPredicate([](const FCrowdyEffectDiagnostic& D)
		{
			return D.Severity == ECrowdyEffectSeverity::Error;
		}));
	return true;
}

// A Result node that writes self.hp and also returns its post-mutation value lowers identically to the equivalent
// text "self.hp -= $power\nreturn self.hp".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectGraphReturnWithWriteTest,
	"CrowdySDK.Editor.GraphReturnWithWrite", CrowdyEffectGraphTestFlags)
bool FCrowdyEffectGraphReturnWithWriteTest::RunTest(const FString& Parameters)
{
	UCrowdyEffectGraph* Graph = NewObject<UCrowdyEffectGraph>(GetTransientPackage());

	UCrowdyEffectGraphNode_Tuning* Power = MakeTuning(Graph, TEXT("power"));
	UCrowdyEffectGraphNode_Attribute* HpReturn = MakeAttribute(Graph, ECrowdyEffectRole::Target, TEXT("hp"));

	UCrowdyEffectGraphNode_Result* Result = MakeResultWithHpWrite(Graph, ECrowdyEffectAssignmentOp::Subtract);
	Result->bReturnsValue = true;
	Result->AllocateDefaultPins();

	Link(Power, UCrowdyEffectGraphNode_Tuning::OutputPinName(), Result, UCrowdyEffectGraphNode_Result::WritePinName(0));
	Link(HpReturn, UCrowdyEffectGraphNode_Attribute::OutputPinName(), Result, UCrowdyEffectGraphNode_Result::ReturnPinName());

	CheckParity(*this, Graph, TEXT("self.hp -= $power\nreturn self.hp"));
	return true;
}

// A Result node whose ONLY output is a Return (no writes, no conditions) compiles cleanly: the guard that used to
// reject an empty Result now recognizes a return-only query as legitimate output.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectGraphReturnOnlyTest,
	"CrowdySDK.Editor.GraphReturnOnlyCompiles", CrowdyEffectGraphTestFlags)
bool FCrowdyEffectGraphReturnOnlyTest::RunTest(const FString& Parameters)
{
	UCrowdyEffectGraph* Graph = NewObject<UCrowdyEffectGraph>(GetTransientPackage());

	UCrowdyEffectGraphNode_Attribute* Armor = MakeAttribute(Graph, ECrowdyEffectRole::Target, TEXT("armor"));
	UCrowdyEffectGraphNode_Tuning* Power = MakeTuning(Graph, TEXT("power"));

	UCrowdyEffectGraphNode_BinaryOp* Add = AddNode<UCrowdyEffectGraphNode_BinaryOp>(Graph);
	Add->Op = ECrowdyEffectGraphArithOp::Add;
	Add->AllocateDefaultPins();

	UCrowdyEffectGraphNode_Result* Result = AddNode<UCrowdyEffectGraphNode_Result>(Graph);
	Result->bReturnsValue = true;
	Result->AllocateDefaultPins();

	Link(Armor, UCrowdyEffectGraphNode_Attribute::OutputPinName(), Add, UCrowdyEffectGraphNode_BinaryOp::InputAPinName());
	Link(Power, UCrowdyEffectGraphNode_Tuning::OutputPinName(), Add, UCrowdyEffectGraphNode_BinaryOp::InputBPinName());
	Link(Add, UCrowdyEffectGraphNode_BinaryOp::OutputPinName(), Result, UCrowdyEffectGraphNode_Result::ReturnPinName());

	TArray<FCrowdyEffectDiagnostic> Diagnostics;
	const FCrowdyEffectSpec Spec = FCrowdyEffectGraphCompiler::CompileToSpec(Graph, Diagnostics);

	TestEqual(TEXT("no writes are emitted"), Spec.Assignments.Num(), 0);
	TestTrue(TEXT("the spec carries a return"), Spec.bHasReturn);
	TestEqual(TEXT("the return operand is the emitted expression"), Spec.Return.Literal, TEXT("(self.armor + $power)"));
	TestFalse(TEXT("no error diagnostic is recorded"),
		Diagnostics.ContainsByPredicate([](const FCrowdyEffectDiagnostic& D)
		{
			return D.Severity == ECrowdyEffectSeverity::Error;
		}));

	// The untyped-return warning carries a column, so the two surfaces are compared on the function they produce
	// and the warning is asserted on each side by message.
	CheckFunctionParity(*this, Graph, TEXT("return (self.armor + $power)"));

	const FCrowdyEffectLoweringResult GraphResult =
		FCrowdyEffectGraphCompiler::CompileToFunction(Graph, MakeContext());
	TestTrue(TEXT("the graph warns that the return is untyped"),
		AnyDiagnosticContains(GraphResult.Diagnostics, ECrowdyEffectSeverity::Warning,
			TEXT("declares no return type")));

	const FCrowdyEffectParseResult Text = FCrowdyEffectParser::Parse(TEXT("return (self.armor + $power)"));
	TestTrue(TEXT("and so does the equivalent text"),
		AnyDiagnosticContains(FCrowdyEffectLowering::Lower(Text.Program, MakeContext()).Diagnostics,
			ECrowdyEffectSeverity::Warning, TEXT("declares no return type")));
	return true;
}

// A Result node that says it returns a value but has nothing wired into the Return pin is an Error diagnostic, not
// a crash and not a silent empty return.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectGraphReturnUnconnectedTest,
	"CrowdySDK.Editor.GraphReturnUnconnectedDiagnostic", CrowdyEffectGraphTestFlags)
bool FCrowdyEffectGraphReturnUnconnectedTest::RunTest(const FString& Parameters)
{
	UCrowdyEffectGraph* Graph = NewObject<UCrowdyEffectGraph>(GetTransientPackage());

	UCrowdyEffectGraphNode_Result* Result = MakeResultWithHpWrite(Graph, ECrowdyEffectAssignmentOp::Set);
	Result->bReturnsValue = true;
	Result->AllocateDefaultPins();

	UCrowdyEffectGraphNode_Tuning* Power = MakeTuning(Graph, TEXT("power"));
	Link(Power, UCrowdyEffectGraphNode_Tuning::OutputPinName(), Result, UCrowdyEffectGraphNode_Result::WritePinName(0));
	// The Return pin is left unconnected.

	TArray<FCrowdyEffectDiagnostic> Diagnostics;
	const FCrowdyEffectSpec Spec = FCrowdyEffectGraphCompiler::CompileToSpec(Graph, Diagnostics);

	TestEqual(TEXT("the write still compiles"), Spec.Assignments.Num(), 1);
	TestFalse(TEXT("no return is emitted"), Spec.bHasReturn);
	TestTrue(TEXT("an error diagnostic is recorded"),
		Diagnostics.ContainsByPredicate([](const FCrowdyEffectDiagnostic& D)
		{
			return D.Severity == ECrowdyEffectSeverity::Error;
		}));

	return true;
}

// A Result node with bReturnsValue false produces no return at all, even though the graph has a value that could
// have fed one; the Return pin simply does not exist to wire into.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectGraphNoReturnByDefaultTest,
	"CrowdySDK.Editor.GraphNoReturnByDefault", CrowdyEffectGraphTestFlags)
bool FCrowdyEffectGraphNoReturnByDefaultTest::RunTest(const FString& Parameters)
{
	UCrowdyEffectGraph* Graph = BuildDamageGraph();

	TArray<FCrowdyEffectDiagnostic> Diagnostics;
	const FCrowdyEffectSpec Spec = FCrowdyEffectGraphCompiler::CompileToSpec(Graph, Diagnostics);

	TestFalse(TEXT("the spec carries no return"), Spec.bHasReturn);
	TestTrue(TEXT("the return operand is left default"), Spec.Return.Literal.IsEmpty());

	return true;
}

// The asset-storage end-to-end: a UCrowdyEffect in Graph mode compiles through the registered editor hook to the
// same function input the equivalent Text effect produces. This exercises the whole asset path (Compile() dispatch,
// the no-cycle hook, attribute discovery, the shared context / magnitude / carrier / automation handling), not just
// the bare graph compiler, and confirms the editor module registered the hook at startup.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectGraphAssetModeCompilesTest,
	"CrowdySDK.Editor.GraphAssetModeCompiles", CrowdyEffectGraphTestFlags)
bool FCrowdyEffectGraphAssetModeCompilesTest::RunTest(const FString& Parameters)
{
	// Build self.health -= 5 as a graph against the test container's health attribute.
	UCrowdyEffectGraph* Graph = NewObject<UCrowdyEffectGraph>(GetTransientPackage());
	UCrowdyEffectGraphNode_Constant* Five = MakeConstant(Graph, ECrowdyEffectGraphConstantType::Number, TEXT("5"));

	UCrowdyEffectGraphNode_Result* Result = AddNode<UCrowdyEffectGraphNode_Result>(Graph);
	FCrowdyEffectGraphWrite Write;
	Write.TargetRole = ECrowdyEffectRole::Target;
	Write.Attribute = TEXT("health");
	Write.Op = ECrowdyEffectAssignmentOp::Subtract;
	Result->Writes.Add(Write);
	Result->AllocateDefaultPins();
	Link(Five, UCrowdyEffectGraphNode_Constant::OutputPinName(), Result, UCrowdyEffectGraphNode_Result::WritePinName(0));

	UCrowdyEffect* GraphEffect = NewObject<UCrowdyEffect>(GetTransientPackage());
	GraphEffect->ContainerClass = UCrowdyEffectGraphTestContainer::StaticClass();
	GraphEffect->FunctionName = TEXT("take_damage");
	GraphEffect->Source = ECrowdyEffectSource::Graph;
	GraphEffect->EffectGraph = Graph;

	UCrowdyEffect* TextEffect = NewObject<UCrowdyEffect>(GetTransientPackage());
	TextEffect->ContainerClass = UCrowdyEffectGraphTestContainer::StaticClass();
	TextEffect->FunctionName = TEXT("take_damage");
	TextEffect->Source = ECrowdyEffectSource::Text;
	TextEffect->EffectScript = TEXT("self.health -= 5");

	const FCrowdyEffectLoweringResult GraphResult = GraphEffect->Compile();
	const FCrowdyEffectLoweringResult TextResult = TextEffect->Compile();

	TestFalse(TEXT("the graph asset compiles without error"), GraphResult.HasErrors());
	TestEqual(TEXT("one mutation is produced"), GraphResult.Function.Mutations.Num(), 1);
	TestEqual(TEXT("the graph and text assets compile to identical function inputs"),
		CrowdyStringifyResult(GraphResult), CrowdyStringifyResult(TextResult));

	return true;
}

// A Graph-mode effect with no graph assigned is an Error, not a silent blank, and it does NOT fall through to the
// text path: the leftover EffectScript here is ignored, so the only result is the graph compiler's missing-graph
// diagnostic. Proves Compile() dispatches on Source rather than compiling whatever body happens to be present.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectGraphAssetModeMissingGraphTest,
	"CrowdySDK.Editor.GraphAssetModeMissingGraph", CrowdyEffectGraphTestFlags)
bool FCrowdyEffectGraphAssetModeMissingGraphTest::RunTest(const FString& Parameters)
{
	UCrowdyEffect* Effect = NewObject<UCrowdyEffect>(GetTransientPackage());
	Effect->ContainerClass = UCrowdyEffectGraphTestContainer::StaticClass();
	Effect->FunctionName = TEXT("take_damage");
	Effect->Source = ECrowdyEffectSource::Graph;
	Effect->EffectGraph = nullptr;
	// A valid text body left behind must be ignored in Graph mode; if dispatch were wrong it would produce a mutation.
	Effect->EffectScript = TEXT("self.health -= 5");

	const FCrowdyEffectLoweringResult R = Effect->Compile();

	TestTrue(TEXT("a Graph effect with no graph is an error, not a silent blank"), R.HasErrors());
	TestTrue(TEXT("the error is specifically the missing-graph diagnostic, not an unrelated failure"),
		R.Diagnostics.ContainsByPredicate([](const FCrowdyEffectDiagnostic& D)
		{
			return D.Severity == ECrowdyEffectSeverity::Error && D.Message.Contains(TEXT("effect graph"));
		}));
	TestEqual(TEXT("no mutation is produced (the text body is not compiled in Graph mode)"),
		R.Function.Mutations.Num(), 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
