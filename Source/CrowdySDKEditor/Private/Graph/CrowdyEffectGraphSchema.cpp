// Fill out your copyright notice in the Description page of Project Settings.

#include "Graph/CrowdyEffectGraphSchema.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Graph/CrowdyEffectGraphNodeOptions.h"
#include "Graph/CrowdyEffectGraphNodes.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "CrowdyEffectGraphSchema"

UEdGraphNode* FCrowdyEffectGraphSchemaAction_NewNode::PerformAction(
	UEdGraph* ParentGraph, UEdGraphPin* FromPin, const FVector2f& Location, bool bSelectNewNode)
{
	if (!ParentGraph || !NodeClass)
	{
		return nullptr;
	}

	const FScopedTransaction Transaction(LOCTEXT("AddEffectNode", "Add Effect Graph Node"));
	ParentGraph->Modify();
	if (FromPin)
	{
		FromPin->Modify();
	}

	UCrowdyEffectGraphNode* NewNode = NewObject<UCrowdyEffectGraphNode>(ParentGraph, NodeClass, NAME_None, RF_Transactional);
	ParentGraph->AddNode(NewNode, true, bSelectNewNode);

	NewNode->CreateNewGuid();
	NewNode->PostPlacedNewNode();

	// Stamp the operation this entry names before the pins are allocated, since it can decide a pin's category and how
	// many pins there are.
	ApplyPresets(*NewNode, Presets);

	NewNode->AllocateDefaultPins();
	NewNode->NodePosX = Location.X;
	NewNode->NodePosY = Location.Y;

	// When the node was dragged off an existing pin, wire the new node's compatible pin to it (via the schema's own
	// connection rules), so a drag-and-release lands connected.
	if (FromPin)
	{
		NewNode->AutowireNewNode(FromPin);
	}

	ParentGraph->NotifyGraphChanged();
	return NewNode;
}

int32 FCrowdyEffectGraphSchemaAction_NewNode::ApplyPresets(
	UCrowdyEffectGraphNode& Node, const TMap<FName, FString>& InPresets)
{
	int32 Applied = 0;
	for (const TPair<FName, FString>& Preset : InPresets)
	{
		FProperty* Property = Node.GetClass()->FindPropertyByName(Preset.Key);
		if (!Property)
		{
			continue;
		}
		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(&Node);
		if (Property->ImportText_Direct(*Preset.Value, ValuePtr, &Node, PPF_None) != nullptr)
		{
			++Applied;
		}
	}
	return Applied;
}

