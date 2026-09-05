// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/Effect/CrowdyEffectParser.h"

namespace
{
	bool IsIdentStart(TCHAR C)
	{
		return FChar::IsAlpha(C) || C == TEXT('_');
	}

	bool IsIdentChar(TCHAR C)
	{
		return FChar::IsAlnum(C) || C == TEXT('_');
	}

	// Turns EffectScript source text into a flat token stream. Newlines are significant (statements are
	// line-delimited); '#' begins a line comment. Lexing errors become Error diagnostics but never stop the
	// scan, so the parser still sees a well-formed stream.
	struct FTokenizer
	{
		const FString& Src;
		int32 Pos = 0;
		int32 Line = 1;
		int32 Col = 1;
		TArray<FCrowdyEffectToken>& Out;
		TArray<FCrowdyEffectDiagnostic>& Diags;

		FTokenizer(const FString& InSrc, TArray<FCrowdyEffectToken>& InOut, TArray<FCrowdyEffectDiagnostic>& InDiags)
			: Src(InSrc), Out(InOut), Diags(InDiags) {}

		TCHAR Peek(int32 Ahead = 0) const
		{
			const int32 I = Pos + Ahead;
			return Src.IsValidIndex(I) ? Src[I] : TEXT('\0');
		}

		void Advance()
		{
			if (!Src.IsValidIndex(Pos))
			{
				return;
			}
			if (Src[Pos] == TEXT('\n'))
			{
				++Line;
				Col = 1;
			}
			else
			{
				++Col;
			}
			++Pos;
		}

		void Emit(ECrowdyEffectTokenType Type, const FString& Text, int32 TokLine, int32 TokCol, int32 TokStart)
		{
			// The token is fully consumed by the time Emit is called, so Pos - TokStart is its exact source span.
			Out.Add({ Type, Text, TokLine, TokCol, TokStart, Pos - TokStart });
		}

		void Error(const FString& Message, int32 TokLine, int32 TokCol)
		{
			Diags.Add({ ECrowdyEffectSeverity::Error, TokLine, TokCol, Message });
		}

		void Run()
		{
			while (Src.IsValidIndex(Pos))
			{
				const TCHAR C = Src[Pos];
				const int32 TokLine = Line;
				const int32 TokCol = Col;
				const int32 TokStart = Pos;

				if (C == TEXT('\n') || C == TEXT('\r'))
				{
					if (C == TEXT('\r') && Peek(1) == TEXT('\n'))
					{
						Advance();
					}
					Advance();
					Emit(ECrowdyEffectTokenType::Newline, TEXT("\n"), TokLine, TokCol, TokStart);
					continue;
				}

				if (C == TEXT(' ') || C == TEXT('\t'))
				{
					Advance();
					continue;
				}

				if (C == TEXT('#'))
				{
					while (Src.IsValidIndex(Pos) && Src[Pos] != TEXT('\n') && Src[Pos] != TEXT('\r'))
					{
						Advance();
					}
					continue;
				}

				if (FChar::IsDigit(C) || (C == TEXT('.') && FChar::IsDigit(Peek(1))))
				{
					ReadNumber(TokLine, TokCol, TokStart);
					continue;
				}

				if (C == TEXT('"'))
				{
					ReadString(TokLine, TokCol, TokStart);
					continue;
				}

				if (C == TEXT('$'))
				{
					Advance();
					if (!IsIdentStart(Peek()))
					{
						Error(TEXT("expected a parameter name after '$'"), TokLine, TokCol);
						continue;
					}
					FString Name;
					while (IsIdentChar(Peek()))
					{
						Name.AppendChar(Peek());
						Advance();
					}
					Emit(ECrowdyEffectTokenType::Param, Name, TokLine, TokCol, TokStart);
					continue;
				}

				if (IsIdentStart(C))
				{
					FString Ident;
					while (IsIdentChar(Peek()))
					{
						Ident.AppendChar(Peek());
						Advance();
					}
					Emit(ECrowdyEffectTokenType::Identifier, Ident, TokLine, TokCol, TokStart);
					continue;
				}

				if (!ReadOperator(TokLine, TokCol, TokStart))
				{
					Error(FString::Printf(TEXT("unexpected character '%c'"), C), TokLine, TokCol);
					Advance();
				}
			}

			// A trailing Newline guarantees the last statement has a terminator; End caps the stream.
			Emit(ECrowdyEffectTokenType::Newline, TEXT("\n"), Line, Col, Pos);
			Emit(ECrowdyEffectTokenType::End, FString(), Line, Col, Pos);
		}

