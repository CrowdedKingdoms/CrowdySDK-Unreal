#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyExecDeploy.h"
#include "CrowdyExecRevisions.h"
#include "CrowdyServerObjectDefinition.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

namespace CrowdyExecRevisionsTest
{
	using CrowdyExecDeveloper::FBuildArtifact;
	using CrowdyExecDeveloper::FBuildCrate;
	using CrowdyExecDeveloper::FBuildFile;
	using CrowdyExecRevisions::EChange;
	using CrowdyExecRevisions::FDiffLine;
	using CrowdyExecRevisions::FRevision;
	using CrowdyExecRevisions::FTypeChange;

	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	/** Version 4 of the Server Compute live gate's app, as the platform answered it. */
	const TCHAR* const SampleManifest = TEXT("{\"root\":\"root\",\"types\":{\"root\":{\"kind\":\"hub\",\"calls\":[],\"client\":false,")
		TEXT("\"digest\":\"1dedc54f2759c4b0c746e9a0943f86eff5363217d6fa616231997d4ca8089f45\",\"parent\":null,\"mailbox\":null,\"replicas\":null,")
		TEXT("\"memory_mb\":null,\"concurrency\":null,\"deadline_ms\":null,\"fuel_per_call\":null,\"evict_after_ms\":null,\"persist_every_ms\":null},")
		TEXT("\"sbx_locker\":{\"kind\":\"hub\",\"calls\":[],\"client\":true,\"digest\":\"82117ae1295f3468124b0ad099ecc5f365e39af551b7fd1fa7fd05a786a36b0b\",")
		TEXT("\"parent\":\"root\",\"mailbox\":null,\"replicas\":null,\"memory_mb\":null,\"concurrency\":null,\"deadline_ms\":null,\"fuel_per_call\":null,")
		TEXT("\"evict_after_ms\":300000,\"persist_every_ms\":5000},\"sbx_counter\":{\"kind\":\"hub\",\"calls\":[],\"client\":true,")
		TEXT("\"digest\":\"49818bf390cf07d7cebb4b896da90eb8b736a2652ff21d1df75fefa6ea5a18ff\",\"parent\":\"root\",\"mailbox\":null,\"replicas\":null,")
		TEXT("\"memory_mb\":null,\"concurrency\":null,\"deadline_ms\":null,\"fuel_per_call\":null,\"evict_after_ms\":300000,\"persist_every_ms\":5000}}}");

	const TCHAR* ChangeName(EChange Change)
	{
		switch (Change)
		{
		case EChange::Unchanged: return TEXT("Unchanged");
		case EChange::Changed: return TEXT("Changed");
		case EChange::New: return TEXT("New");
		case EChange::Unknown: return TEXT("Unknown");
		case EChange::Removed: return TEXT("Removed");
		default: return TEXT("NotCompared");
		}
	}

	FBuildCrate MakeCrate(const TCHAR* Name, const TCHAR* Logic)
	{
		FBuildCrate Crate;
		Crate.Name = Name;
		Crate.Files.Add(FBuildFile{TEXT("Cargo.toml"), FString::Printf(TEXT("[package]\nname = \"%s\"\n"), Name)});
		Crate.Files.Add(FBuildFile{TEXT("src/logic.rs"), Logic});
		return Crate;
	}

	FRevision MakeRevision(const FBuildCrate& Crate, const TCHAR* Digest, int32 Version)
	{
		FRevision Revision;
		Revision.Fingerprint = CrowdyExecRevisions::Fingerprint(Crate.Files);
		Revision.Digests.Add(Digest);
		Revision.Versions.Add(Version);
		Revision.Files = Crate.Files;
		return Revision;
	}

