// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Graph/CrowdyEffectGraph.h"
#include "Graph/CrowdyEffectGraphCompiler.h"
#include "Graph/CrowdyEffectGraphNodeOptions.h"
#include "Graph/CrowdyEffectGraphNodes.h"
#include "Graph/CrowdyEffectGraphSchema.h"
#include "Replication/GameModel/CrowdyAttributeRegistry.h"
#include "Replication/GameModel/Effect/CrowdyEffectLowering.h"
#include "Replication/GameModel/Effect/CrowdyEffectParser.h"
#include "Replication/GameModel/Effect/CrowdyEffectSpec.h"
#include "Replication/GameModel/Effect/CrowdyEffectStringifyTestSupport.h"
#include "Replication/GameModel/Effect/CrowdyGameModelFunctionInput.h"
#include "UObject/Package.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyEffectGraphSchemaTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// A graph with its schema class set, the way the toolkit creates one. A node method that routes through
	// UEdGraphNode's base (GetPinDisplayName, for one) dereferences GetSchema() unchecked, so a schema-less graph
	// crashes rather than failing an assertion.
	UCrowdyEffectGraph* MakeGraph()
	{
		UCrowdyEffectGraph* Graph = NewObject<UCrowdyEffectGraph>(GetTransientPackage());
		Graph->Schema = UCrowdyEffectGraphSchema::StaticClass();
		return Graph;
	}

	template <typename TNode>
	TNode* AddNodeWithPins(UCrowdyEffectGraph* Graph)
	{
		TNode* Node = NewObject<TNode>(Graph);
		Node->CreateNewGuid();
		Graph->Nodes.Add(Node);
		Node->AllocateDefaultPins();
		return Node;
	}

	UCrowdyEffectGraphNode_BinaryOp* AddBinaryOp(UCrowdyEffectGraph* Graph)
	{
		UCrowdyEffectGraphNode_BinaryOp* Node = AddNodeWithPins<UCrowdyEffectGraphNode_BinaryOp>(Graph);
		Node->Op = ECrowdyEffectGraphArithOp::Add;
		return Node;
	}

	UCrowdyEffectGraphNode_Constant* AddConstant(UCrowdyEffectGraph* Graph, const TCHAR* Number)
	{
		UCrowdyEffectGraphNode_Constant* Node = AddNodeWithPins<UCrowdyEffectGraphNode_Constant>(Graph);
		Node->ConstantType = ECrowdyEffectGraphConstantType::Number;
		Node->Literal = Number;
		return Node;
	}

	// A Unary node with its operator set before its pins are allocated, so the pin categories match the operator (Not is
	// boolean, Negate is a plain value).
	UCrowdyEffectGraphNode_Unary* AddUnary(UCrowdyEffectGraph* Graph, ECrowdyEffectGraphUnaryOp Op)
	{
		UCrowdyEffectGraphNode_Unary* Node = NewObject<UCrowdyEffectGraphNode_Unary>(Graph);
		Node->CreateNewGuid();
		Graph->Nodes.Add(Node);
		Node->Op = Op;
		Node->AllocateDefaultPins();
		return Node;
	}

	ECanCreateConnectionResponse Response(const UEdGraphPin* A, const UEdGraphPin* B)
	{
		return GetDefault<UCrowdyEffectGraphSchema>()->CanCreateConnection(A, B).Response;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCrowdyEffectGraphSchemaValueConnectsTest,
	"CrowdySDK.Editor.GraphSchemaValueOutputToInputConnects",
	CrowdyEffectGraphSchemaTestFlags)

bool FCrowdyEffectGraphSchemaValueConnectsTest::RunTest(const FString&)
{
	UCrowdyEffectGraph* Graph = MakeGraph();
	UCrowdyEffectGraphNode_Constant* Const = AddConstant(Graph, TEXT("5"));
	UCrowdyEffectGraphNode_BinaryOp* Op = AddBinaryOp(Graph);

	const UEdGraphPin* Out = Const->FindPin(UCrowdyEffectGraphNode_Constant::OutputPinName(), EGPD_Output);
	const UEdGraphPin* In = Op->FindPin(UCrowdyEffectGraphNode_BinaryOp::InputAPinName(), EGPD_Input);

	TestEqual(TEXT("a value output connects to a free value input"),
		Response(Out, In), CONNECT_RESPONSE_MAKE);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCrowdyEffectGraphSchemaRejectsSameDirectionTest,
	"CrowdySDK.Editor.GraphSchemaRejectsSameDirection",
	CrowdyEffectGraphSchemaTestFlags)

bool FCrowdyEffectGraphSchemaRejectsSameDirectionTest::RunTest(const FString&)
{
	UCrowdyEffectGraph* Graph = MakeGraph();
	UCrowdyEffectGraphNode_Constant* A = AddConstant(Graph, TEXT("1"));
	UCrowdyEffectGraphNode_Constant* B = AddConstant(Graph, TEXT("2"));

	const UEdGraphPin* OutA = A->FindPin(UCrowdyEffectGraphNode_Constant::OutputPinName(), EGPD_Output);
	const UEdGraphPin* OutB = B->FindPin(UCrowdyEffectGraphNode_Constant::OutputPinName(), EGPD_Output);

	TestEqual(TEXT("two outputs cannot be connected"),
		Response(OutA, OutB), CONNECT_RESPONSE_DISALLOW);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCrowdyEffectGraphSchemaRejectsSelfTest,
	"CrowdySDK.Editor.GraphSchemaRejectsSelfConnection",
	CrowdyEffectGraphSchemaTestFlags)

bool FCrowdyEffectGraphSchemaRejectsSelfTest::RunTest(const FString&)
{
	UCrowdyEffectGraph* Graph = MakeGraph();
	UCrowdyEffectGraphNode_BinaryOp* Op = AddBinaryOp(Graph);

	const UEdGraphPin* Out = Op->FindPin(UCrowdyEffectGraphNode_BinaryOp::OutputPinName(), EGPD_Output);
	const UEdGraphPin* In = Op->FindPin(UCrowdyEffectGraphNode_BinaryOp::InputAPinName(), EGPD_Input);

	TestEqual(TEXT("a node cannot connect to itself"),
		Response(Out, In), CONNECT_RESPONSE_DISALLOW);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCrowdyEffectGraphSchemaInputSingleLinkTest,
	"CrowdySDK.Editor.GraphSchemaInputSingleLinkBreaks",
	CrowdyEffectGraphSchemaTestFlags)

bool FCrowdyEffectGraphSchemaInputSingleLinkTest::RunTest(const FString&)
{
	UCrowdyEffectGraph* Graph = MakeGraph();
	UCrowdyEffectGraphNode_Constant* First = AddConstant(Graph, TEXT("1"));
	UCrowdyEffectGraphNode_Constant* Second = AddConstant(Graph, TEXT("2"));
	UCrowdyEffectGraphNode_BinaryOp* Op = AddBinaryOp(Graph);

	UEdGraphPin* FirstOut = First->FindPin(UCrowdyEffectGraphNode_Constant::OutputPinName(), EGPD_Output);
	UEdGraphPin* SecondOut = Second->FindPin(UCrowdyEffectGraphNode_Constant::OutputPinName(), EGPD_Output);
	UEdGraphPin* In = Op->FindPin(UCrowdyEffectGraphNode_BinaryOp::InputAPinName(), EGPD_Input);

	FirstOut->MakeLinkTo(In);

	// The input already holds First's link, so a second connection must replace it (break on the input side), not add
	// a second link and not be refused.
	TestEqual(TEXT("a second connection into a used input replaces the existing one"),
		Response(SecondOut, In), CONNECT_RESPONSE_BREAK_OTHERS_B);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCrowdyEffectGraphSchemaRejectsCycleTest,
	"CrowdySDK.Editor.GraphSchemaRejectsCycle",
	CrowdyEffectGraphSchemaTestFlags)

bool FCrowdyEffectGraphSchemaRejectsCycleTest::RunTest(const FString&)
{
	UCrowdyEffectGraph* Graph = MakeGraph();
	UCrowdyEffectGraphNode_BinaryOp* Upstream = AddBinaryOp(Graph);
	UCrowdyEffectGraphNode_BinaryOp* Downstream = AddBinaryOp(Graph);

	// Upstream feeds Downstream.
	UEdGraphPin* UpOut = Upstream->FindPin(UCrowdyEffectGraphNode_BinaryOp::OutputPinName(), EGPD_Output);
	UEdGraphPin* DownInA = Downstream->FindPin(UCrowdyEffectGraphNode_BinaryOp::InputAPinName(), EGPD_Input);
	UpOut->MakeLinkTo(DownInA);

	// Wiring Downstream's output back into Upstream would close the loop.
	const UEdGraphPin* DownOut = Downstream->FindPin(UCrowdyEffectGraphNode_BinaryOp::OutputPinName(), EGPD_Output);
	const UEdGraphPin* UpInB = Upstream->FindPin(UCrowdyEffectGraphNode_BinaryOp::InputBPinName(), EGPD_Input);

	TestEqual(TEXT("a connection that would close a cycle is refused"),
		Response(DownOut, UpInB), CONNECT_RESPONSE_DISALLOW);
	TestTrue(TEXT("WouldConnectionCauseLoop detects the cycle directly"),
		UCrowdyEffectGraphSchema::WouldConnectionCauseLoop(DownOut, UpInB));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCrowdyEffectGraphResultReconstructKeepsWiringTest,
	"CrowdySDK.Editor.GraphResultReconstructPreservesWiringByStableId",
	CrowdyEffectGraphSchemaTestFlags)

