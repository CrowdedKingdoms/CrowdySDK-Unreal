#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

namespace CrowdyExecGuidFormTests
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	const FGuid Sample(0x01234567, 0x89ABCDEF, 0xFEDCBA98, 0x76543210);

	/** The list the local check's kinds contract test holds the generated check_guid to. */
	FString GoldenPath()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("CrowdySDK"));
		if (!Plugin.IsValid())
		{
			return FString();
		}
		FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(Plugin->GetBaseDir(), TEXT("../../scripts/ck-exec-gate/local-check/tests/guid_forms.txt")));
		FPaths::CollapseRelativeDirectories(Path);
		return Path;
	}

	/** Every format FGuid prints, then strings near the edge of what FGuid::Parse reads. */
	TArray<FString> FormsToParse(int32& OutPrinted)
	{
		TArray<FString> Forms;
		for (const EGuidFormats Format : {EGuidFormats::Digits, EGuidFormats::DigitsLower, EGuidFormats::DigitsWithHyphens, EGuidFormats::DigitsWithHyphensLower,
			EGuidFormats::DigitsWithHyphensInBraces, EGuidFormats::DigitsWithHyphensInParentheses, EGuidFormats::HexValuesInBraces, EGuidFormats::UniqueObjectGuid,
			EGuidFormats::Short, EGuidFormats::Base36Encoded})
		{
			Forms.Add(Sample.ToString(Format));
		}
		OutPrinted = Forms.Num();
		const FString Digits = Sample.ToString(EGuidFormats::Digits);
		const FString Hyphens = Sample.ToString(EGuidFormats::DigitsWithHyphens);
		const FString Short = Sample.ToString(EGuidFormats::Short);
		const FString Base36 = Sample.ToString(EGuidFormats::Base36Encoded);
		Forms.Add(FString::ChrN(25, TEXT('Z')));
		Forms.Add(FString::ChrN(25, TEXT('z')));
		Forms.Add(Base36.Left(24) + TEXT("."));
		Forms.Add(FString::ChrN(22, TEXT('+')));
		Forms.Add(FString::ChrN(22, TEXT('/')));
		Forms.Add(FString::ChrN(22, TEXT('-')));
		Forms.Add(FString::ChrN(22, TEXT('_')));
		Forms.Add(FString::ChrN(22, TEXT('=')));
		Forms.Add(Short.Left(21) + TEXT("="));
		Forms.Add(Short.Left(10) + TEXT("=") + Short.Mid(11));
		Forms.Add(Short.Left(10) + TEXT(".") + Short.Mid(11));
		Forms.Add(FString::ChrN(22, static_cast<TCHAR>(0x00E9)));
		Forms.Add(TEXT("{") + Hyphens + TEXT(")"));
		Forms.Add(TEXT("(") + Hyphens + TEXT("}"));
		Forms.Add(TEXT("[") + Hyphens + TEXT("]"));
		Forms.Add(Hyphens.Left(8) + TEXT("0") + Hyphens.Mid(9));
		Forms.Add(Digits.Left(31));
		Forms.Add(Digits + TEXT("0"));
		Forms.Add(Digits.Left(31) + TEXT("G"));
		return Forms;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecGuidFormsTest, "CrowdySDK.CrowdyExecEditor.CodegenGuidFormsMatchFGuidParse", CrowdyExecGuidFormTests::TestFlags)
bool FCrowdyExecGuidFormsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecGuidFormTests;
	int32 Printed = 0;
	const TArray<FString> Forms = FormsToParse(Printed);
	FString Actual;
	for (int32 Index = 0; Index < Forms.Num(); ++Index)
	{
		FGuid Parsed;
		const bool bAccepted = FGuid::Parse(Forms[Index], Parsed);
		if (Index < Printed)
		{
			TestTrue(FString::Printf(TEXT("FGuid::Parse reads what FGuid prints (%s)"), *Forms[Index]), bAccepted && Parsed == Sample);
		}
		Actual += FString::Printf(TEXT("%s %s\n"), bAccepted ? TEXT("accept") : TEXT("refuse"), *Forms[Index]);
	}

	const FString Path = GoldenPath();
	if (!TestFalse(TEXT("the CrowdySDK plugin is found"), Path.IsEmpty()))
	{
		return false;
	}
	FString Golden;
	if (!FFileHelper::LoadFileToString(Golden, *Path))
	{
		FFileHelper::SaveStringToFile(Actual, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		AddError(FString::Printf(TEXT("recorded %s; review it and run again"), *Path));
		return false;
	}
	if (!Golden.Equals(Actual, ESearchCase::CaseSensitive))
	{
		TArray<FString> GoldenLines;
		TArray<FString> ActualLines;
		Golden.ParseIntoArray(GoldenLines, TEXT("\n"), false);
		Actual.ParseIntoArray(ActualLines, TEXT("\n"), false);
		int32 Line = FMath::Min(GoldenLines.Num(), ActualLines.Num()) + 1;
		for (int32 Index = 0; Index < FMath::Min(GoldenLines.Num(), ActualLines.Num()); ++Index)
		{
			if (!GoldenLines[Index].Equals(ActualLines[Index], ESearchCase::CaseSensitive))
			{
				Line = Index + 1;
				break;
			}
		}
		AddError(FString::Printf(TEXT("what FGuid::Parse reads differs from %s at line %d"), *Path, Line));
	}
	return true;
}

#endif