	TStrongObjectPtr<UCrowdyServerObjectDefinition> MakeDefinition(const TCHAR* TypeName, int32 SaveIntervalSeconds, int32 IdleTimeoutSeconds)
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
		Definition->TypeName = TypeName;
		Definition->SaveIntervalSeconds = SaveIntervalSeconds;
		Definition->IdleTimeoutSeconds = IdleTimeoutSeconds;
		return Definition;
	}

	struct FLiveRow
	{
		const TCHAR* Name;
		const TCHAR* Digest;
		/** 0 writes null, as the platform does for a value it was not given. */
		int64 SaveIntervalMs;
		int64 IdleTimeoutMs;
	};

	FString Millis(int64 Value)
	{
		return Value == 0 ? FString(TEXT("null")) : FString::Printf(TEXT("%lld"), Value);
	}

	/** A manifest in the platform's shape: the root first, then each row. */
	FString MakeLiveManifest(TConstArrayView<FLiveRow> Rows)
	{
		FString Json = TEXT("{\"root\":\"root\",\"types\":{\"root\":{\"kind\":\"hub\",\"digest\":\"root_digest\",\"parent\":null,\"evict_after_ms\":null,\"persist_every_ms\":null}");
		for (const FLiveRow& Row : Rows)
		{
			Json += FString::Printf(TEXT(",\"%s\":{\"kind\":\"hub\",\"digest\":\"%s\",\"parent\":\"root\",\"evict_after_ms\":%s,\"persist_every_ms\":%s}"),
				Row.Name, Row.Digest, *Millis(Row.IdleTimeoutMs), *Millis(Row.SaveIntervalMs));
		}
		return Json + TEXT("}}");
	}

	CrowdyExecDeploy::FTypeState MakeTypeState(const TCHAR* TypeName, CrowdyExecDeploy::ECrateState State)
	{
		CrowdyExecDeploy::FTypeState Type;
		Type.TypeName = TypeName;
		Type.State = State;
		return Type;
	}

	const FTypeChange* FindChange(TConstArrayView<FTypeChange> Changes, const TCHAR* TypeName)
	{
		return Changes.FindByPredicate([TypeName](const FTypeChange& Change) { return Change.TypeName.Equals(TypeName, ESearchCase::CaseSensitive); });
	}

	void TestChange(FAutomationTestBase& Test, const TCHAR* Case, TConstArrayView<FTypeChange> Changes, const TCHAR* TypeName, EChange Expected, const TCHAR* What = TEXT(""))
	{
		const FTypeChange* Change = FindChange(Changes, TypeName);
		if (!Test.TestNotNull(*FString::Printf(TEXT("%s: %s is listed"), Case, TypeName), Change))
		{
			return;
		}
		Test.TestEqualSensitive(FString::Printf(TEXT("%s: %s's change"), Case, TypeName), FString(ChangeName(Change->Change)), FString(ChangeName(Expected)));
		Test.TestEqualSensitive(FString::Printf(TEXT("%s: what differs in %s"), Case, TypeName), FString::Join(Change->What, TEXT(", ")), FString(What));
	}

	/** "=a|-b|+c": each line's kind and text, in order. */
	FString Render(TConstArrayView<FDiffLine> Lines)
	{
		TArray<FString> Parts;
		for (const FDiffLine& Line : Lines)
		{
			const TCHAR* Kind = Line.Kind == FDiffLine::EKind::Same ? TEXT("=") : (Line.Kind == FDiffLine::EKind::Added ? TEXT("+") : TEXT("-"));
			Parts.Add(Kind + Line.Text);
		}
		return FString::Join(Parts, TEXT("|"));
	}

	int32 CountKind(TConstArrayView<FDiffLine> Lines, FDiffLine::EKind Kind)
	{
		int32 Count = 0;
		for (const FDiffLine& Line : Lines)
		{
			Count += Line.Kind == Kind ? 1 : 0;
		}
		return Count;
	}

	/** One line "x", then Count lines only Old has; New has Count lines of its own, then "x". */
	void MakeShiftedTexts(int32 Count, FString& OutOld, FString& OutNew)
	{
		OutOld = TEXT("x\n");
		OutNew.Reset();
		for (int32 Index = 0; Index < Count; ++Index)
		{
			OutOld += FString::Printf(TEXT("old %d\n"), Index);
			OutNew += FString::Printf(TEXT("new %d\n"), Index);
		}
		OutNew += TEXT("x\n");
	}

	struct FTempFolder
	{
		FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("CrowdyExecRevisionsTests"), FGuid::NewGuid().ToString()));

		~FTempFolder()
		{
			IFileManager::Get().DeleteDirectory(*Path, false, true);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecRevisionsFingerprintTest, "CrowdySDK.CrowdyExecEditor.RevisionsFingerprintIgnoresLineEndingsAndTrailingSpace", CrowdyExecRevisionsTest::TestFlags)
bool FCrowdyExecRevisionsFingerprintTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecRevisionsTest;
	const TArray<FBuildFile> Files = {{TEXT("Cargo.toml"), TEXT("[package]\nname = \"t\"\n")}, {TEXT("src/logic.rs"), TEXT("fn a() {}\nfn b() {}\n")}};
	const FString Print = CrowdyExecRevisions::Fingerprint(Files);
	TestEqual(TEXT("a fingerprint is a SHA-1 in hex"), Print.Len(), 40);
	TestEqualSensitive(TEXT("the same files give the same fingerprint"), CrowdyExecRevisions::Fingerprint(Files), Print);

	const TArray<FBuildFile> Crlf = {{TEXT("Cargo.toml"), TEXT("[package]\r\nname = \"t\"\r\n")}, {TEXT("src/logic.rs"), TEXT("fn a() {}\r\nfn b() {}\r\n")}};
	TestEqualSensitive(TEXT("CRLF line endings do not change the fingerprint"), CrowdyExecRevisions::Fingerprint(Crlf), Print);

	const TArray<FBuildFile> Trailing = {{TEXT("Cargo.toml"), TEXT("[package]\nname = \"t\"")}, {TEXT("src/logic.rs"), TEXT("fn a() {}\nfn b() {}\n\n  \t\n")}};
	TestEqualSensitive(TEXT("whitespace at the end of a file does not change the fingerprint"), CrowdyExecRevisions::Fingerprint(Trailing), Print);

	const TArray<FBuildFile> Reordered = {Files[1], Files[0]};
	TestEqualSensitive(TEXT("the files' order does not change the fingerprint"), CrowdyExecRevisions::Fingerprint(Reordered), Print);

	const TArray<FBuildFile> Edited = {Files[0], {TEXT("src/logic.rs"), TEXT("fn a() {}\nfn c() {}\n")}};
	TestFalse(TEXT("an edit changes the fingerprint"), CrowdyExecRevisions::Fingerprint(Edited).Equals(Print, ESearchCase::CaseSensitive));

	const TArray<FBuildFile> Indented = {Files[0], {TEXT("src/logic.rs"), TEXT("fn a() {}\n  fn b() {}\n")}};
	TestFalse(TEXT("whitespace inside a file changes the fingerprint"), CrowdyExecRevisions::Fingerprint(Indented).Equals(Print, ESearchCase::CaseSensitive));

	const TArray<FBuildFile> Moved = {Files[0], {TEXT("src/other.rs"), Files[1].Content}};
	TestFalse(TEXT("a file's path is part of the fingerprint"), CrowdyExecRevisions::Fingerprint(Moved).Equals(Print, ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecRevisionsAddTest, "CrowdySDK.CrowdyExecEditor.RevisionsAddPutsNewestFirstAndKeepsTheCap", CrowdyExecRevisionsTest::TestFlags)
bool FCrowdyExecRevisionsAddTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecRevisionsTest;
	const FDateTime Then(2026, 9, 1);
	const FDateTime Later(2026, 9, 2);
	const FBuildCrate One = MakeCrate(TEXT("t"), TEXT("fn one() {}\n"));
	const FBuildCrate Two = MakeCrate(TEXT("t"), TEXT("fn two() {}\n"));
	const FBuildCrate Three = MakeCrate(TEXT("t"), TEXT("fn three() {}\n"));

	TArray<FRevision> History;
	CrowdyExecRevisions::AddRevision(History, One, TEXT("d1"), 1, Then, 3);
	CrowdyExecRevisions::AddRevision(History, Two, TEXT("d2"), 2, Then, 3);
	CrowdyExecRevisions::AddRevision(History, Three, TEXT("d3"), 3, Then, 3);
	if (!TestEqual(TEXT("three deploys make three revisions"), History.Num(), 3))
	{
		return false;
	}
	TestEqualSensitive(TEXT("the newest revision comes first"), History[0].Fingerprint, CrowdyExecRevisions::Fingerprint(Three.Files));
	TestEqualSensitive(TEXT("the oldest revision comes last"), History[2].Fingerprint, CrowdyExecRevisions::Fingerprint(One.Files));

	// The middle revision, so a copy left behind would sit inside the cap rather than be trimmed with the oldest.
	FBuildCrate TwoCrlf = Two;
	TwoCrlf.Files[1].Content = TEXT("fn two() {}\r\n");
	CrowdyExecRevisions::AddRevision(History, TwoCrlf, TEXT("d4"), 4, Later, 3);
	if (!TestEqual(TEXT("code deployed again is the same revision, not another"), History.Num(), 3))
	{
		return false;
	}
	const FRevision& Again = History[0];
	TestEqualSensitive(TEXT("code deployed again moves first"), Again.Fingerprint, CrowdyExecRevisions::Fingerprint(Two.Files));
	TestEqualSensitive(TEXT("it gains the new version, newest first"), FString::JoinBy(Again.Versions, TEXT(","), [](int32 Version) { return FString::FromInt(Version); }), FString(TEXT("4,2")));
	TestEqualSensitive(TEXT("it gains the new digest, newest first"), FString::Join(Again.Digests, TEXT(",")), FString(TEXT("d4,d2")));
	TestTrue(TEXT("it is dated by its latest deploy"), Again.DeployedAt == Later);
	TestEqualSensitive(TEXT("it keeps the files as last sent"), Again.Files[1].Content, TwoCrlf.Files[1].Content);
	TestEqualSensitive(TEXT("the others keep their order"), History[1].Fingerprint, CrowdyExecRevisions::Fingerprint(Three.Files));
	TestEqualSensitive(TEXT("the oldest stays last, with no copy left behind"), History[2].Fingerprint, CrowdyExecRevisions::Fingerprint(One.Files));

	CrowdyExecRevisions::AddRevision(History, Three, TEXT("d3"), 5, Later, 3);
	TestEqualSensitive(TEXT("a digest seen again is not listed twice"), FString::Join(History[0].Digests, TEXT(",")), FString(TEXT("d3")));

	for (int32 Version = 100; Version < 130; ++Version)
	{
		CrowdyExecRevisions::AddRevision(History, Three, FString::Printf(TEXT("d%d"), Version), Version, Later, 3);
	}
	TestEqual(TEXT("a revision deployed again and again lists only its newest versions"), History[0].Versions.Num(), 20);
	TestEqual(TEXT("and only its newest digests"), History[0].Digests.Num(), 20);
	TestEqual(TEXT("the newest version is kept first"), History[0].Versions[0], 129);

	CrowdyExecRevisions::AddRevision(History, MakeCrate(TEXT("t"), TEXT("fn four() {}\n")), TEXT("d6"), 6, Later, 2);
	TestEqual(TEXT("only the Keep newest are kept"), History.Num(), 2);
	TestEqualSensitive(TEXT("the one kept last is the next newest"), History[1].Fingerprint, CrowdyExecRevisions::Fingerprint(Three.Files));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecRevisionsSaveLoadTest, "CrowdySDK.CrowdyExecEditor.RevisionsSaveAndLoadRoundTrip", CrowdyExecRevisionsTest::TestFlags)
