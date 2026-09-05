// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/Effect/CrowdyEffectLowering.h"

#include "Replication/GameModel/CrowdyGameModelMetaKeys.h" // NotifyIdParam / ModelChangedChannelPrefix
#include "Replication/GameModel/Effect/CrowdyEffectDslEscape.h"

namespace
{
	// Formats a clamp bound: an integral double as an integer ("100"), otherwise a compact float. Mirrors the
	// schema sync's default canonicalization so a clamp bound and its property default read the same.
	FString FormatBound(double V)
	{
		if (FMath::IsFinite(V) && V == FMath::RoundToDouble(V) && FMath::Abs(V) < 1e15)
		{
			return FString::Printf(TEXT("%lld"), static_cast<int64>(V));
		}
		return FString::SanitizeFloat(V);
	}

	// Escapes a string for embedding as a JSON string value (the invoke-policy is a JSON string). JSON requires
	// every control char below 0x20 to be escaped, so a raw control char pasted into a string literal never
	// produces invalid JSON the server would reject.
	FString JsonEscape(const FString& In)
	{
		FString Out;
		Out.Reserve(In.Len() + 2);
		for (const TCHAR C : In)
		{
			switch (C)
			{
			case TEXT('\\'): Out += TEXT("\\\\"); break;
			case TEXT('"'): Out += TEXT("\\\""); break;
			case TEXT('\b'): Out += TEXT("\\b"); break;
			case TEXT('\f'): Out += TEXT("\\f"); break;
			case TEXT('\n'): Out += TEXT("\\n"); break;
			case TEXT('\r'): Out += TEXT("\\r"); break;
			case TEXT('\t'): Out += TEXT("\\t"); break;
			default:
				if (C < 0x20)
				{
					Out += FString::Printf(TEXT("\\u%04x"), static_cast<uint32>(C));
				}
				else
				{
					Out.AppendChar(C);
				}
				break;
			}
		}
		return Out;
	}

	// One node of the server invoke-policy tree. Hand-emitted (not FJsonObject) so key order is deterministic
	// and the produced JSON string is stable for a diff / a golden test.
	struct FPolicyNode
	{
		FString Type;
		TArray<FPolicyNode> Rules; // and / or / not
		FString Feature;           // tier_feature
		FString GroupId;           // group_permission
		FString Permission;        // group_permission (optional)
		FString Key;               // grid_permission
		FString GridId;            // grid_permission (optional)
		FString Expression;        // condition
	};

	FString EmitPolicyJson(const FPolicyNode& Node)
	{
		FString Out = TEXT("{\"type\":\"") + Node.Type + TEXT("\"");
		if (Node.Type == TEXT("and") || Node.Type == TEXT("or") || Node.Type == TEXT("not"))
		{
			Out += TEXT(",\"rules\":[");
			for (int32 Index = 0; Index < Node.Rules.Num(); ++Index)
			{
				if (Index > 0)
				{
					Out += TEXT(",");
				}
				Out += EmitPolicyJson(Node.Rules[Index]);
			}
			Out += TEXT("]");
		}
		else if (Node.Type == TEXT("tier_feature"))
		{
			Out += TEXT(",\"feature\":\"") + JsonEscape(Node.Feature) + TEXT("\"");
		}
		else if (Node.Type == TEXT("group_permission"))
		{
			Out += TEXT(",\"groupId\":\"") + JsonEscape(Node.GroupId) + TEXT("\"");
			if (!Node.Permission.IsEmpty())
			{
				Out += TEXT(",\"permission\":\"") + JsonEscape(Node.Permission) + TEXT("\"");
			}
		}
		else if (Node.Type == TEXT("grid_permission"))
		{
			Out += TEXT(",\"key\":\"") + JsonEscape(Node.Key) + TEXT("\"");
			if (!Node.GridId.IsEmpty())
			{
				Out += TEXT(",\"gridId\":\"") + JsonEscape(Node.GridId) + TEXT("\"");
			}
		}
		else if (Node.Type == TEXT("condition"))
		{
			Out += TEXT(",\"expression\":\"") + JsonEscape(Node.Expression) + TEXT("\"");
		}
		Out += TEXT("}");
		return Out;
	}

	using FExprPtr = TSharedPtr<FCrowdyEffectExpr>;

	void CollectParamRefs(const FExprPtr& NodePtr, int32 Depth, TArray<FString>& OutNames);
	void CollectRawParamMentions(const FExprPtr& NodePtr, int32 Depth, TArray<FString>& OutNames);

	// True for a numeric literal whose value is zero, in every spelling the lexer can produce ("0", "0.0", "00").
	bool IsZeroNumberLiteral(const FExprPtr& NodePtr)
	{
		return NodePtr.IsValid()
			&& NodePtr->Kind == ECrowdyEffectExprKind::NumberLiteral
			&& FCString::Atod(*NodePtr->Text) == 0.0;
	}

	// True when a run of decimal digits is larger than a signed 64-bit integer holds. Compared as text rather than
	// parsed, since the whole point is that the value fits in no integer type. The negative limit is one larger.
	bool IntegerDigitsOverflowInt64(const FString& Digits, bool bNegative)
	{
		int32 FirstSignificant = 0;
		while (FirstSignificant + 1 < Digits.Len() && Digits[FirstSignificant] == TEXT('0'))
		{
			++FirstSignificant;
		}
		const FString Significant = Digits.RightChop(FirstSignificant);
		const FString Limit = bNegative ? TEXT("9223372036854775808") : TEXT("9223372036854775807");
		if (Significant.Len() != Limit.Len())
		{
			return Significant.Len() > Limit.Len();
		}
		return Significant.Compare(Limit) > 0;
	}

	// Increments a depth counter for the life of a recursive call, decrementing on every exit path.
	struct FDepthScope
	{
		int32& Depth;
		explicit FDepthScope(int32& InDepth) : Depth(InDepth) { ++Depth; }
		~FDepthScope() { --Depth; }
	};

	// Walks an AST + a context and produces the function input. Holds the running cross-entity / source flags
	// so the default gate and source_id injection can be decided after every statement is seen.
	struct FLowerer
	{
		const FCrowdyEffectLoweringContext& Context;
		TArray<FCrowdyEffectDiagnostic>& Diags;

		bool bSourceReferenced = false; // source.<attr> used -> inject a source_id container_ref param
		bool bCrossEntity = false;      // source or an explicit ref used -> default gate is is_participant

		// Bounds the recursive walks (Emit, LowerPolicyExpr, FlattenBinary) so a deep AST cannot overflow the
		// stack. The parser caps AST size, but a deep left-nested chain from a flat expression parses fine, so
		// the lowering needs its own guard. On overflow it records one error and returns a benign value.
		int32 Depth = 0;
		bool bDepthExceeded = false;
		static constexpr int32 MaxDepth = 400;

		// How an attribute was spelled the first time the effect named it on a given base, so a second, different
		// spelling of the same attribute on the SAME base (self or source) can be reported against it. self and
		// source name attributes on different containers, so the two are policed independently: reading
		// source.Hp and writing self.hp in one effect is not a spelling conflict.
		struct FAttrSpelling
		{
			FString Text;
			int32 Line = 0;
		};
		TMap<FName, FAttrSpelling> FirstSpellingSelf;
		TMap<FName, FAttrSpelling> FirstSpellingSource;
		TSet<FName> ReportedSpellingConflictsSelf;
		TSet<FName> ReportedSpellingConflictsSource;

		FLowerer(const FCrowdyEffectLoweringContext& InContext, TArray<FCrowdyEffectDiagnostic>& InDiags)
			: Context(InContext), Diags(InDiags) {}

		// True (once) when the recursion cap is hit; records a single diagnostic. Callers return a benign value.
		bool DepthExceeded(int32 Line, int32 Col)
		{
			if (Depth <= MaxDepth)
			{
				return false;
			}
			if (!bDepthExceeded)
			{
				bDepthExceeded = true;
				Error(Line, Col, TEXT("effect expression is nested too deeply to compile"));
			}
			return true;
		}