		void ReadNumber(int32 TokLine, int32 TokCol, int32 TokStart)
		{
			FString Num;
			bool bSawDot = false;
			while (true)
			{
				const TCHAR C = Peek();
				if (FChar::IsDigit(C))
				{
					Num.AppendChar(C);
					Advance();
				}
				else if (C == TEXT('.') && !bSawDot && FChar::IsDigit(Peek(1)))
				{
					bSawDot = true;
					Num.AppendChar(C);
					Advance();
				}
				else
				{
					break;
				}
			}
			Emit(ECrowdyEffectTokenType::Number, Num, TokLine, TokCol, TokStart);
		}

		void ReadString(int32 TokLine, int32 TokCol, int32 TokStart)
		{
			Advance();
			FString Content;
			bool bClosed = false;
			while (Src.IsValidIndex(Pos))
			{
				const TCHAR C = Peek();
				if (C == TEXT('\n') || C == TEXT('\r'))
				{
					break;
				}
				if (C == TEXT('\\'))
				{
					const TCHAR Next = Peek(1);
					if (Next == TEXT('"') || Next == TEXT('\\'))
					{
						Content.AppendChar(Next);
						Advance();
						Advance();
						continue;
					}
					if (Next == TEXT('n'))
					{
						Content.AppendChar(TEXT('\n'));
						Advance();
						Advance();
						continue;
					}
					Content.AppendChar(C);
					Advance();
					continue;
				}
				if (C == TEXT('"'))
				{
					Advance();
					bClosed = true;
					break;
				}
				Content.AppendChar(C);
				Advance();
			}
			if (!bClosed)
			{
				Error(TEXT("unterminated string literal"), TokLine, TokCol);
			}
			Emit(ECrowdyEffectTokenType::String, Content, TokLine, TokCol, TokStart);
		}

		bool ReadOperator(int32 TokLine, int32 TokCol, int32 TokStart)
		{
			const TCHAR C = Peek();
			const TCHAR N = Peek(1);

			auto Two = [&](ECrowdyEffectTokenType Type, const TCHAR* Text)
			{
				Advance();
				Advance();
				Emit(Type, Text, TokLine, TokCol, TokStart);
			};
			auto One = [&](ECrowdyEffectTokenType Type, const TCHAR* Text)
			{
				Advance();
				Emit(Type, Text, TokLine, TokCol, TokStart);
			};

			switch (C)
			{
			case TEXT('='):
				if (N == TEXT('=')) { Two(ECrowdyEffectTokenType::Eq, TEXT("==")); return true; }
				One(ECrowdyEffectTokenType::Assign, TEXT("=")); return true;
			case TEXT('!'):
				if (N == TEXT('=')) { Two(ECrowdyEffectTokenType::NotEq, TEXT("!=")); return true; }
				One(ECrowdyEffectTokenType::Bang, TEXT("!")); return true;
			case TEXT('<'):
				if (N == TEXT('=')) { Two(ECrowdyEffectTokenType::Le, TEXT("<=")); return true; }
				One(ECrowdyEffectTokenType::Lt, TEXT("<")); return true;
			case TEXT('>'):
				if (N == TEXT('=')) { Two(ECrowdyEffectTokenType::Ge, TEXT(">=")); return true; }
				One(ECrowdyEffectTokenType::Gt, TEXT(">")); return true;
			case TEXT('&'):
				if (N == TEXT('&')) { Two(ECrowdyEffectTokenType::AndAnd, TEXT("&&")); return true; }
				return false;
			case TEXT('|'):
				if (N == TEXT('|')) { Two(ECrowdyEffectTokenType::OrOr, TEXT("||")); return true; }
				return false;
			case TEXT('+'):
				if (N == TEXT('=')) { Two(ECrowdyEffectTokenType::PlusAssign, TEXT("+=")); return true; }
				One(ECrowdyEffectTokenType::Plus, TEXT("+")); return true;
			case TEXT('-'):
				if (N == TEXT('=')) { Two(ECrowdyEffectTokenType::MinusAssign, TEXT("-=")); return true; }
				One(ECrowdyEffectTokenType::Minus, TEXT("-")); return true;
			case TEXT('*'):
				if (N == TEXT('=')) { Two(ECrowdyEffectTokenType::StarAssign, TEXT("*=")); return true; }
				One(ECrowdyEffectTokenType::Star, TEXT("*")); return true;
			case TEXT('/'):
				if (N == TEXT('=')) { Two(ECrowdyEffectTokenType::SlashAssign, TEXT("/=")); return true; }
				One(ECrowdyEffectTokenType::Slash, TEXT("/")); return true;
			case TEXT('%'): One(ECrowdyEffectTokenType::Percent, TEXT("%")); return true;
			case TEXT('.'): One(ECrowdyEffectTokenType::Dot, TEXT(".")); return true;
			case TEXT(','): One(ECrowdyEffectTokenType::Comma, TEXT(",")); return true;
			case TEXT(':'): One(ECrowdyEffectTokenType::Colon, TEXT(":")); return true;
			case TEXT('('): One(ECrowdyEffectTokenType::LParen, TEXT("(")); return true;
			case TEXT(')'): One(ECrowdyEffectTokenType::RParen, TEXT(")")); return true;
			default: return false;
			}
		}
	};

