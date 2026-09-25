#include "Replication/GameModel/CrowdyGameModelSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyCppClient.h"
#include "Misc/AutomationTest.h"
#include "Replication/GameModel/CrowdyGameModelTestSupport.h"
#include "UObject/Package.h"

// The public container list reads every page a type has, not only the server's default first page.
namespace CrowdyListContainersTestSupport
{
	constexpr EAutomationTestFlags ListContainersTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	const TCHAR* const ListTestEndpoint = TEXT("https://game.test");
	const TCHAR* const ListTestType = TEXT("ListTestType");

	// A gameModelContainers response carrying Rows containers, numbered from First.
	FString ContainersPage(int32 Rows, int32 First = 0)
	{
		FString Body = TEXT("{\"data\":{\"gameModelContainers\":[");
		for (int32 Index = 0; Index < Rows; ++Index)
		{
			Body += FString::Printf(TEXT("%s{\"containerId\":\"c%d\",\"typeName\":\"ListTestType\"}"),
				Index == 0 ? TEXT("") : TEXT(","), First + Index);
		}
		Body += TEXT("]}}");
		return Body;
	}

	struct FListOutcome
	{
		int32 Calls = 0;
		bool bOk = false;
		TArray<TSharedPtr<FJsonObject>> Rows;
	};

	UCrowdyGameModelSubsystem* MakeListTestModel()
	{
		UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
		Model->BeginWorldSessionForTest();
		Model->SetApiContextForTest(ListTestEndpoint, TEXT("test-token"), 42);
		return Model;
	}

	// Lists ListTestType and polls until the list answers, recording each request's offset and bearer on the way.
	// OnFirstSent runs once the first page has gone out, before its answer is delivered.
	TSharedRef<FListOutcome> RunList(UCrowdyGameModelSubsystem* Model, FCrowdyGameModelTestClientHost& ClientHost,
		TArray<int32>& OutOffsets, TArray<FString>* OutAuthorizations = nullptr, TFunction<void()> OnFirstSent = nullptr)
	{
		FCrowdyCppClient* Client = ClientHost.Client.Get();
		Client->SetTestOnRequest([Client, &OutOffsets, OutAuthorizations, OnFirstSent](const FString&)
		{
			FString Body;
			Client->GetLastTestRequestBody(Body);
			const TSharedPtr<FJsonObject> Request = ParseObject(Body);
			const TSharedPtr<FJsonObject>* Variables = nullptr;
			const bool bHasVariables = Request.IsValid() && Request->TryGetObjectField(TEXT("variables"), Variables);
			OutOffsets.Add(bHasVariables ? static_cast<int32>((*Variables)->GetNumberField(TEXT("offset"))) : -1);
			FString Url, Authorization;
			Client->GetLastTestRequest(Url, Authorization);
			if (OutAuthorizations)
			{
				OutAuthorizations->Add(Authorization);
			}
			if (OnFirstSent && OutOffsets.Num() == 1)
			{
				OnFirstSent();
			}
		});

		const TSharedRef<FListOutcome> Outcome = MakeShared<FListOutcome>();
		Model->ListContainers(ListTestType, FString(), [Outcome](bool bOk, TArray<TSharedPtr<FJsonObject>> Rows)
		{
			++Outcome->Calls;
			Outcome->bOk = bOk;
			Outcome->Rows = MoveTemp(Rows);
		});
		ClientHost.PollUntil([&Outcome]() { return Outcome->Calls > 0; });
		Client->SetTestOnRequest(nullptr);
		return Outcome;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyListContainersShortPageTest,
	"CrowdySDK.GameModel.ListContainers.ShortPageEndsTheList", CrowdyListContainersTestSupport::ListContainersTestFlags)
bool FCrowdyListContainersShortPageTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyListContainersTestSupport;

	UCrowdyGameModelSubsystem* Model = MakeListTestModel();
	FCrowdyGameModelTestClientHost ClientHost(Model, ListTestEndpoint, ContainersPage(3));

