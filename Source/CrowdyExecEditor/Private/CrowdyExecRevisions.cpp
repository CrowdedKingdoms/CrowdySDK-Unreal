#include "CrowdyExecRevisions.h"

#include "Algo/Sort.h"
#include "Containers/StringConv.h"
#include "CrowdyServerObjectDefinition.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace CrowdyExecRevisionsDetail
{
	using CrowdyExecDeveloper::FBuildFile;
	using CrowdyExecRevisions::FRevision;

	const TCHAR* const HistoryFileName = TEXT("revisions.json");
	constexpr int32 HistoryFormat = 1;
	/** Above this many line pairs a diff shows the old text removed and the new text added. */
	constexpr int64 MaxDiffCells = 4000000;
	/** How many versions and digests one revision lists, newest first. */
	constexpr int32 MaxListed = 20;

	/** Line endings and whitespace at the end of a file do not make it another revision. */
	FString RevisionComparable(const FString& Text)
	{
		return Text.Replace(TEXT("\r\n"), TEXT("\n")).TrimEnd();
	}

	bool IsSameText(const FString& A, const FString& B)
	{
		return A.Equals(B, ESearchCase::CaseSensitive);
	}

	void PutFirst(TArray<FString>& Items, const FString& Item)
	{
		Items.RemoveAll([&Item](const FString& Existing) { return IsSameText(Existing, Item); });
		Items.Insert(Item, 0);
	}

	void PutFirst(TArray<int32>& Items, int32 Item)
	{
		Items.Remove(Item);
		Items.Insert(Item, 0);
	}

	void WriteRevision(TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>& Writer, const FRevision& Revision)
	{
		Writer.WriteObjectStart();
		Writer.WriteValue(TEXT("fingerprint"), Revision.Fingerprint);
		Writer.WriteValue(TEXT("deployedAt"), Revision.DeployedAt.ToIso8601());
		Writer.WriteArrayStart(TEXT("versions"));
		for (const int32 Version : Revision.Versions)
		{
			Writer.WriteValue(Version);
		}
		Writer.WriteArrayEnd();
		Writer.WriteArrayStart(TEXT("digests"));
		for (const FString& Digest : Revision.Digests)
		{
			Writer.WriteValue(Digest);
		}
		Writer.WriteArrayEnd();
		Writer.WriteArrayStart(TEXT("files"));
		for (const FBuildFile& File : Revision.Files)
		{
			Writer.WriteObjectStart();
			Writer.WriteValue(TEXT("path"), File.Path);
			Writer.WriteValue(TEXT("content"), File.Content);
			Writer.WriteObjectEnd();
		}
		Writer.WriteArrayEnd();
		Writer.WriteObjectEnd();
	}

	bool ReadFile(const FJsonValue& Value, FBuildFile& OutFile)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		return Value.TryGetObject(Object) && (*Object)->TryGetStringField(TEXT("path"), OutFile.Path) && (*Object)->TryGetStringField(TEXT("content"), OutFile.Content);
	}

	bool ReadRevision(const FJsonValue& Value, FRevision& OutRevision)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!Value.TryGetObject(Object) || !(*Object)->TryGetStringField(TEXT("fingerprint"), OutRevision.Fingerprint))
		{
			return false;
		}
		const FJsonObject& Revision = **Object;
		FString DeployedAt;
		if (Revision.TryGetStringField(TEXT("deployedAt"), DeployedAt))
		{
			FDateTime::ParseIso8601(*DeployedAt, OutRevision.DeployedAt);
		}
		const TArray<TSharedPtr<FJsonValue>>* Versions = nullptr;
		if (Revision.TryGetArrayField(TEXT("versions"), Versions))
		{
			for (const TSharedPtr<FJsonValue>& Version : *Versions)
			{
				OutRevision.Versions.Add(static_cast<int32>(Version->AsNumber()));
			}
		}
		Revision.TryGetStringArrayField(TEXT("digests"), OutRevision.Digests);
		const TArray<TSharedPtr<FJsonValue>>* Files = nullptr;
		if (!Revision.TryGetArrayField(TEXT("files"), Files))
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& File : *Files)
		{
			if (!File || !ReadFile(*File, OutRevision.Files.AddDefaulted_GetRef()))
			{
				return false;
			}
		}
		return true;
	}

	/** 0 when the manifest leaves it out or null. */
	int64 ReadMs(const FJsonObject& Type, const TCHAR* Field)
	{
		int64 Ms = 0;
		return Type.TryGetNumberField(Field, Ms) ? Ms : 0;
	}

	const CrowdyExecRevisions::FLiveType* FindLiveType(TConstArrayView<CrowdyExecRevisions::FLiveType> Types, const FString& TypeName)
	{
		return Types.FindByPredicate([&TypeName](const CrowdyExecRevisions::FLiveType& Type) { return IsSameText(Type.TypeName, TypeName); });
	}

	int32 FindRevisionWithDigest(TConstArrayView<FRevision> History, const FString& Digest)
	{
		if (Digest.IsEmpty())
		{
			return INDEX_NONE;
		}
		return History.IndexOfByPredicate([&Digest](const FRevision& Revision)
		{
			return Revision.Digests.ContainsByPredicate([&Digest](const FString& Candidate) { return IsSameText(Candidate, Digest); });
		});
	}

	/** A live 0 means the manifest does not say, which is not a difference. */
	void AddIntervalChanges(const UCrowdyServerObjectDefinition* Definition, const CrowdyExecRevisions::FLiveType& Live, TArray<FString>& OutWhat)
	{
		if (!Definition)
		{
			return;
		}
		const int64 SaveIntervalMs = static_cast<int64>(Definition->SaveIntervalSeconds) * 1000;
		const int64 IdleTimeoutMs = static_cast<int64>(Definition->IdleTimeoutSeconds) * 1000;
		if (Live.SaveIntervalMs != 0 && Live.SaveIntervalMs != SaveIntervalMs)
		{
			OutWhat.Add(TEXT("save interval"));
		}
		if (Live.IdleTimeoutMs != 0 && Live.IdleTimeoutMs != IdleTimeoutMs)
		{
			OutWhat.Add(TEXT("idle timeout"));
		}
	}

	CrowdyExecRevisions::EChange CompareType(const CrowdyExecDeploy::FTypeState& Type, const CrowdyExecDeveloper::FBuildCrate* Crate, const UCrowdyServerObjectDefinition* Definition,
		TConstArrayView<CrowdyExecRevisions::FLiveType> LiveTypes, TFunctionRef<TArray<FRevision>(const FString& TypeName)> HistoryFor, CrowdyExecRevisions::FTypeChange& Change)
	{
		using CrowdyExecRevisions::EChange;
		if (Type.State != CrowdyExecDeploy::ECrateState::UpToDate || !Crate)
		{
			return EChange::NotCompared;
		}
		const CrowdyExecRevisions::FLiveType* Live = FindLiveType(LiveTypes, Type.TypeName);
		if (!Live)
		{
			return EChange::New;
		}
		const TArray<FRevision> History = HistoryFor(Type.TypeName);
		Change.LiveRevision = FindRevisionWithDigest(History, Live->Digest);
		AddIntervalChanges(Definition, *Live, Change.What);
		if (Change.LiveRevision == INDEX_NONE)
		{
			return EChange::Unknown;
		}
		if (!IsSameText(History[Change.LiveRevision].Fingerprint, CrowdyExecRevisions::Fingerprint(Crate->Files)))
		{
			Change.What.Insert(TEXT("server code"), 0);
		}
		return Change.What.IsEmpty() ? EChange::Unchanged : EChange::Changed;
	}

	TArray<FString> SplitLines(const FString& Text)
	{
		TArray<FString> Lines;
		Text.Replace(TEXT("\r\n"), TEXT("\n")).ParseIntoArray(Lines, TEXT("\n"), false);
		// A final line break ends the last line; it does not start another.
		if (!Lines.IsEmpty() && Lines.Last().IsEmpty())
		{
			Lines.Pop();
		}
		return Lines;
	}

	void AddLines(TArray<CrowdyExecRevisions::FDiffLine>& Out, TConstArrayView<FString> Lines, CrowdyExecRevisions::FDiffLine::EKind Kind)
	{
		for (const FString& Line : Lines)
		{
			Out.Add(CrowdyExecRevisions::FDiffLine{Kind, Line});
		}
	}

	/** The longest common subsequence of lines, walked from the start so removals come before additions at each change. */
	void DiffMiddle(TConstArrayView<FString> Old, TConstArrayView<FString> New, TArray<CrowdyExecRevisions::FDiffLine>& Out)
	{
		using EKind = CrowdyExecRevisions::FDiffLine::EKind;
		const int32 Rows = Old.Num() + 1;
		const int32 Columns = New.Num() + 1;
		TArray<int32> Common;
		Common.SetNumZeroed(Rows * Columns);
		for (int32 OldIndex = Old.Num() - 1; OldIndex >= 0; --OldIndex)
		{
			for (int32 NewIndex = New.Num() - 1; NewIndex >= 0; --NewIndex)
			{
				const int32 Cell = OldIndex * Columns + NewIndex;
				Common[Cell] = IsSameText(Old[OldIndex], New[NewIndex]) ? Common[Cell + Columns + 1] + 1 : FMath::Max(Common[Cell + Columns], Common[Cell + 1]);
			}
		}
		int32 OldIndex = 0;
		int32 NewIndex = 0;
		while (OldIndex < Old.Num() && NewIndex < New.Num())
		{
			const int32 Cell = OldIndex * Columns + NewIndex;
			if (IsSameText(Old[OldIndex], New[NewIndex]))
			{
				Out.Add({EKind::Same, Old[OldIndex++]});
				++NewIndex;
				continue;
			}
			if (Common[Cell + Columns] >= Common[Cell + 1])
			{
				Out.Add({EKind::Removed, Old[OldIndex++]});
				continue;
			}
			Out.Add({EKind::Added, New[NewIndex++]});
		}
		AddLines(Out, Old.RightChop(OldIndex), EKind::Removed);
		AddLines(Out, New.RightChop(NewIndex), EKind::Added);
	}
}