	using FExprPtr = TSharedPtr<FCrowdyEffectExpr>;

	// Recursive-descent statement parser with precedence-climbing for expressions. Exceptions are disabled in
	// UE, so a syntax error sets bAborted (recording one diagnostic); every parse step bails while it is set,
	// and ParseStatement clears it and resyncs at the next newline so one bad line does not cascade.
	struct FParser
	{
		const TArray<FCrowdyEffectToken>& Tokens;
		TArray<FCrowdyEffectDiagnostic>& Diags;
		int32 Cursor = 0;
		bool bAborted = false;

		// Guards against a pathological (or copy-pasted-junk) effect crashing the editor / cook with an
		// uncatchable stack overflow. Depth bounds the recursive descent (deep parens / calls); NodeCount
		// bounds a huge flat expression that the descent handles iteratively but which builds a deep AST the
		// lowering later walks recursively. Both fail with a located diagnostic instead of overflowing.
		int32 Depth = 0;
		int32 NodeCount = 0;
		static constexpr int32 MaxDepth = 256;
		static constexpr int32 MaxNodes = 4000;

		// The line of the return that filled the program's slot, so a second one can point back at the first.
		int32 FirstReturnLine = 0;

		FParser(const TArray<FCrowdyEffectToken>& InTokens, TArray<FCrowdyEffectDiagnostic>& InDiags)
			: Tokens(InTokens), Diags(InDiags) {}

		const FCrowdyEffectToken& Cur() const { return Tokens[Cursor]; }
		const FCrowdyEffectToken& Peek(int32 Ahead) const
		{
			const int32 I = FMath::Clamp(Cursor + Ahead, 0, Tokens.Num() - 1);
			return Tokens[I];
		}
		bool Is(ECrowdyEffectTokenType Type) const { return Cur().Type == Type; }
		bool IsEnd() const { return Cur().Type == ECrowdyEffectTokenType::End; }

		FCrowdyEffectToken Take()
		{
			const FCrowdyEffectToken& T = Tokens[Cursor];
			if (Cursor < Tokens.Num() - 1)
			{
				++Cursor;
			}
			return T;
		}

		// Record the first error of a statement and stop; later Fail calls in the same statement are ignored.
		void Fail(const FString& Message, const FCrowdyEffectToken& At)
		{
			if (bAborted)
			{
				return;
			}
			Diags.Add({ ECrowdyEffectSeverity::Error, At.Line, At.Col, Message });
			bAborted = true;
		}

		FCrowdyEffectToken Expect(ECrowdyEffectTokenType Type, const TCHAR* What)
		{
			if (bAborted)
			{
				return FCrowdyEffectToken{};
			}
			if (!Is(Type))
			{
				Fail(FString::Printf(TEXT("expected %s"), What), Cur());
				return FCrowdyEffectToken{};
			}
			return Take();
		}

