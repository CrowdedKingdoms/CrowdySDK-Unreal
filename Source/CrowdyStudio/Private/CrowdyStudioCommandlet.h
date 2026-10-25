#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "Model/CrowdyStudioTypes.h"
#include "CrowdyStudioCommandlet.generated.h"

/** One parsed command line of the Crowdy Studio commandlet. */
struct FCrowdyStudioCommandRequest
{
	/** apps, createapp or sync, lower case. */
	FString Op;
	int64 OrgId = 0;
	int64 AppId = 0;
	FString Name;
	FString Slug;
	FString Datacenter;
	FString Description;
	bool bYes = false;
	/** -editoropen: write the project settings even while an Unreal Editor is running. */
	bool bEditorOpen = false;
};

/**
 * The Project page's app choice from the command line, as the developer signed in to Crowdy Studio on this machine:
 *   UnrealEditor-Cmd <project> -run=CrowdyStudio -op=<operation> [-out=<file>]
 * apps lists the account's organizations and their apps, the datacenters an app can be placed in, and the project's current
 * Network settings. createapp -org=<id> -name=<text> -slug=<text> -datacenter=<code> [-description=<text>] -yes creates an app
 * as the Create app sheet does. sync -app=<id> -yes writes that app into Config/DefaultGame.ini as Sync to project does.
 * createapp and sync describe what they would do and change nothing without -yes, and exit 1; a sync that would change nothing
 * succeeds without it. A confirmed sync is refused while an Unreal Editor is running unless -editoropen is given. -out writes
 * the result as one JSON object. Exit code 0 on success, 1 otherwise.
 */
UCLASS()
class UCrowdyStudioCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UCrowdyStudioCommandlet();
	virtual int32 Main(const FString& Params) override;

	/** The text an unknown operation is answered with: every operation Main accepts, with its switches. */
	static const TCHAR* GetUsage();

	/** Reads the operation and its switches; on a refusal, OutError names the switch. A missing -yes is not refused here, see NeedsConfirmation. */
	static bool ParseRequest(const FString& Params, FCrowdyStudioCommandRequest& OutRequest, FString& OutError);

	/** True for an operation that changes the app list or the project and was not given -yes. */
	static bool NeedsConfirmation(const FCrowdyStudioCommandRequest& Request);

	/** The reason the Create app sheet would refuse this request for this account, or empty when it would not. */
	static FString FindCreateProblem(const FCrowdyStudioCommandRequest& Request, const TArray<TSharedPtr<FStudioOrg>>& Organizations,
		const TArray<FStudioDatacenter>& Datacenters);

	/** The reason a confirmed sync must not write Config/DefaultGame.ini now, or empty when it may. */
	static FString FindSyncWriteProblem(const FCrowdyStudioCommandRequest& Request, bool bEditorRunning);

	static TSharedPtr<FStudioApp> FindApp(const TArray<TSharedPtr<FStudioApp>>& Apps, int64 AppId);

	/** What Sync to project leaves the settings as for this app, without writing anything. */
	static FStudioSettingsSnapshot ProposeSync(const FStudioApp& App);
};
