#pragma once

#include "CoreMinimal.h"
#include "CrowdyExecCodegen.h"

class UCrowdyServerObjectDefinition;

namespace CrowdyExecCodegenMenu
{
	/** What a logic file lacks and has to spare, in a sentence or two: "logic.rs is missing join, which this type now has." */
	FText DescribeLogicGaps(const CrowdyExecCodegen::FLogicGaps& Gaps, const FString& FileName, const FString& StateName);

	/** The missing functions as they would be added, one blank line apart. */
	FString JoinStubs(const CrowdyExecCodegen::FLogicGaps& Gaps);

	/** Bakes the definition and writes its server code, asking first when the new code renames or drops saved values or the Type Name changed; reports every problem in a dialog. Then offers to add the functions the logic file lacks. True when the code was written. */
	bool GenerateServerCode(UCrowdyServerObjectDefinition& Definition);

	/** The same, with ServerDirectory in place of the project's Server folder. */
	bool GenerateServerCode(UCrowdyServerObjectDefinition& Definition, const FString& ServerDirectory);

	/** Says so in a dialog and returns true when another of the project's definitions has this one's Type Name. */
	bool RefuseSharedTypeName(const UCrowdyServerObjectDefinition& Definition);

	/** Gets a dialog's category, buttons, default button and message, and returns the button pressed. */
	using FDialogAnswer = TFunction<EAppReturnType::Type(EAppMsgCategory, EAppMsgType::Type, EAppReturnType::Type, const FText&)>;

	/** Answers every dialog of Generate in place of the user while it lives, and gets its notices as Ok messages, for tests. */
	class FScopedDialogAnswers
	{
	public:
		explicit FScopedDialogAnswers(FDialogAnswer Answer);
		~FScopedDialogAnswers();

		FScopedDialogAnswers(const FScopedDialogAnswers&) = delete;
		FScopedDialogAnswers& operator=(const FScopedDialogAnswers&) = delete;

	private:
		FDialogAnswer Previous;
	};

	/** Definition asset moves not saved yet. A crate is retagged only once the moved asset is saved, so it never names a path that is not on disk. */
	class FPendingRetags
	{
	public:
		/** Records the asset at OldPath moving to NewPath; moves in a row keep the first path, and a move back to it drops the entry. */
		void Record(const FString& OldPath, const FString& NewPath);

		/** Removes and returns the moves whose new path is in the package PackageName, each as {path on disk, new path}. */
		TArray<TPair<FString, FString>> TakeForPackage(const FString& PackageName);

	private:
		/** New path to the path the asset had before its first unsaved move. */
		TMap<FString, FString> Moves;
	};

	/** The crate folder under ServerDirectory that names the definition at DefinitionPath under another folder name, when ServerDirectory/TypeName does not already hold that definition's crate; empty otherwise. */
	FString FindRenamedCrateDirectory(const FString& DefinitionPath, const FString& TypeName, const FString& ServerDirectory);

	/** The List a renamed value belongs to: the first of the definition's Lists holding ValueId under NewName, since Lists of one shape share the struct named after the first. Empty when none does. */
	FString FindRenamedList(const UCrowdyServerObjectDefinition& Definition, const CrowdyExecCodegen::FListValueRename& Rename);

	/** Keeps each renamed value's old name on the server through List Value Names, in one undo step. Returns the renames whose List was not found. */
	TArray<CrowdyExecCodegen::FListValueRename> KeepOldListValueNames(UCrowdyServerObjectDefinition& Definition, TConstArrayView<CrowdyExecCodegen::FListValueRename> Renames);

	/** Renames the folder From to To in one step, so a failure leaves every file where it was. Refuses when To already exists. */
	bool MoveCrateDirectory(const FString& From, const FString& To, FString& OutError);

	/** Adds Generate Server Code to the Content Browser menu of Server Object definitions. Call inside a tool menu owner scope. */
	void ExtendAssetMenu();
}