		static bool IdentEquals(const FCrowdyEffectToken& T, const TCHAR* Word)
		{
			return T.Type == ECrowdyEffectTokenType::Identifier && T.Text == Word;
		}

		FExprPtr MakeExpr(ECrowdyEffectExprKind Kind, const FCrowdyEffectToken& At)
		{
			if (++NodeCount > MaxNodes)
			{
				Fail(TEXT("effect expression has too many terms"), At);
			}
			FExprPtr E = MakeShared<FCrowdyEffectExpr>();
			E->Kind = Kind;
			E->Line = At.Line;
			E->Col = At.Col;
			return E;
		}

		FExprPtr ParseExpression(int32 MinPrec = 1)
		{
			// Each recursive descent (a paren, a call arg, an operator RHS) deepens the parse stack; bail with
			// a diagnostic before it can overflow.
			if (++Depth > MaxDepth)
			{
				Fail(TEXT("expression nested too deeply"), Cur());
				--Depth;
				return FExprPtr();
			}

			FExprPtr Left = ParseUnary();
			while (!bAborted)
			{
				const int32 Prec = CrowdyEffectBinaryPrecedence(Cur().Text);
				if (Prec == 0 || Prec < MinPrec)
				{
					break;
				}
				const FCrowdyEffectToken OpTok = Take();
				FExprPtr Right = ParseExpression(Prec + 1);
				if (bAborted)
				{
					break;
				}
				FExprPtr Node = MakeExpr(ECrowdyEffectExprKind::Binary, OpTok);
				Node->Op = OpTok.Text;
				Node->Lhs = Left;
				Node->Rhs = Right;
				Left = Node;
			}

			--Depth;
			return Left;
		}

		FExprPtr ParseUnary()
		{
			// Collect prefix ! / - iteratively so a long run cannot overflow the parse stack; the nodes it
			// builds are still counted by MakeExpr.
			TArray<FCrowdyEffectToken> PrefixOps;
			while (Is(ECrowdyEffectTokenType::Bang) || Is(ECrowdyEffectTokenType::Minus))
			{
				PrefixOps.Add(Take());
			}

			FExprPtr Operand = ParsePrimary();
			for (int32 Index = PrefixOps.Num() - 1; Index >= 0 && !bAborted; --Index)
			{
				FExprPtr Node = MakeExpr(ECrowdyEffectExprKind::Unary, PrefixOps[Index]);
				Node->Op = PrefixOps[Index].Text;
				Node->Lhs = Operand;
				Operand = Node;
			}
			return Operand;
		}

		FExprPtr ParsePrimary()
		{
			const FCrowdyEffectToken Tok = Cur();
			switch (Tok.Type)
			{
			case ECrowdyEffectTokenType::Number:
			{
				Take();
				FExprPtr E = MakeExpr(ECrowdyEffectExprKind::NumberLiteral, Tok);
				E->Text = Tok.Text;
				return E;
			}
			case ECrowdyEffectTokenType::String:
			{
				Take();
				FExprPtr E = MakeExpr(ECrowdyEffectExprKind::StringLiteral, Tok);
				E->Text = Tok.Text;
				return E;
			}
			case ECrowdyEffectTokenType::Param:
			{
				Take();
				FExprPtr E = MakeExpr(ECrowdyEffectExprKind::Param, Tok);
				E->Text = Tok.Text;
				return E;
			}
			case ECrowdyEffectTokenType::LParen:
			{
				Take();
				FExprPtr Inner = ParseExpression();
				Expect(ECrowdyEffectTokenType::RParen, TEXT("')'"));
				return Inner;
			}
			case ECrowdyEffectTokenType::Identifier:
				return ParseIdentifierPrimary(Tok);
			default:
				Fail(TEXT("expected a value"), Tok);
				return FExprPtr();
			}
		}