void UCrowdyEffectGraphSchema::GetGraphContextActions(FGraphContextMenuBuilder& ContextMenuBuilder) const
{
	// Entries are grouped into named categories the menu renders as expandable sections. This is a different mechanism
	// from Grouping, which draws divider bars: every entry keeps grouping 0, so the categories organize the palette
	// without cutting it into boxed-off rows.
	const FText AttributesCategory = LOCTEXT("CatAttributes", "Attributes");
	const FText ValuesCategory = LOCTEXT("CatValues", "Values");
	const FText ArithmeticCategory = LOCTEXT("CatArithmetic", "Arithmetic");
	const FText ComparisonCategory = LOCTEXT("CatComparison", "Comparison");
	const FText LogicCategory = LOCTEXT("CatLogic", "Logic");
	const FText ServerFunctionsCategory = LOCTEXT("CatServerFunctions", "Server Functions");
	const FText FlowCategory = LOCTEXT("CatFlow", "Flow");

	auto Add = [&ContextMenuBuilder](
		const FText& Category, TSubclassOf<UCrowdyEffectGraphNode> NodeClass, const FText& MenuDesc,
		const FText& Tooltip, const FText& Keywords = FText::GetEmpty(), TMap<FName, FString> Presets = {})
	{
		const TSharedPtr<FCrowdyEffectGraphSchemaAction_NewNode> Action =
			MakeShared<FCrowdyEffectGraphSchemaAction_NewNode>(
				Category, MenuDesc, Tooltip, NodeClass, Keywords, MoveTemp(Presets));
		ContextMenuBuilder.AddAction(Action);
	};

	const FName RolePropertyName = GET_MEMBER_NAME_CHECKED(UCrowdyEffectGraphNode_Attribute, Role);
	const FName ConstantTypePropertyName = GET_MEMBER_NAME_CHECKED(UCrowdyEffectGraphNode_Constant, ConstantType);
	const FName ArithOpPropertyName = GET_MEMBER_NAME_CHECKED(UCrowdyEffectGraphNode_BinaryOp, Op);
	const FName ComparatorPropertyName = GET_MEMBER_NAME_CHECKED(UCrowdyEffectGraphNode_Compare, Comparator);
	const FName LogicOpPropertyName = GET_MEMBER_NAME_CHECKED(UCrowdyEffectGraphNode_Logic, Op);
	const FName UnaryOpPropertyName = GET_MEMBER_NAME_CHECKED(UCrowdyEffectGraphNode_Unary, Op);
	const FName CalleePropertyName = GET_MEMBER_NAME_CHECKED(UCrowdyEffectGraphNode_Call, Callee);
	const FName ArgCountPropertyName = GET_MEMBER_NAME_CHECKED(UCrowdyEffectGraphNode_Call, ArgCount);

	Add(AttributesCategory, UCrowdyEffectGraphNode_Attribute::StaticClass(),
		LOCTEXT("NewAttributeSelf", "Attribute (self)"),
		LOCTEXT("NewAttributeSelfTip", "Read an attribute of the Target (self) container."),
		LOCTEXT("NewAttributeSelfKeys", "attribute self target read"),
		{{ RolePropertyName, TEXT("Target") }});
	Add(AttributesCategory, UCrowdyEffectGraphNode_Attribute::StaticClass(),
		LOCTEXT("NewAttributeSource", "Attribute (source)"),
		LOCTEXT("NewAttributeSourceTip", "Read an attribute of the Source (instigator) container."),
		LOCTEXT("NewAttributeSourceKeys", "attribute source instigator read"),
		{{ RolePropertyName, TEXT("Source") }});
	Add(ValuesCategory, UCrowdyEffectGraphNode_Tuning::StaticClass(),
		LOCTEXT("NewTuning", "Tuning Parameter"),
		LOCTEXT("NewTuningTip", "Read a designer-tunable value exposed on the effect asset."),
		LOCTEXT("NewTuningKeys", "tuning parameter magnitude balance"));
	Add(AttributesCategory, UCrowdyEffectGraphNode_ReadRef::StaticClass(),
		LOCTEXT("NewReadRef", "Read Referenced Attribute"),
		LOCTEXT("NewReadRefTip", "Read an attribute from another container by its id."),
		LOCTEXT("NewReadRefKeys", "ref reference container id read"));

	Add(ValuesCategory, UCrowdyEffectGraphNode_Constant::StaticClass(),
		LOCTEXT("NewConstNumber", "Constant: Number"),
		LOCTEXT("NewConstNumberTip", "A literal number."),
		LOCTEXT("NewConstNumberKeys", "constant literal number float int"),
		{{ ConstantTypePropertyName, TEXT("Number") }});
	Add(ValuesCategory, UCrowdyEffectGraphNode_Constant::StaticClass(),
		LOCTEXT("NewConstBool", "Constant: Bool ( true / false )"),
		LOCTEXT("NewConstBoolTip", "A literal true or false."),
		LOCTEXT("NewConstBoolKeys", "constant literal bool boolean true false"),
		{{ ConstantTypePropertyName, TEXT("Bool") }});
	Add(ValuesCategory, UCrowdyEffectGraphNode_Constant::StaticClass(),
		LOCTEXT("NewConstString", "Constant: String"),
		LOCTEXT("NewConstStringTip", "A literal string, quoted and escaped on emit."),
		LOCTEXT("NewConstStringKeys", "constant literal string text"),
		{{ ConstantTypePropertyName, TEXT("String") }});
	Add(ValuesCategory, UCrowdyEffectGraphNode_Constant::StaticClass(),
		LOCTEXT("NewConstNull", "Constant: Null"),
		LOCTEXT("NewConstNullTip", "The literal null."),
		LOCTEXT("NewConstNullKeys", "constant literal null none empty"),
		{{ ConstantTypePropertyName, TEXT("Null") }});

	Add(ArithmeticCategory, UCrowdyEffectGraphNode_BinaryOp::StaticClass(),
		LOCTEXT("NewAdd", "Add ( A + B )"),
		LOCTEXT("NewAddTip", "Adds two values."),
		LOCTEXT("NewAddKeys", "+ add plus sum arithmetic"),
		{{ ArithOpPropertyName, TEXT("Add") }});
	Add(ArithmeticCategory, UCrowdyEffectGraphNode_BinaryOp::StaticClass(),
		LOCTEXT("NewSubtract", "Subtract ( A - B )"),
		LOCTEXT("NewSubtractTip", "Subtracts the second value from the first."),
		LOCTEXT("NewSubtractKeys", "- subtract minus difference arithmetic"),
		{{ ArithOpPropertyName, TEXT("Subtract") }});
	Add(ArithmeticCategory, UCrowdyEffectGraphNode_BinaryOp::StaticClass(),
		LOCTEXT("NewMultiply", "Multiply ( A * B )"),
		LOCTEXT("NewMultiplyTip", "Multiplies two values."),
		LOCTEXT("NewMultiplyKeys", "* multiply times product scale arithmetic"),
		{{ ArithOpPropertyName, TEXT("Multiply") }});
	Add(ArithmeticCategory, UCrowdyEffectGraphNode_BinaryOp::StaticClass(),
		LOCTEXT("NewDivide", "Divide ( A / B )"),
		LOCTEXT("NewDivideTip", "Divides the first value by the second."),
		LOCTEXT("NewDivideKeys", "/ divide quotient arithmetic"),
		{{ ArithOpPropertyName, TEXT("Divide") }});
	Add(ArithmeticCategory, UCrowdyEffectGraphNode_BinaryOp::StaticClass(),
		LOCTEXT("NewModulo", "Modulo ( A % B )"),
		LOCTEXT("NewModuloTip", "The remainder of dividing the first value by the second."),
		LOCTEXT("NewModuloKeys", "% modulo mod remainder arithmetic"),
		{{ ArithOpPropertyName, TEXT("Modulo") }});
	Add(ArithmeticCategory, UCrowdyEffectGraphNode_Unary::StaticClass(),
		LOCTEXT("NewNegate", "Negate ( -X )"),
		LOCTEXT("NewNegateTip", "Arithmetic negation of one value."),
		LOCTEXT("NewNegateKeys", "- negate negative minus invert arithmetic"),
		{{ UnaryOpPropertyName, TEXT("Negate") }});

	Add(ComparisonCategory, UCrowdyEffectGraphNode_Compare::StaticClass(),
		LOCTEXT("NewEqual", "Equal ( A == B )"),
		LOCTEXT("NewEqualTip", "True when the two values are equal."),
		LOCTEXT("NewEqualKeys", "== equal equals compare"),
		{{ ComparatorPropertyName, TEXT("Equal") }});
	Add(ComparisonCategory, UCrowdyEffectGraphNode_Compare::StaticClass(),
		LOCTEXT("NewNotEqual", "Not Equal ( A != B )"),
		LOCTEXT("NewNotEqualTip", "True when the two values differ."),
		LOCTEXT("NewNotEqualKeys", "!= not equal differs compare"),
		{{ ComparatorPropertyName, TEXT("NotEqual") }});
	Add(ComparisonCategory, UCrowdyEffectGraphNode_Compare::StaticClass(),
		LOCTEXT("NewLess", "Less Than ( A < B )"),
		LOCTEXT("NewLessTip", "True when the first value is less than the second."),
		LOCTEXT("NewLessKeys", "< less than below compare"),
		{{ ComparatorPropertyName, TEXT("Less") }});
	Add(ComparisonCategory, UCrowdyEffectGraphNode_Compare::StaticClass(),
		LOCTEXT("NewLessOrEqual", "Less Or Equal ( A <= B )"),
		LOCTEXT("NewLessOrEqualTip", "True when the first value is less than or equal to the second."),
		LOCTEXT("NewLessOrEqualKeys", "<= less or equal at most compare"),
		{{ ComparatorPropertyName, TEXT("LessOrEqual") }});
	Add(ComparisonCategory, UCrowdyEffectGraphNode_Compare::StaticClass(),
		LOCTEXT("NewGreater", "Greater Than ( A > B )"),
		LOCTEXT("NewGreaterTip", "True when the first value is greater than the second."),
		LOCTEXT("NewGreaterKeys", "> greater than above compare"),
		{{ ComparatorPropertyName, TEXT("Greater") }});
	Add(ComparisonCategory, UCrowdyEffectGraphNode_Compare::StaticClass(),
		LOCTEXT("NewGreaterOrEqual", "Greater Or Equal ( A >= B )"),
		LOCTEXT("NewGreaterOrEqualTip", "True when the first value is greater than or equal to the second."),
		LOCTEXT("NewGreaterOrEqualKeys", ">= greater or equal at least compare"),
		{{ ComparatorPropertyName, TEXT("GreaterOrEqual") }});

	Add(LogicCategory, UCrowdyEffectGraphNode_Logic::StaticClass(),
		LOCTEXT("NewAnd", "And ( A && B )"),
		LOCTEXT("NewAndTip", "True when both boolean inputs are true."),
		LOCTEXT("NewAndKeys", "&& and both logic boolean"),
		{{ LogicOpPropertyName, TEXT("And") }});
	Add(LogicCategory, UCrowdyEffectGraphNode_Logic::StaticClass(),
		LOCTEXT("NewOr", "Or ( A || B )"),
		LOCTEXT("NewOrTip", "True when either boolean input is true."),
		LOCTEXT("NewOrKeys", "|| or either logic boolean"),
		{{ LogicOpPropertyName, TEXT("Or") }});
	Add(LogicCategory, UCrowdyEffectGraphNode_Unary::StaticClass(),
		LOCTEXT("NewNot", "Not ( !X )"),
		LOCTEXT("NewNotTip", "Logical negation of one boolean value."),
		LOCTEXT("NewNotKeys", "! not invert negate logic boolean"),
		{{ UnaryOpPropertyName, TEXT("Not") }});

	Add(FlowCategory, UCrowdyEffectGraphNode_If::StaticClass(),
		LOCTEXT("NewIf", "Select ( If / Then / Else )"),
		LOCTEXT("NewIfTip", "Yields one of two values depending on a condition."),
		LOCTEXT("NewIfKeys", "if ternary select branch condition choose"));

	// One entry per builtin, each spawning the call already named and sized to that function's arity. Every builtin the
	// language has is offered here, so there is no blank "type the name yourself" call node: a builtin's name is never
	// typed, and a name that is typed is always a server function.
	for (const FCrowdyEffectBuiltinCall& Builtin : CrowdyEffectGraphNodeOptions::BuiltinCalls())
	{
		Add(FText::FromString(Builtin.PaletteCategory), UCrowdyEffectGraphNode_Call::StaticClass(),
			FText::FromString(Builtin.DisplayName),
			FText::FromString(Builtin.Description),
			FText::FromString(FString::Printf(TEXT("%s %s function builtin call"), *Builtin.DisplayName, *Builtin.Callee)),
			{{ CalleePropertyName, Builtin.Callee },
			 { ArgCountPropertyName, FString::FromInt(Builtin.MinArgs) }});
	}

	Add(ServerFunctionsCategory, UCrowdyEffectGraphNode_ServerCall::StaticClass(),
		LOCTEXT("NewServerCall", "Server Function (by name)"),
		LOCTEXT("NewServerCallTip",
			"Call a function you authored on this effect. Not one of the language's builtins: you name it, and the "
			"schema sync uploads it to the server."),
		LOCTEXT("NewServerCallKeys", "server function authored custom fn call"));
}