const CrowdyExecDeveloper::FBuildFile* CrowdyExecRevisions::FRevision::FindFile(const FString& Path) const
{
	return Files.FindByPredicate([&Path](const CrowdyExecDeveloper::FBuildFile& File) { return File.Path.Equals(Path, ESearchCase::CaseSensitive); });
}

FString CrowdyExecRevisions::GetHistoryPath(const FString& CrateDirectory)
{
	return FPaths::Combine(CrateDirectory, CrowdyExecRevisionsDetail::HistoryFileName);
}

bool CrowdyExecRevisions::LoadHistory(const FString& CrateDirectory, TArray<FRevision>& OutRevisions, FString& OutError)
{
	using namespace CrowdyExecRevisionsDetail;
	OutRevisions.Reset();
	const FString Path = GetHistoryPath(CrateDirectory);
	if (!FPaths::FileExists(Path))
	{
		return true;
	}
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		OutError = FString::Printf(TEXT("could not read %s"), *Path);
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	const TArray<TSharedPtr<FJsonValue>>* Revisions = nullptr;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root || !Root->TryGetArrayField(TEXT("revisions"), Revisions))
	{
		OutError = FString::Printf(TEXT("%s is not a deploy record; fix or delete it"), *Path);
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Revision : *Revisions)
	{
		if (!Revision || !ReadRevision(*Revision, OutRevisions.AddDefaulted_GetRef()))
		{
			OutRevisions.Reset();
			OutError = FString::Printf(TEXT("%s has a revision that cannot be read; fix or delete it"), *Path);
			return false;
		}
	}
	return true;
}

