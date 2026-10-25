#include "CrowdyStudioCommandlet.h"

#include "Async/TaskGraphInterfaces.h"
#include "ConfigSync/FCrowdyConfigSync.h"
#include "Containers/Ticker.h"
#include "CoreGlobals.h"
#include "CrowdyStudioModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Gql/CrowdyStudioQueries.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Model/FCrowdyStudioController.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace CrowdyStudioCommandletDetail
{
	const TCHAR* const Usage = TEXT("-run=CrowdyStudio -op=apps (the account's organizations and their apps, the datacenters an app can be placed in, and the project's Network settings)")
		TEXT(" | createapp -org=<id> -name=<text> -slug=<text> -datacenter=<code from apps> [-description=<text>] -yes (the datacenter is permanent)")
		TEXT(" | sync -app=<id> -yes [-editoropen] (writes the app's id, organization and game endpoints into Config/DefaultGame.ini; refused while an Unreal Editor is running unless -editoropen)")
		TEXT(", and -out=<file> for a JSON result. Quote a value with spaces or commas. createapp and sync change nothing without -yes.");

	const TCHAR* const SignInMessage = TEXT("Sign in to Crowdy Studio on this machine first");

	constexpr double ReadTimeoutSeconds = 60.0;
	constexpr double CreateTimeoutSeconds = 180.0;

	struct FRun
	{
		FCrowdyStudioCommandRequest Request;
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		TSharedPtr<FCrowdyStudioController> Controller;
		FString Error;
		/** The controller's latest error status since the last reset. */
		FString StatusError;
		int64 CreatedAppId = 0;
		bool bCreateAnswered = false;
	};

	void Step(const FString& Line)
	{
		UE_LOG(LogCrowdyStudio, Display, TEXT("Crowdy Studio: %s"), *Line);
	}

	FString IdText(int64 Id)
	{
		return FString::Printf(TEXT("%lld"), Id);
	}

	TArray<TSharedPtr<FJsonValue>> StringValues(const TArray<FString>& Items)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& Item : Items)
		{
			Values.Add(MakeShared<FJsonValueString>(Item));
		}
		return Values;
	}

	void SetSettings(FJsonObject& Out, const FStudioSettingsSnapshot& Settings)
	{
		Out.SetStringField(TEXT("appId"), IdText(Settings.AppId));
		Out.SetStringField(TEXT("orgId"), IdText(Settings.OrgId));
		Out.SetStringField(TEXT("discoveryUrl"), Settings.DiscoveryUrl);
		Out.SetStringField(TEXT("gameApiHttpUrl"), Settings.GameApiHttpUrl);
		Out.SetStringField(TEXT("gameApiWsUrl"), Settings.GameApiWsUrl);
	}

	FString SettingsLine(const FStudioSettingsSnapshot& Settings)
	{
		return FString::Printf(TEXT("AppID=%lld OrgId=%lld DiscoveryUrl=%s GameApiHttpUrl=%s GameApiWsUrl=%s"), Settings.AppId, Settings.OrgId,
			*Settings.DiscoveryUrl, *Settings.GameApiHttpUrl, *Settings.GameApiWsUrl);
	}

	bool ReadSwitch(const FString& Params, const TCHAR* Name, FString& OutValue)
	{
		OutValue.Reset();
		FParse::Value(*Params, *FString::Printf(TEXT("-%s="), Name), OutValue);
		OutValue.TrimStartAndEndInline();
		return !OutValue.IsEmpty();
	}

	bool IsDigits(const FString& Text)
	{
		for (const TCHAR C : Text)
		{
			if (!FChar::IsDigit(C))
			{
				return false;
			}
		}
		return true;
	}

	bool ReadId(const FString& Params, const TCHAR* Name, int64& OutId, FString& OutError)
	{
		FString Text;
		if (!ReadSwitch(Params, Name, Text))
		{
			OutError = FString::Printf(TEXT("-%s=<id> is required, the number shown by -op=apps"), Name);
			return false;
		}
		if (Text.Len() > 18 || !IsDigits(Text) || FCString::Atoi64(*Text) <= 0)
		{
			OutError = FString::Printf(TEXT("-%s=%s is not an id; it is the positive number shown by -op=apps"), Name, *Text);
			return false;
		}
		OutId = FCString::Atoi64(*Text);
		return true;
	}

	bool ParseCreateApp(const FString& Params, FCrowdyStudioCommandRequest& Out, FString& OutError)
	{
		if (!ReadId(Params, TEXT("org"), Out.OrgId, OutError))
		{
			return false;
		}
		if (!ReadSwitch(Params, TEXT("name"), Out.Name))
		{
			OutError = TEXT("-name=<text> is required, the app's display name");
			return false;
		}
		if (!ReadSwitch(Params, TEXT("slug"), Out.Slug))
		{
			OutError = TEXT("-slug=<text> is required, the app's URL identifier");
			return false;
		}
		if (!Out.Slug.Equals(CrowdyStudioGql::SlugFromName(Out.Slug), ESearchCase::CaseSensitive))
		{
			OutError = TEXT("-slug can only use lowercase letters, numbers, and single hyphens");
			return false;
		}
		if (!ReadSwitch(Params, TEXT("datacenter"), Out.Datacenter))
		{
			OutError = TEXT("-datacenter=<code> is required, one of the placeable codes shown by -op=apps");
			return false;
		}
		ReadSwitch(Params, TEXT("description"), Out.Description);
		return true;
	}

	/** A commandlet has no engine loop, so the ticker (which also ticks the HTTP module) is driven here until the condition holds or the time runs out. */
	bool Pump(TFunctionRef<bool()> IsDone, double TimeoutSeconds)
	{
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		double Last = FPlatformTime::Seconds();
		while (!IsDone() && !IsEngineExitRequested())
		{
			const double Now = FPlatformTime::Seconds();
			if (Now > Deadline)
			{
				return false;
			}
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FTSTicker::GetCoreTicker().Tick(static_cast<float>(Now - Last));
			Last = Now;
			FPlatformProcess::Sleep(0.01f);
		}
		return IsDone();
	}

	/** Waits for every request the controller has in flight; any error status raised meanwhile fails the step. */
	bool Settle(FRun& Run)
	{
		if (!Pump([&Run]() { return !Run.Controller->IsBusy(); }, ReadTimeoutSeconds))
		{
			Run.Error = TEXT("Crowdy did not answer in time");
			return false;
		}
		if (!Run.StatusError.IsEmpty())
		{
			Run.Error = Run.StatusError;
			return false;
		}
		return true;
	}

	bool SignIn(FRun& Run)
	{
		int32 Scope = 0;
		int64 UserId = 0;
		if (!CrowdyStudioAuth::GetSignedInIdentity(Scope, UserId))
		{
			Run.Error = SignInMessage;
			return false;
		}
		const TCHAR* ScopeName = Scope == static_cast<int32>(ECrowdyStudioAuthScope::Session) ? TEXT("session") : TEXT("organizationToken");
		const TSharedRef<FJsonObject> Identity = MakeShared<FJsonObject>();
		Identity->SetStringField(TEXT("scope"), ScopeName);
		Identity->SetStringField(TEXT("userId"), IdText(UserId));
		Run.Result->SetObjectField(TEXT("signedIn"), Identity);

		Run.Controller = MakeShared<FCrowdyStudioController>();
		FString* StatusError = &Run.StatusError;
		Run.Controller->OnStatusMessage.AddLambda([StatusError](const FString& Message, bool bIsError)
		{
			if (bIsError)
			{
				*StatusError = Message;
			}
		});
		if (!Run.Controller->RestoreSavedSignIn())
		{
			Run.Error = SignInMessage;
			return false;
		}
		if (!Settle(Run))
		{
			return false;
		}
		if (!Run.Controller->IsSignedIn())
		{
			Run.Error = FString::Printf(TEXT("The stored sign-in was not accepted. %s"), SignInMessage);
			return false;
		}
		Step(FString::Printf(TEXT("signed in (%s, user %lld) with %d organization(s) and %d app(s)"), ScopeName, UserId,
			Run.Controller->GetOrganizations().Num(), Run.Controller->GetApps().Num()));
		return true;
	}

	bool LoadDatacenters(FRun& Run)
	{
		Run.StatusError.Reset();
		Run.Controller->FetchPlaceableDatacenters();
		if (!Settle(Run))
		{
			return false;
		}
		if (!Run.Controller->HasFetchedPlaceableDatacenters())
		{
			Run.Error = TEXT("Could not read the datacenters this deployment can place an app in");
			return false;
		}
		return true;
	}

	bool RunApps(FRun& Run)
	{
		if (!SignIn(Run) || !LoadDatacenters(Run))
		{
			return false;
		}
		// The list answer carries no datacenter; one discovery read for every app fills it in.
		Run.StatusError.Reset();
		TArray<int64> AppIds;
		for (const TSharedPtr<FStudioApp>& App : Run.Controller->GetApps())
		{
			AppIds.Add(App->AppId);
		}
		Run.Controller->FetchAppDiscovery(AppIds);
		if (!Settle(Run))
		{
			return false;
		}

		const FStudioSettingsSnapshot Current = FCrowdyConfigSync::ReadCurrentSettings();
		const FString Environment = FCrowdyConfigSync::GetBackendMode();
		Run.Result->SetStringField(TEXT("environment"), Environment);
		SetSettings(*Run.Result, Current);
		Step(FString::Printf(TEXT("project: Environment=%s %s"), *Environment, *SettingsLine(Current)));

		TArray<TSharedPtr<FJsonValue>> DatacenterRows;
		for (const FStudioDatacenter& Dc : Run.Controller->GetPlaceableDatacenters())
		{
			const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("code"), Dc.Code);
			Row->SetBoolField(TEXT("placeable"), Dc.bPlaceable);
			Row->SetStringField(TEXT("serving"), Dc.Serving);
			Row->SetStringField(TEXT("gameApiUrl"), Dc.GameApiUrl);
			DatacenterRows.Add(MakeShared<FJsonValueObject>(Row));
			Step(FString::Printf(TEXT("datacenter %s: %s, %s"), *Dc.Code, Dc.bPlaceable ? TEXT("placeable") : TEXT("not placeable"), *Dc.Serving));
		}
		Run.Result->SetArrayField(TEXT("datacenters"), DatacenterRows);

		TArray<TSharedPtr<FJsonValue>> OrgRows;
		for (const TSharedPtr<FStudioOrg>& Org : Run.Controller->GetOrganizations())
		{
			const TSharedRef<FJsonObject> OrgRow = MakeShared<FJsonObject>();
			OrgRow->SetStringField(TEXT("orgId"), IdText(Org->OrgId));
			OrgRow->SetStringField(TEXT("name"), Org->Name);
			OrgRow->SetStringField(TEXT("slug"), Org->Slug);
			OrgRow->SetArrayField(TEXT("permissions"), StringValues(Org->Permissions));
			Step(FString::Printf(TEXT("organization %lld \"%s\" (%s)"), Org->OrgId, *Org->Name, *Org->Slug));

			TArray<TSharedPtr<FJsonValue>> AppRows;
			for (const TSharedPtr<FStudioApp>& App : Run.Controller->GetApps())
			{
				if (App->OrgId != Org->OrgId)
				{
					continue;
				}
				const bool bArchived = App->Status == TEXT("ARCHIVED");
				const TSharedRef<FJsonObject> AppRow = MakeShared<FJsonObject>();
				AppRow->SetStringField(TEXT("appId"), IdText(App->AppId));
				AppRow->SetStringField(TEXT("name"), App->Name);
				AppRow->SetStringField(TEXT("slug"), App->Slug);
				AppRow->SetStringField(TEXT("datacenter"), App->DatacenterCode);
				AppRow->SetStringField(TEXT("status"), App->Status);
				AppRow->SetBoolField(TEXT("archived"), bArchived);
				AppRows.Add(MakeShared<FJsonValueObject>(AppRow));
				Step(FString::Printf(TEXT("  app %lld \"%s\" (%s) datacenter=%s %s%s"), App->AppId, *App->Name, *App->Slug, *App->DatacenterCode,
					*App->Status, bArchived ? TEXT(", archived") : TEXT("")));
			}
			OrgRow->SetArrayField(TEXT("apps"), AppRows);
			OrgRows.Add(MakeShared<FJsonValueObject>(OrgRow));
		}
		Run.Result->SetArrayField(TEXT("organizations"), OrgRows);
		return true;
	}

	bool RunCreateApp(FRun& Run)
	{
		const FCrowdyStudioCommandRequest& Request = Run.Request;
		if (!SignIn(Run) || !LoadDatacenters(Run))
		{
			return false;
		}
		const FString Problem = UCrowdyStudioCommandlet::FindCreateProblem(Request, Run.Controller->GetOrganizations(), Run.Controller->GetPlaceableDatacenters());
		if (!Problem.IsEmpty())
		{
			Run.Error = Problem;
			return false;
		}

		Run.Result->SetStringField(TEXT("orgId"), IdText(Request.OrgId));
		Run.Result->SetStringField(TEXT("name"), Request.Name);
		Run.Result->SetStringField(TEXT("slug"), Request.Slug);
		Run.Result->SetStringField(TEXT("datacenter"), Request.Datacenter);
		Run.Result->SetStringField(TEXT("description"), Request.Description);
		Run.Result->SetBoolField(TEXT("created"), false);
		const bool bNeedsConfirmation = UCrowdyStudioCommandlet::NeedsConfirmation(Request);
		Step(FString::Printf(TEXT("%s app \"%s\" (%s) in organization %lld, datacenter %s (permanent)"), bNeedsConfirmation ? TEXT("would create") : TEXT("creating"),
			*Request.Name, *Request.Slug, Request.OrgId, *Request.Datacenter));
		if (bNeedsConfirmation)
		{
			Run.Error = TEXT("Creating an app is permanent, its datacenter can never change; pass -yes to confirm.");
			return false;
		}

		Run.StatusError.Reset();
		Run.Controller->CreateApp(Request.OrgId, Request.Name, Request.Slug, Request.Datacenter, Request.Description,
			[&Run](int64 CreatedAppId)
			{
				Run.CreatedAppId = CreatedAppId;
				Run.bCreateAnswered = true;
			});
		if (!Pump([&Run]() { return Run.bCreateAnswered; }, CreateTimeoutSeconds))
		{
			Run.Error = TEXT("No answer in time; the app may still have been created, check -op=apps");
			return false;
		}
		if (Run.CreatedAppId == 0)
		{
			Run.Error = Run.StatusError.IsEmpty() ? FString(TEXT("The app could not be created")) : Run.StatusError;
			return false;
		}
		Run.Result->SetBoolField(TEXT("created"), true);
		Run.Result->SetStringField(TEXT("appId"), IdText(Run.CreatedAppId));
		Step(FString::Printf(TEXT("created app %lld; write it into the project with -op=sync -app=%lld -yes"), Run.CreatedAppId, Run.CreatedAppId));
		return true;
	}

	bool RunSync(FRun& Run)
	{
		const int64 AppId = Run.Request.AppId;
		if (!SignIn(Run))
		{
			return false;
		}
		if (!UCrowdyStudioCommandlet::FindApp(Run.Controller->GetApps(), AppId))
		{
			Run.Error = FString::Printf(TEXT("App %lld is not one of this account's apps; -op=apps lists them"), AppId);
			return false;
		}
		// Selecting an app in the console reads its record before Sync to project writes it; the same read is made here.
		Run.StatusError.Reset();
		Run.Controller->FetchApp(AppId);
		if (!Settle(Run))
		{
			return false;
		}
		const TSharedPtr<FStudioApp> App = UCrowdyStudioCommandlet::FindApp(Run.Controller->GetApps(), AppId);
		if (!App)
		{
			Run.Error = FString::Printf(TEXT("App %lld is no longer one of this account's apps"), AppId);
			return false;
		}

		const FStudioSettingsSnapshot Before = FCrowdyConfigSync::ReadCurrentSettings();
		const FStudioSettingsSnapshot After = UCrowdyStudioCommandlet::ProposeSync(*App);
		const TSharedRef<FJsonObject> BeforeRow = MakeShared<FJsonObject>();
		const TSharedRef<FJsonObject> AfterRow = MakeShared<FJsonObject>();
		SetSettings(*BeforeRow, Before);
		SetSettings(*AfterRow, After);
		Run.Result->SetObjectField(TEXT("before"), BeforeRow);
		Run.Result->SetObjectField(TEXT("after"), AfterRow);
		Run.Result->SetBoolField(TEXT("changed"), false);
		Run.Result->SetStringField(TEXT("environment"), FCrowdyConfigSync::GetBackendMode());
		const bool bChanged = !(Before == After);
		Step(FString::Printf(TEXT("app %lld \"%s\": %s"), AppId, *App->Name, bChanged ? TEXT("the project settings change") : TEXT("the project already holds these values")));
		Step(FString::Printf(TEXT("  was: %s"), *SettingsLine(Before)));
		Step(FString::Printf(TEXT("  now: %s"), *SettingsLine(After)));
		if (App->GameApiUrl.IsEmpty())
		{
			Step(TEXT("  the app reports no game endpoint, so GameApiHttpUrl and GameApiWsUrl stay as they are"));
		}
		if (!bChanged)
		{
			Step(FString::Printf(TEXT("already synced to app %lld; nothing to write"), AppId));
			return true;
		}
		if (UCrowdyStudioCommandlet::NeedsConfirmation(Run.Request))
		{
			Run.Error = TEXT("Sync rewrites AppID, OrgId and the game endpoints in Config/DefaultGame.ini; pass -yes to confirm.");
			return false;
		}
		const FString WriteProblem = UCrowdyStudioCommandlet::FindSyncWriteProblem(Run.Request, FPlatformProcess::IsApplicationRunning(TEXT("UnrealEditor")));
		if (!WriteProblem.IsEmpty())
		{
			Run.Error = WriteProblem;
			return false;
		}

		if (!FCrowdyConfigSync::ApplyAppToSettings(*App))
		{
			Run.Error = FCrowdyConfigSync::WriteFailedMessage;
			return false;
		}
		if (!(FCrowdyConfigSync::ReadCurrentSettings() == After))
		{
			Run.Error = TEXT("The project settings did not take the app's values");
			return false;
		}
		Run.Result->SetBoolField(TEXT("changed"), true);
		Step(TEXT("wrote the app into Config/DefaultGame.ini"));
		return true;
	}

	bool Execute(FRun& Run)
	{
		if (Run.Request.Op == TEXT("apps"))
		{
			return RunApps(Run);
		}
		if (Run.Request.Op == TEXT("createapp"))
		{
			return RunCreateApp(Run);
		}
		return RunSync(Run);
	}

	bool WriteResult(const TSharedRef<FJsonObject>& Result, const FString& OutPath)
	{
		const FString Path = FPaths::ConvertRelativePathToFull(FPaths::LaunchDir(), OutPath);
		FString Text;
		FJsonSerializer::Serialize(Result, TJsonWriterFactory<>::Create(&Text));
		if (!FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			UE_LOG(LogCrowdyStudio, Error, TEXT("Crowdy Studio: could not write %s"), *Path);
			return false;
		}
		Step(FString::Printf(TEXT("wrote %s"), *Path));
		return true;
	}

	bool Finish(FRun& Run, const FString& Params, bool bOk)
	{
		Run.Result->SetStringField(TEXT("op"), Run.Request.Op);
		Run.Result->SetBoolField(TEXT("ok"), bOk);
		Run.Result->SetStringField(TEXT("error"), Run.Error);
		UE_CLOG(bOk, LogCrowdyStudio, Display, TEXT("Crowdy Studio: done"));
		UE_CLOG(!bOk, LogCrowdyStudio, Error, TEXT("Crowdy Studio: %s"), *Run.Error);
		FString OutPath;
		if (!FParse::Value(*Params, TEXT("-out="), OutPath))
		{
			return bOk;
		}
		return WriteResult(Run.Result, OutPath) && bOk;
	}
}

