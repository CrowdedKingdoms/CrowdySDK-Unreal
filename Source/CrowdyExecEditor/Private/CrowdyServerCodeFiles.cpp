#include "CrowdyServerCodeFiles.h"

#include "Containers/StringConv.h"
#include "CrowdyExecInternal.h"
#include "CrowdyServerObjectDefinition.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SCrowdyServerCodeTab.h"

#define LOCTEXT_NAMESPACE "CrowdyServerCodeFiles"

namespace CrowdyServerCodeFilesDetail
{
	constexpr int32 MaxTypeNameLength = 48;

	CrowdyServerCodeFiles::FEditStash UnsavedEdits;

	TArray<SCrowdyServerCodeTab*> LiveTabs;

	bool IsLowerAscii(TCHAR Char)
	{
		return Char >= 'a' && Char <= 'z';
	}

	bool IsUpperAscii(TCHAR Char)
	{
		return Char >= 'A' && Char <= 'Z';
	}

	bool IsDigitAscii(TCHAR Char)
	{
		return Char >= '0' && Char <= '9';
	}

	/** LF line breaks, no whitespace at the end of any line or of the text. */
	FString NormalizedCode(const FString& Text)
	{
		TArray<FString> Lines;
		CrowdyServerCodeFiles::WithLineFeeds(Text).ParseIntoArray(Lines, TEXT("\n"), false);
		for (FString& Line : Lines)
		{
			Line.TrimEndInline();
		}
		FString Joined = FString::Join(Lines, TEXT("\n"));
		Joined.TrimEndInline();
		return Joined;
	}

	/** True when the bytes, after any UTF-8 byte order mark, decode as UTF-8 and encode back unchanged. */
	bool IsUtf8(const TArray<uint8>& Bytes)
	{
		const int32 Start = Bytes.Num() >= 3 && Bytes[0] == 0xEF && Bytes[1] == 0xBB && Bytes[2] == 0xBF ? 3 : 0;
		const int32 Count = Bytes.Num() - Start;
		if (Count == 0)
		{
			return true;
		}
		const ANSICHAR* const Source = reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + Start);
		const FUTF8ToTCHAR Decoded(Source, Count);
		const FTCHARToUTF8 Encoded(Decoded.Get(), Decoded.Length());
		return Encoded.Length() == Count && FMemory::Memcmp(Encoded.Get(), Source, Count) == 0;
	}
}

TArray<FString> CrowdyServerCodeEdits::UnsavedFiles()
{
	using namespace CrowdyServerCodeFilesDetail;
	TArray<FString> Files = UnsavedEdits.Files();
	for (const SCrowdyServerCodeTab* Tab : LiveTabs)
	{
		const FString File = Tab->GetUnsavedFile();
		if (!File.IsEmpty())
		{
			Files.AddUnique(File);
		}
	}
	return Files;
}

void CrowdyServerCodeEdits::ForgetUnder(const FString& Directory)
{
	using namespace CrowdyServerCodeFilesDetail;
	UnsavedEdits.ForgetUnder(Directory);
	for (SCrowdyServerCodeTab* Tab : LiveTabs)
	{
		Tab->ForgetEditsUnder(Directory);
	}
}

void CrowdyServerCodeEdits::ShowWritten(const FString& Path)
{
	for (SCrowdyServerCodeTab* Tab : CrowdyServerCodeFilesDetail::LiveTabs)
	{
		Tab->ShowWritten(Path);
	}
}

CrowdyServerCodeFiles::FEditStash& CrowdyServerCodeEdits::Kept()
{
	return CrowdyServerCodeFilesDetail::UnsavedEdits;
}

void CrowdyServerCodeEdits::AddTab(SCrowdyServerCodeTab& Tab)
{
	CrowdyServerCodeFilesDetail::LiveTabs.AddUnique(&Tab);
}

void CrowdyServerCodeEdits::RemoveTab(SCrowdyServerCodeTab& Tab)
{
	CrowdyServerCodeFilesDetail::LiveTabs.RemoveSingleSwap(&Tab);
}

CrowdyServerCodeFiles::FDiskText CrowdyServerCodeFiles::Read(const FString& Path)
{
	using namespace CrowdyServerCodeFilesDetail;
	FDiskText File;
	File.bExists = FPaths::FileExists(Path);
	TArray<uint8> Bytes;
	if (!File.bExists || !FFileHelper::LoadFileToArray(Bytes, *Path, FILEREAD_Silent))
	{
		return File;
	}
	File.bReadable = true;
	File.bUtf8 = IsUtf8(Bytes);
	FString Text;
	FFileHelper::BufferToString(Text, Bytes.GetData(), Bytes.Num());
	File.bCrlf = Text.Contains(TEXT("\r\n"));
	File.Text = WithLineFeeds(MoveTemp(Text));
	return File;
}

