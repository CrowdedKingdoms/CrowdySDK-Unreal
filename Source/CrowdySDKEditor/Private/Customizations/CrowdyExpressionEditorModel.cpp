// Fill out your copyright notice in the Description page of Project Settings.

#include "Customizations/CrowdyExpressionEditorModel.h"

#include "Replication/GameModel/Effect/CrowdyEffectAst.h"
#include "Replication/GameModel/Effect/CrowdyEffectParser.h"

namespace
{
	bool IsExprIdentChar(TCHAR C)
	{
		return FChar::IsAlnum(C) || C == TEXT('_');
	}

	// True when the caret (the partial word starting at WordStart) sits inside a `require` statement: the current line,
	// after any leading whitespace, begins with the `require` keyword. Policy keywords are only offered there.
	bool IsInRequireStatement(const FString& Text, int32 WordStart)
	{
		int32 LineStart = FMath::Clamp(WordStart, 0, Text.Len());
		while (LineStart > 0 && Text[LineStart - 1] != TEXT('\n'))
		{
			--LineStart;
		}

		int32 Pos = LineStart;
		while (Pos < Text.Len() && (Text[Pos] == TEXT(' ') || Text[Pos] == TEXT('\t')))
		{
			++Pos;
		}

		static const FString Require = TEXT("require");
		if (Text.Mid(Pos, Require.Len()) != Require)
		{
			return false;
		}
		const int32 After = Pos + Require.Len();
		return After >= Text.Len() || !IsExprIdentChar(Text[After]);
	}

	bool IsKeywordName(const FString& Text)
	{
		return CrowdyExpressionEditorModel::KeywordNames().Contains(Text)
			|| CrowdyExpressionEditorModel::BuiltinNames().Contains(Text);
	}

	// Real tokens are everything the lexer produced except the two synthetic trailing terminators the parser
	// relies on (Newline, End); the editor only classifies genuine source tokens.
	TArray<FCrowdyEffectToken> RealTokens(const FString& Expression)
	{
		TArray<FCrowdyEffectToken> All = FCrowdyEffectParser::TokenizeExpression(Expression);
		TArray<FCrowdyEffectToken> Real;
		Real.Reserve(All.Num());
		for (const FCrowdyEffectToken& T : All)
		{
			if (T.Type != ECrowdyEffectTokenType::Newline && T.Type != ECrowdyEffectTokenType::End)
			{
				Real.Add(T);
			}
		}
		return Real;
	}
}

namespace CrowdyExpressionEditorModel
{
	const TArray<FString>& KeywordNames()
	{
		static const TArray<FString> Keywords = {
			TEXT("self"), TEXT("source"), TEXT("ref"), TEXT("raw"), TEXT("fn"),
			TEXT("require"), TEXT("return"), TEXT("true"), TEXT("false"), TEXT("null")
		};
		return Keywords;
	}

	const TArray<FString>& BuiltinNames()
	{
		static const TArray<FString> Builtins = {
			TEXT("if"), TEXT("max"), TEXT("min"), TEXT("clamp"),
			TEXT("floor"), TEXT("ceil"), TEXT("round"), TEXT("abs")
		};
		return Builtins;
	}

	const TArray<FString>& PolicyKeywordNames()
	{
		static const TArray<FString> Policy = {
			TEXT("owner_of_self"), TEXT("host"), TEXT("my_turn"), TEXT("participant"), TEXT("automation"),
			TEXT("anyone")
		};
		return Policy;
	}