const FPinConnectionResponse UCrowdyEffectGraphSchema::CanCreateConnection(
	const UEdGraphPin* A, const UEdGraphPin* B) const
{
	if (!A || !B)
	{
		return FPinConnectionResponse(CONNECT_RESPONSE_DISALLOW, TEXT("Invalid pin"));
	}

	if (A->GetOwningNodeUnchecked() == B->GetOwningNodeUnchecked())
	{
		return FPinConnectionResponse(CONNECT_RESPONSE_DISALLOW, TEXT("A node cannot connect to itself"));
	}

	if (A->Direction == B->Direction)
	{
		return FPinConnectionResponse(CONNECT_RESPONSE_DISALLOW, TEXT("Connect a value output to a value input"));
	}

	auto IsEffectValuePin = [](const UEdGraphPin* Pin)
	{
		return Pin->PinType.PinCategory == CrowdyEffectGraphPins::ValueCategory
			|| Pin->PinType.PinCategory == CrowdyEffectGraphPins::BoolCategory;
	};
	if (!IsEffectValuePin(A) || !IsEffectValuePin(B))
	{
		return FPinConnectionResponse(CONNECT_RESPONSE_DISALLOW, TEXT("Only value pins can be connected"));
	}

	const UEdGraphPin* OutputPin = (A->Direction == EGPD_Output) ? A : B;
	const UEdGraphPin* InputPin = (A->Direction == EGPD_Input) ? A : B;

	// Refuse a driver that can never satisfy what the input needs (a string into arithmetic, a number into a condition
	// gate, a comparison result into a container id). An ambiguous producer, an attribute, a tuning value, a ref read,
	// a call, a select, is always allowed: it may genuinely be the right kind at runtime, and over-restricting here
	// would block legitimate graphs the compiler accepts.
	const ECrowdyEffectGraphPinNeed Need = RequirementForInput(InputPin);
	const ECrowdyEffectGraphValueKind Kind = ClassifyProducer(OutputPin->GetOwningNodeUnchecked());
	if (!IsKindAcceptable(Kind, Need))
	{
		switch (Need)
		{
		case ECrowdyEffectGraphPinNeed::Boolean:
			return FPinConnectionResponse(CONNECT_RESPONSE_DISALLOW,
				TEXT("This input needs a true/false value; a number, string, null, or arithmetic result cannot gate a condition"));
		case ECrowdyEffectGraphPinNeed::Numeric:
			return FPinConnectionResponse(CONNECT_RESPONSE_DISALLOW,
				TEXT("This input needs a number; a string or null cannot be used in arithmetic"));
		case ECrowdyEffectGraphPinNeed::Identifier:
			return FPinConnectionResponse(CONNECT_RESPONSE_DISALLOW,
				TEXT("This input needs a container id; a number, true/false, or null is never an id"));
		default:
			return FPinConnectionResponse(CONNECT_RESPONSE_DISALLOW, TEXT("That value cannot drive this input"));
		}
	}

	if (WouldConnectionCauseLoop(OutputPin, InputPin))
	{
		return FPinConnectionResponse(CONNECT_RESPONSE_DISALLOW, TEXT("That connection would create a cycle"));
	}

	// A value input holds a single link, so a new connection replaces whatever it already had. Breaking on the input
	// side is what keeps the graph free of the "only the first link is read" ambiguity the compiler otherwise inherits.
	if (InputPin->LinkedTo.Num() > 0)
	{
		const ECanCreateConnectionResponse BreakSide =
			(InputPin == A) ? CONNECT_RESPONSE_BREAK_OTHERS_A : CONNECT_RESPONSE_BREAK_OTHERS_B;
		return FPinConnectionResponse(BreakSide, TEXT("Replace the existing input connection"));
	}

	return FPinConnectionResponse(CONNECT_RESPONSE_MAKE, TEXT("Connect value"));
}

