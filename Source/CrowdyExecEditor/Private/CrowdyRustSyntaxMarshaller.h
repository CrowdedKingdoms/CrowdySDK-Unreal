#pragma once

#include "CoreMinimal.h"
#include "Containers/StringView.h"
#include "Fonts/SlateFontInfo.h"
#include "Framework/Text/SyntaxHighlighterTextLayoutMarshaller.h"
#include "Styling/SlateTypes.h"

/** Splitting Rust source into the pieces the Server Code editor colours. */
namespace CrowdyRustSyntax
{
	enum class EKind : uint8
	{
		Plain,
		Keyword,
		Type,
		String,
		Comment,
		Number,
		Count
	};

	struct FSpan
	{
		int32 Start = 0;
		int32 End = 0;
		EKind Kind = EKind::Plain;
	};

	/** What a line leaves open for the next one: a block comment (Rust nests them) or a string. */
	struct FCarry
	{
		int32 CommentDepth = 0;
		bool bInString = false;
	};

	/** The spans of one line, in order and covering it end to end. Carry brings in what the previous line left open and takes out what this one leaves open. */
	TArray<FSpan> ScanLine(FStringView Line, FCarry& Carry);
}

/** Colours Rust source in a multi-line text box: keywords, types, strings, comments and numbers. */
class FCrowdyRustSyntaxMarshaller : public FSyntaxHighlighterTextLayoutMarshaller
{
public:
	static TSharedRef<FCrowdyRustSyntaxMarshaller> Create(const FSlateFontInfo& Font);

protected:
	virtual void ParseTokens(const FString& SourceString, FTextLayout& TargetTextLayout, TArray<ISyntaxTokenizer::FTokenizedLine> TokenizedLines) override;

private:
	FCrowdyRustSyntaxMarshaller(TSharedPtr<ISyntaxTokenizer> InTokenizer, const FSlateFontInfo& Font);

	FTextBlockStyle Styles[static_cast<int32>(CrowdyRustSyntax::EKind::Count)];
};
