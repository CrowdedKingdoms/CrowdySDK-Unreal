// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/Effect/CrowdyEffectSpecBuilder.h"

#include "Replication/GameModel/Effect/CrowdyEffectParser.h"
#include "Replication/GameModel/Effect/CrowdyEffectSpec.h"

namespace
{
	// The UI's Target is the affected/bound container = the AST's self; Source is the instigator = the AST's
	// source. Getting this backwards silently inverts every effect (a heal becomes damage-on-the-caster), so it
	// is asserted directly in the tests.
	ECrowdyEffectRefBase MapRole(ECrowdyEffectRole Role)
	{
		return Role == ECrowdyEffectRole::Source ? ECrowdyEffectRefBase::SourceRef : ECrowdyEffectRefBase::SelfRef;
	}

	ECrowdyEffectAssignOp MapAssignOp(ECrowdyEffectAssignmentOp Op)
	{
		switch (Op)
		{
		case ECrowdyEffectAssignmentOp::Set:      return ECrowdyEffectAssignOp::Set;
		case ECrowdyEffectAssignmentOp::Add:      return ECrowdyEffectAssignOp::Add;
		case ECrowdyEffectAssignmentOp::Subtract: return ECrowdyEffectAssignOp::Sub;
		case ECrowdyEffectAssignmentOp::Multiply: return ECrowdyEffectAssignOp::Mul;
		case ECrowdyEffectAssignmentOp::Divide:   return ECrowdyEffectAssignOp::Div;
		default:                                  return ECrowdyEffectAssignOp::Set;
		}
	}

	const TCHAR* BinaryOpToString(ECrowdyEffectBinaryOp Op)
	{
		switch (Op)
		{
		case ECrowdyEffectBinaryOp::Add:      return TEXT("+");
		case ECrowdyEffectBinaryOp::Subtract: return TEXT("-");
		case ECrowdyEffectBinaryOp::Multiply: return TEXT("*");
		case ECrowdyEffectBinaryOp::Divide:   return TEXT("/");
		default:                              return TEXT("+");
		}
	}

	const TCHAR* ComparatorToString(ECrowdyEffectComparator Cmp)
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

	// Maps a closed policy keyword to the SHORT bareword the lowering recognizes (owner -> owner_of_self, etc.,
	// done inside FCrowdyEffectLowering). The structured form emits the same Identifier the text parser would.
	const TCHAR* KeywordToString(ECrowdyEffectPolicyKeyword Keyword)
	{
		switch (Keyword)
		{
		case ECrowdyEffectPolicyKeyword::Owner:       return TEXT("owner");
		case ECrowdyEffectPolicyKeyword::MyTurn:      return TEXT("my_turn");
		case ECrowdyEffectPolicyKeyword::Host:        return TEXT("host");
		case ECrowdyEffectPolicyKeyword::Participant: return TEXT("participant");
		case ECrowdyEffectPolicyKeyword::Automation:  return TEXT("automation");
		default:                                      return TEXT("owner");
		}
	}

	TSharedPtr<FCrowdyEffectExpr> MakeNull()
	{
		TSharedPtr<FCrowdyEffectExpr> Expr = MakeShared<FCrowdyEffectExpr>();
		Expr->Kind = ECrowdyEffectExprKind::NullLiteral;
		return Expr;
	}

	TSharedPtr<FCrowdyEffectExpr> MakeBinary(const FString& Op, TSharedPtr<FCrowdyEffectExpr> Lhs, TSharedPtr<FCrowdyEffectExpr> Rhs)
	{
		TSharedPtr<FCrowdyEffectExpr> Expr = MakeShared<FCrowdyEffectExpr>();
		Expr->Kind = ECrowdyEffectExprKind::Binary;
		Expr->Op = Op;
		Expr->Lhs = MoveTemp(Lhs);
		Expr->Rhs = MoveTemp(Rhs);
		return Expr;
	}

	// Precedence climbing over parallel operand/operator arrays (Operands has N, Ops has N-1: Ops[i] joins
	// Operands[i] and Operands[i+1]). Uses the SHARED CrowdyEffectBinaryPrecedence so the structured right-hand
	// side groups exactly as the text parser would. Recursion depth is bounded by the number of precedence
	// levels (a same-precedence chain builds left-associatively via the outer loop), not the term count.
	TSharedPtr<FCrowdyEffectExpr> ClimbExpr(
		const TArray<TSharedPtr<FCrowdyEffectExpr>>& Operands, const TArray<FString>& Ops, int32& Pos, int32 MinPrec)
	{
		TSharedPtr<FCrowdyEffectExpr> Left = Operands[Pos];
		++Pos;
		while (Pos - 1 < Ops.Num())
		{
			const FString& Op = Ops[Pos - 1];
			const int32 Prec = CrowdyEffectBinaryPrecedence(Op);
			if (Prec < MinPrec)
			{
				break;
			}
			TSharedPtr<FCrowdyEffectExpr> Right = ClimbExpr(Operands, Ops, Pos, Prec + 1);
			Left = MakeBinary(Op, MoveTemp(Left), MoveTemp(Right));
		}
		return Left;
	}

