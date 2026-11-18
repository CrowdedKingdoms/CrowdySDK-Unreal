#include "Subsystem/CrowdyStudioProjects.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Containers/Ticker.h"
#include "CrowdyCppClient.h"
#include "Engine/GameInstance.h"
#include "Misc/AutomationTest.h"
#include "Misc/DateTime.h"
#include "Subsystem/CrowdyMods.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include <type_traits>

namespace CrowdyStudioProjectsTests
{
	constexpr EAutomationTestFlags StudioFlags = EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
	constexpr int64 StudioTestApp = 31337;
	constexpr int64 StudioBigGrid = 9007199254740995LL;

	FString StudioErrorAnswer(const TCHAR* Code)
	{
		return FString::Printf(TEXT("{\"errors\":[{\"message\":\"No.\",\"extensions\":{\"code\":\"%s\"}}],\"data\":null}"), Code);
	}

	FString StudioDataAnswer(const TCHAR* Field, const FString& Value)
	{
		return FString::Printf(TEXT("{\"data\":{\"%s\":%s}}"), Field, *Value);
	}

	const TCHAR* const StudioTwoFiles =
		TEXT("[{\"target\":\"SERVER\",\"path\":\"Cargo.toml\",\"content\":\"[package]\\n\",\"revision\":\"3\"},")
		TEXT("{\"target\":\"CLIENT\",\"path\":\"src/lib.rs\",\"content\":\"// client\\n\",\"revision\":\"4\"}]");

	FString StudioProjectJson(const TCHAR* Id, const TCHAR* Pairing, bool bGitHub = false, const TCHAR* Files = TEXT("[]"), const TCHAR* GridId = TEXT("\"9007199254740995\""))
	{
		return FString::Printf(
			TEXT("{\"projectId\":\"%s\",\"gridId\":%s,\"name\":\"spinner\",\"serverModuleName\":\"spinner\",\"clientModuleName\":null,\"pairingPreference\":\"%s\",")
			TEXT("\"revision\":\"7\",\"archived\":false,\"updatedAt\":\"2026-11-11T10:00:00.250Z\",\"source\":\"%s\",\"githubOwner\":%s,\"githubRepo\":%s,")
			TEXT("\"githubBranch\":%s,\"githubSha\":%s,\"files\":%s}"),
			Id, GridId, Pairing, bGitHub ? TEXT("GITHUB") : TEXT("STUDIO"), bGitHub ? TEXT("\"acme\"") : TEXT("null"), bGitHub ? TEXT("\"spinner\"") : TEXT("null"),
			bGitHub ? TEXT("\"main\"") : TEXT("null"), bGitHub ? TEXT("\"abc123\"") : TEXT("null"), Files);
	}

	FString StudioListAnswer(const TArray<FString>& Rows)
	{
		return StudioDataAnswer(TEXT("crowdyStudioProjects"), FString::Printf(TEXT("[%s]"), *FString::Join(Rows, TEXT(","))));
	}