	TArray<int32> Offsets;
	const TSharedRef<FListOutcome> Outcome = RunList(Model, ClientHost, Offsets);
	TestEqual(TEXT("the list answered exactly once"), Outcome->Calls, 1);
	TestTrue(TEXT("and succeeded"), Outcome->bOk);
	TestEqual(TEXT("with every row of the page"), Outcome->Rows.Num(), 3);
	if (TestEqual(TEXT("a page shorter than the page size is the last one asked for"), Offsets.Num(), 1))
	{
		TestEqual(TEXT("and it starts at the beginning"), Offsets[0], 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyListContainersFullPageTest,
	"CrowdySDK.GameModel.ListContainers.FullPageReadsTheNext", CrowdyListContainersTestSupport::ListContainersTestFlags)
bool FCrowdyListContainersFullPageTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyListContainersTestSupport;

	constexpr int32 PageSize = UCrowdyGameModelSubsystem::BulkResolvePageSize;
	UCrowdyGameModelSubsystem* Model = MakeListTestModel();
	FCrowdyGameModelTestClientHost ClientHost(Model, ListTestEndpoint);
	ClientHost.Client->SetTestResponseScript({
		TPair<int32, FString>(200, ContainersPage(PageSize)),
		TPair<int32, FString>(200, ContainersPage(2, PageSize)),
	});

	TArray<int32> Offsets;
	const TSharedRef<FListOutcome> Outcome = RunList(Model, ClientHost, Offsets);
	TestEqual(TEXT("the list answered exactly once"), Outcome->Calls, 1);
	TestTrue(TEXT("and succeeded"), Outcome->bOk);
	TestEqual(TEXT("with the rows of both pages"), Outcome->Rows.Num(), PageSize + 2);
	if (TestEqual(TEXT("a full page is followed by one more request"), Offsets.Num(), 2))
	{
		TestEqual(TEXT("the first page starts at the beginning"), Offsets[0], 0);
		TestEqual(TEXT("the second starts where the first ended"), Offsets[1], PageSize);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyListContainersFailedPageTest,
	"CrowdySDK.GameModel.ListContainers.FailedPageFailsTheList", CrowdyListContainersTestSupport::ListContainersTestFlags)
bool FCrowdyListContainersFailedPageTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyListContainersTestSupport;

	constexpr int32 PageSize = UCrowdyGameModelSubsystem::BulkResolvePageSize;
	UCrowdyGameModelSubsystem* Model = MakeListTestModel();
	FCrowdyGameModelTestClientHost ClientHost(Model, ListTestEndpoint);
	ClientHost.Client->SetTestResponseScript({
		TPair<int32, FString>(200, ContainersPage(PageSize)),
		TPair<int32, FString>(500, TEXT("{\"errors\":[{\"message\":\"page failed\"}]}")),
	});

	TArray<int32> Offsets;
	const TSharedRef<FListOutcome> Outcome = RunList(Model, ClientHost, Offsets);
	TestEqual(TEXT("both pages were asked for"), Offsets.Num(), 2);
	TestEqual(TEXT("the list answered exactly once"), Outcome->Calls, 1);
	TestFalse(TEXT("a failed page fails the list"), Outcome->bOk);
	TestEqual(TEXT("and hands back none of the rows read before it"), Outcome->Rows.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyListContainersPageCapTest,
	"CrowdySDK.GameModel.ListContainers.StopsAtThePageCap", CrowdyListContainersTestSupport::ListContainersTestFlags)
bool FCrowdyListContainersPageCapTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyListContainersTestSupport;

	constexpr int32 PageSize = UCrowdyGameModelSubsystem::BulkResolvePageSize;
	constexpr int32 MaxPages = UCrowdyGameModelSubsystem::BulkResolveMaxPagesPerType;
	AddExpectedMessagePlain(TEXT("any rows past them were not read"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);

	// Every page is full of rows no earlier page held, so only the cap ends the list.
	UCrowdyGameModelSubsystem* Model = MakeListTestModel();
	FCrowdyGameModelTestClientHost ClientHost(Model, ListTestEndpoint, ContainersPage(PageSize, MaxPages * PageSize));
	TArray<TPair<int32, FString>> Pages;
	for (int32 Page = 0; Page < MaxPages; ++Page)
	{
		Pages.Emplace(200, ContainersPage(PageSize, Page * PageSize));
	}
	ClientHost.Client->SetTestResponseScript(MoveTemp(Pages));

	TArray<int32> Offsets;
	const TSharedRef<FListOutcome> Outcome = RunList(Model, ClientHost, Offsets);
	TestEqual(TEXT("the list answered exactly once"), Outcome->Calls, 1);
	TestTrue(TEXT("a capped list still succeeds"), Outcome->bOk);
	TestEqual(TEXT("no more pages are read than the cap"), Offsets.Num(), MaxPages);
	TestEqual(TEXT("with every row those pages held"), Outcome->Rows.Num(), PageSize * MaxPages);
	return true;
}

// A token re-minted while the list is out is the one the next page carries; a stale bearer reinstalled on the shared
// client would otherwise ride every later page.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyListContainersFreshContextTest,
	"CrowdySDK.GameModel.ListContainers.EachPageUsesTheCurrentToken", CrowdyListContainersTestSupport::ListContainersTestFlags)
bool FCrowdyListContainersFreshContextTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyListContainersTestSupport;

