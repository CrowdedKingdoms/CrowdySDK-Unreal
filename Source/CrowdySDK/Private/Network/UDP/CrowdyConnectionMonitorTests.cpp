#include "Network/UDP/CrowdyConnectionMonitor.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/GameInstance.h"
#include "Misc/AutomationTest.h"
#include "Network/UDP/CrowdyUDPSubsystem.h"
#include "Subsystem/CrowdySDKSubsystem.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

// The monitor attaches to two delegates other code binds first: the SDK subsystem binds the UDP subsystem's
// success delegate during its own Initialize, and a game may bind OnUDPTimedOut for a HUD before it calls
// InitConnectionMonitor. Neither may stop the monitor's own handlers from attaching. The subsystems here are
// bare objects (never initialised); the monitor's handlers read no world when there is none.
namespace CrowdyConnectionMonitorTestSupport
{
	constexpr EAutomationTestFlags MonitorTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	struct FMonitorFixture
	{
		TStrongObjectPtr<UGameInstance> Instance;
		UCrowdySDKSubsystem* Sdk = nullptr;
		UCrowdyUDPSubsystem* Udp = nullptr;
		UCrowdyConnectionMonitor* Monitor = nullptr;

		FMonitorFixture()
			: Instance(NewObject<UGameInstance>(GetTransientPackage()))
		{
			Sdk = NewObject<UCrowdySDKSubsystem>(Instance.Get());
			Udp = NewObject<UCrowdyUDPSubsystem>(Instance.Get());
			Monitor = NewObject<UCrowdyConnectionMonitor>(Instance.Get());
		}
	};
}

// With the UDP subsystem's success delegate already bound (as the SDK subsystem leaves it), a timeout must take
// the monitor to Disconnected and the SDK's success broadcast must bring it back to Connected.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyConnectionMonitorReportsConnectedTest,
	"CrowdySDK.SDK.ConnectionMonitorReportsConnected",
	CrowdyConnectionMonitorTestSupport::MonitorTestFlags)

bool FCrowdyConnectionMonitorReportsConnectedTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyConnectionMonitorTestSupport;
	FMonitorFixture F;

	// Any bound function stands in for the SDK subsystem's handler; it is never fired here.
	F.Udp->OnUDPConnectionSuccessful.AddDynamic(F.Udp, &UCrowdyUDPSubsystem::ResetUDPNetworkStats);
	F.Monitor->AttachTo(F.Sdk, F.Udp);

	F.Sdk->OnUDPTimedOut.Broadcast();
	TestEqual(TEXT("a timeout takes the monitor to Disconnected"),
		static_cast<int32>(F.Monitor->GetReconnectState()), static_cast<int32>(ECrowdyReconnectState::Disconnected));

	F.Sdk->OnUDPConnectionSuccess.Broadcast();
	TestEqual(TEXT("the SDK's success broadcast takes the monitor back to Connected"),
		static_cast<int32>(F.Monitor->GetReconnectState()), static_cast<int32>(ECrowdyReconnectState::Connected));
	TestTrue(TEXT("the monitor's success handler is bound on the SDK subsystem"),
		F.Sdk->OnUDPConnectionSuccess.Contains(F.Monitor, FName(TEXT("OnUdpConnectionSuccess"))));
	return true;
}

// A game that bound OnUDPTimedOut first must not silence the monitor.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyConnectionMonitorAttachesBehindGameBindingTest,
	"CrowdySDK.SDK.ConnectionMonitorAttachesBehindGameBinding",
	CrowdyConnectionMonitorTestSupport::MonitorTestFlags)

bool FCrowdyConnectionMonitorAttachesBehindGameBindingTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyConnectionMonitorTestSupport;
	FMonitorFixture F;

	F.Sdk->OnUDPTimedOut.AddDynamic(F.Udp, &UCrowdyUDPSubsystem::ResetUDPNetworkStats);
	F.Monitor->AttachTo(F.Sdk, F.Udp);

	F.Sdk->OnUDPTimedOut.Broadcast();
	TestEqual(TEXT("the monitor still hears the timeout"),
		static_cast<int32>(F.Monitor->GetReconnectState()), static_cast<int32>(ECrowdyReconnectState::Disconnected));

	// Attaching twice must not double the handler.
	F.Monitor->AttachTo(F.Sdk, F.Udp);
	TestEqual(TEXT("a second attach leaves one game binding and one monitor binding"),
		F.Sdk->OnUDPTimedOut.GetAllObjects().Num(), 2);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