	TArray<FString> StudioRows(int32 Count)
	{
		TArray<FString> Rows;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Rows.Add(StudioProjectJson(*FString::Printf(TEXT("p-%d"), Index), TEXT("PAIRED")));
		}
		return Rows;
	}

	// The server starter, with a README the platform's project would refuse.
	const TCHAR* const StudioStarterAnswer =
		TEXT("{\"data\":{\"execModStarter\":{\"crate\":\"grid-mod\",\"nodeType\":\"mod:grid-mod\",\"description\":\"Greets visitors.\",")
		TEXT("\"files\":[{\"path\":\"Cargo.toml\",\"content\":\"[package]\\nname = \\\"grid-mod\\\"\\n\"},{\"path\":\"src/lib.rs\",\"content\":\"// visit\\n\"},")
		TEXT("{\"path\":\"README.md\",\"content\":\"hi\"}]}}}");

	// The platform's CLIENT half starter, marked so its files show apart from this SDK's copy.
	const TCHAR* const StudioClientStarterAnswer =
		TEXT("{\"data\":{\"execModClientStarter\":{\"crate\":\"client-half\",\"nodeType\":\"client\",\"description\":\"A CLIENT half.\",")
		TEXT("\"files\":[{\"path\":\"Cargo.toml\",\"content\":\"[package]\\nname = \\\"client-half\\\"\\n[lib]\\ncrate-type = [\\\"cdylib\\\"]\\n[dependencies]\\ncrowdy-client-sdk = \\\"0.1.0\\\"\\n\"},")
		TEXT("{\"path\":\"src/lib.rs\",\"content\":\"// from the platform\\ncrowdy::register_module!(init: init);\\n\"}]}}}");

	// An API host whose build toolchain predates the CLIENT half starter.
	const TCHAR* const StudioClientStarterUnavailable =
		TEXT("{\"errors\":[{\"message\":\"The CLIENT starter is unavailable.\",\"extensions\":{\"code\":\"INTERNAL_SERVER_ERROR\",\"httpStatus\":503}}],\"data\":null}");

	int32 StudioCountOf(const FString& Text, const TCHAR* Needle)
	{
		int32 Count = 0;
		int32 At = 0;
		while ((At = Text.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromStart, At)) != INDEX_NONE)
		{
			++Count;
			At += FCString::Strlen(Needle);
		}
		return Count;
	}

	int64 StudioTimeMs(int32 Year, int32 Month, int32 Day, int32 Hour, int32 Millisecond)
	{
		return (FDateTime(Year, Month, Day, Hour, 0, 0, Millisecond) - FDateTime(1970, 1, 1)).GetTicks() / ETimespan::TicksPerMillisecond;
	}

	struct FStudioRig
	{
		TStrongObjectPtr<UGameInstance> Instance;
		TStrongObjectPtr<UCrowdyMods> Mods;
		TStrongObjectPtr<UCrowdyStudioProjects> Projects;
		TSharedPtr<FCrowdyCppClient> Client;
		int32 Answers = 0;
		FCrowdyStudioProjectList List;
		FCrowdyStudioProjectInfo Info;

		explicit FStudioRig(const FString& Body, int32 Status = 200, bool bSignedIn = true)
			: Instance(NewObject<UGameInstance>(GetTransientPackage(), NAME_None, RF_Transient))
			, Mods(NewObject<UCrowdyMods>(Instance.Get()))
			, Projects(NewObject<UCrowdyStudioProjects>(Instance.Get()))
			, Client(FCrowdyCppClient::MakeForTest(Body, Status))
		{
			if (Client.IsValid() && bSignedIn)
			{
				Client->SetGameToken(TEXT("app-token"));
				Client->SetManagementToken(TEXT("session-token"));
			}
			Mods->InitializeForTest(bSignedIn ? Client : nullptr, StudioTestApp);
			Projects->InitializeForTest(bSignedIn ? Client : nullptr, StudioTestApp, Mods.Get());
		}

		void Script(TArray<TPair<int32, FString>> Responses)
		{
			Client->SetTestResponseScript(MoveTemp(Responses));
		}

		void ListProjects()
		{
			Projects->ListProjects([this](const FCrowdyStudioProjectList& Answer) { List = Answer; ++Answers; });
		}

		void GetProject(const FString& Id)
		{
			Projects->GetProject(Id, [this](const FCrowdyStudioProjectInfo& Answer) { Info = Answer; ++Answers; });
		}

		void CreateProject(const FString& Name, ECrowdyStudioProjectKind Kind)
		{
			Projects->CreateProject(Name, Kind, [this](const FCrowdyStudioProjectInfo& Answer) { Info = Answer; ++Answers; });
		}

		void CreateProject(const FString& Name, ECrowdyStudioProjectKind Kind, const FString& ServerModule, const FString& ClientModule)
		{
			Projects->CreateProject(Name, Kind, ServerModule, ClientModule, [this](const FCrowdyStudioProjectInfo& Answer) { Info = Answer; ++Answers; });
		}

		/** Pumps the client and the ticker until Done or the steps run out. */
		bool PumpUntil(TFunctionRef<bool()> Done, int32 Steps = 400)
		{
			for (int32 Step = 0; Step < Steps && !Done(); ++Step)
			{
				Client->Poll();
				FTSTicker::GetCoreTicker().Tick(0.f);
			}
			return Done();
		}

		void Pump()
		{
			PumpUntil([this]() { return Answers > 0; });
		}

		FString SentBody() const
		{
			FString Body;
			Client->GetLastTestRequestBody(Body);
			return Body;
		}

		bool SentAnything() const
		{
			FString Url;
			FString Authorization;
			return Client->GetLastTestRequest(Url, Authorization);
		}

		FString SentAuthorization() const
		{
			FString Url;
			FString Authorization;
			Client->GetLastTestRequest(Url, Authorization);
			return Authorization;
		}
	};
}

using namespace CrowdyStudioProjectsTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioProjectsKindsTest, "CrowdySDK.StudioProjects.KindsFollowThePlatformsPairing", StudioFlags)
bool FCrowdyStudioProjectsKindsTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("SERVER_ONLY first"), static_cast<int32>(ECrowdyStudioProjectKind::Server), 0);
	TestEqual(TEXT("then CLIENT_ONLY"), static_cast<int32>(ECrowdyStudioProjectKind::Client), 1);
	TestEqual(TEXT("then PAIRED"), static_cast<int32>(ECrowdyStudioProjectKind::Paired), 2);
	TestEqual(TEXT("then INDEPENDENT"), static_cast<int32>(ECrowdyStudioProjectKind::Independent), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioProjectsDefaultsTest, "CrowdySDK.StudioProjects.AnswersDefaultToNothing", StudioFlags)