bool CrowdyExecRevisions::SaveHistory(const FString& CrateDirectory, TConstArrayView<FRevision> Revisions, FString& OutError)
{
	using namespace CrowdyExecRevisionsDetail;
	FString Text;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Text);
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("format"), HistoryFormat);
	Writer->WriteArrayStart(TEXT("revisions"));
	for (const FRevision& Revision : Revisions)
	{
		WriteRevision(*Writer, Revision);
	}
	Writer->WriteArrayEnd();
	Writer->WriteObjectEnd();
	Writer->Close();
	// The same bytes on every platform, so the committed file only changes when a revision does.
	Text = Text.Replace(TEXT("\r\n"), TEXT("\n")) + TEXT("\n");
	const FString Path = GetHistoryPath(CrateDirectory);
	if (!FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("could not write %s"), *Path);
		return false;
	}
	return true;
}

FString CrowdyExecRevisions::Fingerprint(TConstArrayView<CrowdyExecDeveloper::FBuildFile> Files)
{
	using namespace CrowdyExecRevisionsDetail;
	TArray<const FBuildFile*> Sorted;
	for (const FBuildFile& File : Files)
	{
		Sorted.Add(&File);
	}
	Algo::Sort(Sorted, [](const FBuildFile* A, const FBuildFile* B) { return A->Path.Compare(B->Path, ESearchCase::CaseSensitive) < 0; });
	FSHA1 Hash;
	for (const FBuildFile* File : Sorted)
	{
		const FTCHARToUTF8 Utf8(*(File->Path + TEXT("\n") + RevisionComparable(File->Content) + TEXT("\n")));
		Hash.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	}
	Hash.Final();
	FSHAHash Digest;
	Hash.GetHash(Digest.Hash);
	return Digest.ToString().ToLower();
}

