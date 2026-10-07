#include "CrowdyCppClient.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"

// The client's per-operation diagnostics: calls, failures, latency samples, the sdk and http split, and the
// transport's in-flight counts.
namespace CrowdyCppOpStatsTestSupport
{
	constexpr EAutomationTestFlags OpStatsTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	const TCHAR* const ProbeOperation = TEXT("Teams");

	FCrowdyCppRequestHandle IssueProbe(const TSharedPtr<FCrowdyCppClient>& Client)
	{
		return Client->RunOp(ECrowdyCppApiDomain::Teams, ProbeOperation, MakeShared<FJsonObject>(),
			[](FCrowdyCppJsonResult) {});
	}

	const FCrowdyCppOpStats* FindOp(const TArray<FCrowdyCppOpStats>& Ops, const FString& Operation)
	{
		return Ops.FindByPredicate([&Operation](const FCrowdyCppOpStats& Op) { return Op.Operation == Operation; });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppOpStatsClientOpStatsTest,
	"CrowdySDK.CrowdyCpp.OpStats.ClientOpStats", CrowdyCppOpStatsTestSupport::OpStatsTestFlags)
bool FCrowdyCppOpStatsClientOpStatsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCppOpStatsTestSupport;

	FCrowdyCppClientConfig Config;
	Config.ApiUrl = TEXT("https://game.test");
	Config.DiscoveryUrl = TEXT("https://api.test");
	const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(TEXT("{\"data\":{}}"), 200, Config);
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	IssueProbe(Client);
	Client->Poll();
	TArray<FCrowdyCppOpStats> Ops;
	FCrowdyCppTransportStats Transport;
	Client->GetStats(Ops, Transport);
	const FCrowdyCppOpStats* Probe = FindOp(Ops, ProbeOperation);
	if (!TestNotNull(TEXT("the operation is recorded under its name"), Probe))
	{
		return false;
	}
	TestEqual(TEXT("one completed call"), Probe->Calls, 1);
	TestEqual(TEXT("an answered call is not a failure"), Probe->Failures, 0);
	TestEqual(TEXT("one latency sample"), Probe->RecentLatenciesMs.Num(), 1);

	// Cancelled completions are failures, and the latency window stays bounded however many arrive.
	constexpr int32 Burst = FCrowdyCppClient::OpLatencySamples + 44;
	for (int32 Index = 0; Index < Burst; ++Index)
	{
		IssueProbe(Client);
	}
	Client->CancelAll();
	Client->GetStats(Ops, Transport);
	Probe = FindOp(Ops, ProbeOperation);
	if (!TestNotNull(TEXT("still recorded after the burst"), Probe))
	{
		return false;
	}
	TestEqual(TEXT("every completion counted"), Probe->Calls, Burst + 1);
	TestEqual(TEXT("every cancellation is a failure"), Probe->Failures, Burst);
	TestEqual(TEXT("the latency window is bounded"), Probe->RecentLatenciesMs.Num(), FCrowdyCppClient::OpLatencySamples);

	Client->ResetStats();
	Client->GetStats(Ops, Transport);
	TestEqual(TEXT("a reset leaves no operation with calls"), Ops.Num(), 0);
	TestEqual(TEXT("the canned transport counts no bytes"), Transport.ResponseBytes, static_cast<int64>(0));
	return true;
}

// A request handed to the HTTP module splits into an sdk and an http sample; one whose hand-off was never recorded
// adds to neither, rather than adding a zero that would read as "no wait".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppOpStatsHandOffSplitTest,
	"CrowdySDK.CrowdyCpp.OpStats.HandOffSplit", CrowdyCppOpStatsTestSupport::OpStatsTestFlags)
