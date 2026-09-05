// Fill out your copyright notice in the Description page of Project Settings.

#include "Graph/CrowdyEffectGraphCompiler.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Graph/CrowdyEffectGraph.h"
#include "Graph/CrowdyEffectGraphNodes.h"
#include "Replication/GameModel/Effect/CrowdyEffectAst.h"
#include "Replication/GameModel/Effect/CrowdyEffectSpecBuilder.h"

namespace
{
	// Emitting deeper than this is treated as a malformed graph (an over-deep chain or a cycle in the value nodes).
	constexpr int32 GraphEmitMaxDepth = 64;

	const TCHAR* ArithOpGlyph(ECrowdyEffectGraphArithOp Op)
	{
		switch (Op)
		{
		case ECrowdyEffectGraphArithOp::Add:      return TEXT("+");
		case ECrowdyEffectGraphArithOp::Subtract: return TEXT("-");
		case ECrowdyEffectGraphArithOp::Multiply: return TEXT("*");
		case ECrowdyEffectGraphArithOp::Divide:   return TEXT("/");
		case ECrowdyEffectGraphArithOp::Modulo:   return TEXT("%");
		default:                                  return TEXT("+");
		}
	}

	const TCHAR* CompareGlyph(ECrowdyEffectComparator Cmp)
	{
		switch (Cmp)
		{
		case ECrowdyEffectComparator::Equal:          return TEXT("==");
		case ECrowdyEffectComparator::NotEqual:       return TEXT("!=");
		case ECrowdyEffectComparator::Less:           return TEXT("<");
		case ECrowdyEffectComparator::Greater:        return TEXT(">");
		case ECrowdyEffectComparator::LessOrEqual:    return TEXT("<=");
		case ECrowdyEffectComparator::GreaterOrEqual: return TEXT(">=");
		default:                                      return TEXT("==");
		}
	}

	const TCHAR* LogicGlyph(ECrowdyEffectGraphLogicOp Op)
	{
		return Op == ECrowdyEffectGraphLogicOp::Or ? TEXT("||") : TEXT("&&");
	}

	const TCHAR* UnaryGlyph(ECrowdyEffectGraphUnaryOp Op)
	{
		return Op == ECrowdyEffectGraphUnaryOp::Negate ? TEXT("-") : TEXT("!");
	}

	const TCHAR* RolePrefix(ECrowdyEffectRole Role)
	{
		return Role == ECrowdyEffectRole::Source ? TEXT("source.") : TEXT("self.");
	}

	// Escapes a string constant for embedding inside DSL double quotes, mirroring the escapes the tokenizer decodes:
	// backslash and quote are backslash-escaped, and a line feed becomes "\n". A raw line feed would otherwise
	// terminate the string when reparsed, so it must be encoded; a carriage return has no representable escape and is
	// rejected before this runs.
	FString EscapeDslString(const FString& In)
	{
		FString Out;
		Out.Reserve(In.Len() + 2);
		for (const TCHAR C : In)
		{
			if (C == TEXT('\n'))
			{
				Out.Append(TEXT("\\n"));
				continue;
			}
			if (C == TEXT('\\') || C == TEXT('"'))
			{
				Out.AppendChar(TEXT('\\'));
			}
			Out.AppendChar(C);
		}
		return Out;
	}

	// The output pin driving an input, or null when nothing is wired into it.
	const UEdGraphPin* LinkedSource(const UEdGraphPin* InputPin)
	{
		if (InputPin && InputPin->LinkedTo.Num() > 0)
		{
			return InputPin->LinkedTo[0];
		}
		return nullptr;
	}

	// Collects diagnostics during a compile. Every problem lands in the flat list (the caller's OutDiagnostics, byte
	// identical to before); when NodeDiagnostics is set, the same problem is also paired with the graph node it came
	// from, so the editor can badge that node. Node is best-effort: a whole-graph problem passes null.
	struct FGraphDiagSink
	{
		TArray<FCrowdyEffectDiagnostic>& Flat;
		TArray<FCrowdyEffectGraphNodeDiagnostic>* NodeDiagnostics = nullptr;

		void Error(const UEdGraphNode* Node, int32 Line, const FString& Message)
		{
			Flat.Add({ ECrowdyEffectSeverity::Error, Line, 0, Message });
			if (NodeDiagnostics)
			{
				FCrowdyEffectGraphNodeDiagnostic Diag;
				Diag.Severity = ECrowdyEffectSeverity::Error;
				Diag.Node = Node;
				Diag.Message = Message;
				NodeDiagnostics->Add(MoveTemp(Diag));
			}
		}
	};