bool FCrowdyEffectGraphResultReconstructKeepsWiringTest::RunTest(const FString&)
{
	UCrowdyEffectGraph* Graph = MakeGraph();

	UCrowdyEffectGraphNode_Constant* HpValue = AddConstant(Graph, TEXT("1"));
	UCrowdyEffectGraphNode_Constant* ManaValue = AddConstant(Graph, TEXT("2"));

	// A Result with two writes: [0] = hp, [1] = mana.
	UCrowdyEffectGraphNode_Result* Result = NewObject<UCrowdyEffectGraphNode_Result>(Graph);
	Result->CreateNewGuid();
	Graph->Nodes.Add(Result);
	FCrowdyEffectGraphWrite Hp;
	Hp.Attribute = TEXT("hp");
	FCrowdyEffectGraphWrite Mana;
	Mana.Attribute = TEXT("mana");
	Result->Writes.Add(Hp);
	Result->Writes.Add(Mana);
	Result->AllocateDefaultPins();

	HpValue->FindPin(UCrowdyEffectGraphNode_Constant::OutputPinName(), EGPD_Output)
		->MakeLinkTo(Result->FindPin(UCrowdyEffectGraphNode_Result::WritePinName(0), EGPD_Input));
	ManaValue->FindPin(UCrowdyEffectGraphNode_Constant::OutputPinName(), EGPD_Output)
		->MakeLinkTo(Result->FindPin(UCrowdyEffectGraphNode_Result::WritePinName(1), EGPD_Input));

	// Remove the first write (hp), then reconstruct as a details-panel edit would.
	Result->Writes.RemoveAt(0);
	Result->ReconstructNode();

	// The single remaining write (mana, now at index 0) must still carry mana's value, not hp's.
	const UEdGraphPin* SurvivingPin = Result->FindPin(UCrowdyEffectGraphNode_Result::WritePinName(0), EGPD_Input);
	if (!TestNotNull(TEXT("the surviving write pin exists"), SurvivingPin))
	{
		return false;
	}
	TestEqual(TEXT("the surviving write keeps exactly one wired value"), SurvivingPin->LinkedTo.Num(), 1);
	if (SurvivingPin->LinkedTo.Num() == 1)
	{
		TestEqual(TEXT("the surviving write is wired to mana's value, not hp's"),
			SurvivingPin->LinkedTo[0]->GetOwningNode(), Cast<UEdGraphNode>(ManaValue));
	}

	// The removed write's value node is now unwired.
	const UEdGraphPin* HpOut = HpValue->FindPin(UCrowdyEffectGraphNode_Constant::OutputPinName(), EGPD_Output);
	TestEqual(TEXT("the removed write's value node is disconnected"), HpOut->LinkedTo.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCrowdyEffectGraphSchemaBoolToBoolTest,
	"CrowdySDK.Editor.GraphSchemaBoolProducerConnectsToBoolInput",
	CrowdyEffectGraphSchemaTestFlags)

bool FCrowdyEffectGraphSchemaBoolToBoolTest::RunTest(const FString&)
{
	UCrowdyEffectGraph* Graph = MakeGraph();
	UCrowdyEffectGraphNode_Compare* Compare = AddNodeWithPins<UCrowdyEffectGraphNode_Compare>(Graph);
	UCrowdyEffectGraphNode_If* If = AddNodeWithPins<UCrowdyEffectGraphNode_If>(Graph);

	const UEdGraphPin* BoolOut = Compare->FindPin(UCrowdyEffectGraphNode_Compare::OutputPinName(), EGPD_Output);
	const UEdGraphPin* BoolIn = If->FindPin(UCrowdyEffectGraphNode_If::ConditionPinName(), EGPD_Input);

	TestEqual(TEXT("a comparison (a boolean producer) connects to a boolean input"),
		Response(BoolOut, BoolIn), CONNECT_RESPONSE_MAKE);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCrowdyEffectGraphSchemaAmbiguousToBoolTest,
	"CrowdySDK.Editor.GraphSchemaAmbiguousValueConnectsToBoolInput",
	CrowdyEffectGraphSchemaTestFlags)

bool FCrowdyEffectGraphSchemaAmbiguousToBoolTest::RunTest(const FString&)
{
	UCrowdyEffectGraph* Graph = MakeGraph();
	UCrowdyEffectGraphNode_Attribute* Attribute = AddNodeWithPins<UCrowdyEffectGraphNode_Attribute>(Graph);
	UCrowdyEffectGraphNode_If* If = AddNodeWithPins<UCrowdyEffectGraphNode_If>(Graph);

	const UEdGraphPin* AttrOut = Attribute->FindPin(UCrowdyEffectGraphNode_Attribute::OutputPinName(), EGPD_Output);
	const UEdGraphPin* BoolIn = If->FindPin(UCrowdyEffectGraphNode_If::ConditionPinName(), EGPD_Input);

	// An attribute genuinely can be a bool at runtime, so it is allowed into a boolean input (not over-restricted).
	TestEqual(TEXT("an attribute (an ambiguous value) is allowed into a boolean input"),
		Response(AttrOut, BoolIn), CONNECT_RESPONSE_MAKE);
	TestFalse(TEXT("an attribute is not classified as plainly non-boolean"),
		UCrowdyEffectGraphSchema::IsPlainlyNonBooleanProducer(Attribute));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCrowdyEffectGraphSchemaRejectsNumberIntoBoolTest,
	"CrowdySDK.Editor.GraphSchemaRejectsNumberConstantIntoBoolInput",
	CrowdyEffectGraphSchemaTestFlags)

bool FCrowdyEffectGraphSchemaRejectsNumberIntoBoolTest::RunTest(const FString&)
{
	UCrowdyEffectGraph* Graph = MakeGraph();
	UCrowdyEffectGraphNode_Constant* Number = AddConstant(Graph, TEXT("5"));
	UCrowdyEffectGraphNode_If* If = AddNodeWithPins<UCrowdyEffectGraphNode_If>(Graph);

	const UEdGraphPin* NumberOut = Number->FindPin(UCrowdyEffectGraphNode_Constant::OutputPinName(), EGPD_Output);
	const UEdGraphPin* BoolIn = If->FindPin(UCrowdyEffectGraphNode_If::ConditionPinName(), EGPD_Input);

	TestEqual(TEXT("a number constant cannot gate a boolean input"),
		Response(NumberOut, BoolIn), CONNECT_RESPONSE_DISALLOW);
	TestTrue(TEXT("a number constant is classified as plainly non-boolean"),
		UCrowdyEffectGraphSchema::IsPlainlyNonBooleanProducer(Number));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCrowdyEffectGraphSchemaRejectsArithmeticIntoBoolTest,
	"CrowdySDK.Editor.GraphSchemaRejectsArithmeticIntoBoolInput",
	CrowdyEffectGraphSchemaTestFlags)

bool FCrowdyEffectGraphSchemaRejectsArithmeticIntoBoolTest::RunTest(const FString&)
{
	UCrowdyEffectGraph* Graph = MakeGraph();
	UCrowdyEffectGraphNode_BinaryOp* Arith = AddBinaryOp(Graph);
	UCrowdyEffectGraphNode_Logic* Logic = AddNodeWithPins<UCrowdyEffectGraphNode_Logic>(Graph);

	const UEdGraphPin* ArithOut = Arith->FindPin(UCrowdyEffectGraphNode_BinaryOp::OutputPinName(), EGPD_Output);
	const UEdGraphPin* BoolIn = Logic->FindPin(UCrowdyEffectGraphNode_Logic::InputAPinName(), EGPD_Input);

	TestEqual(TEXT("an arithmetic result cannot feed a boolean logic input"),
		Response(ArithOut, BoolIn), CONNECT_RESPONSE_DISALLOW);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCrowdyEffectGraphSchemaRejectsNegateIntoBoolTest,
	"CrowdySDK.Editor.GraphSchemaRejectsNegateIntoBoolInput",
	CrowdyEffectGraphSchemaTestFlags)

bool FCrowdyEffectGraphSchemaRejectsNegateIntoBoolTest::RunTest(const FString&)
{
	UCrowdyEffectGraph* Graph = MakeGraph();
	UCrowdyEffectGraphNode_Unary* Negate = AddUnary(Graph, ECrowdyEffectGraphUnaryOp::Negate);
	UCrowdyEffectGraphNode_If* If = AddNodeWithPins<UCrowdyEffectGraphNode_If>(Graph);

	const UEdGraphPin* NegateOut = Negate->FindPin(UCrowdyEffectGraphNode_Unary::OutputPinName(), EGPD_Output);
	const UEdGraphPin* BoolIn = If->FindPin(UCrowdyEffectGraphNode_If::ConditionPinName(), EGPD_Input);

	TestEqual(TEXT("an arithmetic negate cannot gate a boolean input"),
		Response(NegateOut, BoolIn), CONNECT_RESPONSE_DISALLOW);
	TestTrue(TEXT("a negate is classified as plainly non-boolean"),
		UCrowdyEffectGraphSchema::IsPlainlyNonBooleanProducer(Negate));

	// A logical not, by contrast, is a boolean producer and connects.
	UCrowdyEffectGraphNode_Unary* Not = AddUnary(Graph, ECrowdyEffectGraphUnaryOp::Not);
	const UEdGraphPin* NotOut = Not->FindPin(UCrowdyEffectGraphNode_Unary::OutputPinName(), EGPD_Output);
	UCrowdyEffectGraphNode_If* If2 = AddNodeWithPins<UCrowdyEffectGraphNode_If>(Graph);
	const UEdGraphPin* BoolIn2 = If2->FindPin(UCrowdyEffectGraphNode_If::ConditionPinName(), EGPD_Input);
	TestEqual(TEXT("a logical not connects to a boolean input"),
		Response(NotOut, BoolIn2), CONNECT_RESPONSE_MAKE);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCrowdyEffectGraphSchemaBoolToValueTest,
	"CrowdySDK.Editor.GraphSchemaBoolProducerConnectsToValueInput",
	CrowdyEffectGraphSchemaTestFlags)

bool FCrowdyEffectGraphSchemaBoolToValueTest::RunTest(const FString&)
{
	UCrowdyEffectGraph* Graph = MakeGraph();
	UCrowdyEffectGraphNode_Compare* Compare = AddNodeWithPins<UCrowdyEffectGraphNode_Compare>(Graph);
	UCrowdyEffectGraphNode_BinaryOp* Arith = AddBinaryOp(Graph);

	const UEdGraphPin* BoolOut = Compare->FindPin(UCrowdyEffectGraphNode_Compare::OutputPinName(), EGPD_Output);
	const UEdGraphPin* ValueIn = Arith->FindPin(UCrowdyEffectGraphNode_BinaryOp::InputAPinName(), EGPD_Input);

	// A value input is permissive, so a boolean output may feed it (a bool participates in arithmetic as 0/1).
	TestEqual(TEXT("a boolean producer connects to a permissive value input"),
		Response(BoolOut, ValueIn), CONNECT_RESPONSE_MAKE);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCrowdyEffectGraphMenuGranularOperatorsTest,
	"CrowdySDK.Editor.GraphMenuOffersOneEntryPerOperator",
	CrowdyEffectGraphSchemaTestFlags)

bool FCrowdyEffectGraphMenuGranularOperatorsTest::RunTest(const FString&)
{
	UCrowdyEffectGraph* Graph = MakeGraph();

	FGraphContextMenuBuilder MenuBuilder(Graph);
	GetDefault<UCrowdyEffectGraphSchema>()->GetGraphContextActions(MenuBuilder);

	// The distinct preset values offered for each node class, so a bundled entry (one generic node the author must then
	// retarget) or a duplicated operator both fail.
	TMap<UClass*, TSet<FString>> PresetsByClass;
	TMap<UClass*, int32> EntriesByClass;
	int32 UnpresetOperatorEntries = 0;

	// The action menu draws a divider wherever the grouping changes, so every entry must share one grouping or the
	// palette breaks into a boxed-off row per operation.
	TSet<int32> Groupings;

	const TSet<UClass*> OperatorClasses = {
		UCrowdyEffectGraphNode_Attribute::StaticClass(),
		UCrowdyEffectGraphNode_Constant::StaticClass(),
		UCrowdyEffectGraphNode_BinaryOp::StaticClass(),
		UCrowdyEffectGraphNode_Compare::StaticClass(),
		UCrowdyEffectGraphNode_Logic::StaticClass(),
		UCrowdyEffectGraphNode_Unary::StaticClass()
	};

	for (int32 Index = 0; Index < MenuBuilder.GetNumActions(); ++Index)
	{
		const TSharedPtr<FEdGraphSchemaAction> Action = MenuBuilder.GetSchemaAction(Index);
		if (!Action.IsValid() || Action->GetTypeId() != FCrowdyEffectGraphSchemaAction_NewNode::StaticGetTypeId())
		{
			continue;
		}

		const FCrowdyEffectGraphSchemaAction_NewNode& NewNode =
			static_cast<const FCrowdyEffectGraphSchemaAction_NewNode&>(*Action);
		UClass* NodeClass = NewNode.NodeClass.Get();
		if (!NodeClass)
		{
			continue;
		}

		EntriesByClass.FindOrAdd(NodeClass)++;
		Groupings.Add(NewNode.GetGrouping());
		if (NewNode.Presets.Num() > 0)
		{
			// One key per entry identifies the operation it spawns, so a duplicated operator collapses in the set.
			FString Signature;
			for (const TPair<FName, FString>& Preset : NewNode.Presets)
			{
				Signature += Preset.Key.ToString() + TEXT("=") + Preset.Value + TEXT(";");
			}
			PresetsByClass.FindOrAdd(NodeClass).Add(Signature);
		}
		else if (OperatorClasses.Contains(NodeClass))
		{
			++UnpresetOperatorEntries;
		}
	}

	TestEqual(TEXT("no operator-carrying node is offered as a single bundled entry"), UnpresetOperatorEntries, 0);
	TestEqual(TEXT("every entry shares one grouping, so the palette draws no dividers"), Groupings.Num(), 1);

	auto CheckDistinctPresets = [this, &PresetsByClass, &EntriesByClass](UClass* NodeClass, int32 Expected, const TCHAR* Label)
	{
		TestEqual(FString::Printf(TEXT("%s offers one entry per operation"), Label),
			EntriesByClass.FindRef(NodeClass), Expected);
		TestEqual(FString::Printf(TEXT("%s entries each preset a distinct operation"), Label),
			PresetsByClass.FindRef(NodeClass).Num(), Expected);
	};

	CheckDistinctPresets(UCrowdyEffectGraphNode_Attribute::StaticClass(), 2, TEXT("Attribute"));
	CheckDistinctPresets(UCrowdyEffectGraphNode_Constant::StaticClass(), 4, TEXT("Constant"));
	CheckDistinctPresets(UCrowdyEffectGraphNode_BinaryOp::StaticClass(), 5, TEXT("Arithmetic"));
	CheckDistinctPresets(UCrowdyEffectGraphNode_Compare::StaticClass(), 6, TEXT("Compare"));
	CheckDistinctPresets(UCrowdyEffectGraphNode_Logic::StaticClass(), 2, TEXT("Logic"));

	// Not and Negate are the two Unary entries, but they are listed with the logic and the arithmetic groups rather
	// than as one "Unary" entry.
	CheckDistinctPresets(UCrowdyEffectGraphNode_Unary::StaticClass(), 2, TEXT("Unary"));

	// Every builtin is its own entry, named and sized; there is no blank one, because a builtin's name is never typed.
	const int32 BuiltinCount = CrowdyEffectGraphNodeOptions::BuiltinCalls().Num();
	TestEqual(TEXT("every builtin is offered as its own entry"),
		EntriesByClass.FindRef(UCrowdyEffectGraphNode_Call::StaticClass()), BuiltinCount);
	TestEqual(TEXT("each builtin entry presets a distinct function"),
		PresetsByClass.FindRef(UCrowdyEffectGraphNode_Call::StaticClass()).Num(), BuiltinCount);

	// Calling a server function is its own node type, so it is never a mode of the builtin call node.
	TestEqual(TEXT("the server function node is offered once, as its own node type"),
		EntriesByClass.FindRef(UCrowdyEffectGraphNode_ServerCall::StaticClass()), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCrowdyEffectGraphMenuPresetAppliesTest,
	"CrowdySDK.Editor.GraphMenuEntrySpawnsItsOperator",
	CrowdyEffectGraphSchemaTestFlags)

bool FCrowdyEffectGraphMenuPresetAppliesTest::RunTest(const FString&)
{
	UCrowdyEffectGraph* Graph = MakeGraph();

	auto Spawn = [Graph](TSubclassOf<UCrowdyEffectGraphNode> NodeClass, TMap<FName, FString> Presets)
	{
		FCrowdyEffectGraphSchemaAction_NewNode Action(
			FText::GetEmpty(), FText::GetEmpty(), FText::GetEmpty(), NodeClass, FText::GetEmpty(), MoveTemp(Presets));
		return Action.PerformAction(Graph, nullptr, FVector2f(0.f, 0.f), /*bSelectNewNode*/ false);
	};

	const UCrowdyEffectGraphNode_BinaryOp* Subtract = Cast<UCrowdyEffectGraphNode_BinaryOp>(Spawn(
		UCrowdyEffectGraphNode_BinaryOp::StaticClass(),
		{{ GET_MEMBER_NAME_CHECKED(UCrowdyEffectGraphNode_BinaryOp, Op), TEXT("Subtract") }}));
	if (TestNotNull(TEXT("the subtract entry spawns an arithmetic node"), Subtract))
	{
		TestTrue(TEXT("the subtract entry spawns it already set to subtract"),
			Subtract->Op == ECrowdyEffectGraphArithOp::Subtract);
	}

	// A builtin entry presets both the callee and its arity, so the argument pins exist the moment it is placed.
	const UCrowdyEffectGraphNode_Call* Clamp = Cast<UCrowdyEffectGraphNode_Call>(Spawn(
		UCrowdyEffectGraphNode_Call::StaticClass(),
		{{ GET_MEMBER_NAME_CHECKED(UCrowdyEffectGraphNode_Call, Callee), TEXT("clamp") },
		 { GET_MEMBER_NAME_CHECKED(UCrowdyEffectGraphNode_Call, ArgCount), TEXT("3") }}));
	if (TestNotNull(TEXT("the clamp entry spawns a call node"), Clamp))
	{
		TestEqual(TEXT("the clamp entry stores the language's lowercase callee"), Clamp->Callee, FString(TEXT("clamp")));
		TestEqual(TEXT("the clamp entry presets its arity"), Clamp->ArgCount, 3);
		TestNotNull(TEXT("the preset arity allocated the third argument pin"),
			Clamp->FindPin(UCrowdyEffectGraphNode_Call::ArgPinName(2), EGPD_Input));
		TestFalse(TEXT("a builtin call is not a server call"), Clamp->bIsFnCall);
	}

	// The server call is its own node type and carries the fn: flag from its class default, not from a preset.
	const UCrowdyEffectGraphNode_Call* ServerCall = Cast<UCrowdyEffectGraphNode_Call>(Spawn(
		UCrowdyEffectGraphNode_ServerCall::StaticClass(), {}));
	if (TestNotNull(TEXT("the server function entry spawns a server call node"), ServerCall))
	{
		TestTrue(TEXT("a server call node compiles as fn:"), ServerCall->bIsFnCall);
	}

	// Not and Negate carry different pin categories, so the preset must land before the pins are allocated.
	const UCrowdyEffectGraphNode_Unary* Not = Cast<UCrowdyEffectGraphNode_Unary>(Spawn(
		UCrowdyEffectGraphNode_Unary::StaticClass(),
		{{ GET_MEMBER_NAME_CHECKED(UCrowdyEffectGraphNode_Unary, Op), TEXT("Not") }}));
	if (TestNotNull(TEXT("the not entry spawns a unary node"), Not))
	{
		const UEdGraphPin* NotIn = Not->FindPin(UCrowdyEffectGraphNode_Unary::InputPinName(), EGPD_Input);
		if (TestNotNull(TEXT("the not node has its input pin"), NotIn))
		{
			TestEqual(TEXT("the not node's input pin is boolean, so the preset landed before pin allocation"),
				NotIn->PinType.PinCategory, CrowdyEffectGraphPins::BoolCategory);
		}
	}

	const UCrowdyEffectGraphNode_Unary* Negate = Cast<UCrowdyEffectGraphNode_Unary>(Spawn(
		UCrowdyEffectGraphNode_Unary::StaticClass(),
		{{ GET_MEMBER_NAME_CHECKED(UCrowdyEffectGraphNode_Unary, Op), TEXT("Negate") }}));
	if (TestNotNull(TEXT("the negate entry spawns a unary node"), Negate))
	{
		const UEdGraphPin* NegateIn = Negate->FindPin(UCrowdyEffectGraphNode_Unary::InputPinName(), EGPD_Input);
		if (TestNotNull(TEXT("the negate node has its input pin"), NegateIn))
		{
			TestEqual(TEXT("the negate node's input pin is a plain value pin"),
				NegateIn->PinType.PinCategory, CrowdyEffectGraphPins::ValueCategory);
		}
	}

	// An entry with no presets leaves the class defaults alone, and an unknown property name is skipped, not fatal.
	UCrowdyEffectGraphNode_Compare* Compare = NewObject<UCrowdyEffectGraphNode_Compare>(Graph);
	TestEqual(TEXT("an entry with no presets writes nothing"),
		FCrowdyEffectGraphSchemaAction_NewNode::ApplyPresets(*Compare, {}), 0);
	TestEqual(TEXT("an unknown property name is skipped"),
		FCrowdyEffectGraphSchemaAction_NewNode::ApplyPresets(*Compare, {{ TEXT("NotAProperty"), TEXT("x") }}), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCrowdyEffectGraphSchemaRejectsNonNumericIntoArithmeticTest,
	"CrowdySDK.Editor.GraphSchemaRejectsNonNumericIntoArithmetic",
	CrowdyEffectGraphSchemaTestFlags)

bool FCrowdyEffectGraphSchemaRejectsNonNumericIntoArithmeticTest::RunTest(const FString&)
{
	UCrowdyEffectGraph* Graph = MakeGraph();
	UCrowdyEffectGraphNode_BinaryOp* Arith = AddBinaryOp(Graph);

	UCrowdyEffectGraphNode_Constant* Text = AddNodeWithPins<UCrowdyEffectGraphNode_Constant>(Graph);
	Text->ConstantType = ECrowdyEffectGraphConstantType::String;
	Text->Literal = TEXT("dead");

	UCrowdyEffectGraphNode_Constant* Nothing = AddNodeWithPins<UCrowdyEffectGraphNode_Constant>(Graph);
	Nothing->ConstantType = ECrowdyEffectGraphConstantType::Null;

	UEdGraphPin* OperandA = Arith->FindPin(UCrowdyEffectGraphNode_BinaryOp::InputAPinName(), EGPD_Input);
	TestEqual(TEXT("a string constant cannot be an arithmetic operand"),
		Response(Text->FindPin(UCrowdyEffectGraphNode_Constant::OutputPinName(), EGPD_Output), OperandA),
		CONNECT_RESPONSE_DISALLOW);
	TestEqual(TEXT("a null constant cannot be an arithmetic operand"),
		Response(Nothing->FindPin(UCrowdyEffectGraphNode_Constant::OutputPinName(), EGPD_Output), OperandA),
		CONNECT_RESPONSE_DISALLOW);

	// An attribute is Unknown at author time, so it stays allowed: refusing it would block a legitimate graph.
	UCrowdyEffectGraphNode_Attribute* Attribute = AddNodeWithPins<UCrowdyEffectGraphNode_Attribute>(Graph);
	TestEqual(TEXT("an attribute read is still allowed as an arithmetic operand"),
		Response(Attribute->FindPin(UCrowdyEffectGraphNode_Attribute::OutputPinName(), EGPD_Output), OperandA),
		CONNECT_RESPONSE_MAKE);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCrowdyEffectGraphSchemaRejectsNullIntoConditionTest,
	"CrowdySDK.Editor.GraphSchemaRejectsNullIntoCondition",
	CrowdyEffectGraphSchemaTestFlags)

bool FCrowdyEffectGraphSchemaRejectsNullIntoConditionTest::RunTest(const FString&)
{
	UCrowdyEffectGraph* Graph = MakeGraph();
	UCrowdyEffectGraphNode_Constant* Nothing = AddNodeWithPins<UCrowdyEffectGraphNode_Constant>(Graph);
	Nothing->ConstantType = ECrowdyEffectGraphConstantType::Null;
	UCrowdyEffectGraphNode_If* If = AddNodeWithPins<UCrowdyEffectGraphNode_If>(Graph);

	TestEqual(TEXT("a null constant cannot gate a condition"),
		Response(Nothing->FindPin(UCrowdyEffectGraphNode_Constant::OutputPinName(), EGPD_Output),
			If->FindPin(UCrowdyEffectGraphNode_If::ConditionPinName(), EGPD_Input)),
		CONNECT_RESPONSE_DISALLOW);
	TestTrue(TEXT("a null constant is classified as plainly non-boolean"),
		UCrowdyEffectGraphSchema::IsPlainlyNonBooleanProducer(Nothing));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCrowdyEffectGraphSchemaContainerIdNeedsAnIdTest,
	"CrowdySDK.Editor.GraphSchemaContainerIdRejectsNonIdValues",
	CrowdyEffectGraphSchemaTestFlags)

bool FCrowdyEffectGraphSchemaContainerIdNeedsAnIdTest::RunTest(const FString&)
{
	UCrowdyEffectGraph* Graph = MakeGraph();
	UCrowdyEffectGraphNode_ReadRef* ReadRef = AddNodeWithPins<UCrowdyEffectGraphNode_ReadRef>(Graph);
	UEdGraphPin* IdPin = ReadRef->FindPin(UCrowdyEffectGraphNode_ReadRef::IdPinName(), EGPD_Input);

	UCrowdyEffectGraphNode_Constant* Number = AddConstant(Graph, TEXT("42"));
	TestEqual(TEXT("a number is never a container id"),
		Response(Number->FindPin(UCrowdyEffectGraphNode_Constant::OutputPinName(), EGPD_Output), IdPin),
		CONNECT_RESPONSE_DISALLOW);

	UCrowdyEffectGraphNode_Compare* Compare = AddNodeWithPins<UCrowdyEffectGraphNode_Compare>(Graph);
	TestEqual(TEXT("a comparison result is never a container id"),
		Response(Compare->FindPin(UCrowdyEffectGraphNode_Compare::OutputPinName(), EGPD_Output), IdPin),
		CONNECT_RESPONSE_DISALLOW);

	UCrowdyEffectGraphNode_Constant* Text = AddNodeWithPins<UCrowdyEffectGraphNode_Constant>(Graph);
	Text->ConstantType = ECrowdyEffectGraphConstantType::String;
	Text->Literal = TEXT("2f6c...");
	TestEqual(TEXT("a string constant is a valid container id"),
		Response(Text->FindPin(UCrowdyEffectGraphNode_Constant::OutputPinName(), EGPD_Output), IdPin),
		CONNECT_RESPONSE_MAKE);

	// An attribute holding a container_ref is the common case, and it is Unknown at author time.
	UCrowdyEffectGraphNode_Attribute* Attribute = AddNodeWithPins<UCrowdyEffectGraphNode_Attribute>(Graph);
	TestEqual(TEXT("an attribute read is still allowed as a container id"),
		Response(Attribute->FindPin(UCrowdyEffectGraphNode_Attribute::OutputPinName(), EGPD_Output), IdPin),
		CONNECT_RESPONSE_MAKE);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCrowdyEffectGraphConditionPinReadsFromWiringTest,
	"CrowdySDK.Editor.GraphConditionPinDescribesWhatIsWired",
	CrowdyEffectGraphSchemaTestFlags)

bool FCrowdyEffectGraphConditionPinReadsFromWiringTest::RunTest(const FString&)
{
	UCrowdyEffectGraph* Graph = MakeGraph();

	UCrowdyEffectGraphNode_Result* Result = NewObject<UCrowdyEffectGraphNode_Result>(Graph);
	Result->CreateNewGuid();
	Graph->Nodes.Add(Result);
	Result->Requires.Add(FCrowdyEffectGraphRequire());
	Result->AllocateDefaultPins();

	UEdGraphPin* ConditionPin = Result->FindPin(UCrowdyEffectGraphNode_Result::RequirePinName(0), EGPD_Input);
	if (!TestNotNull(TEXT("the condition pin exists"), ConditionPin))
	{
		return false;
	}

	// Nothing wired and no note: the pin reads as a prompt, and there is nothing to describe.
	TestTrue(TEXT("an unwired condition has nothing to describe"), Result->DescribeWiredCondition(0).IsEmpty());

	UCrowdyEffectGraphNode_Compare* Compare = AddNodeWithPins<UCrowdyEffectGraphNode_Compare>(Graph);
	Compare->Comparator = ECrowdyEffectComparator::Greater;
	Compare->FindPin(UCrowdyEffectGraphNode_Compare::OutputPinName(), EGPD_Output)->MakeLinkTo(ConditionPin);

	const FString Described = Result->DescribeWiredCondition(0);
	TestFalse(TEXT("a wired condition describes its driver"), Described.IsEmpty());
	TestTrue(TEXT("the condition pin label names what is wired instead of staying a blank prompt"),
		Result->GetPinDisplayName(ConditionPin).ToString().Contains(Described));

	// An author's own note always wins over the derived description.
	Result->Requires[0].Note = TEXT("target is alive");
	Result->ReconstructNode();
	ConditionPin = Result->FindPin(UCrowdyEffectGraphNode_Result::RequirePinName(0), EGPD_Input);
	if (TestNotNull(TEXT("the condition pin survives the reconstruct"), ConditionPin))
	{
		TestTrue(TEXT("an authored note takes precedence over the derived description"),
			Result->GetPinDisplayName(ConditionPin).ToString().Contains(TEXT("target is alive")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCrowdyEffectGraphDisplayNamesTest,
	"CrowdySDK.Editor.GraphDisplayNamesFollowUnrealCasing",
	CrowdyEffectGraphSchemaTestFlags)

bool FCrowdyEffectGraphDisplayNamesTest::RunTest(const FString&)
{
	// A builtin displays Unreal-style but is stored and emitted in the language's own lowercase spelling.
	TestEqual(TEXT("a builtin displays Unreal-style"),
		CrowdyEffectGraphNodeOptions::BuiltinDisplayName(TEXT("to_string")), FString(TEXT("To String")));
	TestEqual(TEXT("a builtin lookup is case insensitive"),
		CrowdyEffectGraphNodeOptions::BuiltinDisplayName(TEXT("ABS")), FString(TEXT("Abs")));
	TestEqual(TEXT("an authored name reads back verbatim"),
		CrowdyEffectGraphNodeOptions::BuiltinDisplayName(TEXT("compute_bonus")), FString(TEXT("compute_bonus")));
	for (const FCrowdyEffectBuiltinCall& Call : CrowdyEffectGraphNodeOptions::BuiltinCalls())
	{
		TestEqual(FString::Printf(TEXT("'%s' is stored lowercase"), *Call.Callee), Call.Callee, Call.Callee.ToLower());
	}

	TestEqual(TEXT("a bool literal displays Unreal-style"),
		CrowdyEffectGraphNodeOptions::BoolLiteralDisplayName(TEXT("true")), FString(TEXT("True")));
	TestEqual(TEXT("a bool literal displays Unreal-style when false"),
		CrowdyEffectGraphNodeOptions::BoolLiteralDisplayName(TEXT("false")), FString(TEXT("False")));

	UCrowdyEffectGraph* Graph = MakeGraph();

	// The tuning node reads as a plain parameter; the language's "$" sigil is emit syntax, not authoring surface.
	UCrowdyEffectGraphNode_Tuning* Tuning = AddNodeWithPins<UCrowdyEffectGraphNode_Tuning>(Graph);
	TestFalse(TEXT("an unnamed tuning node shows no sigil"),
		Tuning->GetNodeTitle(ENodeTitleType::ListView).ToString().Contains(TEXT("$")));
	Tuning->ParamName = TEXT("damage");
	TestFalse(TEXT("a named tuning node shows no sigil"),
		Tuning->GetNodeTitle(ENodeTitleType::ListView).ToString().Contains(TEXT("$")));

	// An operation node names what it does, not just its symbol pattern.
	UCrowdyEffectGraphNode_BinaryOp* Arith = AddBinaryOp(Graph);
	Arith->Op = ECrowdyEffectGraphArithOp::Subtract;
	TestTrue(TEXT("an arithmetic node names its operation"),
		Arith->GetNodeTitle(ENodeTitleType::ListView).ToString().Contains(TEXT("Subtract")));

	UCrowdyEffectGraphNode_Compare* Compare = AddNodeWithPins<UCrowdyEffectGraphNode_Compare>(Graph);
	Compare->Comparator = ECrowdyEffectComparator::GreaterOrEqual;
	TestTrue(TEXT("a comparison node names its operation"),
		Compare->GetNodeTitle(ENodeTitleType::ListView).ToString().Contains(TEXT("Greater Or Equal")));

	// A server call reads as a server function, not as an "fn:" prefix.
	UCrowdyEffectGraphNode_ServerCall* ServerCall = AddNodeWithPins<UCrowdyEffectGraphNode_ServerCall>(Graph);
	ServerCall->Callee = TEXT("compute_bonus");
	const FString ServerTitle = ServerCall->GetNodeTitle(ENodeTitleType::ListView).ToString();
	TestTrue(TEXT("a server call node reads as a server function"), ServerTitle.Contains(TEXT("Server Function")));
	TestFalse(TEXT("a server call node does not show the fn: prefix"), ServerTitle.Contains(TEXT("fn:")));

	// A builtin call node titles with the Unreal-style label while its callee stays lowercase.
	UCrowdyEffectGraphNode_Call* Call = AddNodeWithPins<UCrowdyEffectGraphNode_Call>(Graph);
	Call->Callee = TEXT("to_string");
	TestEqual(TEXT("a builtin call node titles with the Unreal-style label"),
		Call->GetNodeTitle(ENodeTitleType::ListView).ToString(), FString(TEXT("To String")));
	TestEqual(TEXT("the stored callee is untouched by the label"), Call->Callee, FString(TEXT("to_string")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCrowdyEffectGraphBuiltinArityTest,
	"CrowdySDK.Editor.GraphBuiltinArityIsEnforced",
	CrowdyEffectGraphSchemaTestFlags)

bool FCrowdyEffectGraphBuiltinArityTest::RunTest(const FString&)
{
	// A fixed-arity builtin accepts exactly one count, whatever is asked for.
	TestEqual(TEXT("'not' takes one operand however many are asked for"),
		CrowdyEffectGraphNodeOptions::ClampArgCount(TEXT("not"), 3), 1);
	TestEqual(TEXT("'pow' takes two operands"),
		CrowdyEffectGraphNodeOptions::ClampArgCount(TEXT("pow"), 5), 2);
	TestEqual(TEXT("'rand' takes none"),
		CrowdyEffectGraphNodeOptions::ClampArgCount(TEXT("rand"), 2), 0);

	// A variadic builtin keeps a count the author chooses, but never below its minimum.
	TestEqual(TEXT("'concat' keeps an author-chosen count"),
		CrowdyEffectGraphNodeOptions::ClampArgCount(TEXT("concat"), 4), 4);
	TestEqual(TEXT("'concat' never drops below two"),
		CrowdyEffectGraphNodeOptions::ClampArgCount(TEXT("concat"), 1), 2);

	// A server function's arity lives on the server, so the editor must not constrain it.
	TestEqual(TEXT("an authored function's count passes through"),
		CrowdyEffectGraphNodeOptions::ClampArgCount(TEXT("compute_bonus"), 7), 7);

	UCrowdyEffectGraph* Graph = MakeGraph();

	// A node carrying an over-wide count (a hand edit, or an asset authored before the constraint) is corrected when
	// its pins are allocated, so it can never emit a call the server would reject.
	UCrowdyEffectGraphNode_Call* Not = NewObject<UCrowdyEffectGraphNode_Call>(Graph);
	Not->CreateNewGuid();
	Graph->Nodes.Add(Not);
	Not->Callee = TEXT("not");
	Not->ArgCount = 4;
	Not->AllocateDefaultPins();
	TestEqual(TEXT("an over-wide builtin count is corrected at pin allocation"), Not->ArgCount, 1);
	TestNotNull(TEXT("the single operand pin exists"),
		Not->FindPin(UCrowdyEffectGraphNode_Call::ArgPinName(0), EGPD_Input));
	TestNull(TEXT("no second operand pin is created"),
		Not->FindPin(UCrowdyEffectGraphNode_Call::ArgPinName(1), EGPD_Input));

	// A server call is left alone even when its name happens to match a builtin.
	UCrowdyEffectGraphNode_ServerCall* Server = NewObject<UCrowdyEffectGraphNode_ServerCall>(Graph);
	Server->CreateNewGuid();
	Graph->Nodes.Add(Server);
	Server->Callee = TEXT("not");
	Server->ArgCount = 4;
	Server->AllocateDefaultPins();
	TestEqual(TEXT("a server function's count is never constrained by a same-named builtin"), Server->ArgCount, 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCrowdyEffectGraphMenuCategoriesTest,
	"CrowdySDK.Editor.GraphMenuGroupsEntriesIntoCategories",
	CrowdyEffectGraphSchemaTestFlags)

bool FCrowdyEffectGraphMenuCategoriesTest::RunTest(const FString&)
{
	UCrowdyEffectGraph* Graph = MakeGraph();

	FGraphContextMenuBuilder MenuBuilder(Graph);
	GetDefault<UCrowdyEffectGraphSchema>()->GetGraphContextActions(MenuBuilder);

	TSet<FString> Categories;
	TSet<int32> Groupings;
	int32 ServerCallEntries = 0;
	int32 BlankCallEntries = 0;

	for (int32 Index = 0; Index < MenuBuilder.GetNumActions(); ++Index)
	{
		const TSharedPtr<FEdGraphSchemaAction> Action = MenuBuilder.GetSchemaAction(Index);
		if (!Action.IsValid() || Action->GetTypeId() != FCrowdyEffectGraphSchemaAction_NewNode::StaticGetTypeId())
		{
			continue;
		}
		const FCrowdyEffectGraphSchemaAction_NewNode& NewNode =
			static_cast<const FCrowdyEffectGraphSchemaAction_NewNode&>(*Action);

		Categories.Add(NewNode.GetCategory().ToString());
		Groupings.Add(NewNode.GetGrouping());

		if (NewNode.NodeClass == UCrowdyEffectGraphNode_ServerCall::StaticClass())
		{
			++ServerCallEntries;
		}
		else if (NewNode.NodeClass == UCrowdyEffectGraphNode_Call::StaticClass() && NewNode.Presets.IsEmpty())
		{
			++BlankCallEntries;
		}
	}

	// Categories organize the palette into sections; grouping stays uniform, since that is what draws divider bars.
	for (const TCHAR* Expected : { TEXT("Attributes"), TEXT("Values"), TEXT("Arithmetic"), TEXT("Comparison"),
		TEXT("Logic"), TEXT("Functions"), TEXT("Server Functions"), TEXT("Flow") })
	{
		TestTrue(FString::Printf(TEXT("the '%s' category is offered"), Expected), Categories.Contains(Expected));
	}
	TestEqual(TEXT("every entry still shares one grouping, so no dividers are drawn"), Groupings.Num(), 1);

	// Every builtin has its own entry, so there is no blank call node to type a builtin name into; a typed name always
	// means a server function.
	TestEqual(TEXT("no blank builtin call entry is offered"), BlankCallEntries, 0);
	TestEqual(TEXT("the server function node is offered once"), ServerCallEntries, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCrowdyEffectGraphNodeColorsTest,
	"CrowdySDK.Editor.GraphNodeFamiliesAreDistinctlyColored",
	CrowdyEffectGraphSchemaTestFlags)

bool FCrowdyEffectGraphNodeColorsTest::RunTest(const FString&)
{
	// Every family must be its own colour, or two node kinds read as the same thing on the canvas.
	const TArray<ECrowdyEffectGraphNodeFamily> Families = {
		ECrowdyEffectGraphNodeFamily::Attribute, ECrowdyEffectGraphNodeFamily::Value,
		ECrowdyEffectGraphNodeFamily::Arithmetic, ECrowdyEffectGraphNodeFamily::Comparison,
		ECrowdyEffectGraphNodeFamily::Logic, ECrowdyEffectGraphNodeFamily::Function,
		ECrowdyEffectGraphNodeFamily::ServerFunction, ECrowdyEffectGraphNodeFamily::Flow,
		ECrowdyEffectGraphNodeFamily::Result
	};

	TArray<FLinearColor> Seen;
	for (const ECrowdyEffectGraphNodeFamily Family : Families)
	{
		const FLinearColor Color = UCrowdyEffectGraphNode::FamilyColor(Family);
		for (const FLinearColor& Other : Seen)
		{
			TestFalse(TEXT("no two families share a colour"), Other.Equals(Color, 0.02f));
		}
		Seen.Add(Color);
	}

	UCrowdyEffectGraph* Graph = MakeGraph();

	// A node's colour follows its family, and the family follows the palette section it came from.
	TestTrue(TEXT("an attribute read draws as an Attribute"),
		AddNodeWithPins<UCrowdyEffectGraphNode_Attribute>(Graph)->GetNodeFamily()
			== ECrowdyEffectGraphNodeFamily::Attribute);
	TestTrue(TEXT("a ref read draws as an Attribute"),
		AddNodeWithPins<UCrowdyEffectGraphNode_ReadRef>(Graph)->GetNodeFamily()
			== ECrowdyEffectGraphNodeFamily::Attribute);
	TestTrue(TEXT("a comparison draws as a Comparison"),
		AddNodeWithPins<UCrowdyEffectGraphNode_Compare>(Graph)->GetNodeFamily()
			== ECrowdyEffectGraphNodeFamily::Comparison);
	TestTrue(TEXT("a builtin call draws as a Function"),
		AddNodeWithPins<UCrowdyEffectGraphNode_Call>(Graph)->GetNodeFamily()
			== ECrowdyEffectGraphNodeFamily::Function);
	TestTrue(TEXT("a server call draws as a Server Function"),
		AddNodeWithPins<UCrowdyEffectGraphNode_ServerCall>(Graph)->GetNodeFamily()
			== ECrowdyEffectGraphNodeFamily::ServerFunction);

	// The unary node is the one that changes family with its operator, matching where each sits in the palette.
	TestTrue(TEXT("a logical not draws as Logic"),
		AddUnary(Graph, ECrowdyEffectGraphUnaryOp::Not)->GetNodeFamily() == ECrowdyEffectGraphNodeFamily::Logic);
	TestTrue(TEXT("an arithmetic negate draws as Arithmetic"),
		AddUnary(Graph, ECrowdyEffectGraphUnaryOp::Negate)->GetNodeFamily()
			== ECrowdyEffectGraphNodeFamily::Arithmetic);

	// A server call must not inherit the builtin call's colour just because it derives from it.
	UCrowdyEffectGraphNode_Call* Builtin = AddNodeWithPins<UCrowdyEffectGraphNode_Call>(Graph);
	UCrowdyEffectGraphNode_ServerCall* Server = AddNodeWithPins<UCrowdyEffectGraphNode_ServerCall>(Graph);
	TestFalse(TEXT("a server call is coloured apart from a builtin call"),
		Builtin->GetNodeTitleColor().Equals(Server->GetNodeTitleColor(), 0.02f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCrowdyEffectGraphGridBuiltinsTest,
	"CrowdySDK.Editor.GraphOffersGridPermissionBuiltins",
	CrowdyEffectGraphSchemaTestFlags)

bool FCrowdyEffectGraphGridBuiltinsTest::RunTest(const FString&)
{
	// The six database-backed reads the expression language documents. Their arities differ from the pure maths, and
	// getting one wrong authors a call the server rejects, so each is pinned here.
	struct FExpected
	{
		const TCHAR* Callee;
		int32 MinArgs;
		int32 MaxArgs;
	};
	const FExpected Expected[] = {
		{ TEXT("has_grid_permission"),  2, 3 },
		{ TEXT("has_chunk_permission"), 5, 6 },
		{ TEXT("grid_at"),              3, 4 },
		{ TEXT("grid_contains"),        4, 4 },
		{ TEXT("grid_min"),             2, 2 },
		{ TEXT("grid_max"),             2, 2 }
	};

	for (const FExpected& Want : Expected)
	{
		const FCrowdyEffectBuiltinCall* Builtin = CrowdyEffectGraphNodeOptions::FindBuiltin(Want.Callee);
		if (!TestNotNull(*FString::Printf(TEXT("'%s' is offered as a builtin"), Want.Callee), Builtin))
		{
			continue;
		}
		TestEqual(FString::Printf(TEXT("'%s' minimum arity"), Want.Callee), Builtin->MinArgs, Want.MinArgs);
		TestEqual(FString::Printf(TEXT("'%s' maximum arity"), Want.Callee), Builtin->MaxArgs, Want.MaxArgs);

		// They list apart from the pure maths, since they hit the database and are metered.
		TestEqual(FString::Printf(TEXT("'%s' lists under its own palette section"), Want.Callee),
			Builtin->PaletteCategory, FString(TEXT("Grid & Permissions")));
	}

	// A permission read takes the caller's id, which the server injects, so it must not read as an undeclared
	// parameter the author forgot to expose.
	TestEqual(TEXT("an optional grid id is accepted"),
		CrowdyEffectGraphNodeOptions::ClampArgCount(TEXT("has_grid_permission"), 3), 3);
	TestEqual(TEXT("a fourth argument is refused"),
		CrowdyEffectGraphNodeOptions::ClampArgCount(TEXT("has_grid_permission"), 4), 3);
	TestEqual(TEXT("dropping the permission key is refused"),
		CrowdyEffectGraphNodeOptions::ClampArgCount(TEXT("has_grid_permission"), 1), 2);

	// Every builtin names its arguments, or its pins read as Arg_0 / Arg_1 and say nothing.
	for (const FCrowdyEffectBuiltinCall& Builtin : CrowdyEffectGraphNodeOptions::BuiltinCalls())
	{
		TestTrue(FString::Printf(TEXT("'%s' names every required argument"), *Builtin.Callee),
			Builtin.ArgNames.Num() >= Builtin.MinArgs);
		TestFalse(FString::Printf(TEXT("'%s' says what it does"), *Builtin.Callee), Builtin.Description.IsEmpty());

		// The grid family is the one that needs more than a definition: it is only meaningful in a world that uses
		// grids, and it costs a metered lookup, so each entry has to say when reaching for it is the right move. The
		// pure maths (Sqrt, Abs) are self-explanatory and are held to no such bar.
		if (Builtin.PaletteCategory == TEXT("Grid & Permissions"))
		{
			TestTrue(FString::Printf(TEXT("'%s' explains when to use it"), *Builtin.Callee),
				Builtin.Description.Contains(TEXT("Use it")));
			// Every one of the six is database-backed and metered, so every one has to say so. An author choosing
			// between a cheap attribute read and one of these should be able to see the difference.
			TestTrue(FString::Printf(TEXT("'%s' warns that it is metered"), *Builtin.Callee),
				Builtin.Description.Contains(TEXT("metered")));
		}
	}

	// An optional argument says so, and a variadic builtin's extras still get a readable label.
	TestEqual(TEXT("a required argument reads plainly"),
		CrowdyEffectGraphNodeOptions::ArgDisplayName(TEXT("has_chunk_permission"), 1),
		FString(TEXT("Permission Key")));
	TestEqual(TEXT("an optional argument is marked optional"),
		CrowdyEffectGraphNodeOptions::ArgDisplayName(TEXT("has_chunk_permission"), 5),
		FString(TEXT("Overlap Mode (optional)")));
	TestEqual(TEXT("a variadic extra falls back to a numbered label"),
		CrowdyEffectGraphNodeOptions::ArgDisplayName(TEXT("concat"), 4), FString(TEXT("Value 5 (optional)")));
	TestTrue(TEXT("a server function's arguments are left unlabelled"),
		CrowdyEffectGraphNodeOptions::ArgDisplayName(TEXT("compute_bonus"), 0).IsEmpty());

	// A placed node explains itself, rather than only its palette entry doing so.
	UCrowdyEffectGraph* Graph = MakeGraph();
	UCrowdyEffectGraphNode_Call* GridAt = NewObject<UCrowdyEffectGraphNode_Call>(Graph);
	GridAt->CreateNewGuid();
	Graph->Nodes.Add(GridAt);
	GridAt->Callee = TEXT("grid_at");
	GridAt->ArgCount = 4;
	GridAt->AllocateDefaultPins();

	TestTrue(TEXT("a placed grid node's tooltip explains when to use it"),
		GridAt->GetTooltipText().ToString().Contains(TEXT("Use it when")));

	const UEdGraphPin* ModePin = GridAt->FindPin(UCrowdyEffectGraphNode_Call::ArgPinName(3), EGPD_Input);
	if (TestNotNull(TEXT("the optional mode pin exists"), ModePin))
	{
		TestEqual(TEXT("the mode pin is labelled, not Arg_3"),
			ModePin->PinFriendlyName.ToString(), FString(TEXT("Overlap Mode (optional)")));
	}
	return true;
}

// The six list builtins, each pinned by name so a future edit to one entry's arity is caught here rather than only
// showing up as a drifted server call. array is the only variadic one of the six (0 to 16, matching how a fresh list
// author starts with no elements); the rest are fixed-arity.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCrowdyEffectGraphListBuiltinsArityTest,
	"CrowdySDK.Editor.GraphOffersListBuiltins",
	CrowdyEffectGraphSchemaTestFlags)

bool FCrowdyEffectGraphListBuiltinsArityTest::RunTest(const FString&)
{
	struct FExpected
	{
		const TCHAR* Callee;
		int32 MinArgs;
		int32 MaxArgs;
	};
	const FExpected Expected[] = {
		{ TEXT("at"),        2, 2 },
		{ TEXT("set_at"),    3, 3 },
		{ TEXT("append"),    2, 2 },
		{ TEXT("remove_at"), 2, 2 },
		{ TEXT("index_of"),  2, 2 },
		{ TEXT("array"),     0, 16 }
	};

	for (const FExpected& Want : Expected)
	{
		const FCrowdyEffectBuiltinCall* Builtin = CrowdyEffectGraphNodeOptions::FindBuiltin(Want.Callee);
		if (!TestNotNull(*FString::Printf(TEXT("'%s' is offered as a builtin"), Want.Callee), Builtin))
		{
			continue;
		}
		TestEqual(FString::Printf(TEXT("'%s' minimum arity"), Want.Callee), Builtin->MinArgs, Want.MinArgs);
		TestEqual(FString::Printf(TEXT("'%s' maximum arity"), Want.Callee), Builtin->MaxArgs, Want.MaxArgs);
	}
	return true;
}

// The connection classifier's answers for a list-producing value: refused wherever the language cannot use a whole
// list (arithmetic, a condition), accepted wherever a pin merely needs Any (another list builtin's argument), and
// Unknown is unaffected. The Unknown case is a regression pin, not a new assertion: an attribute read is Unknown at
// author time, and if that stopped being accepted into arithmetic it would break every existing graph that reads an
// attribute into a sum or a comparison.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCrowdyEffectGraphArrayConnectionClassifierTest,
	"CrowdySDK.Editor.GraphSchemaClassifiesArrayProducers",
	CrowdyEffectGraphSchemaTestFlags)

bool FCrowdyEffectGraphArrayConnectionClassifierTest::RunTest(const FString&)
{
	UCrowdyEffectGraph* Graph = MakeGraph();

	// Built by hand rather than through AddNodeWithPins, which allocates pins immediately off the class defaults:
	// setting Callee/ArgCount first and allocating once matches how the palette and the compiler actually place a
	// Call node, and avoids allocating the pins twice.
	UCrowdyEffectGraphNode_Call* ArrayNode = NewObject<UCrowdyEffectGraphNode_Call>(Graph);
	ArrayNode->CreateNewGuid();
	Graph->Nodes.Add(ArrayNode);
	ArrayNode->Callee = TEXT("array");
	ArrayNode->ArgCount = 0;
	ArrayNode->AllocateDefaultPins();
	const UEdGraphPin* ArrayOut = ArrayNode->FindPin(UCrowdyEffectGraphNode_Call::OutputPinName(), EGPD_Output);

	TestTrue(TEXT("a list-producing call classifies as Array"),
		UCrowdyEffectGraphSchema::ClassifyProducer(ArrayNode) == ECrowdyEffectGraphValueKind::Array);

	UCrowdyEffectGraphNode_BinaryOp* Arith = AddBinaryOp(Graph);
	UEdGraphPin* ArithIn = Arith->FindPin(UCrowdyEffectGraphNode_BinaryOp::InputAPinName(), EGPD_Input);
	TestEqual(TEXT("a list cannot be an arithmetic operand"),
		Response(ArrayOut, ArithIn), CONNECT_RESPONSE_DISALLOW);

	UCrowdyEffectGraphNode_If* If = AddNodeWithPins<UCrowdyEffectGraphNode_If>(Graph);
	TestEqual(TEXT("a list cannot gate a condition"),
		Response(ArrayOut, If->FindPin(UCrowdyEffectGraphNode_If::ConditionPinName(), EGPD_Input)),
		CONNECT_RESPONSE_DISALLOW);

	UCrowdyEffectGraphNode_Call* Append = NewObject<UCrowdyEffectGraphNode_Call>(Graph);
	Append->CreateNewGuid();
	Graph->Nodes.Add(Append);
	Append->Callee = TEXT("append");
	Append->ArgCount = 2;
	Append->AllocateDefaultPins();
	UEdGraphPin* AppendListArg = Append->FindPin(UCrowdyEffectGraphNode_Call::ArgPinName(0), EGPD_Input);
	TestEqual(TEXT("a list feeds a list-builtin's argument"),
		Response(ArrayOut, AppendListArg), CONNECT_RESPONSE_MAKE);

	// Regression pin: an attribute read is Unknown at author time, and Unknown must still be accepted into
	// arithmetic, exactly as it was before the Array kind existed.
	UCrowdyEffectGraphNode_Attribute* Attribute = AddNodeWithPins<UCrowdyEffectGraphNode_Attribute>(Graph);
	TestTrue(TEXT("an attribute read still classifies as Unknown"),
		UCrowdyEffectGraphSchema::ClassifyProducer(Attribute) == ECrowdyEffectGraphValueKind::Unknown);
	TestEqual(TEXT("Unknown still connects into arithmetic"),
		Response(Attribute->FindPin(UCrowdyEffectGraphNode_Attribute::OutputPinName(), EGPD_Output), ArithIn),
		CONNECT_RESPONSE_MAKE);

	return true;
}

// Parity goldens for the third return-authoring surface (the graph), proving the graph, the equivalent EffectScript
// text, and the concrete lowered values (ReturnExpression / ReturnType) all agree. A golden that only compares the
// two surfaces to each other cannot tell you both are wrong at once, so each test below also pins the literal
// lowered strings by hand.
namespace
{
	// The same vocabulary the compiler parity tests use: a clamped int hp, a plain int armor, and an int attack the
	// source is read through, plus the tuning magnitudes referenced below. Re-declared locally rather than editing
	// the compiler tests file, which owns that helper.
	FCrowdyEffectLoweringContext MakeReturnParityContext()
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

		for (const TCHAR* MagName : { TEXT("power") })
		{
			FCrowdyEffectParamDecl Param;
			Param.Name = MagName;
			Param.ValueType = TEXT("int");
			Ctx.Magnitudes.Add(Param);
		}

		// A carrier has to be selected for "a question authors no notification" to mean anything: with no carrier
		// the notification block never runs at all, so the assertion would hold no matter what the rule was.
		Ctx.NotificationCarrier = ECrowdyModelNotificationCarrier::Channel;

		return Ctx;
	}

	template <typename TNode>
	TNode* AddReturnParityNode(UCrowdyEffectGraph* Graph)
	{
		TNode* Node = NewObject<TNode>(Graph);
		Node->CreateNewGuid();
		Graph->Nodes.Add(Node);
		return Node;
	}

	void LinkReturnParity(UEdGraphNode* FromNode, FName FromPinName, UEdGraphNode* ToNode, FName ToPinName)
	{
		UEdGraphPin* Out = FromNode ? FromNode->FindPin(FromPinName, EGPD_Output) : nullptr;
		UEdGraphPin* In = ToNode ? ToNode->FindPin(ToPinName, EGPD_Input) : nullptr;
		if (Out && In)
		{
			Out->MakeLinkTo(In);
		}
	}

	UCrowdyEffectGraphNode_Attribute* MakeReturnParityAttribute(
		UCrowdyEffectGraph* Graph, ECrowdyEffectRole Role, const TCHAR* Attr)
	{
		UCrowdyEffectGraphNode_Attribute* Node = AddReturnParityNode<UCrowdyEffectGraphNode_Attribute>(Graph);
		Node->Role = Role;
		Node->Attribute = Attr;
		Node->AllocateDefaultPins();
		return Node;
	}

	UCrowdyEffectGraphNode_Tuning* MakeReturnParityTuning(UCrowdyEffectGraph* Graph, const TCHAR* Param)
	{
		UCrowdyEffectGraphNode_Tuning* Node = AddReturnParityNode<UCrowdyEffectGraphNode_Tuning>(Graph);
		Node->ParamName = Param;
		Node->AllocateDefaultPins();
		return Node;
	}

	UCrowdyEffectGraphNode_Constant* MakeReturnParityConstant(
		UCrowdyEffectGraph* Graph, ECrowdyEffectGraphConstantType Type, const TCHAR* Literal)
	{
		UCrowdyEffectGraphNode_Constant* Node = AddReturnParityNode<UCrowdyEffectGraphNode_Constant>(Graph);
		Node->ConstantType = Type;
		Node->Literal = Literal;
		Node->AllocateDefaultPins();
		return Node;
	}

	// Byte-identical parity: used only where neither surface emits a location-derived diagnostic, since a
	// graph-synthesized expression and its re-parsed text fragment do not share source positions (both emit the
	// same fixed-position "magnitude declared but unused" warnings identically, which is why the other parity
	// tests in this file already tolerate this comparison).
	void CheckReturnParityExact(FAutomationTestBase& Test, UCrowdyEffectGraph* Graph, const TCHAR* EffectText)
	{
		const FCrowdyEffectLoweringContext Ctx = MakeReturnParityContext();

		const FString FromGraph = CrowdyStringifyResult(FCrowdyEffectGraphCompiler::CompileToFunction(Graph, Ctx));

		const FCrowdyEffectParseResult Parsed = FCrowdyEffectParser::Parse(EffectText);
		Test.TestFalse(TEXT("the equivalent text parses without error"), Parsed.HasErrors());
		const FString FromText = CrowdyStringifyResult(FCrowdyEffectLowering::Lower(Parsed.Program, Ctx));

		Test.TestEqual(TEXT("the graph and the equivalent text lower identically"), FromGraph, FromText);
	}

	// Every diagnostic as severity and message only. A graph has no source text, so its expressions reach the
	// lowering as fragments parsed on their own, and a position inside a fragment counts from the fragment rather
	// than from a line that does not exist. The position is the only part that cannot be compared across the two
	// surfaces, so it is the only part dropped.
	FString ReturnParityDiagnosticsWithoutPositions(const TArray<FCrowdyEffectDiagnostic>& Diagnostics)
	{
		FString S;
		for (const FCrowdyEffectDiagnostic& D : Diagnostics)
		{
			S += FString::Printf(TEXT("<%s|%s>"),
				D.Severity == ECrowdyEffectSeverity::Error ? TEXT("error") : TEXT("warning"), *D.Message);
		}
		return S;
	}

	// Parity for a return whose diagnostics carry a source position: the lowered function is compared byte for
	// byte, and the diagnostics are compared on which ones fire and what they say, but not on where they point.
	void CheckReturnParityFunctionOnly(FAutomationTestBase& Test, UCrowdyEffectGraph* Graph, const TCHAR* EffectText)
	{
		const FCrowdyEffectLoweringContext Ctx = MakeReturnParityContext();

		const FCrowdyEffectLoweringResult GraphResult = FCrowdyEffectGraphCompiler::CompileToFunction(Graph, Ctx);
		const FString FromGraph = CrowdyStringifyFunction(GraphResult.Function);

		const FCrowdyEffectParseResult Parsed = FCrowdyEffectParser::Parse(EffectText);
		Test.TestFalse(TEXT("the equivalent text parses without error"), Parsed.HasErrors());
		const FCrowdyEffectLoweringResult TextResult = FCrowdyEffectLowering::Lower(Parsed.Program, Ctx);
		const FString FromText = CrowdyStringifyFunction(TextResult.Function);

		Test.TestEqual(TEXT("the graph and the equivalent text lower to the identical function input"), FromGraph, FromText);
		Test.TestEqual(TEXT("and say the same things to the author"),
			ReturnParityDiagnosticsWithoutPositions(GraphResult.Diagnostics),
			ReturnParityDiagnosticsWithoutPositions(TextResult.Diagnostics));

		Test.TestTrue(TEXT("the graph path warns that the return type is undeclared"),
			GraphResult.Diagnostics.ContainsByPredicate([](const FCrowdyEffectDiagnostic& D)
			{
				return D.Severity == ECrowdyEffectSeverity::Warning && D.Message.Contains(TEXT("declares no return type"));
			}));
	}
}

// The house style: a write and a return of the SAME attribute the write just changed. Every shipped kit function
// that declares a return follows this shape (attack mutates hp then returns it, grant adds to quantity then
// returns it), so this is the return the graph has to get right first.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectGraphReturnParityWriteThenReturnTest,
	"CrowdySDK.Editor.GraphReturnParityWriteThenReturn", CrowdyEffectGraphSchemaTestFlags)
bool FCrowdyEffectGraphReturnParityWriteThenReturnTest::RunTest(const FString&)
{
	UCrowdyEffectGraph* Graph = NewObject<UCrowdyEffectGraph>(GetTransientPackage());

	UCrowdyEffectGraphNode_Tuning* Power = MakeReturnParityTuning(Graph, TEXT("power"));
	UCrowdyEffectGraphNode_Attribute* HpReturn =
		MakeReturnParityAttribute(Graph, ECrowdyEffectRole::Target, TEXT("hp"));

	UCrowdyEffectGraphNode_Result* Result = AddReturnParityNode<UCrowdyEffectGraphNode_Result>(Graph);
	FCrowdyEffectGraphWrite Write;
	Write.TargetRole = ECrowdyEffectRole::Target;
	Write.Attribute = TEXT("hp");
	Write.Op = ECrowdyEffectAssignmentOp::Subtract;
	Result->Writes.Add(Write);
	Result->bReturnsValue = true;
	Result->AllocateDefaultPins();

	LinkReturnParity(Power, UCrowdyEffectGraphNode_Tuning::OutputPinName(), Result, UCrowdyEffectGraphNode_Result::WritePinName(0));
	LinkReturnParity(HpReturn, UCrowdyEffectGraphNode_Attribute::OutputPinName(), Result, UCrowdyEffectGraphNode_Result::ReturnPinName());

	const FCrowdyEffectLoweringContext Ctx = MakeReturnParityContext();
	const FCrowdyEffectLoweringResult GraphResult = FCrowdyEffectGraphCompiler::CompileToFunction(Graph, Ctx);

	TestFalse(TEXT("the graph compiles without error"), GraphResult.HasErrors());
	TestEqual(TEXT("the return reads exactly self.hp"), GraphResult.Function.ReturnExpression, FString(TEXT("self.hp")));
	TestEqual(TEXT("the return type is inferred from the returned attribute"), GraphResult.Function.ReturnType, FString(TEXT("int")));
	TestEqual(TEXT("one mutation is still produced"), GraphResult.Function.Mutations.Num(), 1);

	// The other side of the rule the query test pins: an effect that does change something still tells peers so.
	TestEqual(TEXT("a write still authors one model-changed notification"),
		GraphResult.Function.Notifications.Num(), 1);

	CheckReturnParityExact(*this, Graph, TEXT("self.hp -= $power\nreturn self.hp"));
	return true;
}

// The headline case this feature unlocks: a zero-mutation query. No writes, no conditions, only a return of a
// comparison ("can I afford this" in miniature). Unauthorable before this feature existed, since an effect with no
// mutations returned nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectGraphReturnParityZeroMutationQueryTest,
	"CrowdySDK.Editor.GraphReturnParityZeroMutationQuery", CrowdyEffectGraphSchemaTestFlags)
bool FCrowdyEffectGraphReturnParityZeroMutationQueryTest::RunTest(const FString&)
{
	UCrowdyEffectGraph* Graph = NewObject<UCrowdyEffectGraph>(GetTransientPackage());

	UCrowdyEffectGraphNode_Attribute* Armor =
		MakeReturnParityAttribute(Graph, ECrowdyEffectRole::Target, TEXT("armor"));
	UCrowdyEffectGraphNode_Tuning* Power = MakeReturnParityTuning(Graph, TEXT("power"));

	UCrowdyEffectGraphNode_Compare* Cmp = AddReturnParityNode<UCrowdyEffectGraphNode_Compare>(Graph);
	Cmp->Comparator = ECrowdyEffectComparator::GreaterOrEqual;
	Cmp->AllocateDefaultPins();

	UCrowdyEffectGraphNode_Result* Result = AddReturnParityNode<UCrowdyEffectGraphNode_Result>(Graph);
	Result->bReturnsValue = true;
	Result->AllocateDefaultPins();

	LinkReturnParity(Armor, UCrowdyEffectGraphNode_Attribute::OutputPinName(), Cmp, UCrowdyEffectGraphNode_Compare::InputAPinName());
	LinkReturnParity(Power, UCrowdyEffectGraphNode_Tuning::OutputPinName(), Cmp, UCrowdyEffectGraphNode_Compare::InputBPinName());
	LinkReturnParity(Cmp, UCrowdyEffectGraphNode_Compare::OutputPinName(), Result, UCrowdyEffectGraphNode_Result::ReturnPinName());

	const FCrowdyEffectLoweringContext Ctx = MakeReturnParityContext();
	const FCrowdyEffectLoweringResult GraphResult = FCrowdyEffectGraphCompiler::CompileToFunction(Graph, Ctx);

	TestFalse(TEXT("the graph compiles without error"), GraphResult.HasErrors());
	TestEqual(TEXT("no mutation is produced"), GraphResult.Function.Mutations.Num(), 0);
	TestEqual(TEXT("the return reads exactly the comparison, unparenthesized at the top level"),
		GraphResult.Function.ReturnExpression, FString(TEXT("self.armor >= $power")));
	TestTrue(TEXT("a comparison's type is not inferable, so the return type stays undeclared"),
		GraphResult.Function.ReturnType.IsEmpty());
	TestTrue(TEXT("a query with no mutations authors no model-changed notification"),
		GraphResult.Function.Notifications.IsEmpty());

	TArray<FCrowdyEffectDiagnostic> SpecDiagnostics;
	const FCrowdyEffectSpec Spec = FCrowdyEffectGraphCompiler::CompileToSpec(Graph, SpecDiagnostics);
	TestEqual(TEXT("no writes are emitted"), Spec.Assignments.Num(), 0);
	TestEqual(TEXT("no requires are emitted"), Spec.Requires.Num(), 0);
	TestTrue(TEXT("the spec carries a return"), Spec.bHasReturn);

	CheckReturnParityFunctionOnly(*this, Graph, TEXT("return self.armor >= $power"));
	return true;
}

// A compound return: arithmetic nested inside a builtin call, so both precedence (the multiply must keep the
// subtraction parenthesized) and call-argument formatting have to survive the round trip through the emitted text.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectGraphReturnParityCompoundExpressionTest,
	"CrowdySDK.Editor.GraphReturnParityCompoundExpression", CrowdyEffectGraphSchemaTestFlags)
bool FCrowdyEffectGraphReturnParityCompoundExpressionTest::RunTest(const FString&)
{
	UCrowdyEffectGraph* Graph = NewObject<UCrowdyEffectGraph>(GetTransientPackage());

	UCrowdyEffectGraphNode_Constant* Two =
		MakeReturnParityConstant(Graph, ECrowdyEffectGraphConstantType::Number, TEXT("2"));
	UCrowdyEffectGraphNode_Attribute* Hp = MakeReturnParityAttribute(Graph, ECrowdyEffectRole::Target, TEXT("hp"));
	UCrowdyEffectGraphNode_Tuning* Power = MakeReturnParityTuning(Graph, TEXT("power"));

	UCrowdyEffectGraphNode_BinaryOp* Sub = AddReturnParityNode<UCrowdyEffectGraphNode_BinaryOp>(Graph);
	Sub->Op = ECrowdyEffectGraphArithOp::Subtract;
	Sub->AllocateDefaultPins();

	UCrowdyEffectGraphNode_BinaryOp* Mul = AddReturnParityNode<UCrowdyEffectGraphNode_BinaryOp>(Graph);
	Mul->Op = ECrowdyEffectGraphArithOp::Multiply;
	Mul->AllocateDefaultPins();

	UCrowdyEffectGraphNode_Constant* Zero =
		MakeReturnParityConstant(Graph, ECrowdyEffectGraphConstantType::Number, TEXT("0"));
	UCrowdyEffectGraphNode_Constant* Hundred =
		MakeReturnParityConstant(Graph, ECrowdyEffectGraphConstantType::Number, TEXT("100"));

	UCrowdyEffectGraphNode_Call* Clamp = AddReturnParityNode<UCrowdyEffectGraphNode_Call>(Graph);
	Clamp->Callee = TEXT("clamp");
	Clamp->ArgCount = 3;
	Clamp->AllocateDefaultPins();

	UCrowdyEffectGraphNode_Result* Result = AddReturnParityNode<UCrowdyEffectGraphNode_Result>(Graph);
	Result->bReturnsValue = true;
	Result->AllocateDefaultPins();

	LinkReturnParity(Hp, UCrowdyEffectGraphNode_Attribute::OutputPinName(), Sub, UCrowdyEffectGraphNode_BinaryOp::InputAPinName());
	LinkReturnParity(Power, UCrowdyEffectGraphNode_Tuning::OutputPinName(), Sub, UCrowdyEffectGraphNode_BinaryOp::InputBPinName());
	LinkReturnParity(Two, UCrowdyEffectGraphNode_Constant::OutputPinName(), Mul, UCrowdyEffectGraphNode_BinaryOp::InputAPinName());
	LinkReturnParity(Sub, UCrowdyEffectGraphNode_BinaryOp::OutputPinName(), Mul, UCrowdyEffectGraphNode_BinaryOp::InputBPinName());
	LinkReturnParity(Mul, UCrowdyEffectGraphNode_BinaryOp::OutputPinName(), Clamp, UCrowdyEffectGraphNode_Call::ArgPinName(0));
	LinkReturnParity(Zero, UCrowdyEffectGraphNode_Constant::OutputPinName(), Clamp, UCrowdyEffectGraphNode_Call::ArgPinName(1));
	LinkReturnParity(Hundred, UCrowdyEffectGraphNode_Constant::OutputPinName(), Clamp, UCrowdyEffectGraphNode_Call::ArgPinName(2));
	LinkReturnParity(Clamp, UCrowdyEffectGraphNode_Call::OutputPinName(), Result, UCrowdyEffectGraphNode_Result::ReturnPinName());

	const FCrowdyEffectLoweringContext Ctx = MakeReturnParityContext();
	const FCrowdyEffectLoweringResult GraphResult = FCrowdyEffectGraphCompiler::CompileToFunction(Graph, Ctx);

	TestFalse(TEXT("the graph compiles without error"), GraphResult.HasErrors());
	TestEqual(TEXT("no mutation is produced by a pure return"), GraphResult.Function.Mutations.Num(), 0);
	TestEqual(TEXT("the multiply keeps the subtraction parenthesized, and the return is never clamp-wrapped"),
		GraphResult.Function.ReturnExpression, FString(TEXT("clamp(2 * (self.hp - $power), 0, 100)")));
	TestTrue(TEXT("a compound expression's type is not inferable, so the return type stays undeclared"),
		GraphResult.Function.ReturnType.IsEmpty());

	CheckReturnParityFunctionOnly(*this, Graph, TEXT("return clamp(2 * (self.hp - $power), 0, 100)"));
	return true;
}

// A return that reads the Source role: what makes an effect cross-entity. Both surfaces must inject the same
// source_id parameter and widen the same default invoke gate from owner_of_self to is_participant, identically.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectGraphReturnParitySourceReadTest,
	"CrowdySDK.Editor.GraphReturnParitySourceRead", CrowdyEffectGraphSchemaTestFlags)
bool FCrowdyEffectGraphReturnParitySourceReadTest::RunTest(const FString&)
{
	UCrowdyEffectGraph* Graph = NewObject<UCrowdyEffectGraph>(GetTransientPackage());

	UCrowdyEffectGraphNode_Attribute* Attack =
		MakeReturnParityAttribute(Graph, ECrowdyEffectRole::Source, TEXT("attack"));

	UCrowdyEffectGraphNode_Result* Result = AddReturnParityNode<UCrowdyEffectGraphNode_Result>(Graph);
	Result->bReturnsValue = true;
	Result->AllocateDefaultPins();

	LinkReturnParity(Attack, UCrowdyEffectGraphNode_Attribute::OutputPinName(), Result, UCrowdyEffectGraphNode_Result::ReturnPinName());

	const FCrowdyEffectLoweringContext Ctx = MakeReturnParityContext();
	const FCrowdyEffectLoweringResult GraphResult = FCrowdyEffectGraphCompiler::CompileToFunction(Graph, Ctx);

	TestFalse(TEXT("the graph compiles without error"), GraphResult.HasErrors());
	TestTrue(TEXT("reading source through the return is detected as a cross-entity reference"), GraphResult.bSourceReferenced);
	TestEqual(TEXT("the return reads through the injected source ref"),
		GraphResult.Function.ReturnExpression, FString(TEXT("ref($source_id).attack")));
	TestEqual(TEXT("the return type is inferred from the source attribute"), GraphResult.Function.ReturnType, FString(TEXT("int")));
	TestTrue(TEXT("a source_id container_ref parameter is injected"),
		GraphResult.Function.Parameters.ContainsByPredicate([](const FCrowdyGameModelFunctionParam& P)
		{
			return P.Name == TEXT("source_id") && P.ValueType == TEXT("container_ref");
		}));
	TestTrue(TEXT("the default invoke gate widens to is_participant"),
		GraphResult.Function.InvokePolicyJson.Contains(TEXT("is_participant")));

	CheckReturnParityExact(*this, Graph, TEXT("return source.attack"));
	return true;
}

// The exactly-once-add idiom documented on the Append builtin: p = if(index_of(p, X) < 0, append(p, X), p). A plain
// '=' assignment adds no parentheses of its own (unlike '+=' '-=' '*=' '/=', which always parenthesize their
// right-hand side), so this pins that the if(...)/index_of(...)/append(...) shape survives lowering byte for byte
// rather than being reordered or over/under-parenthesized by the emitter.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectLoweringGuardedAppendRoundTripTest,
	"CrowdySDK.Editor.LoweringGuardedAppendRoundTrip", CrowdyEffectGraphSchemaTestFlags)
bool FCrowdyEffectLoweringGuardedAppendRoundTripTest::RunTest(const FString&)
{
	FCrowdyEffectLoweringContext Ctx;
	Ctx.FunctionName = TEXT("add_item");
	Ctx.ContainerTypeName = TEXT("Hero");

	FCrowdyAttributeDef ListAttr;
	ListAttr.PropertyName = FName(TEXT("P"));
	ListAttr.Key = TEXT("p");
	ListAttr.ValueType = TEXT("array");
	Ctx.Attributes.Add(ListAttr);

	FCrowdyEffectParamDecl XParam;
	XParam.Name = TEXT("x");
	XParam.ValueType = TEXT("int");
	Ctx.Magnitudes.Add(XParam);

	const FCrowdyEffectParseResult Parsed = FCrowdyEffectParser::Parse(
		TEXT("self.p = if(index_of(self.p, $x) < 0, append(self.p, $x), self.p)"));
	TestFalse(TEXT("the guarded-add script parses without error"), Parsed.HasErrors());

	const FCrowdyEffectLoweringResult Result = FCrowdyEffectLowering::Lower(Parsed.Program, Ctx);
	TestFalse(TEXT("the guarded-add script lowers without error"), Result.HasErrors());
	if (TestEqual(TEXT("one mutation is produced"), Result.Function.Mutations.Num(), 1))
	{
		TestEqual(TEXT("the guarded-add shape survives emission unwrapped: no clamp exists for a list attribute, "
			"and a plain '=' assignment adds no extra parentheses of its own"),
			Result.Function.Mutations[0].Expression,
			FString(TEXT("if(index_of(self.p, $x) < 0, append(self.p, $x), self.p)")));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