		void Error(int32 Line, int32 Col, const FString& Message)
		{
			Diags.Add({ ECrowdyEffectSeverity::Error, Line, Col, Message });
		}

		void Warning(int32 Line, int32 Col, const FString& Message)
		{
			Diags.Add({ ECrowdyEffectSeverity::Warning, Line, Col, Message });
		}

		// True when the effect declares a source container type of its own. A container is declared by its type
		// name, so that is the gate: a source type whose whole contribution is functions has no attributes at all,
		// and reading an empty attribute list as "no source type was declared" would send its reads back to the
		// target's vocabulary, which is the mistake typing the source exists to prevent. A context built in code
		// may fill only the attribute list, so either field counts as a declaration.
		bool HasDeclaredSourceSchema() const
		{
			return !Context.SourceContainerTypeName.IsEmpty() || !Context.SourceAttributes.IsEmpty();
		}

		// The attributes a reference base is checked against: the source container type's for source.<attr> once a
		// source type is declared, the target container type's otherwise. An explicit ref names a container of
		// unknown type, so it is never resolved through here at all.
		const TArray<FCrowdyAttributeDef>& AttrsForBase(ECrowdyEffectRefBase Base) const
		{
			return (Base == ECrowdyEffectRefBase::SourceRef && HasDeclaredSourceSchema())
				? Context.SourceAttributes : Context.Attributes;
		}

		// How a message names the container a reference on this base resolves against, so an error about a source
		// attribute never names the target's type. This has to follow the SAME gate the attribute lookup follows: a
		// context that declares source attributes without a type name has no name to give, and naming the target's
		// type there would point the reader at a schema the attribute was never looked up in.
		FString ContainerDescriptionForBase(ECrowdyEffectRefBase Base) const
		{
			if (Base == ECrowdyEffectRefBase::SourceRef && HasDeclaredSourceSchema())
			{
				return Context.SourceContainerTypeName.IsEmpty()
					? FString(TEXT("the source container"))
					: FString::Printf(TEXT("container type '%s'"), *Context.SourceContainerTypeName);
			}
			return FString::Printf(TEXT("container type '%s'"), *Context.ContainerTypeName);
		}

		/**
		 * An attribute answers to two spellings and no others: the variable's name exactly as declared, and
		 * its server key. Both comparisons are case-sensitive, which is the point: the server key is
		 * lowercased, so matching without case let every casing of a name compile to the same attribute, and
		 * a script could read as though it touched two different values while touching one.
		 *
		 * The base decides which container type's attributes the name is looked up in, so a self read and a source
		 * read of the same word can resolve to two different attributes, or one of them to none at all.
		 */
		const FCrowdyAttributeDef* ResolveAttr(const FString& Name, ECrowdyEffectRefBase Base) const
		{
			return AttrsForBase(Base).FindByPredicate([&Name](const FCrowdyAttributeDef& D)
			{
				return D.PropertyName.ToString().Equals(Name, ESearchCase::CaseSensitive)
					|| D.Key.Equals(Name, ESearchCase::CaseSensitive);
			});
		}

		// The attribute a name would have reached if case were ignored, so a near miss can be named in the error.
		const FCrowdyAttributeDef* ResolveAttrIgnoringCase(const FString& Name, ECrowdyEffectRefBase Base) const
		{
			return AttrsForBase(Base).FindByPredicate([&Name](const FCrowdyAttributeDef& D)
			{
				return D.PropertyName.ToString().Equals(Name, ESearchCase::IgnoreCase)
					|| D.Key.Equals(Name, ESearchCase::IgnoreCase);
			});
		}

		/**
		 * Records how an attribute was spelled at this point in the effect on the given base, and reports the
		 * second spelling when one attribute is written two different ways on that SAME base. Both the declared
		 * name and the server key are legal on their own, but mixing them inside a single effect reads as though
		 * two separate values were involved, which is how "self.PulseIndex = self.pulseindex + 1" can look like a
		 * read of a different field. self.<attr> and source.<attr> name attributes on different containers, so
		 * they are tracked and reported separately.
		 */
		void NoteAttrSpelling(const FCrowdyAttributeDef& Def, const FString& Spelling, int32 Line, int32 Col, ECrowdyEffectRefBase Base)
		{
			const bool bSource = (Base == ECrowdyEffectRefBase::SourceRef);
			TMap<FName, FAttrSpelling>& Spellings = bSource ? FirstSpellingSource : FirstSpellingSelf;
			TSet<FName>& Reported = bSource ? ReportedSpellingConflictsSource : ReportedSpellingConflictsSelf;

			const FAttrSpelling* First = Spellings.Find(Def.PropertyName);
			if (!First)
			{
				Spellings.Add(Def.PropertyName, FAttrSpelling{ Spelling, Line });
				return;
			}
			if (First->Text.Equals(Spelling, ESearchCase::CaseSensitive)
				|| Reported.Contains(Def.PropertyName))
			{
				return;
			}
			Reported.Add(Def.PropertyName);
			// The return is lowered after every statement but may be written above one, so the recorded spelling is
			// not always the earlier one. Naming both lines keeps the advice right whichever way round they are.
			Error(Line, Col, FString::Printf(
				TEXT("'%s' (line %d) and '%s' (line %d) are two spellings of the same attribute; spell it the same way throughout the effect - either '%s' everywhere or '%s' everywhere"),
				*First->Text, First->Line, *Spelling, Line, *First->Text, *Spelling));
		}

		// Rejecting a mis-cased name is only useful if the message says what to write instead. Both the type the
		// message names and the list the suggestion is drawn from follow the base: a source read that misses is
		// reported against the source container type, and is only ever offered a near miss the source really has.
		FString DescribeUnknownAttr(const FString& Name, ECrowdyEffectRefBase Base) const
		{
			const FString Container = ContainerDescriptionForBase(Base);
			if (const FCrowdyAttributeDef* Near = ResolveAttrIgnoringCase(Name, Base))
			{
				return FString::Printf(
					TEXT("unknown attribute '%s' on %s. Attribute names are case-sensitive; did you mean '%s'?"),
					*Name, *Container, *Near->PropertyName.ToString());
			}
			return FString::Printf(
				TEXT("unknown attribute '%s' on %s"), *Name, *Container);
		}

		// The one list lives on FCrowdyEffectLowering, because the effect asset validates timer parameters against
		// exactly the same names and a second copy would be the one that goes stale.
		static bool IsReservedParamName(const FString& Name)
		{
			return FCrowdyEffectLowering::IsReservedParamName(Name);
		}

		// The grid and permission builtins take a closed set of literals in two positions: an overlap mode, and an
		// axis. The server reports a warning for a bad one at upload, which is late and easy to miss, so a literal
		// that can be checked here is checked here. A non-literal argument (a param, an attribute) is left alone,
		// since its value is only known at run time.
		void CheckGridLiteralArgs(const FCrowdyEffectExpr& Call)
		{
			struct FLiteralRule
			{
				const TCHAR* Callee;
				int32 ArgIndex;
				bool bIsAxis;
			};
			static const FLiteralRule Rules[] = {
				{ TEXT("grid_at"),              3, false },
				{ TEXT("has_chunk_permission"), 5, false },
				{ TEXT("grid_min"),             1, true  },
				{ TEXT("grid_max"),             1, true  }
			};

			for (const FLiteralRule& Rule : Rules)
			{
				if (!Call.Text.Equals(Rule.Callee, ESearchCase::IgnoreCase) || !Call.Args.IsValidIndex(Rule.ArgIndex))
				{
					continue;
				}
				const TSharedPtr<FCrowdyEffectExpr>& Arg = Call.Args[Rule.ArgIndex];
				if (!Arg.IsValid() || Arg->Kind != ECrowdyEffectExprKind::StringLiteral)
				{
					continue;
				}

				const FString Value = Arg->Text;
				const bool bValid = Rule.bIsAxis
					? (Value == TEXT("x") || Value == TEXT("y") || Value == TEXT("z"))
					: (Value == TEXT("first") || Value == TEXT("smallest") || Value == TEXT("largest"));
				if (bValid)
				{
					continue;
				}

				if (Rule.bIsAxis)
				{
					Warning(Arg->Line, Arg->Col, FString::Printf(
						TEXT("'%s' expects an axis of \"x\", \"y\", or \"z\"; \"%s\" will be rejected"),
						Rule.Callee, *Value));
				}
				else
				{
					Warning(Arg->Line, Arg->Col, FString::Printf(
						TEXT("'%s' expects a mode of \"first\", \"smallest\", or \"largest\"; \"%s\" will be rejected"),
						Rule.Callee, *Value));
				}
			}
		}