	FString EmitExprInternal(const UEdGraphPin* SourceOutputPin, FGraphDiagSink& Sink, int32 Depth);

	// A node's display title, used to point a diagnostic at the offending node (e.g. "If (Cond, Then, Else)").
	FString NodeLabel(const UEdGraphNode* Node)
	{
		return Node ? Node->GetNodeTitle(ENodeTitleType::ListView).ToString() : FString(TEXT("a node"));
	}

	// An input pin's readable label: its friendly name when set (e.g. "Then"), otherwise its raw name.
	FString InputPinLabel(const UEdGraphPin* Pin, FName Fallback)
	{
		if (Pin && !Pin->PinFriendlyName.IsEmpty())
		{
			return Pin->PinFriendlyName.ToString();
		}
		return Fallback.ToString();
	}
}

namespace
{
	FString EmitExprInternal(const UEdGraphPin* SourceOutputPin, FGraphDiagSink& Sink, int32 Depth)
	{
		if (Depth > GraphEmitMaxDepth)
		{
			Sink.Error(nullptr, 0, TEXT("effect graph is nested too deeply, or contains a cycle, to compile"));
			return FString();
		}
		if (!SourceOutputPin)
		{
			Sink.Error(nullptr, 0, TEXT("an effect graph value input is not connected"));
			return FString();
		}

		const UEdGraphNode* OwningNode = SourceOutputPin->GetOwningNodeUnchecked();

		// Emits the value wired into one of OwningNode's named input pins, recording an error if nothing is connected.
		// An empty return always signals a failure the caller propagates (every valid emit is non-empty).
		const auto EmitInput = [&Sink, Depth](const UEdGraphNode* Node, FName PinName) -> FString
		{
			const UEdGraphPin* InputPin = Node->FindPin(PinName, EGPD_Input);
			const UEdGraphPin* Source = LinkedSource(InputPin);
			if (!Source)
			{
				Sink.Error(Node, 0, FString::Printf(TEXT("the '%s' input of the '%s' node is not connected"),
					*InputPinLabel(InputPin, PinName), *NodeLabel(Node)));
				return FString();
			}
			return EmitExprInternal(Source, Sink, Depth + 1);
		};

		if (const UCrowdyEffectGraphNode_Tuning* Tuning = Cast<UCrowdyEffectGraphNode_Tuning>(OwningNode))
		{
			if (Tuning->ParamName.TrimStartAndEnd().IsEmpty())
			{
				Sink.Error(OwningNode, 0, TEXT("an effect graph tuning node has no $param name"));
				return FString();
			}
			return TEXT("$") + Tuning->ParamName;
		}
		if (const UCrowdyEffectGraphNode_Attribute* Attribute = Cast<UCrowdyEffectGraphNode_Attribute>(OwningNode))
		{
			if (Attribute->Attribute.TrimStartAndEnd().IsEmpty())
			{
				Sink.Error(OwningNode, 0, TEXT("an effect graph attribute node has no attribute name"));
				return FString();
			}
			return RolePrefix(Attribute->Role) + Attribute->Attribute;
		}
		if (const UCrowdyEffectGraphNode_Constant* Constant = Cast<UCrowdyEffectGraphNode_Constant>(OwningNode))
		{
			const FString Trimmed = Constant->Literal.TrimStartAndEnd();
			switch (Constant->ConstantType)
			{
			case ECrowdyEffectGraphConstantType::Null:
				return TEXT("null");
			case ECrowdyEffectGraphConstantType::String:
				// A carriage return has no escape the tokenizer decodes, so it cannot round-trip through the text form.
				if (Constant->Literal.Contains(TEXT("\r")))
				{
					Sink.Error(OwningNode, 0, TEXT("an effect graph string constant contains a carriage return, which cannot be represented"));
					return FString();
				}
				return TEXT("\"") + EscapeDslString(Constant->Literal) + TEXT("\"");
			case ECrowdyEffectGraphConstantType::Bool:
				if (Trimmed != TEXT("true") && Trimmed != TEXT("false"))
				{
					Sink.Error(OwningNode, 0, TEXT("an effect graph bool constant must be 'true' or 'false'"));
					return FString();
				}
				return Trimmed;
			case ECrowdyEffectGraphConstantType::Number:
			default:
				// A single numeric literal only; a compound expression typed here would splice across the surrounding
				// operator and break precedence, so reject anything that is not a plain number.
				if (Trimmed.IsEmpty() || !Trimmed.IsNumeric())
				{
					Sink.Error(OwningNode, 0, TEXT("an effect graph number constant is not a valid number"));
					return FString();
				}
				return Trimmed;
			}
		}
		if (const UCrowdyEffectGraphNode_BinaryOp* Binary = Cast<UCrowdyEffectGraphNode_BinaryOp>(OwningNode))
		{
			const FString A = EmitInput(Binary, UCrowdyEffectGraphNode_BinaryOp::InputAPinName());
			const FString B = EmitInput(Binary, UCrowdyEffectGraphNode_BinaryOp::InputBPinName());
			if (A.IsEmpty() || B.IsEmpty())
			{
				return FString();
			}

			// Fully parenthesized so the emitted text reparses to the exact tree the graph describes, precedence intact.
			return FString(TEXT("(")) + A + TEXT(" ") + ArithOpGlyph(Binary->Op) + TEXT(" ") + B + TEXT(")");
		}
		if (const UCrowdyEffectGraphNode_Compare* Compare = Cast<UCrowdyEffectGraphNode_Compare>(OwningNode))
		{
			const FString A = EmitInput(Compare, UCrowdyEffectGraphNode_Compare::InputAPinName());
			const FString B = EmitInput(Compare, UCrowdyEffectGraphNode_Compare::InputBPinName());
			if (A.IsEmpty() || B.IsEmpty())
			{
				return FString();
			}
			return FString(TEXT("(")) + A + TEXT(" ") + CompareGlyph(Compare->Comparator) + TEXT(" ") + B + TEXT(")");
		}
		if (const UCrowdyEffectGraphNode_Logic* Logic = Cast<UCrowdyEffectGraphNode_Logic>(OwningNode))
		{
			const FString A = EmitInput(Logic, UCrowdyEffectGraphNode_Logic::InputAPinName());
			const FString B = EmitInput(Logic, UCrowdyEffectGraphNode_Logic::InputBPinName());
			if (A.IsEmpty() || B.IsEmpty())
			{
				return FString();
			}
			return FString(TEXT("(")) + A + TEXT(" ") + LogicGlyph(Logic->Op) + TEXT(" ") + B + TEXT(")");
		}
		if (const UCrowdyEffectGraphNode_Unary* Unary = Cast<UCrowdyEffectGraphNode_Unary>(OwningNode))
		{
			const FString X = EmitInput(Unary, UCrowdyEffectGraphNode_Unary::InputPinName());
			if (X.IsEmpty())
			{
				return FString();
			}
			return FString(TEXT("(")) + UnaryGlyph(Unary->Op) + X + TEXT(")");
		}
		if (const UCrowdyEffectGraphNode_If* If = Cast<UCrowdyEffectGraphNode_If>(OwningNode))
		{
			const FString Cond = EmitInput(If, UCrowdyEffectGraphNode_If::ConditionPinName());
			const FString Then = EmitInput(If, UCrowdyEffectGraphNode_If::ThenPinName());
			const FString Else = EmitInput(If, UCrowdyEffectGraphNode_If::ElsePinName());
			if (Cond.IsEmpty() || Then.IsEmpty() || Else.IsEmpty())
			{
				return FString();
			}
			return FString(TEXT("if(")) + Cond + TEXT(", ") + Then + TEXT(", ") + Else + TEXT(")");
		}
		if (const UCrowdyEffectGraphNode_Call* Call = Cast<UCrowdyEffectGraphNode_Call>(OwningNode))
		{
			const FString Callee = Call->Callee.TrimStartAndEnd();
			if (Callee.IsEmpty())
			{
				Sink.Error(OwningNode, 0, TEXT("an effect graph call node has no function name"));
				return FString();
			}
			FString Args;
			for (int32 Index = 0; Index < Call->ArgCount; ++Index)
			{
				const FString Arg = EmitInput(Call, UCrowdyEffectGraphNode_Call::ArgPinName(Index));
				if (Arg.IsEmpty())
				{
					return FString();
				}
				if (Index > 0)
				{
					Args += TEXT(", ");
				}
				Args += Arg;
			}
			const FString Prefix = Call->bIsFnCall ? TEXT("fn:") : TEXT("");
			return Prefix + Callee + TEXT("(") + Args + TEXT(")");
		}
		if (const UCrowdyEffectGraphNode_ReadRef* Ref = Cast<UCrowdyEffectGraphNode_ReadRef>(OwningNode))
		{
			const FString Attr = Ref->Attribute.TrimStartAndEnd();
			if (Attr.IsEmpty())
			{
				Sink.Error(OwningNode, 0, TEXT("an effect graph ref-read node has no attribute name"));
				return FString();
			}
			const FString Id = EmitInput(Ref, UCrowdyEffectGraphNode_ReadRef::IdPinName());
			if (Id.IsEmpty())
			{
				return FString();
			}
			return FString(TEXT("ref(")) + Id + TEXT(").") + Attr;
		}

		Sink.Error(OwningNode, 0, TEXT("unsupported effect graph value node"));
		return FString();
	}

