#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyRustSyntaxMarshaller.h"
#include "CrowdyServerCodeFiles.h"
#include "Misc/AutomationTest.h"

namespace CrowdyRustSyntaxTest
{
	using CrowdyRustSyntax::EKind;

	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	const TCHAR* KindName(EKind Kind)
	{
		switch (Kind)
		{
		case EKind::Keyword: return TEXT("K");
		case EKind::Type: return TEXT("T");
		case EKind::String: return TEXT("S");
		case EKind::Comment: return TEXT("C");
		case EKind::Number: return TEXT("N");
		default: return TEXT("P");
		}
	}

	/** The line's spans as "K[fn] P[ ] ...", skipping plain whitespace-only spans so cases stay readable. */
	FString Render(const FString& Line, CrowdyRustSyntax::FCarry& Carry)
	{
		TArray<FString> Parts;
		for (const CrowdyRustSyntax::FSpan& Span : CrowdyRustSyntax::ScanLine(Line, Carry))
		{
			const FString Text = Line.Mid(Span.Start, Span.End - Span.Start);
			if (Span.Kind == EKind::Plain && Text.TrimStartAndEnd().IsEmpty())
			{
				continue;
			}
			Parts.Add(FString::Printf(TEXT("%s[%s]"), KindName(Span.Kind), *Text));
		}
		return FString::Join(Parts, TEXT(" "));
	}

	FString Render(const FString& Line)
	{
		CrowdyRustSyntax::FCarry Carry;
		return Render(Line, Carry);
	}

	bool CoversLine(const FString& Line)
	{
		CrowdyRustSyntax::FCarry Carry;
		int32 At = 0;
		for (const CrowdyRustSyntax::FSpan& Span : CrowdyRustSyntax::ScanLine(Line, Carry))
		{
			if (Span.Start != At || Span.End <= Span.Start)
			{
				return false;
			}
			At = Span.End;
		}
		return At == Line.Len();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRustSyntaxLineTest, "CrowdySDK.CrowdyExecEditor.RustSyntaxColoursALine", CrowdyRustSyntaxTest::TestFlags)
bool FCrowdyRustSyntaxLineTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyRustSyntaxTest;
	TestTrue(TEXT("the spans cover the line end to end, in order"), CoversLine(TEXT("fn f() { let s = \"a // b\"; } // c")));
	TestTrue(TEXT("a comment inside a string stays string"), Render(TEXT("let s = \"a // b\"; // c")).Contains(TEXT("S[\"a // b\"]")));
	TestTrue(TEXT("the real comment starts at its own slashes"), Render(TEXT("let s = \"a // b\"; // c")).EndsWith(TEXT("C[// c]")));
	TestTrue(TEXT("keywords"), Render(TEXT("pub fn add")).StartsWith(TEXT("K[pub] K[fn]")));
	TestTrue(TEXT("a range is two numbers"), Render(TEXT("0..10")).Equals(TEXT("N[0] P[..] N[10]"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("a char literal is a string"), Render(TEXT("let c = 'a';")).Contains(TEXT("S['a']")));
	TestFalse(TEXT("a lifetime is not a string"), Render(TEXT("fn f<'a>(s: &'a str)")).Contains(TEXT("S[")));
	TestTrue(TEXT("a capitalised name is a type"), Render(TEXT("Vec<u32>")).StartsWith(TEXT("T[Vec]")));
	TestTrue(TEXT("a primitive is a type"), Render(TEXT("Vec<u32>")).Contains(TEXT("T[u32]")));
	TestTrue(TEXT("Self is a keyword"), Render(TEXT("Ok(Self::default())")).Contains(TEXT("K[Self]")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRustSyntaxCarryTest, "CrowdySDK.CrowdyExecEditor.RustSyntaxCarriesAcrossLines", CrowdyRustSyntaxTest::TestFlags)
bool FCrowdyRustSyntaxCarryTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyRustSyntaxTest;
	CrowdyRustSyntax::FCarry Carry;
	Render(TEXT("/* a /* b */"), Carry);
	TestEqual(TEXT("a nested block comment is still open after its inner end"), Carry.CommentDepth, 1);
	const FString Next = Render(TEXT("c */ fn"), Carry);
	TestTrue(TEXT("the next line is comment up to the closing mark"), Next.StartsWith(TEXT("C[c */]")));
	TestTrue(TEXT("and code after it"), Next.EndsWith(TEXT("K[fn]")));
	TestEqual(TEXT("the comment is closed"), Carry.CommentDepth, 0);

	CrowdyRustSyntax::FCarry Open;
	Render(TEXT("let s = \"a\\\"b"), Open);
	TestTrue(TEXT("an escaped quote does not close the string"), Open.bInString);
	Render(TEXT("still\";"), Open);
	TestFalse(TEXT("and the next line closes it"), Open.bInString);

	TestEqualSensitive(TEXT("line breaks become line feeds"), CrowdyServerCodeFiles::WithLineFeeds(TEXT("a\r\nb\rc")), FString(TEXT("a\nb\nc")));
	return true;
}

#endif
