// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Gql/CrowdyStudioQueries.h"
#include "UI/CrowdyAppListFilter.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyProjectPageTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	TSharedPtr<FStudioApp> MakeApp(int64 AppId, int64 OrgId, const TCHAR* Name, const TCHAR* Slug, const TCHAR* Status)
	{
		TSharedPtr<FStudioApp> App = MakeShared<FStudioApp>();
		App->AppId = AppId;
		App->OrgId = OrgId;
		App->Name = Name;
		App->Slug = Slug;
		App->Status = Status;
		return App;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyProjectPageOrgFilterNarrowsAppsTest,
	"CrowdySDK.CrowdyStudio.ProjectPage.OrgFilterNarrowsApps", CrowdyProjectPageTestFlags)

bool FCrowdyProjectPageOrgFilterNarrowsAppsTest::RunTest(const FString& /*Parameters*/)
{
	// The bug this pins: the org selector changed a stored id and nothing else, so the list kept showing every
	// app the account could see. The rail now shows only the apps of the chosen org, and every org when none is
	// chosen.
	TArray<TSharedPtr<FStudioApp>> Apps;
	Apps.Add(MakeApp(1, 10, TEXT("Crowded Kingdoms"), TEXT("crowded-kingdoms"), TEXT("LIVE")));
	Apps.Add(MakeApp(2, 10, TEXT("Sandbox"), TEXT("sandbox"), TEXT("DRAFT")));
	Apps.Add(MakeApp(3, 20, TEXT("Other Studio Game"), TEXT("osg"), TEXT("LIVE")));
	Apps.Add(MakeApp(4, 20, TEXT("Retired"), TEXT("retired"), TEXT("ARCHIVED")));

	TArray<TSharedPtr<FStudioApp>> Visible;
	FCrowdyAppListFilter Filter;

	CrowdyFilterApps(Apps, Filter, Visible);
	TestEqual(TEXT("No org chosen shows every app"), Visible.Num(), 4);

	Filter.OrgId = 10;
	CrowdyFilterApps(Apps, Filter, Visible);
	TestEqual(TEXT("Org 10 shows its two apps"), Visible.Num(), 2);
	TestTrue(TEXT("Org 10's apps are the ones shown"), Visible.Num() == 2 && Visible[0]->AppId == 1 && Visible[1]->AppId == 2);

	Filter.OrgId = 20;
	Filter.Status = TEXT("LIVE");
	CrowdyFilterApps(Apps, Filter, Visible);
	TestEqual(TEXT("Status narrows within the org"), Visible.Num(), 1);
	TestTrue(TEXT("The live app of org 20 is the one shown"), Visible.Num() == 1 && Visible[0]->AppId == 3);

	Filter = FCrowdyAppListFilter();
	Filter.Search = TEXT("sand");
	CrowdyFilterApps(Apps, Filter, Visible);
	TestEqual(TEXT("Search matches the name case-insensitively"), Visible.Num(), 1);

	Filter.Search = TEXT("4");
	CrowdyFilterApps(Apps, Filter, Visible);
	TestEqual(TEXT("Search matches the id"), Visible.Num(), 1);
	TestTrue(TEXT("The id match is app 4"), Visible.Num() == 1 && Visible[0]->AppId == 4);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyProjectPageCreateAppSendsDatacenterTest,
	"CrowdySDK.CrowdyStudio.ProjectPage.CreateAppSendsDatacenter", CrowdyProjectPageTestFlags)