	// Builds one shared AST from a structured spec (used by the graph compiler's lowering path).
	struct FSpecBuilder
	{
		const FCrowdyEffectSpec& Spec;
		TArray<FCrowdyEffectDiagnostic>& Diags;

		FSpecBuilder(const FCrowdyEffectSpec& InSpec, TArray<FCrowdyEffectDiagnostic>& InDiags)
			: Spec(InSpec), Diags(InDiags)
		{
		}

		void Error(int32 Line, const FString& Message)
		{
			Diags.Add({ ECrowdyEffectSeverity::Error, Line, 0, Message });
		}

		// Parses a single sub-expression string into an AST. On any parse error it records one located error citing
		// the fragment and returns an invalid pointer, so a bad Expression / If / Call operand fails its statement
		// rather than emitting malformed DSL. The parser is itself nesting-guarded, so forged input cannot overflow.
		TSharedPtr<FCrowdyEffectExpr> ParseSub(const FString& Text, int32 Line, const FString& Where, const TCHAR* What)
		{
			TArray<FCrowdyEffectDiagnostic> LocalDiags;
			TSharedPtr<FCrowdyEffectExpr> Expr = FCrowdyEffectParser::ParseExpression(Text, LocalDiags);
			const bool bLocalError = LocalDiags.ContainsByPredicate(
				[](const FCrowdyEffectDiagnostic& D) { return D.Severity == ECrowdyEffectSeverity::Error; });
			if (!Expr.IsValid() || bLocalError)
			{
				FString Detail;
				for (const FCrowdyEffectDiagnostic& D : LocalDiags)
				{
					if (D.Severity == ECrowdyEffectSeverity::Error)
					{
						Detail = D.Message;
						break;
					}
				}
				Error(Line, FString::Printf(TEXT("%s: %s is not a valid expression ('%s')%s%s"),
					*Where, What, *Text,
					Detail.IsEmpty() ? TEXT("") : TEXT(": "), *Detail));
				return TSharedPtr<FCrowdyEffectExpr>();
			}
			return Expr;
		}