	FCrowdyEffectSpec CompileToSpecInternal(const UCrowdyEffectGraph* Graph, FGraphDiagSink& Sink)
	{
		FCrowdyEffectSpec Spec;

		if (!Graph)
		{
			Sink.Error(nullptr, 0, TEXT("there is no effect graph to compile"));
			return Spec;
		}

		const UCrowdyEffectGraphNode_Result* Result = nullptr;
		int32 ResultCount = 0;
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (const UCrowdyEffectGraphNode_Result* Candidate = Cast<UCrowdyEffectGraphNode_Result>(Node))
			{
				if (!Result)
				{
					Result = Candidate;
				}
				++ResultCount;
			}
		}
		if (!Result)
		{
			Sink.Error(nullptr, 0, TEXT("the effect graph has no Result node"));
			return Spec;
		}
		if (ResultCount > 1)
		{
			// More than one Result is ambiguous: only the first is compiled, so the rest would silently vanish.
			Sink.Error(Result, 0, TEXT("the effect graph has more than one Result node; only the first is compiled"));
		}
		if (Result->Writes.Num() == 0 && Result->KeywordConditions.Num() == 0 && Result->Requires.Num() == 0
			&& !Result->bReturnsValue)
		{
			Sink.Error(Result, 0, TEXT("the effect graph Result node has no writes, conditions, or return value"));
			return Spec;
		}