		FExprPtr ParseIdentifierPrimary(const FCrowdyEffectToken& Tok)
		{
			if (Tok.Text == TEXT("true") || Tok.Text == TEXT("false"))
			{
				Take();
				FExprPtr E = MakeExpr(ECrowdyEffectExprKind::BoolLiteral, Tok);
				E->Text = Tok.Text;
				return E;
			}
			if (Tok.Text == TEXT("null"))
			{
				Take();
				return MakeExpr(ECrowdyEffectExprKind::NullLiteral, Tok);
			}
			if (Tok.Text == TEXT("self") || Tok.Text == TEXT("source"))
			{
				Take();
				return ParsePropertyAccessTail(
					Tok, Tok.Text == TEXT("self") ? ECrowdyEffectRefBase::SelfRef : ECrowdyEffectRefBase::SourceRef, nullptr);
			}
			if (Tok.Text == TEXT("ref"))
			{
				Take();
				Expect(ECrowdyEffectTokenType::LParen, TEXT("'(' after ref"));
				FExprPtr Arg = ParseExpression();
				Expect(ECrowdyEffectTokenType::RParen, TEXT("')'"));
				if (bAborted)
				{
					return FExprPtr();
				}
				return ParsePropertyAccessTail(Tok, ECrowdyEffectRefBase::ExplicitRef, Arg);
			}
			if (Tok.Text == TEXT("raw"))
			{
				Take();
				Expect(ECrowdyEffectTokenType::LParen, TEXT("'(' after raw"));
				if (bAborted)
				{
					return FExprPtr();
				}
				if (!Is(ECrowdyEffectTokenType::String))
				{
					Fail(TEXT("raw(...) takes a single string literal"), Cur());
					return FExprPtr();
				}
				const FCrowdyEffectToken Str = Take();
				Expect(ECrowdyEffectTokenType::RParen, TEXT("')'"));
				FExprPtr E = MakeExpr(ECrowdyEffectExprKind::Raw, Tok);
				E->Text = Str.Text;
				return E;
			}
			if (Tok.Text == TEXT("fn"))
			{
				Take();
				Expect(ECrowdyEffectTokenType::Colon, TEXT("':' after fn"));
				const FCrowdyEffectToken Name = Expect(ECrowdyEffectTokenType::Identifier, TEXT("a function name after 'fn:'"));
				if (bAborted)
				{
					return FExprPtr();
				}
				FExprPtr Call = MakeExpr(ECrowdyEffectExprKind::Call, Tok);
				Call->bIsFnCall = true;
				Call->Text = Name.Text;
				ParseCallArgs(Call);
				return Call;
			}

			if (Peek(1).Type == ECrowdyEffectTokenType::LParen)
			{
				Take();
				FExprPtr Call = MakeExpr(ECrowdyEffectExprKind::Call, Tok);
				Call->bIsFnCall = false;
				Call->Text = Tok.Text;
				ParseCallArgs(Call);
				return Call;
			}

			// A bareword with no call parens: only valid in a require as a policy keyword (owner, my_turn, ...).
			// Lowering decides its meaning by context; in an arithmetic expression it becomes an error there.
			Take();
			FExprPtr Ident = MakeExpr(ECrowdyEffectExprKind::Identifier, Tok);
			Ident->Text = Tok.Text;
			return Ident;
		}

		FExprPtr ParsePropertyAccessTail(const FCrowdyEffectToken& At, ECrowdyEffectRefBase Base, FExprPtr RefArg)
		{
			Expect(ECrowdyEffectTokenType::Dot, TEXT("'.' then an attribute name"));
			const FCrowdyEffectToken Attr = Expect(ECrowdyEffectTokenType::Identifier, TEXT("an attribute name"));
			if (bAborted)
			{
				return FExprPtr();
			}
			FExprPtr E = MakeExpr(ECrowdyEffectExprKind::PropertyAccess, At);
			E->RefBase = Base;
			E->RefArg = RefArg;
			E->Attr = Attr.Text;
			return E;
		}

		void ParseCallArgs(FExprPtr Call)
		{
			Expect(ECrowdyEffectTokenType::LParen, TEXT("'('"));
			if (bAborted)
			{
				return;
			}
			if (Is(ECrowdyEffectTokenType::RParen))
			{
				Take();
				return;
			}
			while (!bAborted)
			{
				FExprPtr Arg = ParseExpression();
				if (bAborted)
				{
					return;
				}
				Call->Args.Add(Arg);
				if (Is(ECrowdyEffectTokenType::Comma))
				{
					Take();
					continue;
				}
				break;
			}
			Expect(ECrowdyEffectTokenType::RParen, TEXT("')'"));
		}

