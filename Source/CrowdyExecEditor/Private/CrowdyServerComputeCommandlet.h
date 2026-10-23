#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "CrowdyServerComputeCommandlet.generated.h"

/**
 * The Server Code tab's operations from the command line, for scripts and CI, as the developer signed in to Crowdy Studio:
 *   UnrealEditor-Cmd <project> -run=CrowdyServerCompute -op=<operation> [-out=<file>]
 * Operations: status, versions, changes, deploy, starters, activate -version=N, disable -type=T, enable -type=T,
 * logs [-type=T] [-flow=F] [-limit=N], stats [-type=T]. changes compares each type with the live version; deploy records
 * each type's deployed revision beside its server code. deploy, starters, activate, disable and enable change the app's live
 * server code and run only with -yes. -out writes the result as one JSON object. Exit code 0 on success, 1 otherwise.
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

	/** Splits a batch file into one argument list per operation, skipping blank and # lines; fails, naming the line, on a nested batch or when nothing is left. */
	static bool ParseBatchLines(const FString& Text, TArray<FString>& OutLines, FString& OutError);
};