	TArray<FCrowdyExpressionChip> ClassifyChips(const FString& Expression)
	{
		const TArray<FCrowdyEffectToken> Tokens = RealTokens(Expression);
		TArray<FCrowdyExpressionChip> Chips;
		Chips.Reserve(Tokens.Num());

		auto AddChip = [&Chips, &Expression](ECrowdyExpressionChipKind Kind, int32 Start, int32 Len)
		{
			FCrowdyExpressionChip Chip;
			Chip.Kind = Kind;
			Chip.Start = Start;
			Chip.Len = Len;
			Chip.Text = Expression.Mid(Start, Len);
			Chips.Add(MoveTemp(Chip));
		};

		int32 Index = 0;
		while (Index < Tokens.Num())
		{
			const FCrowdyEffectToken& T = Tokens[Index];

			switch (T.Type)
			{
			case ECrowdyEffectTokenType::Identifier:
			{
				const bool bIsRole = (T.Text == TEXT("self") || T.Text == TEXT("source"));
				if (bIsRole && Index + 2 < Tokens.Num()
					&& Tokens[Index + 1].Type == ECrowdyEffectTokenType::Dot
					&& Tokens[Index + 2].Type == ECrowdyEffectTokenType::Identifier)
				{
					const FCrowdyEffectToken& Attr = Tokens[Index + 2];
					AddChip(ECrowdyExpressionChipKind::Attribute, T.Start, (Attr.Start + Attr.Len) - T.Start);
					Index += 3;
					continue;
				}
				if (T.Text == TEXT("fn") && Index + 2 < Tokens.Num()
					&& Tokens[Index + 1].Type == ECrowdyEffectTokenType::Colon
					&& Tokens[Index + 2].Type == ECrowdyEffectTokenType::Identifier)
				{
					const FCrowdyEffectToken& Name = Tokens[Index + 2];
					AddChip(ECrowdyExpressionChipKind::FunctionName, T.Start, (Name.Start + Name.Len) - T.Start);
					Index += 3;
					continue;
				}
				if (Index + 1 < Tokens.Num() && Tokens[Index + 1].Type == ECrowdyEffectTokenType::LParen)
				{
					AddChip(ECrowdyExpressionChipKind::FunctionName, T.Start, T.Len);
					++Index;
					continue;
				}
				AddChip(IsKeywordName(T.Text) ? ECrowdyExpressionChipKind::Keyword : ECrowdyExpressionChipKind::Identifier,
					T.Start, T.Len);
				++Index;
				continue;
			}
			case ECrowdyEffectTokenType::Number:
				AddChip(ECrowdyExpressionChipKind::Number, T.Start, T.Len);
				++Index;
				continue;
			case ECrowdyEffectTokenType::String:
				AddChip(ECrowdyExpressionChipKind::String, T.Start, T.Len);
				++Index;
				continue;
			case ECrowdyEffectTokenType::Param:
				AddChip(ECrowdyExpressionChipKind::Magnitude, T.Start, T.Len);
				++Index;
				continue;
			case ECrowdyEffectTokenType::LParen:
			case ECrowdyEffectTokenType::RParen:
				AddChip(ECrowdyExpressionChipKind::Paren, T.Start, T.Len);
				++Index;
				continue;
			case ECrowdyEffectTokenType::Comma:
				AddChip(ECrowdyExpressionChipKind::Comma, T.Start, T.Len);
				++Index;
				continue;
			default:
				// Every remaining token type is an operator (arithmetic / comparison / logical / assignment) or a
				// bare dot / colon that did not coalesce into an attribute or fn: chip.
				AddChip(ECrowdyExpressionChipKind::Operator, T.Start, T.Len);
				++Index;
				continue;
			}
		}

		return Chips;
	}

	namespace
	{
		ECrowdyExpressionHighlightColor ColorForChip(ECrowdyExpressionChipKind Kind)
		{
			switch (Kind)
			{
			case ECrowdyExpressionChipKind::Attribute:    return ECrowdyExpressionHighlightColor::Attribute;
			case ECrowdyExpressionChipKind::Magnitude:    return ECrowdyExpressionHighlightColor::Magnitude;
			case ECrowdyExpressionChipKind::Number:       return ECrowdyExpressionHighlightColor::Number;
			case ECrowdyExpressionChipKind::String:       return ECrowdyExpressionHighlightColor::String;
			case ECrowdyExpressionChipKind::Operator:     return ECrowdyExpressionHighlightColor::Operator;
			case ECrowdyExpressionChipKind::Paren:
			case ECrowdyExpressionChipKind::Comma:        return ECrowdyExpressionHighlightColor::Punctuation;
			case ECrowdyExpressionChipKind::FunctionName: return ECrowdyExpressionHighlightColor::FunctionName;
			case ECrowdyExpressionChipKind::Keyword:      return ECrowdyExpressionHighlightColor::Keyword;
			default:                                      return ECrowdyExpressionHighlightColor::Default;
			}
		}
	}

