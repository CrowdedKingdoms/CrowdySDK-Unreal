#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyServerCodeFiles.h"
#include "CrowdyServerObjectDefinition.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

namespace CrowdyServerCodeFilesTest
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	struct FTempFolder
	{
		FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("CrowdyServerCodeFilesTests"), FGuid::NewGuid().ToString()));

		~FTempFolder()
		{
			IFileManager::Get().DeleteDirectory(*Path, false, true);
		}
	};

	TArray<uint8> LoadBytes(const FString& Path)
	{
		TArray<uint8> Bytes;
		FFileHelper::LoadFileToArray(Bytes, *Path);
		return Bytes;
	}

	bool SaveBytes(std::initializer_list<uint8> Bytes, const FString& Path)
	{
		const TArray<uint8> Data(Bytes);
		return FFileHelper::SaveArrayToFile(TArrayView64<const uint8>(Data), *Path);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerCodeFilesLineEndingsTest, "CrowdySDK.CrowdyExecEditor.ServerCodeFilesKeepLineEndings", CrowdyServerCodeFilesTest::TestFlags)
bool FCrowdyServerCodeFilesLineEndingsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerCodeFilesTest;
	FTempFolder Folder;
	const FString Path = FPaths::Combine(Folder.Path, TEXT("logic.rs"));

	const CrowdyServerCodeFiles::FDiskText Missing = CrowdyServerCodeFiles::Read(Path);
	TestFalse(TEXT("a missing file does not exist"), Missing.bExists);
	TestFalse(TEXT("a missing file is not readable"), Missing.bReadable);

	if (!TestTrue(TEXT("a CRLF file is written"), CrowdyServerCodeFiles::Write(Path, TEXT("fn a() {}\nfn b() {}\n"), true)))
	{
		return false;
	}
	const TArray<uint8> Crlf = LoadBytes(Path);
	TestTrue(TEXT("the file starts with the code, not a byte order mark"), Crlf.Num() > 0 && Crlf[0] == 'f');
	TestEqual(TEXT("each line ends with CRLF"), Crlf.Num(), 22);
	const CrowdyServerCodeFiles::FDiskText ReadCrlf = CrowdyServerCodeFiles::Read(Path);
	TestTrue(TEXT("the file is read"), ReadCrlf.bExists && ReadCrlf.bReadable);
	TestTrue(TEXT("the file is noted as CRLF"), ReadCrlf.bCrlf);
	TestTrue(TEXT("the file is noted as UTF-8"), ReadCrlf.bUtf8);
	TestEqualSensitive(TEXT("the text reads back with LF"), ReadCrlf.Text, FString(TEXT("fn a() {}\nfn b() {}\n")));

	CrowdyServerCodeFiles::Write(Path, TEXT("fn a() {}\n"), false);
	const CrowdyServerCodeFiles::FDiskText ReadLf = CrowdyServerCodeFiles::Read(Path);
	TestFalse(TEXT("an LF file is noted as LF"), ReadLf.bCrlf);
	TestEqual(TEXT("an LF file keeps LF"), LoadBytes(Path).Num(), 10);
	TestFalse(TEXT("a changed file is not the same as before"), CrowdyServerCodeFiles::IsSame(ReadCrlf, ReadLf));
	TestTrue(TEXT("two reads of one file are the same"), CrowdyServerCodeFiles::IsSame(ReadLf, CrowdyServerCodeFiles::Read(Path)));

	CrowdyServerCodeFiles::Write(Path, TEXT("fn a() {}\r\n"), false);
	TestFalse(TEXT("only the line endings changing is a change"), CrowdyServerCodeFiles::IsSame(ReadLf, CrowdyServerCodeFiles::Read(Path)));

	CrowdyServerCodeFiles::Write(Path, FString(TEXT("// café\n")), false);
	const TArray<uint8> Accented = LoadBytes(Path);
	TestTrue(TEXT("non-ASCII text is written as UTF-8"), Accented.Num() == 9 && Accented[6] == 0xC3 && Accented[7] == 0xA9);
	TestTrue(TEXT("UTF-8 text reads as UTF-8"), CrowdyServerCodeFiles::Read(Path).bUtf8);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerCodeFilesEncodingTest, "CrowdySDK.CrowdyExecEditor.ServerCodeFilesNoticeEncoding", CrowdyServerCodeFilesTest::TestFlags)
