// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/Effect/CrowdyEffectSpecPrinter.h"

#include "Replication/GameModel/Effect/CrowdyEffectDslEscape.h"
#include "Replication/GameModel/Effect/CrowdyEffectParser.h"
#include "Replication/GameModel/Effect/CrowdyEffectSpec.h"

namespace
{
	// The printed precedence of a node, matching the lowering's own Emit precedence so parenthesization is minimal
	// and identical between the two. raw() is opaque (lowest precedence, always wrapped when combined); leaves are
	// atomic (highest).
	int32 PrintPrec(const FCrowdyEffectExpr& E)
	{
		if (E.Kind == ECrowdyEffectExprKind::Binary) return CrowdyEffectBinaryPrecedence(E.Op);
		if (E.Kind == ECrowdyEffectExprKind::Unary) return 6;
		if (E.Kind == ECrowdyEffectExprKind::Raw) return 0;
		return 100;
	}

	FString PrintNode(const TSharedPtr<FCrowdyEffectExpr>& NodePtr, int32 Depth);

	FString PrintChild(const TSharedPtr<FCrowdyEffectExpr>& Child, int32 ParentPrec, bool bIsRight, int32 Depth)
	{
		const FString S = PrintNode(Child, Depth + 1);
		const int32 P = Child.IsValid() ? PrintPrec(*Child) : 100;
		const bool bWrap = P < ParentPrec || (P == ParentPrec && bIsRight);
		return bWrap ? (TEXT("(") + S + TEXT(")")) : S;
	}

	FString PrintNode(const TSharedPtr<FCrowdyEffectExpr>& NodePtr, int32 Depth)
	{
		if (!NodePtr.IsValid())
		{
			return FString();
		}
		// A generous cap: a real effect nests only a handful deep, so this only ever trips on a pathological tree
		// and stops it printing rather than overflowing the stack.
		if (Depth > 512)
		{
			return FString();
		}
		const FCrowdyEffectExpr& N = *NodePtr;
		switch (N.Kind)
		{
		case ECrowdyEffectExprKind::NumberLiteral:
		case ECrowdyEffectExprKind::BoolLiteral:
			return N.Text;
		case ECrowdyEffectExprKind::StringLiteral:
			return FString(TEXT("\"")) + DslStringEscape(N.Text) + TEXT("\"");
		case ECrowdyEffectExprKind::NullLiteral:
			return TEXT("null");
		case ECrowdyEffectExprKind::Param:
			return TEXT("$") + N.Text;
		case ECrowdyEffectExprKind::Identifier:
			return N.Text;
		case ECrowdyEffectExprKind::PropertyAccess:
			switch (N.RefBase)
			{
			case ECrowdyEffectRefBase::SelfRef:   return TEXT("self.") + N.Attr;
			case ECrowdyEffectRefBase::SourceRef: return TEXT("source.") + N.Attr;
			default:                              return TEXT("ref(") + PrintChild(N.RefArg, 0, false, Depth) + TEXT(").") + N.Attr;
			}
		case ECrowdyEffectExprKind::Unary:
			return N.Op + PrintChild(N.Lhs, 6, false, Depth);
		case ECrowdyEffectExprKind::Binary:
		{
			const int32 P = CrowdyEffectBinaryPrecedence(N.Op);
			return PrintChild(N.Lhs, P, false, Depth) + TEXT(" ") + N.Op + TEXT(" ") + PrintChild(N.Rhs, P, true, Depth);
		}
		case ECrowdyEffectExprKind::Call:
		{
			FString Args;
			for (int32 Index = 0; Index < N.Args.Num(); ++Index)
			{
				if (Index > 0)
				{
					Args += TEXT(", ");
				}
				Args += PrintChild(N.Args[Index], 0, false, Depth);
			}
			const FString Prefix = N.bIsFnCall ? (TEXT("fn:") + N.Text) : N.Text;
			return Prefix + TEXT("(") + Args + TEXT(")");
		}
		case ECrowdyEffectExprKind::Raw:
			return N.Text;
		default:
			return FString();
		}
	}
}

FString FCrowdyEffectSpecPrinter::PrintExpression(const TSharedPtr<FCrowdyEffectExpr>& Expr)
{
	return PrintNode(Expr, 0);
}

FString FCrowdyEffectSpecPrinter::PrintOperand(const FCrowdyEffectOperand& Operand)
{
	switch (Operand.Kind)
	{
	case ECrowdyEffectOperandKind::Number:
		return Operand.Literal.TrimStartAndEnd();
	case ECrowdyEffectOperandKind::Attribute:
		return (Operand.Role == ECrowdyEffectRole::Source ? FString(TEXT("source.")) : FString(TEXT("self.")))
			+ Operand.Name.TrimStartAndEnd();
	case ECrowdyEffectOperandKind::Magnitude:
		return TEXT("$") + Operand.Name.TrimStartAndEnd();
	case ECrowdyEffectOperandKind::Raw:
		return Operand.Literal;
	case ECrowdyEffectOperandKind::Expression:
	{
		TArray<FCrowdyEffectDiagnostic> Ignored;
		const TSharedPtr<FCrowdyEffectExpr> Parsed = FCrowdyEffectParser::ParseExpression(Operand.Literal, Ignored);
		return Parsed.IsValid() ? PrintExpression(Parsed) : Operand.Literal;
	}
	case ECrowdyEffectOperandKind::If:
		return FString::Printf(TEXT("if(%s, %s, %s)"),
			*Operand.If.Condition.TrimStartAndEnd(), *Operand.If.Then.TrimStartAndEnd(), *Operand.If.Else.TrimStartAndEnd());
	case ECrowdyEffectOperandKind::Call:
	{
		FString Args;
		for (int32 Index = 0; Index < Operand.Call.Args.Num(); ++Index)
		{
			if (Index > 0)
			{
				Args += TEXT(", ");
			}
			Args += Operand.Call.Args[Index].TrimStartAndEnd();
		}
		return Operand.Call.Callee.TrimStartAndEnd() + TEXT("(") + Args + TEXT(")");
	}
	default:
		return FString();
	}
}

FString FCrowdyEffectSpecPrinter::PrintTermChain(const TArray<FCrowdyEffectTerm>& Terms)
{
	FString Out;
	for (int32 Index = 0; Index < Terms.Num(); ++Index)
	{
		if (Index > 0)
		{
			const TCHAR* Op = TEXT("+");
			switch (Terms[Index].Op)
			{
			case ECrowdyEffectBinaryOp::Add:      Op = TEXT("+"); break;
			case ECrowdyEffectBinaryOp::Subtract: Op = TEXT("-"); break;
			case ECrowdyEffectBinaryOp::Multiply: Op = TEXT("*"); break;
			case ECrowdyEffectBinaryOp::Divide:   Op = TEXT("/"); break;
			}
			Out += FString(TEXT(" ")) + Op + TEXT(" ");
		}
		Out += PrintOperand(Terms[Index].Operand);
	}
	return Out;
}