bool FCrowdyProjectPageCreateAppSendsDatacenterTest::RunTest(const FString& /*Parameters*/)
{
	// createApp refuses an input without a datacenter, which is the failure the old form hit. The variables
	// builder is the one place the input is shaped, so this is where a dropped field would show.
	const TSharedPtr<FJsonObject> Variables = CrowdyStudioGql::BuildCreateAppVariables(90820086220032LL, TEXT("Test"), TEXT("test"), TEXT("or"), TEXT(""));

	const TSharedPtr<FJsonObject>* Input = nullptr;
	if (!TestTrue(TEXT("The variables carry an input object"), Variables->TryGetObjectField(TEXT("input"), Input)))
	{
		return false;
	}

	FString Datacenter;
	TestTrue(TEXT("The input names a datacenter"), (*Input)->TryGetStringField(TEXT("datacenter"), Datacenter));
	TestEqual(TEXT("The datacenter is the chosen code"), Datacenter, FString(TEXT("or")));

	// appId and orgId are BigInt on the wire: a JSON number would lose precision above 2^53.
	FString OrgId;
	TestTrue(TEXT("orgId is emitted as a string"), (*Input)->TryGetStringField(TEXT("orgId"), OrgId));
	TestEqual(TEXT("orgId keeps every digit"), OrgId, FString(TEXT("90820086220032")));

	TestFalse(TEXT("An empty description is omitted rather than sent blank"), (*Input)->HasField(TEXT("description")));
	TestFalse(TEXT("Status is left to the server default"), (*Input)->HasField(TEXT("status")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyProjectPageSlugFromNameTest,
	"CrowdySDK.CrowdyStudio.ProjectPage.SlugFromName", CrowdyProjectPageTestFlags)

bool FCrowdyProjectPageSlugFromNameTest::RunTest(const FString& /*Parameters*/)
{
	TestEqual(TEXT("Lowercases and hyphenates"), CrowdyStudioGql::SlugFromName(TEXT("My Game")), FString(TEXT("my-game")));
	TestEqual(TEXT("Collapses runs of separators"), CrowdyStudioGql::SlugFromName(TEXT("Crowded  Kingdoms: Titan!")), FString(TEXT("crowded-kingdoms-titan")));
	TestEqual(TEXT("Never starts or ends with a hyphen"), CrowdyStudioGql::SlugFromName(TEXT("  -Test- ")), FString(TEXT("test")));
	TestEqual(TEXT("A valid slug is its own slug"), CrowdyStudioGql::SlugFromName(TEXT("ck-2")), FString(TEXT("ck-2")));
	TestEqual(TEXT("Nothing usable gives an empty slug"), CrowdyStudioGql::SlugFromName(TEXT("!!!")), FString());

	// The 128 cut must not leave a trailing hyphen, or the derived slug fails its own validation.
	FString Long;
	for (int32 I = 0; I < 65; ++I) { Long += TEXT("a "); }
	const FString Cut = CrowdyStudioGql::SlugFromName(Long);
	TestEqual(TEXT("The cut slug is 127 long (128 ended on a hyphen)"), Cut.Len(), 127);
	TestFalse(TEXT("The cut slug does not end in a hyphen"), Cut.EndsWith(TEXT("-")));
	TestEqual(TEXT("The cut slug is its own slug"), CrowdyStudioGql::SlugFromName(Cut), Cut);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyProjectPageParsesPlaceableDatacentersTest,
	"CrowdySDK.CrowdyStudio.ProjectPage.ParsesPlaceableDatacenters", CrowdyProjectPageTestFlags)

bool FCrowdyProjectPageParsesPlaceableDatacentersTest::RunTest(const FString& /*Parameters*/)
{
	const TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	const TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("placementEnforced"), true);
	TArray<TSharedPtr<FJsonValue>> Datacenters;
	auto AddDc = [&Datacenters](const TCHAR* Code, bool bPlaceable, const TCHAR* Serving)
	{
		const TSharedPtr<FJsonObject> Dc = MakeShared<FJsonObject>();
		Dc->SetStringField(TEXT("code"), Code);
		Dc->SetStringField(TEXT("gameApiUrl"), FString::Printf(TEXT("https://ck-%s.test.example.com"), Code));
		Dc->SetStringField(TEXT("gameApiWsUrl"), FString::Printf(TEXT("wss://ck-%s.test.example.com"), Code));
		Dc->SetBoolField(TEXT("placeable"), bPlaceable);
		Dc->SetNumberField(TEXT("appShardCount"), 4);
		Dc->SetStringField(TEXT("serving"), Serving);
		Datacenters.Add(MakeShared<FJsonValueObject>(Dc));
	};
	AddDc(TEXT("or"), true, TEXT("SERVING"));
	AddDc(TEXT("va"), false, TEXT("NOT_SERVING"));
	Root->SetArrayField(TEXT("datacenters"), Datacenters);
	Data->SetObjectField(TEXT("placeableDatacenters"), Root);

	TArray<FStudioDatacenter> Parsed;
	bool bEnforced = false;
	CrowdyStudioGql::ParsePlaceableDatacenters(CrowdyStudioGql::WrapDataEnvelope(Data), Parsed, bEnforced);

	TestTrue(TEXT("Placement enforcement is read"), bEnforced);
	if (!TestEqual(TEXT("Both datacenters are read"), Parsed.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("The code is read"), Parsed[0].Code, FString(TEXT("or")));
	TestTrue(TEXT("Placeable is read"), Parsed[0].bPlaceable);
	// Three-valued on the wire (UNKNOWN is not an outage), so it is kept as sent rather than folded into a bool.
	TestEqual(TEXT("Serving is kept as sent"), Parsed[0].Serving, FString(TEXT("SERVING")));
	TestFalse(TEXT("A non-placeable datacenter stays non-placeable"), Parsed[1].bPlaceable);
	TestEqual(TEXT("NOT_SERVING is kept as sent"), Parsed[1].Serving, FString(TEXT("NOT_SERVING")));
	TestEqual(TEXT("The shard count is read"), Parsed[0].AppShardCount, 4);
	TestEqual(TEXT("The endpoint is read"), Parsed[0].GameApiUrl, FString(TEXT("https://ck-or.test.example.com")));
	TestEqual(TEXT("The WS endpoint is read"), Parsed[0].GameApiWsUrl, FString(TEXT("wss://ck-or.test.example.com")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyProjectPageParsesAppDiscoveryTest,
	"CrowdySDK.CrowdyStudio.ProjectPage.ParsesAppDiscovery", CrowdyProjectPageTestFlags)

bool FCrowdyProjectPageParsesAppDiscoveryTest::RunTest(const FString& /*Parameters*/)
{
	// appDiscovery answers a list keyed by appId; the entry for the asked app supplies the datacenter code and
	// the WS endpoint, and never the HTTP endpoint (that stays the app record's own routing).
	auto Entry = [](const TCHAR* AppId, const TCHAR* Code, const TCHAR* Ws)
	{
		const TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
		Node->SetStringField(TEXT("appId"), AppId);
		Node->SetStringField(TEXT("datacenterCode"), Code);
		Node->SetStringField(TEXT("gameApiUrl"), TEXT("https://elsewhere.example.com"));
		Node->SetStringField(TEXT("gameApiWsUrl"), Ws);
		return MakeShared<FJsonValueObject>(Node);
	};
	TArray<TSharedPtr<FJsonValue>> Entries;
	Entries.Add(Entry(TEXT("11"), TEXT("va"), TEXT("wss://ck-va.test.example.com")));
	Entries.Add(Entry(TEXT("90820086220032"), TEXT("or"), TEXT("wss://ck-or.test.example.com")));
	const TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("appDiscovery"), Entries);

	FStudioApp App;
	App.AppId = 90820086220032LL;
	App.GameApiUrl = TEXT("https://ck-or.test.example.com");
	App.DatacenterCode = TEXT("stale");

	TestTrue(TEXT("The asked app's entry is found"), CrowdyStudioGql::ParseAppDiscovery(CrowdyStudioGql::WrapDataEnvelope(Data), App.AppId, App));
	TestEqual(TEXT("The datacenter code is the asked app's, not the first entry's"), App.DatacenterCode, FString(TEXT("or")));
	TestEqual(TEXT("The WS endpoint is read"), App.GameApiWsUrl, FString(TEXT("wss://ck-or.test.example.com")));
	TestEqual(TEXT("The HTTP endpoint is left to the app record"), App.GameApiUrl, FString(TEXT("https://ck-or.test.example.com")));

	FStudioApp Other;
	Other.AppId = 7;
	Other.DatacenterCode = TEXT("stale");
	TestFalse(TEXT("An app the reply does not name is reported missing"), CrowdyStudioGql::ParseAppDiscovery(CrowdyStudioGql::WrapDataEnvelope(Data), Other.AppId, Other));
	TestEqual(TEXT("A missing entry leaves the record untouched"), Other.DatacenterCode, FString(TEXT("stale")));

	// A null code (an un-placed app) clears the previous one rather than keeping it.
	const TSharedPtr<FJsonObject> Unplaced = MakeShared<FJsonObject>();
	Unplaced->SetStringField(TEXT("appId"), TEXT("7"));
	Unplaced->SetField(TEXT("datacenterCode"), MakeShared<FJsonValueNull>());
	TArray<TSharedPtr<FJsonValue>> UnplacedEntries;
	UnplacedEntries.Add(MakeShared<FJsonValueObject>(Unplaced));
	const TSharedPtr<FJsonObject> UnplacedData = MakeShared<FJsonObject>();
	UnplacedData->SetArrayField(TEXT("appDiscovery"), UnplacedEntries);
	TestTrue(TEXT("The un-placed app's entry is found"), CrowdyStudioGql::ParseAppDiscovery(CrowdyStudioGql::WrapDataEnvelope(UnplacedData), Other.AppId, Other));
	TestTrue(TEXT("A null code clears the stale one"), Other.DatacenterCode.IsEmpty());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