bool FCrowdyServerCodeFilesEncodingTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerCodeFilesTest;
	FTempFolder Folder;
	const FString Path = FPaths::Combine(Folder.Path, TEXT("logic.rs"));

	SaveBytes({0xEF, 0xBB, 0xBF, 'a', '\n'}, Path);
	const CrowdyServerCodeFiles::FDiskText Bom = CrowdyServerCodeFiles::Read(Path);
	TestTrue(TEXT("UTF-8 with a byte order mark is UTF-8"), Bom.bUtf8);
	TestEqualSensitive(TEXT("the byte order mark is not part of the text"), Bom.Text, FString(TEXT("a\n")));

	SaveBytes({'a', 0xFF, 'b', '\n'}, Path);
	const CrowdyServerCodeFiles::FDiskText Latin = CrowdyServerCodeFiles::Read(Path);
	TestTrue(TEXT("a file that is not UTF-8 is still read"), Latin.bReadable);
	TestFalse(TEXT("a file that is not UTF-8 is noted"), Latin.bUtf8);

	SaveBytes({0xFF, 0xFE, 'a', 0x00, '\n', 0x00}, Path);
	const CrowdyServerCodeFiles::FDiskText Utf16 = CrowdyServerCodeFiles::Read(Path);
	TestFalse(TEXT("UTF-16 is not UTF-8"), Utf16.bUtf8);
	TestEqualSensitive(TEXT("UTF-16 text is read"), Utf16.Text, FString(TEXT("a\n")));

	SaveBytes({}, Path);
	const CrowdyServerCodeFiles::FDiskText Empty = CrowdyServerCodeFiles::Read(Path);
	TestTrue(TEXT("an empty file exists, is read and is UTF-8"), Empty.bExists && Empty.bReadable && Empty.bUtf8 && Empty.Text.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerCodeFilesTypeNameTest, "CrowdySDK.CrowdyExecEditor.ServerCodeTypeNameRule", CrowdyServerCodeFilesTest::TestFlags)
bool FCrowdyServerCodeFilesTypeNameTest::RunTest(const FString& Parameters)
{
	for (const TCHAR* Valid : {TEXT("sbx_counter"), TEXT("a"), TEXT("a1_b2")})
	{
		TestTrue(FString::Printf(TEXT("%s is a valid Type Name"), Valid), CrowdyServerCodeFiles::IsValidTypeName(Valid));
	}
	TestTrue(TEXT("48 characters are allowed"), CrowdyServerCodeFiles::IsValidTypeName(FString::ChrN(48, TEXT('a'))));
	TestFalse(TEXT("49 characters are not"), CrowdyServerCodeFiles::IsValidTypeName(FString::ChrN(49, TEXT('a'))));
	for (const TCHAR* Invalid : {TEXT(""), TEXT(".."), TEXT("../escape"), TEXT("a/b"), TEXT("a\\b"), TEXT("a..b"), TEXT("Upper"), TEXT("1st"), TEXT("_lead"), TEXT("a b"), TEXT("a-b")})
	{
		TestFalse(FString::Printf(TEXT("'%s' is not a valid Type Name"), Invalid), CrowdyServerCodeFiles::IsValidTypeName(Invalid));
	}
	using ETypeNameProblem = CrowdyServerCodeFiles::ETypeNameProblem;
	TestTrue(TEXT("an empty Type Name is named as empty"), CrowdyServerCodeFiles::CheckTypeName(TEXT("")) == ETypeNameProblem::Empty);
	TestTrue(TEXT("a long Type Name is named as too long"), CrowdyServerCodeFiles::CheckTypeName(FString::ChrN(49, TEXT('a'))) == ETypeNameProblem::TooLong);
	TestTrue(TEXT("a leading digit is named as a bad start"), CrowdyServerCodeFiles::CheckTypeName(TEXT("1st")) == ETypeNameProblem::FirstNotLetter);
	TestTrue(TEXT("a leading capital is named as a bad start"), CrowdyServerCodeFiles::CheckTypeName(TEXT("Upper")) == ETypeNameProblem::FirstNotLetter);
	TestTrue(TEXT("a dash is named as a bad character"), CrowdyServerCodeFiles::CheckTypeName(TEXT("a-b")) == ETypeNameProblem::BadCharacter);
	TestTrue(TEXT("a valid Type Name has no problem"), CrowdyServerCodeFiles::CheckTypeName(TEXT("sbx_counter")) == ETypeNameProblem::None);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerCodeFilesSuggestTypeNameTest, "CrowdySDK.CrowdyExecEditor.ServerCodeSuggestTypeName", CrowdyServerCodeFilesTest::TestFlags)