UCrowdyStudioCommandlet::UCrowdyStudioCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
	// Otherwise any error line logged during the run turns a success into exit code 1.
	UseCommandletResultAsExitCode = true;
}

int32 UCrowdyStudioCommandlet::Main(const FString& Params)
{
	using namespace CrowdyStudioCommandletDetail;
	FRun Run;
	if (!ParseRequest(Params, Run.Request, Run.Error))
	{
		return Finish(Run, Params, false) ? 0 : 1;
	}
	return Finish(Run, Params, Execute(Run)) ? 0 : 1;
}

const TCHAR* UCrowdyStudioCommandlet::GetUsage()
{
	return CrowdyStudioCommandletDetail::Usage;
}

bool UCrowdyStudioCommandlet::ParseRequest(const FString& Params, FCrowdyStudioCommandRequest& OutRequest, FString& OutError)
{
	using namespace CrowdyStudioCommandletDetail;
	OutRequest = FCrowdyStudioCommandRequest();
	OutError.Reset();
	ReadSwitch(Params, TEXT("op"), OutRequest.Op);
	OutRequest.Op.ToLowerInline();
	OutRequest.bYes = FParse::Param(*Params, TEXT("yes"));
	OutRequest.bEditorOpen = FParse::Param(*Params, TEXT("editoropen"));
	if (OutRequest.Op == TEXT("apps"))
	{
		return true;
	}
	if (OutRequest.Op == TEXT("sync"))
	{
		return ReadId(Params, TEXT("app"), OutRequest.AppId, OutError);
	}
	if (OutRequest.Op == TEXT("createapp"))
	{
		return ParseCreateApp(Params, OutRequest, OutError);
	}
	OutError = FString::Printf(TEXT("Unknown or missing -op '%s'. Usage: %s"), *OutRequest.Op, Usage);
	return false;
}