		// Builds one operand into an expression node. Returns an invalid pointer (and records a located diagnostic)
		// for any operand the text parser would reject, so the structured and text surfaces agree on validity: a
		// caller that gets a null operand skips the whole statement rather than emitting half of it.
		TSharedPtr<FCrowdyEffectExpr> BuildOperand(const FCrowdyEffectOperand& Operand, int32 Line, const FString& Where)
		{
			switch (Operand.Kind)
			{
			case ECrowdyEffectOperandKind::Number:
			{
				const FString Trimmed = Operand.Literal.TrimStartAndEnd();
				if (Trimmed.IsEmpty() || !Trimmed.IsNumeric())
				{
					Error(Line, FString::Printf(TEXT("%s: '%s' is not a valid number"), *Where, *Operand.Literal));
					return TSharedPtr<FCrowdyEffectExpr>();
				}
				TSharedPtr<FCrowdyEffectExpr> Expr = MakeShared<FCrowdyEffectExpr>();
				Expr->Kind = ECrowdyEffectExprKind::NumberLiteral;
				Expr->Text = Trimmed;
				return Expr;
			}
			case ECrowdyEffectOperandKind::Attribute:
			{
				if (Operand.Name.TrimStartAndEnd().IsEmpty())
				{
					Error(Line, FString::Printf(TEXT("%s: an attribute operand has no attribute name"), *Where));
					return TSharedPtr<FCrowdyEffectExpr>();
				}
				TSharedPtr<FCrowdyEffectExpr> Expr = MakeShared<FCrowdyEffectExpr>();
				Expr->Kind = ECrowdyEffectExprKind::PropertyAccess;
				Expr->RefBase = MapRole(Operand.Role);
				Expr->Attr = Operand.Name.TrimStartAndEnd();
				return Expr;
			}
			case ECrowdyEffectOperandKind::Magnitude:
			{
				if (Operand.Name.TrimStartAndEnd().IsEmpty())
				{
					Error(Line, FString::Printf(TEXT("%s: a magnitude operand has no $param name"), *Where));
					return TSharedPtr<FCrowdyEffectExpr>();
				}
				TSharedPtr<FCrowdyEffectExpr> Expr = MakeShared<FCrowdyEffectExpr>();
				Expr->Kind = ECrowdyEffectExprKind::Param;
				Expr->Text = Operand.Name.TrimStartAndEnd();
				return Expr;
			}
			case ECrowdyEffectOperandKind::Raw:
			{
				// Raw is a verbatim escape whose content the author owns; it is spliced unparsed (lowering treats it
				// as an opaque, lowest-precedence operand). Empty text still has to be refused: it would otherwise
				// build a perfectly valid operand that emits nothing, which reads downstream as "authored, and the
				// answer is blank" rather than "not authored yet".
				if (Operand.Literal.TrimStartAndEnd().IsEmpty())
				{
					Error(Line, FString::Printf(TEXT("%s: a raw operand has no expression text"), *Where));
					return TSharedPtr<FCrowdyEffectExpr>();
				}
				TSharedPtr<FCrowdyEffectExpr> Expr = MakeShared<FCrowdyEffectExpr>();
				Expr->Kind = ECrowdyEffectExprKind::Raw;
				Expr->Text = Operand.Literal;
				return Expr;
			}
			case ECrowdyEffectOperandKind::Expression:
			{
				if (Operand.Literal.TrimStartAndEnd().IsEmpty())
				{
					Error(Line, FString::Printf(TEXT("%s: an expression operand has no expression text"), *Where));
					return TSharedPtr<FCrowdyEffectExpr>();
				}
				return ParseSub(Operand.Literal, Line, Where, TEXT("the expression"));
			}
			case ECrowdyEffectOperandKind::If:
			{
				if (Operand.If.Condition.TrimStartAndEnd().IsEmpty())
				{
					Error(Line, FString::Printf(TEXT("%s: an if operand has no condition"), *Where));
					return TSharedPtr<FCrowdyEffectExpr>();
				}
				TSharedPtr<FCrowdyEffectExpr> Cond = ParseSub(Operand.If.Condition, Line, Where, TEXT("the if condition"));
				TSharedPtr<FCrowdyEffectExpr> Then = Operand.If.Then.TrimStartAndEnd().IsEmpty()
					? MakeNull() : ParseSub(Operand.If.Then, Line, Where, TEXT("the if 'then' value"));
				TSharedPtr<FCrowdyEffectExpr> Else = Operand.If.Else.TrimStartAndEnd().IsEmpty()
					? MakeNull() : ParseSub(Operand.If.Else, Line, Where, TEXT("the if 'else' value"));
				if (!Cond.IsValid() || !Then.IsValid() || !Else.IsValid())
				{
					return TSharedPtr<FCrowdyEffectExpr>();
				}
				TSharedPtr<FCrowdyEffectExpr> Call = MakeShared<FCrowdyEffectExpr>();
				Call->Kind = ECrowdyEffectExprKind::Call;
				Call->bIsFnCall = false;
				Call->Text = TEXT("if");
				Call->Args = { Cond, Then, Else };
				return Call;
			}
			case ECrowdyEffectOperandKind::Call:
			{
				FString Callee = Operand.Call.Callee.TrimStartAndEnd();
				if (Callee.IsEmpty())
				{
					Error(Line, FString::Printf(TEXT("%s: a call operand has no function name"), *Where));
					return TSharedPtr<FCrowdyEffectExpr>();
				}
				TSharedPtr<FCrowdyEffectExpr> Call = MakeShared<FCrowdyEffectExpr>();
				Call->Kind = ECrowdyEffectExprKind::Call;
				// A "fn:" prefix calls an authored server function; anything else is a builtin (max, min, ...).
				if (Callee.StartsWith(TEXT("fn:")))
				{
					Call->bIsFnCall = true;
					Call->Text = Callee.RightChop(3).TrimStartAndEnd();
				}
				else
				{
					Call->bIsFnCall = false;
					Call->Text = Callee;
				}
				if (Call->Text.IsEmpty())
				{
					Error(Line, FString::Printf(TEXT("%s: a call operand has no function name"), *Where));
					return TSharedPtr<FCrowdyEffectExpr>();
				}
				for (const FString& ArgText : Operand.Call.Args)
				{
					if (ArgText.TrimStartAndEnd().IsEmpty())
					{
						Error(Line, FString::Printf(TEXT("%s: call '%s' has an empty argument"), *Where, *Call->Text));
						return TSharedPtr<FCrowdyEffectExpr>();
					}
					TSharedPtr<FCrowdyEffectExpr> Arg = ParseSub(ArgText, Line, Where, TEXT("a call argument"));
					if (!Arg.IsValid())
					{
						return TSharedPtr<FCrowdyEffectExpr>();
					}
					Call->Args.Add(Arg);
				}
				return Call;
			}
			default:
				return MakeNull();
			}
		}