	// Shared run-building core: given the source text and its already-computed diagnostic ranges, partition into
	// contiguous coloured runs. The single-expression and whole-body highlighters differ only in how they produce
	// the diagnostics, so both funnel through here and get identical colouring / error-splitting behaviour.
	static TArray<FCrowdyExpressionHighlightRun> BuildRunsWithErrors(
		const FString& Text, const TArray<FCrowdyExpressionDiagnosticRange>& Diagnostics)
	{
		const int32 Len = Text.Len();
		TArray<FCrowdyExpressionHighlightRun> Runs;
		if (Len == 0)
		{
			return Runs;
		}

		const TArray<FCrowdyExpressionChip> Chips = ClassifyChips(Text);

		TArray<FCrowdyExpressionDiagnosticRange> ErrorRanges;
		for (const FCrowdyExpressionDiagnosticRange& Range : Diagnostics)
		{
			if (Range.bIsError && Range.Len > 0)
			{
				ErrorRanges.Add(Range);
			}
		}

		// Cut the text at every token edge and every error edge, so no resulting span straddles a colour change or an
		// error boundary. Then each [a, b) span takes the colour of the chip covering it (Default if none) and is an
		// error span when it lies inside any error range.
		TArray<int32> Cuts;
		Cuts.Add(0);
		Cuts.Add(Len);
		for (const FCrowdyExpressionChip& Chip : Chips)
		{
			Cuts.Add(FMath::Clamp(Chip.Start, 0, Len));
			Cuts.Add(FMath::Clamp(Chip.Start + Chip.Len, 0, Len));
		}
		for (const FCrowdyExpressionDiagnosticRange& Range : ErrorRanges)
		{
			Cuts.Add(FMath::Clamp(Range.Start, 0, Len));
			Cuts.Add(FMath::Clamp(Range.Start + Range.Len, 0, Len));
		}
		Cuts.Sort();

		auto ColorAt = [&Chips](int32 Position) -> ECrowdyExpressionHighlightColor
		{
			for (const FCrowdyExpressionChip& Chip : Chips)
			{
				if (Chip.Start <= Position && Position < Chip.Start + Chip.Len)
				{
					return ColorForChip(Chip.Kind);
				}
			}
			return ECrowdyExpressionHighlightColor::Default;
		};

		auto IsError = [&ErrorRanges](int32 Position) -> bool
		{
			for (const FCrowdyExpressionDiagnosticRange& Range : ErrorRanges)
			{
				if (Range.Start <= Position && Position < Range.Start + Range.Len)
				{
					return true;
				}
			}
			return false;
		};

		for (int32 CutIndex = 0; CutIndex + 1 < Cuts.Num(); ++CutIndex)
		{
			const int32 Start = Cuts[CutIndex];
			const int32 End = Cuts[CutIndex + 1];
			if (End <= Start)
			{
				continue;
			}

			const ECrowdyExpressionHighlightColor Color = ColorAt(Start);
			const bool bError = IsError(Start);

			if (Runs.Num() > 0 && Runs.Last().Color == Color && Runs.Last().bError == bError
				&& Runs.Last().Start + Runs.Last().Len == Start)
			{
				Runs.Last().Len += End - Start;
			}
			else
			{
				FCrowdyExpressionHighlightRun Run;
				Run.Start = Start;
				Run.Len = End - Start;
				Run.Color = Color;
				Run.bError = bError;
				Runs.Add(Run);
			}
		}

		return Runs;
	}

	TArray<FCrowdyExpressionHighlightRun> BuildHighlightRuns(const FString& Expression)
	{
		return BuildRunsWithErrors(Expression, DiagnoseRanges(Expression));
	}

	TArray<FCrowdyExpressionHighlightRun> BuildBodyHighlightRuns(const FString& Body)
	{
		return BuildRunsWithErrors(Body, DiagnoseBodyRanges(Body));
	}

