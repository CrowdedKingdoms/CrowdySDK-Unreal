#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "ConfigSync/FCrowdyConfigSync.h"
#include "CrowdyStudioCommandlet.h"
#include "Misc/AutomationTest.h"
#include "Model/CrowdyStudioControllerTestAccess.h"
#include "UObject/StrongObjectPtr.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyStudioCommandletTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	const TCHAR* const CompleteCreate = TEXT("-run=CrowdyStudio -op=createapp -org=74637546537472 -name=\"My Game\" -slug=my-game -datacenter=or -description=\"A game, with a comma\"");

	bool Refuses(const FString& Params, const TCHAR* Switch)
	{
		FCrowdyStudioCommandRequest Request;
		FString Error;
		return !UCrowdyStudioCommandlet::ParseRequest(Params, Request, Error) && Error.Contains(Switch);
	}

	FStudioDatacenter Datacenter(const TCHAR* Code, bool bPlaceable)
	{
		FStudioDatacenter Dc;
		Dc.Code = Code;
		Dc.bPlaceable = bPlaceable;
		return Dc;
	}

	TSharedPtr<FStudioOrg> Org(int64 OrgId)
	{
		TSharedPtr<FStudioOrg> Result = MakeShared<FStudioOrg>();
		Result->OrgId = OrgId;
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioCommandletParsesCreateAppTest, "CrowdySDK.CrowdyStudio.Commandlet.ParsesCreateApp", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioCommandletParsesCreateAppTest::RunTest(const FString& /*Parameters*/)
{
	FCrowdyStudioCommandRequest Request;
	FString Error;
	TestTrue(TEXT("a complete createapp parses"), UCrowdyStudioCommandlet::ParseRequest(CompleteCreate, Request, Error));
	TestTrue(TEXT("no error is reported"), Error.IsEmpty());
	TestEqual(TEXT("the operation is createapp"), Request.Op, FString(TEXT("createapp")));
	TestEqual(TEXT("an organization id wider than 32 bits is kept"), Request.OrgId, static_cast<int64>(74637546537472));
	TestEqual(TEXT("a quoted name keeps its space"), Request.Name, FString(TEXT("My Game")));
	TestEqual(TEXT("the slug is read"), Request.Slug, FString(TEXT("my-game")));
	TestEqual(TEXT("the datacenter is read"), Request.Datacenter, FString(TEXT("or")));
	TestEqual(TEXT("a quoted description keeps its comma"), Request.Description, FString(TEXT("A game, with a comma")));

	TestTrue(TEXT("the description is optional"), UCrowdyStudioCommandlet::ParseRequest(TEXT("-op=createapp -org=5 -name=G -slug=g -datacenter=va"), Request, Error));
	TestTrue(TEXT("an absent description is empty"), Request.Description.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioCommandletNamesTheMissingSwitchTest, "CrowdySDK.CrowdyStudio.Commandlet.NamesTheMissingSwitch", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioCommandletNamesTheMissingSwitchTest::RunTest(const FString& /*Parameters*/)
{
	TestTrue(TEXT("createapp without -org names -org"), Refuses(TEXT("-op=createapp -name=G -slug=g -datacenter=or"), TEXT("-org")));
	TestTrue(TEXT("a non-numeric -org names -org"), Refuses(TEXT("-op=createapp -org=acme -name=G -slug=g -datacenter=or"), TEXT("-org")));
	TestTrue(TEXT("a zero -org names -org"), Refuses(TEXT("-op=createapp -org=0 -name=G -slug=g -datacenter=or"), TEXT("-org")));
	TestTrue(TEXT("createapp without -name names -name"), Refuses(TEXT("-op=createapp -org=5 -slug=g -datacenter=or"), TEXT("-name")));
	TestTrue(TEXT("createapp without -slug names -slug"), Refuses(TEXT("-op=createapp -org=5 -name=G -datacenter=or"), TEXT("-slug")));
	TestTrue(TEXT("a slug the Create app sheet would refuse names -slug"), Refuses(TEXT("-op=createapp -org=5 -name=G -slug=Bad_Slug -datacenter=or"), TEXT("-slug")));
	TestTrue(TEXT("createapp without -datacenter names -datacenter"), Refuses(TEXT("-op=createapp -org=5 -name=G -slug=g"), TEXT("-datacenter")));
	TestTrue(TEXT("sync without -app names -app"), Refuses(TEXT("-op=sync -yes"), TEXT("-app")));
	TestTrue(TEXT("a non-numeric -app names -app"), Refuses(TEXT("-op=sync -app=latest -yes"), TEXT("-app")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioCommandletRequiresYesTest, "CrowdySDK.CrowdyStudio.Commandlet.RequiresYesToChange", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioCommandletRequiresYesTest::RunTest(const FString& /*Parameters*/)
{
	FCrowdyStudioCommandRequest Request;
	FString Error;

	UCrowdyStudioCommandlet::ParseRequest(CompleteCreate, Request, Error);
	TestTrue(TEXT("createapp without -yes needs confirmation"), UCrowdyStudioCommandlet::NeedsConfirmation(Request));
	UCrowdyStudioCommandlet::ParseRequest(FString(CompleteCreate) + TEXT(" -yes"), Request, Error);
	TestFalse(TEXT("createapp with -yes does not"), UCrowdyStudioCommandlet::NeedsConfirmation(Request));

	UCrowdyStudioCommandlet::ParseRequest(TEXT("-op=sync -app=77"), Request, Error);
	TestEqual(TEXT("the app id is read"), Request.AppId, static_cast<int64>(77));
	TestTrue(TEXT("sync without -yes needs confirmation"), UCrowdyStudioCommandlet::NeedsConfirmation(Request));
	UCrowdyStudioCommandlet::ParseRequest(TEXT("-op=sync -app=77 -yes"), Request, Error);
	TestFalse(TEXT("sync with -yes does not"), UCrowdyStudioCommandlet::NeedsConfirmation(Request));

	UCrowdyStudioCommandlet::ParseRequest(TEXT("-op=apps"), Request, Error);
	TestFalse(TEXT("apps changes nothing, so it never needs -yes"), UCrowdyStudioCommandlet::NeedsConfirmation(Request));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioCommandletUnknownOpPrintsUsageTest, "CrowdySDK.CrowdyStudio.Commandlet.UnknownOpPrintsUsage", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioCommandletUnknownOpPrintsUsageTest::RunTest(const FString& /*Parameters*/)
{
	FCrowdyStudioCommandRequest Request;
	FString Error;
	TestFalse(TEXT("an unknown operation is refused"), UCrowdyStudioCommandlet::ParseRequest(TEXT("-run=CrowdyStudio -op=archive -app=5"), Request, Error));
	TestTrue(TEXT("the refusal names the operation"), Error.Contains(TEXT("archive")));
	TestTrue(TEXT("the refusal carries the usage"), Error.Contains(UCrowdyStudioCommandlet::GetUsage()));

	Error.Reset();
	TestFalse(TEXT("a missing operation is refused"), UCrowdyStudioCommandlet::ParseRequest(TEXT("-run=CrowdyStudio"), Request, Error));
	TestTrue(TEXT("the missing-operation refusal carries the usage"), Error.Contains(UCrowdyStudioCommandlet::GetUsage()));

	const FString Usage = UCrowdyStudioCommandlet::GetUsage();
	for (const TCHAR* Needle : { TEXT("-op=apps"), TEXT("createapp"), TEXT("-org="), TEXT("-name="), TEXT("-slug="), TEXT("-datacenter="), TEXT("-description="), TEXT("sync"), TEXT("-app="), TEXT("-yes"), TEXT("-editoropen"), TEXT("-out=") })
	{
		TestTrue(FString::Printf(TEXT("the usage names %s"), Needle), Usage.Contains(Needle));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioCommandletRefusesLikeTheCreateSheetTest, "CrowdySDK.CrowdyStudio.Commandlet.RefusesLikeTheCreateSheet", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioCommandletRefusesLikeTheCreateSheetTest::RunTest(const FString& /*Parameters*/)
{
	FCrowdyStudioCommandRequest Request;
	FString Error;
	UCrowdyStudioCommandlet::ParseRequest(TEXT("-op=createapp -org=5 -name=G -slug=g -datacenter=va"), Request, Error);
	const TArray<TSharedPtr<FStudioOrg>> Orgs = { Org(5) };
	const TArray<FStudioDatacenter> Datacenters = { Datacenter(TEXT("or"), true), Datacenter(TEXT("va"), true), Datacenter(TEXT("eu"), false) };

	TestTrue(TEXT("a placeable datacenter in the account's organization is accepted"), UCrowdyStudioCommandlet::FindCreateProblem(Request, Orgs, Datacenters).IsEmpty());

	TestTrue(TEXT("an organization the account is not in is refused, naming -org"),
		UCrowdyStudioCommandlet::FindCreateProblem(Request, { Org(6) }, Datacenters).Contains(TEXT("-org")));

	Request.Datacenter = TEXT("eu");
	TestTrue(TEXT("a datacenter that cannot take an app is refused, naming -datacenter and the choices"),
		UCrowdyStudioCommandlet::FindCreateProblem(Request, Orgs, Datacenters).Contains(TEXT("-datacenter=eu")));
	TestTrue(TEXT("the refusal lists the placeable codes"), UCrowdyStudioCommandlet::FindCreateProblem(Request, Orgs, Datacenters).Contains(TEXT("or, va")));

	Request.Datacenter = TEXT("VA");
	TestFalse(TEXT("a code in the wrong case is refused, as it is sent verbatim"), UCrowdyStudioCommandlet::FindCreateProblem(Request, Orgs, Datacenters).IsEmpty());

	TestFalse(TEXT("a deployment with nothing placeable refuses"), UCrowdyStudioCommandlet::FindCreateProblem(Request, Orgs, { Datacenter(TEXT("eu"), false) }).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioCommandletSyncMatchesProjectPageTest, "CrowdySDK.CrowdyStudio.Commandlet.SyncMatchesProjectPage", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioCommandletSyncMatchesProjectPageTest::RunTest(const FString& /*Parameters*/)
{
	FStudioApp WithEndpoint;
	WithEndpoint.AppId = 74637611160064;
	WithEndpoint.OrgId = 74637546537472;
	WithEndpoint.GameApiUrl = TEXT("https://game.example.test");

	FStudioApp WithoutEndpoint;
	WithoutEndpoint.AppId = 74637611160065;
	WithoutEndpoint.OrgId = 74637546537473;

	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::AddApp(*Controller, WithEndpoint);
	FCrowdyStudioControllerTestAccess::AddApp(*Controller, WithoutEndpoint);

	for (const FStudioApp* Case : { &WithEndpoint, &WithoutEndpoint })
	{
		// The Sync to project button writes the selected app onto the developer settings; a transient object takes the same
		// write here without saving anything.
		FCrowdyStudioControllerTestAccess::SetSelectedApp(*Controller, Case->AppId);
		const TSharedPtr<FStudioApp> Selected = Controller->GetSelectedApp();
		if (!TestTrue(TEXT("the controller selects the app"), Selected.IsValid()))
		{
			continue;
		}
		const TStrongObjectPtr<UCrowdySDKDeveloperSettings> Written(NewObject<UCrowdySDKDeveloperSettings>(GetTransientPackage()));
		FCrowdyConfigSync::ApplyAppToObject(*Written, *Selected);
		const FStudioSettingsSnapshot ButtonWrites = FCrowdyConfigSync::ReadSettings(*Written);

		const TSharedPtr<FStudioApp> Found = UCrowdyStudioCommandlet::FindApp(Controller->GetApps(), Case->AppId);
		if (!TestTrue(TEXT("the commandlet finds the app"), Found.IsValid()))
		{
			continue;
		}
		const FStudioSettingsSnapshot Proposed = UCrowdyStudioCommandlet::ProposeSync(*Found);
		TestTrue(TEXT("the commandlet proposes what Sync to project writes"), Proposed == ButtonWrites);
		TestTrue(TEXT("and what the Project page shows"), Proposed == Controller->BuildProposedSettings());
		TestEqual(TEXT("the app id is the app's"), Proposed.AppId, Case->AppId);
		TestEqual(TEXT("the organization id is the app's"), Proposed.OrgId, Case->OrgId);
		TestEqual(TEXT("the shared origin is left to the backend selector"), Proposed.DiscoveryUrl, Controller->GetCurrentSettings().DiscoveryUrl);
	}

	const FStudioSettingsSnapshot Proposed = UCrowdyStudioCommandlet::ProposeSync(WithEndpoint);
	TestEqual(TEXT("the HTTP endpoint is the app's with the Game API path"), Proposed.GameApiHttpUrl, FString(TEXT("https://game.example.test/graphql")));
	TestEqual(TEXT("the WS endpoint is the same URL with a ws scheme"), Proposed.GameApiWsUrl, FString(TEXT("wss://game.example.test/graphql")));

	const FStudioSettingsSnapshot Kept = UCrowdyStudioCommandlet::ProposeSync(WithoutEndpoint);
	TestEqual(TEXT("an app with no game endpoint leaves the HTTP endpoint as it is"), Kept.GameApiHttpUrl, Controller->GetCurrentSettings().GameApiHttpUrl);
	TestEqual(TEXT("and the WS endpoint"), Kept.GameApiWsUrl, Controller->GetCurrentSettings().GameApiWsUrl);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioCommandletRefusesSyncWhileEditorRunsTest, "CrowdySDK.CrowdyStudio.Commandlet.RefusesSyncWhileEditorRuns", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioCommandletRefusesSyncWhileEditorRunsTest::RunTest(const FString& /*Parameters*/)
{
	FCrowdyStudioCommandRequest Request;
	FString Error;
	UCrowdyStudioCommandlet::ParseRequest(TEXT("-op=sync -app=77 -yes"), Request, Error);
	TestFalse(TEXT("an editor is not assumed open"), Request.bEditorOpen);

	const FString Refusal = UCrowdyStudioCommandlet::FindSyncWriteProblem(Request, /*bEditorRunning*/ true);
	TestFalse(TEXT("a running editor refuses the write"), Refusal.IsEmpty());
	TestTrue(TEXT("the refusal names the way past it"), Refusal.Contains(TEXT("-editoropen")));
	TestTrue(TEXT("no running editor, no refusal"), UCrowdyStudioCommandlet::FindSyncWriteProblem(Request, /*bEditorRunning*/ false).IsEmpty());

	UCrowdyStudioCommandlet::ParseRequest(TEXT("-op=sync -app=77 -yes -editoropen"), Request, Error);
	TestTrue(TEXT("-editoropen is read"), Request.bEditorOpen);
	TestTrue(TEXT("-editoropen writes anyway"), UCrowdyStudioCommandlet::FindSyncWriteProblem(Request, /*bEditorRunning*/ true).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioControllerClassifiesCredentialRejectionTest, "CrowdySDK.CrowdyStudio.Controller.ClassifiesCredentialRejection", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioControllerClassifiesCredentialRejectionTest::RunTest(const FString& /*Parameters*/)
{
	using Access = FCrowdyStudioControllerTestAccess;
	TestTrue(TEXT("HTTP 401 is a rejected token"), Access::IsCredentialRejection(TEXT("Server returned HTTP 401"), FString()));
	TestTrue(TEXT("HTTP 403 is a rejected token"), Access::IsCredentialRejection(TEXT("Server returned HTTP 403"), FString()));
	TestTrue(TEXT("the UNAUTHENTICATED code is a rejected token"), Access::IsCredentialRejection(TEXT("You must sign in"), TEXT("UNAUTHENTICATED")));

	TestFalse(TEXT("HTTP 500 says nothing about the token"), Access::IsCredentialRejection(TEXT("Server returned HTTP 500"), FString()));
	TestFalse(TEXT("HTTP 503 says nothing about the token"), Access::IsCredentialRejection(TEXT("Server returned HTTP 503"), FString()));
	TestFalse(TEXT("a network error says nothing about the token"), Access::IsCredentialRejection(TEXT("Could not connect to the server"), FString()));
	TestFalse(TEXT("a timeout says nothing about the token"), Access::IsCredentialRejection(TEXT("The request timed out"), FString()));
	TestFalse(TEXT("another server code says nothing about the token"), Access::IsCredentialRejection(TEXT("Something broke"), TEXT("INTERNAL_ERROR")));
	TestFalse(TEXT("a cancellation says nothing about the token"), Access::IsCredentialRejection(FCrowdyCppClient::CanceledErrorMessage(), FString()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioCommandletFindsAnAppTest, "CrowdySDK.CrowdyStudio.Commandlet.FindsAnApp", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioCommandletFindsAnAppTest::RunTest(const FString& /*Parameters*/)
{
	TArray<TSharedPtr<FStudioApp>> Apps;
	Apps.Add(nullptr);
	Apps.Add(MakeShared<FStudioApp>());
	Apps.Last()->AppId = 74637611160064;

	TestTrue(TEXT("an app is found by its full-width id"), UCrowdyStudioCommandlet::FindApp(Apps, 74637611160064) == Apps.Last());
	TestFalse(TEXT("another id is not found"), UCrowdyStudioCommandlet::FindApp(Apps, 74637611160065).IsValid());
	return true;
}

#endif
