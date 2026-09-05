#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Containers/Ticker.h"
#include "Engine/GameInstance.h"
#include "HAL/PlatformProcess.h"
#include "Messages/Actor/FActorUpdateRequestMessage.h"
#include "Messages/GameObjects/FGameEventRequest.h"
#include "Network/CrowdyCpp/CrowdyCppReplicationSubsystem.h"
#include "Network/UDP/CrowdyCppReplicationTestSupport.h"
#include "Network/UDP/CrowdyUDPSubsystem.h"
#include "Serialization/CrowdyWireParitySupport.h"
#include "UObject/StrongObjectPtr.h"

// These exercise the remap that feeds "stat crowdyudp" from the routed transport: the once-a-second sample
// UCrowdyCppReplicationSubsystem reports, and the per-send client-notify counter. What UCrowdyUDPSubsystem does
// with a sample once it has one is covered directly; what the connection's own cumulative counters look like
// across a reconnect is covered through a real connection restart, since the delta computation that guards
// against it is not itself production surface anything outside this module needs to call.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStatsSampleFeedsCountersTest,
	"CrowdySDK.Stats.RoutedTransportFeedsTheThroughputCounters",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyStatsSampleFeedsCountersTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UGameInstance> OuterInstance(NewObject<UGameInstance>(GetTransientPackage()));
	const TStrongObjectPtr<UCrowdyUDPSubsystem> Stats(NewObject<UCrowdyUDPSubsystem>(OuterInstance.Get()));
	if (!TestTrue(TEXT("a stats target was created"), OuterInstance.IsValid() && Stats.IsValid()))
	{
		return false;
	}

	// Bumped ahead of the sample, exactly like the parser does on every delivered message: the sample carries
	// no receive count of its own, since that counter already has its own correct writer.
	Stats->IncrementReceivedMessageCount();
	Stats->IncrementReceivedMessageCount();
	Stats->IncrementReceivedMessageCount();

	FCrowdyTransportSample Sample;
	Sample.BytesSent = 512;
	Sample.BytesReceived = 2048;
	Sample.DatagramsSent = 4;
	Sample.DatagramsReceived = 9;
	Sample.MessagesSent = 6;

	Stats->ReportTransportSample(Sample);

	const FUDPNetworkStatistics Result = Stats->GetUDPNetworkStats();
	TestEqual(TEXT("bytes sent came from the sample"), Result.BytesSent, Sample.BytesSent);
	TestEqual(TEXT("bytes received came from the sample"), Result.BytesReceived, Sample.BytesReceived);
	TestEqual(TEXT("datagrams sent came from the sample"), Result.DatagramsSent, Sample.DatagramsSent);
	TestEqual(TEXT("datagrams received came from the sample"), Result.DatagramsReceived, Sample.DatagramsReceived);
	TestEqual(TEXT("messages sent per second came from the sample"), Result.MessagesSentPerSecond,
		Sample.MessagesSent);
	TestEqual(TEXT("messages received per second is what was counted before the sample arrived"),
		Result.MessagesReceivedPerSecond, 3);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStatsResetClearsTotalsTest,
	"CrowdySDK.Stats.ResetClearsTheAccumulatedTotals",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyStatsResetClearsTotalsTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UGameInstance> OuterInstance(NewObject<UGameInstance>(GetTransientPackage()));
	const TStrongObjectPtr<UCrowdyUDPSubsystem> Stats(NewObject<UCrowdyUDPSubsystem>(OuterInstance.Get()));
	if (!TestTrue(TEXT("a stats target was created"), OuterInstance.IsValid() && Stats.IsValid()))
	{
		return false;
	}

	FCrowdyTransportSample First;
	First.MessagesSent = 5;
	Stats->ReportTransportSample(First);
	TestEqual(TEXT("the first sample accumulates"), Stats->GetUDPNetworkStats().TotalMessagesSent,
		static_cast<int64>(5));

	// A second report with nothing in between. An assignment here would read as 8 having never happened: this is
	// the case an assignment and an accumulation disagree on, since a zero-valued next sample would not.
	FCrowdyTransportSample Second;
	Second.MessagesSent = 3;
	Stats->ReportTransportSample(Second);
	TestEqual(TEXT("a second sample adds to the running total rather than replacing it"),
		Stats->GetUDPNetworkStats().TotalMessagesSent, static_cast<int64>(8));

	Stats->ResetUDPNetworkStats();
	TestEqual(TEXT("reset clears the total"), Stats->GetUDPNetworkStats().TotalMessagesSent, static_cast<int64>(0));

	FCrowdyTransportSample Third;
	Third.MessagesSent = 2;
	Stats->ReportTransportSample(Third);

	// The reset's effect has to still hold once the next report lands: what it cleared is the running total, not
	// merely whatever the report before it had contributed.
	TestEqual(TEXT("the total after reset starts again from zero rather than from what reset cleared"),
		Stats->GetUDPNetworkStats().TotalMessagesSent, static_cast<int64>(2));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStatsSnapshotRestartDoesNotGoNegativeTest,
	"CrowdySDK.Stats.ASnapshotThatRestartsDoesNotGoNegative",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyStatsSnapshotRestartDoesNotGoNegativeTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;

	// A connection that is made to carry some traffic before the restart, so the reading it leaves behind is
	// well above zero and a naive difference against the fresh connection below would be a large negative one.
	FConnectedFixture Rising;
	if (!TestTrue(TEXT("a first connection to a stand-in server came up"), Rising.Open()))
	{
		return false;
	}
	if (!TestTrue(TEXT("it primed the server with its address"), Rising.Prime()))
	{
		Rising.Shut();
		return false;
	}

	const TStrongObjectPtr<UGameInstance> OuterInstance(NewObject<UGameInstance>(GetTransientPackage()));
	const TStrongObjectPtr<UCrowdyCppReplicationSubsystem> Routing(
		NewObject<UCrowdyCppReplicationSubsystem>(OuterInstance.Get()));
	const TStrongObjectPtr<UCrowdyUDPSubsystem> Stats(NewObject<UCrowdyUDPSubsystem>(OuterInstance.Get()));
	if (!TestTrue(TEXT("the routing host and its stats target were created"),
		OuterInstance.IsValid() && Routing.IsValid() && Stats.IsValid()))
	{
		Rising.Shut();
		return false;
	}

	Routing->SetReceiveTarget(nullptr, nullptr, Stats.Get());
	Routing->SetConnection(Rising.Connection);

	// The first report has no earlier reading to compare against, so it always reports the whole current one.
	FTSTicker::GetCoreTicker().Tick(0.016f);
	const FUDPNetworkStatistics AfterRising = Stats->GetUDPNetworkStats();
	TestTrue(TEXT("nothing about the first report is negative"),
		AfterRising.BytesSent >= 0 && AfterRising.DatagramsSent >= 0);

	// A second stand-in server and a fresh connection to it, which is what a reconnect looks like from the
	// subsystem's side: the new connection's cumulative counters start again at zero, below the reading the
	// retiring one had already reached.
	FConnectedFixture Restarted;
	if (!TestTrue(TEXT("a second connection to a fresh stand-in server came up"), Restarted.Open()))
	{
		Routing->SetConnection(nullptr);
		Rising.Shut();
		return false;
	}

	Routing->SetConnection(Restarted.Connection);

	// The report is gated to once a second, so the next one has to wait for the same wall clock the production
	// code reads rather than for another tick of this test.
	FPlatformProcess::Sleep(1.1f);
	FTSTicker::GetCoreTicker().Tick(0.016f);

	const FUDPNetworkStatistics AfterRestart = Stats->GetUDPNetworkStats();
	TestTrue(TEXT("bytes sent did not go negative across the restart"), AfterRestart.BytesSent >= 0);
	TestTrue(TEXT("bytes received did not go negative across the restart"), AfterRestart.BytesReceived >= 0);
	TestTrue(TEXT("datagrams sent did not go negative across the restart"), AfterRestart.DatagramsSent >= 0);
	TestTrue(TEXT("datagrams received did not go negative across the restart"), AfterRestart.DatagramsReceived >= 0);
	TestTrue(TEXT("messages sent did not go negative across the restart"),
		AfterRestart.MessagesSentPerSecond >= 0);

	Routing->SetConnection(nullptr);
	Restarted.Shut();
	Rising.Shut();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStatsClientNotifySendsAreCountedTest,
	"CrowdySDK.Stats.ClientNotifySendsAreCounted",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyStatsClientNotifySendsAreCountedTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;

	FConnectedFixture Fixture;
	if (!TestTrue(TEXT("a connection to a local stand-in server came up"), Fixture.Open()))
	{
		return false;
	}

	const TStrongObjectPtr<UGameInstance> OuterInstance(NewObject<UGameInstance>(GetTransientPackage()));
	const TStrongObjectPtr<UCrowdyCppReplicationSubsystem> Routing(
		NewObject<UCrowdyCppReplicationSubsystem>(OuterInstance.Get()));
	const TStrongObjectPtr<UCrowdyUDPSubsystem> Stats(NewObject<UCrowdyUDPSubsystem>(OuterInstance.Get()));
	if (!TestTrue(TEXT("the routing host and its stats target were created"),
		OuterInstance.IsValid() && Routing.IsValid() && Stats.IsValid()))
	{
		Fixture.Shut();
		return false;
	}

	Routing->SetReceiveTarget(nullptr, nullptr, Stats.Get());
	Routing->SetConnection(Fixture.Connection);

	const int32 SentBefore = Stats->GetUDPNetworkStats().TotalClientNotifiesSent;

	FGameEventRequest Notify;
	Notify.AppID = 7;
	Notify.UUID = CrowdyWireParity::GoldenActorId();
	Notify.EventType = 1;
	Notify.StateBytes = {0x01};
	Notify.StateSize = Notify.StateBytes.Num();

	TestEqual(TEXT("a client event notification is sent"),
		static_cast<int32>(Routing->TrySendMessage(Notify)),
		static_cast<int32>(ECrowdyCppSendOutcome::Sent));

	FActorUpdateRequestMessage NotANotification;
	NotANotification.AppID = 7;
	NotANotification.ChunkX = 1;
	NotANotification.ChunkY = 2;
	NotANotification.ChunkZ = 3;
	NotANotification.UUID = CrowdyWireParity::GoldenActorId();
	NotANotification.ReplicationDistance = ECrowdyReplicationDistance::Five_Chunks;
	NotANotification.DecayRate = ECrowdyDecayRate::Linear_25;
	NotANotification.StateBytes = {0x02};
	NotANotification.StateSize = NotANotification.StateBytes.Num();

	TestEqual(TEXT("a message that is not a client event notification is also sent"),
		static_cast<int32>(Routing->TrySendMessage(NotANotification)),
		static_cast<int32>(ECrowdyCppSendOutcome::Sent));

	// Sends are counted off the game thread and published by the poll, so the count only reaches the stats target
	// once a report runs. The report is gated to once a second and an engine tick may already have spent this
	// subsystem's first ungated one, so give it until the gate can pass rather than assuming a single tick will do.
	const double Deadline = FPlatformTime::Seconds() + 3.0;
	int32 SentAfter = SentBefore;
	while (FPlatformTime::Seconds() < Deadline)
	{
		FTSTicker::GetCoreTicker().Tick(0.016f);
		SentAfter = Stats->GetUDPNetworkStats().TotalClientNotifiesSent;
		if (SentAfter != SentBefore)
		{
			break;
		}
		FPlatformProcess::Sleep(0.05f);
	}

	TestEqual(TEXT("only the client event notification moved the counter"), SentAfter - SentBefore, 1);

	Routing->SetConnection(nullptr);
	Fixture.Shut();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStatsReportIsARateNotATotalTest,
	"CrowdySDK.Stats.EachReportCoversOnlyTheTrafficSinceTheLastOne",
	CrowdyReplicationTestSupport::CrowdyReplicationTestFlags)