		for (int32 Index = 0; Index < Result->Writes.Num(); ++Index)
		{
			const FCrowdyEffectGraphWrite& Write = Result->Writes[Index];
			const UEdGraphPin* Source = LinkedSource(
				Result->FindPin(UCrowdyEffectGraphNode_Result::WritePinName(Index), EGPD_Input));
			if (!Source)
			{
				Sink.Error(Result, Index + 1, FString::Printf(
					TEXT("the write to %s%s has no value connected"), RolePrefix(Write.TargetRole), *Write.Attribute));
				continue;
			}

			const FString Emitted = EmitExprInternal(Source, Sink, 0);
			if (Emitted.IsEmpty())
			{
				// EmitExprInternal already recorded the underlying error; skip the half-built assignment.
				continue;
			}

			FCrowdyEffectAssignmentSpec Assignment;
			Assignment.TargetRole = Write.TargetRole;
			Assignment.Attribute = Write.Attribute;
			Assignment.Operator = Write.Op;

			FCrowdyEffectTerm Term;
			Term.Op = ECrowdyEffectBinaryOp::Add;
			Term.Operand.Kind = ECrowdyEffectOperandKind::Expression;
			Term.Operand.Literal = Emitted;
			Assignment.Value.Add(MoveTemp(Term));

			Spec.Assignments.Add(MoveTemp(Assignment));
		}

		for (const ECrowdyEffectPolicyKeyword Keyword : Result->KeywordConditions)
		{
			FCrowdyEffectRequireSpec Require;
			Require.Kind = ECrowdyEffectRequireKind::Keyword;
			Require.Keyword = Keyword;
			Spec.Requires.Add(MoveTemp(Require));
		}

