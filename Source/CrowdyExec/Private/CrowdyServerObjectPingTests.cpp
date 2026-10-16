#include "CrowdyServerObjectPingTestTypes.h"
#include "CrowdyServerObjectTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/TaskGraphInterfaces.h"
#include "CrowdyCppClient.h"
#include "CrowdyNativeExec.h"
#include "CrowdyServerObject.h"
#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectLibrary.h"
#include "CrowdyServerObjectLink.h"
#include "CrowdyServerObjectSubsystem.h"
#include "CrowdyServerObjectTestRig.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

namespace CrowdyServerObjectPingTests
{
	constexpr uint8 PingTag = 0x04;
	constexpr uint8 PongTag = 0x84;
	const FString NoConnectionReason = TEXT("No Server Object connection is open");
	const FString ClosedReason = TEXT("The Server Object connection closed before the platform answered");
	const FString TooManyReason = TEXT("Too many pings: the platform answers at most 60 every 10 seconds");

	struct FPingResult
	{
		bool bOk = false;
		float RoundTripMs = 0.f;
		FString Reason;
	};

	struct FPingSpy
	{
		TSharedRef<TArray<FPingResult>> Results = MakeShared<TArray<FPingResult>>();

		TFunction<void(bool, float, const FString&)> OnDone() const
		{
			TSharedRef<TArray<FPingResult>> Out = Results;
			return [Out](bool bOk, float RoundTripMs, const FString& Reason)
			{
				FPingResult& Result = Out->AddDefaulted_GetRef();
				Result.bOk = bOk;
				Result.RoundTripMs = RoundTripMs;
				Result.Reason = Reason;
			};
		}

		int32 Num() const { return Results->Num(); }
		const FPingResult& First() const { return (*Results)[0]; }
	};

	/** Restores the Server Object timeouts a test changed on the settings. */
	struct FTimeoutSettingsGuard
	{
		UCrowdySDKDeveloperSettings* Settings = GetMutableDefault<UCrowdySDKDeveloperSettings>();
		const float CallSeconds = Settings->ServerObjectCallTimeoutSeconds;
		const float OpenSeconds = Settings->ServerObjectOpenTimeoutSeconds;

		~FTimeoutSettingsGuard()
		{
			Settings->ServerObjectCallTimeoutSeconds = CallSeconds;
			Settings->ServerObjectOpenTimeoutSeconds = OpenSeconds;
		}
	};

	/** The rig's pump, except that the ping frames it would drop are kept, by nonce. */
	inline void PumpWithPings(CrowdyServerObjectTests::FRig& Rig, TArray<uint32>& Nonces)
	{
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		Rig.Client->Poll();
		for (const TArray<uint8>& Bytes : Rig.Client->TakeTestExecSentFrames())
		{
			if (Bytes.Num() == 5 && Bytes[0] == PingTag)
			{
				Nonces.Add(Bytes[1] | (Bytes[2] << 8) | (Bytes[3] << 16) | (static_cast<uint32>(Bytes[4]) << 24));
				continue;
			}
			CrowdyServerObjectTests::FSentFrame Frame;
			if (CrowdyServerObjectTests::ParseFrame(Bytes, Frame))
			{
				Rig.Sent.Add(MoveTemp(Frame));
			}
		}
	}

	inline bool PumpPingsUntil(CrowdyServerObjectTests::FRig& Rig, TArray<uint32>& Nonces, TFunctionRef<bool()> Done, double Seconds = 3.0)
	{
		const double Until = FPlatformTime::Seconds() + Seconds;
		for (;;)
		{
			PumpWithPings(Rig, Nonces);
			if (Done())
			{
				return true;
			}
			if (FPlatformTime::Seconds() > Until)
			{
				return false;
			}
			FPlatformProcess::Sleep(0.005f);
		}
	}

	inline TArray<uint8> PongFrame(uint32 Nonce)
	{
		return {PongTag, static_cast<uint8>(Nonce), static_cast<uint8>(Nonce >> 8), static_cast<uint8>(Nonce >> 16), static_cast<uint8>(Nonce >> 24)};
	}