bool FCrowdyServerCodeFilesSuggestTypeNameTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		const TCHAR* AssetName;
		const TCHAR* Expected;
	};
	const FCase Cases[] = {
		{TEXT("CSO_VillageBeacon"), TEXT("village_beacon")},
		{TEXT("CSO_Boss"), TEXT("boss")},
		{TEXT("cso_boss"), TEXT("boss")},
		{TEXT("CSO_"), TEXT("")},
		{TEXT("DA_SbxCounter"), TEXT("sbx_counter")},
		{TEXT("SbxCounter"), TEXT("sbx_counter")},
		{TEXT("da_boss"), TEXT("boss")},
		{TEXT("DA_HTTPServer"), TEXT("http_server")},
		{TEXT("DA_Boss2Counter"), TEXT("boss2_counter")},
		{TEXT("DA_Level10"), TEXT("level10")},
		{TEXT("DA_2FastCounter"), TEXT("fast_counter")},
		{TEXT("DA_My Boss-Fight"), TEXT("my_boss_fight")},
		{TEXT("DA___Boss__Fight_"), TEXT("boss_fight")},
		{TEXT("DA_Bo$ss"), TEXT("boss")},
		{TEXT(""), TEXT("")},
		{TEXT("DA_"), TEXT("")},
		{TEXT("DA_123"), TEXT("")},
	};
	for (const FCase& Case : Cases)
	{
		TestEqualSensitive(FString::Printf(TEXT("'%s' suggests '%s'"), Case.AssetName, Case.Expected), CrowdyServerCodeFiles::SuggestTypeName(Case.AssetName), FString(Case.Expected));
	}
	const FString Long = CrowdyServerCodeFiles::SuggestTypeName(TEXT("DA_") + FString::ChrN(47, TEXT('A')).ToLower() + TEXT("Bcd"));
	TestEqualSensitive(TEXT("a long name is cut to 48 characters without a trailing underscore"), Long, FString::ChrN(47, TEXT('a')));
	TestEqual(TEXT("a long name without word breaks is cut to 48"), CrowdyServerCodeFiles::SuggestTypeName(FString::ChrN(60, TEXT('b'))).Len(), 48);
	for (const TCHAR* AssetName : {TEXT("CSO_SbxCounter"), TEXT("DA_SbxCounter"), TEXT("DA_2FastCounter"), TEXT("DA_My Boss-Fight"), TEXT("Weird__Name$$42")})
	{
		TestTrue(FString::Printf(TEXT("the suggestion for '%s' is a valid Type Name"), AssetName), CrowdyServerCodeFiles::IsValidTypeName(CrowdyServerCodeFiles::SuggestTypeName(AssetName)));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerCodeFilesSameCodeTest, "CrowdySDK.CrowdyExecEditor.ServerCodeSameCodeIgnoresLineEnds", CrowdyServerCodeFilesTest::TestFlags)
