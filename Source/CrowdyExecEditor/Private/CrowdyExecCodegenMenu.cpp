#include "CrowdyExecCodegenMenu.h"

#include "Algo/Transform.h"
#include "ContentBrowserMenuContexts.h"
#include "CrowdyExecCodegen.h"
#include "CrowdyExecDeploy.h"
#include "CrowdyExecInternal.h"
#include "CrowdyServerObjectLibrary.h"
#include "CrowdyExecLog.h"
#include "CrowdyServerCodeFiles.h"
#include "CrowdyServerComputeService.h"
#include "CrowdyServerObjectDefinition.h"
#include "FileHelpers.h"
#include "Framework/Notifications/NotificationManager.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ScopedTransaction.h"
#include "StructUtils/PropertyBag.h"
#include "Textures/SlateIcon.h"
#include "ToolMenus.h"
#include "UObject/Package.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "CrowdyExecCodegenMenu"

namespace CrowdyExecCodegenMenu
{
	FDialogAnswer DialogAnswer;

	FScopedDialogAnswers::FScopedDialogAnswers(FDialogAnswer Answer)
		: Previous(MoveTemp(DialogAnswer))
	{
		DialogAnswer = MoveTemp(Answer);
	}

	FScopedDialogAnswers::~FScopedDialogAnswers()
	{
		DialogAnswer = MoveTemp(Previous);
	}

	/** The Generate All run in progress: how it answers, what the current type's Generate said last, and what a run without a window left unanswered. */
	struct FAskLog
	{
		EGenerateAnswers Answers = EGenerateAnswers::Dialogs;
		FString LastMessage;
		bool bLastWasQuestion = false;
		TArray<FString> Unanswered;
		/** What a run without a window had no notice to show, such as the names that now stand for another type. */
		TArray<FString> Said;
		FTypeLogicGaps Gaps;
	};

	FAskLog* AskLog = nullptr;

	/** Every dialog of Generate goes through here, so a test or a run without a window can answer it. bYesIsSafe: yes keeps saved data and deletes nothing, so a run told to say yes may. */
	EAppReturnType::Type Ask(EAppMsgCategory Category, EAppMsgType::Type Type, EAppReturnType::Type Default, const FText& Message, const FText& Title, bool bYesIsSafe = false)
	{
		if (AskLog)
		{
			AskLog->LastMessage = Message.ToString();
			AskLog->bLastWasQuestion = Type != EAppMsgType::Ok;
			if (AskLog->Answers != EGenerateAnswers::Dialogs)
			{
				const EAppReturnType::Type Answer = bYesIsSafe && AskLog->Answers == EGenerateAnswers::Yes ? EAppReturnType::Yes : Default;
				if (Answer != EAppReturnType::Yes)
				{
					AskLog->Unanswered.Add(AskLog->LastMessage);
				}
				return Answer;
			}
		}
		if (DialogAnswer)
		{
			return DialogAnswer(Category, Type, Default, Message);
		}
		return FMessageDialog::Open(Category, Type, Default, Message, Title);
	}

	void FPendingRetags::Record(const FString& OldPath, const FString& NewPath)
	{
		FString Origin = OldPath;
		Moves.RemoveAndCopyValue(OldPath, Origin);
		if (Origin.Equals(NewPath, ESearchCase::CaseSensitive))
		{
			return;
		}
		Moves.Add(NewPath, Origin);
	}

	TArray<TPair<FString, FString>> FPendingRetags::TakeForPackage(const FString& PackageName)
	{
		TArray<TPair<FString, FString>> Taken;
		for (auto It = Moves.CreateIterator(); It; ++It)
		{
			if (FPackageName::ObjectPathToPackageName(It.Key()).Equals(PackageName, ESearchCase::IgnoreCase))
			{
				Taken.Add({It.Value(), It.Key()});
				It.RemoveCurrent();
			}
		}
		return Taken;
	}

	void ShowGenerateError(const UCrowdyServerObjectDefinition& Definition, const FString& Problem)
	{
		const FText Title = FText::Format(LOCTEXT("ErrorTitle", "Generate Server Code: {0}"), FText::FromString(Definition.GetName()));
		Ask(EAppMsgCategory::Error, EAppMsgType::Ok, EAppReturnType::Ok, FText::FromString(Problem), Title);
	}

	const TCHAR* const TypesPath = TEXT("src/types.rs");

	const CrowdyExecCodegen::FGeneratedFile* FindTypesFile(const CrowdyExecCodegen::FGeneratedCrate& Crate)
	{
		return Crate.Files.FindByPredicate([](const CrowdyExecCodegen::FGeneratedFile& File) { return File.Path.Equals(TypesPath, ESearchCase::CaseSensitive); });
	}

	/** Reads the types.rs under Directory that new code would replace; OutTypes stays empty when there is none. False, after saying so, when it cannot be read. */
	bool ReadCurrentTypes(const UCrowdyServerObjectDefinition& Definition, const FString& Directory, FString& OutTypes)
	{
		OutTypes.Reset();
		const FString Path = FPaths::Combine(Directory, TypesPath);
		if (!FPaths::FileExists(Path) || FFileHelper::LoadFileToString(OutTypes, *Path))
		{
			return true;
		}
		ShowGenerateError(Definition, FString::Printf(TEXT("Could not read %s, which is needed to check what the new server code changes. Close any program using it and try again."), *Path));
		return false;
	}