bool FCrowdyStatsReportIsARateNotATotalTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyReplicationTestSupport;

	// The connection's own counters are cumulative and the stat fields are per-second rates, so every report has to
	// subtract the previous reading. Two equally sized batches are what makes that visible: a report that forgot to
	// subtract, or forgot to store the reading it subtracted against, makes the second batch read larger than the
	// first even though the same number of messages went out. The connection emits nothing on its own between
	// batches, so the two windows are comparable.
	FConnectedFixture Fixture;
	if (!TestTrue(TEXT("a connection to a local stand-in server came up"), Fixture.Open()))
	{
		return false;
	}

	const TStrongObjectPtr<UGameInstance> OuterInstance(NewObject<UGameInstance>(GetTransientPackage()));
	const TStrongObjectPtr<UCrowdyCppReplicationSubsystem> Routing(
		NewObject<UCrowdyCppReplicationSubsystem>(OuterInstance.Get()));
	const TStrongObjectPtr<UCrowdyUDPSubsystem> Stats(NewObject<UCrowdyUDPSubsystem>(OuterInstance.Get()));
	if (!TestTrue(TEXT("the routing host and its stats target were created"),
		OuterInstance.IsValid() && Routing.IsValid() && Stats.IsValid()))
	{
		Fixture.Shut();
		return false;
	}

	Routing->SetReceiveTarget(nullptr, nullptr, Stats.Get());
	Routing->SetConnection(Fixture.Connection);

	auto SendOneBatch = [this, &Routing](const int32 Count)
	{
		for (int32 Index = 0; Index < Count; ++Index)
		{
			FActorUpdateRequestMessage Update;
			Update.AppID = 7;
			Update.ChunkX = 1;
			Update.ChunkY = 2;
			Update.ChunkZ = 3;
			Update.UUID = CrowdyWireParity::GoldenActorId();
			Update.ReplicationDistance = ECrowdyReplicationDistance::Five_Chunks;
			Update.DecayRate = ECrowdyDecayRate::Linear_25;
			Update.StateBytes = {static_cast<uint8>(Index)};
			Update.StateSize = Update.StateBytes.Num();

			TestEqual(TEXT("the batch message was accepted"),
				static_cast<int32>(Routing->TrySendMessage(Update)),
				static_cast<int32>(ECrowdyCppSendOutcome::Sent));
		}
	};

	// Establishes a reading to subtract against, so neither measured batch is the first report.
	FTSTicker::GetCoreTicker().Tick(0.016f);

	const int32 BatchSize = 3;

	SendOneBatch(BatchSize);
	FPlatformProcess::Sleep(1.1f);
	FTSTicker::GetCoreTicker().Tick(0.016f);
	const int32 FirstBatchRate = Stats->GetUDPNetworkStats().MessagesSentPerSecond;

	SendOneBatch(BatchSize);
	FPlatformProcess::Sleep(1.1f);
	FTSTicker::GetCoreTicker().Tick(0.016f);
	const int32 SecondBatchRate = Stats->GetUDPNetworkStats().MessagesSentPerSecond;

	TestEqual(TEXT("the first batch is reported as its own size"), FirstBatchRate, BatchSize);
	TestEqual(TEXT("the second batch is reported as its own size, not as the running total"),
		SecondBatchRate, FirstBatchRate);

	Routing->SetConnection(nullptr);
	Fixture.Shut();
	return true;
}

#endif
