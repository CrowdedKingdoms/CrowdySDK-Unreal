#pragma once

#include "CoreMinimal.h"

class SCrowdyServerCodeTab;
struct FCrowdyServerFunction;

/** Reading and writing a logic file the way the Server Code editor does, and the small rules the definition asset applies. */
namespace CrowdyServerCodeFiles
{
	struct FDiskText
	{
		/** The file's text with LF line breaks. */
		FString Text;
		bool bExists = false;
		bool bReadable = false;
		bool bUtf8 = true;
		bool bCrlf = false;
	};

	enum class ETypeNameProblem : uint8
	{
		None,
		Empty,
		TooLong,
		FirstNotLetter,
		BadCharacter
	};

	/** Reads a file, noting whether it exists, could be read, is UTF-8 and breaks lines with CRLF. */
	FDiskText Read(const FString& Path);

	/** Writes Text (LF line breaks) as UTF-8 without a byte order mark, breaking lines with CRLF when bCrlf. */
	bool Write(const FString& Path, const FString& Text, bool bCrlf);

	/** True when both reads found the same file content. */
	bool IsSame(const FDiskText& A, const FDiskText& B);

	/** Text with every line break as LF, the way the editor keeps it. */
	FString WithLineFeeds(FString Text);

	/** Edits not saved yet, and the file as they started from it. */
	struct FEdits
	{
		FString Text;
		FDiskText Base;
	};

	/** What an editor shows for a file. */
	struct FShownText
	{
		FString Text;
		/** The file as the edits started from it: what a save checks the disk against. */
		FDiskText Base;
		bool bDirty = false;
		/** The shown edits were made before the file last changed on disk. */
		bool bStale = false;
	};

	/** The file as read from disk, or the edits kept for it (null when none) over the file they started from. */
	FShownText ShowText(const FDiskText& OnDisk, const FEdits* Stashed);

	/** Unsaved edits kept by file while no editor shows that file. */
	class FEditStash
	{
	public:
		/** Keeps Edits for Path in place of anything kept before: the newest edits win. */
		void Put(const FString& Path, FEdits Edits);
		/** Removes and returns the edits kept for Path. */
		TOptional<FEdits> Take(const FString& Path);
		bool Contains(const FString& Path) const { return Kept.Contains(Path); }
		void Forget(const FString& Path) { Kept.Remove(Path); }
		/** Forgets the edits to every file in Directory or below it. */
		void ForgetUnder(const FString& Directory);
		TArray<FString> Files() const;

	private:
		TMap<FString, FEdits> Kept;
	};

	/** What is wrong with a Type Name, the first problem found. */
	ETypeNameProblem CheckTypeName(const FString& TypeName);

	/** A Type Name the server accepts: lowercase letters, digits and underscores, starting with a letter, at most 48 characters. */
	bool IsValidTypeName(const FString& TypeName);

	/** A Type Name made from an asset's name: CSO_SbxCounter gives sbx_counter; a DA_ prefix is dropped too. Empty when nothing usable is left. */
	FString SuggestTypeName(const FString& AssetName);

	/** The problem in words, empty for None. */
	FText DescribeTypeNameProblem(ETypeNameProblem Problem);

	/** Why a Type Name another definition also has cannot be used. */
	FText SharedTypeNameProblem();

	/** True when two source texts differ only in line endings and whitespace at the ends of lines. */
	bool IsSameCode(const FString& A, const FString& B);

	/** Makes Field watched or not, whatever its spelling in Watched. True when Watched changed. */
	bool SetWatchedField(TArray<FName>& Watched, FName Field, bool bWatched);

	/** The watched names that are not among StateFields, in order. */
	TArray<FName> FindMissingFields(TConstArrayView<FName> Watched, TConstArrayView<FName> StateFields);

	/** An app version as text, never grouped: 12345, not 12,345. */
	FText VersionText(int32 Version);

	/** "just now", "5 min ago", "2 h ago", "3 d ago", else the date. */
	FText TimeAgo(const FDateTime& Time, const FDateTime& Now);

	/** "Version 4 · live · 2 h ago" for a deployed revision. */
	FText RevisionLabel(int32 Version, bool bLive, const FDateTime& DeployedAt, const FDateTime& Now);
}

/** Unsaved edits made in the Server Code tabs of open Server Object editors, and those kept for files no tab shows. */
namespace CrowdyServerCodeEdits
{
	/** Logic files with edits made in a Server Code tab that are not saved yet (absolute paths). */
	TArray<FString> UnsavedFiles();

	/** Drops unsaved edits to files in Directory or below it, kept or open in an editor, so nothing asks to save a file deleted with it. Call it once the files are deleted. */
	void ForgetUnder(const FString& Directory);

	/** Shows Path as written to disk in every Server Code tab showing it with no unsaved edits. */
	void ShowWritten(const FString& Path);

	/** Edits kept by file while no Server Code tab shows that file. */
	CrowdyServerCodeFiles::FEditStash& Kept();

	/** Adds or removes a tab whose unsaved edits UnsavedFiles and ForgetUnder include. */
	void AddTab(SCrowdyServerCodeTab& Tab);
	void RemoveTab(SCrowdyServerCodeTab& Tab);
}