bool UCrowdyEffectGraphSchema::WouldConnectionCauseLoop(const UEdGraphPin* OutputPin, const UEdGraphPin* InputPin)
{
	if (!OutputPin || !InputPin)
	{
		return false;
	}

	const UEdGraphNode* Producer = OutputPin->GetOwningNodeUnchecked();
	const UEdGraphNode* Consumer = InputPin->GetOwningNodeUnchecked();
	if (!Producer || !Consumer)
	{
		return false;
	}

	// Walk forward along the existing dataflow from the consumer (each node's output pins to the nodes they feed). If
	// that reaches the producer, then adding producer -> consumer closes a loop.
	TSet<const UEdGraphNode*> Visited;
	TArray<const UEdGraphNode*> Stack;
	Stack.Push(Consumer);

	while (Stack.Num() > 0)
	{
		const UEdGraphNode* Node = Stack.Pop();
		if (Node == Producer)
		{
			return true;
		}
		if (Visited.Contains(Node))
		{
			continue;
		}
		Visited.Add(Node);

		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output)
			{
				continue;
			}
			for (const UEdGraphPin* Linked : Pin->LinkedTo)
			{
				if (Linked)
				{
					if (const UEdGraphNode* Next = Linked->GetOwningNodeUnchecked())
					{
						Stack.Push(Next);
					}
				}
			}
		}
	}

	return false;
}

