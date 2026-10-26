#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "Templates/Function.h"
#include "CrowdyServerComputeCommandlet.generated.h"

class UCrowdyServerObjectDefinition;

/**
 * The Server Code tab's operations from the command line, for scripts and CI, as the developer signed in to Crowdy Studio:
 *   UnrealEditor-Cmd <project> -run=CrowdyServerCompute -op=<operation> [-out=<file>]
 * Operations: status, versions, changes, deploy, starters, activate -version=N, disable -type=T, enable -type=T,
 * restart [-type=T] -yes (without -type, every type with an instance running other code than the active version's; it stops every running instance of those types and, without -yes, only lists them),
 * logs [-type=T] [-flow=F] [-level=all|info|warnings|errors] [-limit=N] [-before=<nextBefore of the previous page>], stats [-type=T] [-window=hour|day|week]. changes compares each type with the live version; deploy records
 * each type's deployed revision beside its server code. deploy, starters, activate, disable and enable change the app's live
 * server code and run only with -yes. generate [-yes] writes the server code of every type that is out of date or has none and saves the
 * definitions it changed (with -type=T, that one type, up to date or not); it exits 1 if any type was refused (a named type that does not bake is), lists in skipped the types it left alone because they need you first, and in saved the definitions it saved, and -yes answers the
 * questions that keep saved data. enable without -type switches the whole app on. whoami says whether Crowdy Studio is signed in on
 * this machine and which app the project uses, without the token. exporttype [-type=T] [-spec=<file>] puts a type's settings, as a JSON
 * spec, in the result's definition and in the file (without -type, a new type's template). newtype -spec=<json or @file> [-path=...]
 * makes a type from a spec, and edittype -type=T -spec=... [-yes] replaces a type's settings with a spec that holds all of them; both
 * refuse, with every problem in the result's errors, and change nothing when the spec has one; edittype without -yes only reports
 * changed and fails. removetype -type=T [-yes] removes an ordinary type (definition, generated Server Code folder, Game Kit settings) and renametype
 * -type=T -to=/Game/Path/NewAssetName [-yes] moves its definition asset, not its Type Name; without -yes both only say what they would do and fail.
 * generate, newtype, edittype -yes, addstarter, removestarter -yes, removetype -yes and renametype -yes refuse while an Unreal Editor is running, which
 * would save its own copy of the types over what they write; -editoropen writes anyway. -out writes the result as one JSON object. Exit code 0 on success, 1 otherwise.
 * batch -ops=<file> runs the operations in a file, one per line, in this one editor start. A line holds the arguments of a single
 * run (for example -op=status -out=status.json); blank lines and lines starting with # are skipped. It stops at the first operation
 * that fails unless that line has -batchcontinue, and exits 1 if any failed. A line cannot itself be -op=batch.
 */
UCLASS()
class UCrowdyServerComputeCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UCrowdyServerComputeCommandlet();
	virtual int32 Main(const FString& Params) override;

	/** The -type= of enable or disable into OutType. Enable may leave it empty, which switches the whole app on; disable needs one, and fails with OutError without it. */
	static bool ReadSwitchType(const FString& Params, bool bEnabled, FString& OutType, FString& OutError);

	/** What whoami finds missing: the sign-in, the app, both or neither (empty). */
	static FString FindSignInProblem(bool bSignedIn, int64 AppId);

	/** What whoami calls a sign-in: scope 1 is the developer's own session and any other scope an organization token; empty when not signed in. */
	static const TCHAR* GetSignInScopeName(bool bSignedIn, int32 Scope);

	/** Whether an Unreal Editor (not this commandlet) is running on this machine. */
	static bool IsEditorRunning();

	/** The refusal for an op that writes the project while an editor is running, which would save its own copy over it; empty when -editoropen is passed or none is running. */
	static FString FindEditorOpenProblem(const FString& Params, TFunctionRef<bool()> EditorIsRunning);

	/** What removetype and renametype refuse about a definition in package Package that is not under /Game/ (Action is "removed" or "moved"); empty when it is. */
	static FString FindNotInGameProblem(const FString& Package, const TCHAR* Action);

	/** What removetype refuses before it asks for -yes: a Game Kit starter (removestarter takes those with the starters that need them) or a definition that Referencers, other packages, still use; empty when nothing stops it. UsedRedirectors, the redirectors among Referencers that something else still uses, add that they must be fixed up first. */
	static FString FindRemoveTypeProblem(const UCrowdyServerObjectDefinition& Definition, TConstArrayView<FName> Referencers, TConstArrayView<FName> UsedRedirectors = TConstArrayView<FName>());

	/** What removetype answers when the definition's package is still on disk (Exists says whether a package is) after the editor deleted it, so nothing else is removed; empty when it is gone. */
	static FString FindNotDeletedProblem(const FString& Package, TFunctionRef<bool(const FString&)> Exists);

	/** What removetype without -yes says it would remove. ShownFolder is empty when the type has no Server Code folder; FolderStays is why the folder is left as it is, empty when it is deleted. */
	static FString DescribeRemoveType(const FString& TypeName, const FString& AssetPackage, const FString& ShownFolder, const FString& FolderStays);

	/** What renametype refuses about moving the definition in package CurrentPackage to the package To: not under /Game/, not a valid package name, already there, or taken (IsTaken says whether a package exists); empty when nothing stops it. */
	static FString FindRenameProblem(const FString& To, const FString& CurrentPackage, TFunctionRef<bool(const FString&)> IsTaken);

	/** What renametype without -yes says it would move. */
	static FString DescribeRenameType(const FString& TypeName, const FString& From, const FString& To);

	/** The -level= of logs as the Logs tab's filter (all, info, warnings, errors) into OutMaxLevel, -1 when absent or all; fails with OutError for any other name. */
	static bool ReadLogLevel(const FString& Params, int32& OutMaxLevel, FString& OutError);

	/** The -window= of stats (hour, day, week) as minutes into OutMinutes, 0 when absent, which the platform answers with the last hour; fails with OutError for any other name. */
	static bool ReadStatsWindow(const FString& Params, int32& OutMinutes, FString& OutError);

	/** The text an unknown operation is answered with: every operation Main accepts, with its switches. */
	static const TCHAR* GetUsage();

	/** Every -op= value Main accepts. */
	static TArray<FString> GetOperationNames();

	/** Splits a batch file into one argument list per operation, skipping blank and # lines; fails, naming the line, on a nested batch or when nothing is left. */
	static bool ParseBatchLines(const FString& Text, TArray<FString>& OutLines, FString& OutError);

	/** Reads a JSON switch such as -inputs=: the JSON itself, or @<file> / @"<file with spaces>" holding it. */
	static bool ReadJsonSwitch(const FString& Params, const TCHAR* Switch, FString& OutJson, FString& OutError);
};
