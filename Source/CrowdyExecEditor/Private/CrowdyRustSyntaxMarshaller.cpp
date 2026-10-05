#include "CrowdyRustSyntaxMarshaller.h"

#include "Framework/Text/IRun.h"
#include "Framework/Text/SlateTextRun.h"
#include "Framework/Text/SyntaxTokenizer.h"
#include "Framework/Text/TextLayout.h"
#include "Styling/AppStyle.h"

namespace CrowdyRustSyntaxDetail
{
	using CrowdyRustSyntax::EKind;

	const TCHAR* const Keywords[] = {
		TEXT("as"), TEXT("async"), TEXT("await"), TEXT("break"), TEXT("const"), TEXT("continue"), TEXT("crate"), TEXT("dyn"),
		TEXT("else"), TEXT("enum"), TEXT("extern"), TEXT("false"), TEXT("fn"), TEXT("for"), TEXT("if"), TEXT("impl"), TEXT("in"),
		TEXT("let"), TEXT("loop"), TEXT("match"), TEXT("mod"), TEXT("move"), TEXT("mut"), TEXT("pub"), TEXT("ref"), TEXT("return"),
		TEXT("self"), TEXT("Self"), TEXT("static"), TEXT("struct"), TEXT("super"), TEXT("trait"), TEXT("true"), TEXT("type"),
		TEXT("unsafe"), TEXT("use"), TEXT("where"), TEXT("while"), TEXT("yield")
	};

	const TCHAR* const PrimitiveTypes[] = {
		TEXT("bool"), TEXT("char"), TEXT("str"), TEXT("u8"), TEXT("u16"), TEXT("u32"), TEXT("u64"), TEXT("u128"), TEXT("usize"),
		TEXT("i8"), TEXT("i16"), TEXT("i32"), TEXT("i64"), TEXT("i128"), TEXT("isize"), TEXT("f32"), TEXT("f64")
	};

	/** Hands the whole text to ParseTokens as one token per line; the scanning happens there, where state can carry across lines. */
	class FLineTokenizer : public ISyntaxTokenizer
	{
	public:
		virtual void Process(TArray<FTokenizedLine>& OutTokenizedLines, const FString& Input) override
		{
			TArray<FTextRange> LineRanges;
			FTextRange::CalculateLineRangesFromString(Input, LineRanges);
			for (const FTextRange& Range : LineRanges)
			{
				FTokenizedLine& Line = OutTokenizedLines.AddDefaulted_GetRef();
				Line.Range = Range;
				Line.Tokens.Emplace(ETokenType::Literal, Range);
			}
		}
	};

	bool IsDigit(TCHAR Char)
	{
		return Char >= '0' && Char <= '9';
	}

	bool IsWordStart(TCHAR Char)
	{
		return Char == '_' || FChar::IsAlpha(Char);
	}

	bool IsWordPart(TCHAR Char)
	{
		return Char == '_' || FChar::IsAlnum(Char);
	}

	bool IsOneOf(FStringView Word, TConstArrayView<const TCHAR*> List)
	{
		return List.ContainsByPredicate([Word](const TCHAR* Entry) { return Word.Equals(Entry, ESearchCase::CaseSensitive); });
	}

	EKind KindOfWord(FStringView Word)
	{
		if (IsOneOf(Word, MakeArrayView(Keywords)))
		{
			return EKind::Keyword;
		}
		return IsOneOf(Word, MakeArrayView(PrimitiveTypes)) || FChar::IsUpper(Word[0]) ? EKind::Type : EKind::Plain;
	}

	int32 WordEnd(FStringView Line, int32 Index)
	{
		while (Index < Line.Len() && IsWordPart(Line[Index]))
		{
			++Index;
		}
		return Index;
	}