bool UCrowdyEffectGraphSchema::IsPlainlyNonBooleanProducer(const UEdGraphNode* Node)
{
	return !IsKindAcceptable(ClassifyProducer(Node), ECrowdyEffectGraphPinNeed::Boolean);
}

ECrowdyEffectGraphValueKind UCrowdyEffectGraphSchema::ClassifyProducer(const UEdGraphNode* Node)
{
	if (const UCrowdyEffectGraphNode_Constant* Constant = Cast<UCrowdyEffectGraphNode_Constant>(Node))
	{
		switch (Constant->ConstantType)
		{
		case ECrowdyEffectGraphConstantType::Number: return ECrowdyEffectGraphValueKind::Number;
		case ECrowdyEffectGraphConstantType::Bool:   return ECrowdyEffectGraphValueKind::Bool;
		case ECrowdyEffectGraphConstantType::String: return ECrowdyEffectGraphValueKind::String;
		case ECrowdyEffectGraphConstantType::Null:   return ECrowdyEffectGraphValueKind::Null;
		}
	}
	if (const UCrowdyEffectGraphNode_Unary* Unary = Cast<UCrowdyEffectGraphNode_Unary>(Node))
	{
		return Unary->Op == ECrowdyEffectGraphUnaryOp::Negate
			? ECrowdyEffectGraphValueKind::Number
			: ECrowdyEffectGraphValueKind::Bool;
	}
	if (Node && Node->IsA<UCrowdyEffectGraphNode_BinaryOp>())
	{
		return ECrowdyEffectGraphValueKind::Number;
	}
	if (Node && (Node->IsA<UCrowdyEffectGraphNode_Compare>() || Node->IsA<UCrowdyEffectGraphNode_Logic>()))
	{
		return ECrowdyEffectGraphValueKind::Bool;
	}
	if (const UCrowdyEffectGraphNode_Call* Call = Cast<UCrowdyEffectGraphNode_Call>(Node))
	{
		// The four list builtins that hand back the whole (new) list. at/index_of return an element or an index, not
		// a list, so they fall through to Unknown like any other call; a server function (bIsFnCall) is unknown too,
		// since its return type is authored on the server.
		static const TCHAR* ArrayProducingCallees[] = { TEXT("array"), TEXT("append"), TEXT("set_at"), TEXT("remove_at") };
		if (!Call->bIsFnCall)
		{
			for (const TCHAR* Callee : ArrayProducingCallees)
			{
				if (Call->Callee.Equals(Callee, ESearchCase::IgnoreCase))
				{
					return ECrowdyEffectGraphValueKind::Array;
				}
			}
		}
	}

	// An attribute, a tuning value, a ref read, a call, or a select can be anything at runtime.
	return ECrowdyEffectGraphValueKind::Unknown;
}