bool UCrowdyStudioCommandlet::NeedsConfirmation(const FCrowdyStudioCommandRequest& Request)
{
	return !Request.bYes && (Request.Op == TEXT("createapp") || Request.Op == TEXT("sync"));
}

FString UCrowdyStudioCommandlet::FindCreateProblem(const FCrowdyStudioCommandRequest& Request, const TArray<TSharedPtr<FStudioOrg>>& Organizations,
	const TArray<FStudioDatacenter>& Datacenters)
{
	const bool bKnownOrg = Organizations.ContainsByPredicate([&Request](const TSharedPtr<FStudioOrg>& Org) { return Org.IsValid() && Org->OrgId == Request.OrgId; });
	if (!bKnownOrg)
	{
		return FString::Printf(TEXT("-org=%lld is not one of this account's organizations; -op=apps lists them"), Request.OrgId);
	}
	TArray<FString> Placeable;
	for (const FStudioDatacenter& Dc : Datacenters)
	{
		if (Dc.bPlaceable)
		{
			Placeable.Add(Dc.Code);
		}
	}
	if (Placeable.IsEmpty())
	{
		return TEXT("This deployment has no datacenter that can take a new app right now, so creation will be refused. Ask the server operator.");
	}
	if (!Placeable.ContainsByPredicate([&Request](const FString& Code) { return Code.Equals(Request.Datacenter, ESearchCase::CaseSensitive); }))
	{
		return FString::Printf(TEXT("-datacenter=%s is not a datacenter an app can be placed in; choose one of: %s"), *Request.Datacenter, *FString::Join(Placeable, TEXT(", ")));
	}
	return FString();
}

FString UCrowdyStudioCommandlet::FindSyncWriteProblem(const FCrowdyStudioCommandRequest& Request, bool bEditorRunning)
{
	if (!bEditorRunning || Request.bEditorOpen)
	{
		return FString();
	}
	return TEXT("An Unreal Editor is running. Close it first: it holds this project's settings in memory and would write its own copy over the synced app. Pass -editoropen to write anyway.");
}

TSharedPtr<FStudioApp> UCrowdyStudioCommandlet::FindApp(const TArray<TSharedPtr<FStudioApp>>& Apps, int64 AppId)
{
	for (const TSharedPtr<FStudioApp>& App : Apps)
	{
		if (App.IsValid() && App->AppId == AppId)
		{
			return App;
		}
	}
	return nullptr;
}

FStudioSettingsSnapshot UCrowdyStudioCommandlet::ProposeSync(const FStudioApp& App)
{
	return FCrowdyConfigSync::BuildProposedSettings(App);
}