	/** Past the digits, suffix and fraction of a number: 1_000u32, 0xFF, 2.5f64. A dot counts only before a digit, so 0..10 stays a range. */
	int32 NumberEnd(FStringView Line, int32 Index)
	{
		++Index;
		while (Index < Line.Len())
		{
			if (IsWordPart(Line[Index]))
			{
				++Index;
				continue;
			}
			if (Line[Index] != '.' || Index + 1 >= Line.Len() || !IsDigit(Line[Index + 1]))
			{
				break;
			}
			Index += 2;
		}
		return Index;
	}

	/** Past the end of an open block comment, or the end of the line while it stays open. */
	int32 BlockCommentEnd(FStringView Line, int32 Index, int32& Depth)
	{
		while (Index < Line.Len() && Depth > 0)
		{
			const TCHAR Next = Index + 1 < Line.Len() ? Line[Index + 1] : TEXT('\0');
			const bool bOpens = Line[Index] == '/' && Next == '*';
			const bool bCloses = Line[Index] == '*' && Next == '/';
			Depth += bOpens ? 1 : (bCloses ? -1 : 0);
			Index += bOpens || bCloses ? 2 : 1;
		}
		return Index;
	}

	/** Past the closing quote of an open string, or the end of the line while it stays open. */
	int32 StringEnd(FStringView Line, int32 Index, bool& bInString)
	{
		while (Index < Line.Len())
		{
			if (Line[Index] == '\\')
			{
				Index += 2;
				continue;
			}
			if (Line[Index] == '"')
			{
				bInString = false;
				return Index + 1;
			}
			++Index;
		}
		return Line.Len();
	}

	/** Past a character literal ('a', '\n'), or past a lifetime or label ('a), which is not one. */
	int32 QuoteEnd(FStringView Line, int32 Index, bool& bLiteral)
	{
		const int32 Len = Line.Len();
		bLiteral = Index + 1 < Len && Line[Index + 1] == '\\';
		if (bLiteral)
		{
			int32 Close = Index + 3;
			while (Close < Len && Line[Close] != '\'')
			{
				++Close;
			}
			return FMath::Min(Close + 1, Len);
		}
		bLiteral = Index + 2 < Len && Line[Index + 2] == '\'';
		return bLiteral ? Index + 3 : WordEnd(Line, Index + 1);
	}

	void AddSpan(TArray<CrowdyRustSyntax::FSpan>& Spans, int32 Start, int32 End, EKind Kind)
	{
		if (End <= Start)
		{
			return;
		}
		if (!Spans.IsEmpty() && Spans.Last().Kind == Kind && Spans.Last().End == Start)
		{
			Spans.Last().End = End;
			return;
		}
		Spans.Add({Start, End, Kind});
	}
}

TArray<CrowdyRustSyntax::FSpan> CrowdyRustSyntax::ScanLine(FStringView Line, FCarry& Carry)
{
	using namespace CrowdyRustSyntaxDetail;
	TArray<FSpan> Spans;
	const int32 Len = Line.Len();
	int32 Index = 0;
	while (Index < Len)
	{
		const int32 Start = Index;
		const TCHAR Char = Line[Index];
		const TCHAR Next = Index + 1 < Len ? Line[Index + 1] : TEXT('\0');
		if (!Carry.bInString && Carry.CommentDepth == 0 && Char == '/' && Next == '*')
		{
			Carry.CommentDepth = 1;
			Index += 2;
		}
		if (Carry.CommentDepth > 0)
		{
			Index = BlockCommentEnd(Line, Index, Carry.CommentDepth);
			AddSpan(Spans, Start, Index, EKind::Comment);
			continue;
		}
		if (!Carry.bInString && Char == '"')
		{
			Carry.bInString = true;
			++Index;
		}
		if (Carry.bInString)
		{
			Index = StringEnd(Line, Index, Carry.bInString);
			AddSpan(Spans, Start, Index, EKind::String);
			continue;
		}
		if (Char == '/' && Next == '/')
		{
			AddSpan(Spans, Start, Len, EKind::Comment);
			break;
		}
		if (Char == '\'')
		{
			bool bLiteral = false;
			Index = QuoteEnd(Line, Index, bLiteral);
			AddSpan(Spans, Start, Index, bLiteral ? EKind::String : EKind::Plain);
			continue;
		}
		if (IsDigit(Char))
		{
			Index = NumberEnd(Line, Index);
			AddSpan(Spans, Start, Index, EKind::Number);
			continue;
		}
		if (IsWordStart(Char))
		{
			Index = WordEnd(Line, Index);
			AddSpan(Spans, Start, Index, KindOfWord(Line.Mid(Start, Index - Start)));
			continue;
		}
		++Index;
		AddSpan(Spans, Start, Index, EKind::Plain);
	}
	return Spans;
}