bool FCrowdyExecRevisionsSaveLoadTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecRevisionsTest;
	const FTempFolder Folder;
	FBuildCrate Tricky = MakeCrate(TEXT("t"), TEXT("let s = \"quoted\\n\";\r\n\tlet e = 'é';\r\n"));
	TArray<FRevision> Saved;
	Saved.Add(MakeRevision(Tricky, TEXT("d2"), 2));
	Saved[0].Versions.Add(1);
	Saved[0].DeployedAt = FDateTime(2026, 9, 29, 19, 32, 36, 125);
	Saved.Add(MakeRevision(MakeCrate(TEXT("t"), TEXT("fn old() {}\n")), TEXT("d0"), 0));
	FString Error;
	if (!TestTrue(FString::Printf(TEXT("the history is saved (%s)"), *Error), CrowdyExecRevisions::SaveHistory(Folder.Path, Saved, Error)))
	{
		return false;
	}
	TestTrue(TEXT("the history is kept in revisions.json in the crate folder"), FPaths::FileExists(FPaths::Combine(Folder.Path, TEXT("revisions.json"))));

	TArray<FRevision> Loaded;
	if (!TestTrue(FString::Printf(TEXT("the history loads (%s)"), *Error), CrowdyExecRevisions::LoadHistory(Folder.Path, Loaded, Error))
		|| !TestEqual(TEXT("every revision comes back"), Loaded.Num(), 2))
	{
		return false;
	}
	TestEqualSensitive(TEXT("the fingerprint comes back"), Loaded[0].Fingerprint, Saved[0].Fingerprint);
	TestEqualSensitive(TEXT("the versions come back in order"), FString::JoinBy(Loaded[0].Versions, TEXT(","), [](int32 Version) { return FString::FromInt(Version); }), FString(TEXT("2,1")));
	TestEqualSensitive(TEXT("the digests come back"), FString::Join(Loaded[0].Digests, TEXT(",")), FString(TEXT("d2")));
	TestTrue(TEXT("the deploy time comes back"), Loaded[0].DeployedAt == Saved[0].DeployedAt);
	if (!TestEqual(TEXT("every file comes back"), Loaded[0].Files.Num(), Tricky.Files.Num()))
	{
		return false;
	}
	for (int32 Index = 0; Index < Tricky.Files.Num(); ++Index)
	{
		TestEqualSensitive(TEXT("a file's path comes back"), Loaded[0].Files[Index].Path, Tricky.Files[Index].Path);
		TestEqualSensitive(TEXT("a file's text comes back exactly, quotes, tabs and line endings included"), Loaded[0].Files[Index].Content, Tricky.Files[Index].Content);
	}
	const FBuildFile* Logic = Loaded[0].FindFile(TEXT("src/logic.rs"));
	TestTrue(TEXT("a revision's file is found by its path"), Logic && Logic->Content.Equals(Tricky.Files[1].Content, ESearchCase::CaseSensitive));
	TestNull(TEXT("a path the revision does not have finds nothing"), Loaded[0].FindFile(TEXT("src/LOGIC.rs")));
	TestEqualSensitive(TEXT("the second revision comes back second"), Loaded[1].Fingerprint, Saved[1].Fingerprint);

	FString Text;
	FFileHelper::LoadFileToString(Text, *CrowdyExecRevisions::GetHistoryPath(Folder.Path));
	TestFalse(TEXT("the file has LF line endings on every platform"), Text.Contains(TEXT("\r")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecRevisionsMissingOrCorruptTest, "CrowdySDK.CrowdyExecEditor.RevisionsMissingIsEmptyCorruptFails", CrowdyExecRevisionsTest::TestFlags)
bool FCrowdyExecRevisionsMissingOrCorruptTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecRevisionsTest;
	const FTempFolder Folder;
	TArray<FRevision> Loaded;
	Loaded.AddDefaulted();
	FString Error;
	TestTrue(TEXT("a type never deployed has no history, which is not an error"), CrowdyExecRevisions::LoadHistory(Folder.Path, Loaded, Error));
	TestEqual(TEXT("its history is empty"), Loaded.Num(), 0);

	const FString Path = CrowdyExecRevisions::GetHistoryPath(Folder.Path);
	FFileHelper::SaveStringToFile(FString(TEXT("not a deploy record")), *Path);
	TestFalse(TEXT("a history that is not JSON fails"), CrowdyExecRevisions::LoadHistory(Folder.Path, Loaded, Error));
	TestTrue(TEXT("the error names the file"), Error.Contains(TEXT("revisions.json")));

	FFileHelper::SaveStringToFile(FString(TEXT("{\"format\":1,\"revisions\":[{\"fingerprint\":\"abc\",\"versions\":[1]}]}")), *Path);
	Error.Reset();
	TestFalse(TEXT("a revision without its files fails"), CrowdyExecRevisions::LoadHistory(Folder.Path, Loaded, Error));
	TestEqual(TEXT("a history that fails loads nothing"), Loaded.Num(), 0);
	TestFalse(TEXT("it says why"), Error.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecRevisionsParseManifestTest, "CrowdySDK.CrowdyExecEditor.RevisionsParseManifest", CrowdyExecRevisionsTest::TestFlags)
bool FCrowdyExecRevisionsParseManifestTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecRevisionsTest;
	TArray<CrowdyExecRevisions::FLiveType> Types;
	if (!TestTrue(TEXT("the platform's manifest parses"), CrowdyExecRevisions::ParseManifest(SampleManifest, Types))
		|| !TestEqual(TEXT("it runs three types, the root included"), Types.Num(), 3))
	{
		return false;
	}
	TestEqualSensitive(TEXT("types are sorted by name"), Types[0].TypeName + TEXT(",") + Types[1].TypeName + TEXT(",") + Types[2].TypeName, FString(TEXT("root,sbx_counter,sbx_locker")));
	TestEqualSensitive(TEXT("a type's digest is read"), Types[1].Digest, FString(TEXT("49818bf390cf07d7cebb4b896da90eb8b736a2652ff21d1df75fefa6ea5a18ff")));
	TestEqual(TEXT("a type's save interval is read"), Types[1].SaveIntervalMs, static_cast<int64>(5000));
	TestEqual(TEXT("a type's idle timeout is read"), Types[1].IdleTimeoutMs, static_cast<int64>(300000));
	TestEqual(TEXT("a null save interval reads 0"), Types[0].SaveIntervalMs, static_cast<int64>(0));
	TestEqual(TEXT("a null idle timeout reads 0"), Types[0].IdleTimeoutMs, static_cast<int64>(0));

	TestTrue(TEXT("a type with no intervals parses"), CrowdyExecRevisions::ParseManifest(TEXT("{\"root\":\"root\",\"types\":{\"t\":{\"kind\":\"hub\",\"digest\":\"d\"}}}"), Types));
	TestTrue(TEXT("its missing intervals read 0"), Types.Num() == 1 && Types[0].SaveIntervalMs == 0 && Types[0].IdleTimeoutMs == 0);

	TestFalse(TEXT("text that is not JSON is not a manifest"), CrowdyExecRevisions::ParseManifest(TEXT("not json"), Types));
	TestFalse(TEXT("JSON without types is not a manifest"), CrowdyExecRevisions::ParseManifest(TEXT("{\"root\":\"root\"}"), Types));
	TestFalse(TEXT("a type that is not an object is not a manifest"), CrowdyExecRevisions::ParseManifest(TEXT("{\"types\":{\"a\":{\"digest\":\"d\"},\"b\":3}}"), Types));
	TestEqual(TEXT("a manifest that fails gives no types"), Types.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecRevisionsCompareTest, "CrowdySDK.CrowdyExecEditor.RevisionsCompareMatrix", CrowdyExecRevisionsTest::TestFlags)
bool FCrowdyExecRevisionsCompareTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecRevisionsTest;
	using CrowdyExecDeploy::ECrateState;

	const FBuildCrate Same = MakeCrate(TEXT("t_same"), TEXT("fn same() {}\n"));
	const FBuildCrate CodeLive = MakeCrate(TEXT("t_code"), TEXT("fn live() {}\n"));
	const FBuildCrate CodeNow = MakeCrate(TEXT("t_code"), TEXT("fn edited() {}\n"));
	const FBuildCrate Save = MakeCrate(TEXT("t_save"), TEXT("fn save() {}\n"));
	const FBuildCrate Idle = MakeCrate(TEXT("t_idle"), TEXT("fn idle() {}\n"));
	const FBuildCrate Unset = MakeCrate(TEXT("t_unset"), TEXT("fn unset() {}\n"));
	const FBuildCrate Fresh = MakeCrate(TEXT("t_new"), TEXT("fn fresh() {}\n"));
	const FBuildCrate Stranger = MakeCrate(TEXT("t_unknown"), TEXT("fn stranger() {}\n"));
	const FBuildCrate NotReady = MakeCrate(TEXT("t_notready"), TEXT("fn stale() {}\n"));

	CrowdyExecDeploy::FProjectDeploy Project;
	// A crate for the type that is not ready too, matching what runs, so only its state keeps it from reading unchanged.
	Project.Crates = {CrowdyExecDeploy::MakeRootCrate(), Same, CodeNow, Save, Idle, Unset, Fresh, Stranger, NotReady};
	for (const TCHAR* Name : {TEXT("t_same"), TEXT("t_code"), TEXT("t_save"), TEXT("t_idle"), TEXT("t_unset"), TEXT("t_new"), TEXT("t_unknown")})
	{
		Project.Types.Add(MakeTypeState(Name, ECrateState::UpToDate));
	}
	Project.Types.Add(MakeTypeState(TEXT("t_notready"), ECrateState::OutOfDate));

	TArray<TStrongObjectPtr<UCrowdyServerObjectDefinition>> Owned;
	TArray<const UCrowdyServerObjectDefinition*> Definitions;
	for (const TCHAR* Name : {TEXT("t_same"), TEXT("t_code"), TEXT("t_save"), TEXT("t_idle"), TEXT("t_unset"), TEXT("t_new"), TEXT("t_unknown"), TEXT("t_notready")})
	{
		Owned.Add(MakeDefinition(Name, 30, 300));
		Definitions.Add(Owned.Last().Get());
	}

	TMap<FString, TArray<FRevision>> Histories;
	Histories.Add(TEXT("t_same"), TArray<FRevision>{MakeRevision(Same, TEXT("d_same"), 1)});
	// The newest record matches the code now, but the live version runs the older one (a rollback).
	Histories.Add(TEXT("t_code"), TArray<FRevision>{MakeRevision(CodeNow, TEXT("d_code_new"), 2), MakeRevision(CodeLive, TEXT("d_code"), 1)});
	Histories.Add(TEXT("t_save"), TArray<FRevision>{MakeRevision(Save, TEXT("d_save"), 1)});
	Histories.Add(TEXT("t_idle"), TArray<FRevision>{MakeRevision(Idle, TEXT("d_idle"), 1)});
	Histories.Add(TEXT("t_unset"), TArray<FRevision>{MakeRevision(Unset, TEXT("d_unset"), 1)});
	Histories.Add(TEXT("t_unknown"), TArray<FRevision>{MakeRevision(Stranger, TEXT("d_recorded"), 1)});
	Histories.Add(TEXT("t_notready"), TArray<FRevision>{MakeRevision(NotReady, TEXT("d_notready"), 1)});
	const auto HistoryFor = [&Histories](const FString& TypeName) { return Histories.FindRef(TypeName); };

	CrowdyExecDeveloper::FVersion Live;
	Live.Version = 4;
	Live.bActive = true;
	Live.ManifestJson = MakeLiveManifest({
		{TEXT("t_same"), TEXT("d_same"), 30000, 300000},
		{TEXT("t_code"), TEXT("d_code"), 30000, 300000},
		{TEXT("t_save"), TEXT("d_save"), 10000, 300000},
		{TEXT("t_idle"), TEXT("d_idle"), 30000, 60000},
		{TEXT("t_unset"), TEXT("d_unset"), 0, 0},
		{TEXT("t_unknown"), TEXT("d_elsewhere"), 30000, 300000},
		{TEXT("t_notready"), TEXT("d_notready"), 30000, 300000},
		{TEXT("t_gone"), TEXT("d_gone"), 30000, 300000}});

	const TArray<FTypeChange> Changes = CrowdyExecRevisions::Compare(Project, Definitions, &Live, true, HistoryFor);
	const TCHAR* const Case = TEXT("against version 4");
	TestChange(*this, Case, Changes, TEXT("t_same"), EChange::Unchanged);
	TestChange(*this, Case, Changes, TEXT("t_code"), EChange::Changed, TEXT("server code"));
	TestChange(*this, Case, Changes, TEXT("t_save"), EChange::Changed, TEXT("save interval"));
	TestChange(*this, Case, Changes, TEXT("t_idle"), EChange::Changed, TEXT("idle timeout"));
	TestChange(*this, Case, Changes, TEXT("t_unset"), EChange::Unchanged);
	TestChange(*this, Case, Changes, TEXT("t_new"), EChange::New);
	TestChange(*this, Case, Changes, TEXT("t_unknown"), EChange::Unknown);
	TestChange(*this, Case, Changes, TEXT("t_notready"), EChange::NotCompared);
	TestChange(*this, Case, Changes, TEXT("t_gone"), EChange::Removed);
	TestNull(TEXT("the platform's root is never listed as removed"), FindChange(Changes, TEXT("root")));
	TestEqual(TEXT("each project type, then each live-only type"), Changes.Num(), 9);
	if (const FTypeChange* Code = FindChange(Changes, TEXT("t_code")))
	{
		TestEqual(TEXT("the live revision is the one whose digest runs, not the newest"), Code->LiveRevision, 1);
		TestEqual(TEXT("the live version is named"), Code->LiveVersion, 4);
	}
	if (const FTypeChange* Unchanged = FindChange(Changes, TEXT("t_same")))
	{
		TestEqual(TEXT("an unchanged type's live revision is found"), Unchanged->LiveRevision, 0);
	}
	if (const FTypeChange* Unknown = FindChange(Changes, TEXT("t_unknown")))
	{
		TestEqual(TEXT("unknown live code has no revision"), Unknown->LiveRevision, static_cast<int32>(INDEX_NONE));
	}
	TestTrue(TEXT("changes against version 4 are changes"), CrowdyExecRevisions::HasChanges(Changes));

	const TArray<FTypeChange> NothingLive = CrowdyExecRevisions::Compare(Project, Definitions, nullptr, true, HistoryFor);
	TestChange(*this, TEXT("nothing live"), NothingLive, TEXT("t_same"), EChange::New);
	TestChange(*this, TEXT("nothing live"), NothingLive, TEXT("t_unknown"), EChange::New);
	TestChange(*this, TEXT("nothing live"), NothingLive, TEXT("t_notready"), EChange::NotCompared);
	TestEqual(TEXT("with nothing live, nothing is removed"), NothingLive.Num(), 8);

	const TArray<FTypeChange> NotRead = CrowdyExecRevisions::Compare(Project, Definitions, &Live, false, HistoryFor);
	TestChange(*this, TEXT("versions not read"), NotRead, TEXT("t_code"), EChange::NotCompared);
	TestChange(*this, TEXT("versions not read"), NotRead, TEXT("t_new"), EChange::NotCompared);
	TestNull(TEXT("with the versions not read, nothing is removed"), FindChange(NotRead, TEXT("t_gone")));
	TestFalse(TEXT("nothing compared is no change"), CrowdyExecRevisions::HasChanges(NotRead));

	CrowdyExecDeveloper::FVersion Broken = Live;
	Broken.ManifestJson = TEXT("not json");
	const TArray<FTypeChange> Unreadable = CrowdyExecRevisions::Compare(Project, Definitions, &Broken, true, HistoryFor);
	TestChange(*this, TEXT("a manifest that cannot be read"), Unreadable, TEXT("t_same"), EChange::Unknown);
	TestChange(*this, TEXT("a manifest that cannot be read"), Unreadable, TEXT("t_notready"), EChange::NotCompared);
	TestNull(TEXT("with a manifest that cannot be read, nothing is removed"), FindChange(Unreadable, TEXT("t_gone")));
	TestTrue(TEXT("a manifest that cannot be read is never no change"), CrowdyExecRevisions::HasChanges(Unreadable));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecRevisionsHasChangesTest, "CrowdySDK.CrowdyExecEditor.RevisionsHasChanges", CrowdyExecRevisionsTest::TestFlags)
bool FCrowdyExecRevisionsHasChangesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecRevisionsTest;
	const auto With = [](EChange Change)
	{
		FTypeChange Unchanged;
		Unchanged.Change = EChange::Unchanged;
		FTypeChange Other;
		Other.Change = Change;
		return TArray<FTypeChange>{Unchanged, Other};
	};
	TestFalse(TEXT("no types, no changes"), CrowdyExecRevisions::HasChanges(TArray<FTypeChange>()));
	TestFalse(TEXT("unchanged is no change"), CrowdyExecRevisions::HasChanges(With(EChange::Unchanged)));
	TestFalse(TEXT("not compared is no change"), CrowdyExecRevisions::HasChanges(With(EChange::NotCompared)));
	TestTrue(TEXT("a changed type is a change"), CrowdyExecRevisions::HasChanges(With(EChange::Changed)));
	TestTrue(TEXT("a new type is a change"), CrowdyExecRevisions::HasChanges(With(EChange::New)));
	TestTrue(TEXT("unknown live code is a change"), CrowdyExecRevisions::HasChanges(With(EChange::Unknown)));
	TestTrue(TEXT("a removed type is a change"), CrowdyExecRevisions::HasChanges(With(EChange::Removed)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecRevisionsDiffTest, "CrowdySDK.CrowdyExecEditor.RevisionsDiffLines", CrowdyExecRevisionsTest::TestFlags)
bool FCrowdyExecRevisionsDiffTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecRevisionsTest;
	TestEqualSensitive(TEXT("a small edit shows kept, removed and added lines in order"),
		Render(CrowdyExecRevisions::DiffLines(TEXT("a\nb\nc\n"), TEXT("a\nB\nc\nd\n"))), FString(TEXT("=a|-b|+B|=c|+d")));
	TestEqualSensitive(TEXT("line endings alone change nothing"), Render(CrowdyExecRevisions::DiffLines(TEXT("a\r\nb\r\n"), TEXT("a\nb"))), FString(TEXT("=a|=b")));
	TestEqualSensitive(TEXT("an empty line is a line"), Render(CrowdyExecRevisions::DiffLines(TEXT("a\n\nb\n"), TEXT("a\nb\n"))), FString(TEXT("=a|-|=b")));
	TestEqualSensitive(TEXT("from nothing, every line is added"), Render(CrowdyExecRevisions::DiffLines(FString(), TEXT("a\nb\n"))), FString(TEXT("+a|+b")));
	TestEqualSensitive(TEXT("a line moved down is found as kept"),
		Render(CrowdyExecRevisions::DiffLines(TEXT("x\no1\no2\n"), TEXT("n1\nn2\nx\n"))), FString(TEXT("+n1|+n2|=x|-o1|-o2")));

	FString Old;
	FString New;
	MakeShiftedTexts(2000, Old, New);
	const TArray<FDiffLine> Large = CrowdyExecRevisions::DiffLines(Old, New);
	TestEqual(TEXT("a very large diff lists every line"), Large.Num(), 4002);
	TestEqual(TEXT("a very large diff is compared as wholes, with no kept line"), CountKind(Large, FDiffLine::EKind::Same), 0);
	if (TestTrue(TEXT("a very large diff has lines"), Large.Num() == 4002))
	{
		TestTrue(TEXT("the old text comes first, removed"), Large[0].Kind == FDiffLine::EKind::Removed && Large[0].Text.Equals(TEXT("x")));
		TestTrue(TEXT("the new text comes after, added"), Large[2001].Kind == FDiffLine::EKind::Added && Large[4001].Text.Equals(TEXT("x")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecRevisionsRecordDeployTest, "CrowdySDK.CrowdyExecEditor.RevisionsRecordDeploy", CrowdyExecRevisionsTest::TestFlags)
bool FCrowdyExecRevisionsRecordDeployTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecRevisionsTest;
	const FTempFolder Folder;
	CrowdyExecDeploy::FProjectDeploy Project;
	Project.Crates = {CrowdyExecDeploy::MakeRootCrate(), MakeCrate(TEXT("t_a"), TEXT("fn a() {}\n")), MakeCrate(TEXT("t_b"), TEXT("fn b() {}\n"))};
	Project.Types = {MakeTypeState(TEXT("t_a"), CrowdyExecDeploy::ECrateState::UpToDate), MakeTypeState(TEXT("t_b"), CrowdyExecDeploy::ECrateState::UpToDate)};
	const TArray<FBuildArtifact> Artifacts = {{TEXT("root"), TEXT("d_root"), 1}, {TEXT("t_a"), TEXT("d_a"), 1}, {TEXT("t_b"), TEXT("d_b"), 1}};
	TArray<FString> Asked;
	const auto CrateDirectoryFor = [&Folder, &Asked](const FString& TypeName)
	{
		Asked.Add(TypeName);
		return FPaths::Combine(Folder.Path, TypeName);
	};
	const FDateTime When(2026, 9, 29, 12, 0, 0);

	TArray<FString> Errors;
	CrowdyExecRevisions::RecordDeploy(Project, Artifacts, 7, When, 5, CrateDirectoryFor, Errors);
	TestEqualSensitive(TEXT("recording succeeds"), FString::Join(Errors, TEXT("; ")), FString());
	TestEqualSensitive(TEXT("only the project's types are recorded, never the root"), FString::Join(Asked, TEXT(",")), FString(TEXT("t_a,t_b")));
	TestFalse(TEXT("the root gets no history"), FPaths::FileExists(CrowdyExecRevisions::GetHistoryPath(FPaths::Combine(Folder.Path, TEXT("root")))));

	TArray<FRevision> History;
	FString Error;
	if (!TestTrue(TEXT("t_a's history loads"), CrowdyExecRevisions::LoadHistory(FPaths::Combine(Folder.Path, TEXT("t_a")), History, Error))
		|| !TestEqual(TEXT("t_a has one revision"), History.Num(), 1))
	{
		return false;
	}
	TestEqualSensitive(TEXT("its digest is t_a's build artifact"), FString::Join(History[0].Digests, TEXT(",")), FString(TEXT("d_a")));
	TestEqualSensitive(TEXT("its version is the deployed one"), FString::JoinBy(History[0].Versions, TEXT(","), [](int32 Version) { return FString::FromInt(Version); }), FString(TEXT("7")));
	TestTrue(TEXT("it is dated by the deploy"), History[0].DeployedAt == When);
	TestEqualSensitive(TEXT("it keeps the files as sent"), History[0].Fingerprint, CrowdyExecRevisions::Fingerprint(Project.Crates[1].Files));
	TestTrue(TEXT("t_b's history is written too"), CrowdyExecRevisions::LoadHistory(FPaths::Combine(Folder.Path, TEXT("t_b")), History, Error)
		&& History.Num() == 1 && History[0].Digests.Num() == 1 && History[0].Digests[0].Equals(TEXT("d_b"), ESearchCase::CaseSensitive));

	const FString BrokenPath = CrowdyExecRevisions::GetHistoryPath(FPaths::Combine(Folder.Path, TEXT("t_b")));
	FFileHelper::SaveStringToFile(FString(TEXT("broken")), *BrokenPath);
	Errors.Reset();
	CrowdyExecRevisions::RecordDeploy(Project, Artifacts, 8, When, 5, CrateDirectoryFor, Errors);
	TestTrue(TEXT("a history that cannot be read is named in the errors"), Errors.Num() == 1 && Errors[0].StartsWith(TEXT("t_b:")));
	FString BrokenText;
	FFileHelper::LoadFileToString(BrokenText, *BrokenPath);
	TestEqualSensitive(TEXT("a history that cannot be read is left as it was"), BrokenText, FString(TEXT("broken")));
	TestTrue(TEXT("the other types are still recorded"), CrowdyExecRevisions::LoadHistory(FPaths::Combine(Folder.Path, TEXT("t_a")), History, Error)
		&& History.Num() == 1 && History[0].Versions.Num() == 2 && History[0].Versions[0] == 8);
	return true;
}

#endif