		bool IsParamDeclared(const FString& Name) const
		{
			if (IsReservedParamName(Name))
			{
				return true;
			}
			return Context.Magnitudes.ContainsByPredicate(
				[&Name](const FCrowdyEffectParamDecl& P) { return P.Name == Name; });
		}

		static int32 ExprPrec(const FCrowdyEffectExpr& E)
		{
			if (E.Kind == ECrowdyEffectExprKind::Binary) return CrowdyEffectBinaryPrecedence(E.Op);
			if (E.Kind == ECrowdyEffectExprKind::Unary) return 6;
			// raw() is an opaque operand: treat it as the lowest precedence so EmitChild parenthesizes it
			// whenever it is combined with any surrounding operator (its content may be a multi-term expression,
			// which would otherwise misgroup).
			if (E.Kind == ECrowdyEffectExprKind::Raw) return 0;
			return 100; // atomic
		}

		FString EmitChild(const FExprPtr& Child, int32 ParentPrec, bool bIsRight)
		{
			const FString S = Emit(Child);
			const int32 P = Child.IsValid() ? ExprPrec(*Child) : 100;
			const bool bWrap = P < ParentPrec || (P == ParentPrec && bIsRight);
			return bWrap ? (TEXT("(") + S + TEXT(")")) : S;
		}

		FString EmitPropertyAccess(const FCrowdyEffectExpr& N)
		{
			switch (N.RefBase)
			{
			case ECrowdyEffectRefBase::SelfRef:
			{
				if (const FCrowdyAttributeDef* A = ResolveAttr(N.Attr, ECrowdyEffectRefBase::SelfRef))
				{
					NoteAttrSpelling(*A, N.Attr, N.Line, N.Col, ECrowdyEffectRefBase::SelfRef);
					return TEXT("self.") + A->Key;
				}
				Error(N.Line, N.Col, DescribeUnknownAttr(N.Attr, ECrowdyEffectRefBase::SelfRef));
				return TEXT("self.") + N.Attr.ToLower();
			}
			case ECrowdyEffectRefBase::SourceRef:
			{
				bSourceReferenced = true;
				bCrossEntity = true;
				if (const FCrowdyAttributeDef* A = ResolveAttr(N.Attr, ECrowdyEffectRefBase::SourceRef))
				{
					NoteAttrSpelling(*A, N.Attr, N.Line, N.Col, ECrowdyEffectRefBase::SourceRef);
					return TEXT("ref($source_id).") + A->Key;
				}
				Error(N.Line, N.Col, DescribeUnknownAttr(N.Attr, ECrowdyEffectRefBase::SourceRef));
				return TEXT("ref($source_id).") + N.Attr.ToLower();
			}
			case ECrowdyEffectRefBase::ExplicitRef:
			default:
			{
				bCrossEntity = true;
				const FString ArgStr = EmitChild(N.RefArg, 0, false);
				// An explicit ref targets an unknown container type, so the attribute cannot be validated or
				// key-resolved here; it is passed through verbatim (the advanced escape hatch).
				return TEXT("ref(") + ArgStr + TEXT(").") + N.Attr;
			}
			}
		}

