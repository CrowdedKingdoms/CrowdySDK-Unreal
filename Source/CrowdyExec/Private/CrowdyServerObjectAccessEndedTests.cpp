#include "CrowdyServerObjectTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyCppClient.h"
#include "CrowdyNativeExec.h"
#include "CrowdyServerObject.h"
#include "CrowdyServerObjectSubsystem.h"
#include "CrowdyServerObjectTestRig.h"
#include "Misc/AutomationTest.h"
#include "Subsystem/CrowdyGameSession.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace CrowdyServerObjectAccessEndedTests
{
	const FString ServerReason = TEXT("access ended: suspended");
	const FString ObjectReason = TEXT("Your access ended: suspended");

	TStrongObjectPtr<UCrowdyServerObjectTestAccessSpy> ListenToSession(CrowdyServerObjectTests::FRig& Rig)
	{
		TStrongObjectPtr<UCrowdyServerObjectTestAccessSpy> Spy(NewObject<UCrowdyServerObjectTestAccessSpy>(GetTransientPackage(), NAME_None, RF_Transient));
		Rig.Session->OnAccessEnded.AddDynamic(Spy.Get(), &UCrowdyServerObjectTestAccessSpy::HandleAccessEnded);
		return Spy;
	}

	/** The server closes the open connection because the player's access ended, and Object is waited on to fail. */
	bool EndAccess(CrowdyServerObjectTests::FRig& Rig, UCrowdyServerObject* Object)
	{
		Rig.Client->TestExecCloseFromServer(4403, true, ServerReason);
		return Rig.Test.TestTrue(TEXT("the object fails when the server ends the player's access"),
			Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Failed; }));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectAccessEndedFailsObjectsTest,
	"CrowdySDK.CrowdyExec.AccessEnded.FailsObjectsAndTellsSessionOnce", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectAccessEndedFailsObjectsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectAccessEndedTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestAccessSpy> Spy = ListenToSession(Rig);
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.AcquireReady(Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5);
	if (!Object)
	{
		return false;
	}
	FStatusSpy Status;
	Status.Listen(Object);

	if (!EndAccess(Rig, Object))
	{
		return false;
	}
	TestEqualSensitive(TEXT("the object says why"), Object->GetFailureReason(), ObjectReason);
	TestTrue(TEXT("the failure was broadcast"), Status.Seen->Contains(ECrowdyServerObjectStatus::Failed));
	TestTrue(TEXT("the connection is closed"), Rig.Subsystem->GetConnection() == nullptr);
	TestTrue(TEXT("the session knows the access ended"), Rig.Session->HasAccessEnded());
	TestEqualSensitive(TEXT("with the server's reason"), Rig.Session->GetAccessEndedReason(), ServerReason);
	TestEqual(TEXT("the session announced it once"), Spy->Reasons.Num(), 1);

	Rig.Tick(5.f);
	Rig.PumpFor(0.2);
	TestEqual(TEXT("and still once after more ticks"), Spy->Reasons.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectAccessEndedNoRedialTest,
	"CrowdySDK.CrowdyExec.AccessEnded.DoesNotRedial", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectAccessEndedNoRedialTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectAccessEndedTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.AcquireReady(Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5);
	if (!Object || !EndAccess(Rig, Object))
	{
		return false;
	}
	TestEqual(TEXT("one dial so far"), DialsOf(Rig), 1);

	Rig.Tick(60.f);
	Rig.PumpFor(0.3);
	TestEqual(TEXT("no redial however long it waits"), DialsOf(Rig), 1);

	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Late = MakeOwner();
	UCrowdyServerObject* Other = Rig.Acquire(TEXT("boss-2"), Late.Get());
	if (!Other)
	{
		return false;
	}
	TestEqual(TEXT("the acquire itself fails it"), static_cast<int32>(Other->GetStatus()), static_cast<int32>(ECrowdyServerObjectStatus::Failed));
	TestTrue(TEXT("without opening a connection"), Rig.Subsystem->GetConnection() == nullptr);
	Rig.Tick(5.f);
	Rig.PumpFor(0.3);
	TestEqual(TEXT("an object acquired afterwards dials nothing"), DialsOf(Rig), 1);
	TestEqual(TEXT("it fails at once"), static_cast<int32>(Other->GetStatus()), static_cast<int32>(ECrowdyServerObjectStatus::Failed));
	TestEqualSensitive(TEXT("with the same reason"), Other->GetFailureReason(), ObjectReason);

	FStatusSpy Reacquired;
	Reacquired.Listen(Object);
	UCrowdyServerObject* Same = Rig.Acquire(Rig.InstanceId, Late.Get());
	TestTrue(TEXT("acquiring the failed object again returns it"), Same == Object);
	TestFalse(TEXT("without restarting it"), Reacquired.Seen->Contains(ECrowdyServerObjectStatus::Connecting));
	TestEqual(TEXT("still failed"), static_cast<int32>(Object->GetStatus()), static_cast<int32>(ECrowdyServerObjectStatus::Failed));
	TestTrue(TEXT("and no connection opens"), Rig.Subsystem->GetConnection() == nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectAccessEndedAcquireLiveTest,
	"CrowdySDK.CrowdyExec.AccessEnded.AcquireFailsALiveObject", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectAccessEndedAcquireLiveTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.AcquireReady(Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5);
	if (!Object)
	{
		return false;
	}

	Rig.Session->SetAccessEnded(TEXT("The account is blocked"), FString());
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Late = MakeOwner();
	TestTrue(TEXT("acquiring the live object returns it"), Rig.Acquire(Rig.InstanceId, Late.Get()) == Object);
	TestEqual(TEXT("failed by the acquire itself"), static_cast<int32>(Object->GetStatus()), static_cast<int32>(ECrowdyServerObjectStatus::Failed));
	TestEqualSensitive(TEXT("with the session's reason"), Object->GetFailureReason(), FString(TEXT("Your access ended: The account is blocked")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectAccessEndedLaterCallTest,
	"CrowdySDK.CrowdyExec.AccessEnded.LaterCallAnswersAccessEnded", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectAccessEndedLaterCallTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectAccessEndedTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.AcquireReady(Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5);
	if (!Object || !EndAccess(Rig, Object))
	{
		return false;
	}

	FCallSpy Call;
	Object->Call(AttackFunction, AttackParams(250), Call.OnDone());
	if (!TestEqual(TEXT("a call after the cut completes at once"), Call.Num(), 1))
	{
		return false;
	}
	const FCrowdyServerCallResult& Result = (*Call.Results)[0];
	TestEqual(TEXT("as Access Ended"), static_cast<int32>(Result.Outcome), static_cast<int32>(ECrowdyServerCallOutcome::AccessEnded));
	TestFalse(TEXT("which is not retryable"), Result.bRetryable);
	TestEqualSensitive(TEXT("and says why"), Result.Reason, ObjectReason);
	TestFalse(TEXT("nothing was sent"), Rig.SentAnyCall());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectAccessEndedInFlightCallTest,
	"CrowdySDK.CrowdyExec.AccessEnded.InFlightCallAnswersAccessEnded", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectAccessEndedInFlightCallTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectAccessEndedTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.AcquireReady(Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5);
	if (!Object)
	{
		return false;
	}

	FCallSpy Refused;
	Object->Call(AttackFunction, AttackParams(250), Refused.OnDone());
	FSentFrame Sent;
	if (!TestTrue(TEXT("the first call is in flight"), Rig.WaitForFrame(KindCall, AttackMethod, Sent)))
	{
		return false;
	}
	Rig.Reply(Sent.Rid, ECrowdyNativeExecStatus::Denied, Utf8Bytes(ServerReason));
	if (!TestTrue(TEXT("a Denied answer with the platform's reason completes the call"), Rig.PumpUntil([&Refused]() { return Refused.Num() == 1; })))
	{
		return false;
	}
	const FCrowdyServerCallResult& Answer = (*Refused.Results)[0];
	TestEqual(TEXT("as Access Ended, not Denied"), static_cast<int32>(Answer.Outcome), static_cast<int32>(ECrowdyServerCallOutcome::AccessEnded));
	TestFalse(TEXT("which is not retryable"), Answer.bRetryable);
	TestEqualSensitive(TEXT("with the reason"), Answer.Reason, ObjectReason);

	FCallSpy Cut;
	Object->Call(AttackFunction, AttackParams(250), Cut.OnDone());
	if (!TestTrue(TEXT("the second call is in flight"), Rig.WaitForFrame(KindCall, AttackMethod, Sent)))
	{
		return false;
	}
	if (!EndAccess(Rig, Object) || !TestEqual(TEXT("the cut completes the call in flight"), Cut.Num(), 1))
	{
		return false;
	}
	const FCrowdyServerCallResult& Result = (*Cut.Results)[0];
	TestEqual(TEXT("as Access Ended"), static_cast<int32>(Result.Outcome), static_cast<int32>(ECrowdyServerCallOutcome::AccessEnded));
	TestFalse(TEXT("which is not retryable"), Result.bRetryable);
	TestEqualSensitive(TEXT("with the reason"), Result.Reason, ObjectReason);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectAccessEndedReadsStopTest,
	"CrowdySDK.CrowdyExec.AccessEnded.RefusedReadFailsInsteadOfRetrying", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectAccessEndedReadsStopTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectAccessEndedTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	if (!Object || !Rig.Open())
	{
		return false;
	}
	FSentFrame Read;
	if (!TestTrue(TEXT("the object reads"), Rig.WaitForFrame(KindCall, TEXT("read"), Read)))
	{
		return false;
	}
	Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::Denied, Utf8Bytes(ServerReason));
	TestTrue(TEXT("a read refused because access ended fails the object"),
		Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Failed; }));
	TestEqualSensitive(TEXT("with the reason"), Object->GetFailureReason(), ObjectReason);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectAccessEndedSessionEndedElsewhereTest,
	"CrowdySDK.CrowdyExec.AccessEnded.SessionEndedElsewhereFailsWaitingObjects", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectAccessEndedSessionEndedElsewhereTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	AddExpectedErrorPlain(ConnectFailedWarning, EAutomationExpectedErrorFlags::Contains, 1);
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	if (!Object || !FailDial(Rig, 1))
	{
		return false;
	}

	Rig.Session->SetAccessEnded(TEXT("The account is blocked"), FString());
	Rig.Tick(5.f);
	Rig.PumpFor(0.3);
	TestEqual(TEXT("a session that heard it elsewhere fails the waiting object"), static_cast<int32>(Object->GetStatus()), static_cast<int32>(ECrowdyServerObjectStatus::Failed));
	TestEqualSensitive(TEXT("with the session's reason"), Object->GetFailureReason(), FString(TEXT("Your access ended: The account is blocked")));
	TestEqual(TEXT("and never dials again"), DialsOf(Rig), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectAccessEndedClearedDialsAgainTest,
	"CrowdySDK.CrowdyExec.AccessEnded.AcquireAfterClearDialsAgain", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectAccessEndedClearedDialsAgainTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectAccessEndedTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestAccessSpy> Spy = ListenToSession(Rig);
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.AcquireReady(Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5);
	if (!Object || !EndAccess(Rig, Object))
	{
		return false;
	}
	TestEqual(TEXT("one dial before the cut"), DialsOf(Rig), 1);

	Rig.Session->ClearAccessEnded();
	UCrowdyServerObject* Again = Rig.Acquire(Rig.InstanceId, Owner.Get());
	if (!Again)
	{
		return false;
	}
	if (!TestTrue(TEXT("acquiring after the access came back dials again"), Rig.PumpUntil([&Rig]() { return DialsOf(Rig) >= 2; })))
	{
		return false;
	}
	Rig.Client->TestExecOpen();
	TestTrue(TEXT("and the object reaches Ready"), Rig.AnswerFirstRead(Again, Boss(2900, false, ECrowdyExecTestPhase::Calm), 7));

	Rig.Client->TestExecCloseFromServer(4403, true, ServerReason);
	Rig.PumpUntil([Again]() { return Again->GetStatus() == ECrowdyServerObjectStatus::Failed; });
	TestEqual(TEXT("a second cut after the clear is announced again"), Spy->Reasons.Num(), 2);
	return true;
}

#endif