	/** A Ready object on an open connection, for the tests that ping over it. */
	inline UCrowdyServerObject* AcquireReadyObject(CrowdyServerObjectTests::FRig& Rig, const UObject* Owner)
	{
		return Rig.AcquireReady(Owner, CrowdyServerObjectTests::Boss(3000, false, ECrowdyExecTestPhase::Calm), 5);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectPingWithoutAConnectionTest,
	"CrowdySDK.CrowdyExec.ServerObjectPing.WithoutAConnectionFailsBeforeReturningAndNeverDials", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectPingWithoutAConnectionTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectPingTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	FPingSpy Idle;
	Rig.Subsystem->Ping(Idle.OnDone());
	if (!TestEqual(TEXT("with no Server Object a ping is answered before Ping returns"), Idle.Num(), 1))
	{
		return false;
	}
	TestFalse(TEXT("as a failure"), Idle.First().bOk);
	TestEqualSensitive(TEXT("saying no connection is open"), Idle.First().Reason, NoConnectionReason);
	TestEqual(TEXT("with no round trip"), Idle.First().RoundTripMs, 0.f);
	Rig.PumpFor(0.3);
	TestEqual(TEXT("and it does not dial"), DialsOf(Rig), 0);

	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	if (!Rig.Acquire(Rig.InstanceId, Owner.Get()) || !TestTrue(TEXT("an object dials the gateway"), Rig.PumpUntil([&Rig]() { return DialsOf(Rig) >= 1; })))
	{
		return false;
	}
	FPingSpy Dialing;
	Rig.Subsystem->Ping(Dialing.OnDone());
	TestTrue(TEXT("a connection still opening is not open"), Dialing.Num() == 1 && !Dialing.First().bOk && Dialing.First().Reason.Equals(NoConnectionReason, ESearchCase::CaseSensitive));
	Rig.PumpFor(0.3);
	TestEqual(TEXT("and the ping does not dial again"), DialsOf(Rig), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectPingAnsweredTest,
	"CrowdySDK.CrowdyExec.ServerObjectPing.AnAnsweredPingReportsItsRoundTripOnce", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectPingAnsweredTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectPingTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	if (!AcquireReadyObject(Rig, Owner.Get()))
	{
		return false;
	}
	FPingSpy Spy;
	TArray<uint32> Nonces;
	Rig.Subsystem->Ping(Spy.OnDone());
	if (!TestTrue(TEXT("the ping goes out as one frame"), PumpPingsUntil(Rig, Nonces, [&Nonces]() { return Nonces.Num() >= 1; })))
	{
		return false;
	}
	TestEqual(TEXT("it is not answered before the platform answers"), Spy.Num(), 0);

	FPlatformProcess::Sleep(0.06f);
	Rig.Client->TestExecReceiveBinary(PongFrame(Nonces[0]));
	if (!TestTrue(TEXT("a pong answers it"), PumpPingsUntil(Rig, Nonces, [&Spy]() { return Spy.Num() >= 1; })))
	{
		return false;
	}
	TestTrue(TEXT("as a success"), Spy.First().bOk);
	TestTrue(FString::Printf(TEXT("the round trip covers the wait (%f ms)"), Spy.First().RoundTripMs), Spy.First().RoundTripMs >= 50.f && Spy.First().RoundTripMs < 5000.f);
	TestTrue(TEXT("with no reason"), Spy.First().Reason.IsEmpty());

	Rig.Client->TestExecReceiveBinary(PongFrame(Nonces[0]));
	PumpPingsUntil(Rig, Nonces, []() { return false; }, 0.2);
	TestEqual(TEXT("a second pong for it changes nothing"), Spy.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectPingRefusedTest,
	"CrowdySDK.CrowdyExec.ServerObjectPing.ARefusedPingFailsWithTheMessage", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectPingRefusedTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectPingTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	if (!AcquireReadyObject(Rig, Owner.Get()))
	{
		return false;
	}
	FPingSpy Spy;
	TArray<uint32> Nonces;
	Rig.Subsystem->Ping(Spy.OnDone());
	if (!TestTrue(TEXT("the ping goes out"), PumpPingsUntil(Rig, Nonces, [&Nonces]() { return Nonces.Num() >= 1; })))
	{
		return false;
	}
	Rig.Reply(Nonces[0], ECrowdyNativeExecStatus::Denied, Utf8Bytes(TEXT("ping denied")));
	if (!TestTrue(TEXT("a refusal answers it"), PumpPingsUntil(Rig, Nonces, [&Spy]() { return Spy.Num() >= 1; })))
	{
		return false;
	}
	TestFalse(TEXT("as a failure"), Spy.First().bOk);
	TestEqualSensitive(TEXT("with the platform's message"), Spy.First().Reason, FString(TEXT("ping denied")));
	TestEqual(TEXT("and no round trip"), Spy.First().RoundTripMs, 0.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectDialTimeoutsTest,
	"CrowdySDK.CrowdyExec.ServerObjectConnectionSettings.TheDialCarriesBothTimeouts", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectDialTimeoutsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectPingTests;
	FTimeoutSettingsGuard Guard;
	Guard.Settings->ServerObjectCallTimeoutSeconds = 12.5f;
	Guard.Settings->ServerObjectOpenTimeoutSeconds = 3.25f;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	if (!AcquireReadyObject(Rig, Owner.Get()))
	{
		return false;
	}
	const TPair<int32, int32> Dialed = Rig.Subsystem->GetDialTimeoutsForTest();
	TestEqual(TEXT("the connection dials with the project's call timeout"), Dialed.Key, 12500);
	TestEqual(TEXT("and its open timeout"), Dialed.Value, 3250);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectPingRateTest,
	"CrowdySDK.CrowdyExec.ServerObjectPing.TheSixtyFirstPingInTenSecondsIsRefusedAndTheWindowSlides", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectPingRateTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectPingTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	if (!AcquireReadyObject(Rig, Owner.Get()))
	{
		return false;
	}
	FPingSpy Waiting;
	TArray<uint32> Nonces;
	for (int32 Index = 0; Index < UCrowdyServerObjectSubsystem::MaxPingsPerWindow; ++Index)
	{
		Rig.Subsystem->Ping(Waiting.OnDone());
	}
	TestEqual(TEXT("sixty pings wait for their answers"), Waiting.Num(), 0);

	FPingSpy Over;
	Rig.Subsystem->Ping(Over.OnDone());
	if (!TestEqual(TEXT("the sixty-first is answered before Ping returns"), Over.Num(), 1))
	{
		return false;
	}
	TestFalse(TEXT("as a failure"), Over.First().bOk);
	TestEqualSensitive(TEXT("saying how many the platform answers"), Over.First().Reason, TooManyReason);
	PumpPingsUntil(Rig, Nonces, [&Nonces]() { return Nonces.Num() > UCrowdyServerObjectSubsystem::MaxPingsPerWindow; }, 0.5);
	TestEqual(TEXT("and only sixty went out"), Nonces.Num(), UCrowdyServerObjectSubsystem::MaxPingsPerWindow);

	Rig.Tick(UCrowdyServerObjectSubsystem::PingWindowSeconds);
	FPingSpy Edge;
	Rig.Subsystem->Ping(Edge.OnDone());
	TestTrue(TEXT("at the window's edge the platform may still count them, so a ping is refused"), Edge.Num() == 1 && Edge.First().Reason.Equals(TooManyReason, ESearchCase::CaseSensitive));
	Rig.Tick(UCrowdyServerObjectSubsystem::PingWindowMarginSeconds + 0.5f);
	FPingSpy Later;
	Rig.Subsystem->Ping(Later.OnDone());
	TestEqual(TEXT("after the window has passed a ping waits again"), Later.Num(), 0);
	PumpPingsUntil(Rig, Nonces, [&Nonces]() { return Nonces.Num() > UCrowdyServerObjectSubsystem::MaxPingsPerWindow; });
	TestEqual(TEXT("and goes out"), Nonces.Num(), UCrowdyServerObjectSubsystem::MaxPingsPerWindow + 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectPingClosedTest,
	"CrowdySDK.CrowdyExec.ServerObjectPing.AConnectionClosingUnderAPingReportsOnTheNextTick", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectPingClosedTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectPingTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	if (!AcquireReadyObject(Rig, Owner.Get()))
	{
		return false;
	}
	FPingSpy Spy;
	TArray<uint32> Nonces;
	Rig.Subsystem->Ping(Spy.OnDone());
	if (!TestTrue(TEXT("the ping goes out"), PumpPingsUntil(Rig, Nonces, [&Nonces]() { return Nonces.Num() >= 1; })))
	{
		return false;
	}
	Rig.Subsystem->DropConnectionForTest();
	TestEqual(TEXT("no caller code runs inside the close"), Spy.Num(), 0);
	Rig.Subsystem->TickForTest(0.1f);
	if (!TestEqual(TEXT("the next tick reports it"), Spy.Num(), 1))
	{
		return false;
	}
	TestFalse(TEXT("as a failure"), Spy.First().bOk);
	TestEqualSensitive(TEXT("saying the connection closed"), Spy.First().Reason, ClosedReason);

	FPingSpy After;
	Rig.Subsystem->Ping(After.OnDone());
	TestTrue(TEXT("a ping with the connection gone fails at once"), After.Num() == 1 && After.First().Reason.Equals(NoConnectionReason, ESearchCase::CaseSensitive));
	Rig.Subsystem->TickForTest(0.1f);
	TestEqual(TEXT("and each ping reports once"), Spy.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectPingActionTest,
	"CrowdySDK.CrowdyExec.ServerObjectPing.TheBlueprintNodeFiresOnSuccessOrOnFailedOnce", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectPingActionTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectPingTests;
	TStrongObjectPtr<UCrowdyServerPingTestListener> Lost(NewObject<UCrowdyServerPingTestListener>(GetTransientPackage(), NAME_None, RF_Transient));
	TStrongObjectPtr<UCrowdyServerPingAction> NoWorld(UCrowdyServerPingAction::PingServerObjects(nullptr));
	if (!TestNotNull(TEXT("the node makes its action"), NoWorld.Get()))
	{
		return false;
	}
	NoWorld->OnSuccess.AddDynamic(Lost.Get(), &UCrowdyServerPingTestListener::HandleSuccess);
	NoWorld->OnFailed.AddDynamic(Lost.Get(), &UCrowdyServerPingTestListener::HandleFailed);
	NoWorld->Activate();
	TestEqual(TEXT("without a world On Failed runs once"), Lost->FailedReasons.Num(), 1);
	TestEqual(TEXT("and On Success never"), Lost->SuccessReasons.Num(), 0);

	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerPingTestListener> Closed(NewObject<UCrowdyServerPingTestListener>(GetTransientPackage(), NAME_None, RF_Transient));
	TStrongObjectPtr<UCrowdyServerPingAction> NoConnection(UCrowdyServerPingAction::PingServerObjects(nullptr));
	NoConnection->SetSubsystemForTest(Rig.Subsystem.Get());
	NoConnection->OnSuccess.AddDynamic(Closed.Get(), &UCrowdyServerPingTestListener::HandleSuccess);
	NoConnection->OnFailed.AddDynamic(Closed.Get(), &UCrowdyServerPingTestListener::HandleFailed);
	NoConnection->Activate();
	TestTrue(TEXT("with no connection On Failed carries the reason and no round trip"),
		Closed->FailedReasons.Num() == 1 && Closed->FailedReasons[0].Equals(NoConnectionReason, ESearchCase::CaseSensitive) && Closed->FailedRoundTrips[0] == 0.f);
	TestEqual(TEXT("and On Success never runs"), Closed->SuccessReasons.Num(), 0);

	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	if (!AcquireReadyObject(Rig, Owner.Get()))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerPingTestListener> Answered(NewObject<UCrowdyServerPingTestListener>(GetTransientPackage(), NAME_None, RF_Transient));
	TStrongObjectPtr<UCrowdyServerPingAction> Action(UCrowdyServerPingAction::PingServerObjects(nullptr));
	Action->SetSubsystemForTest(Rig.Subsystem.Get());
	Action->OnSuccess.AddDynamic(Answered.Get(), &UCrowdyServerPingTestListener::HandleSuccess);
	Action->OnFailed.AddDynamic(Answered.Get(), &UCrowdyServerPingTestListener::HandleFailed);
	Action->Activate();
	TArray<uint32> Nonces;
	if (!TestTrue(TEXT("the node's ping goes out"), PumpPingsUntil(Rig, Nonces, [&Nonces]() { return Nonces.Num() >= 1; })))
	{
		return false;
	}
	FPlatformProcess::Sleep(0.03f);
	Rig.Client->TestExecReceiveBinary(PongFrame(Nonces[0]));
	if (!TestTrue(TEXT("a pong runs On Success"), PumpPingsUntil(Rig, Nonces, [&Answered]() { return Answered->SuccessRoundTrips.Num() >= 1; })))
	{
		return false;
	}
	TestTrue(TEXT("with the measured round trip and no reason"), Answered->SuccessRoundTrips[0] >= 25.f && Answered->SuccessReasons[0].IsEmpty());
	TestEqual(TEXT("and On Failed never runs"), Answered->FailedReasons.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectConnectionSettingsTest,
	"CrowdySDK.CrowdyExec.ServerObjectConnectionSettings.DefaultsClampAndRoundToMilliseconds", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectConnectionSettingsTest::RunTest(const FString& Parameters)
{
	UCrowdySDKDeveloperSettings* Settings = NewObject<UCrowdySDKDeveloperSettings>(GetTransientPackage(), NAME_None, RF_Transient);
	Settings->ServerObjectCallTimeoutSeconds = 12.5f;
	Settings->ServerObjectOpenTimeoutSeconds = 2.0004f;
	TestEqual(TEXT("seconds become milliseconds"), Settings->GetServerObjectCallTimeoutMs(), 12500);
	TestEqual(TEXT("rounded to the millisecond"), Settings->GetServerObjectOpenTimeoutMs(), 2000);

	Settings->ServerObjectCallTimeoutSeconds = 0.2f;
	Settings->ServerObjectOpenTimeoutSeconds = -5.f;
	TestEqual(TEXT("a call timeout below its range dials at its minimum, above the longest Time per Call"), Settings->GetServerObjectCallTimeoutMs(), 11000);
	TestEqual(TEXT("an open timeout at its own"), Settings->GetServerObjectOpenTimeoutMs(), 1000);

	Settings->ServerObjectCallTimeoutSeconds = 500.f;
	Settings->ServerObjectOpenTimeoutSeconds = 100.f;
	TestEqual(TEXT("a call timeout above its range dials at its maximum"), Settings->GetServerObjectCallTimeoutMs(), 120000);
	TestEqual(TEXT("and an open timeout at its own"), Settings->GetServerObjectOpenTimeoutMs(), 60000);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectCallActionTimeoutTest,
	"CrowdySDK.CrowdyExec.ServerObjectBlueprint.CallNodesPassTheirTimeoutToTheCall", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectCallActionTimeoutTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectPingTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquireReadyObject(Rig, Owner.Get());
	if (!Object)
	{
		return false;
	}
	const auto MakeListener = []()
	{
		return TStrongObjectPtr<UCrowdyServerObjectTestListener>(NewObject<UCrowdyServerObjectTestListener>(GetTransientPackage(), NAME_None, RF_Transient));
	};
	const auto Start = [](UCrowdyServerCallAction* Action, UCrowdyServerObjectTestListener* Listener)
	{
		Action->OnSuccess.AddDynamic(Listener, &UCrowdyServerObjectTestListener::HandleSuccess);
		Action->OnFailed.AddDynamic(Listener, &UCrowdyServerObjectTestListener::HandleFailed);
		Action->Activate();
	};
	TStrongObjectPtr<UCrowdyServerObjectTestListener> PlainListener = MakeListener();
	TStrongObjectPtr<UCrowdyServerObjectTestListener> TypedListener = MakeListener();
	TStrongObjectPtr<UCrowdyServerObjectTestListener> DefaultListener = MakeListener();
	TStrongObjectPtr<UCrowdyServerCallAction> Plain(UCrowdyServerCallAction::CallServerFunction(nullptr, Object, AttackFunction, AttackParams(5), 2.f));
	TStrongObjectPtr<UCrowdyServerCallAction> Typed(UCrowdyServerTypedCallAction::CallServerFunctionWithInputs(nullptr, Object, AttackFunction, AttackParams(5), FString(), 3.f));
	TStrongObjectPtr<UCrowdyServerCallAction> Untimed(UCrowdyServerCallAction::CallServerFunction(nullptr, Object, AttackFunction, AttackParams(5)));
	if (!TestTrue(TEXT("each node makes its action"), Plain.IsValid() && Typed.IsValid() && Untimed.IsValid()))
	{
		return false;
	}
	Start(Plain.Get(), PlainListener.Get());
	Start(Typed.Get(), TypedListener.Get());
	Start(Untimed.Get(), DefaultListener.Get());

	Rig.Tick(2.5f);
	TestTrue(TEXT("Call Server Function gives up when its Timeout runs out"), PlainListener->FailedOutcomes == TArray<ECrowdyServerCallOutcome>({ECrowdyServerCallOutcome::Timeout}));
	TestTrue(TEXT("saying how long it waited"), PlainListener->FailedReasons.Num() == 1
		&& PlainListener->FailedReasons[0].Equals(FString::Printf(TEXT("No answer within 2 seconds; %s may still have run on the server"), AttackFunction), ESearchCase::CaseSensitive));
	TestEqual(TEXT("the other two are still waiting"), TypedListener->FailedOutcomes.Num() + DefaultListener->FailedOutcomes.Num(), 0);

	Rig.Tick(1.f);
	TestTrue(TEXT("the typed node gives up at its own Timeout"), TypedListener->FailedOutcomes == TArray<ECrowdyServerCallOutcome>({ECrowdyServerCallOutcome::Timeout}));
	TestTrue(TEXT("saying how long it waited"), TypedListener->FailedReasons.Num() == 1
		&& TypedListener->FailedReasons[0].Equals(FString::Printf(TEXT("No answer within 3 seconds; %s may still have run on the server"), AttackFunction), ESearchCase::CaseSensitive));
	TestTrue(TEXT("a call with no Timeout waits for the connection's own"), DefaultListener->FailedOutcomes.IsEmpty() && DefaultListener->SuccessOutcomes.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectWidgetOwnerTest,
	"CrowdySDK.CrowdyExec.ServerObjectWidgetOwner.HoldsByGetAndByLinkUntilItIsDestroyed", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectWidgetOwnerTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Widget = MakeOwner();
	FString NoWorldError;
	TestNull(TEXT("Get Server Object with an owner that is in no world gives nothing"),
		UCrowdyServerObjectLibrary::GetServerObject(Widget.Get(), Rig.Definition.Get(), Rig.InstanceId, Widget.Get(), NoWorldError));
	TestFalse(TEXT("and says why"), NoWorldError.IsEmpty());

	UCrowdyServerObject* Held = Rig.Acquire(Rig.InstanceId, Widget.Get());
	UCrowdyServerObjectLink* Link = Rig.Subsystem->FindLink(Widget.Get(), Rig.Definition.Get(), ECrowdyServerObjectFind::InstanceId, Rig.InstanceId, 0);
	if (!Held || !TestNotNull(TEXT("Find By Asset Link makes a Link for the widget"), Link))
	{
		return false;
	}
	TestTrue(TEXT("the widget's Get Server Object and its Link share one object"), Link->GetServerObject() == Held);
	TestEqual(TEXT("which is the only object"), Rig.Subsystem->NumObjectsForTest(), 1);
	if (!Rig.Open() || !Rig.AnswerFirstRead(Held, Boss(3000, false, ECrowdyExecTestPhase::Calm), 5))
	{
		return false;
	}
	int32 Health = 0;
	TestTrue(TEXT("a value reads through the widget's Link"),
		UCrowdyServerObjectLibrary::ReadServerVariableInto(Link, Rig.Definition.Get(), TEXT("Health"), FCrowdyServerObjectTestPins::StaticStruct()->FindPropertyByName(TEXT("Count")), &Health) && Health == 3000);
	TestEqual(TEXT("the widget holds one Link"), Rig.Subsystem->NumLinksForTest(), 1);

	Destroy(Widget);
	Rig.Tick(0.1f);
	TestEqual(TEXT("a destroyed widget's Link is dropped"), Rig.Subsystem->NumLinksForTest(), 0);
	TestEqual(TEXT("its object waits out the grace period for a quick re-acquire"), Rig.Subsystem->NumObjectsForTest(), 1);
	Rig.Tick(UCrowdyServerObjectSubsystem::GracePeriodSeconds + 1.f);
	TestEqual(TEXT("and is then given back"), Rig.Subsystem->NumObjectsForTest(), 0);
	return true;
}

#endif