		static ECrowdyEffectAssignOp AssignOpFor(ECrowdyEffectTokenType Type)
		{
			switch (Type)
			{
			case ECrowdyEffectTokenType::PlusAssign: return ECrowdyEffectAssignOp::Add;
			case ECrowdyEffectTokenType::MinusAssign: return ECrowdyEffectAssignOp::Sub;
			case ECrowdyEffectTokenType::StarAssign: return ECrowdyEffectAssignOp::Mul;
			case ECrowdyEffectTokenType::SlashAssign: return ECrowdyEffectAssignOp::Div;
			default: return ECrowdyEffectAssignOp::Set;
			}
		}

		static bool IsAssignToken(ECrowdyEffectTokenType Type)
		{
			return Type == ECrowdyEffectTokenType::Assign || Type == ECrowdyEffectTokenType::PlusAssign
				|| Type == ECrowdyEffectTokenType::MinusAssign || Type == ECrowdyEffectTokenType::StarAssign
				|| Type == ECrowdyEffectTokenType::SlashAssign;
		}

		void SkipToLineEnd()
		{
			while (!Is(ECrowdyEffectTokenType::Newline) && !IsEnd())
			{
				Take();
			}
		}

		void ParseStatement(FCrowdyEffectProgram& Program)
		{
			while (Is(ECrowdyEffectTokenType::Newline))
			{
				Take();
			}
			if (IsEnd())
			{
				return;
			}

			const int32 StartCursor = Cursor;
			const FCrowdyEffectToken First = Cur();

			if (IdentEquals(First, TEXT("require")))
			{
				Take();
				FCrowdyEffectStatement Stmt;
				Stmt.Kind = ECrowdyEffectStmtKind::Require;
				Stmt.Line = First.Line;
				Stmt.Col = First.Col;
				Stmt.Condition = ParseExpression();
				if (!bAborted && !Is(ECrowdyEffectTokenType::Newline) && !IsEnd())
				{
					Fail(TEXT("unexpected trailing tokens after the require condition"), Cur());
				}
				if (!bAborted)
				{
					Program.Statements.Add(MoveTemp(Stmt));
				}
			}
			else if (IdentEquals(First, TEXT("return")))
			{
				Take();
				FExprPtr Expr = ParseExpression();
				if (!bAborted && !Is(ECrowdyEffectTokenType::Newline) && !IsEnd())
				{
					Fail(TEXT("unexpected trailing tokens after the return expression"), Cur());
				}
				if (!bAborted)
				{
					if (Program.ReturnExpr.IsValid())
					{
						Fail(FString::Printf(
							TEXT("this effect already returns a value on line %d; an invocation answers with exactly one value"),
							FirstReturnLine), First);
					}
					else
					{
						Program.ReturnExpr = Expr;
						FirstReturnLine = First.Line;
					}
				}
			}
			else if (IdentEquals(First, TEXT("clamp")))
			{
				Fail(TEXT("clamp bounds are inherited from the attribute's ClampMin/ClampMax; do not write a clamp line"), First);
			}
			else if (!IdentEquals(First, TEXT("self")) && !IdentEquals(First, TEXT("source")))
			{
				Fail(TEXT("a statement must be an assignment (self.<attr> / source.<attr> <op> ...), a 'require', or a 'return'"), First);
			}
			else
			{
				const ECrowdyEffectRefBase Base = IdentEquals(First, TEXT("self"))
					? ECrowdyEffectRefBase::SelfRef : ECrowdyEffectRefBase::SourceRef;
				Take();
				Expect(ECrowdyEffectTokenType::Dot, TEXT("'.' then an attribute name"));
				const FCrowdyEffectToken Attr = Expect(ECrowdyEffectTokenType::Identifier, TEXT("an attribute name"));

				if (!bAborted && !IsAssignToken(Cur().Type))
				{
					Fail(TEXT("expected an assignment operator (=, +=, -=, *=, /=)"), Cur());
				}

				if (!bAborted)
				{
					const FCrowdyEffectToken OpTok = Take();
					FCrowdyEffectStatement Stmt;
					Stmt.Kind = ECrowdyEffectStmtKind::Assignment;
					Stmt.Line = First.Line;
					Stmt.Col = First.Col;
					Stmt.TargetBase = Base;
					Stmt.TargetAttr = Attr.Text;
					Stmt.AssignOp = AssignOpFor(OpTok.Type);
					Stmt.Rhs = ParseExpression();

					if (!bAborted && !Is(ECrowdyEffectTokenType::Newline) && !IsEnd())
					{
						Fail(TEXT("unexpected trailing tokens after the expression"), Cur());
					}
					if (!bAborted)
					{
						Program.Statements.Add(MoveTemp(Stmt));
					}
				}
			}

			if (bAborted)
			{
				bAborted = false;
				if (Cursor == StartCursor && !IsEnd())
				{
					Take();
				}
				SkipToLineEnd();
			}
		}