	/** Asks before replacing server code whose saved instances hold a struct, field or value the new code drops or retypes. False when the current code cannot be read. */
	bool ConfirmRemovals(const UCrowdyServerObjectDefinition& Definition, const CrowdyExecCodegen::FGeneratedCrate& Crate, const FString& Directory)
	{
		const CrowdyExecCodegen::FGeneratedFile* Types = FindTypesFile(Crate);
		FString OldTypes;
		if (!Types)
		{
			return true;
		}
		if (!ReadCurrentTypes(Definition, Directory, OldTypes))
		{
			return false;
		}
		if (OldTypes.IsEmpty())
		{
			return true;
		}
		const TArray<FString> Removals = CrowdyExecCodegen::FindRemovals(OldTypes, Types->Text);
		if (Removals.IsEmpty())
		{
			return true;
		}
		const FText Message = FText::Format(LOCTEXT("ConfirmRemovals", "Instances saved by the current server code hold values the new code drops:\n\n{0}\n\nKeep the old name on the server with a server name on the definition, or keep the value instead of deleting it. Regenerate anyway?"),
			FText::FromString(FString::Join(Removals, TEXT("\n"))));
		const EAppReturnType::Type Answer = Ask(EAppMsgCategory::Warning, EAppMsgType::YesNo, EAppReturnType::No, Message,
			LOCTEXT("ConfirmRemovalsTitle", "Generate Server Code"));
		return Answer == EAppReturnType::Yes;
	}