		for (int32 Index = 0; Index < Result->Requires.Num(); ++Index)
		{
			const UEdGraphPin* Source = LinkedSource(
				Result->FindPin(UCrowdyEffectGraphNode_Result::RequirePinName(Index), EGPD_Input));
			if (!Source)
			{
				Sink.Error(Result, Result->Writes.Num() + Index + 1,
					TEXT("a condition on the Result node has no boolean value connected"));
				continue;
			}

			FCrowdyEffectRequireSpec Require;
			Require.Kind = ECrowdyEffectRequireKind::Comparison;
			Require.Left.Kind = ECrowdyEffectOperandKind::Expression;
			Require.Right.Kind = ECrowdyEffectOperandKind::Expression;

			// A Compare node wired straight into a condition maps to the comparison require it already describes
			// (Left <cmp> Right), so the common case compiles to exactly what the text form's "require a >= b" produces.
			// Any other boolean value (an And/Or, a Not, a bool attribute, is_null(...)) is required to hold via
			// "<expr> == true"; the wrap is invisible in the deployed function.
			const UEdGraphNode* SourceNode = Source->GetOwningNodeUnchecked();
			if (const UCrowdyEffectGraphNode_Compare* Compare = Cast<UCrowdyEffectGraphNode_Compare>(SourceNode))
			{
				const UEdGraphPin* LeftSource = LinkedSource(
					Compare->FindPin(UCrowdyEffectGraphNode_Compare::InputAPinName(), EGPD_Input));
				const UEdGraphPin* RightSource = LinkedSource(
					Compare->FindPin(UCrowdyEffectGraphNode_Compare::InputBPinName(), EGPD_Input));
				if (!LeftSource || !RightSource)
				{
					Sink.Error(SourceNode, Result->Writes.Num() + Index + 1, FString::Printf(
						TEXT("an input of the '%s' compare node feeding a condition is not connected"), *NodeLabel(SourceNode)));
					continue;
				}
				const FString Left = EmitExprInternal(LeftSource, Sink, 0);
				const FString Right = EmitExprInternal(RightSource, Sink, 0);
				if (Left.IsEmpty() || Right.IsEmpty())
				{
					continue;
				}
				Require.Comparator = Compare->Comparator;
				Require.Left.Literal = Left;
				Require.Right.Literal = Right;
			}
			else
			{
				const FString Expr = EmitExprInternal(Source, Sink, 0);
				if (Expr.IsEmpty())
				{
					// EmitExprInternal already recorded the underlying error; skip the half-built condition.
					continue;
				}
				Require.Comparator = ECrowdyEffectComparator::Equal;
				Require.Left.Literal = Expr;
				Require.Right.Literal = TEXT("true");
			}

			Spec.Requires.Add(MoveTemp(Require));
		}

		if (Result->bReturnsValue)
		{
			const UEdGraphPin* Source = LinkedSource(
				Result->FindPin(UCrowdyEffectGraphNode_Result::ReturnPinName(), EGPD_Input));
			if (!Source)
			{
				Sink.Error(Result, Result->Writes.Num() + Result->Requires.Num() + 1,
					TEXT("the Return has no value connected"));
			}
			else
			{
				const FString Emitted = EmitExprInternal(Source, Sink, 0);
				if (!Emitted.IsEmpty())
				{
					Spec.bHasReturn = true;
					Spec.Return.Kind = ECrowdyEffectOperandKind::Expression;
					Spec.Return.Literal = Emitted;
				}
				// EmitExprInternal already recorded the underlying error when Emitted is empty; the spec is left
				// with no return rather than a half-built one.
			}
		}

		return Spec;
	}
}

FString FCrowdyEffectGraphCompiler::EmitExpr(
	const UEdGraphPin* SourceOutputPin, TArray<FCrowdyEffectDiagnostic>& OutDiagnostics, int32 Depth)
{
	FGraphDiagSink Sink{ OutDiagnostics, nullptr };
	return EmitExprInternal(SourceOutputPin, Sink, Depth);
}

FCrowdyEffectSpec FCrowdyEffectGraphCompiler::CompileToSpec(
	const UCrowdyEffectGraph* Graph, TArray<FCrowdyEffectDiagnostic>& OutDiagnostics)
{
	FGraphDiagSink Sink{ OutDiagnostics, nullptr };
	return CompileToSpecInternal(Graph, Sink);
}

FCrowdyEffectSpec FCrowdyEffectGraphCompiler::CompileToSpecWithNodeDiagnostics(
	const UCrowdyEffectGraph* Graph,
	TArray<FCrowdyEffectDiagnostic>& OutDiagnostics,
	TArray<FCrowdyEffectGraphNodeDiagnostic>& OutNodeDiagnostics)
{
	FGraphDiagSink Sink{ OutDiagnostics, &OutNodeDiagnostics };
	return CompileToSpecInternal(Graph, Sink);
}

FCrowdyEffectLoweringResult FCrowdyEffectGraphCompiler::CompileToFunction(
	const UCrowdyEffectGraph* Graph, const FCrowdyEffectLoweringContext& Context)
{
	TArray<FCrowdyEffectDiagnostic> SpecDiagnostics;
	const FCrowdyEffectSpec Spec = CompileToSpec(Graph, SpecDiagnostics);

	TArray<FCrowdyEffectDiagnostic> BuildDiagnostics;
	const FCrowdyEffectProgram Program = FCrowdyEffectSpecBuilder::BuildProgram(Spec, BuildDiagnostics);

	FCrowdyEffectLoweringResult Result = FCrowdyEffectLowering::Lower(Program, Context);

	// Surface the graph and builder problems in the same result the caller inspects, ahead of the lowering ones.
	TArray<FCrowdyEffectDiagnostic> Combined;
	Combined.Append(SpecDiagnostics);
	Combined.Append(BuildDiagnostics);
	Combined.Append(Result.Diagnostics);
	Result.Diagnostics = MoveTemp(Combined);

	return Result;
}