bool FCrowdyCppOpStatsHandOffSplitTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCppOpStatsTestSupport;

	const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(TEXT("{\"data\":{}}"), 200);
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	IssueProbe(Client);
	Client->Poll();
	TArray<FCrowdyCppOpStats> Ops;
	FCrowdyCppTransportStats Transport;
	Client->GetStats(Ops, Transport);
	const FCrowdyCppOpStats* Probe = FindOp(Ops, ProbeOperation);
	if (!TestNotNull(TEXT("the unstamped call is recorded"), Probe))
	{
		return false;
	}
	TestEqual(TEXT("its latency is sampled"), Probe->RecentLatenciesMs.Num(), 1);
	TestEqual(TEXT("an unstamped call has no sdk sample"), Probe->RecentSdkMs.Num(), 0);
	TestEqual(TEXT("nor an http sample"), Probe->RecentHttpMs.Num(), 0);

	// The wait before the completion is delivered comes after the hand-off, so it belongs to http, not to sdk.
	constexpr double DeliveryDelayMs = 30.0;
	Client->SetTestStampsHandOff(true);
	IssueProbe(Client);
	FPlatformProcess::Sleep(static_cast<float>(DeliveryDelayMs / 1000.0));
	Client->Poll();
	Client->GetStats(Ops, Transport);
	Probe = FindOp(Ops, ProbeOperation);
	if (!TestNotNull(TEXT("the stamped call is recorded"), Probe))
	{
		return false;
	}
	TestEqual(TEXT("both calls' latencies are sampled"), Probe->RecentLatenciesMs.Num(), 2);
	if (!TestEqual(TEXT("the stamped call has an sdk sample"), Probe->RecentSdkMs.Num(), 1)
		|| !TestEqual(TEXT("and an http sample"), Probe->RecentHttpMs.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("the sdk part is not negative"), Probe->RecentSdkMs[0] >= 0.0);
	// Half the delay on each side: a sleep on this platform can wake a timer tick early.
	TestTrue(TEXT("the sdk part ends before the delivery wait"), Probe->RecentSdkMs[0] < DeliveryDelayMs * 0.5);
	TestTrue(TEXT("the http part holds the delivery wait"), Probe->RecentHttpMs[0] >= DeliveryDelayMs * 0.5);

	Client->ResetStats();
	IssueProbe(Client);
	Client->Poll();
	Client->GetStats(Ops, Transport);
	Probe = FindOp(Ops, ProbeOperation);
	TestTrue(TEXT("a reset empties the split along with the latencies"),
		Probe && Probe->RecentSdkMs.Num() == 1 && Probe->RecentLatenciesMs.Num() == 1);
	return true;
}

// A canceled request's http time would end at the cancel rather than at an answer, so it records no split; its
// latency is still sampled.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppOpStatsCanceledSplitTest,
	"CrowdySDK.CrowdyCpp.OpStats.CanceledRequestHasNoSplit", CrowdyCppOpStatsTestSupport::OpStatsTestFlags)
bool FCrowdyCppOpStatsCanceledSplitTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCppOpStatsTestSupport;

	const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(TEXT("{\"data\":{}}"), 200);
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}
	Client->SetTestStampsHandOff(true);
	IssueProbe(Client);
	TestEqual(TEXT("one request was canceled"), Client->CancelAll(), 1);

	TArray<FCrowdyCppOpStats> Ops;
	FCrowdyCppTransportStats Transport;
	Client->GetStats(Ops, Transport);
	const FCrowdyCppOpStats* Probe = FindOp(Ops, ProbeOperation);
	if (!TestNotNull(TEXT("the canceled call is recorded"), Probe))
	{
		return false;
	}
	TestEqual(TEXT("its latency is sampled"), Probe->RecentLatenciesMs.Num(), 1);
	TestEqual(TEXT("but it has no sdk sample"), Probe->RecentSdkMs.Num(), 0);
	TestEqual(TEXT("nor an http sample"), Probe->RecentHttpMs.Num(), 0);
	return true;
}

// The transport counts the requests it holds open and the most it held at once since the last reset.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppOpStatsPeakInFlightTest,
	"CrowdySDK.CrowdyCpp.OpStats.PeakInFlight", CrowdyCppOpStatsTestSupport::OpStatsTestFlags)
bool FCrowdyCppOpStatsPeakInFlightTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCppOpStatsTestSupport;

	const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(TEXT("{\"data\":{}}"), 200);
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	// A second request issued while the first is still in the transport, the overlap a burst produces.
	bool bIssuedOverlap = false;
	FCrowdyCppClient* RawClient = Client.Get();
	Client->SetTestOnRequest([RawClient, &bIssuedOverlap](const FString&)
	{
		if (bIssuedOverlap)
		{
			return;
		}
		bIssuedOverlap = true;
		RawClient->RunOp(ECrowdyCppApiDomain::Teams, ProbeOperation, MakeShared<FJsonObject>(),
			[](FCrowdyCppJsonResult) {});
	});
	IssueProbe(Client);
	Client->SetTestOnRequest(nullptr);
	Client->Poll();

	TArray<FCrowdyCppOpStats> Ops;
	FCrowdyCppTransportStats Transport;
	Client->GetStats(Ops, Transport);
	TestTrue(TEXT("the overlapping request was issued"), bIssuedOverlap);
	TestEqual(TEXT("nothing is in flight once both answered"), Transport.InFlight, 0);
	TestEqual(TEXT("two were in flight at once"), Transport.PeakInFlight, 2);

	Client->ResetStats();
	Client->GetStats(Ops, Transport);
	TestEqual(TEXT("a reset restarts the peak from what is in flight now"), Transport.PeakInFlight, 0);

	IssueProbe(Client);
	Client->Poll();
	Client->GetStats(Ops, Transport);
	TestEqual(TEXT("one request alone peaks at one"), Transport.PeakInFlight, 1);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
