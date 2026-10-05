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
 */
UCLASS()
class UCrowdyServerComputeCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UCrowdyServerComputeCommandlet();
	virtual int32 Main(const FString& Params) override;
};