ECrowdyEffectGraphPinNeed UCrowdyEffectGraphSchema::RequirementForInput(const UEdGraphPin* InputPin)
{
	if (!InputPin)
	{
		return ECrowdyEffectGraphPinNeed::Any;
	}
	if (InputPin->PinType.PinCategory == CrowdyEffectGraphPins::BoolCategory)
	{
		return ECrowdyEffectGraphPinNeed::Boolean;
	}

	const UEdGraphNode* Owner = InputPin->GetOwningNodeUnchecked();
	if (Owner && Owner->IsA<UCrowdyEffectGraphNode_BinaryOp>())
	{
		return ECrowdyEffectGraphPinNeed::Numeric;
	}
	if (Owner && Owner->IsA<UCrowdyEffectGraphNode_Unary>())
	{
		// A logical not's input is a boolean pin and was already handled above, so a value-category unary input is the
		// arithmetic negate.
		return ECrowdyEffectGraphPinNeed::Numeric;
	}
	if (Owner && Owner->IsA<UCrowdyEffectGraphNode_ReadRef>()
		&& InputPin->PinName == UCrowdyEffectGraphNode_ReadRef::IdPinName())
	{
		return ECrowdyEffectGraphPinNeed::Identifier;
	}
	return ECrowdyEffectGraphPinNeed::Any;
}

