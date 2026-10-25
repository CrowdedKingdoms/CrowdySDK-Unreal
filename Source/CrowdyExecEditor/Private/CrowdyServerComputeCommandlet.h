#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "Templates/Function.h"
#include "CrowdyServerComputeCommandlet.generated.h"

/**
 * The Server Code tab's operations from the command line, for scripts and CI, as the developer signed in to Crowdy Studio:
 *   UnrealEditor-Cmd <project> -run=CrowdyServerCompute -op=<operation> [-out=<file>]
 * Operations: status, versions, changes, deploy, starters, activate -version=N, disable -type=T, enable -type=T,
 * logs [-type=T] [-flow=F] [-limit=N], stats [-type=T]. changes compares each type with the live version; deploy records
 * each type's deployed revision beside its server code. deploy, starters, activate, disable and enable change the app's live
 * server code and run only with -yes. generate [-yes] writes the server code of every type that is out of date or has none and saves the
 * definitions it changed (with -type=T, that one type, up to date or not); it exits 1 if any type was refused (a named type that does not bake is), lists in skipped the types it left alone because they need you first, and in saved the definitions it saved, and -yes answers the
 * questions that keep saved data. enable without -type switches the whole app on. whoami says whether Crowdy Studio is signed in on
 * this machine and which app the project uses, without the token. exporttype [-type=T] [-spec=<file>] puts a type's settings, as a JSON
 * spec, in the result's definition and in the file (without -type, a new type's template). newtype -spec=<json or @file> [-path=...]
 * makes a type from a spec, and edittype -type=T -spec=... [-yes] replaces a type's settings with a spec that holds all of them; both
 * refuse, with every problem in the result's errors, and change nothing when the spec has one; edittype without -yes only reports
 * changed and fails. generate, newtype, edittype -yes, addstarter and removestarter -yes refuse while an Unreal Editor is running, which
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

	/** The text an unknown operation is answered with: every operation Main accepts, with its switches. */
	static const TCHAR* GetUsage();

	/** Every -op= value Main accepts. */
	static TArray<FString> GetOperationNames();

	/** Splits a batch file into one argument list per operation, skipping blank and # lines; fails, naming the line, on a nested batch or when nothing is left. */
	static bool ParseBatchLines(const FString& Text, TArray<FString>& OutLines, FString& OutError);

	/** Reads a JSON switch such as -inputs=: the JSON itself, or @<file> / @"<file with spaces>" holding it. */
	static bool ReadJsonSwitch(const FString& Params, const TCHAR* Switch, FString& OutJson, FString& OutError);
};