		void Run(FCrowdyEffectProgram& Program)
		{
			while (!IsEnd())
			{
				ParseStatement(Program);
			}
		}

		// Parse exactly one expression that consumes the whole token stream. Any leftover tokens are an error, so
		// a fragment like "1 2" or "self.hp -= 5" (a statement, not an expression) is rejected rather than silently
		// truncated. Returns an invalid pointer on any error.
		FExprPtr RunExpression()
		{
			FExprPtr Expr = ParseExpression();
			while (Is(ECrowdyEffectTokenType::Newline))
			{
				Take();
			}
			if (!bAborted && !IsEnd())
			{
				Fail(TEXT("unexpected trailing tokens after the expression"), Cur());
			}
			return bAborted ? FExprPtr() : Expr;
		}
	};
}

FCrowdyEffectParseResult FCrowdyEffectParser::Parse(const FString& Source)
{
	FCrowdyEffectParseResult Result;

	// An effect body is a few lines; anything vastly larger is junk. Reject it up front so the tokenizer and
	// parser never process a megabyte of pathological input.
	constexpr int32 MaxSourceLen = 100000;
	if (Source.Len() > MaxSourceLen)
	{
		Result.Diagnostics.Add({ ECrowdyEffectSeverity::Error, 1, 0,
			FString::Printf(TEXT("effect script is too large (%d chars, max %d)"), Source.Len(), MaxSourceLen) });
		return Result;
	}

	TArray<FCrowdyEffectToken> Tokens;
	FTokenizer Tokenizer(Source, Tokens, Result.Diagnostics);
	Tokenizer.Run();

	FParser Parser(Tokens, Result.Diagnostics);
	Parser.Run(Result.Program);

	return Result;
}

TSharedPtr<FCrowdyEffectExpr> FCrowdyEffectParser::ParseExpression(
	const FString& Source, TArray<FCrowdyEffectDiagnostic>& OutDiagnostics)
{
	// The same up-front size cap as Parse: an expression fragment is tiny, so anything vastly larger is junk and
	// is rejected before the tokenizer and parser ever touch it.
	constexpr int32 MaxSourceLen = 100000;
	if (Source.Len() > MaxSourceLen)
	{
		OutDiagnostics.Add({ ECrowdyEffectSeverity::Error, 1, 0,
			FString::Printf(TEXT("effect expression is too large (%d chars, max %d)"), Source.Len(), MaxSourceLen) });
		return TSharedPtr<FCrowdyEffectExpr>();
	}

	TArray<FCrowdyEffectToken> Tokens;
	FTokenizer Tokenizer(Source, Tokens, OutDiagnostics);
	Tokenizer.Run();

	FParser Parser(Tokens, OutDiagnostics);
	return Parser.RunExpression();
}

TArray<FCrowdyEffectToken> FCrowdyEffectParser::TokenizeExpression(const FString& Source)
{
	TArray<FCrowdyEffectToken> Tokens;

	// The same up-front size cap as Parse. An over-long input is junk; return nothing rather than lexing it.
	constexpr int32 MaxSourceLen = 100000;
	if (Source.Len() > MaxSourceLen)
	{
		return Tokens;
	}

	TArray<FCrowdyEffectDiagnostic> IgnoredDiagnostics;
	FTokenizer Tokenizer(Source, Tokens, IgnoredDiagnostics);
	Tokenizer.Run();
	return Tokens;
}