	TArray<FCrowdyExpressionHighlightRun> BuildBodyHighlightRuns(
		const FString& Body, const TArray<FCrowdyEffectDiagnostic>& ExtraDiagnostics)
	{
		TArray<FCrowdyExpressionDiagnosticRange> Ranges = DiagnoseBodyRanges(Body);
		Ranges.Append(MapBodyDiagnosticsToRanges(Body, ExtraDiagnostics));
		return BuildRunsWithErrors(Body, Ranges);
	}

	int32 CountLines(const FString& Body)
	{
		int32 Lines = 1;
		for (int32 Index = 0; Index < Body.Len(); ++Index)
		{
			if (Body[Index] == TEXT('\n'))
			{
				++Lines;
			}
		}
		return Lines;
	}

	TArray<FCrowdyExpressionDiagnosticRange> DiagnoseRanges(const FString& Expression)
	{
		TArray<FCrowdyEffectDiagnostic> Diagnostics;
		FCrowdyEffectParser::ParseExpression(Expression, Diagnostics);

		const TArray<FCrowdyEffectToken> Tokens = RealTokens(Expression);
		const int32 ExprLen = Expression.Len();

		TArray<FCrowdyExpressionDiagnosticRange> Ranges;
		Ranges.Reserve(Diagnostics.Num());
		for (const FCrowdyEffectDiagnostic& D : Diagnostics)
		{
			// The expression is a single line, so Line is 1; map the 1-based column onto a 0-based offset, clamping
			// against a diagnostic whose column ran past the end (an "expected X" at end of input).
			int32 Start = D.Col > 0 ? D.Col - 1 : 0;
			Start = FMath::Clamp(Start, 0, ExprLen);

			int32 Len = 0;
			if (Start < ExprLen)
			{
				// Underline the whole token located at the column, falling back to a single character.
				Len = 1;
				for (const FCrowdyEffectToken& T : Tokens)
				{
					if (T.Start <= Start && Start < T.Start + T.Len)
					{
						Len = (T.Start + T.Len) - Start;
						break;
					}
				}
			}

			FCrowdyExpressionDiagnosticRange Range;
			Range.Start = Start;
			Range.Len = Len;
			Range.Message = D.Message;
			Range.bIsError = D.Severity == ECrowdyEffectSeverity::Error;
			Ranges.Add(MoveTemp(Range));
		}

		return Ranges;
	}

	TArray<FCrowdyExpressionDiagnosticRange> MapBodyDiagnosticsToRanges(
		const FString& Body, const TArray<FCrowdyEffectDiagnostic>& Diagnostics)
	{
		// A body is a statement list, so each diagnostic's 1-based (line, column) has to resolve against where that
		// line starts. The single-line DiagnoseRanges can assume line 1; this one cannot.
		const TArray<FCrowdyEffectToken> Tokens = RealTokens(Body);
		const int32 BodyLen = Body.Len();

		// LineStart[L] is the absolute offset of the first character of 1-based line L. Index 0 is unused; line 1
		// starts at 0, and the character after each '\n' begins the next line.
		TArray<int32> LineStart;
		LineStart.Add(0);
		LineStart.Add(0);
		for (int32 Pos = 0; Pos < BodyLen; ++Pos)
		{
			if (Body[Pos] == TEXT('\n'))
			{
				LineStart.Add(Pos + 1);
			}
		}

		TArray<FCrowdyExpressionDiagnosticRange> Ranges;
		Ranges.Reserve(Diagnostics.Num());
		for (const FCrowdyEffectDiagnostic& D : Diagnostics)
		{
			const int32 Line = D.Line > 0 ? D.Line : 1;
			const int32 Base = LineStart.IsValidIndex(Line) ? LineStart[Line] : 0;
			int32 Start = Base + (D.Col > 0 ? D.Col - 1 : 0);
			Start = FMath::Clamp(Start, 0, BodyLen);

			int32 Len = 0;
			// A diagnostic that resolves onto a line break (an "expected a value" at end of a non-final line) gets no
			// underline, matching the single-line parser's end-of-input convention; underlining the '\n' would paint a
			// stray mark at the line end.
			if (Start < BodyLen && Body[Start] != TEXT('\n'))
			{
				// Underline the whole token located at the offset, falling back to a single character. Tokens never span
				// a newline, so the underline stays within the offending line.
				Len = 1;
				for (const FCrowdyEffectToken& T : Tokens)
				{
					if (T.Start <= Start && Start < T.Start + T.Len)
					{
						Len = (T.Start + T.Len) - Start;
						break;
					}
				}
			}

			FCrowdyExpressionDiagnosticRange Range;
			Range.Start = Start;
			Range.Len = Len;
			Range.Message = D.Message;
			Range.bIsError = D.Severity == ECrowdyEffectSeverity::Error;
			Ranges.Add(MoveTemp(Range));
		}

		return Ranges;
	}