		FString Emit(const FExprPtr& NodePtr)
		{
			if (!NodePtr.IsValid())
			{
				return FString();
			}
			FDepthScope Scope(Depth);
			if (DepthExceeded(NodePtr->Line, NodePtr->Col))
			{
				return FString();
			}
			const FCrowdyEffectExpr& N = *NodePtr;
			switch (N.Kind)
			{
			case ECrowdyEffectExprKind::NumberLiteral:
				return N.Text;
			case ECrowdyEffectExprKind::StringLiteral:
				return FString(TEXT("\"")) + DslStringEscape(N.Text) + TEXT("\"");
			case ECrowdyEffectExprKind::BoolLiteral:
				return N.Text;
			case ECrowdyEffectExprKind::NullLiteral:
				return TEXT("null");
			case ECrowdyEffectExprKind::Param:
				if (!IsParamDeclared(N.Text))
				{
					Warning(N.Line, N.Col, FString::Printf(
						TEXT("parameter '$%s' is used but not declared as a magnitude"), *N.Text));
				}
				return TEXT("$") + N.Text;
			case ECrowdyEffectExprKind::Identifier:
				// A bareword in an arithmetic expression is meaningless (only valid in a require policy).
				Error(N.Line, N.Col, FString::Printf(
					TEXT("unexpected identifier '%s' in an expression (reads must be self.<attr>, source.<attr>, ref(...).<attr>, a $param, or a builtin call)"),
					*N.Text));
				return N.Text;
			case ECrowdyEffectExprKind::PropertyAccess:
				return EmitPropertyAccess(N);
			case ECrowdyEffectExprKind::Unary:
				return N.Op + EmitChild(N.Lhs, 6, false);
			case ECrowdyEffectExprKind::Binary:
			{
				if ((N.Op == TEXT("/") || N.Op == TEXT("%")) && IsZeroNumberLiteral(N.Rhs))
				{
					Error(N.Rhs->Line, N.Rhs->Col, FString::Printf(
						TEXT("division by zero: the right-hand side of '%s' is the literal 0"), *N.Op));
				}
				const int32 P = CrowdyEffectBinaryPrecedence(N.Op);
				return EmitChild(N.Lhs, P, false) + TEXT(" ") + N.Op + TEXT(" ") + EmitChild(N.Rhs, P, true);
			}
			case ECrowdyEffectExprKind::Call:
			{
				if (!N.bIsFnCall)
				{
					CheckGridLiteralArgs(N);
				}
				FString Args;
				for (int32 Index = 0; Index < N.Args.Num(); ++Index)
				{
					if (Index > 0)
					{
						Args += TEXT(", ");
					}
					Args += EmitChild(N.Args[Index], 0, false);
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

		/**
		 * Rejects a whole-value literal an int attribute cannot hold. Only a bare literal (optionally negated) is
		 * checked, because that is the only case whose value is known without evaluating the effect; a literal
		 * buried in arithmetic is left to the server.
		 */
		void CheckIntLiteralFits(const FExprPtr& RhsPtr, const FCrowdyAttributeDef& Target)
		{
			bool bNegative = false;
			FExprPtr Node = RhsPtr;
			while (Node.IsValid() && Node->Kind == ECrowdyEffectExprKind::Unary && Node->Op == TEXT("-"))
			{
				bNegative = !bNegative;
				Node = Node->Lhs;
			}
			if (!Node.IsValid() || Node->Kind != ECrowdyEffectExprKind::NumberLiteral
				|| Node->Text.Contains(TEXT(".")) || !IntegerDigitsOverflowInt64(Node->Text, bNegative))
			{
				return;
			}
			Error(Node->Line, Node->Col, FString::Printf(
				TEXT("the literal %s%s does not fit in the int attribute '%s' (the range is -9223372036854775808 to 9223372036854775807)"),
				bNegative ? TEXT("-") : TEXT(""), *Node->Text, *Target.Key));
		}

		void LowerAssignment(const FCrowdyEffectStatement& S, FCrowdyGameModelFunctionInput& Fn)
		{
			if (S.TargetBase == ECrowdyEffectRefBase::SourceRef)
			{
				bSourceReferenced = true;
				bCrossEntity = true;
			}

			// A write to source.<attr> is checked against the source container type's attributes when the effect
			// declares one, exactly like a read of it.
			const FCrowdyAttributeDef* Target = ResolveAttr(S.TargetAttr, S.TargetBase);
			if (!Target)
			{
				Error(S.Line, S.Col, DescribeUnknownAttr(S.TargetAttr, S.TargetBase));
				return;
			}
			NoteAttrSpelling(*Target, S.TargetAttr, S.Line, S.Col, S.TargetBase);

			const bool bArithmetic = S.AssignOp != ECrowdyEffectAssignOp::Set;
			if (bArithmetic && Target->ValueType != TEXT("int") && Target->ValueType != TEXT("float"))
			{
				Error(S.Line, S.Col, FString::Printf(
					TEXT("operator cannot apply to the %s attribute '%s'; only int/float attributes support += -= *= /="),
					*Target->ValueType, *Target->Key));
				return;
			}

			if (S.AssignOp == ECrowdyEffectAssignOp::Div && IsZeroNumberLiteral(S.Rhs))
			{
				Error(S.Rhs->Line, S.Rhs->Col, TEXT("division by zero: '/=' divides by the literal 0"));
			}
			if (Target->ValueType == TEXT("int"))
			{
				CheckIntLiteralFits(S.Rhs, *Target);
			}

			const FString TargetStr = (S.TargetBase == ECrowdyEffectRefBase::SelfRef)
				? FString(TEXT("self")) : FString(TEXT("ref($source_id)"));
			const FString LhsRead = TargetStr + TEXT(".") + Target->Key;
			const FString Rhs = Emit(S.Rhs);

			FString Inner;
			switch (S.AssignOp)
			{
			case ECrowdyEffectAssignOp::Set: Inner = Rhs; break;
			case ECrowdyEffectAssignOp::Add: Inner = LhsRead + TEXT(" + (") + Rhs + TEXT(")"); break;
			case ECrowdyEffectAssignOp::Sub: Inner = LhsRead + TEXT(" - (") + Rhs + TEXT(")"); break;
			case ECrowdyEffectAssignOp::Mul: Inner = LhsRead + TEXT(" * (") + Rhs + TEXT(")"); break;
			case ECrowdyEffectAssignOp::Div: Inner = LhsRead + TEXT(" / (") + Rhs + TEXT(")"); break;
			}

			FString Expression = Inner;
			if (Target->bHasClamp)
			{
				Expression = TEXT("max(") + FormatBound(Target->ClampMin) + TEXT(", min(")
					+ FormatBound(Target->ClampMax) + TEXT(", ") + Inner + TEXT("))");
			}

			FCrowdyGameModelMutation Mut;
			Mut.Target = TargetStr;
			Mut.Property = Target->Key;
			Mut.Expression = Expression;
			Fn.Mutations.Add(MoveTemp(Mut));
		}

		FString ArgLiteral(const FCrowdyEffectExpr& Call, int32 Index, const TCHAR* What)
		{
			if (!Call.Args.IsValidIndex(Index) || !Call.Args[Index].IsValid())
			{
				Error(Call.Line, Call.Col, FString::Printf(
					TEXT("requirement '%s' expects %s as argument %d"), *Call.Text, What, Index + 1));
				return FString();
			}
			const FCrowdyEffectExpr& Arg = *Call.Args[Index];
			if (Arg.Kind == ECrowdyEffectExprKind::StringLiteral || Arg.Kind == ECrowdyEffectExprKind::NumberLiteral)
			{
				return Arg.Text;
			}
			Error(Arg.Line, Arg.Col, FString::Printf(
				TEXT("requirement '%s' expects a literal for %s"), *Call.Text, What));
			return FString();
		}

		bool TryPolicyCall(const FCrowdyEffectExpr& Call, FPolicyNode& Out)
		{
			const FString& Name = Call.Text;
			if (Name == TEXT("feature") || Name == TEXT("tier_feature"))
			{
				Out.Type = TEXT("tier_feature");
				Out.Feature = ArgLiteral(Call, 0, TEXT("a feature key"));
				return true;
			}
			if (Name == TEXT("grid_permission"))
			{
				Out.Type = TEXT("grid_permission");
				Out.Key = ArgLiteral(Call, 0, TEXT("a permission key"));
				if (Call.Args.Num() >= 2)
				{
					Out.GridId = ArgLiteral(Call, 1, TEXT("a grid id"));
				}
				return true;
			}
			if (Name == TEXT("group_permission"))
			{
				Out.Type = TEXT("group_permission");
				Out.GroupId = ArgLiteral(Call, 0, TEXT("a group id"));
				if (Call.Args.Num() >= 2)
				{
					Out.Permission = ArgLiteral(Call, 1, TEXT("a permission"));
				}
				return true;
			}
			return false;
		}

		FPolicyNode KeywordLeaf(const FCrowdyEffectExpr& E)
		{
			FPolicyNode N;
			const FString& K = E.Text;
			if (K == TEXT("owner") || K == TEXT("owner_of_self")) { N.Type = TEXT("owner_of_self"); }
			else if (K == TEXT("my_turn") || K == TEXT("is_current_turn")) { N.Type = TEXT("is_current_turn"); }
			else if (K == TEXT("host") || K == TEXT("is_host")) { N.Type = TEXT("is_host"); }
			else if (K == TEXT("participant") || K == TEXT("is_participant")) { N.Type = TEXT("is_participant"); }
			else if (K == TEXT("automation") || K == TEXT("is_automation")) { N.Type = TEXT("is_automation"); }
			else
			{
				Error(E.Line, E.Col, FString::Printf(TEXT("unknown requirement '%s'"), *K));
				N.Type = TEXT("condition");
				N.Expression = K;
			}
			return N;
		}

		void FlattenBinary(const FExprPtr& NodePtr, const TCHAR* Op, TArray<FPolicyNode>& Out)
		{
			FDepthScope Scope(Depth);
			if (DepthExceeded(NodePtr.IsValid() ? NodePtr->Line : 0, NodePtr.IsValid() ? NodePtr->Col : 0))
			{
				return;
			}
			if (NodePtr.IsValid() && NodePtr->Kind == ECrowdyEffectExprKind::Binary && NodePtr->Op == Op)
			{
				FlattenBinary(NodePtr->Lhs, Op, Out);
				FlattenBinary(NodePtr->Rhs, Op, Out);
			}
			else
			{
				Out.Add(LowerPolicyExpr(NodePtr));
			}
		}

		FPolicyNode LowerPolicyExpr(const FExprPtr& NodePtr)
		{
			FDepthScope Scope(Depth);
			if (!NodePtr.IsValid() || DepthExceeded(NodePtr->Line, NodePtr->Col))
			{
				FPolicyNode N;
				N.Type = TEXT("condition");
				return N;
			}
			const FCrowdyEffectExpr& E = *NodePtr;

			if (E.Kind == ECrowdyEffectExprKind::Binary && E.Op == TEXT("&&"))
			{
				FPolicyNode N;
				N.Type = TEXT("and");
				FlattenBinary(NodePtr, TEXT("&&"), N.Rules);
				return N;
			}
			if (E.Kind == ECrowdyEffectExprKind::Binary && E.Op == TEXT("||"))
			{
				FPolicyNode N;
				N.Type = TEXT("or");
				FlattenBinary(NodePtr, TEXT("||"), N.Rules);
				return N;
			}
			if (E.Kind == ECrowdyEffectExprKind::Unary && E.Op == TEXT("!"))
			{
				FPolicyNode N;
				N.Type = TEXT("not");
				N.Rules.Add(LowerPolicyExpr(E.Lhs));
				return N;
			}
			if (E.Kind == ECrowdyEffectExprKind::Identifier)
			{
				return KeywordLeaf(E);
			}
			if (E.Kind == ECrowdyEffectExprKind::Call && !E.bIsFnCall)
			{
				FPolicyNode Leaf;
				if (TryPolicyCall(E, Leaf))
				{
					return Leaf;
				}
			}

			// Anything else (a comparison, an arithmetic/boolean expression, a builtin call) is a condition.
			FPolicyNode N;
			N.Type = TEXT("condition");
			N.Expression = Emit(NodePtr);
			return N;
		}

		/**
		 * True when an expression could read the given attribute of the given base. Anything opaque counts as a
		 * read: an explicit ref may point at the same container, a raw(...) escape is never parsed, and a model
		 * function is free to read whatever it likes. Being generous here only ever suppresses a warning.
		 */
		bool ExprObservesAttr(
			const FExprPtr& NodePtr, int32 WalkDepth, ECrowdyEffectRefBase Base, const FCrowdyAttributeDef& Attr) const
		{
			if (!NodePtr.IsValid())
			{
				return false;
			}
			if (WalkDepth > MaxDepth)
			{
				return true;
			}
			const FCrowdyEffectExpr& N = *NodePtr;
			if (N.Kind == ECrowdyEffectExprKind::Raw
				|| (N.Kind == ECrowdyEffectExprKind::Call && N.bIsFnCall)
				|| (N.Kind == ECrowdyEffectExprKind::PropertyAccess
					&& (N.RefBase == ECrowdyEffectRefBase::ExplicitRef
						|| (N.RefBase == Base && ResolveAttr(N.Attr, Base) == &Attr))))
			{
				return true;
			}
			if (ExprObservesAttr(N.Lhs, WalkDepth + 1, Base, Attr)
				|| ExprObservesAttr(N.Rhs, WalkDepth + 1, Base, Attr)
				|| ExprObservesAttr(N.RefArg, WalkDepth + 1, Base, Attr))
			{
				return true;
			}
			for (const FExprPtr& Arg : N.Args)
			{
				if (ExprObservesAttr(Arg, WalkDepth + 1, Base, Attr))
				{
					return true;
				}
			}
			return false;
		}

		// The attribute a bare self.<attr> / source.<attr> read names, or null for anything else. Used to type a
		// return the author did not declare a type for, so it resolves on the read's own base: a returned source
		// attribute is typed by the source container type's declaration of it.
		const FCrowdyAttributeDef* BareAttrRead(const FExprPtr& NodePtr) const
		{
			if (!NodePtr.IsValid() || NodePtr->Kind != ECrowdyEffectExprKind::PropertyAccess
				|| NodePtr->RefBase == ECrowdyEffectRefBase::ExplicitRef)
			{
				return nullptr;
			}
			return ResolveAttr(NodePtr->Attr, NodePtr->RefBase);
		}

		// The types a return may be declared as. An aggregate attribute has no return-type spelling, so returning
		// one leaves the type undeclared.
		static bool IsDeclarableReturnType(const FString& Type)
		{
			return Type == TEXT("int") || Type == TEXT("float") || Type == TEXT("bool") || Type == TEXT("string");
		}

		/**
		 * The type to declare for a return. An explicit declaration always wins. Otherwise a bare attribute read
		 * answers with the attribute's own type, which covers the common case of returning what the effect just
		 * wrote; anything else is left undeclared with a warning, since the server treats returnType as optional
		 * but a caller cannot decode an untyped value into a typed pin.
		 */
		FString ResolveReturnType(const FExprPtr& ReturnExpr)
		{
			const int32 Line = ReturnExpr.IsValid() ? ReturnExpr->Line : 0;
			const int32 Col = ReturnExpr.IsValid() ? ReturnExpr->Col : 0;
			const FCrowdyAttributeDef* Attr = BareAttrRead(ReturnExpr);

			if (!Context.ReturnType.IsEmpty())
			{
				if (!IsDeclarableReturnType(Context.ReturnType))
				{
					Error(Line, Col, FString::Printf(
						TEXT("'%s' is not a return type; use int, float, bool, or string"), *Context.ReturnType));
					return FString();
				}
				// Widening an int answer to float is a deliberate choice an author may want; any other disagreement
				// between the declaration and the attribute being returned decodes wrongly at the caller.
				if (Attr && IsDeclarableReturnType(Attr->ValueType) && Attr->ValueType != Context.ReturnType
					&& !(Attr->ValueType == TEXT("int") && Context.ReturnType == TEXT("float")))
				{
					Warning(Line, Col, FString::Printf(
						TEXT("this effect declares a return type of '%s' but returns '%s', which is %s; the caller "
						"decodes the value as the declared type"),
						*Context.ReturnType, *Attr->Key, *Attr->ValueType));
				}
				return Context.ReturnType;
			}

			if (Attr && IsDeclarableReturnType(Attr->ValueType))
			{
				return Attr->ValueType;
			}
			if (ReturnExpr.IsValid() && ReturnExpr->Kind == ECrowdyEffectExprKind::PropertyAccess
				&& ReturnExpr->RefBase != ECrowdyEffectRefBase::ExplicitRef && !Attr)
			{
				// The attribute could not be resolved at all, which the unknown-attribute error already reports.
				// Adding "declare a type or return a single attribute" here would point at the wrong fix.
				return FString();
			}
			Warning(Line, Col,
				TEXT("this effect returns a value but declares no return type, so the value arrives untyped; set the "
				"effect's Return Type, or return a single attribute so its type can be read off the attribute"));
			return FString();
		}

		static bool IsGridBuiltin(const FString& Callee)
		{
			return Callee.Equals(TEXT("grid_at"), ESearchCase::IgnoreCase)
				|| Callee.Equals(TEXT("grid_contains"), ESearchCase::IgnoreCase)
				|| Callee.Equals(TEXT("grid_min"), ESearchCase::IgnoreCase)
				|| Callee.Equals(TEXT("grid_max"), ESearchCase::IgnoreCase);
		}

		/**
		 * A raw(...) escape is spliced through unparsed, so a non-public read inside one would otherwise leave the
		 * server with no signal at all. The text is scanned for each non-public attribute's key the same way
		 * magnitude mentions are scanned out of it, and the warning says the text was not parsed so a mention that
		 * is only incidental reads as the false positive it is.
		 */
		void WarnOnNonPublicRawText(const FCrowdyEffectExpr& N)
		{
			for (const FCrowdyAttributeDef& Attr : Context.Attributes)
			{
				if (Attr.Visibility == TEXT("public"))
				{
					continue;
				}
				if (N.Text.Contains(TEXT(".") + Attr.Key, ESearchCase::CaseSensitive))
				{
					Warning(N.Line, N.Col, FString::Printf(
						TEXT("the returned value contains a raw expression mentioning '%s', which is %s-visible; the "
						"raw text is not parsed, so check whether it hands that value to every caller"),
						*Attr.Key, *Attr.Visibility));
				}
			}
		}

		/**
		 * Warns when a return hands out an attribute the server would not have shown the caller. Read visibility is
		 * enforced when a container's state is pulled, and a return expression is not that path: an owner or hidden
		 * attribute reaching a caller who may invoke is a disclosure the author is the only one able to judge.
		 * Reported once per attribute.
		 */
		void WarnOnNonPublicReturnReads(const FExprPtr& NodePtr, int32 WalkDepth, TSet<FName>& Reported)
		{
			if (!NodePtr.IsValid() || WalkDepth > MaxDepth)
			{
				return;
			}
			const FCrowdyEffectExpr& N = *NodePtr;
			if (N.Kind == ECrowdyEffectExprKind::PropertyAccess)
			{
				// An explicit ref names a container whose type is unknown here, so the visibility shown is this
				// effect's own type's. It is still worth saying: ref($source_id).<attr> is the exact form a
				// source.<attr> read lowers to, so skipping it would let the same disclosure through unremarked.
				if (const FCrowdyAttributeDef* Attr = ResolveAttr(N.Attr, N.RefBase))
				{
					if (Attr->Visibility != TEXT("public") && !Reported.Contains(Attr->PropertyName))
					{
						Reported.Add(Attr->PropertyName);
						const TCHAR* Qualifier = (N.RefBase == ECrowdyEffectRefBase::ExplicitRef)
							? TEXT(" (read through a ref, so the visibility shown is this container type's)")
							: TEXT("");
						Warning(N.Line, N.Col, FString::Printf(
							TEXT("the returned value reads '%s', which is %s-visible, so anyone allowed to invoke this "
							"effect learns it even though a state read would not show it%s"),
							*Attr->Key, *Attr->Visibility, Qualifier));
					}
				}
			}
			else if (N.Kind == ECrowdyEffectExprKind::Raw)
			{
				WarnOnNonPublicRawText(N);
			}
			else if (N.Kind == ECrowdyEffectExprKind::Call && !N.bIsFnCall && IsGridBuiltin(N.Text))
			{
				Warning(N.Line, N.Col, FString::Printf(
					TEXT("the returned value calls '%s', which reads grid state; grid queries are otherwise "
					"administrative, so returning one hands that state to anyone allowed to invoke this effect"),
					*N.Text));
			}
			WarnOnNonPublicReturnReads(N.Lhs, WalkDepth + 1, Reported);
			WarnOnNonPublicReturnReads(N.Rhs, WalkDepth + 1, Reported);
			WarnOnNonPublicReturnReads(N.RefArg, WalkDepth + 1, Reported);
			for (const FExprPtr& Arg : N.Args)
			{
				WarnOnNonPublicReturnReads(Arg, WalkDepth + 1, Reported);
			}
		}

		// Lowers the program's optional return onto the function, and reports the two ways a scope and a return can
		// contradict each other.
		void LowerReturn(const FCrowdyEffectProgram& Program, FCrowdyGameModelFunctionInput& Fn)
		{
			if (Program.ReturnExpr.IsValid())
			{
				// A return is a pure read the server evaluates once every mutation has run, so unlike an assignment
				// it is never clamp-wrapped: nothing is being written for a bound to apply to.
				Fn.ReturnExpression = Emit(Program.ReturnExpr);
				Fn.ReturnType = ResolveReturnType(Program.ReturnExpr);
				TSet<FName> ReportedVisibility;
				WarnOnNonPublicReturnReads(Program.ReturnExpr, 0, ReportedVisibility);
				return;
			}

			// Every diagnostic below is about a return the author meant to write, so each one ends by naming where
			// this surface writes it.
			const FString Hint = Context.ReturnAuthoringHint.IsEmpty()
				? FString() : (TEXT("; ") + Context.ReturnAuthoringHint);

			if (!Context.ReturnType.IsEmpty())
			{
				Warning(0, 0, FString::Printf(
					TEXT("this effect declares a return type of '%s' but returns nothing, so every invocation answers "
					"with no value%s"), *Context.ReturnType, *Hint));
			}
			// An internal function has no direct entry point by design, so with neither a return nor an automation
			// to run it, nothing can reach it and it answers nothing. Automation-invocable is the case the rule must
			// not catch: a trusted server-side function is reached by its automation, not by a fn: call.
			if (Context.InvokeScope == TEXT("internal"))
			{
				if (!Context.bAutonomousInvocable)
				{
					Error(0, 0, FString::Printf(
						TEXT("this effect is callable only from other effects but returns nothing, so nothing can "
						"reach it: give it a return value, or let players or automations call it%s"), *Hint));
				}
				else
				{
					// The documented meaning of the internal scope is "reachable only through a fn: call", and every
					// documented automation entry point uses the server scope instead. The pairing ships in practice,
					// so it is not refused, but an author should hear that their automation may not reach it.
					Warning(0, 0, TEXT("this effect runs automatically but is callable only from other effects; the "
						"documented scope for an automation entry point is Server only, so check that the automation "
						"actually runs before relying on it"));
				}
			}
		}

		/**
		 * Warns about a write that a later plain '=' replaces before anything can read it, which makes the earlier
		 * write do nothing at all. Only that shape is reported: writes run in order and each sees the previous
		 * result, so repeated compound writes accumulate and are perfectly meaningful. A return never counts as a
		 * read here: the server evaluates it after every mutation, so it observes the final value and cannot
		 * rescue a write that was already replaced.
		 */
		void CheckOverwrittenWrites(const FCrowdyEffectProgram& Program)
		{
			const TArray<FCrowdyEffectStatement>& Statements = Program.Statements;
			for (int32 Later = 1; Later < Statements.Num(); ++Later)
			{
				const FCrowdyEffectStatement& Overwrite = Statements[Later];
				if (Overwrite.Kind != ECrowdyEffectStmtKind::Assignment
					|| Overwrite.AssignOp != ECrowdyEffectAssignOp::Set)
				{
					continue;
				}
				const FCrowdyAttributeDef* Attr = ResolveAttr(Overwrite.TargetAttr, Overwrite.TargetBase);
				if (!Attr)
				{
					continue;
				}

				int32 Earlier = INDEX_NONE;
				for (int32 Index = Later - 1; Index >= 0 && Earlier == INDEX_NONE; --Index)
				{
					const FCrowdyEffectStatement& Candidate = Statements[Index];
					if (Candidate.Kind == ECrowdyEffectStmtKind::Assignment
						&& Candidate.TargetBase == Overwrite.TargetBase
						&& ResolveAttr(Candidate.TargetAttr, Candidate.TargetBase) == Attr)
					{
						Earlier = Index;
					}
				}
				if (Earlier == INDEX_NONE)
				{
					continue;
				}

				bool bObserved = ExprObservesAttr(Overwrite.Rhs, 0, Overwrite.TargetBase, *Attr);
				for (int32 Index = Earlier + 1; Index < Later && !bObserved; ++Index)
				{
					const FCrowdyEffectStatement& Between = Statements[Index];
					if (Between.Kind != ECrowdyEffectStmtKind::Assignment)
					{
						// A require folds into the invoke policy and is evaluated by the server before any
						// mutation runs, so its position between two writes carries no meaning: it never
						// observes an attribute for the purpose of this scan.
						continue;
					}
					// A compound write to the same attribute reads it before it writes it.
					bObserved = (Between.AssignOp != ECrowdyEffectAssignOp::Set
							&& Between.TargetBase == Overwrite.TargetBase
							&& ResolveAttr(Between.TargetAttr, Between.TargetBase) == Attr)
						|| ExprObservesAttr(Between.Rhs, 0, Overwrite.TargetBase, *Attr);
				}
				if (!bObserved)
				{
					Warning(Overwrite.Line, Overwrite.Col, FString::Printf(
						TEXT("'%s' is written on line %d and overwritten here before anything reads it, so the earlier write has no effect"),
						*Attr->Key, Statements[Earlier].Line));
				}
			}
		}

		/**
		 * Warns about a fn: call whose callee the project knows about but which cannot do what the call implies:
		 * one that authors no return has nothing for the call to read, and one that writes state does not write it
		 * here, since a fn: call reads a return value and runs no mutations. A callee can be both at once, and each
		 * is worth its own line. Walks the same shape WarnOnNonPublicReturnReads does (Lhs, Rhs, RefArg, every Arg),
		 * but over the whole program rather than only the return expression, since a fn: call can appear in a
		 * require condition, in a mutation's right-hand side at any depth, or in the return expression itself.
		 * A tree walk visits each authored call site exactly once, since the AST holds no shared subtrees.
		 */
		void CheckFnCalleeUsage(const FExprPtr& NodePtr, int32 WalkDepth)
		{
			if (!NodePtr.IsValid() || WalkDepth > MaxDepth)
			{
				return;
			}
			const FCrowdyEffectExpr& N = *NodePtr;
			if (N.Kind == ECrowdyEffectExprKind::Call && N.bIsFnCall)
			{
				FCrowdyEffectFnCallee Callee;
				// A lookup that returns false means the name is unknown to the project, which is not itself a
				// problem: a fn: call may legitimately name a hand-authored server function or a kit function
				// that is not a Crowdy Effect asset here. Only a known callee is judged.
				//
				// Everything a callee lookup can produce MUST stay Warning severity. Several callers compile with
				// no lookup at all (ECrowdyEffectFnCatalog::None) on the guarantee that this changes advisory
				// warnings only; an Error emitted here would make those compiles accept an effect the authoring
				// surfaces reject.
				if (Context.FnCalleeLookup(N.Text, Callee))
				{
					if (!Callee.bAuthorsReturn)
					{
						Warning(N.Line, N.Col, FString::Printf(
							TEXT("fn:%s(...) calls a function this project knows about, but it returns nothing, so this call has no value to read; give '%s' a return value"),
							*N.Text, *N.Text));
					}
					// Worth saying even when the callee does return a value, because nothing at the call site
					// reveals it: a fn: call reads that return value and runs none of the callee's writes.
					if (Callee.bHasMutations)
					{
						Warning(N.Line, N.Col, FString::Printf(
							TEXT("fn:%s(...) reads that function's return value only, so the state it writes is not changed here; apply it as an effect of its own if you need its writes to run"),
							*N.Text));
					}
				}
			}
			CheckFnCalleeUsage(N.Lhs, WalkDepth + 1);
			CheckFnCalleeUsage(N.Rhs, WalkDepth + 1);
			CheckFnCalleeUsage(N.RefArg, WalkDepth + 1);
			for (const FExprPtr& Arg : N.Args)
			{
				CheckFnCalleeUsage(Arg, WalkDepth + 1);
			}
		}

		// Runs CheckFnCalleeUsage over every expression position a fn: call can occupy: every statement's
		// right-hand side, every require's condition, and the return expression. An unset FnCalleeLookup means no
		// catalog is available at all (no editor, a cooked build, a bare unit test), so this emits nothing rather
		// than treating "unknown" as "known and returnless".
		void CheckFnCalleeCalls(const FCrowdyEffectProgram& Program)
		{
			if (!Context.FnCalleeLookup)
			{
				return;
			}
			for (const FCrowdyEffectStatement& S : Program.Statements)
			{
				CheckFnCalleeUsage(S.Rhs, 0);
				CheckFnCalleeUsage(S.Condition, 0);
			}
			CheckFnCalleeUsage(Program.ReturnExpr, 0);
		}

		FCrowdyGameModelFunctionInput Run(const FCrowdyEffectProgram& Program)
		{
			FCrowdyGameModelFunctionInput Fn;
			Fn.Name = Context.FunctionName;
			Fn.ContainerTypeName = Context.ContainerTypeName;
			Fn.Description = Context.Description;
			Fn.bAutonomousInvocable = Context.bAutonomousInvocable;
			if (Context.InvokeScope == TEXT("player") || Context.InvokeScope == TEXT("server")
				|| Context.InvokeScope == TEXT("internal"))
			{
				Fn.InvokeScope = Context.InvokeScope;
			}
			else
			{
				Error(0, 0, FString::Printf(
					TEXT("'%s' is not an invoke scope; use player, server, or internal"), *Context.InvokeScope));
				Fn.InvokeScope = TEXT("player");
			}

			// A declared source container type that resolves to nothing leaves the effect with no schema to check its
			// source reads against. Checking them against the target's attributes instead would accept names the
			// source does not have, and the mistake would surface only as a failed function on the server, so the
			// declaration is refused here. Like the invoke scope it is a setting rather than a line of the body, so
			// it carries no source position.
			if (Context.bSourceContainerTypeUnresolved)
			{
				Error(0, 0, FString::Printf(
					TEXT("the source container type '%s' is not a known Game Model container type, so nothing here can "
					"check what the source has; pick a container type that exists, or set it back to 'Same as target' to "
					"treat the source as another container of this effect's own type"), *Context.SourceContainerTypeName));
			}

			TArray<FPolicyNode> RequireNodes;
			for (const FCrowdyEffectStatement& S : Program.Statements)
			{
				if (S.Kind == ECrowdyEffectStmtKind::Assignment)
				{
					LowerAssignment(S, Fn);
				}
				else
				{
					RequireNodes.Add(LowerPolicyExpr(S.Condition));
				}
			}

			// Before the parameter and policy blocks below: a return that reads source.<attr> is what makes the
			// effect cross-entity, so it has to be seen before the injected source_id param and the default gate
			// are decided.
			LowerReturn(Program, Fn);

			CheckOverwrittenWrites(Program);
			CheckFnCalleeCalls(Program);

			// Every $param the effect actually mentions, including one that appears only inside a raw(...) escape,
			// whose text the lowering otherwise never looks inside.
			TArray<FString> ReferencedParams;
			for (const FCrowdyEffectStatement& S : Program.Statements)
			{
				CollectParamRefs(S.Rhs, 0, ReferencedParams);
				CollectParamRefs(S.Condition, 0, ReferencedParams);
				CollectRawParamMentions(S.Rhs, 0, ReferencedParams);
				CollectRawParamMentions(S.Condition, 0, ReferencedParams);
			}
			CollectParamRefs(Program.ReturnExpr, 0, ReferencedParams);
			CollectRawParamMentions(Program.ReturnExpr, 0, ReferencedParams);

			// Parameters: declared magnitudes in order, then an injected source_id when the effect is cross-entity.
			int32 SortOrder = 0;
			for (const FCrowdyEffectParamDecl& M : Context.Magnitudes)
			{
				if (IsReservedParamName(M.Name))
				{
					Error(0, 0, FString::Printf(
						TEXT("the magnitude name '%s' is reserved by the effect layer and cannot be declared"), *M.Name));
					continue;
				}
				if (!ReferencedParams.Contains(M.Name))
				{
					// A magnitude has no line of its own, so this diagnostic carries no source position.
					Warning(0, 0, FString::Printf(
						TEXT("the magnitude '%s' is declared but never used in this effect; it is still emitted as a function parameter callers have to supply"),
						*M.Name));
				}
				FCrowdyGameModelFunctionParam P;
				P.Name = M.Name;
				P.ValueType = M.ValueType;
				P.DefaultValueJson = M.DefaultValueJson;
				P.Description = M.Description;
				P.bRequired = M.DefaultValueJson.IsEmpty();
				P.SortOrder = SortOrder++;
				Fn.Parameters.Add(MoveTemp(P));
			}
			if (bSourceReferenced)
			{
				FCrowdyGameModelFunctionParam P;
				P.Name = TEXT("source_id");
				P.ValueType = TEXT("container_ref");
				P.bRequired = true;
				P.SortOrder = SortOrder++;
				Fn.Parameters.Add(MoveTemp(P));
			}

			// Model-driven notification: when a carrier is selected, author the notification naming the changed
			// container so peers re-pull without the acting client sending a separate ping, and so automation-driven
			// changes (no acting client) notify identically. The id comes from the server-injected $self_container_id
			// system param (Game API v0.20.0), evaluated per invocation, so nothing needs to be passed in: a player
			// invoke and an automation run over N containers each name their own container.
			//
			// A function that writes nothing cannot have changed the model, so it authors no notification: otherwise
			// a question ("can I afford this") would make every peer re-pull a container that did not move.
			if (Context.NotificationCarrier != ECrowdyModelNotificationCarrier::None && !Fn.Mutations.IsEmpty())
			{
				const FString SelfIdRef = FString::Printf(TEXT("$%s"), CrowdyGameModelMetaKeys::SelfContainerIdParam);
				FCrowdyGameModelNotification Notif;
				if (Context.NotificationCarrier == ECrowdyModelNotificationCarrier::Channel)
				{
					// Reaches every default-session-channel member regardless of position. The destination arg is
					// filled by the schema sync, which names the app's session channel rather than resolving it
					// here. payload = concat(prefix, $self_container_id): the
					// client checks the prefix and decodes the container id after it. $self_container_id is already a
					// string, so no to_string cast is needed.
					Notif.Kind = TEXT("channel");
					FCrowdyGameModelNotificationArg PayloadArg;
					PayloadArg.Name = TEXT("payload");
					PayloadArg.Expression = FString::Printf(TEXT("concat(\"%s\", %s)"),
						CrowdyGameModelMetaKeys::ModelChangedChannelPrefix, *SelfIdRef);
					Notif.Args.Add(MoveTemp(PayloadArg));
				}
				else // Spatial: proximity fan-out (opcode 139); the existing SERVER_EVENT receive path decodes state.
				{
					// state carries the container id; event_type marks it model-changed. chunk_x/y/z are filled by
					// the schema sync from the container's position attributes when present (a container with no
					// position cannot target spatially - use Channel).
					Notif.Kind = TEXT("spatial");
					Notif.EmitAs = TEXT("server_event");
					FCrowdyGameModelNotificationArg EventTypeArg;
					EventTypeArg.Name = TEXT("event_type");
					EventTypeArg.Expression = FString::FromInt(static_cast<int32>(CrowdyGameModelMetaKeys::ModelChangedEventType));
					Notif.Args.Add(MoveTemp(EventTypeArg));
					FCrowdyGameModelNotificationArg StateArg;
					StateArg.Name = TEXT("state");
					StateArg.Expression = SelfIdRef;
					Notif.Args.Add(MoveTemp(StateArg));
				}
				Fn.Notifications.Add(MoveTemp(Notif));
			}

			// Invoke policy: explicit requires (and-combined) override the inferred default gate. A pure internal
			// helper has no caller to gate, so it gets no policy unless the author wrote one; inferring
			// owner_of_self there would describe a caller that cannot exist. An internal function an automation may
			// also run does have a caller, and is exactly the shape a trusted server-side grant takes, so it gets
			// is_automation rather than nothing: the alternative sends an explicit null that clears whatever policy
			// the server function already carried.
			FPolicyNode Root;
			if (RequireNodes.Num() == 1)
			{
				Root = RequireNodes[0];
			}
			else if (RequireNodes.Num() > 1)
			{
				Root.Type = TEXT("and");
				Root.Rules = MoveTemp(RequireNodes);
			}
			else if (Fn.InvokeScope != TEXT("internal"))
			{
				Root.Type = bCrossEntity ? TEXT("is_participant") : TEXT("owner_of_self");
			}
			else if (Context.bAutonomousInvocable)
			{
				Root.Type = TEXT("is_automation");
			}
			if (!Root.Type.IsEmpty())
			{
				Fn.InvokePolicyJson = EmitPolicyJson(Root);
			}

			return Fn;
		}
	};

	// Collects every $param an expression subtree references, de-duplicated in first-seen order. Bounded by the
	// same depth cap the lowering uses on its own walks: a flat expression can still parse into a deep left-nested
	// chain, so an unguarded recursion could overflow the stack.
	void CollectParamRefs(const FExprPtr& NodePtr, int32 Depth, TArray<FString>& OutNames)
	{
		if (!NodePtr.IsValid() || Depth > FLowerer::MaxDepth)
		{
			return;
		}
		const FCrowdyEffectExpr& N = *NodePtr;
		if (N.Kind == ECrowdyEffectExprKind::Param)
		{
			OutNames.AddUnique(N.Text);
		}
		CollectParamRefs(N.Lhs, Depth + 1, OutNames);
		CollectParamRefs(N.Rhs, Depth + 1, OutNames);
		CollectParamRefs(N.RefArg, Depth + 1, OutNames);
		for (const TSharedPtr<FCrowdyEffectExpr>& Arg : N.Args)
		{
			CollectParamRefs(Arg, Depth + 1, OutNames);
		}
	}

	// Adds every "$name" mentioned inside a raw(...) escape's verbatim text. That text is spliced through unparsed,
	// so a magnitude used only there would otherwise look unused. Bounded by the same depth cap as the other walks.
	void CollectRawParamMentions(const FExprPtr& NodePtr, int32 Depth, TArray<FString>& OutNames)
	{
		if (!NodePtr.IsValid() || Depth > FLowerer::MaxDepth)
		{
			return;
		}
		const FCrowdyEffectExpr& N = *NodePtr;
		if (N.Kind == ECrowdyEffectExprKind::Raw)
		{
			for (int32 Index = 0; Index < N.Text.Len(); ++Index)
			{
				if (N.Text[Index] != TEXT('$'))
				{
					continue;
				}
				FString Name;
				for (int32 Scan = Index + 1;
					Scan < N.Text.Len() && (FChar::IsAlnum(N.Text[Scan]) || N.Text[Scan] == TEXT('_'));
					++Scan)
				{
					Name.AppendChar(N.Text[Scan]);
				}
				if (!Name.IsEmpty() && !FChar::IsDigit(Name[0]))
				{
					OutNames.AddUnique(Name);
				}
			}
		}
		CollectRawParamMentions(N.Lhs, Depth + 1, OutNames);
		CollectRawParamMentions(N.Rhs, Depth + 1, OutNames);
		CollectRawParamMentions(N.RefArg, Depth + 1, OutNames);
		for (const FExprPtr& Arg : N.Args)
		{
			CollectRawParamMentions(Arg, Depth + 1, OutNames);
		}
	}
}

bool FCrowdyEffectLowering::IsReservedParamName(const FString& Name)
{
	return Name == TEXT("source_id")
		|| Name == CrowdyGameModelMetaKeys::NotifyIdParam
		|| Name == CrowdyGameModelMetaKeys::SelfContainerIdParam
		|| Name == TEXT("caller_user_id")
		|| Name == TEXT("current_turn_user_id")
		|| Name == TEXT("self_owner_id")
		|| Name == TEXT("session_id");
}

FCrowdyEffectLoweringResult FCrowdyEffectLowering::Lower(
	const FCrowdyEffectProgram& Program, const FCrowdyEffectLoweringContext& Context)
{
	FCrowdyEffectLoweringResult Result;
	FLowerer Lowerer(Context, Result.Diagnostics);
	Result.Function = Lowerer.Run(Program);
	Result.bSourceReferenced = Lowerer.bSourceReferenced;
	return Result;
}

TArray<FString> FCrowdyEffectLowering::CollectUndeclaredParams(
	const FCrowdyEffectProgram& Program, const TArray<FString>& DeclaredNames)
{
	TArray<FString> Referenced;
	for (const FCrowdyEffectStatement& S : Program.Statements)
	{
		CollectParamRefs(S.Rhs, 0, Referenced);
		CollectParamRefs(S.Condition, 0, Referenced);
	}
	CollectParamRefs(Program.ReturnExpr, 0, Referenced);

	TArray<FString> Undeclared;
	for (const FString& Name : Referenced)
	{
		// A reserved name (source_id / a server-injected system param like self_container_id) is never a magnitude, and a
		// declared magnitude is already covered; everything else the effect reads but never declared is "missing".
		if (FLowerer::IsReservedParamName(Name) || DeclaredNames.Contains(Name))
		{
			continue;
		}
		Undeclared.Add(Name);
	}
	return Undeclared;
}