		FCrowdyEffectProgram Build()
		{
			FCrowdyEffectProgram Program;

			// A synthetic 1-based "line" per statement gives diagnostics a stable row reference (the structured form
			// has no real lines). Assignments come first (they run in order), then the require gates.
			int32 Line = 0;

			for (const FCrowdyEffectAssignmentSpec& Assignment : Spec.Assignments)
			{
				++Line;
				if (Assignment.Value.Num() == 0)
				{
					Error(Line, FString::Printf(TEXT("assignment %d has no value on its right-hand side"), Line));
					continue;
				}

				// Build every term; a bad operand makes the whole assignment an error, not half of one. All terms are
				// built so every problem is reported in one pass.
				const FString AssignmentWhere = FString::Printf(TEXT("assignment %d"), Line);
				TArray<TSharedPtr<FCrowdyEffectExpr>> Operands;
				TArray<FString> Ops;
				Operands.Reserve(Assignment.Value.Num());
				Ops.Reserve(Assignment.Value.Num() - 1);
				bool bOperandsValid = true;
				for (int32 Index = 0; Index < Assignment.Value.Num(); ++Index)
				{
					TSharedPtr<FCrowdyEffectExpr> Expr = BuildOperand(Assignment.Value[Index].Operand, Line, AssignmentWhere);
					if (!Expr.IsValid())
					{
						bOperandsValid = false;
					}
					Operands.Add(Expr);
					if (Index > 0)
					{
						Ops.Add(BinaryOpToString(Assignment.Value[Index].Op));
					}
				}
				if (!bOperandsValid)
				{
					continue;
				}

				int32 Pos = 0;
				TSharedPtr<FCrowdyEffectExpr> Rhs = ClimbExpr(Operands, Ops, Pos, 1);

				FCrowdyEffectStatement Statement;
				Statement.Kind = ECrowdyEffectStmtKind::Assignment;
				Statement.Line = Line;
				Statement.TargetBase = MapRole(Assignment.TargetRole);
				Statement.TargetAttr = Assignment.Attribute.TrimStartAndEnd();
				Statement.AssignOp = MapAssignOp(Assignment.Operator);
				Statement.Rhs = MoveTemp(Rhs);
				Program.Statements.Add(MoveTemp(Statement));
			}

			for (const FCrowdyEffectRequireSpec& Require : Spec.Requires)
			{
				++Line;

				FCrowdyEffectStatement Statement;
				Statement.Kind = ECrowdyEffectStmtKind::Require;
				Statement.Line = Line;

				if (Require.Kind == ECrowdyEffectRequireKind::Keyword)
				{
					TSharedPtr<FCrowdyEffectExpr> Condition = MakeShared<FCrowdyEffectExpr>();
					Condition->Kind = ECrowdyEffectExprKind::Identifier;
					Condition->Text = KeywordToString(Require.Keyword);
					Statement.Condition = MoveTemp(Condition);
				}
				else
				{
					const FString RequireWhere = FString::Printf(TEXT("require %d"), Line);
					TSharedPtr<FCrowdyEffectExpr> Left = BuildOperand(Require.Left, Line, RequireWhere);
					TSharedPtr<FCrowdyEffectExpr> Right = BuildOperand(Require.Right, Line, RequireWhere);
					if (!Left.IsValid() || !Right.IsValid())
					{
						continue;
					}
					Statement.Condition = MakeBinary(
						ComparatorToString(Require.Comparator), MoveTemp(Left), MoveTemp(Right));
				}

				Program.Statements.Add(MoveTemp(Statement));
			}

			if (Spec.bHasReturn)
			{
				++Line;
				const FString ReturnWhere = FString::Printf(TEXT("return %d"), Line);
				TSharedPtr<FCrowdyEffectExpr> ReturnExpr = BuildOperand(Spec.Return, Line, ReturnWhere);
				if (ReturnExpr.IsValid())
				{
					Program.ReturnExpr = MoveTemp(ReturnExpr);
				}
			}

			return Program;
		}
	};
}

FCrowdyEffectProgram FCrowdyEffectSpecBuilder::BuildProgram(
	const FCrowdyEffectSpec& Spec, TArray<FCrowdyEffectDiagnostic>& OutDiagnostics)
{
	FSpecBuilder Builder(Spec, OutDiagnostics);
	return Builder.Build();
}