void CrowdyExecRevisions::AddRevision(TArray<FRevision>& History, const CrowdyExecDeveloper::FBuildCrate& Crate, const FString& Digest, int32 Version, const FDateTime& When, int32 Keep)
{
	using namespace CrowdyExecRevisionsDetail;
	const FString Print = Fingerprint(Crate.Files);
	const int32 Found = History.IndexOfByPredicate([&Print](const FRevision& Revision) { return IsSameText(Revision.Fingerprint, Print); });
	FRevision Revision;
	if (Found != INDEX_NONE)
	{
		Revision = MoveTemp(History[Found]);
		History.RemoveAt(Found);
	}
	Revision.Fingerprint = Print;
	PutFirst(Revision.Versions, Version);
	if (!Digest.IsEmpty())
	{
		PutFirst(Revision.Digests, Digest);
	}
	// Unchanged code rides along in every deploy, so a long-lived revision would otherwise list every version since.
	Revision.Versions.SetNum(FMath::Min(Revision.Versions.Num(), MaxListed));
	Revision.Digests.SetNum(FMath::Min(Revision.Digests.Num(), MaxListed));
	Revision.DeployedAt = When;
	Revision.Files = Crate.Files;
	History.Insert(MoveTemp(Revision), 0);
	History.SetNum(FMath::Min(History.Num(), FMath::Max(Keep, 1)));
}

void CrowdyExecRevisions::RecordDeploy(const CrowdyExecDeploy::FProjectDeploy& Project, TConstArrayView<CrowdyExecDeveloper::FBuildArtifact> Artifacts, int32 Version,
	const FDateTime& When, int32 Keep, TFunctionRef<FString(const FString& TypeName)> CrateDirectoryFor, TArray<FString>& OutErrors)
{
	using namespace CrowdyExecRevisionsDetail;
	for (const CrowdyExecDeveloper::FBuildCrate& Crate : Project.Crates)
	{
		if (IsSameText(Crate.Name, CrowdyExecDeploy::RootTypeName))
		{
			continue;
		}
		const FString Directory = CrateDirectoryFor(Crate.Name);
		if (Directory.IsEmpty())
		{
			OutErrors.Add(FString::Printf(TEXT("%s: its server code folder is not known"), *Crate.Name));
			continue;
		}
		const CrowdyExecDeveloper::FBuildArtifact* Artifact = Artifacts.FindByPredicate([&Crate](const CrowdyExecDeveloper::FBuildArtifact& Candidate)
		{
			return IsSameText(Candidate.Crate, Crate.Name);
		});
		TArray<FRevision> History;
		FString Error;
		if (!LoadHistory(Directory, History, Error))
		{
			OutErrors.Add(FString::Printf(TEXT("%s: %s"), *Crate.Name, *Error));
			continue;
		}
		AddRevision(History, Crate, Artifact ? Artifact->Digest : FString(), Version, When, Keep);
		if (!SaveHistory(Directory, History, Error))
		{
			OutErrors.Add(FString::Printf(TEXT("%s: %s"), *Crate.Name, *Error));
		}
	}
}

bool CrowdyExecRevisions::ParseManifest(const FString& ManifestJson, TArray<FLiveType>& OutTypes)
{
	using namespace CrowdyExecRevisionsDetail;
	OutTypes.Reset();
	TSharedPtr<FJsonObject> Root;
	const TSharedPtr<FJsonObject>* Types = nullptr;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ManifestJson), Root) || !Root || !Root->TryGetObjectField(TEXT("types"), Types))
	{
		return false;
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Types)->Values)
	{
		const TSharedPtr<FJsonObject>* Type = nullptr;
		if (!Pair.Value || !Pair.Value->TryGetObject(Type))
		{
			OutTypes.Reset();
			return false;
		}
		FLiveType& Live = OutTypes.AddDefaulted_GetRef();
		Live.TypeName = Pair.Key;
		(*Type)->TryGetStringField(TEXT("digest"), Live.Digest);
		Live.SaveIntervalMs = ReadMs(**Type, TEXT("persist_every_ms"));
		Live.IdleTimeoutMs = ReadMs(**Type, TEXT("evict_after_ms"));
	}
	Algo::Sort(OutTypes, [](const FLiveType& A, const FLiveType& B) { return A.TypeName.Compare(B.TypeName, ESearchCase::CaseSensitive) < 0; });
	return true;
}