	TArray<FCrowdyExpressionDiagnosticRange> DiagnoseBodyRanges(const FString& Body)
	{
		return MapBodyDiagnosticsToRanges(Body, FCrowdyEffectParser::Parse(Body).Diagnostics);
	}
}

FCrowdyExpressionCompletionSource::FCrowdyExpressionCompletionSource(FCrowdyExpressionVocabulary InVocabulary)
	: Vocabulary(MoveTemp(InVocabulary))
{
}

TArray<FCrowdyExpressionCompletion> FCrowdyExpressionCompletionSource::GetCompletions(
	const FString& Text, int32 Caret, int32& OutReplaceStart, int32& OutReplaceLen) const
{
	Caret = FMath::Clamp(Caret, 0, Text.Len());

	// The partial word already typed is the run of identifier characters ending at the caret; the chosen
	// completion replaces exactly that run.
	int32 WordStart = Caret;
	while (WordStart > 0 && IsExprIdentChar(Text[WordStart - 1]))
	{
		--WordStart;
	}
	const FString Partial = Text.Mid(WordStart, Caret - WordStart);
	OutReplaceStart = WordStart;
	OutReplaceLen = Caret - WordStart;

	enum class EContext : uint8 { General, Attribute, SourceAttribute, Magnitude, Function };
	EContext Context = EContext::General;
	if (WordStart > 0)
	{
		const TCHAR Before = Text[WordStart - 1];
		if (Before == TEXT('$'))
		{
			Context = EContext::Magnitude;
		}
		else if (Before == TEXT('.'))
		{
			// Which container's attributes to offer depends on the base before the dot, so read it. `source` is the
			// only base with a schema of its own; `self` and an explicit ref() both resolve against the target, the
			// latter because a ref names a container of unknown type and the lowering checks it that way too.
			int32 BaseEnd = WordStart - 2;
			int32 BaseStart = BaseEnd;
			while (BaseStart >= 0 && IsExprIdentChar(Text[BaseStart]))
			{
				--BaseStart;
			}
			const FString Base = Text.Mid(BaseStart + 1, BaseEnd - BaseStart);
			Context = Base.Equals(TEXT("source"), ESearchCase::CaseSensitive)
				? EContext::SourceAttribute : EContext::Attribute;
		}
		else if (Before == TEXT(':'))
		{
			// A function completion only when the ':' is the ':' of an fn: prefix.
			int32 IdentEnd = WordStart - 2;
			int32 IdentStart = IdentEnd;
			while (IdentStart >= 0 && IsExprIdentChar(Text[IdentStart]))
			{
				--IdentStart;
			}
			if (Text.Mid(IdentStart + 1, IdentEnd - IdentStart) == TEXT("fn"))
			{
				Context = EContext::Function;
			}
		}
	}

	struct FCandidate
	{
		FString Key;
		FString Insert;
		FString Label;
		ECrowdyExpressionChipKind Kind;
	};

	TArray<FCandidate> Candidates;
	auto AddSimple = [&Candidates](const FString& Name, ECrowdyExpressionChipKind Kind)
	{
		Candidates.Add({ Name, Name, Name, Kind });
	};

	switch (Context)
	{
	case EContext::Attribute:
		for (const FString& Name : Vocabulary.Attributes)
		{
			AddSimple(Name, ECrowdyExpressionChipKind::Attribute);
		}
		break;
	case EContext::SourceAttribute:
		// An undeclared source schema means the source is another container of the target's type, so the target's
		// attributes are the right answer. A declared one answers for itself, empty included.
		for (const FString& Name :
			Vocabulary.bSourceSchemaDeclared ? Vocabulary.SourceAttributes : Vocabulary.Attributes)
		{
			AddSimple(Name, ECrowdyExpressionChipKind::Attribute);
		}
		break;
	case EContext::Magnitude:
		// The '$' is already typed, so the insertion is the bare name.
		for (const FString& Name : Vocabulary.Magnitudes)
		{
			AddSimple(Name, ECrowdyExpressionChipKind::Magnitude);
		}
		break;
	case EContext::Function:
		for (const FString& Name : Vocabulary.Functions)
		{
			AddSimple(Name, ECrowdyExpressionChipKind::FunctionName);
		}
		break;
	case EContext::General:
	default:
		for (const FString& Name : CrowdyExpressionEditorModel::KeywordNames())
		{
			AddSimple(Name, ECrowdyExpressionChipKind::Keyword);
		}
		for (const FString& Name : CrowdyExpressionEditorModel::BuiltinNames())
		{
			AddSimple(Name, ECrowdyExpressionChipKind::Keyword);
		}
		for (const FString& Name : Vocabulary.Attributes)
		{
			AddSimple(Name, ECrowdyExpressionChipKind::Attribute);
		}
		for (const FString& Name : Vocabulary.Functions)
		{
			AddSimple(Name, ECrowdyExpressionChipKind::FunctionName);
		}
		// A magnitude read needs its '$' sigil, which is not yet typed in a general position, so insert it here.
		for (const FString& Name : Vocabulary.Magnitudes)
		{
			const FString Sigiled = FString(TEXT("$")) + Name;
			Candidates.Add({ Name, Sigiled, Sigiled, ECrowdyExpressionChipKind::Magnitude });
		}
		// Inside a `require` clause, offer the policy keywords (host, owner_of_self, ...) too; they are bare identifiers
		// valid only there.
		if (IsInRequireStatement(Text, WordStart))
		{
			for (const FString& Name : CrowdyExpressionEditorModel::PolicyKeywordNames())
			{
				AddSimple(Name, ECrowdyExpressionChipKind::Keyword);
			}
		}
		break;
	}

	struct FRanked
	{
		int32 Rank = 0;
		FCandidate Candidate;
	};

	TArray<FRanked> Ranked;
	Ranked.Reserve(Candidates.Num());
	for (const FCandidate& Candidate : Candidates)
	{
		int32 Rank;
		if (Partial.IsEmpty() || Candidate.Key.StartsWith(Partial, ESearchCase::IgnoreCase))
		{
			Rank = 0;
		}
		else if (Candidate.Key.Contains(Partial, ESearchCase::IgnoreCase))
		{
			Rank = 1;
		}
		else
		{
			continue;
		}
		Ranked.Add({ Rank, Candidate });
	}

	Ranked.Sort([](const FRanked& A, const FRanked& B)
	{
		if (A.Rank != B.Rank)
		{
			return A.Rank < B.Rank;
		}
		return A.Candidate.Key.Compare(B.Candidate.Key, ESearchCase::IgnoreCase) < 0;
	});

	TArray<FCrowdyExpressionCompletion> Out;
	Out.Reserve(Ranked.Num());
	for (const FRanked& R : Ranked)
	{
		Out.Add({ R.Candidate.Insert, R.Candidate.Label, R.Candidate.Kind });
	}
	return Out;
}

FString FCrowdyExpressionCompletionSource::ApplyCompletion(
	const FString& Text, int32 ReplaceStart, int32 ReplaceLen, const FString& Insert, int32& OutNewCaret)
{
	ReplaceStart = FMath::Clamp(ReplaceStart, 0, Text.Len());
	ReplaceLen = FMath::Clamp(ReplaceLen, 0, Text.Len() - ReplaceStart);

	const FString Left = Text.Left(ReplaceStart);
	const FString Right = Text.RightChop(ReplaceStart + ReplaceLen);
	OutNewCaret = ReplaceStart + Insert.Len();
	return Left + Insert + Right;
}