TSharedRef<FCrowdyRustSyntaxMarshaller> FCrowdyRustSyntaxMarshaller::Create(const FSlateFontInfo& Font)
{
	return MakeShareable(new FCrowdyRustSyntaxMarshaller(MakeShared<CrowdyRustSyntaxDetail::FLineTokenizer>(), Font));
}

FCrowdyRustSyntaxMarshaller::FCrowdyRustSyntaxMarshaller(TSharedPtr<ISyntaxTokenizer> InTokenizer, const FSlateFontInfo& Font)
	: FSyntaxHighlighterTextLayoutMarshaller(MoveTemp(InTokenizer))
{
	using CrowdyRustSyntax::EKind;
	const auto StyleOf = [&Font](const TCHAR* Name)
	{
		return FTextBlockStyle(FAppStyle::Get().GetWidgetStyle<FTextBlockStyle>(Name)).SetFont(Font).SetShadowOffset(FVector2D::ZeroVector);
	};
	Styles[static_cast<int32>(EKind::Plain)] = StyleOf(TEXT("SyntaxHighlight.SourceCode.Operator"));
	Styles[static_cast<int32>(EKind::Keyword)] = StyleOf(TEXT("SyntaxHighlight.SourceCode.Keyword"));
	Styles[static_cast<int32>(EKind::Type)] = StyleOf(TEXT("SyntaxHighlight.SourceCode.Operator")).SetColorAndOpacity(FLinearColor(FColor(78, 201, 176)));
	Styles[static_cast<int32>(EKind::String)] = StyleOf(TEXT("SyntaxHighlight.SourceCode.String"));
	Styles[static_cast<int32>(EKind::Comment)] = StyleOf(TEXT("SyntaxHighlight.SourceCode.Comment"));
	Styles[static_cast<int32>(EKind::Number)] = StyleOf(TEXT("SyntaxHighlight.SourceCode.Number"));
}

void FCrowdyRustSyntaxMarshaller::ParseTokens(const FString& SourceString, FTextLayout& TargetTextLayout, TArray<ISyntaxTokenizer::FTokenizedLine> TokenizedLines)
{
	TArray<FTextLayout::FNewLineData> LinesToAdd;
	LinesToAdd.Reserve(TokenizedLines.Num());
	CrowdyRustSyntax::FCarry Carry;
	for (const ISyntaxTokenizer::FTokenizedLine& TokenizedLine : TokenizedLines)
	{
		TSharedRef<FString> LineText = MakeShared<FString>(SourceString.Mid(TokenizedLine.Range.BeginIndex, TokenizedLine.Range.Len()));
		TArray<TSharedRef<IRun>> Runs;
		for (const CrowdyRustSyntax::FSpan& Span : CrowdyRustSyntax::ScanLine(*LineText, Carry))
		{
			Runs.Add(FSlateTextRun::Create(FRunInfo(), LineText, Styles[static_cast<int32>(Span.Kind)], FTextRange(Span.Start, Span.End)));
		}
		if (Runs.IsEmpty())
		{
			Runs.Add(FSlateTextRun::Create(FRunInfo(), LineText, Styles[static_cast<int32>(CrowdyRustSyntax::EKind::Plain)], FTextRange(0, 0)));
		}
		LinesToAdd.Emplace(MoveTemp(LineText), MoveTemp(Runs));
	}
	TargetTextLayout.AddLines(LinesToAdd);
}