bool UCrowdyEffectGraphSchema::IsKindAcceptable(ECrowdyEffectGraphValueKind Kind, ECrowdyEffectGraphPinNeed Need)
{
	if (Kind == ECrowdyEffectGraphValueKind::Unknown)
	{
		return true;
	}

	switch (Need)
	{
	case ECrowdyEffectGraphPinNeed::Boolean:
		return Kind == ECrowdyEffectGraphValueKind::Bool;
	case ECrowdyEffectGraphPinNeed::Numeric:
		// A boolean participates in arithmetic as 0 / 1, which the effect language allows; a string or null does not.
		return Kind == ECrowdyEffectGraphValueKind::Number || Kind == ECrowdyEffectGraphValueKind::Bool;
	case ECrowdyEffectGraphPinNeed::Identifier:
		return Kind == ECrowdyEffectGraphValueKind::String;
	case ECrowdyEffectGraphPinNeed::Any:
	default:
		return true;
	}
}

FLinearColor UCrowdyEffectGraphSchema::GetPinTypeColor(const FEdGraphPinType& PinType) const
{
	if (PinType.PinCategory == CrowdyEffectGraphPins::BoolCategory)
	{
		// A warm red distinguishes a boolean pin from the teal value pins, the way the material editor tints its bool
		// pins apart from scalars.
		return FLinearColor(0.80f, 0.32f, 0.28f);
	}
	if (PinType.PinCategory == CrowdyEffectGraphPins::ValueCategory)
	{
		return FLinearColor(0.22f, 0.66f, 0.74f);
	}
	return FLinearColor::White;
}

void UCrowdyEffectGraphSchema::CreateDefaultNodesForGraph(UEdGraph& Graph) const
{
	// A fresh graph opens with its single Result output node, the way a new material opens with its output node. The
	// author wires values into it and adds writes / conditions on it through the details panel.
	FGraphNodeCreator<UCrowdyEffectGraphNode_Result> Creator(Graph);
	UCrowdyEffectGraphNode_Result* Result = Creator.CreateNode();
	Result->NodePosX = 400;
	Result->NodePosY = 0;
	Creator.Finalize();
}

#undef LOCTEXT_NAMESPACE