bool FCrowdyServerCodeFilesSameCodeTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("CRLF and LF are the same code"), CrowdyServerCodeFiles::IsSameCode(TEXT("a\r\nb\r\n"), TEXT("a\nb\n")));
	TestTrue(TEXT("whitespace at the end of a line is ignored"), CrowdyServerCodeFiles::IsSameCode(TEXT("a  \nb\t\n"), TEXT("a\nb")));
	TestTrue(TEXT("blank lines at the end are ignored"), CrowdyServerCodeFiles::IsSameCode(TEXT("a\n\n\n"), TEXT("a")));
	TestFalse(TEXT("indentation is not ignored"), CrowdyServerCodeFiles::IsSameCode(TEXT("  a\n"), TEXT("a\n")));
	TestFalse(TEXT("a changed line is a change"), CrowdyServerCodeFiles::IsSameCode(TEXT("a\nb\n"), TEXT("a\nc\n")));
	TestFalse(TEXT("a blank line in the middle is a change"), CrowdyServerCodeFiles::IsSameCode(TEXT("a\n\nb"), TEXT("a\nb")));
	TestFalse(TEXT("case is a change"), CrowdyServerCodeFiles::IsSameCode(TEXT("A"), TEXT("a")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerCodeFilesWatchedFieldsTest, "CrowdySDK.CrowdyExecEditor.ServerCodeWatchedFieldToggles", CrowdyServerCodeFilesTest::TestFlags)
bool FCrowdyServerCodeFilesWatchedFieldsTest::RunTest(const FString& Parameters)
{
	TArray<FName> Watched;
	TestTrue(TEXT("watching a new field changes the list"), CrowdyServerCodeFiles::SetWatchedField(Watched, TEXT("Health"), true));
	TestFalse(TEXT("watching it again changes nothing"), CrowdyServerCodeFiles::SetWatchedField(Watched, TEXT("Health"), true));
	TestFalse(TEXT("watching it in another case changes nothing"), CrowdyServerCodeFiles::SetWatchedField(Watched, TEXT("health"), true));
	TestEqual(TEXT("the field is listed once"), Watched.Num(), 1);
	TestTrue(TEXT("unwatching a field not listed changes nothing"), !CrowdyServerCodeFiles::SetWatchedField(Watched, TEXT("Armor"), false));
	Watched.Add(TEXT("HEALTH"));
	TestTrue(TEXT("unwatching changes the list"), CrowdyServerCodeFiles::SetWatchedField(Watched, TEXT("Health"), false));
	TestEqual(TEXT("unwatching removes every spelling"), Watched.Num(), 0);

	const TArray<FName> Listed = {TEXT("Health"), TEXT("Gone"), TEXT("phase"), TEXT("Old")};
	const TArray<FName> Fields = {TEXT("Health"), TEXT("Phase")};
	const TArray<FName> Missing = CrowdyServerCodeFiles::FindMissingFields(Listed, Fields);
	if (TestEqual(TEXT("two watched names are not State fields"), Missing.Num(), 2))
	{
		TestEqual(TEXT("the first missing name, in order"), Missing[0].ToString(), FString(TEXT("Gone")));
		TestEqual(TEXT("the second missing name, in order"), Missing[1].ToString(), FString(TEXT("Old")));
	}
	TestEqual(TEXT("with no State every watched name is missing"), CrowdyServerCodeFiles::FindMissingFields(Listed, TArray<FName>()).Num(), 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerCodeFilesSetFieldAudienceTest, "CrowdySDK.CrowdyExecEditor.ServerCodeFilesSetFieldAudience", CrowdyServerCodeFilesTest::TestFlags)
bool FCrowdyServerCodeFilesSetFieldAudienceTest::RunTest(const FString& Parameters)
{
	using CrowdyServerCodeFiles::GetFieldAudience;
	using CrowdyServerCodeFiles::SetFieldAudience;
	TArray<FName> Watched;
	TArray<FName> Owner;
	TArray<FName> Team;
	const FName Health(TEXT("Health"));
	TestFalse(TEXT("a variable in no list is Server Only"), GetFieldAudience(Watched, Owner, Team, Health).IsSet());
	TestFalse(TEXT("Server Only for a variable already server only changes nothing"), SetFieldAudience(Watched, Owner, Team, Health, TOptional<ECrowdyServerFieldAudience>()));

	TestTrue(TEXT("Server Only to Everyone changes the lists"), SetFieldAudience(Watched, Owner, Team, Health, ECrowdyServerFieldAudience::Everyone));
	TestTrue(TEXT("and reads back as Everyone"), GetFieldAudience(Watched, Owner, Team, Health) == ECrowdyServerFieldAudience::Everyone);
	TestTrue(TEXT("it is only in Visible to Players"), Watched.Contains(Health) && Owner.IsEmpty() && Team.IsEmpty());
	TestFalse(TEXT("Everyone again changes nothing"), SetFieldAudience(Watched, Owner, Team, Health, ECrowdyServerFieldAudience::Everyone));

	TestTrue(TEXT("Everyone to Owner changes the lists"), SetFieldAudience(Watched, Owner, Team, Health, ECrowdyServerFieldAudience::Owner));
	TestTrue(TEXT("and reads back as Owner"), GetFieldAudience(Watched, Owner, Team, Health) == ECrowdyServerFieldAudience::Owner);
	TestTrue(TEXT("it is in Visible to Players and Visible to Owner Only"), Watched.Contains(Health) && Owner.Contains(Health) && Team.IsEmpty());
	TestFalse(TEXT("Owner again changes nothing"), SetFieldAudience(Watched, Owner, Team, Health, ECrowdyServerFieldAudience::Owner));

	TestTrue(TEXT("Owner to Team changes the lists"), SetFieldAudience(Watched, Owner, Team, Health, ECrowdyServerFieldAudience::Team));
	TestTrue(TEXT("and reads back as Team"), GetFieldAudience(Watched, Owner, Team, Health) == ECrowdyServerFieldAudience::Team);
	TestTrue(TEXT("it left Visible to Owner Only"), Watched.Contains(Health) && Owner.IsEmpty() && Team.Contains(Health));

	TestTrue(TEXT("Team to Server Only changes the lists"), SetFieldAudience(Watched, Owner, Team, Health, TOptional<ECrowdyServerFieldAudience>()));
	TestFalse(TEXT("and reads back as Server Only"), GetFieldAudience(Watched, Owner, Team, Health).IsSet());
	TestTrue(TEXT("it is in no list"), Watched.IsEmpty() && Owner.IsEmpty() && Team.IsEmpty());

	Watched = {TEXT("HEALTH")};
	Owner = {TEXT("health")};
	TestTrue(TEXT("the spelling does not matter"), GetFieldAudience(Watched, Owner, Team, Health) == ECrowdyServerFieldAudience::Owner);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerCodeFilesSetVisibleToPlayersTest, "CrowdySDK.CrowdyExecEditor.ServerCodeFilesSetVisibleToPlayers", CrowdyServerCodeFilesTest::TestFlags)
bool FCrowdyServerCodeFilesSetVisibleToPlayersTest::RunTest(const FString& Parameters)
{
	using CrowdyServerCodeFiles::SetVisibleToPlayers;
	const FName Gold(TEXT("Gold"));
	TArray<FName> Watched = {Gold};
	TArray<FName> Owner = {Gold};
	TArray<FName> Team;
	TestFalse(TEXT("showing a variable already shown changes nothing"), SetVisibleToPlayers(Watched, Owner, Team, Gold, true));
	TestTrue(TEXT("and keeps its audience"), Owner.Contains(Gold));

	TestTrue(TEXT("hiding an Owner Only variable changes the lists"), SetVisibleToPlayers(Watched, Owner, Team, Gold, false));
	TestTrue(TEXT("it leaves every list, so no hidden audience is left behind"), Watched.IsEmpty() && Owner.IsEmpty() && Team.IsEmpty());

	Watched = {Gold};
	Team = {Gold};
	TestTrue(TEXT("hiding a Team Only variable changes the lists"), SetVisibleToPlayers(Watched, Owner, Team, Gold, false));
	TestTrue(TEXT("it leaves Team Only too"), Watched.IsEmpty() && Team.IsEmpty());

	TestTrue(TEXT("showing a hidden variable changes the lists"), SetVisibleToPlayers(Watched, Owner, Team, Gold, true));
	TestTrue(TEXT("it comes back for everyone"), Watched.Contains(Gold) && Owner.IsEmpty() && Team.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerCodeFilesRevisionLabelTest, "CrowdySDK.CrowdyExecEditor.ServerCodeRevisionLabel", CrowdyServerCodeFilesTest::TestFlags)
bool FCrowdyServerCodeFilesRevisionLabelTest::RunTest(const FString& Parameters)
{
	const FDateTime Now(2026, 9, 29, 14, 34, 0);
	TestEqualSensitive(TEXT("the live revision"), CrowdyServerCodeFiles::RevisionLabel(4, true, Now - FTimespan::FromHours(2.5), Now).ToString(), FString(TEXT("Version 4 · live · 2 h ago")));
	TestEqualSensitive(TEXT("an older revision"), CrowdyServerCodeFiles::RevisionLabel(3, false, Now - FTimespan::FromHours(25.0), Now).ToString(), FString(TEXT("Version 3 · 1 d ago")));
	TestEqualSensitive(TEXT("large versions are not grouped"), CrowdyServerCodeFiles::RevisionLabel(12345, false, Now, Now).ToString(), FString(TEXT("Version 12345 · just now")));
	TestEqualSensitive(TEXT("minutes"), CrowdyServerCodeFiles::TimeAgo(Now - FTimespan::FromMinutes(5.5), Now).ToString(), FString(TEXT("5 min ago")));
	TestEqualSensitive(TEXT("a week or more reads as the date"), CrowdyServerCodeFiles::TimeAgo(FDateTime(2026, 9, 1), Now).ToString(), FString(TEXT("2026-09-01")));
	TestEqualSensitive(TEXT("a time a little ahead of the clock reads as now"), CrowdyServerCodeFiles::TimeAgo(Now + FTimespan::FromSeconds(30.0), Now).ToString(), FString(TEXT("just now")));
	TestEqualSensitive(TEXT("a time well ahead of the clock reads as the date"), CrowdyServerCodeFiles::TimeAgo(FDateTime(2026, 10, 2), Now).ToString(), FString(TEXT("2026-10-02")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerCodeFilesShowTextTest, "CrowdySDK.CrowdyExecEditor.ServerCodeShowsKeptEdits", CrowdyServerCodeFilesTest::TestFlags)
bool FCrowdyServerCodeFilesShowTextTest::RunTest(const FString& Parameters)
{
	using CrowdyServerCodeFiles::FDiskText;
	using CrowdyServerCodeFiles::FEdits;
	using CrowdyServerCodeFiles::FShownText;
	FDiskText OnDisk;
	OnDisk.bExists = true;
	OnDisk.bReadable = true;
	OnDisk.Text = TEXT("fn a() {}\n");

	const FShownText Plain = CrowdyServerCodeFiles::ShowText(OnDisk, nullptr);
	TestEqualSensitive(TEXT("with no kept edits the file is shown"), Plain.Text, OnDisk.Text);
	TestTrue(TEXT("with no kept edits the base is the file read"), CrowdyServerCodeFiles::IsSame(Plain.Base, OnDisk));
	TestFalse(TEXT("the file as read is not unsaved"), Plain.bDirty);
	TestFalse(TEXT("the file as read is not stale"), Plain.bStale);

	const FEdits Edits{TEXT("fn a() { b() }\n"), OnDisk};
	const FShownText Restored = CrowdyServerCodeFiles::ShowText(OnDisk, &Edits);
	TestEqualSensitive(TEXT("kept edits are shown"), Restored.Text, Edits.Text);
	TestTrue(TEXT("kept edits are unsaved"), Restored.bDirty);
	TestFalse(TEXT("kept edits to an unchanged file are not stale"), Restored.bStale);

	FDiskText Changed = OnDisk;
	Changed.Text = TEXT("fn c() {}\n");
	const FShownText Stale = CrowdyServerCodeFiles::ShowText(Changed, &Edits);
	TestEqualSensitive(TEXT("kept edits are shown over a file changed since"), Stale.Text, Edits.Text);
	TestTrue(TEXT("edits made before the file changed on disk are stale"), Stale.bStale);
	TestTrue(TEXT("a save checks the disk against the file the edits started from"), CrowdyServerCodeFiles::IsSame(Stale.Base, OnDisk));
	TestTrue(TEXT("edits to a file deleted since are stale"), CrowdyServerCodeFiles::ShowText(FDiskText(), &Edits).bStale);

	const FEdits Undone{OnDisk.Text, OnDisk};
	TestFalse(TEXT("edits undone back to the file are not unsaved"), CrowdyServerCodeFiles::ShowText(OnDisk, &Undone).bDirty);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerCodeFilesEditStashTest, "CrowdySDK.CrowdyExecEditor.ServerCodeEditStashKeepsNewest", CrowdyServerCodeFilesTest::TestFlags)
bool FCrowdyServerCodeFilesEditStashTest::RunTest(const FString& Parameters)
{
	using CrowdyServerCodeFiles::FEdits;
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("CrowdyServerCodeFilesTests")));
	const FString Counter = FPaths::Combine(Root, TEXT("Server/counter/src/logic.rs"));
	const FString CounterTwo = FPaths::Combine(Root, TEXT("Server/counter_two/src/logic.rs"));
	const FString Boss = FPaths::Combine(Root, TEXT("Server/boss/src/logic.rs"));

	CrowdyServerCodeFiles::FEditStash Stash;
	Stash.Put(Counter, FEdits{TEXT("older"), {}});
	Stash.Put(Counter, FEdits{TEXT("newer"), {}});
	const TOptional<FEdits> Taken = Stash.Take(Counter);
	if (TestTrue(TEXT("edits kept for a file are taken"), Taken.IsSet()))
	{
		TestEqualSensitive(TEXT("the newest edits win"), Taken->Text, FString(TEXT("newer")));
	}
	TestFalse(TEXT("taking the edits removes them, so they come back once"), Stash.Contains(Counter));
	TestFalse(TEXT("a file with nothing kept gives nothing"), Stash.Take(Boss).IsSet());

	Stash.Put(Counter, FEdits{TEXT("a"), {}});
	Stash.Put(CounterTwo, FEdits{TEXT("b"), {}});
	Stash.Put(Boss, FEdits{TEXT("c"), {}});
	Stash.ForgetUnder(FPaths::Combine(Root, TEXT("Server/counter")));
	TestFalse(TEXT("a file in the forgotten folder is dropped"), Stash.Contains(Counter));
	TestTrue(TEXT("a folder whose name only starts the same is kept"), Stash.Contains(CounterTwo));
	TestTrue(TEXT("another folder is kept"), Stash.Contains(Boss));
	TestEqual(TEXT("the kept files are listed"), Stash.Files().Num(), 2);
	Stash.Forget(Boss);
	TestFalse(TEXT("a forgotten file is dropped"), Stash.Contains(Boss));
	return true;
}

#endif