	constexpr int32 PageSize = UCrowdyGameModelSubsystem::BulkResolvePageSize;
	UCrowdyGameModelSubsystem* Model = MakeListTestModel();
	Model->SetApiContextForTest(ListTestEndpoint, TEXT("first-token"), 42);
	FCrowdyGameModelTestClientHost ClientHost(Model, ListTestEndpoint);
	ClientHost.Client->SetTestResponseScript({
		TPair<int32, FString>(200, ContainersPage(PageSize)),
		TPair<int32, FString>(200, ContainersPage(1, PageSize)),
	});

	TArray<int32> Offsets;
	TArray<FString> Authorizations;
	const TSharedRef<FListOutcome> Outcome = RunList(Model, ClientHost, Offsets, &Authorizations,
		[Model]() { Model->SetApiContextForTest(ListTestEndpoint, TEXT("second-token"), 42); });
	TestTrue(TEXT("the list succeeded"), Outcome->bOk);
	if (!TestEqual(TEXT("two pages were asked for"), Authorizations.Num(), 2))
	{
		return false;
	}
	TestTrue(TEXT("the first page carried the token current when it went out"), Authorizations[0].Contains(TEXT("first-token")));
	TestTrue(TEXT("the second page carried the re-minted token"), Authorizations[1].Contains(TEXT("second-token")));
	return true;
}

// A server that ignores the limit cannot make one page count for more than a page, and the oversized page still
// reads as full, so the list goes on to the next.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyListContainersOversizedPageTest,
	"CrowdySDK.GameModel.ListContainers.OversizedPageIsCut", CrowdyListContainersTestSupport::ListContainersTestFlags)
bool FCrowdyListContainersOversizedPageTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyListContainersTestSupport;

	constexpr int32 PageSize = UCrowdyGameModelSubsystem::BulkResolvePageSize;
	UCrowdyGameModelSubsystem* Model = MakeListTestModel();
	FCrowdyGameModelTestClientHost ClientHost(Model, ListTestEndpoint);
	ClientHost.Client->SetTestResponseScript({
		TPair<int32, FString>(200, ContainersPage(PageSize + 200)),
		TPair<int32, FString>(200, ContainersPage(3, PageSize + 200)),
	});

	TArray<int32> Offsets;
	const TSharedRef<FListOutcome> Outcome = RunList(Model, ClientHost, Offsets);
	TestTrue(TEXT("the list succeeded"), Outcome->bOk);
	TestEqual(TEXT("the oversized page counted as full, so the next was asked for"), Offsets.Num(), 2);
	TestEqual(TEXT("and held no more than a page's worth"), Outcome->Rows.Num(), PageSize + 3);
	return true;
}

// A row returned by two pages, as a row landing at the boundary between two reads can be, is listed once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyListContainersDuplicateRowTest,
	"CrowdySDK.GameModel.ListContainers.RepeatedRowListedOnce", CrowdyListContainersTestSupport::ListContainersTestFlags)
bool FCrowdyListContainersDuplicateRowTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyListContainersTestSupport;

	constexpr int32 PageSize = UCrowdyGameModelSubsystem::BulkResolvePageSize;
	UCrowdyGameModelSubsystem* Model = MakeListTestModel();
	FCrowdyGameModelTestClientHost ClientHost(Model, ListTestEndpoint);
	// The second page starts with the last row of the first.
	ClientHost.Client->SetTestResponseScript({
		TPair<int32, FString>(200, ContainersPage(PageSize)),
		TPair<int32, FString>(200, ContainersPage(2, PageSize - 1)),
	});

	TArray<int32> Offsets;
	const TSharedRef<FListOutcome> Outcome = RunList(Model, ClientHost, Offsets);
	TestTrue(TEXT("the list succeeded"), Outcome->bOk);
	TestEqual(TEXT("the repeated row is listed once"), Outcome->Rows.Num(), PageSize + 1);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