bool FCrowdyStudioProjectsDefaultsTest::RunTest(const FString& Parameters)
{
	static_assert(std::is_same_v<decltype(FCrowdyStudioProject::GridId), int64>, "a grid id is a BigInt");
	static_assert(std::is_same_v<decltype(FCrowdyStudioProject::UpdatedAtMs), int64>, "a time is int64 milliseconds");
	const FCrowdyStudioProject Project;
	TestTrue(TEXT("a default project names nothing"), Project.ProjectId.IsEmpty() && Project.Name.IsEmpty() && Project.Revision.IsEmpty()
		&& Project.ServerModule.IsEmpty() && Project.ClientModule.IsEmpty());
	TestTrue(TEXT("a default project is bound to nothing"), Project.UpdatedAtMs == 0 && Project.GitHubSha.IsEmpty() && Project.GitHubRef.IsEmpty() && Project.GridId == 0);
	const FCrowdyStudioProjectInfo Info;
	TestTrue(TEXT("a default answer is not a success and holds no files"), Info.Result != ECrowdyStudioResult::Success && Info.Files.IsEmpty());
	const FCrowdyStudioProjectList List;
	TestTrue(TEXT("a default list is not a success and is empty"), List.Result != ECrowdyStudioResult::Success && List.Projects.IsEmpty() && !List.bMore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioProjectsListTest, "CrowdySDK.CrowdyServices.StudioProjects.ListSendsThePageAndParsesRows", StudioFlags)
bool FCrowdyStudioProjectsListTest::RunTest(const FString& Parameters)
{
	FStudioRig Rig(StudioListAnswer({StudioProjectJson(TEXT("p-1"), TEXT("PAIRED")), StudioProjectJson(TEXT("p-2"), TEXT("INDEPENDENT"), true, TEXT("[]"), TEXT("null"))}));
	Rig.ListProjects();
	TestEqual(TEXT("nothing answers inside the call"), Rig.Answers, 0);
	Rig.Pump();
	TestTrue(TEXT("the list is read"), Rig.List.Result == ECrowdyStudioResult::Success);
	TestEqual(TEXT("with both rows"), Rig.List.Projects.Num(), 2);
	TestFalse(TEXT("a short page claims no more"), Rig.List.bMore);
	const FString Body = Rig.SentBody();
	TestTrue(TEXT("the read names the app, the page and archived ones left out"),
		Body.Contains(TEXT("\"appId\":\"31337\"")) && Body.Contains(TEXT("\"limit\":50")) && Body.Contains(TEXT("\"offset\":0")) && Body.Contains(TEXT("\"includeArchived\":false")));
	TestEqual(TEXT("on the app token"), Rig.SentAuthorization(), FString(TEXT("Bearer app-token")));
	if (Rig.List.Projects.Num() != 2)
	{
		return true;
	}
	const FCrowdyStudioProject& First = Rig.List.Projects[0];
	TestTrue(TEXT("a Studio row keeps its id, revision, kind and source"), First.ProjectId == TEXT("p-1") && First.Revision == TEXT("7") && First.Kind == ECrowdyStudioProjectKind::Paired
		&& First.Source == ECrowdyStudioProjectSource::Studio);
	TestEqual(TEXT("its updated time is milliseconds"), First.UpdatedAtMs, StudioTimeMs(2026, 11, 11, 10, 250));
	TestEqual(TEXT("its grid id is a whole int64"), First.GridId, StudioBigGrid);
	TestTrue(TEXT("its modules read, an unset one as empty"), First.ServerModule == TEXT("spinner") && First.ClientModule.IsEmpty());
	TestTrue(TEXT("a Studio row has no GitHub ref"), First.GitHubRef.IsEmpty() && First.GitHubSha.IsEmpty());
	const FCrowdyStudioProject& Second = Rig.List.Projects[1];
	TestTrue(TEXT("a GitHub row keeps its ref and sha"), Second.Source == ECrowdyStudioProjectSource::GitHub && Second.GitHubRef == TEXT("acme/spinner@main") && Second.GitHubSha == TEXT("abc123"));
	TestEqual(TEXT("a GitHub row's revision carries the commit, so a commit changes it"), Second.Revision, FString(TEXT("7@abc123")));
	TestTrue(TEXT("its kind is read and a null grid is 0"), Second.Kind == ECrowdyStudioProjectKind::Independent && Second.GridId == 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioProjectsMoreTest, "CrowdySDK.CrowdyServices.StudioProjects.MoreIsAProbePastAFullPage", StudioFlags)
bool FCrowdyStudioProjectsMoreTest::RunTest(const FString& Parameters)
{
	{
		FStudioRig Rig(StudioListAnswer(StudioRows(49)));
		Rig.ListProjects();
		Rig.Pump();
		TestTrue(TEXT("49 rows are a short page"), Rig.List.Result == ECrowdyStudioResult::Success && Rig.List.Projects.Num() == 49 && !Rig.List.bMore);
		TestTrue(TEXT("and are not probed"), Rig.SentBody().Contains(TEXT("\"limit\":50")));
	}
	{
		FStudioRig Rig{FString()};
		Rig.Script({{200, StudioListAnswer(StudioRows(50))}, {200, StudioListAnswer(StudioRows(1))}});
		Rig.ListProjects();
		Rig.Pump();
		TestTrue(TEXT("a full page with a row after it says more"), Rig.List.Result == ECrowdyStudioResult::Success && Rig.List.Projects.Num() == 50 && Rig.List.bMore);
		TestTrue(TEXT("the probe asks for one row past the page"), Rig.SentBody().Contains(TEXT("\"limit\":1")) && Rig.SentBody().Contains(TEXT("\"offset\":50")));
	}
	{
		FStudioRig Rig{FString()};
		Rig.Script({{200, StudioListAnswer(StudioRows(50))}, {200, StudioListAnswer(StudioRows(0))}});
		Rig.ListProjects();
		Rig.Pump();
		TestTrue(TEXT("a full page with nothing after it says no more"), Rig.List.Result == ECrowdyStudioResult::Success && Rig.List.Projects.Num() == 50 && !Rig.List.bMore);
	}
	{
		FStudioRig Rig{FString()};
		Rig.Script({{200, StudioListAnswer(StudioRows(50))}, {200, StudioErrorAnswer(TEXT("FORBIDDEN"))}});
		Rig.ListProjects();
		Rig.Pump();
		TestTrue(TEXT("a probe that fails fails the call and returns no rows"), Rig.List.Result == ECrowdyStudioResult::Denied && Rig.List.Projects.IsEmpty());
	}
	{
		FStudioRig Rig(StudioListAnswer(StudioRows(51)));
		Rig.ListProjects();
		Rig.Pump();
		TestTrue(TEXT("more rows than a page holds is a bad reply"), Rig.List.Result == ECrowdyStudioResult::BadReply && Rig.List.Projects.IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioProjectsGetTest, "CrowdySDK.CrowdyServices.StudioProjects.GetParsesFilesAndRevision", StudioFlags)
bool FCrowdyStudioProjectsGetTest::RunTest(const FString& Parameters)
{
	{
		FStudioRig Rig(StudioDataAnswer(TEXT("crowdyStudioProject"), StudioProjectJson(TEXT("p-1"), TEXT("PAIRED"), false, StudioTwoFiles)));
		Rig.GetProject(TEXT("p-1"));
		TestEqual(TEXT("nothing answers inside the call"), Rig.Answers, 0);
		Rig.Pump();
		TestTrue(TEXT("the project is read"), Rig.Info.Result == ECrowdyStudioResult::Success);
		TestTrue(TEXT("with its id and revision"), Rig.Info.Project.ProjectId == TEXT("p-1") && Rig.Info.Project.Revision == TEXT("7"));
		TestTrue(TEXT("the read names the app and the project"), Rig.SentBody().Contains(TEXT("\"appId\":\"31337\"")) && Rig.SentBody().Contains(TEXT("\"projectId\":\"p-1\"")));
		if (TestEqual(TEXT("both files are read"), Rig.Info.Files.Num(), 2))
		{
			TestTrue(TEXT("the first is the SERVER Cargo.toml, newlines kept"), Rig.Info.Files[0].Target == ECrowdyStudioTarget::Server && Rig.Info.Files[0].Path == TEXT("Cargo.toml")
				&& Rig.Info.Files[0].Content == TEXT("[package]\n") && Rig.Info.Files[0].Revision == TEXT("3"));
			TestTrue(TEXT("the second is the CLIENT lib"), Rig.Info.Files[1].Target == ECrowdyStudioTarget::Client && Rig.Info.Files[1].Path == TEXT("src/lib.rs")
				&& Rig.Info.Files[1].Revision == TEXT("4"));
		}
	}
	{
		FStudioRig Rig(StudioDataAnswer(TEXT("crowdyStudioProject"), StudioProjectJson(TEXT("p-1"), TEXT("PAIRED"), true, StudioTwoFiles)));
		Rig.GetProject(TEXT("p-1"));
		Rig.Pump();
		TestTrue(TEXT("a GitHub project's get carries the same revision@sha as its list row"), Rig.Info.Result == ECrowdyStudioResult::Success && Rig.Info.Project.Revision == TEXT("7@abc123"));
	}
	{
		FStudioRig Rig(StudioErrorAnswer(TEXT("NOT_FOUND")));
		Rig.GetProject(TEXT("p-9"));
		Rig.Pump();
		TestTrue(TEXT("a missing project is not found and carries nothing"), Rig.Info.Result == ECrowdyStudioResult::NotFound && Rig.Info.Project.ProjectId.IsEmpty() && Rig.Info.Files.IsEmpty());
	}
	{
		FStudioRig Rig(StudioDataAnswer(TEXT("crowdyStudioProject"), StudioProjectJson(TEXT("p-2"), TEXT("PAIRED"))));
		Rig.GetProject(TEXT("p-1"));
		Rig.Pump();
		TestTrue(TEXT("another project's answer is a bad reply"), Rig.Info.Result == ECrowdyStudioResult::BadReply && Rig.Info.Project.ProjectId.IsEmpty());
	}
	{
		FStudioRig Rig(StudioErrorAnswer(TEXT("FORBIDDEN")));
		Rig.GetProject(TEXT("p-1"));
		Rig.Pump();
		TestTrue(TEXT("a permission code is denied"), Rig.Info.Result == ECrowdyStudioResult::Denied);
	}
	{
		FStudioRig Rig(StudioErrorAnswer(TEXT("INVALID_INPUT")));
		Rig.GetProject(TEXT("p-1"));
		Rig.Pump();
		TestTrue(TEXT("any other code is refused"), Rig.Info.Result == ECrowdyStudioResult::Refused);
	}
	{
		FStudioRig Rig(FString(), 503);
		Rig.GetProject(TEXT("p-1"));
		Rig.Pump();
		TestTrue(TEXT("a call that did not complete fails"), Rig.Info.Result == ECrowdyStudioResult::Failed);
	}
	{
		FStudioRig Rig{FString()};
		Rig.GetProject(FString());
		Rig.Pump();
		TestTrue(TEXT("an empty id is refused without a request"), Rig.Info.Result == ECrowdyStudioResult::Refused && !Rig.SentAnything());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioProjectsMalformedTest, "CrowdySDK.CrowdyServices.StudioProjects.AMalformedRowIsABadReply", StudioFlags)
bool FCrowdyStudioProjectsMalformedTest::RunTest(const FString& Parameters)
{
	const FString Good = StudioProjectJson(TEXT("p-1"), TEXT("PAIRED"));
	const TCHAR* const Breaks[][2] = {
		{TEXT("\"revision\":\"7\""), TEXT("\"revision\":\"7x\"")},
		{TEXT("\"revision\":\"7\""), TEXT("\"rev\":\"7\"")},
		{TEXT("\"pairingPreference\":\"PAIRED\""), TEXT("\"pairingPreference\":\"BOTH\"")},
		{TEXT("\"name\":\"spinner\""), TEXT("\"name\":5")},
		{TEXT("\"updatedAt\":\"2026-11-11T10:00:00.250Z\""), TEXT("\"updatedAt\":\"yesterday\"")},
		{TEXT("\"source\":\"STUDIO\""), TEXT("\"source\":\"GIT\"")},
		{TEXT("\"gridId\":\"9007199254740995\""), TEXT("\"gridId\":\"12abc\"")},
	};
	for (const TCHAR* const (&Break)[2] : Breaks)
	{
		FStudioRig Rig(StudioListAnswer({Good, Good.Replace(Break[0], Break[1])}));
		Rig.ListProjects();
		Rig.Pump();
		TestTrue(FString::Printf(TEXT("a row with %s is a bad reply and no row is returned"), Break[1]), Rig.List.Result == ECrowdyStudioResult::BadReply && Rig.List.Projects.IsEmpty());
	}
	{
		FStudioRig Rig(StudioDataAnswer(TEXT("crowdyStudioProject"), StudioProjectJson(TEXT("p-1"), TEXT("PAIRED"), false, TEXT("[{\"target\":\"BOTH\",\"path\":\"Cargo.toml\",\"content\":\"\",\"revision\":\"1\"}]"))));
		Rig.GetProject(TEXT("p-1"));
		Rig.Pump();
		TestTrue(TEXT("a file for no known target is a bad reply and no file is returned"), Rig.Info.Result == ECrowdyStudioResult::BadReply && Rig.Info.Files.IsEmpty());
	}
	const TCHAR* const BadPaths[] = {TEXT("src/../x.rs"), TEXT("../Cargo.toml"), TEXT("/src/a.rs"), TEXT("src//a.rs"), TEXT("src/a\\\\b.rs"), TEXT("src/a/.rs"), TEXT("src/a.rs.rs"),
		TEXT("src/a b.rs"), TEXT("src/a.txt"), TEXT("build.rs"), TEXT("src/.rs"), TEXT("src/a/b/"), TEXT("Cargo.toml/x")};
	for (const TCHAR* const Path : BadPaths)
	{
		const FString Files = FString::Printf(TEXT("[{\"target\":\"SERVER\",\"path\":\"%s\",\"content\":\"\",\"revision\":\"1\"}]"), Path);
		FStudioRig Rig(StudioDataAnswer(TEXT("crowdyStudioProject"), StudioProjectJson(TEXT("p-1"), TEXT("PAIRED"), false, *Files)));
		Rig.GetProject(TEXT("p-1"));
		Rig.Pump();
		TestTrue(FString::Printf(TEXT("a file path of %s is a bad reply and no file is returned"), Path), Rig.Info.Result == ECrowdyStudioResult::BadReply && Rig.Info.Files.IsEmpty());
	}
	const TCHAR* const GoodPaths[] = {TEXT("Cargo.toml"), TEXT("README.md"), TEXT("src/lib.rs"), TEXT("src/a_b/C-d/e9.rs")};
	for (const TCHAR* const Path : GoodPaths)
	{
		const FString Files = FString::Printf(TEXT("[{\"target\":\"SERVER\",\"path\":\"%s\",\"content\":\"\",\"revision\":\"1\"}]"), Path);
		FStudioRig Rig(StudioDataAnswer(TEXT("crowdyStudioProject"), StudioProjectJson(TEXT("p-1"), TEXT("PAIRED"), false, *Files)));
		Rig.GetProject(TEXT("p-1"));
		Rig.Pump();
		TestTrue(FString::Printf(TEXT("a file path of %s reads"), Path), Rig.Info.Result == ECrowdyStudioResult::Success && Rig.Info.Files.Num() == 1);
	}
	{
		const FString NoFiles = StudioProjectJson(TEXT("p-1"), TEXT("PAIRED")).Replace(TEXT("\"files\":[]"), TEXT("\"filez\":[]"));
		FStudioRig Rig(StudioDataAnswer(TEXT("crowdyStudioProject"), NoFiles));
		Rig.GetProject(TEXT("p-1"));
		Rig.Pump();
		TestTrue(TEXT("a project with no files field is a bad reply"), Rig.Info.Result == ECrowdyStudioResult::BadReply);
	}
	{
		FStudioRig Rig(StudioDataAnswer(TEXT("crowdyStudioProject"), TEXT("[]")));
		Rig.GetProject(TEXT("p-1"));
		Rig.Pump();
		TestTrue(TEXT("an answer that is not an object is a bad reply"), Rig.Info.Result == ECrowdyStudioResult::BadReply);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioProjectsCreateTest, "CrowdySDK.CrowdyServices.StudioProjects.CreateSendsPairingAndStartersByKind", StudioFlags)
bool FCrowdyStudioProjectsCreateTest::RunTest(const FString& Parameters)
{
	struct FKindCase
	{
		ECrowdyStudioProjectKind Kind;
		const TCHAR* Pairing;
	};
	const FKindCase Cases[] = {
		{ECrowdyStudioProjectKind::Server, TEXT("SERVER_ONLY")}, {ECrowdyStudioProjectKind::Client, TEXT("CLIENT_ONLY")},
		{ECrowdyStudioProjectKind::Paired, TEXT("PAIRED")}, {ECrowdyStudioProjectKind::Independent, TEXT("INDEPENDENT")}};
	for (const FKindCase& Case : Cases)
	{
		const bool bServer = Case.Kind != ECrowdyStudioProjectKind::Client;
		const bool bClient = Case.Kind != ECrowdyStudioProjectKind::Server;
		const FString Created = StudioDataAnswer(TEXT("crowdyStudioProjectCreate"), StudioProjectJson(TEXT("p-new"), Case.Pairing, false, StudioTwoFiles));
		FStudioRig Rig(Created);
		TArray<TPair<int32, FString>> Script;
		if (bServer)
		{
			Script.Add({200, StudioStarterAnswer});
		}
		if (bClient)
		{
			Script.Add({200, StudioClientStarterAnswer});
		}
		Script.Add({200, Created});
		Rig.Script(MoveTemp(Script));
		Rig.CreateProject(TEXT("My Spinner"), Case.Kind);
		TestEqual(FString::Printf(TEXT("%s: nothing answers inside the call"), Case.Pairing), Rig.Answers, 0);
		Rig.Pump();
		const FString Body = Rig.SentBody();
		TestTrue(FString::Printf(TEXT("%s: the created project is read"), Case.Pairing), Rig.Info.Result == ECrowdyStudioResult::Success && Rig.Info.Project.ProjectId == TEXT("p-new") && Rig.Info.Files.Num() == 2);
		TestTrue(FString::Printf(TEXT("%s: the pairing is sent explicitly"), Case.Pairing), Body.Contains(FString::Printf(TEXT("\"pairingPreference\":\"%s\""), Case.Pairing)));
		TestTrue(FString::Printf(TEXT("%s: the create names the app"), Case.Pairing), Body.Contains(TEXT("\"appId\":\"31337\"")));
		TestEqual(FString::Printf(TEXT("%s: the SERVER half carries the platform's starter files"), Case.Pairing), StudioCountOf(Body, TEXT("\"target\":\"SERVER\"")), bServer ? 2 : 0);
		TestEqual(FString::Printf(TEXT("%s: the CLIENT half carries the platform's CLIENT starter files"), Case.Pairing), StudioCountOf(Body, TEXT("\"target\":\"CLIENT\"")), bClient ? 2 : 0);
		TestTrue(FString::Printf(TEXT("%s: the SERVER starter's source travels exactly for a SERVER half"), Case.Pairing), Body.Contains(TEXT("// visit")) == bServer);
		TestTrue(FString::Printf(TEXT("%s: the CLIENT starter's source travels exactly for a CLIENT half"), Case.Pairing), Body.Contains(TEXT("// from the platform")) == bClient);
		TestTrue(FString::Printf(TEXT("%s: the CLIENT starter is a crowdy-client-sdk cdylib, exactly for a CLIENT half"), Case.Pairing),
			(Body.Contains(TEXT("crowdy-client-sdk")) && Body.Contains(TEXT("cdylib")) && Body.Contains(TEXT("register_module!"))) == bClient);
		TestFalse(FString::Printf(TEXT("%s: a README the platform's project would refuse is dropped"), Case.Pairing), Body.Contains(TEXT("README.md")));
		TestTrue(FString::Printf(TEXT("%s: the SERVER module is the name's slug, exactly for a SERVER half"), Case.Pairing), Body.Contains(TEXT("\"serverModuleName\":\"my-spinner\"")) == bServer);
		TestTrue(FString::Printf(TEXT("%s: the CLIENT module is the slug plus -client, never a bare mod name, exactly for a CLIENT half"), Case.Pairing),
			Body.Contains(TEXT("\"clientModuleName\":\"my-spinner-client\"")) == bClient);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioProjectsClientStarterTest, "CrowdySDK.CrowdyServices.StudioProjects.CreateTakesTheClientStarterFromThePlatform", StudioFlags)
bool FCrowdyStudioProjectsClientStarterTest::RunTest(const FString& Parameters)
{
	const FString Created = StudioDataAnswer(TEXT("crowdyStudioProjectCreate"), StudioProjectJson(TEXT("p-new"), TEXT("CLIENT_ONLY"), false, StudioTwoFiles));
	{
		FStudioRig Rig(Created);
		TArray<FString> Sent;
		FCrowdyCppClient* Raw = Rig.Client.Get();
		Rig.Client->SetTestOnRequest([&Sent, Raw](const FString&)
		{
			FString Body;
			Raw->GetLastTestRequestBody(Body);
			Sent.Add(Body);
		});
		Rig.Script({{200, StudioClientStarterAnswer}, {200, Created}});
		Rig.CreateProject(TEXT("My Spinner"), ECrowdyStudioProjectKind::Client);
		Rig.Pump();
		Rig.Client->SetTestOnRequest(nullptr);
		const FString Body = Rig.SentBody();
		TestTrue(TEXT("the project is created"), Rig.Info.Result == ECrowdyStudioResult::Success);
		TestTrue(TEXT("the CLIENT starter is read, then the project created"), Sent.Num() == 2 && Sent[0].Contains(TEXT("ExecModClientStarter"))
			&& Sent[1].Contains(TEXT("CrowdyStudioProjectCreate")));
		TestTrue(TEXT("with the platform's CLIENT starter"), Body.Contains(TEXT("// from the platform")));
		TestFalse(TEXT("and not this SDK's copy"), Body.Contains(TEXT("tick_interval_ms")));
		TestTrue(TEXT("its package renamed for the CLIENT module, whichever host answered"), Body.Contains(TEXT("name = \\\"my-spinner-client\\\""))
			&& !Body.Contains(TEXT("client-half")));
		TestTrue(TEXT("and the rest of its manifest kept"), Body.Contains(TEXT("crate-type = [\\\"cdylib\\\"]")) && Body.Contains(TEXT("crowdy-client-sdk")));
	}
	{
		FStudioRig Rig(Created);
		Rig.Script({{200, StudioErrorAnswer(TEXT("GRAPHQL_VALIDATION_FAILED"))}, {200, Created}});
		Rig.CreateProject(TEXT("My Spinner"), ECrowdyStudioProjectKind::Client);
		Rig.Pump();
		TestTrue(TEXT("a Game API that does not know the CLIENT starter still creates the project"), Rig.Info.Result == ECrowdyStudioResult::Success);
		TestTrue(TEXT("from this SDK's copy"), Rig.SentBody().Contains(TEXT("tick_interval_ms")));
	}
	{
		FStudioRig Rig(Created);
		TArray<FString> Sent;
		FCrowdyCppClient* Raw = Rig.Client.Get();
		Rig.Client->SetTestOnRequest([&Sent, Raw](const FString&)
		{
			FString Body;
			Raw->GetLastTestRequestBody(Body);
			Sent.Add(Body);
		});
		Rig.Script({{200, StudioErrorAnswer(TEXT("FORBIDDEN"))}, {200, Created}});
		Rig.CreateProject(TEXT("My Spinner"), ECrowdyStudioProjectKind::Client);
		Rig.Pump();
		Rig.Client->SetTestOnRequest(nullptr);
		TestTrue(TEXT("a refused CLIENT starter fails the create as denied"), Rig.Info.Result == ECrowdyStudioResult::Denied && Rig.Info.Project.ProjectId.IsEmpty());
		TestEqual(TEXT("and no project is created"), Sent.Num(), 1);
	}
	{
		FStudioRig Rig(Created);
		Rig.Script({{502, TEXT("<html>gateway</html>")}, {200, Created}});
		Rig.CreateProject(TEXT("My Spinner"), ECrowdyStudioProjectKind::Client);
		Rig.Pump();
		TestTrue(TEXT("a CLIENT starter that did not arrive fails the create, not the offline copy"), Rig.Info.Result != ECrowdyStudioResult::Success
			&& !Rig.SentBody().Contains(TEXT("pairingPreference")));
	}
	{
		FStudioRig Rig(Created);
		Rig.Script({{503, StudioClientStarterUnavailable}, {200, Created}});
		Rig.CreateProject(TEXT("My Spinner"), ECrowdyStudioProjectKind::Client);
		Rig.Pump();
		const FString Body = Rig.SentBody();
		TestTrue(TEXT("a host that answers the CLIENT starter with 503 still creates the project"), Rig.Info.Result == ECrowdyStudioResult::Success);
		TestEqual(TEXT("from this SDK's copy of the CLIENT starter, both its files"), StudioCountOf(Body, TEXT("\"target\":\"CLIENT\"")), 2);
		TestTrue(TEXT("which is CrowdyCPP's, with its tick interval, serde_json and tick example"), Body.Contains(TEXT("tick_interval_ms")) && Body.Contains(TEXT("serde_json"))
			&& Body.Contains(TEXT("hud_set")));
		TestTrue(TEXT("its package named for the CLIENT module"), Body.Contains(TEXT("name = \\\"my-spinner-client\\\"")));
		TestFalse(TEXT("and none of the platform's"), Body.Contains(TEXT("// from the platform")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioProjectsCreateRefusalsTest, "CrowdySDK.CrowdyServices.StudioProjects.CreateRefusesBeforeSending", StudioFlags)
bool FCrowdyStudioProjectsCreateRefusalsTest::RunTest(const FString& Parameters)
{
	struct FBadCase
	{
		const TCHAR* Why;
		const TCHAR* Name;
		ECrowdyStudioProjectKind Kind;
		FString Server;
		FString Client;
	};
	const FBadCase Cases[] = {
		{TEXT("a module name with a space"), TEXT("spinner"), ECrowdyStudioProjectKind::Paired, TEXT("bad name"), TEXT("c")},
		{TEXT("an uppercase module name"), TEXT("spinner"), ECrowdyStudioProjectKind::Paired, TEXT("Spinner"), TEXT("c")},
		{TEXT("a client module name with a dot"), TEXT("spinner"), ECrowdyStudioProjectKind::Paired, TEXT("spinner"), TEXT("c.lient")},
		{TEXT("a module name past 48 characters"), TEXT("spinner"), ECrowdyStudioProjectKind::Server, FString::ChrN(49, TEXT('a')), TEXT("")},
		{TEXT("equal module names"), TEXT("spinner"), ECrowdyStudioProjectKind::Independent, TEXT("spinner"), TEXT("spinner")},
		{TEXT("a SERVER module on a client-only project"), TEXT("spinner"), ECrowdyStudioProjectKind::Client, TEXT("spinner"), TEXT("c")},
		{TEXT("a CLIENT module on a server-only project"), TEXT("spinner"), ECrowdyStudioProjectKind::Server, TEXT("s"), TEXT("spinner")},
		{TEXT("no SERVER module on a server-only project"), TEXT("spinner"), ECrowdyStudioProjectKind::Server, TEXT(""), TEXT("")},
		{TEXT("no CLIENT module on a client-only project"), TEXT("spinner"), ECrowdyStudioProjectKind::Client, TEXT(""), TEXT("")},
		{TEXT("no SERVER module on a paired project"), TEXT("spinner"), ECrowdyStudioProjectKind::Paired, TEXT(""), TEXT("c")},
		{TEXT("no CLIENT module on an independent project"), TEXT("spinner"), ECrowdyStudioProjectKind::Independent, TEXT("s"), TEXT("")},
		{TEXT("a blank name"), TEXT("  "), ECrowdyStudioProjectKind::Paired, TEXT("a"), TEXT("b")}};
	for (const FBadCase& Case : Cases)
	{
		FStudioRig Rig(StudioDataAnswer(TEXT("crowdyStudioProjectCreate"), StudioProjectJson(TEXT("p-new"), TEXT("PAIRED"))));
		Rig.CreateProject(Case.Name, Case.Kind, Case.Server, Case.Client);
		TestEqual(FString::Printf(TEXT("%s: nothing answers inside the call"), Case.Why), Rig.Answers, 0);
		Rig.Pump();
		TestTrue(FString::Printf(TEXT("%s is refused"), Case.Why), Rig.Info.Result == ECrowdyStudioResult::Refused && Rig.Info.Message.Contains(TEXT("not sent")));
		TestFalse(FString::Printf(TEXT("%s sends nothing, not even the starter read"), Case.Why), Rig.SentAnything());
	}
	for (const ECrowdyStudioProjectKind Kind : {ECrowdyStudioProjectKind::Server, ECrowdyStudioProjectKind::Client, ECrowdyStudioProjectKind::Paired, ECrowdyStudioProjectKind::Independent})
	{
		FStudioRig Rig(StudioDataAnswer(TEXT("crowdyStudioProjectCreate"), StudioProjectJson(TEXT("p-new"), TEXT("PAIRED"))));
		Rig.CreateProject(TEXT("éè !!"), Kind);
		Rig.Pump();
		TestTrue(TEXT("a name with no ASCII letter or digit is refused for every kind"), Rig.Info.Result == ECrowdyStudioResult::Refused && Rig.Info.Message.Contains(TEXT("not sent")));
		TestFalse(TEXT("and sends nothing"), Rig.SentAnything());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioProjectsCreateFailuresTest, "CrowdySDK.CrowdyServices.StudioProjects.CreateAnswersEachFailure", StudioFlags)
bool FCrowdyStudioProjectsCreateFailuresTest::RunTest(const FString& Parameters)
{
	const FString Created = StudioDataAnswer(TEXT("crowdyStudioProjectCreate"), StudioProjectJson(TEXT("p-new"), TEXT("PAIRED")));
	{
		FStudioRig Rig{FString()};
		Rig.Script({{200, StudioErrorAnswer(TEXT("FORBIDDEN"))}, {200, Created}});
		Rig.CreateProject(TEXT("spinner"), ECrowdyStudioProjectKind::Paired);
		Rig.Pump();
		TestTrue(TEXT("a refused server starter is denied and creates nothing"), Rig.Info.Result == ECrowdyStudioResult::Denied && Rig.Info.Project.ProjectId.IsEmpty()
			&& !Rig.SentBody().Contains(TEXT("pairingPreference")));
	}
	{
		FStudioRig Rig{FString()};
		Rig.Script({{200, TEXT("{\"data\":{\"execModStarter\":{\"crate\":\"x\"}}}")}, {200, Created}});
		Rig.CreateProject(TEXT("spinner"), ECrowdyStudioProjectKind::Server);
		Rig.Pump();
		TestTrue(TEXT("a malformed server starter is a bad reply and creates nothing"), Rig.Info.Result == ECrowdyStudioResult::BadReply && !Rig.SentBody().Contains(TEXT("pairingPreference")));
	}
	{
		FStudioRig Rig(StudioErrorAnswer(TEXT("NAME_TAKEN")));
		Rig.CreateProject(TEXT("spinner"), ECrowdyStudioProjectKind::Client);
		Rig.Pump();
		TestTrue(TEXT("the platform's refusal is refused and carries no project"), Rig.Info.Result == ECrowdyStudioResult::Refused && Rig.Info.Project.ProjectId.IsEmpty());
	}
	{
		FStudioRig Rig(StudioDataAnswer(TEXT("crowdyStudioProjectCreate"), TEXT("{\"projectId\":\"p-new\"}")));
		Rig.CreateProject(TEXT("spinner"), ECrowdyStudioProjectKind::Client);
		Rig.Pump();
		TestTrue(TEXT("a created project that is malformed is a bad reply"), Rig.Info.Result == ECrowdyStudioResult::BadReply && Rig.Info.Project.ProjectId.IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioProjectsSignedOutTest, "CrowdySDK.CrowdyServices.StudioProjects.NoTokenIsNotSignedInLater", StudioFlags)
bool FCrowdyStudioProjectsSignedOutTest::RunTest(const FString& Parameters)
{
	FStudioRig Rig(StudioListAnswer(StudioRows(1)), 200, false);
	Rig.ListProjects();
	FCrowdyStudioProjectInfo Got;
	FCrowdyStudioProjectInfo CreatedClient;
	FCrowdyStudioProjectInfo CreatedPaired;
	Rig.Projects->GetProject(TEXT("p-1"), [&](const FCrowdyStudioProjectInfo& Answer) { Got = Answer; ++Rig.Answers; });
	Rig.Projects->CreateProject(TEXT("spinner"), ECrowdyStudioProjectKind::Client, [&](const FCrowdyStudioProjectInfo& Answer) { CreatedClient = Answer; ++Rig.Answers; });
	Rig.Projects->CreateProject(TEXT("spinner"), ECrowdyStudioProjectKind::Paired, [&](const FCrowdyStudioProjectInfo& Answer) { CreatedPaired = Answer; ++Rig.Answers; });
	TestEqual(TEXT("nothing answers inside the calls"), Rig.Answers, 0);
	Rig.PumpUntil([&Rig]() { return Rig.Answers == 4; });
	TestEqual(TEXT("every call answers once"), Rig.Answers, 4);
	TestTrue(TEXT("a list is not signed in"), Rig.List.Result == ECrowdyStudioResult::NotSignedIn);
	TestTrue(TEXT("a read is not signed in"), Got.Result == ECrowdyStudioResult::NotSignedIn);
	TestTrue(TEXT("a client-only create is not signed in"), CreatedClient.Result == ECrowdyStudioResult::NotSignedIn);
	TestTrue(TEXT("a create that needs the server starter is not signed in"), CreatedPaired.Result == ECrowdyStudioResult::NotSignedIn);
	TestFalse(TEXT("and nothing was sent"), Rig.SentAnything());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioProjectsDeinitializeTest, "CrowdySDK.CrowdyServices.StudioProjects.ACallAfterDeinitializeAnswersNobody", StudioFlags)
bool FCrowdyStudioProjectsDeinitializeTest::RunTest(const FString& Parameters)
{
	FStudioRig Rig(StudioListAnswer(StudioRows(1)));
	Rig.ListProjects();
	Rig.Projects->Deinitialize();
	Rig.ListProjects();
	Rig.GetProject(TEXT("p-1"));
	Rig.CreateProject(TEXT("spinner"), ECrowdyStudioProjectKind::Client);
	Rig.CreateProject(TEXT("spinner"), ECrowdyStudioProjectKind::Paired);
	Rig.PumpUntil([]() { return false; }, 40);
	TestEqual(TEXT("no call answers once the subsystem has ended"), Rig.Answers, 0);
	return true;
}

#endif