TArray<CrowdyExecRevisions::FTypeChange> CrowdyExecRevisions::Compare(const CrowdyExecDeploy::FProjectDeploy& Project, TConstArrayView<const UCrowdyServerObjectDefinition*> Definitions,
	const CrowdyExecDeveloper::FVersion* Live, bool bLiveKnown, TFunctionRef<TArray<FRevision>(const FString& TypeName)> HistoryFor)
{
	using namespace CrowdyExecRevisionsDetail;
	TArray<FLiveType> LiveTypes;
	const bool bReadable = !bLiveKnown || !Live || ParseManifest(Live->ManifestJson, LiveTypes);
	const int32 LiveVersion = Live ? Live->Version : 0;
	TArray<FTypeChange> Changes;
	for (const CrowdyExecDeploy::FTypeState& Type : Project.Types)
	{
		FTypeChange& Change = Changes.AddDefaulted_GetRef();
		Change.TypeName = Type.TypeName;
		Change.LiveVersion = LiveVersion;
		if (!bLiveKnown)
		{
			continue;
		}
		// A live manifest that cannot be read means what runs is unknown, which a deploy replaces; never "no changes".
		if (!bReadable)
		{
			Change.Change = Type.State == CrowdyExecDeploy::ECrateState::UpToDate ? EChange::Unknown : EChange::NotCompared;
			continue;
		}
		const CrowdyExecDeveloper::FBuildCrate* Crate = Project.Crates.FindByPredicate([&Type](const CrowdyExecDeveloper::FBuildCrate& Candidate) { return IsSameText(Candidate.Name, Type.TypeName); });
		const UCrowdyServerObjectDefinition* const* Definition = Definitions.FindByPredicate([&Type](const UCrowdyServerObjectDefinition* Candidate)
		{
			return Candidate && IsSameText(Candidate->TypeName, Type.TypeName);
		});
		Change.Change = CompareType(Type, Crate, Definition ? *Definition : nullptr, LiveTypes, HistoryFor, Change);
	}
	for (const FLiveType& LiveType : LiveTypes)
	{
		const bool bInProject = Project.Types.ContainsByPredicate([&LiveType](const CrowdyExecDeploy::FTypeState& Type) { return IsSameText(Type.TypeName, LiveType.TypeName); });
		if (bInProject || IsSameText(LiveType.TypeName, CrowdyExecDeploy::RootTypeName))
		{
			continue;
		}
		FTypeChange& Change = Changes.AddDefaulted_GetRef();
		Change.TypeName = LiveType.TypeName;
		Change.Change = EChange::Removed;
		Change.LiveVersion = LiveVersion;
	}
	return Changes;
}

bool CrowdyExecRevisions::HasChanges(TConstArrayView<FTypeChange> Changes)
{
	return Changes.ContainsByPredicate([](const FTypeChange& Change)
	{
		return Change.Change == EChange::New || Change.Change == EChange::Changed || Change.Change == EChange::Unknown || Change.Change == EChange::Removed;
	});
}

TArray<CrowdyExecRevisions::FDiffLine> CrowdyExecRevisions::DiffLines(const FString& Old, const FString& New)
{
	using namespace CrowdyExecRevisionsDetail;
	const TArray<FString> OldLines = SplitLines(Old);
	const TArray<FString> NewLines = SplitLines(New);
	int32 Prefix = 0;
	while (Prefix < OldLines.Num() && Prefix < NewLines.Num() && IsSameText(OldLines[Prefix], NewLines[Prefix]))
	{
		++Prefix;
	}
	int32 Suffix = 0;
	while (Suffix < OldLines.Num() - Prefix && Suffix < NewLines.Num() - Prefix && IsSameText(OldLines[OldLines.Num() - 1 - Suffix], NewLines[NewLines.Num() - 1 - Suffix]))
	{
		++Suffix;
	}
	const TConstArrayView<FString> OldMiddle = TConstArrayView<FString>(OldLines).Mid(Prefix, OldLines.Num() - Prefix - Suffix);
	const TConstArrayView<FString> NewMiddle = TConstArrayView<FString>(NewLines).Mid(Prefix, NewLines.Num() - Prefix - Suffix);
	TArray<FDiffLine> Lines;
	AddLines(Lines, TConstArrayView<FString>(OldLines).Left(Prefix), FDiffLine::EKind::Same);
	if (static_cast<int64>(OldMiddle.Num() + 1) * (NewMiddle.Num() + 1) > MaxDiffCells)
	{
		AddLines(Lines, OldMiddle, FDiffLine::EKind::Removed);
		AddLines(Lines, NewMiddle, FDiffLine::EKind::Added);
	}
	else
	{
		DiffMiddle(OldMiddle, NewMiddle, Lines);
	}
	AddLines(Lines, TConstArrayView<FString>(OldLines).Right(Suffix), FDiffLine::EKind::Same);
	return Lines;
}
