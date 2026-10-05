#pragma once

#include "CoreMinimal.h"

class UCrowdyServerObjectDefinition;

/** Writes a Server Object type's server code: a Rust crate whose types and glue come from its definition. */
namespace CrowdyExecCodegen
{
	struct FGeneratedFile
	{
		/** Relative to the crate folder, with forward slashes: Cargo.toml, src/lib.rs, src/types.rs, src/logic.rs. */
		FString Path;
		FString Text;
	};

	/** A function the generated Functions trait asks the logic file for, as a new logic.rs writes it. */
	struct FLogicStub
	{
		/** The Rust function name as the logic file spells it: end_match, r#match. */
		FString Method;
		/** The whole function, indented for its impl block, ending in a line break. */
		FString Text;
	};

	struct FGeneratedCrate
	{
		/** The crate's name, which is the type's name on the server. */
		FString Name;
		TArray<FGeneratedFile> Files;
		/** The state struct the logic file implements Functions on. */
		FString StateName;
		/** Every function the Functions trait asks for, in its order. */
		TArray<FLogicStub> Stubs;
	};

	/** How a logic file falls short of a crate's Functions trait. */
	struct FLogicGaps
	{
		/** The trait's functions it does not define, in the trait's order. */
		TArray<FLogicStub> Missing;
		/** Functions its impl Functions block defines that the trait no longer has. */
		TArray<FString> Leftover;
		/** Where Missing goes: the closing brace of impl Functions for the state struct; INDEX_NONE when there is no such block. */
		int32 InsertAt = INDEX_NONE;
	};

	/** Compares Logic (LF line breaks) with what Crate's Functions trait asks for, skipping comments, strings and characters. */
	CROWDYEXECEDITOR_API FLogicGaps FindLogicGaps(const FGeneratedCrate& Crate, const FString& Logic);

	/** Logic with Gaps.Missing written just before Gaps.InsertAt; Logic unchanged when nothing is missing or there is nowhere to write it. */
	CROWDYEXECEDITOR_API FString AddLogicStubs(const FString& Logic, const FLogicGaps& Gaps);

	/** Builds every file of the crate, src/logic.rs only when Code Source is Generated. The same definition always gives the same text. False with OutError when a type cannot be written. */
	CROWDYEXECEDITOR_API bool Generate(const UCrowdyServerObjectDefinition& Definition, FGeneratedCrate& OutCrate, FString& OutError);

	/** A List value that kept its id under a new name. Struct is the struct or List name types.rs gives it; the names are server names. */
	struct FListValueRename
	{
		FString Struct;
		FString OldName;
		FString NewName;
		FGuid ValueId;
	};

	/** What replacing OldTypes with NewTypes (two types.rs texts) would lose: a removed struct, field or enum value, or a changed field type. One line each. A renamed List value is not lost; only a change of its type is reported. */
	CROWDYEXECEDITOR_API TArray<FString> FindRemovals(const FString& OldTypes, const FString& NewTypes);

	/** The List values NewTypes renames: the same id under another name, the old name gone. Each struct sharing a List's struct reports it under its own name. None for a file written before ids. */
	CROWDYEXECEDITOR_API TArray<FListValueRename> FindRenames(const FString& OldTypes, const FString& NewTypes);

	/** The names NewTypes gives another type than OldTypes did: a List that became a struct of its own, or another name for a struct. One sentence each, saying what no longer builds. */
	CROWDYEXECEDITOR_API TArray<FString> FindTypeIdentityChanges(const FString& OldTypes, const FString& NewTypes);

	/** Writes the crate under Directory. src/logic.rs, when the crate has one, is written only when absent, since it holds the user's code. */
	CROWDYEXECEDITOR_API bool WriteCrate(const FGeneratedCrate& Crate, const FString& Directory, TArray<FString>& OutWritten, FString& OutError);

	/** The path of the definition asset a crate was generated from, as its Cargo.toml records it under [package.metadata.crowdy]; empty when the file or the entry is missing, as for a definition with no asset. */
	CROWDYEXECEDITOR_API FString ReadCrateDefinition(const FString& Directory);

	/** CargoText without its [package.metadata.crowdy] section, which the platform's build refuses: the Cargo.toml a deploy sends. */
	CROWDYEXECEDITOR_API FString WithoutCrateDefinition(const FString& CargoText);

	/** Points every crate folder directly under ServerDirectory whose Cargo.toml records the definition at OldPath (in any case) at NewPath, as after the asset is moved or renamed. Only that entry changes; a file that cannot be replaced stays as it was, with a warning. Returns how many crates now record NewPath. */
	CROWDYEXECEDITOR_API int32 RetagCrates(const FString& ServerDirectory, const FString& OldPath, const FString& NewPath);

	/** The project's Server folder, absolute, which holds a crate folder per type. */
	CROWDYEXECEDITOR_API FString GetServerDirectory();

	/** Where a definition's server code lives: Server/<type name> in the project folder. */
	CROWDYEXECEDITOR_API FString GetCrateDirectory(const UCrowdyServerObjectDefinition& Definition);

	/** True when Code Source is My Own File, whether or not a Logic File is chosen. */
	CROWDYEXECEDITOR_API bool HasOwnLogicFile(const UCrowdyServerObjectDefinition& Definition);

	/** The file holding the type's Server Functions, absolute: src/logic.rs in its crate for Generated, else its Logic File resolved against the project folder, empty when none is chosen. */
	CROWDYEXECEDITOR_API FString GetLogicFile(const UCrowdyServerObjectDefinition& Definition);

	/** The same, with the crate at CrateDirectory instead of GetCrateDirectory. */
	CROWDYEXECEDITOR_API FString GetLogicFile(const UCrowdyServerObjectDefinition& Definition, const FString& CrateDirectory);
}