	/** The top-level `mod name;` lines of a lib.rs, trimmed, that load a module file Generate does not write (it writes only logic and types). */
	TArray<FString> HandAddedModules(const FString& Text)
	{
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, false);
		TArray<FString> Modules;
		for (FString& Line : Lines)
		{
			FString Declaration = Line;
			int32 Comment = INDEX_NONE;
			if (Declaration.FindChar(TEXT('/'), Comment) && Declaration.Mid(Comment, 2) == TEXT("//"))
			{
				Declaration.LeftInline(Comment);
			}
			Declaration.TrimEndInline();
			const int32 Mod = Declaration.Find(TEXT("mod "), ESearchCase::CaseSensitive);
			// What may stand before `mod`: nothing, a visibility such as pub(crate), or an attribute such as #[path = "x.rs"].
			const FString Before = Mod == INDEX_NONE ? FString() : Declaration.Left(Mod).TrimEnd();
			const bool bTopLevel = Mod != INDEX_NONE && !Line.StartsWith(TEXT(" ")) && !Line.StartsWith(TEXT("\t")) && Declaration.EndsWith(TEXT(";"), ESearchCase::CaseSensitive)
				&& (Before.IsEmpty() || Before.StartsWith(TEXT("pub"), ESearchCase::CaseSensitive) || Before.StartsWith(TEXT("#["), ESearchCase::CaseSensitive));
			if (!bTopLevel)
			{
				continue;
			}
			const FString Name = Declaration.Mid(Mod + 4).LeftChop(1).TrimStartAndEnd();
			if (!Name.Equals(TEXT("logic"), ESearchCase::CaseSensitive) && !Name.Equals(TEXT("types"), ESearchCase::CaseSensitive))
			{
				Modules.Add(Declaration);
			}
		}
		return Modules;
	}

	/** Asks before Generate rewrites a src/lib.rs under Directory that loads module files of the author's own. False to stop. */
	bool ConfirmLibOverwrite(const FString& Directory)
	{
		const FString Path = FPaths::Combine(Directory, TEXT("src/lib.rs"));
		FString OnDisk;
		if (!FPaths::FileExists(Path) || !FFileHelper::LoadFileToString(OnDisk, *Path))
		{
			return true;
		}
		const TArray<FString> Extra = HandAddedModules(OnDisk);
		if (Extra.IsEmpty())
		{
			return true;
		}
		const FText Message = FText::Format(LOCTEXT("ConfirmLibOverwrite", "{0} has lines Generate does not write and would remove: {1}. Generate rewrites lib.rs; declare your own modules at the top of logic.rs instead, where `mod name;` loads src/logic/name.rs. Regenerate anyway?"),
			FText::FromString(Path), FText::FromString(FString::Join(Extra, TEXT(", "))));
		return Ask(EAppMsgCategory::Warning, EAppMsgType::YesNo, EAppReturnType::No, Message, LOCTEXT("ConfirmLibOverwriteTitle", "Generate Server Code")) == EAppReturnType::Yes;
	}

	FString FindRenamedList(const UCrowdyServerObjectDefinition& Definition, const CrowdyExecCodegen::FListValueRename& Rename)
	{
		TArray<FCrowdyServerNamedList> Lists;
		Definition.GetLists(Lists);
		const FCrowdyServerNamedList* Found = Lists.FindByPredicate([&Rename](const FCrowdyServerNamedList& List)
		{
			const FPropertyBagPropertyDesc* Desc = List.Values ? List.Values->FindPropertyDescByID(Rename.ValueId) : nullptr;
			return Desc && CrowdyExec::SameKeptName(Desc->Name.ToString(), Rename.NewName);
		});
		return Found ? Found->Name : FString();
	}

	TArray<CrowdyExecCodegen::FListValueRename> KeepOldListValueNames(UCrowdyServerObjectDefinition& Definition, TConstArrayView<CrowdyExecCodegen::FListValueRename> Renames)
	{
		const FScopedTransaction Transaction(LOCTEXT("KeepOldServerNames", "Keep Old Server Names"));
		TArray<CrowdyExecCodegen::FListValueRename> NotFound;
		for (const CrowdyExecCodegen::FListValueRename& Rename : Renames)
		{
			const FString List = FindRenamedList(Definition, Rename);
			if (List.IsEmpty())
			{
				NotFound.Add(Rename);
				continue;
			}
			Definition.KeepListValueServerName(FName(*List), Rename.ValueId, Rename.OldName);
		}
		return NotFound;
	}

	/** Asks whether List values renamed since the current code keep their old names on the server; on Keep, records them and generates Crate again. False to stop. */
	bool ConfirmRenames(UCrowdyServerObjectDefinition& Definition, CrowdyExecCodegen::FGeneratedCrate& Crate, const FString& Directory)
	{
		const CrowdyExecCodegen::FGeneratedFile* Types = FindTypesFile(Crate);
		FString OldTypes;
		if (!Types)
		{
			return true;
		}
		if (!ReadCurrentTypes(Definition, Directory, OldTypes))
		{
			return false;
		}
		const TArray<CrowdyExecCodegen::FListValueRename> Found = OldTypes.IsEmpty() ? TArray<CrowdyExecCodegen::FListValueRename>() : CrowdyExecCodegen::FindRenames(OldTypes, Types->Text);
		// Lists sharing one struct each report the same rename; one line and one entry cover them all.
		TArray<CrowdyExecCodegen::FListValueRename> Renames;
		for (const CrowdyExecCodegen::FListValueRename& Rename : Found)
		{
			const bool bListed = Renames.ContainsByPredicate([&Rename](const CrowdyExecCodegen::FListValueRename& Kept)
			{
				return Kept.ValueId == Rename.ValueId && Kept.OldName.Equals(Rename.OldName, ESearchCase::CaseSensitive) && Kept.NewName.Equals(Rename.NewName, ESearchCase::CaseSensitive);
			});
			if (!bListed)
			{
				Renames.Add(Rename);
			}
		}
		if (Renames.IsEmpty())
		{
			return true;
		}
		TArray<FString> Lines;
		for (const CrowdyExecCodegen::FListValueRename& Rename : Renames)
		{
			Lines.Add(FString::Printf(TEXT("%s was renamed to %s (%s)"), *Rename.OldName, *Rename.NewName, *Rename.Struct));
		}
		const FText Message = FText::Format(LOCTEXT("ConfirmRenames", "Saved instances hold these values under their old names:\n\n{0}\n\nYes: keep the old names on the server, so saved values carry over.\nNo: rename them on the server; saved values under the old names are dropped.\nCancel: stop."),
			FText::FromString(FString::Join(Lines, TEXT("\n"))));
		const EAppReturnType::Type Answer = Ask(EAppMsgCategory::Warning, EAppMsgType::YesNoCancel, EAppReturnType::Cancel, Message,
			LOCTEXT("ConfirmRenamesTitle", "Generate Server Code"), true);
		if (Answer != EAppReturnType::Yes)
		{
			return Answer == EAppReturnType::No;
		}
		const TArray<CrowdyExecCodegen::FListValueRename> NotFound = KeepOldListValueNames(Definition, Renames);
		if (!NotFound.IsEmpty())
		{
			TArray<FString> Missing;
			for (const CrowdyExecCodegen::FListValueRename& Rename : NotFound)
			{
				Missing.Add(Rename.NewName);
			}
			ShowGenerateError(Definition, FString::Printf(TEXT("No List of this definition holds %s any more, so it is renamed on the server."), *FString::Join(Missing, TEXT(", "))));
		}
		TArray<FString> Errors;
		if (!Definition.Bake(Errors))
		{
			ShowGenerateError(Definition, FString::Join(Errors, TEXT("\n")));
			return false;
		}
		FString Error;
		if (!CrowdyExecCodegen::Generate(Definition, Crate, Error))
		{
			ShowGenerateError(Definition, Error);
			return false;
		}
		return true;
	}

	FString FindRenamedCrateDirectory(const FString& DefinitionPath, const FString& TypeName, const FString& ServerDirectory)
	{
		if (DefinitionPath.IsEmpty())
		{
			return FString();
		}
		const FString Current = FPaths::Combine(ServerDirectory, TypeName);
		const FString CurrentOwner = CrowdyExecCodegen::ReadCrateDefinition(Current);
		// A crate generated before crates named their definition is taken to be this one's.
		if (IFileManager::Get().DirectoryExists(*Current) && (CurrentOwner.IsEmpty() || CurrentOwner.Equals(DefinitionPath, ESearchCase::IgnoreCase)))
		{
			return FString();
		}
		TArray<FString> Folders;
		IFileManager::Get().FindFiles(Folders, *FPaths::Combine(ServerDirectory, TEXT("*")), false, true);
		Folders.Sort();
		FString Newest;
		FDateTime NewestStamp;
		for (const FString& Folder : Folders)
		{
			const FString Candidate = FPaths::Combine(ServerDirectory, Folder);
			if (Folder.Equals(TypeName, ESearchCase::IgnoreCase) || !CrowdyExecCodegen::ReadCrateDefinition(Candidate).Equals(DefinitionPath, ESearchCase::IgnoreCase))
			{
				continue;
			}
			// Several crates can name one definition; the one Generate wrote last is the live one.
			const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*FPaths::Combine(Candidate, TEXT("Cargo.toml")));
			if (Newest.IsEmpty() || Stamp > NewestStamp)
			{
				Newest = Candidate;
				NewestStamp = Stamp;
			}
		}
		return Newest;
	}

	bool MoveCrateDirectory(const FString& From, const FString& To, FString& OutError)
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		if (PlatformFile.DirectoryExists(*To) || PlatformFile.FileExists(*To))
		{
			OutError = FString::Printf(TEXT("%s already exists, so %s was not moved."), *To, *From);
			return false;
		}
		if (!PlatformFile.DirectoryExists(*From))
		{
			OutError = FString::Printf(TEXT("%s does not exist, so nothing was moved."), *From);
			return false;
		}
		if (!PlatformFile.MoveFile(*To, *From))
		{
			OutError = FString::Printf(TEXT("Could not move %s to %s; nothing was moved. Close any program using a file in it and try again."), *From, *To);
			return false;
		}
		return true;
	}

	/** Whether the type's My Own File Logic File sits in Folder, where a move of Folder would leave the Logic File chosen at a path that no longer holds it. */
	bool OwnLogicFileIsUnder(const UCrowdyServerObjectDefinition& Definition, const FString& Folder)
	{
		if (!CrowdyExecCodegen::HasOwnLogicFile(Definition))
		{
			return false;
		}
		const FString LogicFile = CrowdyExecCodegen::GetLogicFile(Definition);
		return !LogicFile.IsEmpty() && FPaths::IsUnderDirectory(LogicFile, Folder);
	}

	/** What a move does to the type's logic: keeps logic.rs, or, with My Own File, leaves the Logic File alone unless it sits in the moved folder. */
	FText DescribeLogicOnMove(const UCrowdyServerObjectDefinition& Definition, const FString& Old, const FText& OldName, const FText& NewName)
	{
		if (!CrowdyExecCodegen::HasOwnLogicFile(Definition))
		{
			return LOCTEXT("MoveKeepsLogic", "Moving keeps your logic.rs.");
		}
		if (!OwnLogicFileIsUnder(Definition, Old))
		{
			return FText::Format(LOCTEXT("LogicElsewhere", "Your Logic File is outside Server/{0}, so moving does not touch it."), OldName);
		}
		return FText::Format(LOCTEXT("LogicInside", "Warning: your Logic File {0} is inside Server/{1}. After moving, choose it again under Server/{2}."),
			FText::FromString(CrowdyExecCodegen::GetLogicFile(Definition)), OldName, NewName);
	}

	/** Asks what to do with server code left under the previous Type Name. False to stop; OutMoveFrom is the folder to move into Directory, empty to leave it. */
	bool AskAboutRename(const UCrowdyServerObjectDefinition& Definition, const FString& Directory, FString& OutMoveFrom)
	{
		OutMoveFrom.Reset();
		const FString Old = FindRenamedCrateDirectory(Definition.GetPathName(), Definition.TypeName, FPaths::GetPath(Directory));
		if (Old.IsEmpty())
		{
			return true;
		}
		const FText OldName = FText::FromString(FPaths::GetCleanFilename(Old));
		const FText NewName = FText::FromString(Definition.TypeName);
		const FText Changed = FText::Format(LOCTEXT("TypeNameChanged", "The Type Name changed from {0} to {1} since the server code was last generated. The server treats {1} as a new type, so saved {0} instances are not carried over."),
			OldName, NewName);
		const FText Title = LOCTEXT("TypeNameChangedTitle", "Generate Server Code");
		if (IFileManager::Get().DirectoryExists(*Directory))
		{
			const FText Message = FText::Format(LOCTEXT("BothFoldersExist", "{0}\n\nServer/{1} and Server/{2} both exist, so nothing is moved. Server/{2} was generated for {3}. Generate into Server/{2} anyway and leave Server/{1} as it is?"),
				Changed, OldName, NewName, FText::FromString(CrowdyExecCodegen::ReadCrateDefinition(Directory)));
			return Ask(EAppMsgCategory::Warning, EAppMsgType::YesNo, EAppReturnType::No, Message, Title) == EAppReturnType::Yes;
		}
		const FText Message = FText::Format(LOCTEXT("MoveOldFolder", "{0}\n\n{3}\n\nYes: move Server/{1} to Server/{2}, then generate.\nNo: generate into a new Server/{2} and leave Server/{1} as it is.\nCancel: stop."),
			Changed, OldName, NewName, DescribeLogicOnMove(Definition, Old, OldName, NewName));
		const EAppReturnType::Type Answer = Ask(EAppMsgCategory::Warning, EAppMsgType::YesNoCancel, EAppReturnType::Cancel, Message, Title, !OwnLogicFileIsUnder(Definition, Old));
		if (Answer != EAppReturnType::Yes)
		{
			return Answer == EAppReturnType::No;
		}
		// A save after the move would recreate the old folder with half the code.
		for (const FString& File : CrowdyServerCodeEdits::UnsavedFiles())
		{
			if (FPaths::IsUnderDirectory(File, Old))
			{
				ShowGenerateError(Definition, FString::Printf(TEXT("%s has edits that are not saved. Save or discard them, then generate again."), *File));
				return false;
			}
		}
		OutMoveFrom = Old;
		return true;
	}

	/** A raw identifier such as r#match reads as the function's name. */
	FString ReadableMethod(const CrowdyExecCodegen::FLogicStub& Stub)
	{
		return Stub.Method.StartsWith(TEXT("r#"), ESearchCase::CaseSensitive) ? Stub.Method.RightChop(2) : Stub.Method;
	}

	FText DescribeLogicGaps(const CrowdyExecCodegen::FLogicGaps& Gaps, const FString& FileName, const FString& StateName)
	{
		TArray<FString> Missing;
		Algo::Transform(Gaps.Missing, Missing, &ReadableMethod);
		const FText File = FText::FromString(FileName);
		const FText Leftover = FText::Format(LOCTEXT("GapsLeftover", "{0} defines {1}, which this type no longer has: rename or delete it."), File, FText::FromString(FString::Join(Gaps.Leftover, TEXT(", "))));
		if (Missing.IsEmpty())
		{
			return Leftover;
		}
		const FText Names = FText::FromString(FString::Join(Missing, TEXT(", ")));
		const FText Found = Gaps.InsertAt == INDEX_NONE
			? FText::Format(LOCTEXT("GapsMissingNoBlock", "{0} is missing {1}, and has no impl Functions for {2} block to add them to."), File, Names, FText::FromString(StateName))
			: FText::Format(LOCTEXT("GapsMissing", "{0} is missing {1}, which this type now has."), File, Names);
		const FText MissingText = Gaps.Macros.IsEmpty() ? Found
			: FText::Format(LOCTEXT("GapsMacros", "{0} {1} also uses {2}, which may define some of these; check before adding them."), Found, File, FText::FromString(FString::Join(Gaps.Macros, TEXT(", "))));
		return Gaps.Leftover.IsEmpty() ? MissingText : FText::Format(LOCTEXT("GapsBoth", "{0} {1}"), MissingText, Leftover);
	}

	FString JoinStubs(const CrowdyExecCodegen::FLogicGaps& Gaps)
	{
		TArray<FString> Bodies;
		for (const CrowdyExecCodegen::FLogicStub& Stub : Gaps.Missing)
		{
			Bodies.Add(Stub.Text);
		}
		return FString::Join(Bodies, TEXT("\n"));
	}

	/** After Generate: offers to add the functions the logic file lacks, and names those the type no longer has. A Server Code tab with unsaved edits to the file shows the same instead. */
	void OfferLogicStubs(const UCrowdyServerObjectDefinition& Definition, const CrowdyExecCodegen::FGeneratedCrate& Crate, const FString& LogicFile)
	{
		const bool bUnsaved = CrowdyServerCodeEdits::UnsavedFiles().ContainsByPredicate([&LogicFile](const FString& File) { return FPaths::IsSamePath(File, LogicFile); });
		const CrowdyServerCodeFiles::FDiskText OnDisk = bUnsaved ? CrowdyServerCodeFiles::FDiskText() : CrowdyServerCodeFiles::Read(LogicFile);
		if (!OnDisk.bReadable)
		{
			return;
		}
		const CrowdyExecCodegen::FLogicGaps Gaps = CrowdyExecCodegen::FindLogicGaps(Crate, OnDisk.Text);
		if (Gaps.Missing.IsEmpty() && Gaps.Leftover.IsEmpty())
		{
			return;
		}
		if (AskLog)
		{
			AskLog->Gaps.LogicFile = LogicFile;
			Algo::Transform(Gaps.Missing, AskLog->Gaps.Missing, &ReadableMethod);
			AskLog->Gaps.Leftover = Gaps.Leftover;
		}
		const FText Title = FText::Format(LOCTEXT("GapsTitle", "Generate Server Code: {0}"), FText::FromString(Definition.GetName()));
		const FText Described = DescribeLogicGaps(Gaps, FPaths::GetCleanFilename(LogicFile), Crate.StateName);
		const FText Unbuildable = LOCTEXT("GapsUnbuildable", "Until then the server code does not build.");
		if (Gaps.Missing.IsEmpty())
		{
			Ask(EAppMsgCategory::Warning, EAppMsgType::Ok, EAppReturnType::Ok, FText::Format(LOCTEXT("GapsLeftoverOnly", "{0}\n\n{1}"), Described, Unbuildable), Title);
			return;
		}
		// A file that is not UTF-8 would be rewritten as UTF-8, and one with macros may define the functions already, so it is left alone, as Save asks before doing.
		if (Gaps.InsertAt == INDEX_NONE || !OnDisk.bUtf8 || !Gaps.Macros.IsEmpty())
		{
			const FString Stubs = JoinStubs(Gaps);
			const FText Kept = OnDisk.bUtf8 ? FText::GetEmpty()
				: FText::Format(LOCTEXT("GapsNotUtf8", "{0} is not UTF-8 text, so it is left as it is. "), FText::FromString(FPaths::GetCleanFilename(LogicFile)));
			const bool bClipboard = !AskLog || AskLog->Answers == EGenerateAnswers::Dialogs;
			if (bClipboard)
			{
				FPlatformApplicationMisc::ClipboardCopy(*Stubs);
			}
			const FText Hand = bClipboard ? LOCTEXT("GapsCopied", "These empty versions are on the clipboard; paste them into the block that implements Functions:")
				: LOCTEXT("GapsNotCopied", "Add these empty versions to the block that implements Functions:");
			Ask(EAppMsgCategory::Warning, EAppMsgType::Ok, EAppReturnType::Ok, FText::Format(LOCTEXT("GapsHandOver", "{0}\n\n{1}{2}\n\n{3}"), Described, Kept, Hand, FText::FromString(Stubs)), Title);
			return;
		}
		const FText Question = FText::Format(LOCTEXT("GapsAsk", "{0}\n\nAdd empty versions of the missing functions? A function fails with \"not written yet\" until you write it; a timer or event does nothing.{1}\n\nThis writes them into {2} on disk. If another program has it open with unsaved changes, save or close it there first, or its next save removes them."),
			Described, Gaps.Leftover.IsEmpty() ? FText::GetEmpty() : FText::Format(LOCTEXT("GapsAskLeftover", "\n\n{0}"), Unbuildable), FText::FromString(FPaths::GetCleanFilename(LogicFile)));
		if (Ask(EAppMsgCategory::Warning, EAppMsgType::YesNo, EAppReturnType::No, Question, Title, true) != EAppReturnType::Yes)
		{
			return;
		}
		// The file may have changed while the question was open, for example saved from another editor.
		const CrowdyServerCodeFiles::FDiskText Now = CrowdyServerCodeFiles::Read(LogicFile);
		const CrowdyExecCodegen::FLogicGaps Fresh = CrowdyExecCodegen::FindLogicGaps(Crate, Now.Text);
		if (!Now.bReadable || !Now.bUtf8 || Fresh.InsertAt == INDEX_NONE || !Fresh.Macros.IsEmpty())
		{
			ShowGenerateError(Definition, FString::Printf(TEXT("%s changed while Generate was asking, so nothing was added. Generate again."), *LogicFile));
			return;
		}
		if (!CrowdyServerCodeFiles::Write(LogicFile, CrowdyExecCodegen::AddLogicStubs(Now.Text, Fresh), Now.bCrlf))
		{
			ShowGenerateError(Definition, FString::Printf(TEXT("Could not write %s. Close any program using it and try again."), *LogicFile));
			return;
		}
		TArray<FString> Inserted;
		for (const CrowdyExecCodegen::FLogicStub& Stub : Fresh.Missing)
		{
			Inserted.Add(Stub.Method);
		}
		CrowdyServerCodeEdits::NoteStubsInserted(LogicFile, MoveTemp(Inserted));
		CrowdyServerCodeEdits::ShowWritten(LogicFile);
		if (AskLog)
		{
			AskLog->Gaps.bStubsAdded = true;
		}
	}

	/** The generated names that stand for another type than in the types.rs under Directory; none when there is no such file. */
	TArray<FString> ReadTypeIdentityChanges(const CrowdyExecCodegen::FGeneratedCrate& Crate, const FString& Directory)
	{
		const CrowdyExecCodegen::FGeneratedFile* Types = FindTypesFile(Crate);
		const FString Path = FPaths::Combine(Directory, TypesPath);
		FString OldTypes;
		if (!Types || !FPaths::FileExists(Path) || !FFileHelper::LoadFileToString(OldTypes, *Path))
		{
			return TArray<FString>();
		}
		return CrowdyExecCodegen::FindTypeIdentityChanges(OldTypes, Types->Text);
	}

	/** Says, without stopping Generate, which names now stand for another type, since logic code relying on the old one stops building. */
	void ShowTypeIdentityChanges(const TArray<FString>& Changes)
	{
		if (Changes.IsEmpty())
		{
			return;
		}
		const FText Message = FText::FromString(FString::Join(Changes, TEXT("\n")));
		if (AskLog && AskLog->Answers != EGenerateAnswers::Dialogs)
		{
			AskLog->Said.Add(Message.ToString());
			UE_LOG(LogCrowdyExec, Warning, TEXT("%s"), *Message.ToString());
			return;
		}
		if (DialogAnswer)
		{
			DialogAnswer(EAppMsgCategory::Warning, EAppMsgType::Ok, EAppReturnType::Ok, Message);
			return;
		}
		UE_LOG(LogCrowdyExec, Warning, TEXT("%s"), *Message.ToString());
		FNotificationInfo Info(Message);
		Info.ExpireDuration = 15.0f;
		FSlateNotificationManager::Get().AddNotification(Info);
	}

	FString CrateDirectoryUnder(const FString& ServerDirectory, const UCrowdyServerObjectDefinition& Definition)
	{
		FString Directory = FPaths::ConvertRelativePathToFull(FPaths::Combine(ServerDirectory, Definition.TypeName));
		FPaths::NormalizeDirectoryName(Directory);
		return Directory;
	}

	bool GenerateServerCode(UCrowdyServerObjectDefinition& Definition)
	{
		return GenerateServerCode(Definition, CrowdyExecCodegen::GetServerDirectory());
	}

	bool GenerateServerCode(UCrowdyServerObjectDefinition& Definition, const FString& ServerDirectory)
	{
		if (Definition.TypeName.Equals(UCrowdyServerObjectDefinition::RootTypeName, ESearchCase::CaseSensitive))
		{
			const bool bSdkRoot = Definition.GetPathName().Equals(UCrowdyServerObjectLibrary::PlayersOnlineDefinitionPath, ESearchCase::IgnoreCase);
			ShowGenerateError(Definition, bSdkRoot ? TEXT("This is the Crowdy SDK's own root; its server code is built into every deploy, so there is nothing to generate.")
				: TEXT("root is the name of the platform's root Server Object type; choose another Type Name."));
			return false;
		}
		TArray<FString> Errors;
		if (!Definition.Bake(Errors))
		{
			ShowGenerateError(Definition, FString::Join(Errors, TEXT("\n")));
			return false;
		}
		CrowdyExecCodegen::FGeneratedCrate Crate;
		FString Error;
		if (!CrowdyExecCodegen::Generate(Definition, Crate, Error))
		{
			ShowGenerateError(Definition, Error);
			return false;
		}
		const FString Directory = CrateDirectoryUnder(ServerDirectory, Definition);
		if (FPaths::FileExists(FPaths::Combine(Directory, TEXT("Cargo.toml"))) && !CrowdyExecCodegen::IsGeneratedCrateFolder(Directory))
		{
			ShowGenerateError(Definition, FString::Printf(TEXT("%s holds server code the Crowdy SDK did not generate, so Generate would overwrite it. Choose another Type Name, or move that folder."), *Directory));
			return false;
		}
		FString MoveFrom;
		if (!AskAboutRename(Definition, Directory, MoveFrom))
		{
			return false;
		}
		// A move already told the developer that saved instances do not carry over.
		if (MoveFrom.IsEmpty() && (!ConfirmRenames(Definition, Crate, Directory) || !ConfirmRemovals(Definition, Crate, Directory)))
		{
			return false;
		}
		if (!ConfirmLibOverwrite(MoveFrom.IsEmpty() ? Directory : MoveFrom))
		{
			return false;
		}
		if (!MoveFrom.IsEmpty() && !MoveCrateDirectory(MoveFrom, Directory, Error))
		{
			ShowGenerateError(Definition, Error);
			return false;
		}
		const TArray<FString> IdentityChanges = ReadTypeIdentityChanges(Crate, Directory);
		TArray<FString> Written;
		if (!CrowdyExecCodegen::WriteCrate(Crate, Directory, Written, Error))
		{
			ShowGenerateError(Definition, Error);
			return false;
		}
		// A Generate All run names what it wrote in one notice.
		if (!AskLog)
		{
			FNotificationInfo Info(FText::Format(LOCTEXT("Written", "Wrote the server code of {0} to {1}"), FText::FromString(Definition.TypeName), FText::FromString(Directory)));
			Info.ExpireDuration = 8.0f;
			FSlateNotificationManager::Get().AddNotification(Info);
		}
		ShowTypeIdentityChanges(IdentityChanges);
		const FString LogicFile = CrowdyExecCodegen::GetLogicFile(Definition, Directory);
		if (LogicFile.IsEmpty())
		{
			ShowGenerateError(Definition, TEXT("Code Source is My Own File but no file is chosen. Choose one, or switch Code Source to Generated."));
			return true;
		}
		if (!FPaths::FileExists(LogicFile))
		{
			ShowGenerateError(Definition, FString::Printf(TEXT("The Logic File %s does not exist. Choose an existing file, or switch Code Source to Generated."), *LogicFile));
			return true;
		}
		OfferLogicStubs(Definition, Crate, LogicFile);
		return true;
	}

	FGenerateAllResult GenerateAll(TConstArrayView<UCrowdyServerObjectDefinition*> Definitions, const FString& ServerDirectory, EGenerateAnswers Answers, const FString& OnlyType)
	{
		FGenerateAllResult Result;
		const bool bNamed = !OnlyType.IsEmpty();
		if (bNamed && !Definitions.ContainsByPredicate([&OnlyType](const UCrowdyServerObjectDefinition* Each) { return Each->TypeName.Equals(OnlyType, ESearchCase::CaseSensitive); }))
		{
			const FString Known = FString::JoinBy(Definitions, TEXT(", "), [](const UCrowdyServerObjectDefinition* Each) { return Each->TypeName; });
			Result.Refused.Add({OnlyType, FString::Printf(TEXT("No Server Object type is named %s in the project. The types are: %s"), *OnlyType, *Known)});
			return Result;
		}
		FAskLog Log;
		Log.Answers = Answers;
		TGuardValue<FAskLog*> Scope(AskLog, &Log);
		for (UCrowdyServerObjectDefinition* Definition : Definitions)
		{
			if (bNamed ? !Definition->TypeName.Equals(OnlyType, ESearchCase::CaseSensitive) : Definition->IsGameKitStarter())
			{
				continue;
			}
			const CrowdyExecDeploy::FTypeState State = CrowdyExecDeploy::InspectType(*Definition, CrateDirectoryUnder(ServerDirectory, *Definition));
			if (State.State == CrowdyExecDeploy::ECrateState::Invalid)
			{
				(bNamed ? Result.Refused : Result.Skipped).Add({State.TypeName, State.Problem});
				continue;
			}
			if (!bNamed && State.bNeedsDeveloper)
			{
				Result.Skipped.Add({State.TypeName, State.Problem});
				continue;
			}
			if (!bNamed && !CrowdyExecDeploy::NeedsGenerating(State))
			{
				continue;
			}
			const bool bShared = Definitions.ContainsByPredicate([Definition](const UCrowdyServerObjectDefinition* Other)
			{
				return Other != Definition && Other->TypeName.Equals(Definition->TypeName, ESearchCase::IgnoreCase);
			});
			if (bShared)
			{
				Result.Refused.Add({State.TypeName, CrowdyServerCodeFiles::SharedTypeNameProblem().ToString()});
				continue;
			}
			Log.LastMessage.Reset();
			Log.bLastWasQuestion = false;
			Log.Unanswered.Reset();
			Log.Said.Reset();
			Log.Gaps = FTypeLogicGaps();
			UPackage* Package = Definition->GetOutermost();
			const bool bWasDirty = Package->IsDirty();
			const bool bGenerated = GenerateServerCode(*Definition, ServerDirectory);
			if (!bWasDirty && Package->IsDirty())
			{
				Result.Dirtied.AddUnique(Package);
			}
			if (bGenerated && !Log.Gaps.LogicFile.IsEmpty())
			{
				Log.Gaps.TypeName = State.TypeName;
				Result.LogicGaps.Add(MoveTemp(Log.Gaps));
			}
			if (bGenerated)
			{
				Result.Generated.Add(State.TypeName);
				Algo::Transform(Log.Unanswered, Result.Notes, [&State](const FString& Text) { return FTypeMessage{State.TypeName, Text}; });
				Algo::Transform(Log.Said, Result.Notes, [&State](const FString& Text) { return FTypeMessage{State.TypeName, Text}; });
				continue;
			}
			Result.Refused.Add({State.TypeName, Log.LastMessage});
			Result.bCancelled = Answers == EGenerateAnswers::Dialogs && Log.bLastWasQuestion;
			if (Result.bCancelled)
			{
				break;
			}
		}
		return Result;
	}

	FGenerateAllResult GenerateAllInProject(EGenerateAnswers Answers, const FString& OnlyType)
	{
		FGenerateAllResult Result = GenerateAll(CrowdyExecDeploy::FindProjectDefinitions(), CrowdyExecCodegen::GetServerDirectory(), Answers, OnlyType);
		Result.bSaveFailed = !Result.Dirtied.IsEmpty() && !UEditorLoadingAndSavingUtils::SavePackages(Result.Dirtied, false);
		return Result;
	}

	FText DescribeGenerateAll(const FGenerateAllResult& Result)
	{
		TArray<FText> Lines;
		if (!Result.Generated.IsEmpty())
		{
			Lines.Add(FText::Format(LOCTEXT("GeneratedAll", "Wrote the server code of {0}."), FText::FromString(FString::Join(Result.Generated, TEXT(", ")))));
		}
		for (const FTypeMessage& Refused : Result.Refused)
		{
			FString Reason = Refused.Text;
			Refused.Text.Split(TEXT("\n"), &Reason, nullptr);
			Lines.Add(FText::Format(LOCTEXT("NotGeneratedOne", "{0} was not generated: {1}"), FText::FromString(Refused.TypeName), FText::FromString(Reason)));
		}
		for (const FTypeMessage& Skipped : Result.Skipped)
		{
			Lines.Add(FText::Format(LOCTEXT("SkippedOne", "{0} was skipped, as it needs you first: {1}"), FText::FromString(Skipped.TypeName), FText::FromString(Skipped.Text)));
		}
		if (Result.bCancelled)
		{
			Lines.Add(LOCTEXT("GenerateAllStopped", "The types after it were left as they are."));
		}
		if (Result.bSaveFailed)
		{
			Lines.Add(LOCTEXT("GenerateAllNotSaved", "The definitions it changed could not be saved; save them from the Content Browser."));
		}
		return Lines.IsEmpty() ? LOCTEXT("NothingToGenerate", "Every type's server code is up to date.") : FText::Join(FText::FromString(TEXT("\n")), Lines);
	}

	/** Refuses, as the Server Code tab does, a definition whose Type Name another definition also has, since both would write one crate folder. True when refused. */
	bool RefuseSharedTypeName(const UCrowdyServerObjectDefinition& Definition)
	{
		FCrowdyServerComputeService& Compute = FCrowdyServerComputeService::Get();
		// The project's list may predate a duplicate or a Type Name edit.
		if (!Compute.IsBusy())
		{
			Compute.RefreshTypes();
		}
		if (!Compute.IsSharedTypeName(Definition.TypeName))
		{
			return false;
		}
		const FString Self = Definition.GetPathName();
		const CrowdyExecDeploy::FTypeState* Other = Compute.GetProject().Types.FindByPredicate([&Definition, &Self](const CrowdyExecDeploy::FTypeState& Type)
		{
			return Type.TypeName.Equals(Definition.TypeName, ESearchCase::IgnoreCase) && !Type.AssetPath.Equals(Self, ESearchCase::IgnoreCase);
		});
		const FText Problem = CrowdyServerCodeFiles::SharedTypeNameProblem();
		const FText Shown = Other ? FText::Format(LOCTEXT("SharedTypeNameWith", "{0} {1} uses it too."), Problem, FText::FromString(Other->AssetPath)) : Problem;
		ShowGenerateError(Definition, Shown.ToString());
		return true;
	}

	void ExecuteGenerate(const FToolMenuContext& MenuContext)
	{
		const UContentBrowserAssetContextMenuContext* Context = UContentBrowserAssetContextMenuContext::FindContextWithAssets(MenuContext);
		if (!Context)
		{
			return;
		}
		for (UCrowdyServerObjectDefinition* Definition : Context->LoadSelectedObjects<UCrowdyServerObjectDefinition>())
		{
			if (!RefuseSharedTypeName(*Definition))
			{
				GenerateServerCode(*Definition);
			}
		}
	}

	void ExtendAssetMenu()
	{
		UToolMenu* Menu = UE::ContentBrowser::ExtendToolMenu_AssetContextMenu(UCrowdyServerObjectDefinition::StaticClass());
		FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("GetAssetActions"));
		Section.AddMenuEntry(TEXT("CrowdyExecGenerateServerCode"), LOCTEXT("GenerateLabel", "Generate Server Code"),
			LOCTEXT("GenerateTooltip", "Writes this type's server code under Server/<Type Name>"), FSlateIcon(),
			FToolUIAction(FToolMenuExecuteAction::CreateStatic(&ExecuteGenerate)));
	}
}

#undef LOCTEXT_NAMESPACE