bool CrowdyServerCodeFiles::Write(const FString& Path, const FString& Text, bool bCrlf)
{
	const FString Out = bCrlf ? Text.Replace(TEXT("\n"), TEXT("\r\n")) : Text;
	return FFileHelper::SaveStringToFile(Out, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

bool CrowdyServerCodeFiles::IsSame(const FDiskText& A, const FDiskText& B)
{
	return A.bExists == B.bExists && A.bReadable == B.bReadable && A.bUtf8 == B.bUtf8 && A.bCrlf == B.bCrlf
		&& A.Text.Equals(B.Text, ESearchCase::CaseSensitive);
}

FString CrowdyServerCodeFiles::WithLineFeeds(FString Text)
{
	Text.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
	Text.ReplaceInline(TEXT("\r"), TEXT("\n"));
	return Text;
}

CrowdyServerCodeFiles::FShownText CrowdyServerCodeFiles::ShowText(const FDiskText& OnDisk, const FEdits* Stashed)
{
	FShownText Shown;
	Shown.Text = Stashed ? Stashed->Text : OnDisk.Text;
	Shown.Base = Stashed ? Stashed->Base : OnDisk;
	Shown.bStale = Stashed && !IsSame(Stashed->Base, OnDisk);
	Shown.bDirty = !Shown.Text.Equals(Shown.Base.Text, ESearchCase::CaseSensitive);
	return Shown;
}

void CrowdyServerCodeFiles::FEditStash::Put(const FString& Path, FEdits Edits)
{
	Kept.Add(Path, MoveTemp(Edits));
}

TOptional<CrowdyServerCodeFiles::FEdits> CrowdyServerCodeFiles::FEditStash::Take(const FString& Path)
{
	FEdits Edits;
	if (!Kept.RemoveAndCopyValue(Path, Edits))
	{
		return {};
	}
	return MoveTemp(Edits);
}

void CrowdyServerCodeFiles::FEditStash::ForgetUnder(const FString& Directory)
{
	for (TMap<FString, FEdits>::TIterator It = Kept.CreateIterator(); It; ++It)
	{
		if (FPaths::IsUnderDirectory(It.Key(), Directory))
		{
			It.RemoveCurrent();
		}
	}
}

TArray<FString> CrowdyServerCodeFiles::FEditStash::Files() const
{
	TArray<FString> Files;
	Kept.GetKeys(Files);
	return Files;
}

CrowdyServerCodeFiles::ETypeNameProblem CrowdyServerCodeFiles::CheckTypeName(const FString& TypeName)
{
	using namespace CrowdyServerCodeFilesDetail;
	if (TypeName.IsEmpty())
	{
		return ETypeNameProblem::Empty;
	}
	if (TypeName.Len() > MaxTypeNameLength)
	{
		return ETypeNameProblem::TooLong;
	}
	if (!IsLowerAscii(TypeName[0]))
	{
		return ETypeNameProblem::FirstNotLetter;
	}
	for (const TCHAR Char : TypeName)
	{
		if (!IsLowerAscii(Char) && !IsDigitAscii(Char) && Char != '_')
		{
			return ETypeNameProblem::BadCharacter;
		}
	}
	return ETypeNameProblem::None;
}

bool CrowdyServerCodeFiles::IsValidTypeName(const FString& TypeName)
{
	return CheckTypeName(TypeName) == ETypeNameProblem::None;
}

FString CrowdyServerCodeFiles::SuggestTypeName(const FString& AssetName)
{
	using namespace CrowdyServerCodeFilesDetail;
	const int32 PrefixLength = AssetName.StartsWith(TEXT("CSO_"), ESearchCase::IgnoreCase) ? 4 : AssetName.StartsWith(TEXT("DA_"), ESearchCase::IgnoreCase) ? 3 : 0;
	const FString Name = AssetName.RightChop(PrefixLength);
	FString Out;
	for (int32 Index = 0; Index < Name.Len(); ++Index)
	{
		const TCHAR Char = Name[Index];
		const TCHAR Previous = Index > 0 ? Name[Index - 1] : TEXT('\0');
		const TCHAR Next = Index + 1 < Name.Len() ? Name[Index + 1] : TEXT('\0');
		if (IsUpperAscii(Char))
		{
			// A capital starts a word after a lowercase letter or digit, and at the end of a run of capitals: HTTPServer is http_server.
			const bool bWordStart = IsLowerAscii(Previous) || IsDigitAscii(Previous) || (IsUpperAscii(Previous) && IsLowerAscii(Next));
			Out += bWordStart ? TEXT("_") : TEXT("");
			Out.AppendChar(FChar::ToLower(Char));
			continue;
		}
		if (IsLowerAscii(Char) || IsDigitAscii(Char))
		{
			Out.AppendChar(Char);
			continue;
		}
		Out += Char == '_' || Char == ' ' || Char == '-' ? TEXT("_") : TEXT("");
	}
	while (Out.Contains(TEXT("__")))
	{
		Out.ReplaceInline(TEXT("__"), TEXT("_"));
	}
	int32 Start = 0;
	while (Start < Out.Len() && !IsLowerAscii(Out[Start]))
	{
		++Start;
	}
	Out.RightChopInline(Start);
	Out.LeftInline(MaxTypeNameLength);
	while (Out.EndsWith(TEXT("_")))
	{
		Out.LeftChopInline(1);
	}
	return Out;
}

FText CrowdyServerCodeFiles::DescribeTypeNameProblem(ETypeNameProblem Problem)
{
	switch (Problem)
	{
	case ETypeNameProblem::Empty: return LOCTEXT("TypeNameEmpty", "Needed: the type's name on the server.");
	case ETypeNameProblem::TooLong: return LOCTEXT("TypeNameTooLong", "At most 48 characters.");
	case ETypeNameProblem::FirstNotLetter: return LOCTEXT("TypeNameFirst", "Start with a lowercase letter.");
	case ETypeNameProblem::BadCharacter: return LOCTEXT("TypeNameCharacters", "Use only lowercase letters, digits and underscores.");
	default: return FText::GetEmpty();
	}
}

FText CrowdyServerCodeFiles::SharedTypeNameProblem()
{
	return LOCTEXT("TypeNameShared", "Another definition already uses this Type Name; give this one its own.");
}

bool CrowdyServerCodeFiles::IsSameCode(const FString& A, const FString& B)
{
	using namespace CrowdyServerCodeFilesDetail;
	return NormalizedCode(A).Equals(NormalizedCode(B), ESearchCase::CaseSensitive);
}

bool CrowdyServerCodeFiles::SetWatchedField(TArray<FName>& Watched, FName Field, bool bWatched)
{
	if (bWatched && Watched.Contains(Field))
	{
		return false;
	}
	if (bWatched)
	{
		Watched.Add(Field);
		return true;
	}
	return Watched.Remove(Field) > 0;
}

TArray<FName> CrowdyServerCodeFiles::FindMissingFields(TConstArrayView<FName> Watched, TConstArrayView<FName> StateFields)
{
	TArray<FName> Missing;
	for (const FName Name : Watched)
	{
		if (!StateFields.Contains(Name))
		{
			Missing.Add(Name);
		}
	}
	return Missing;
}

FText CrowdyServerCodeFiles::VersionText(int32 Version)
{
	return FText::AsNumber(Version, &FNumberFormattingOptions::DefaultNoGrouping());
}

FText CrowdyServerCodeFiles::TimeAgo(const FDateTime& Time, const FDateTime& Now)
{
	if (Time == FDateTime())
	{
		return LOCTEXT("TimeUnknown", "at an unknown time");
	}
	const FTimespan Age = Now - Time;
	// A time slightly ahead of this machine's clock reads as now; further ahead, as its date.
	if (Age.GetTotalSeconds() <= -60.0 || Age.GetTotalDays() >= 7.0)
	{
		return FText::FromString(Time.ToString(TEXT("%Y-%m-%d")));
	}
	if (Age.GetTotalMinutes() < 1.0)
	{
		return LOCTEXT("JustNow", "just now");
	}
	if (Age.GetTotalHours() < 1.0)
	{
		return FText::Format(LOCTEXT("MinutesAgo", "{0} min ago"), VersionText(FMath::FloorToInt(Age.GetTotalMinutes())));
	}
	if (Age.GetTotalDays() < 1.0)
	{
		return FText::Format(LOCTEXT("HoursAgo", "{0} h ago"), VersionText(FMath::FloorToInt(Age.GetTotalHours())));
	}
	return FText::Format(LOCTEXT("DaysAgo", "{0} d ago"), VersionText(FMath::FloorToInt(Age.GetTotalDays())));
}

FText CrowdyServerCodeFiles::RevisionLabel(int32 Version, bool bLive, const FDateTime& DeployedAt, const FDateTime& Now)
{
	const FText Name = Version > 0 ? FText::Format(LOCTEXT("RevisionVersion", "Version {0}"), VersionText(Version)) : LOCTEXT("RevisionNoVersion", "A deployed revision");
	const FText When = TimeAgo(DeployedAt, Now);
	return bLive ? FText::Format(LOCTEXT("RevisionLive", "{0} · live · {1}"), Name, When) : FText::Format(LOCTEXT("RevisionOlder", "{0} · {1}"), Name, When);
}

#undef LOCTEXT_NAMESPACE
