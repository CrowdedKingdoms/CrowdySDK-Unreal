#include "CrowdyServerObjectTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/TaskGraphInterfaces.h"
#include "CrowdyCppClient.h"
#include "CrowdyExecCodec.h"
#include "CrowdyExecInternal.h"
#include "CrowdyExecTestTypes.h"
#include "CrowdyNativeExec.h"
#include "CrowdyServerObject.h"
#include "CrowdyServerObjectComponent.h"
#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectLibrary.h"
#include "CrowdyServerObjectLink.h"
#include "CrowdyServerObjectSubsystem.h"
#include "CrowdyServerObjectTestRig.h"
#include "Engine/GameInstance.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Network/CrowdyCpp/CrowdyCppClientSubsystem.h"
#include "Subsystem/CrowdyGameSession.h"
#include "UObject/Package.h"
#include "UObject/Script.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectAcquireTwiceSharesOneObjectTest,
	"CrowdySDK.CrowdyExec.ServerObjectAcquireTwiceSharesOneObject", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectAcquireTwiceSharesOneObjectTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> First = MakeOwner();
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Second = MakeOwner();

	UCrowdyServerObject* Shared = Rig.Acquire(TEXT("m1"), First.Get());
	UCrowdyServerObject* Again = Rig.Acquire(TEXT("m1"), Second.Get());
	UCrowdyServerObject* Other = Rig.Acquire(TEXT("m2"), First.Get());
	UCrowdyServerObject* Upper = Rig.Acquire(TEXT("M1"), First.Get());
	if (!Shared || !Again || !Other || !Upper)
	{
		return false;
	}
	TestTrue(TEXT("a second owner of the same definition and Instance Id shares the first owner's object"), Shared == Again);
	TestTrue(TEXT("another Instance Id gets another object"), Other != Shared);
	TestTrue(TEXT("Instance Ids compare case-sensitively, so M1 is not m1"), Upper != Shared && Upper != Other);
	TestEqual(TEXT("three distinct pairs hold three objects"), Rig.Subsystem->NumObjectsForTest(), 3);
	TestEqualSensitive(TEXT("the object carries its Instance Id"), Shared->GetInstanceId(), FString(TEXT("m1")));
	TestTrue(TEXT("the object carries its definition"), Shared->GetDefinition() == Rig.Definition.Get());

	const FString BadId = TEXT("An Instance Id must be 1 to 256 bytes with no control characters");
	const FString Longest = FString::ChrN(256, TEXT('a'));
	const FString TooLong = FString::ChrN(257, TEXT('a'));
	const FString WideTooLong = FString::ChrN(129, static_cast<TCHAR>(0xE9));
	const FString WithTab = TEXT("m\t1");
	for (const FString& Id : {FString(), TooLong, WideTooLong, WithTab})
	{
		FString Error;
		UCrowdyServerObject* Refused = Rig.Subsystem->Acquire(Rig.Definition.Get(), Id, First.Get(), Error);
		TestTrue(FString::Printf(TEXT("an Instance Id of %d characters (%d UTF-8 bytes) is refused"), Id.Len(), FTCHARToUTF8(*Id, Id.Len()).Length()),
			Refused == nullptr);
		TestEqualSensitive(TEXT("the Instance Id refusal says what a valid one is"), Error, BadId);
	}
	TestTrue(TEXT("an Instance Id of exactly 256 bytes is accepted"), Rig.Acquire(Longest, First.Get()) != nullptr);

	FString Error;
	TestTrue(TEXT("an acquire without an owner is refused"), Rig.Subsystem->Acquire(Rig.Definition.Get(), TEXT("m1"), nullptr, Error) == nullptr);
	TestEqualSensitive(TEXT("the refusal says an owner is needed"), Error,FString(TEXT("Acquire needs an owner")));
	Error.Reset();
	TestTrue(TEXT("an acquire without a definition is refused"), Rig.Subsystem->Acquire(nullptr, TEXT("m1"), First.Get(), Error) == nullptr);
	TestFalse(TEXT("the missing-definition refusal says why"), Error.IsEmpty());
	TestEqual(TEXT("refusals add no object; only the 256-byte Instance Id did"), Rig.Subsystem->NumObjectsForTest(), 4);

	Rig.Open();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectGraceReleasesAndReacquireReusesTest,
	"CrowdySDK.CrowdyExec.ServerObjectGraceReleasesAndReacquireReuses", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectGraceReleasesAndReacquireReusesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> First = MakeOwner();
	TStrongObjectPtr<UCrowdyServerObject> Object(Rig.Acquire(Rig.InstanceId, First.Get()));
	if (!Object || !Rig.Open())
	{
		return false;
	}
	FStatusSpy Status;
	Status.Listen(Object.Get());

	Destroy(First);
	Rig.Tick(5.f);
	TestNotEqual(TEXT("an object whose last owner died 5 s ago is still alive"), static_cast<int32>(Object->GetStatus()), static_cast<int32>(ECrowdyServerObjectStatus::Released));
	TestEqual(TEXT("it is still held during the grace period"), Rig.Subsystem->NumObjectsForTest(), 1);

	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Second = MakeOwner();
	TestTrue(TEXT("re-acquiring within the grace period returns the same object"), Rig.Acquire(Rig.InstanceId, Second.Get()) == Object.Get());

	Destroy(Second);
	Rig.Subsystem->SetMapLoadingForTest(true);
	Rig.Tick(20.f);
	TestNotEqual(TEXT("no grace time passes while a map loads"), static_cast<int32>(Object->GetStatus()), static_cast<int32>(ECrowdyServerObjectStatus::Released));
	Rig.Subsystem->SetMapLoadingForTest(false);

	Rig.Tick(9.5f);
	TestNotEqual(TEXT("the re-acquire restarted the grace period, so 9.5 s later the object is alive"),
		static_cast<int32>(Object->GetStatus()), static_cast<int32>(ECrowdyServerObjectStatus::Released));
	TestEqual(TEXT("and still held"), Rig.Subsystem->NumObjectsForTest(), 1);

	Rig.Tick(1.f);
	TestEqual(TEXT("10.5 s after its last owner died the object is Released"), static_cast<int32>(Object->GetStatus()), static_cast<int32>(ECrowdyServerObjectStatus::Released));
	TestEqual(TEXT("and no longer held"), Rig.Subsystem->NumObjectsForTest(), 0);
	TestTrue(TEXT("the release was broadcast"), Status.Seen->Contains(ECrowdyServerObjectStatus::Released));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSubscribeBeforeReadTest,
	"CrowdySDK.CrowdyExec.ServerObjectSubscribeBeforeReadThenOneEventPerPush", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectSubscribeBeforeReadTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	if (!Object)
	{
		return false;
	}
	FStatusSpy Status;
	Status.Listen(Object);
	FValuesSpy Values;
	Values.Watch(Object);
	TestEqual(TEXT("a new object starts Connecting"), static_cast<int32>(Object->GetStatus()), static_cast<int32>(ECrowdyServerObjectStatus::Connecting));
	TestEqual(TEXT("its state holds the struct's defaults until the first read"), HealthOf(Object), 4000);
	if (!Rig.Open() || !TestTrue(TEXT("the object reads once connected"), Rig.PumpUntil([&Rig]() { return Rig.FindFrame(KindCall, TEXT("read")) != INDEX_NONE; })))
	{
		return false;
	}

	const int32 SubscribeIndex = Rig.FindFrame(KindSubscribe, TEXT("state"));
	const int32 ReadIndex = Rig.FindFrame(KindCall, TEXT("read"));
	if (!TestTrue(TEXT("the object subscribed to the state topic"), SubscribeIndex != INDEX_NONE))
	{
		return false;
	}
	TestTrue(TEXT("the subscribe went out before the read, so no change falls between them"), SubscribeIndex < ReadIndex);
	const FSentFrame Subscribe = Rig.Sent[SubscribeIndex];
	const FSentFrame Read = Rig.Sent[ReadIndex];
	TestTrue(TEXT("the subscribe names the type and the Instance Id"), Rig.IsAddressed(Subscribe));
	TestTrue(TEXT("the read names the type and the Instance Id"), Rig.IsAddressed(Read));
	TestTrue(TEXT("the read sends an empty map"), Read.Payload == TArray<uint8>({0x80}));

	Rig.Reply(Subscribe.Rid, ECrowdyNativeExecStatus::Ok, {});
	Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::Ok, Rig.ReadMessage(5, Boss(3000, false, ECrowdyExecTestPhase::Enraged)));
	if (!TestTrue(TEXT("the read reply makes the object Ready"), Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; })))
	{
		return false;
	}
	TestTrue(TEXT("becoming Ready was broadcast once"), Status.Seen->Num() == 1 && (*Status.Seen)[0] == ECrowdyServerObjectStatus::Ready);
	const FCrowdyExecTestBossState* State = StateOf(Object);
	TestTrue(TEXT("the state holds the read's values"), State && State->Health == 3000 && !State->bDefeated && State->Phase == ECrowdyExecTestPhase::Enraged);
	TestEqual(TEXT("the first read raised one change event"), Values.Num(), 1);
	TestTrue(FString::Printf(TEXT("the first read's event names every watched value (got %s)"), *Joined(Values.Last())), NamesAre(Values.Last(), AllWatched()));

	const TArray<FString> Pushed = {TEXT("Health"), TEXT("bDefeated")};
	Rig.Push(Rig.PushMessage(Epoch, 6, Boss(2500, true, ECrowdyExecTestPhase::Final), Pushed));
	TestTrue(TEXT("the next push raised a change event"), Rig.PumpUntil([&Values]() { return Values.Num() >= 2; }));
	Rig.PumpFor(0.1);
	TestEqual(TEXT("a push carrying two values raises exactly one event"), Values.Num(), 2);
	TestTrue(FString::Printf(TEXT("that event names both values (got %s)"), *Joined(Values.Last())), NamesAre(Values.Last(), Pushed));
	State = StateOf(Object);
	TestTrue(TEXT("the push changed only the values it carried"), State && State->Health == 2500 && State->bDefeated && State->Phase == ECrowdyExecTestPhase::Enraged);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectLateBinderFiresOnceTest,
	"CrowdySDK.CrowdyExec.ServerObjectLateBinderFiresOnce", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectLateBinderFiresOnceTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	if (!Object)
	{
		return false;
	}
	FValuesSpy Early;
	Early.Watch(Object);
	TestEqual(TEXT("a listener added while Connecting is not called at once"), Early.Num(), 0);
	if (!Rig.Open() || !Rig.AnswerFirstRead(Object, Boss(3000, false, ECrowdyExecTestPhase::Calm), 5))
	{
		return false;
	}
	TestEqual(TEXT("the early listener heard the first read"), Early.Num(), 1);

	FValuesSpy Late;
	Late.Watch(Object);
	TestEqual(TEXT("a listener added after Ready is called once, at once"), Late.Num(), 1);
	TestTrue(FString::Printf(TEXT("naming every watched value (got %s)"), *Joined(Late.Last())), NamesAre(Late.Last(), AllWatched()));
	TestEqual(TEXT("the late listener's catch-up call reaches no other listener"), Early.Num(), 1);
	Rig.PumpFor(0.1);
	TestEqual(TEXT("the late listener is not called again without a change"), Late.Num(), 1);

	const TArray<FString> Pushed = {TEXT("Phase")};
	Rig.Push(Rig.PushMessage(Epoch, 6, Boss(0, false, ECrowdyExecTestPhase::Final), Pushed));
	TestTrue(TEXT("the late listener hears the next push"), Rig.PumpUntil([&Late]() { return Late.Num() >= 2; }));
	TestTrue(FString::Printf(TEXT("the push event names only the pushed value (got %s)"), *Joined(Late.Last())), NamesAre(Late.Last(), Pushed));
	TestEqual(TEXT("the early listener hears the push too"), Early.Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectPushRacingReadIsAppliedOnceTest,
	"CrowdySDK.CrowdyExec.ServerObjectPushRacingReadIsAppliedOnce", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectPushRacingReadIsAppliedOnceTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
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
	FValuesSpy Values;
	Values.Watch(Object);
	FSentFrame Subscribe;
	FSentFrame Read;
	if (!TestTrue(TEXT("the object subscribes"), Rig.WaitForFrame(KindSubscribe, TEXT("state"), Subscribe))
		|| !TestTrue(TEXT("the object reads"), Rig.WaitForFrame(KindCall, TEXT("read"), Read)))
	{
		return false;
	}

	const TArray<FString> HealthOnly = {TEXT("Health")};
	Rig.Reply(Subscribe.Rid, ECrowdyNativeExecStatus::Ok, {});
	Rig.Push(Rig.PushMessage(Epoch, 6, Boss(100, false, ECrowdyExecTestPhase::Calm), HealthOnly));
	Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::Ok, Rig.ReadMessage(5, Boss(200, false, ECrowdyExecTestPhase::Enraged)));
	if (!TestTrue(TEXT("the read reply makes the object Ready"), Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; })))
	{
		return false;
	}
	Rig.PumpFor(0.1);
	TestEqual(TEXT("the push that raced the read is applied on top of it"), HealthOf(Object), 100);
	TestEqual(TEXT("the read and the racing push raise one event each"), Values.Num(), 2);
	TestTrue(FString::Printf(TEXT("the replayed push names only its value (got %s)"), *Joined(Values.Last())), NamesAre(Values.Last(), HealthOnly));
	TestEqual(TEXT("the read's other values stay"), static_cast<int32>(StateOf(Object) ? StateOf(Object)->Phase : ECrowdyExecTestPhase::Calm), static_cast<int32>(ECrowdyExecTestPhase::Enraged));
	TestFalse(TEXT("a push following the read in order forces no second read"), Rig.SentAny(KindCall, TEXT("read")));

	Rig.Push(Rig.PushMessage(Epoch, 5, Boss(50, false, ECrowdyExecTestPhase::Calm), HealthOnly));
	Rig.Push(Rig.PushMessage(Epoch, 7, Boss(75, false, ECrowdyExecTestPhase::Calm), HealthOnly));
	TestTrue(TEXT("the push after the stale one is applied, so the stale one was handled first"), Rig.PumpUntil([Object]() { return HealthOf(Object) == 75; }));
	Rig.PumpFor(0.1);
	TestEqual(TEXT("the push the read already covered raised no event"), Values.Num(), 3);
	TestFalse(TEXT("neither push forced a read"), Rig.SentAny(KindCall, TEXT("read")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectGapOrNewEpochRereadsTest,
	"CrowdySDK.CrowdyExec.ServerObjectGapOrNewEpochRereads", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectGapOrNewEpochRereadsTest::RunTest(const FString& Parameters)
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
	const TArray<FString> HealthOnly = {TEXT("Health")};
	Rig.Sent.Reset();

	Rig.Push(Rig.PushMessage(Epoch, 7, Boss(111, false, ECrowdyExecTestPhase::Calm), HealthOnly));
	FSentFrame GapRead;
	if (!TestTrue(TEXT("a push that skips a seq triggers a new read"), Rig.WaitForFrame(KindCall, TEXT("read"), GapRead)))
	{
		return false;
	}
	TestEqual(TEXT("the push after the gap is not applied"), HealthOf(Object), 3000);
	Rig.Reply(GapRead.Rid, ECrowdyNativeExecStatus::Ok, Rig.ReadMessage(7, Boss(2222, false, ECrowdyExecTestPhase::Calm)));
	if (!TestTrue(TEXT("the new read's values are applied"), Rig.PumpUntil([Object]() { return HealthOf(Object) == 2222; })))
	{
		return false;
	}
	Rig.Sent.Reset();

	// The next seq in order, so only the changed epoch can make it a re-read.
	Rig.Push(Rig.PushMessage(Epoch + 1, 8, Boss(999, false, ECrowdyExecTestPhase::Calm), HealthOnly));
	FSentFrame EpochRead;
	TestTrue(TEXT("a push from another epoch triggers a new read"), Rig.WaitForFrame(KindCall, TEXT("read"), EpochRead));
	TestEqual(TEXT("the push from another epoch is not applied"), HealthOf(Object), 2222);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectBusyWaitsForRetryHintTest,
	"CrowdySDK.CrowdyExec.ServerObjectBusyWaitsForRetryHint", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectBusyWaitsForRetryHintTest::RunTest(const FString& Parameters)
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
	FCallSpy Call;
	const FInstancedStruct Params = AttackParams(250);
	Object->Call(AttackFunction, Params, Call.OnDone());
	FSentFrame First;
	if (!TestTrue(TEXT("the call is sent"), Rig.WaitForFrame(KindCall, AttackMethod, First)))
	{
		return false;
	}
	TestTrue(TEXT("the call names the type and the Instance Id"), Rig.IsAddressed(First));
	TestTrue(TEXT("the call carries its params"), First.Payload == Rig.Encode(Params.Get<FCrowdyServerObjectTestAttackParams>()));

	// CrowdyCPP reads the wait from a rate-limit refusal's "retry in N ms".
	Rig.Reply(First.Rid, ECrowdyNativeExecStatus::Busy, Utf8Bytes(TEXT("rate limited: retry in 1500 ms")));
	Rig.PumpFor(0.1);
	TestEqual(TEXT("a busy reply is retried, not reported"), Call.Num(), 0);
	Rig.Tick(1.f);
	TestFalse(TEXT("no retry before the server's 1.5 s hint, though the default first wait is shorter"), Rig.SentAny(KindCall, AttackMethod));
	Rig.Tick(0.6f);
	FSentFrame Retry;
	if (!TestTrue(TEXT("the retry goes out once the hinted wait has passed"), Rig.WaitForFrame(KindCall, AttackMethod, Retry)))
	{
		return false;
	}
	TestTrue(TEXT("the retry carries the same params"), Retry.Payload == First.Payload);

	FCrowdyServerObjectTestAttackReply Answer;
	Answer.HealthLeft = 2750;
	Answer.bKilled = true;
	Rig.Reply(Retry.Rid, ECrowdyNativeExecStatus::Ok, Rig.Encode(Answer));
	TestTrue(TEXT("the retried call completes"), Rig.PumpUntil([&Call]() { return Call.Num() > 0; }));
	Rig.PumpFor(0.1);
	if (!TestEqual(TEXT("the call completes exactly once"), Call.Num(), 1))
	{
		return false;
	}
	const FCrowdyServerCallResult& Result = (*Call.Results)[0];
	TestEqual(TEXT("the retried call succeeds"), static_cast<int32>(Result.Outcome), static_cast<int32>(ECrowdyServerCallOutcome::Success));
	const FCrowdyServerObjectTestAttackReply* Decoded = Result.Reply.GetPtr<FCrowdyServerObjectTestAttackReply>();
	TestTrue(TEXT("the reply is decoded into the function's Reply struct"), Decoded && Decoded->HealthLeft == 2750 && Decoded->bKilled);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectDroppedSocketRereadsTest,
	"CrowdySDK.CrowdyExec.ServerObjectDroppedSocketRereads", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectDroppedSocketRereadsTest::RunTest(const FString& Parameters)
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
	FValuesSpy Values;
	Values.Watch(Object);
	Rig.Sent.Reset();

	Rig.Client->TestExecCloseFromServer(1006, false);
	if (!TestTrue(TEXT("the dropped socket is redialled"), Rig.PumpUntil([&Rig]() { return Rig.Client->NumTestExecConnections() >= 2; }, 6.0)))
	{
		return false;
	}
	Rig.Client->TestExecOpen();
	if (!TestTrue(TEXT("the reconnect triggers a new read"), Rig.PumpUntil([&Rig]() { return Rig.FindFrame(KindCall, TEXT("read")) != INDEX_NONE; }, 5.0)))
	{
		return false;
	}
	const int32 ReadIndex = Rig.FindFrame(KindCall, TEXT("read"));
	auto LastBefore = [&Rig](uint8 Kind, int32 Before)
	{
		for (int32 Index = Before - 1; Index >= 0; --Index)
		{
			if (Rig.Sent[Index].Kind == Kind && Rig.Sent[Index].Name.Equals(TEXT("state"), ESearchCase::CaseSensitive))
			{
				return Index;
			}
		}
		return int32(INDEX_NONE);
	};
	const int32 SubscribeIndex = LastBefore(KindSubscribe, ReadIndex);
	TestTrue(TEXT("the reconnect subscribes afresh before the read"), SubscribeIndex != INDEX_NONE);
	TestTrue(TEXT("after dropping the old subscription"), LastBefore(KindUnsubscribe, SubscribeIndex) != INDEX_NONE);
	const FSentFrame Read = Rig.Sent[ReadIndex];
	Rig.Sent.RemoveAt(ReadIndex);
	Rig.AckSubscribes();
	TestEqual(TEXT("nothing changed before the new read's reply"), Values.Num(), 1);
	Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::Ok, Rig.ReadMessage(6, Boss(2000, false, ECrowdyExecTestPhase::Calm)));
	TestTrue(TEXT("the new read's changed value raises an event"), Rig.PumpUntil([&Values]() { return Values.Num() >= 2; }));
	TestTrue(FString::Printf(TEXT("the event names only the value that changed while disconnected (got %s)"), *Joined(Values.Last())),
		NamesAre(Values.Last(), {TEXT("Health")}));
	TestEqual(TEXT("the state holds the new read's value"), HealthOf(Object), 2000);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectSignOutFailsPendingCallsTest,
	"CrowdySDK.CrowdyExec.ServerObjectSignOutFailsPendingCalls", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectSignOutFailsPendingCallsTest::RunTest(const FString& Parameters)
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
	FStatusSpy Status;
	Status.Listen(Object);
	FCallSpy Call;
	Object->Call(AttackFunction, AttackParams(250), Call.OnDone());
	FSentFrame Sent;
	if (!TestTrue(TEXT("the call is in flight"), Rig.WaitForFrame(KindCall, AttackMethod, Sent)))
	{
		return false;
	}

	Rig.Subsystem->SignOutForTest();
	if (!TestEqual(TEXT("signing out completes the pending call at once"), Call.Num(), 1))
	{
		return false;
	}
	const FCrowdyServerCallResult& Result = (*Call.Results)[0];
	TestEqual(TEXT("as Canceled"), static_cast<int32>(Result.Outcome), static_cast<int32>(ECrowdyServerCallOutcome::Canceled));
	TestEqualSensitive(TEXT("saying the player signed out"), Result.Reason, FString(TEXT("The player signed out")));
	TestEqual(TEXT("the object is Failed"), static_cast<int32>(Object->GetStatus()), static_cast<int32>(ECrowdyServerObjectStatus::Failed));
	TestEqualSensitive(TEXT("with the sign-out as its reason"), Object->GetFailureReason(), FString(TEXT("The player signed out")));
	TestTrue(TEXT("the failure was broadcast"), Status.Seen->Contains(ECrowdyServerObjectStatus::Failed));
	TestTrue(TEXT("signing out closes the connection"), Rig.Subsystem->GetConnection() == nullptr);

	FCrowdyServerObjectTestAttackReply Late;
	Late.HealthLeft = 1;
	Rig.Reply(Sent.Rid, ECrowdyNativeExecStatus::Ok, Rig.Encode(Late));
	Rig.Push(Rig.PushMessage(Epoch, 6, Boss(1, true, ECrowdyExecTestPhase::Final), AllWatched()));
	Rig.PumpFor(0.3);
	TestEqual(TEXT("neither the canceled completion nor the late reply calls OnDone again"), Call.Num(), 1);
	TestEqual(TEXT("the object stays Failed"), static_cast<int32>(Object->GetStatus()), static_cast<int32>(ECrowdyServerObjectStatus::Failed));
	TestEqual(TEXT("a late push does not touch a Failed object's state"), HealthOf(Object), 3000);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectOwnerOnlyPushTriggersReadTest,
	"CrowdySDK.CrowdyExec.ServerObjectOwnerOnlyPushTriggersRead", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectOwnerOnlyPushTriggersReadTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this, ECrowdyServerObjectVisibility::OwnerOnly);
	if (!Rig.IsValid())
	{
		return false;
	}
	Rig.InstanceId = TEXT("1001");
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.AcquireReady(Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5);
	if (!Object)
	{
		return false;
	}
	FValuesSpy Values;
	Values.Watch(Object);
	Rig.Sent.Reset();

	Rig.Push(Rig.StateMessage(false, Epoch, 6, nullptr, {}));
	FSentFrame Read;
	if (!TestTrue(TEXT("a newer owner-only push, which carries no values, triggers a read"), Rig.WaitForFrame(KindCall, TEXT("read"), Read)))
	{
		return false;
	}
	TestEqual(TEXT("the push itself changes no value"), HealthOf(Object), 3000);
	TestEqual(TEXT("and raises no event"), Values.Num(), 1);

	Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::Ok, Rig.ReadMessage(6, Boss(2800, false, ECrowdyExecTestPhase::Calm)));
	TestTrue(TEXT("the read it triggered delivers the change"), Rig.PumpUntil([&Values]() { return Values.Num() >= 2; }));
	TestTrue(FString::Printf(TEXT("naming the changed value (got %s)"), *Joined(Values.Last())), NamesAre(Values.Last(), {TEXT("Health")}));
	TestEqual(TEXT("the state holds the read's value"), HealthOf(Object), 2800);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectCallRefusalsSendNothingTest,
	"CrowdySDK.CrowdyExec.ServerObjectCallRefusalsSendNothing", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectCallRefusalsSendNothingTest::RunTest(const FString& Parameters)
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
	Rig.Sent.Reset();

	FCallSpy Unknown;
	Object->Call(TEXT("Nope"), FInstancedStruct(), Unknown.OnDone());
	if (TestEqual(TEXT("an unknown function is refused before Call returns"), Unknown.Num(), 1))
	{
		TestEqual(TEXT("as BadRequest"), static_cast<int32>((*Unknown.Results)[0].Outcome), static_cast<int32>(ECrowdyServerCallOutcome::BadRequest));
		TestEqualSensitive(TEXT("naming the missing function"), (*Unknown.Results)[0].Reason, FString(TEXT("test_boss has no function named Nope")));
	}

	const FString WrongParams = TEXT("AttackBoss takes FCrowdyServerObjectTestAttackParams params");
	FCallSpy Wrong;
	Object->Call(AttackFunction, FInstancedStruct::Make(Boss(1, false, ECrowdyExecTestPhase::Calm)), Wrong.OnDone());
	if (TestEqual(TEXT("params of the wrong struct are refused before Call returns"), Wrong.Num(), 1))
	{
		TestEqual(TEXT("as BadRequest"), static_cast<int32>((*Wrong.Results)[0].Outcome), static_cast<int32>(ECrowdyServerCallOutcome::BadRequest));
		TestEqualSensitive(TEXT("naming the struct the function takes"),(*Wrong.Results)[0].Reason, WrongParams);
	}

	FCallSpy Missing;
	Object->Call(AttackFunction, FInstancedStruct(), Missing.OnDone());
	if (TestEqual(TEXT("missing params are refused before Call returns"), Missing.Num(), 1))
	{
		TestEqual(TEXT("as BadRequest"), static_cast<int32>((*Missing.Results)[0].Outcome), static_cast<int32>(ECrowdyServerCallOutcome::BadRequest));
		TestEqualSensitive(TEXT("naming the struct the function takes"),(*Missing.Results)[0].Reason, WrongParams);
	}

	TestFalse(TEXT("no refused call reaches the wire"), Rig.SentAnyCall());
	TestTrue(TEXT("each refused call completed once and no more"), Unknown.Num() == 1 && Wrong.Num() == 1 && Missing.Num() == 1);

	FCallSpy Valid;
	Object->Call(AttackFunction, AttackParams(10), Valid.OnDone());
	FSentFrame Sent;
	TestTrue(TEXT("the same connection does carry a valid call"), Rig.WaitForFrame(KindCall, AttackMethod, Sent));
	TestEqual(TEXT("a valid call waits for its reply"), Valid.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectShutdownDoesNotBroadcastTest,
	"CrowdySDK.CrowdyExec.ServerObjectShutdownDoesNotBroadcast", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectShutdownDoesNotBroadcastTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	TStrongObjectPtr<UCrowdyServerObject> Object(Rig.AcquireReady(Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5));
	if (!Object)
	{
		return false;
	}
	FStatusSpy Status;
	Status.Listen(Object.Get());
	FValuesSpy Values;
	Values.Watch(Object.Get());
	FCallSpy Call;
	Object->Call(AttackFunction, AttackParams(250), Call.OnDone());
	FSentFrame Sent;
	if (!TestTrue(TEXT("the call is in flight"), Rig.WaitForFrame(KindCall, AttackMethod, Sent)))
	{
		return false;
	}
	// Received but not yet polled, so it lands after the shutdown.
	Rig.Push(Rig.PushMessage(Epoch, 6, Boss(1, true, ECrowdyExecTestPhase::Final), AllWatched()));

	Rig.Deinitialize();
	TestEqual(TEXT("shutting down gives the object back"), static_cast<int32>(Object->GetStatus()), static_cast<int32>(ECrowdyServerObjectStatus::Released));
	TestEqual(TEXT("and empties the subsystem"), Rig.Subsystem->NumObjectsForTest(), 0);
	TestTrue(TEXT("and closes the connection"), Rig.Subsystem->GetConnection() == nullptr);
	TestEqual(TEXT("the release at shutdown is not broadcast"), Status.Seen->Num(), 0);
	TestEqual(TEXT("the pending call is dropped without running OnDone"), Call.Num(), 0);

	Rig.PumpFor(0.3);
	TestEqual(TEXT("the canceled completion delivered afterwards does not run OnDone either"), Call.Num(), 0);
	TestEqual(TEXT("nothing after shutdown raises a status event"), Status.Seen->Num(), 0);
	TestEqual(TEXT("nothing at or after shutdown raises a value event beyond the catch-up call"), Values.Num(), 1);
	TestEqual(TEXT("a push arriving after shutdown does not touch the state"), HealthOf(Object.Get()), 3000);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectDeniedReadRetriesThenReadyTest,
	"CrowdySDK.CrowdyExec.ServerObjectDeniedReadRetriesThenReady", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectDeniedReadRetriesThenReadyTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
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
	FStatusSpy Status;
	Status.Listen(Object);
	FSentFrame Subscribe;
	FSentFrame Read;
	if (!TestTrue(TEXT("the object subscribes"), Rig.WaitForFrame(KindSubscribe, TEXT("state"), Subscribe))
		|| !TestTrue(TEXT("the object reads"), Rig.WaitForFrame(KindCall, TEXT("read"), Read)))
	{
		return false;
	}
	Rig.Reply(Subscribe.Rid, ECrowdyNativeExecStatus::Ok, {});
	Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::Denied, Utf8Bytes(TEXT("not allowed")));
	Rig.PumpFor(0.1);
	TestEqual(TEXT("a Denied read leaves the object Connecting"), static_cast<int32>(Object->GetStatus()), static_cast<int32>(ECrowdyServerObjectStatus::Connecting));
	TestFalse(TEXT("the refusal is kept as the failure reason while it retries"), Object->GetFailureReason().IsEmpty());
	TestFalse(TEXT("a Denied read is not a failure"), Status.Seen->Contains(ECrowdyServerObjectStatus::Failed));

	Rig.Tick(0.5f);
	TestFalse(TEXT("the first retry waits its 1 s backoff"), Rig.SentAny(KindCall, TEXT("read")));
	Rig.Tick(0.5f);
	FSentFrame Retry;
	if (!TestTrue(TEXT("the read is retried once the backoff has passed"), Rig.WaitForFrame(KindCall, TEXT("read"), Retry)))
	{
		return false;
	}
	Rig.AckSubscribes();
	Rig.Reply(Retry.Rid, ECrowdyNativeExecStatus::Ok, Rig.ReadMessage(5, Boss(3000, false, ECrowdyExecTestPhase::Calm)));
	TestTrue(TEXT("the retried read makes the object Ready"), Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; }));
	TestTrue(TEXT("a successful read clears the failure reason"), Object->GetFailureReason().IsEmpty());
	TestEqual(TEXT("the state holds the retried read's value"), HealthOf(Object), 3000);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectTrappedCallRereadsTest,
	"CrowdySDK.CrowdyExec.ServerObjectTrappedCallRereads", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectTrappedCallRereadsTest::RunTest(const FString& Parameters)
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
	FCallSpy Call;
	Object->Call(AttackFunction, AttackParams(250), Call.OnDone());
	FSentFrame Sent;
	if (!TestTrue(TEXT("the call is sent"), Rig.WaitForFrame(KindCall, AttackMethod, Sent)))
	{
		return false;
	}
	TestFalse(TEXT("no read is pending before the crash"), Rig.SentAny(KindCall, TEXT("read")));
	Rig.Reply(Sent.Rid, ECrowdyNativeExecStatus::Trapped, Utf8Bytes(TEXT("trap: unreachable")));
	TestTrue(TEXT("the crashed call completes"), Rig.PumpUntil([&Call]() { return Call.Num() > 0; }));
	if (!TestEqual(TEXT("the call completes exactly once"), Call.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("a trapped call is reported as a server crash"), static_cast<int32>((*Call.Results)[0].Outcome),
		static_cast<int32>(ECrowdyServerCallOutcome::ServerCrashed));
	FSentFrame Reread;
	TestTrue(TEXT("the object reads again, since the instance restarted from its last snapshot"), Rig.WaitForFrame(KindCall, TEXT("read"), Reread));
	TestEqual(TEXT("a crashed call does not fail the object"), static_cast<int32>(Object->GetStatus()), static_cast<int32>(ECrowdyServerObjectStatus::Ready));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectReadRefusalsRetryUnlessPermanentTest,
	"CrowdySDK.CrowdyExec.ServerObjectReadRefusalsRetryUnlessPermanent", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectReadRefusalsRetryUnlessPermanentTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
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
	FSentFrame Subscribe;
	FSentFrame Read;
	if (!TestTrue(TEXT("the object subscribes"), Rig.WaitForFrame(KindSubscribe, TEXT("state"), Subscribe))
		|| !TestTrue(TEXT("the object reads"), Rig.WaitForFrame(KindCall, TEXT("read"), Read)))
	{
		return false;
	}
	Rig.Reply(Subscribe.Rid, ECrowdyNativeExecStatus::Ok, {});
	Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::DeadlineExceeded, Utf8Bytes(TEXT("deadline exceeded")));
	Rig.PumpFor(0.1);
	TestEqual(TEXT("a read that timed out, as one queued through an outage does, leaves the object Connecting"),
		static_cast<int32>(Object->GetStatus()), static_cast<int32>(ECrowdyServerObjectStatus::Connecting));
	TestEqualSensitive(TEXT("the timeout is kept as the reason while it retries"), Object->GetFailureReason(), FString(TEXT("The server did not answer in time")));

	Rig.Tick(1.f);
	FSentFrame Retry;
	if (!TestTrue(TEXT("the timed-out read is retried after its backoff"), Rig.WaitForFrame(KindCall, TEXT("read"), Retry)))
	{
		return false;
	}
	Rig.Reply(Retry.Rid, ECrowdyNativeExecStatus::Internal, Utf8Bytes(TEXT("internal")));
	Rig.PumpFor(0.1);
	TestEqual(TEXT("an internal server error on a read is retried, not a failure"),
		static_cast<int32>(Object->GetStatus()), static_cast<int32>(ECrowdyServerObjectStatus::Connecting));

	Rig.Tick(2.f);
	FSentFrame Refused;
	if (!TestTrue(TEXT("the read is retried after the internal error"), Rig.WaitForFrame(KindCall, TEXT("read"), Refused)))
	{
		return false;
	}
	const FString Unreadable = TEXT("unreadable: Id: the server state holds a value players cannot read: not a GUID");
	Rig.Reply(Refused.Rid, ECrowdyNativeExecStatus::AppError, Utf8Bytes(Unreadable));
	Rig.PumpFor(0.1);
	TestEqual(TEXT("a read the server code refused is retried, not a failure"),
		static_cast<int32>(Object->GetStatus()), static_cast<int32>(ECrowdyServerObjectStatus::Connecting));
	TestEqualSensitive(TEXT("the server code's refusal is the reason while it retries"), Object->GetFailureReason(), Unreadable);

	Rig.Tick(4.f);
	FSentFrame Last;
	if (!TestTrue(TEXT("the read is retried again after a doubled backoff"), Rig.WaitForFrame(KindCall, TEXT("read"), Last)))
	{
		return false;
	}
	const FString Hostile = FString(TEXT("\x1b[31mbad")) + FString::ChrN(400, TEXT('x'));
	Rig.Reply(Last.Rid, ECrowdyNativeExecStatus::BadRequest, Utf8Bytes(Hostile));
	Rig.PumpFor(0.1);
	TestEqual(TEXT("a BadRequest read is permanent and fails the object"), static_cast<int32>(Object->GetStatus()), static_cast<int32>(ECrowdyServerObjectStatus::Failed));
	const FString Reason = Object->GetFailureReason();
	TestTrue(FString::Printf(TEXT("server text in the failure reason is capped (%d characters)"), Reason.Len()), Reason.Len() > 0 && Reason.Len() < Hostile.Len());
	TestFalse(TEXT("server text in the failure reason carries no control characters"), Reason.Contains(TEXT("\x1b")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectUnavailableCallIsNotRetriedTest,
	"CrowdySDK.CrowdyExec.ServerObjectUnavailableCallIsNotRetried", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectUnavailableCallIsNotRetriedTest::RunTest(const FString& Parameters)
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
	FCallSpy Call;
	Object->Call(AttackFunction, AttackParams(250), Call.OnDone());
	FSentFrame Sent;
	if (!TestTrue(TEXT("the call is sent"), Rig.WaitForFrame(KindCall, AttackMethod, Sent)))
	{
		return false;
	}
	Rig.Reply(Sent.Rid, ECrowdyNativeExecStatus::Unavailable, Utf8Bytes(TEXT("host down")));
	if (!TestTrue(TEXT("an Unavailable call completes at once, since the server may already have run it"), Rig.PumpUntil([&Call]() { return Call.Num() > 0; })))
	{
		return false;
	}
	const FCrowdyServerCallResult& Result = (*Call.Results)[0];
	TestEqual(TEXT("as Unavailable"), static_cast<int32>(Result.Outcome), static_cast<int32>(ECrowdyServerCallOutcome::Unavailable));
	TestTrue(TEXT("marked retryable, so the caller decides"), Result.bRetryable);
	Rig.Tick(2.f);
	TestFalse(TEXT("the handle never resends it"), Rig.SentAny(KindCall, AttackMethod));
	TestEqual(TEXT("and it completes once"), Call.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectRetryHintIsCappedTest,
	"CrowdySDK.CrowdyExec.ServerObjectRetryHintIsCapped", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectRetryHintIsCappedTest::RunTest(const FString& Parameters)
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
	FCallSpy Call;
	Object->Call(AttackFunction, AttackParams(250), Call.OnDone());
	FSentFrame First;
	if (!TestTrue(TEXT("the call is sent"), Rig.WaitForFrame(KindCall, AttackMethod, First)))
	{
		return false;
	}
	Rig.Reply(First.Rid, ECrowdyNativeExecStatus::Busy, Utf8Bytes(TEXT("rate limited: retry in 100000 ms")));
	Rig.PumpFor(0.1);
	Rig.Tick(29.f);
	TestFalse(TEXT("the hint is honoured up to 30 s"), Rig.SentAny(KindCall, AttackMethod));
	Rig.Tick(1.5f);
	FSentFrame Retry;
	TestTrue(TEXT("a 100 s hint is capped, so the retry goes out after 30 s"), Rig.WaitForFrame(KindCall, AttackMethod, Retry));
	TestEqual(TEXT("the call is still pending"), Call.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectStalePushIsNotDecodedTest,
	"CrowdySDK.CrowdyExec.ServerObjectStalePushIsNotDecoded", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectStalePushIsNotDecodedTest::RunTest(const FString& Parameters)
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
	Rig.Sent.Reset();

	// A covered seq whose fields would not decode: only a decode of it could turn it into a re-read.
	TArray<uint8> Stale;
	FCrowdyExecWriter Writer(Stale);
	Writer.MapHeader(3);
	WriteKey(Writer, "epoch");
	Writer.UInt(Epoch);
	WriteKey(Writer, "seq");
	Writer.UInt(5);
	WriteKey(Writer, "fields");
	Writer.UInt(5);
	Rig.Push(Stale);
	TestFalse(TEXT("a push the read already covered is dropped on its header, never decoded"), Rig.SentAny(KindCall, TEXT("read")));

	const TArray<FString> HealthOnly = {TEXT("Health")};
	Rig.Push(Rig.PushMessage(Epoch, 6, Boss(2500, false, ECrowdyExecTestPhase::Calm), HealthOnly));
	TestTrue(TEXT("the next push in order is decoded into the state"), Rig.PumpUntil([Object]() { return HealthOf(Object) == 2500; }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectRereadDropsBufferedPushesTest,
	"CrowdySDK.CrowdyExec.ServerObjectRereadDropsBufferedPushes", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectRereadDropsBufferedPushesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
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
	FSentFrame Subscribe;
	FSentFrame Read;
	if (!TestTrue(TEXT("the object subscribes"), Rig.WaitForFrame(KindSubscribe, TEXT("state"), Subscribe))
		|| !TestTrue(TEXT("the object reads"), Rig.WaitForFrame(KindCall, TEXT("read"), Read)))
	{
		return false;
	}
	const TArray<FString> HealthOnly = {TEXT("Health")};
	Rig.Reply(Subscribe.Rid, ECrowdyNativeExecStatus::Ok, {});
	Rig.Push(Rig.PushMessage(Epoch, 7, Boss(700, false, ECrowdyExecTestPhase::Calm), HealthOnly));
	Rig.Push(Rig.PushMessage(Epoch, 8, Boss(800, false, ECrowdyExecTestPhase::Calm), HealthOnly));
	Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::Ok, Rig.ReadMessage(5, Boss(500, false, ECrowdyExecTestPhase::Calm)));
	FSentFrame Reread;
	if (!TestTrue(TEXT("the buffered push after a gap requests a new read"), Rig.WaitForFrame(KindCall, TEXT("read"), Reread)))
	{
		return false;
	}
	// Seq 7 on purpose: a push still buffered at seq 8 would then apply on top of it.
	Rig.Reply(Reread.Rid, ECrowdyNativeExecStatus::Ok, Rig.ReadMessage(7, Boss(700, false, ECrowdyExecTestPhase::Calm)));
	TestTrue(TEXT("the new read is applied"), Rig.PumpUntil([Object]() { return HealthOf(Object) != 500; }));
	Rig.PumpFor(0.1);
	TestEqual(TEXT("the pushes buffered behind the re-read were dropped, since the new read covers them"), HealthOf(Object), 700);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectClosedConnectionCompletesOnTickTest,
	"CrowdySDK.CrowdyExec.ServerObjectClosedConnectionCompletesOnTick", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectClosedConnectionCompletesOnTickTest::RunTest(const FString& Parameters)
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
	FCallSpy Call;
	Object->Call(AttackFunction, AttackParams(250), Call.OnDone());
	FSentFrame Sent;
	if (!TestTrue(TEXT("the call is in flight"), Rig.WaitForFrame(KindCall, AttackMethod, Sent)))
	{
		return false;
	}
	FCrowdyNativeExecConnection* Connection = Rig.Subsystem->GetConnection();
	if (!TestNotNull(TEXT("the connection is open"), Connection))
	{
		return false;
	}
	Connection->Close();
	TestEqual(TEXT("a connection closed elsewhere runs no OnDone inside the close"), Call.Num(), 0);
	Rig.Tick(0.1f);
	if (!TestEqual(TEXT("the canceled call completes on the object's next tick"), Call.Num(), 1))
	{
		return false;
	}
	const FCrowdyServerCallResult& Result = (*Call.Results)[0];
	TestEqual(TEXT("as Unavailable"), static_cast<int32>(Result.Outcome), static_cast<int32>(ECrowdyServerCallOutcome::Unavailable));
	TestEqualSensitive(TEXT("saying the connection was closed"), Result.Reason, FString(TEXT("The connection to the server was closed")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectUnsentCallGivesUpTest,
	"CrowdySDK.CrowdyExec.ServerObjectUnsentCallGivesUp", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectUnsentCallGivesUpTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	if (!Object)
	{
		return false;
	}
	FCallSpy Call;
	Object->Call(AttackFunction, AttackParams(250), Call.OnDone());
	Rig.Tick(29.f);
	TestEqual(TEXT("a call with no connection waits up to 30 s"), Call.Num(), 0);
	Rig.Tick(1.5f);
	if (!TestEqual(TEXT("after 30 s without a connection it gives up"), Call.Num(), 1))
	{
		return false;
	}
	const FCrowdyServerCallResult& Result = (*Call.Results)[0];
	TestEqual(TEXT("as Unavailable"), static_cast<int32>(Result.Outcome), static_cast<int32>(ECrowdyServerCallOutcome::Unavailable));
	TestEqualSensitive(TEXT("saying the server could not be reached"), Result.Reason, FString(TEXT("The server could not be reached")));
	TestTrue(TEXT("marked retryable"), Result.bRetryable);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectBinderFromReadyHandlerFiresOnceTest,
	"CrowdySDK.CrowdyExec.ServerObjectBinderFromReadyHandlerFiresOnce", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectBinderFromReadyHandlerFiresOnceTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	if (!Object)
	{
		return false;
	}
	FValuesSpy Late;
	Object->OnStatusChanged.AddLambda([Late](UCrowdyServerObject* Changed, ECrowdyServerObjectStatus NewStatus)
	{
		if (NewStatus == ECrowdyServerObjectStatus::Ready)
		{
			Late.Watch(Changed);
		}
	});
	if (!Rig.Open() || !Rig.AnswerFirstRead(Object, Boss(3000, false, ECrowdyExecTestPhase::Calm), 5))
	{
		return false;
	}
	Rig.PumpFor(0.1);
	TestEqual(TEXT("a listener bound from the Ready handler is called once, by its own catch-up call"), Late.Num(), 1);
	TestTrue(FString::Printf(TEXT("naming every watched value (got %s)"), *Joined(Late.Last())), NamesAre(Late.Last(), AllWatched()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectRefusedSubscribeKeepsBackoffTest,
	"CrowdySDK.CrowdyExec.ServerObjectRefusedSubscribeKeepsBackoff", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectRefusedSubscribeKeepsBackoffTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
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
	const FString Refused = TEXT("This player may not watch this Server Object");
	for (int32 Round = 1; Round <= 2; ++Round)
	{
		FSentFrame Subscribe;
		FSentFrame Read;
		if (!TestTrue(FString::Printf(TEXT("round %d subscribes"), Round), Rig.WaitForFrame(KindSubscribe, TEXT("state"), Subscribe))
			|| !TestTrue(FString::Printf(TEXT("round %d reads"), Round), Rig.WaitForFrame(KindCall, TEXT("read"), Read)))
		{
			return false;
		}
		// The refusal is handled before the read's reply, which then succeeds.
		Rig.Reply(Subscribe.Rid, ECrowdyNativeExecStatus::Denied, Utf8Bytes(TEXT("not client-callable")));
		Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::Ok, Rig.ReadMessage(5, Boss(3000, false, ECrowdyExecTestPhase::Calm)));
		Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; });
		Rig.PumpFor(0.1);
		TestEqualSensitive(FString::Printf(TEXT("round %d: a working read keeps the refused subscribe's reason"), Round), Object->GetFailureReason(), Refused);
		Rig.Tick(1.f);
	}
	TestFalse(TEXT("a working read does not reset the backoff, so the second refusal waits 2 s, not 1 s"), Rig.SentAny(KindSubscribe, TEXT("state")));
	Rig.Tick(1.f);
	TestTrue(TEXT("the subscribe is retried once the doubled backoff has passed"), Rig.SentAny(KindSubscribe, TEXT("state")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectReadBeforeRefusedSubscribeKeepsBackoffTest,
	"CrowdySDK.CrowdyExec.ServerObjectReadBeforeRefusedSubscribeKeepsBackoff", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectReadBeforeRefusedSubscribeKeepsBackoffTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
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
	const FString Refused = TEXT("This player may not watch this Server Object");
	for (int32 Round = 1; Round <= 2; ++Round)
	{
		FSentFrame Subscribe;
		FSentFrame Read;
		if (!TestTrue(FString::Printf(TEXT("round %d subscribes"), Round), Rig.WaitForFrame(KindSubscribe, TEXT("state"), Subscribe))
			|| !TestTrue(FString::Printf(TEXT("round %d reads"), Round), Rig.WaitForFrame(KindCall, TEXT("read"), Read)))
		{
			return false;
		}
		// The read's reply is handled while the subscribe is sent but not yet answered, which then refuses it.
		Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::Ok, Rig.ReadMessage(5, Boss(3000, false, ECrowdyExecTestPhase::Calm)));
		Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; });
		Rig.PumpFor(0.1);
		if (Round == 2)
		{
			TestEqualSensitive(TEXT("a read answered before the subscribe keeps the refused subscribe's reason"), Object->GetFailureReason(), Refused);
		}
		Rig.Reply(Subscribe.Rid, ECrowdyNativeExecStatus::Denied, Utf8Bytes(TEXT("not client-callable")));
		Rig.PumpFor(0.1);
		TestEqualSensitive(FString::Printf(TEXT("round %d: the refused subscribe gives the reason"), Round), Object->GetFailureReason(), Refused);
		Rig.Tick(1.f);
	}
	TestFalse(TEXT("a read answered before the subscribe does not reset the backoff, so the second refusal waits 2 s, not 1 s"), Rig.SentAny(KindSubscribe, TEXT("state")));
	Rig.Tick(1.f);
	TestTrue(TEXT("the subscribe is retried once the doubled backoff has passed"), Rig.SentAny(KindSubscribe, TEXT("state")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectRefusedReadKeepsWatchReasonTest,
	"CrowdySDK.CrowdyExec.ServerObjectRefusedReadKeepsWatchReason", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectRefusedReadKeepsWatchReasonTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
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
	const FString Refused = TEXT("This player may not watch this Server Object");
	FSentFrame Subscribe;
	FSentFrame Read;
	if (!TestTrue(TEXT("the object subscribes"), Rig.WaitForFrame(KindSubscribe, TEXT("state"), Subscribe))
		|| !TestTrue(TEXT("the object reads"), Rig.WaitForFrame(KindCall, TEXT("read"), Read)))
	{
		return false;
	}
	Rig.Reply(Subscribe.Rid, ECrowdyNativeExecStatus::Denied, Utf8Bytes(TEXT("not client-callable")));
	Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::DeadlineExceeded, Utf8Bytes(TEXT("deadline exceeded")));
	Rig.PumpFor(0.1);
	TestEqualSensitive(TEXT("a refused read does not replace the refused subscribe's reason"), Object->GetFailureReason(), Refused);

	// Both refusals count toward one backoff of 2 s. The retried subscribe is refused without a reason of its own.
	Rig.Tick(2.f);
	FSentFrame Resubscribe;
	FSentFrame Reread;
	if (!TestTrue(TEXT("the subscribe is retried"), Rig.WaitForFrame(KindSubscribe, TEXT("state"), Resubscribe))
		|| !TestTrue(TEXT("and so is the read"), Rig.WaitForFrame(KindCall, TEXT("read"), Reread)))
	{
		return false;
	}
	Rig.Reply(Resubscribe.Rid, ECrowdyNativeExecStatus::Busy, Utf8Bytes(TEXT("busy")));
	Rig.Reply(Reread.Rid, ECrowdyNativeExecStatus::Ok, Rig.ReadMessage(5, Boss(3000, false, ECrowdyExecTestPhase::Calm)));
	TestTrue(TEXT("the good read makes the object Ready"), Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; }));
	TestEqualSensitive(TEXT("the reason is still the subscribe's, not the read's"), Object->GetFailureReason(), Refused);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectGoodReadClearsReadReasonTest,
	"CrowdySDK.CrowdyExec.ServerObjectGoodReadClearsReadReason", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectGoodReadClearsReadReasonTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
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
	FSentFrame Subscribe;
	FSentFrame Read;
	if (!TestTrue(TEXT("the object subscribes"), Rig.WaitForFrame(KindSubscribe, TEXT("state"), Subscribe))
		|| !TestTrue(TEXT("the object reads"), Rig.WaitForFrame(KindCall, TEXT("read"), Read)))
	{
		return false;
	}
	// A busy subscribe gives no reason, so the read's refusal becomes the reason. Together they back off 2 s.
	Rig.Reply(Subscribe.Rid, ECrowdyNativeExecStatus::Busy, Utf8Bytes(TEXT("busy")));
	Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::DeadlineExceeded, Utf8Bytes(TEXT("deadline exceeded")));
	Rig.PumpFor(0.1);
	TestEqualSensitive(TEXT("the read's refusal is the reason"), Object->GetFailureReason(), FString(TEXT("The server did not answer in time")));

	// The third refusal backs off 4 s; the good read after it must leave that count alone.
	Rig.Tick(2.f);
	FSentFrame Resubscribe;
	FSentFrame Reread;
	if (!TestTrue(TEXT("the subscribe is retried"), Rig.WaitForFrame(KindSubscribe, TEXT("state"), Resubscribe))
		|| !TestTrue(TEXT("and so is the read"), Rig.WaitForFrame(KindCall, TEXT("read"), Reread)))
	{
		return false;
	}
	Rig.Reply(Resubscribe.Rid, ECrowdyNativeExecStatus::Busy, Utf8Bytes(TEXT("busy")));
	Rig.Reply(Reread.Rid, ECrowdyNativeExecStatus::Ok, Rig.ReadMessage(5, Boss(3000, false, ECrowdyExecTestPhase::Calm)));
	TestTrue(TEXT("the good read makes the object Ready"), Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; }));
	TestTrue(TEXT("a good read clears the read's own reason while the subscribe is still refused"), Object->GetFailureReason().IsEmpty());

	Rig.Tick(4.f);
	FSentFrame Third;
	if (!TestTrue(TEXT("the subscribe is retried after 4 s"), Rig.WaitForFrame(KindSubscribe, TEXT("state"), Third)))
	{
		return false;
	}
	Rig.Reply(Third.Rid, ECrowdyNativeExecStatus::Busy, Utf8Bytes(TEXT("busy")));
	Rig.PumpFor(0.1);
	Rig.Tick(1.f);
	TestFalse(TEXT("the good read kept the backoff, so the fourth refusal waits 8 s, not 1 s"), Rig.SentAny(KindSubscribe, TEXT("state")));
	Rig.Tick(7.f);
	TestTrue(TEXT("the subscribe is retried once that backoff has passed"), Rig.SentAny(KindSubscribe, TEXT("state")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectRebuiltClientRedialsTest,
	"CrowdySDK.CrowdyExec.ServerObjectRebuiltClientRedials", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectRebuiltClientRedialsTest::RunTest(const FString& Parameters)
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

	// Held, as a caller that still references the old client would hold it.
	const TSharedPtr<FCrowdyCppClient> OldClient = Rig.Client;
	const TSharedPtr<FCrowdyCppClient> NewClient = FCrowdyCppClient::MakeForTest(ConnectAnswer, 200);
	if (!TestTrue(TEXT("a second scripted client was built"), NewClient.IsValid()))
	{
		return false;
	}
	Rig.Client = NewClient;
	Rig.Host->SetClientForTest(NewClient, FCrowdyCppClientConfig());
	Rig.Sent.Reset();
	Rig.Subsystem->TickForTest(0.1f);
	if (!TestTrue(TEXT("the next tick dials the rebuilt client"), Rig.PumpUntil([&NewClient]() { return NewClient->NumTestExecConnections() == 1; })))
	{
		return false;
	}
	NewClient->TestExecOpen();
	if (!TestTrue(TEXT("the object reads again on the rebuilt client"), Rig.PumpUntil([&Rig]() { return Rig.FindFrame(KindCall, TEXT("read")) != INDEX_NONE; })))
	{
		return false;
	}
	const int32 SubscribeIndex = Rig.FindFrame(KindSubscribe, TEXT("state"));
	const int32 ReadIndex = Rig.FindFrame(KindCall, TEXT("read"));
	TestTrue(TEXT("it subscribes to the state topic on the rebuilt client before that read"), SubscribeIndex != INDEX_NONE && SubscribeIndex < ReadIndex);
	const FSentFrame Read = Rig.Sent[ReadIndex];
	Rig.AckSubscribes();
	Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::Ok, Rig.ReadMessage(6, Boss(2500, false, ECrowdyExecTestPhase::Calm)));
	TestTrue(TEXT("the rebuilt client's read is applied"), Rig.PumpUntil([Object]() { return HealthOf(Object) == 2500; }));
	TestEqual(TEXT("the object stays Ready across the rebuild"), static_cast<int32>(Object->GetStatus()), static_cast<int32>(ECrowdyServerObjectStatus::Ready));
	TestEqual(TEXT("the old client is not dialled again"), OldClient->NumTestExecConnections(), 1);

	Rig.Shutdown();
	OldClient->Close();
	OldClient->Poll();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectAppChangeRedialsTest,
	"CrowdySDK.CrowdyExec.ServerObjectAppChangeRedials", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectAppChangeRedialsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	if (!Rig.AcquireReady(Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5))
	{
		return false;
	}
	TestEqual(TEXT("one dial so far"), DialsOf(Rig), 1);

	Rig.Session->SetAppID(43);
	Rig.Subsystem->TickForTest(0.1f);
	TestTrue(TEXT("a change of app drops the old app's connection"), Rig.Subsystem->GetConnection() == nullptr);
	if (!TestTrue(TEXT("and dials again on the same client"), Rig.PumpUntil([&Rig]() { return DialsOf(Rig) >= 2; })))
	{
		return false;
	}
	FString Body;
	Rig.Client->GetLastTestRequestBody(Body);
	TestTrue(FString::Printf(TEXT("the new dial asks for the new app (%s)"), *Body), Body.Contains(TEXT("\"43\"")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectFailedMapLoadResumesGraceTest,
	"CrowdySDK.CrowdyExec.ServerObjectFailedMapLoadResumesGrace", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectFailedMapLoadResumesGraceTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	TStrongObjectPtr<UCrowdyServerObject> Object(Rig.Acquire(Rig.InstanceId, Owner.Get()));
	if (!Object)
	{
		return false;
	}
	Rig.Subsystem->SetMapLoadingForTest(true);
	Destroy(Owner);
	Rig.Tick(2.f, 0.1f);
	TestEqual(TEXT("no grace time passes while the map loads"), Rig.Subsystem->NumObjectsForTest(), 1);

	Rig.Subsystem->PostLoadMapForTest(nullptr);
	Rig.Tick(9.5f, 0.1f);
	TestEqual(TEXT("after a failed load the full grace period runs"), Rig.Subsystem->NumObjectsForTest(), 1);
	Rig.Tick(1.f, 0.1f);
	TestEqual(TEXT("a failed load, which reports no world, ends the pause so the object is given back"), Rig.Subsystem->NumObjectsForTest(), 0);
	TestEqual(TEXT("as Released"), static_cast<int32>(Object->GetStatus()), static_cast<int32>(ECrowdyServerObjectStatus::Released));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectAcquireRespectsConnectBackoffTest,
	"CrowdySDK.CrowdyExec.ServerObjectAcquireRespectsConnectBackoff", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectAcquireRespectsConnectBackoffTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	AddExpectedErrorPlain(ConnectFailedWarning, EAutomationExpectedErrorFlags::Contains, 1);
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> First = MakeOwner();
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Second = MakeOwner();
	if (!Rig.Acquire(Rig.InstanceId, First.Get()) || !FailDial(Rig, 1))
	{
		return false;
	}

	TestTrue(TEXT("a new object is handed out during the backoff"), Rig.Acquire(TEXT("boss-2"), First.Get()) != nullptr);
	TestTrue(TEXT("and so is an existing one"), Rig.Acquire(Rig.InstanceId, Second.Get()) != nullptr);
	Rig.PumpFor(0.2);
	TestEqual(TEXT("acquiring during the 1 s backoff does not dial"), DialsOf(Rig), 1);
	Rig.Tick(0.9f, 0.1f);
	Rig.PumpFor(0.2);
	TestEqual(TEXT("nor does ticking 0.9 s of it"), DialsOf(Rig), 1);
	Rig.Tick(0.2f, 0.1f);
	TestTrue(TEXT("the redial comes once the backoff has passed"), Rig.PumpUntil([&Rig]() { return DialsOf(Rig) >= 2; }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectConnectAttemptsResetWhenIdleTest,
	"CrowdySDK.CrowdyExec.ServerObjectConnectAttemptsResetWhenIdle", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectConnectAttemptsResetWhenIdleTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	AddExpectedErrorPlain(ConnectFailedWarning, EAutomationExpectedErrorFlags::Contains, 3);
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	if (!Rig.Acquire(Rig.InstanceId, Owner.Get()) || !FailDial(Rig, 1))
	{
		return false;
	}
	Rig.Tick(1.1f, 0.1f);
	if (!FailDial(Rig, 2))
	{
		return false;
	}
	Rig.Tick(1.1f, 0.1f);
	Rig.PumpFor(0.2);
	TestEqual(TEXT("after two failed dials the next waits 2 s"), DialsOf(Rig), 2);

	// Without a token nothing dials, so the backoff is still counting down when the last object goes.
	Rig.Session->SetGameToken(FString());
	Destroy(Owner);
	for (int32 Step = 0; Step < 150 && Rig.Subsystem->NumObjectsForTest() > 0; ++Step)
	{
		Rig.Subsystem->TickForTest(0.1f);
	}
	if (!TestEqual(TEXT("every object was given back"), Rig.Subsystem->NumObjectsForTest(), 0))
	{
		return false;
	}
	Rig.PumpFor(0.2);
	TestEqual(TEXT("nothing dialled while signed out"), DialsOf(Rig), 2);

	Rig.Session->SetGameToken(TEXT("app-token"));
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Fresh = MakeOwner();
	if (!Rig.Acquire(Rig.InstanceId, Fresh.Get()))
	{
		return false;
	}
	if (!TestTrue(TEXT("once idle, a fresh acquire dials at once"), Rig.PumpUntil([&Rig]() { return DialsOf(Rig) >= 3; })) || !FailDial(Rig, 3))
	{
		return false;
	}
	Rig.Tick(0.9f, 0.1f);
	Rig.PumpFor(0.2);
	TestEqual(TEXT("the first failure after going idle still waits"), DialsOf(Rig), 3);
	Rig.Tick(0.2f, 0.1f);
	TestTrue(TEXT("but only about 1 s, because the attempt count was reset"), Rig.PumpUntil([&Rig]() { return DialsOf(Rig) >= 4; }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectListCallTest,
	"CrowdySDK.CrowdyExec.ServerObjectCallsWithLists", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectListCallTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	const TArray<uint8> StructParams = Rig.Encode(AttackParams(10).Get<FCrowdyServerObjectTestAttackParams>());
	FCrowdyServerObjectTestAttackReply Answer;
	Answer.HealthLeft = 2750;
	Answer.bKilled = true;
	const TArray<uint8> StructReply = Rig.Encode(Answer);

	FCrowdyServerFunction& Attack = Rig.Definition->Functions[0];
	Attack.ParamsForm = ECrowdyServerValuesForm::List;
	Attack.ParamsList.AddProperty(TEXT("Damage"), EPropertyBagPropertyType::Int32);
	Attack.ParamsList.AddProperty(TEXT("Weapon"), EPropertyBagPropertyType::String);
	Attack.ParamsList.SetValueString(TEXT("Weapon"), TEXT("axe"));
	Attack.ReplyForm = ECrowdyServerValuesForm::List;
	Attack.ReplyList.AddProperty(TEXT("HealthLeft"), EPropertyBagPropertyType::Int32);
	Attack.ReplyList.AddProperty(TEXT("bKilled"), EPropertyBagPropertyType::Bool);
	TArray<FString> Errors;
	if (!TestTrue(FString::Printf(TEXT("the List definition bakes (%s)"), *FString::Join(Errors, TEXT("; "))), Rig.Definition->Bake(Errors)))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.AcquireReady(Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5);
	if (!Object)
	{
		return false;
	}
	Rig.Sent.Reset();

	FCallSpy Wrong;
	Object->Call(AttackFunction, AttackParams(10), Wrong.OnDone());
	if (TestEqual(TEXT("a struct for a List is refused before Call returns"), Wrong.Num(), 1))
	{
		TestEqualSensitive(TEXT("naming the List the function takes"), (*Wrong.Results)[0].Reason, FString(TEXT("AttackBoss takes AttackBossParams params")));
	}
	TestNull(TEXT("an unknown function has no params to hand out"), Object->MakeParams(TEXT("Nope")).GetPropertyBagStruct());

	FInstancedPropertyBag Params = Object->MakeParams(AttackFunction);
	const TValueOrError<FString, EPropertyBagResult> Weapon = Params.GetValueString(TEXT("Weapon"));
	TestTrue(TEXT("the handed-out params carry the List's defaults"), Weapon.HasValue() && Weapon.GetValue() == TEXT("axe"));
	Params.SetValueInt32(TEXT("Damage"), 10);
	FCallSpy Call;
	Object->Call(AttackFunction, Params, Call.OnDone());
	FSentFrame Sent;
	if (!TestTrue(TEXT("the List call is sent"), Rig.WaitForFrame(KindCall, AttackMethod, Sent)))
	{
		return false;
	}
	TestTrue(TEXT("carrying exactly the bytes the struct would"), Sent.Payload == StructParams);

	Rig.Reply(Sent.Rid, ECrowdyNativeExecStatus::Ok, StructReply);
	if (!TestTrue(TEXT("the List call completes"), Rig.PumpUntil([&Call]() { return Call.Num() > 0; })))
	{
		return false;
	}
	const FInstancedPropertyBag Reply = CrowdyExec::ToList((*Call.Results)[0].Reply);
	const TValueOrError<int32, EPropertyBagResult> HealthLeft = Reply.GetValueInt32(TEXT("HealthLeft"));
	const TValueOrError<bool, EPropertyBagResult> Killed = Reply.GetValueBool(TEXT("bKilled"));
	TestTrue(TEXT("the reply reads by name"), HealthLeft.HasValue() && HealthLeft.GetValue() == 2750 && Killed.HasValue() && Killed.GetValue());

	Attack.ParamsList.RemovePropertiesByName({TEXT("Damage"), TEXT("Weapon")});
	TestNull(TEXT("a List emptied in the editor hands out nothing, so a call sends nothing"), Object->MakeParams(AttackFunction).GetPropertyBagStruct());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectReshapedStateTest,
	"CrowdySDK.CrowdyExec.ServerObjectRereadsAReshapedState", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectReshapedStateTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	// A List travels like the struct of its values, so the struct definition writes the messages.
	const TArray<uint8> FirstRead = Rig.ReadMessage(5, Boss(3000, false, ECrowdyExecTestPhase::Calm));
	const TArray<uint8> Push = Rig.PushMessage(Epoch, 6, Boss(2500, false, ECrowdyExecTestPhase::Calm), {TEXT("Health")});
	const TArray<uint8> SecondRead = Rig.ReadMessage(6, Boss(2500, false, ECrowdyExecTestPhase::Calm));

	UCrowdyServerObjectDefinition& Definition = *Rig.Definition;
	Definition.StateForm = ECrowdyServerValuesForm::List;
	Definition.StateList.AddProperty(TEXT("Health"), EPropertyBagPropertyType::Int32);
	Definition.StateList.AddProperty(TEXT("bDefeated"), EPropertyBagPropertyType::Bool);
	Definition.StateList.AddProperty(TEXT("Phase"), EPropertyBagPropertyType::Enum, StaticEnum<ECrowdyExecTestPhase>());
	Definition.StateList.AddProperty(TEXT("Secret"), EPropertyBagPropertyType::Int32);
	Definition.StateList.SetValueInt32(TEXT("Secret"), 7);
	TArray<FString> Errors;
	if (!TestTrue(FString::Printf(TEXT("the List State bakes (%s)"), *FString::Join(Errors, TEXT("; "))), Definition.Bake(Errors)))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	FSentFrame Subscribe;
	FSentFrame Read;
	if (!Object || !Rig.Open() || !TestTrue(TEXT("the object subscribes"), Rig.WaitForFrame(KindSubscribe, TEXT("state"), Subscribe))
		|| !TestTrue(TEXT("the object reads"), Rig.WaitForFrame(KindCall, TEXT("read"), Read)))
	{
		return false;
	}
	Rig.Reply(Subscribe.Rid, ECrowdyNativeExecStatus::Ok, {});
	Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::Ok, FirstRead);
	if (!TestTrue(TEXT("the List State becomes Ready"), Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; })))
	{
		return false;
	}
	const FInstancedPropertyBag Values = Object->GetStateList();
	const TValueOrError<int32, EPropertyBagResult> Health = Values.GetValueInt32(TEXT("Health"));
	const TValueOrError<int32, EPropertyBagResult> Secret = Values.GetValueInt32(TEXT("Secret"));
	TestTrue(TEXT("its watched values are read by name"), Health.HasValue() && Health.GetValue() == 3000);
	TestTrue(TEXT("a value players never see keeps the List's starting value"), Secret.HasValue() && Secret.GetValue() == 7);

	// Editing the List while playing gives it a new struct, which the live State is not an instance of.
	Definition.StateList.AddProperty(TEXT("Shield"), EPropertyBagPropertyType::Int32);
	if (!TestTrue(TEXT("the edited List bakes"), Definition.Bake(Errors)))
	{
		return false;
	}
	Rig.Sent.Reset();
	FValuesSpy Spy;
	const FDelegateHandle Watching = Spy.Watch(Object);
	const int32 EventsBefore = Spy.Num();
	Rig.Push(Push);
	FSentFrame Reread;
	TestTrue(TEXT("a push onto the reshaped State is not applied but read again"), Rig.WaitForFrame(KindCall, TEXT("read"), Reread));
	TestEqual(TEXT("and no values event comes from the push"), Spy.Num(), EventsBefore);
	Rig.Reply(Reread.Rid, ECrowdyNativeExecStatus::Ok, SecondRead);
	TestTrue(TEXT("the read gives the State its new shape"),
		Rig.PumpUntil([Object, &Definition]() { return Object->GetState().GetScriptStruct() == Definition.GetStateStruct(); }));
	TestTrue(TEXT("and every watched value counts as changed, since the old shape cannot be compared"),
		Spy.Last() == TArray<FString>({TEXT("Health"), TEXT("bDefeated"), TEXT("Phase")}));
	Object->UnwatchValues(Watching);
	return true;
}

namespace CrowdyServerObjectTests
{
	// These rigs have no world, so registering an async action with its game instance warns.
	const TCHAR* const NoWorldWarning = TEXT("GetWorldFromContextObject");

	const FProperty* PinProperty(FName Pin)
	{
		return FCrowdyServerObjectTestPins::StaticStruct()->FindPropertyByName(Pin);
	}

	bool SetFromPin(FInstancedStruct& Values, FName Name, const FCrowdyServerObjectTestPins& Pins, FName Pin)
	{
		const FProperty* Property = PinProperty(Pin);
		return Property && UCrowdyServerObjectLibrary::SetServerValueFrom(Values, Name, Property, Property->ContainerPtrToValuePtr<void>(&Pins));
	}

	bool GetIntoPin(const FInstancedStruct& Values, FName Name, FCrowdyServerObjectTestPins& Pins, FName Pin)
	{
		const FProperty* Property = PinProperty(Pin);
		return Property && UCrowdyServerObjectLibrary::GetServerValueInto(Values, Name, Property, Property->ContainerPtrToValuePtr<void>(&Pins));
	}

	TStrongObjectPtr<UCrowdyServerObjectTestListener> MakeListener()
	{
		return TStrongObjectPtr<UCrowdyServerObjectTestListener>(NewObject<UCrowdyServerObjectTestListener>(GetTransientPackage(), NAME_None, RF_Transient));
	}

	FCrowdyServerVariablesEvent VariablesEventOf(UCrowdyServerObjectTestListener* Listener)
	{
		FCrowdyServerVariablesEvent Event;
		Event.BindDynamic(Listener, &UCrowdyServerObjectTestListener::HandleVariables);
		return Event;
	}

	TArray<FString> LastVariables(const UCrowdyServerObjectTestListener& Listener)
	{
		return Listener.VariableEvents.IsEmpty() ? TArray<FString>() : Listener.VariableEvents.Last();
	}

	FString StatusNames(const TArray<ECrowdyServerObjectStatus>& Statuses)
	{
		TArray<FString> Names;
		for (const ECrowdyServerObjectStatus Each : Statuses)
		{
			Names.Add(UEnum::GetValueAsString(Each));
		}
		return Joined(Names);
	}

	/** Starts Call Server Function with Listener bound to both outcomes, as a Blueprint graph would. */
	UCrowdyServerCallAction* StartServerCall(UCrowdyServerObject* Object, FName Function, const FInstancedStruct& Inputs, UCrowdyServerObjectTestListener* Listener)
	{
		UCrowdyServerCallAction* Action = UCrowdyServerCallAction::CallServerFunction(nullptr, Object, Function, Inputs);
		if (!Action)
		{
			return nullptr;
		}
		Action->OnSuccess.AddDynamic(Listener, &UCrowdyServerObjectTestListener::HandleSuccess);
		Action->OnFailed.AddDynamic(Listener, &UCrowdyServerObjectTestListener::HandleFailed);
		Action->Activate();
		return Action;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectBlueprintServerValueOnAStructTest,
	"CrowdySDK.CrowdyExec.ServerObjectBlueprint.ServerValueOnAStruct", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectBlueprintServerValueOnAStructTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FCrowdyServerObjectTestPins Pins;
	Pins.Big = 9;
	Pins.Label = TEXT("gold");
	Pins.Count = 42;
	Pins.bFlag = true;
	Pins.Phase = ECrowdyExecTestPhase::Final;
	FInstancedStruct Values = FInstancedStruct::Make(FCrowdyExecTestScalars());
	const FCrowdyExecTestScalars& Target = Values.Get<FCrowdyExecTestScalars>();

	const FProperty* Found = UCrowdyServerObjectLibrary::FindServerValue(Values, TEXT("precise"));
	TestTrue(TEXT("a value is found by its name in any case"), Found && Found->GetName().Equals(TEXT("Precise"), ESearchCase::CaseSensitive));
	TestNull(TEXT("a name the struct lacks finds nothing"), UCrowdyServerObjectLibrary::FindServerValue(Values, TEXT("Nope")));
	TestNull(TEXT("an empty struct finds nothing"), UCrowdyServerObjectLibrary::FindServerValue(FInstancedStruct(), TEXT("Health")));

	TestTrue(TEXT("an int32 is set whatever the name's case"), SetFromPin(Values, TEXT("health"), Pins, TEXT("Count")));
	TestEqual(TEXT("into the value of that name"), Target.Health, 42);
	TestTrue(TEXT("a string is set"), SetFromPin(Values, TEXT("Name"), Pins, TEXT("Label")));
	TestEqualSensitive(TEXT("to the pin's text"), Target.Name, FString(TEXT("gold")));
	TestTrue(TEXT("an enum is set"), SetFromPin(Values, TEXT("PHASE"), Pins, TEXT("Phase")));
	TestEqual(TEXT("to the pin's enum value"), static_cast<int32>(Target.Phase), static_cast<int32>(ECrowdyExecTestPhase::Final));
	TestTrue(TEXT("a bool is set"), SetFromPin(Values, TEXT("bDefeated"), Pins, TEXT("bFlag")));
	TestTrue(TEXT("to the pin's bool"), Target.bDefeated);

	Pins.Big = 5000000000ll;
	TestFalse(TEXT("an int64 pin too large for an int32 value is refused"), SetFromPin(Values, TEXT("Health"), Pins, TEXT("Big")));
	TestEqual(TEXT("which keeps its value"), Target.Health, 42);
	TestFalse(TEXT("an int32 pin is refused for a string value"), SetFromPin(Values, TEXT("Name"), Pins, TEXT("Count")));
	TestEqualSensitive(TEXT("which keeps its text"), Target.Name, FString(TEXT("gold")));
	TestFalse(TEXT("a name the struct lacks is refused"), SetFromPin(Values, TEXT("Missing"), Pins, TEXT("Count")));
	FInstancedStruct Empty;
	TestFalse(TEXT("an empty struct is refused"), SetFromPin(Empty, TEXT("Health"), Pins, TEXT("Count")));
	TestFalse(TEXT("and stays empty"), Empty.IsValid());

	FCrowdyServerObjectTestPins Out;
	Out.Count = 5;
	TestTrue(TEXT("an int32 is read whatever the name's case"), GetIntoPin(Values, TEXT("HEALTH"), Out, TEXT("Count")));
	TestEqual(TEXT("into the pin"), Out.Count, 42);
	TestTrue(TEXT("a string is read"), GetIntoPin(Values, TEXT("name"), Out, TEXT("Label")));
	TestEqualSensitive(TEXT("into the pin"), Out.Label, FString(TEXT("gold")));

	Out.Count = 5;
	Values.GetMutable<FCrowdyExecTestScalars>().Big = -5000000000ll;
	TestFalse(TEXT("an int64 value too small for an int32 pin is refused"), GetIntoPin(Values, TEXT("Big"), Out, TEXT("Count")));
	TestFalse(TEXT("a name the struct lacks is refused for a read"), GetIntoPin(Values, TEXT("Missing"), Out, TEXT("Count")));
	TestFalse(TEXT("an empty struct is refused for a read"), GetIntoPin(FInstancedStruct(), TEXT("Health"), Out, TEXT("Count")));
	TestEqual(TEXT("no refused read writes the pin"), Out.Count, 5);
	TestFalse(TEXT("a string value is refused for an int32 pin"), GetIntoPin(Values, TEXT("Name"), Out, TEXT("Count")));
	TestEqual(TEXT("and leaves the pin alone"), Out.Count, 5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectBlueprintServerValueOnAListTest,
	"CrowdySDK.CrowdyExec.ServerObjectBlueprint.ServerValueOnAList", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectBlueprintServerValueOnAListTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FInstancedPropertyBag List;
	List.AddProperty(TEXT("Gold"), EPropertyBagPropertyType::Int32);
	List.AddProperty(TEXT("Title"), EPropertyBagPropertyType::String);
	List.AddProperty(TEXT("Phase"), EPropertyBagPropertyType::Enum, StaticEnum<ECrowdyExecTestPhase>());
	List.SetValueString(TEXT("Title"), TEXT("none"));
	FInstancedStruct Values;
	Values.InitializeAs(List.GetPropertyBagStruct(), List.GetValue().GetMemory());

	FCrowdyServerObjectTestPins Pins;
	Pins.Count = 42;
	Pins.Phase = ECrowdyExecTestPhase::Enraged;
	TestTrue(TEXT("a List's int32 is set whatever the name's case"), SetFromPin(Values, TEXT("gold"), Pins, TEXT("Count")));
	TestTrue(TEXT("a List's enum is set"), SetFromPin(Values, TEXT("phase"), Pins, TEXT("Phase")));
	TestFalse(TEXT("an int32 pin is refused for a List's string"), SetFromPin(Values, TEXT("Title"), Pins, TEXT("Count")));
	TestFalse(TEXT("a name the List lacks is refused"), SetFromPin(Values, TEXT("Missing"), Pins, TEXT("Count")));

	const FInstancedPropertyBag Read = CrowdyExec::ToList(Values);
	const TValueOrError<int32, EPropertyBagResult> Gold = Read.GetValueInt32(TEXT("Gold"));
	const TValueOrError<ECrowdyExecTestPhase, EPropertyBagResult> Phase = Read.GetValueEnum<ECrowdyExecTestPhase>(TEXT("Phase"));
	const TValueOrError<FString, EPropertyBagResult> Title = Read.GetValueString(TEXT("Title"));
	TestTrue(TEXT("the List holds the int32"), Gold.HasValue() && Gold.GetValue() == 42);
	TestTrue(TEXT("the List holds the enum"), Phase.HasValue() && Phase.GetValue() == ECrowdyExecTestPhase::Enraged);
	TestTrue(TEXT("the refused string keeps its value"), Title.HasValue() && Title.GetValue().Equals(TEXT("none"), ESearchCase::CaseSensitive));

	FCrowdyServerObjectTestPins Out;
	TestTrue(TEXT("a List's value is read by name"), GetIntoPin(Values, TEXT("GOLD"), Out, TEXT("Count")));
	TestEqual(TEXT("into the pin"), Out.Count, 42);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectBlueprintEnumPinHeldAsAByteTest,
	"CrowdySDK.CrowdyExec.ServerObjectBlueprint.EnumPinHeldAsAByte", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectBlueprintEnumPinHeldAsAByteTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FInstancedStruct Values = FInstancedStruct::Make(FCrowdyExecTestScalars());
	const FCrowdyExecTestScalars& Target = Values.Get<FCrowdyExecTestScalars>();

	// A Blueprint pin of an enum is a byte property naming the enum; heap-built so its field class is set.
	TUniquePtr<FByteProperty> PhasePin(new FByteProperty(FFieldVariant(), TEXT("Phase")));
	PhasePin->Enum = StaticEnum<ECrowdyExecTestPhase>();
	uint8 PhaseValue = static_cast<uint8>(ECrowdyExecTestPhase::Enraged);
	TestTrue(TEXT("a byte pin of the value's enum sets an enum value"),
		UCrowdyServerObjectLibrary::SetServerValueFrom(Values, TEXT("Phase"), PhasePin.Get(), &PhaseValue));
	TestEqual(TEXT("to the pin's value"), static_cast<int32>(Target.Phase), static_cast<int32>(ECrowdyExecTestPhase::Enraged));

	Values.GetMutable<FCrowdyExecTestScalars>().Phase = ECrowdyExecTestPhase::Final;
	PhaseValue = 0;
	TestTrue(TEXT("and reads one"), UCrowdyServerObjectLibrary::GetServerValueInto(Values, TEXT("Phase"), PhasePin.Get(), &PhaseValue));
	TestEqual(TEXT("into the pin"), static_cast<int32>(PhaseValue), static_cast<int32>(ECrowdyExecTestPhase::Final));

	TUniquePtr<FByteProperty> StatusPin(new FByteProperty(FFieldVariant(), TEXT("Status")));
	StatusPin->Enum = StaticEnum<ECrowdyServerObjectStatus>();
	uint8 StatusValue = static_cast<uint8>(ECrowdyServerObjectStatus::Ready);
	TestFalse(TEXT("a byte pin of another enum is refused"), UCrowdyServerObjectLibrary::SetServerValueFrom(Values, TEXT("Phase"), StatusPin.Get(), &StatusValue));
	TestEqual(TEXT("which keeps the value"), static_cast<int32>(Target.Phase), static_cast<int32>(ECrowdyExecTestPhase::Final));
	StatusValue = 3;
	TestFalse(TEXT("and cannot read it"), UCrowdyServerObjectLibrary::GetServerValueInto(Values, TEXT("Phase"), StatusPin.Get(), &StatusValue));
	TestEqual(TEXT("which leaves the pin alone"), static_cast<int32>(StatusValue), 3);

	FInstancedStruct Bytes = FInstancedStruct::Make(FCrowdyServerObjectTestByteEnum());
	const FCrowdyServerObjectTestByteEnum& ByteTarget = Bytes.Get<FCrowdyServerObjectTestByteEnum>();
	TUniquePtr<FByteProperty> ChannelPin(new FByteProperty(FFieldVariant(), TEXT("Channel")));
	ChannelPin->Enum = StaticEnum<ECollisionChannel>();
	uint8 ChannelValue = static_cast<uint8>(ECC_Pawn);
	TestTrue(TEXT("a byte pin sets a byte value of the same enum"), UCrowdyServerObjectLibrary::SetServerValueFrom(Bytes, TEXT("Channel"), ChannelPin.Get(), &ChannelValue));
	TestEqual(TEXT("to the pin's value"), static_cast<int32>(ByteTarget.Channel), static_cast<int32>(ECC_Pawn));
	PhaseValue = static_cast<uint8>(ECrowdyExecTestPhase::Enraged);
	TestFalse(TEXT("a byte pin of another enum is refused for a byte value"), UCrowdyServerObjectLibrary::SetServerValueFrom(Bytes, TEXT("Channel"), PhasePin.Get(), &PhaseValue));
	TestEqual(TEXT("which keeps the value"), static_cast<int32>(ByteTarget.Channel), static_cast<int32>(ECC_Pawn));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectBlueprintMakeInputsForAStructTest,
	"CrowdySDK.CrowdyExec.ServerObjectBlueprint.MakeInputsForAStruct", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectBlueprintMakeInputsForAStructTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	FCrowdyServerFunction& Ping = Rig.Definition->Functions.AddDefaulted_GetRef();
	Ping.Name = TEXT("Ping");
	const FString PingMethod = Ping.GetMethodName();
	TArray<FString> Errors;
	const bool bBaked = Rig.Definition->Bake(Errors);
	if (!TestTrue(FString::Printf(TEXT("a function without inputs bakes (%s)"), *FString::Join(Errors, TEXT("; "))), bBaked))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.AcquireReady(Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5);
	if (!Object)
	{
		return false;
	}
	Rig.Sent.Reset();

	FInstancedStruct Inputs = Object->MakeInputs(AttackFunction);
	const FCrowdyServerObjectTestAttackParams* Defaults = Inputs.GetPtr<FCrowdyServerObjectTestAttackParams>();
	TestTrue(TEXT("a function taking a struct hands out that struct"), Inputs.GetScriptStruct() == FCrowdyServerObjectTestAttackParams::StaticStruct());
	TestTrue(TEXT("holding its defaults"), Defaults && Defaults->Damage == 0 && Defaults->Weapon.IsEmpty());
	TestFalse(TEXT("a function without inputs hands out nothing"), Object->MakeInputs(TEXT("Ping")).IsValid());
	TestFalse(TEXT("an unknown function hands out nothing"), Object->MakeInputs(TEXT("Nope")).IsValid());

	FCrowdyServerObjectTestPins Pins;
	Pins.Count = 77;
	Pins.Label = TEXT("sword");
	TestTrue(TEXT("its damage is set by name"), SetFromPin(Inputs, TEXT("damage"), Pins, TEXT("Count")));
	TestTrue(TEXT("its weapon is set by name"), SetFromPin(Inputs, TEXT("Weapon"), Pins, TEXT("Label")));
	FCrowdyServerObjectTestAttackParams Expected;
	Expected.Damage = 77;
	Expected.Weapon = TEXT("sword");

	FCallSpy Call;
	Object->Call(AttackFunction, Inputs, Call.OnDone());
	TestEqual(TEXT("the handed-out inputs pass the call's type check"), Call.Num(), 0);
	FSentFrame Sent;
	if (!TestTrue(TEXT("the call is sent"), Rig.WaitForFrame(KindCall, AttackMethod, Sent)))
	{
		return false;
	}
	TestTrue(TEXT("carrying the values set by name"), Sent.Payload == Rig.Encode(Expected));

	FCallSpy PingCall;
	Object->Call(TEXT("Ping"), Object->MakeInputs(TEXT("Ping")), PingCall.OnDone());
	TestEqual(TEXT("the empty inputs of a function without any pass the type check"), PingCall.Num(), 0);
	FSentFrame PingSent;
	TestTrue(TEXT("and that call is sent"), Rig.WaitForFrame(KindCall, PingMethod, PingSent));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectBlueprintMakeInputsForAListTest,
	"CrowdySDK.CrowdyExec.ServerObjectBlueprint.MakeInputsForAList", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectBlueprintMakeInputsForAListTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	const TArray<uint8> StructParams = Rig.Encode(AttackParams(10).Get<FCrowdyServerObjectTestAttackParams>());
	FCrowdyServerFunction& Attack = Rig.Definition->Functions[0];
	Attack.ParamsForm = ECrowdyServerValuesForm::List;
	Attack.ParamsList.AddProperty(TEXT("Damage"), EPropertyBagPropertyType::Int32);
	Attack.ParamsList.AddProperty(TEXT("Weapon"), EPropertyBagPropertyType::String);
	Attack.ParamsList.SetValueString(TEXT("Weapon"), TEXT("axe"));
	TArray<FString> Errors;
	const bool bBaked = Rig.Definition->Bake(Errors);
	if (!TestTrue(FString::Printf(TEXT("the List definition bakes (%s)"), *FString::Join(Errors, TEXT("; "))), bBaked))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.AcquireReady(Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5);
	if (!Object)
	{
		return false;
	}
	Rig.Sent.Reset();

	FInstancedStruct Inputs = Object->MakeInputs(AttackFunction);
	TestTrue(TEXT("a function taking a List hands out the List's struct"), Inputs.GetScriptStruct() && Inputs.GetScriptStruct() == Attack.GetParamsStruct());
	const TValueOrError<FString, EPropertyBagResult> Weapon = CrowdyExec::ToList(Inputs).GetValueString(TEXT("Weapon"));
	TestTrue(TEXT("holding the List's defaults"), Weapon.HasValue() && Weapon.GetValue().Equals(TEXT("axe"), ESearchCase::CaseSensitive));

	FCrowdyServerObjectTestPins Pins;
	Pins.Count = 10;
	TestTrue(TEXT("a List input is set by name"), SetFromPin(Inputs, TEXT("Damage"), Pins, TEXT("Count")));
	FCallSpy Call;
	Object->Call(AttackFunction, Inputs, Call.OnDone());
	TestEqual(TEXT("the handed-out List passes the call's type check"), Call.Num(), 0);
	FSentFrame Sent;
	if (!TestTrue(TEXT("the call is sent"), Rig.WaitForFrame(KindCall, AttackMethod, Sent)))
	{
		return false;
	}
	TestTrue(TEXT("carrying exactly the bytes the struct would"), Sent.Payload == StructParams);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectBlueprintWatchVariablesTest,
	"CrowdySDK.CrowdyExec.ServerObjectBlueprint.WatchVariables", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectBlueprintWatchVariablesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	if (!Object)
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Early = MakeListener();
	Object->K2_WatchVariables(VariablesEventOf(Early.Get()));
	Object->K2_WatchVariables(FCrowdyServerVariablesEvent());
	TestEqual(TEXT("a watcher bound while Connecting is not run at once"), Early->VariableEvents.Num(), 0);
	if (!Rig.Open() || !Rig.AnswerFirstRead(Object, Boss(3000, false, ECrowdyExecTestPhase::Calm), 5))
	{
		return false;
	}
	Rig.PumpFor(0.1);
	TestEqual(TEXT("the early watcher runs once on the first read"), Early->VariableEvents.Num(), 1);
	TestTrue(FString::Printf(TEXT("naming every watched variable (got %s)"), *Joined(LastVariables(*Early))), NamesAre(LastVariables(*Early), AllWatched()));

	TStrongObjectPtr<UCrowdyServerObjectTestListener> Late = MakeListener();
	Object->K2_WatchVariables(VariablesEventOf(Late.Get()));
	TestEqual(TEXT("a watcher bound after Ready runs once, at once"), Late->VariableEvents.Num(), 1);
	TestTrue(FString::Printf(TEXT("naming every watched variable (got %s)"), *Joined(LastVariables(*Late))), NamesAre(LastVariables(*Late), AllWatched()));
	TestEqual(TEXT("the late watcher's catch-up run reaches no other watcher"), Early->VariableEvents.Num(), 1);

	const TArray<FString> Pushed = {TEXT("Health"), TEXT("bDefeated")};
	Rig.Push(Rig.PushMessage(Epoch, 6, Boss(2500, true, ECrowdyExecTestPhase::Calm), Pushed));
	TestTrue(TEXT("the push runs the watchers"), Rig.PumpUntil([&Late]() { return Late->VariableEvents.Num() >= 2; }));
	Rig.PumpFor(0.1);
	TestEqual(TEXT("a push changing two variables runs the late watcher once"), Late->VariableEvents.Num(), 2);
	TestEqual(TEXT("and the early watcher once"), Early->VariableEvents.Num(), 2);
	TestTrue(FString::Printf(TEXT("naming both variables (got %s)"), *Joined(LastVariables(*Late))), NamesAre(LastVariables(*Late), Pushed));
	TestTrue(FString::Printf(TEXT("for every watcher (got %s)"), *Joined(LastVariables(*Early))), NamesAre(LastVariables(*Early), Pushed));

	Object->K2_UnwatchVariables(VariablesEventOf(Early.Get()));
	const TArray<FString> PhaseOnly = {TEXT("Phase")};
	Rig.Push(Rig.PushMessage(Epoch, 7, Boss(2500, true, ECrowdyExecTestPhase::Final), PhaseOnly));
	TestTrue(TEXT("the next push runs the watcher still bound"), Rig.PumpUntil([&Late]() { return Late->VariableEvents.Num() >= 3; }));
	Rig.PumpFor(0.1);
	TestEqual(TEXT("exactly once"), Late->VariableEvents.Num(), 3);
	TestEqual(TEXT("an unwatched watcher is not run again"), Early->VariableEvents.Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectBlueprintDestroyedWatcherIsDroppedTest,
	"CrowdySDK.CrowdyExec.ServerObjectBlueprint.DestroyedWatcherIsDropped", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectBlueprintDestroyedWatcherIsDroppedTest::RunTest(const FString& Parameters)
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
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Gone = MakeListener();
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Kept = MakeListener();
	Object->K2_WatchVariables(VariablesEventOf(Gone.Get()));
	Object->K2_WatchVariables(VariablesEventOf(Kept.Get()));
	TestEqual(TEXT("both watchers ran their catch-up once"), Gone->VariableEvents.Num() + Kept->VariableEvents.Num(), 2);

	Gone->MarkAsGarbage();
	Gone.Reset();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

	const TArray<FString> HealthOnly = {TEXT("Health")};
	Rig.Push(Rig.PushMessage(Epoch, 6, Boss(2500, false, ECrowdyExecTestPhase::Calm), HealthOnly));
	TestTrue(TEXT("a push after a watcher's object was destroyed still runs the others"), Rig.PumpUntil([&Kept]() { return Kept->VariableEvents.Num() >= 2; }));
	Rig.Push(Rig.PushMessage(Epoch, 7, Boss(2000, false, ECrowdyExecTestPhase::Calm), HealthOnly));
	TestTrue(TEXT("and so does the next one"), Rig.PumpUntil([&Kept]() { return Kept->VariableEvents.Num() >= 3; }));
	Rig.PumpFor(0.1);
	TestEqual(TEXT("once per push"), Kept->VariableEvents.Num(), 3);
	TestTrue(FString::Printf(TEXT("naming the pushed variable (got %s)"), *Joined(LastVariables(*Kept))), NamesAre(LastVariables(*Kept), HealthOnly));
	TestEqual(TEXT("the state holds the last push"), HealthOf(Object), 2000);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectBlueprintStatusChangedMatchesNativeTest,
	"CrowdySDK.CrowdyExec.ServerObjectBlueprint.StatusChangedMatchesNative", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectBlueprintStatusChangedMatchesNativeTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	if (!Object)
	{
		return false;
	}
	FStatusSpy Native;
	Native.Listen(Object);
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Listener = MakeListener();
	Object->StatusChanged.AddDynamic(Listener.Get(), &UCrowdyServerObjectTestListener::HandleStatus);
	if (!Rig.Open() || !Rig.AnswerFirstRead(Object, Boss(3000, false, ECrowdyExecTestPhase::Calm), 5))
	{
		return false;
	}
	TestEqual(TEXT("becoming Ready reaches the Blueprint event once"), Listener->StatusEvents.Num(), 1);

	Rig.Subsystem->SignOutForTest();
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Again = MakeOwner();
	TestTrue(TEXT("acquiring the Failed object again restarts it"), Rig.Acquire(Rig.InstanceId, Again.Get()) == Object);

	const TArray<ECrowdyServerObjectStatus> Expected = {ECrowdyServerObjectStatus::Ready, ECrowdyServerObjectStatus::Failed, ECrowdyServerObjectStatus::Connecting};
	TestTrue(FString::Printf(TEXT("the native event saw Ready, Failed, Connecting (got %s)"), *StatusNames(*Native.Seen)), *Native.Seen == Expected);
	TestTrue(FString::Printf(TEXT("the Blueprint event saw the same, in the same order (got %s)"), *StatusNames(Listener->StatusEvents)),
		Listener->StatusEvents == Expected);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectBlueprintShutdownDoesNotBroadcastTest,
	"CrowdySDK.CrowdyExec.ServerObjectBlueprint.ShutdownDoesNotBroadcast", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectBlueprintShutdownDoesNotBroadcastTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	TStrongObjectPtr<UCrowdyServerObject> Object(Rig.AcquireReady(Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5));
	if (!Object)
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Listener = MakeListener();
	Object->StatusChanged.AddDynamic(Listener.Get(), &UCrowdyServerObjectTestListener::HandleStatus);
	Object->K2_WatchVariables(VariablesEventOf(Listener.Get()));
	// Received but not yet polled, so it lands after the shutdown.
	Rig.Push(Rig.PushMessage(Epoch, 6, Boss(1, true, ECrowdyExecTestPhase::Final), AllWatched()));

	Rig.Deinitialize();
	TestEqual(TEXT("shutting down gives the object back"), static_cast<int32>(Object->GetStatus()), static_cast<int32>(ECrowdyServerObjectStatus::Released));
	TestEqual(TEXT("the release at shutdown is not broadcast to Blueprints"), Listener->StatusEvents.Num(), 0);
	Rig.PumpFor(0.3);
	TestEqual(TEXT("nothing after shutdown reaches the Blueprint status event"), Listener->StatusEvents.Num(), 0);
	TestEqual(TEXT("nothing at or after shutdown runs a Blueprint watcher beyond its catch-up"), Listener->VariableEvents.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectBlueprintCallServerFunctionSucceedsOnceTest,
	"CrowdySDK.CrowdyExec.ServerObjectBlueprint.CallServerFunctionSucceedsOnce", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectBlueprintCallServerFunctionSucceedsOnceTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	AddExpectedMessagePlain(NoWorldWarning, ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
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
	FInstancedStruct Inputs = Object->MakeInputs(AttackFunction);
	FCrowdyServerObjectTestPins Pins;
	Pins.Count = 250;
	Pins.Label = TEXT("axe");
	if (!TestTrue(TEXT("the inputs are set by name"), SetFromPin(Inputs, TEXT("Damage"), Pins, TEXT("Count")) && SetFromPin(Inputs, TEXT("Weapon"), Pins, TEXT("Label"))))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Listener = MakeListener();
	TStrongObjectPtr<UCrowdyServerCallAction> Action(StartServerCall(Object, AttackFunction, Inputs, Listener.Get()));
	if (!TestNotNull(TEXT("Call Server Function makes its action"), Action.Get()))
	{
		return false;
	}
	FSentFrame Sent;
	if (!TestTrue(TEXT("activating it sends the call"), Rig.WaitForFrame(KindCall, AttackMethod, Sent)))
	{
		return false;
	}
	TestTrue(TEXT("carrying the inputs"), Sent.Payload == Rig.Encode(AttackParams(250).Get<FCrowdyServerObjectTestAttackParams>()));
	TestEqual(TEXT("nothing fires before the reply"), Listener->SuccessOutcomes.Num() + Listener->FailedOutcomes.Num(), 0);

	FCrowdyServerObjectTestAttackReply Answer;
	Answer.HealthLeft = 2750;
	Answer.bKilled = true;
	Rig.Reply(Sent.Rid, ECrowdyNativeExecStatus::Ok, Rig.Encode(Answer));
	TestTrue(TEXT("the reply fires On Success"), Rig.PumpUntil([&Listener]() { return Listener->SuccessOutcomes.Num() > 0; }));
	Rig.PumpFor(0.1);
	if (!TestEqual(TEXT("On Success fires exactly once"), Listener->SuccessOutcomes.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("On Failed does not fire"), Listener->FailedOutcomes.Num(), 0);
	TestEqual(TEXT("with the Success outcome"), static_cast<int32>(Listener->SuccessOutcomes[0]), static_cast<int32>(ECrowdyServerCallOutcome::Success));

	FCrowdyServerObjectTestPins Out;
	const FInstancedStruct& Outputs = Listener->SuccessOutputs[0];
	TestTrue(TEXT("its outputs read by name"), GetIntoPin(Outputs, TEXT("HealthLeft"), Out, TEXT("Count")) && GetIntoPin(Outputs, TEXT("bKilled"), Out, TEXT("bFlag")));
	TestEqual(TEXT("holding the reply's int32"), Out.Count, 2750);
	TestTrue(TEXT("and the reply's bool"), Out.bFlag);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectBlueprintCallServerFunctionRefusalsTest,
	"CrowdySDK.CrowdyExec.ServerObjectBlueprint.CallServerFunctionRefusals", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectBlueprintCallServerFunctionRefusalsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	AddExpectedMessagePlain(NoWorldWarning, ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
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
	Rig.Sent.Reset();

	TStrongObjectPtr<UCrowdyServerObjectTestListener> Unknown = MakeListener();
	TStrongObjectPtr<UCrowdyServerCallAction> UnknownAction(StartServerCall(Object, TEXT("Nope"), FInstancedStruct(), Unknown.Get()));
	TestNotNull(TEXT("an unknown function still makes an action"), UnknownAction.Get());
	if (TestEqual(TEXT("an unknown function fires On Failed during Activate"), Unknown->FailedOutcomes.Num(), 1))
	{
		TestEqual(TEXT("as BadRequest"), static_cast<int32>(Unknown->FailedOutcomes[0]), static_cast<int32>(ECrowdyServerCallOutcome::BadRequest));
		TestEqualSensitive(TEXT("naming the missing function"), Unknown->FailedReasons[0], FString(TEXT("test_boss has no function named Nope")));
		TestFalse(TEXT("not retryable"), Unknown->FailedRetryable[0]);
	}

	TStrongObjectPtr<UCrowdyServerObjectTestListener> Nothing = MakeListener();
	TStrongObjectPtr<UCrowdyServerCallAction> NothingAction(StartServerCall(nullptr, AttackFunction, FInstancedStruct(), Nothing.Get()));
	TestNotNull(TEXT("a missing Server Object still makes an action"), NothingAction.Get());
	if (TestEqual(TEXT("a missing Server Object fires On Failed during Activate"), Nothing->FailedOutcomes.Num(), 1))
	{
		TestEqual(TEXT("as BadRequest"), static_cast<int32>(Nothing->FailedOutcomes[0]), static_cast<int32>(ECrowdyServerCallOutcome::BadRequest));
		TestFalse(TEXT("saying why"), Nothing->FailedReasons[0].IsEmpty());
	}

	TestFalse(TEXT("no refused call reaches the wire"), Rig.SentAnyCall());
	TestEqual(TEXT("each refusal fired On Failed once and no more"), Unknown->FailedOutcomes.Num() + Nothing->FailedOutcomes.Num(), 2);
	TestEqual(TEXT("and never On Success"), Unknown->SuccessOutcomes.Num() + Nothing->SuccessOutcomes.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectBlueprintComponentResolvesInstanceIdTest,
	"CrowdySDK.CrowdyExec.ServerObjectBlueprint.ComponentResolvesInstanceId", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectBlueprintComponentResolvesInstanceIdTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	AddExpectedMessagePlain(NoWorldWarning, ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	TStrongObjectPtr<UCrowdyServerObjectComponent> Component(NewObject<UCrowdyServerObjectComponent>(GetTransientPackage(), NAME_None, RF_Transient));

	Component->InstanceMode = ECrowdyServerObjectInstanceMode::InstanceId;
	Component->InstanceId = TEXT("chest-7");
	FString Error;
	TestEqualSensitive(TEXT("Instance Id mode gives the typed Instance Id"), Component->ResolveInstanceId(Error), FString(TEXT("chest-7")));
	TestTrue(FString::Printf(TEXT("with no error (got %s)"), *Error), Error.IsEmpty());

	Component->InstanceId.Reset();
	Error.Reset();
	TestTrue(TEXT("an empty typed Instance Id gives nothing"), Component->ResolveInstanceId(Error).IsEmpty());
	TestFalse(TEXT("saying why"), Error.IsEmpty());

	Component->InstanceMode = ECrowdyServerObjectInstanceMode::ThisActor;
	Error.Reset();
	TestTrue(TEXT("This Actor without a placed actor gives nothing"), Component->ResolveInstanceId(Error).IsEmpty());
	TestFalse(TEXT("saying why"), Error.IsEmpty());

	Component->InstanceMode = ECrowdyServerObjectInstanceMode::SignedInPlayer;
	Error.Reset();
	TestTrue(TEXT("Signed-In Player without a session gives nothing"), Component->ResolveInstanceId(Error).IsEmpty());
	TestEqualSensitive(TEXT("saying nobody is signed in"), Error, FString(TEXT("Nobody is signed in yet")));

	TestTrue(TEXT("with no world there is no player Instance Id"), UCrowdyServerObjectLibrary::GetPlayerInstanceId(nullptr).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectBlueprintLookupNormalisesNamesTest,
	"CrowdySDK.CrowdyExec.ServerObjectBlueprint.LookupNormalisesNames", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectBlueprintLookupNormalisesNamesTest::RunTest(const FString& Parameters)
{
	// A cooked Blueprint struct knows "Max Health" only as MaxHealth, so both sides drop spaces and punctuation.
	const FInstancedStruct Values = FInstancedStruct::Make(FCrowdyExecTestScalars());
	const FProperty* Found = UCrowdyServerObjectLibrary::FindServerValue(Values, TEXT("hea lth!"));
	TestTrue(TEXT("a name is matched without its spaces and punctuation"), Found && Found->GetFName() == GET_MEMBER_NAME_CHECKED(FCrowdyExecTestScalars, Health));

	const FInstancedStruct Transient = FInstancedStruct::Make(FCrowdyServerObjectTestTransient());
	TestNotNull(TEXT("a value that travels is found"), UCrowdyServerObjectLibrary::FindServerValue(Transient, TEXT("Kept")));
	TestNull(TEXT("a value that never travels is not"), UCrowdyServerObjectLibrary::FindServerValue(Transient, TEXT("Scratch")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectBlueprintFloatMeetsDoublePinTest,
	"CrowdySDK.CrowdyExec.ServerObjectBlueprint.FloatMeetsDoublePin", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectBlueprintFloatMeetsDoublePinTest::RunTest(const FString& Parameters)
{
	FInstancedStruct Values = FInstancedStruct::Make(FCrowdyExecTestScalars());
	TUniquePtr<FDoubleProperty> Pin(new FDoubleProperty(FFieldVariant(), TEXT("Pin")));
	double PinValue = 2.25;
	TestTrue(TEXT("a Blueprint Float pin (a double) sets a C++ float value"),
		UCrowdyServerObjectLibrary::SetServerValueFrom(Values, TEXT("Speed"), Pin.Get(), &PinValue));
	TestEqual(TEXT("to the pin's value"), Values.Get<FCrowdyExecTestScalars>().Speed, 2.25f);

	Values.GetMutable<FCrowdyExecTestScalars>().Speed = 1.5f;
	PinValue = 0.0;
	TestTrue(TEXT("and reads one"), UCrowdyServerObjectLibrary::GetServerValueInto(Values, TEXT("Speed"), Pin.Get(), &PinValue));
	TestEqual(TEXT("into the pin"), PinValue, 1.5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectBlueprintMakeInputsIsNotPureTest,
	"CrowdySDK.CrowdyExec.ServerObjectBlueprint.MakeInputsIsNotPure", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectBlueprintMakeInputsIsNotPureTest::RunTest(const FString& Parameters)
{
	// A pure node runs again for each node that reads it, so the Inputs Set Server Value changed would be made afresh for the call.
	const UFunction* Function = UCrowdyServerObject::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UCrowdyServerObject, MakeInputs));
	if (!TestNotNull(TEXT("Make Inputs is a Blueprint function"), Function))
	{
		return false;
	}
	TestTrue(TEXT("that Blueprint can call"), Function->HasAnyFunctionFlags(FUNC_BlueprintCallable));
	TestFalse(TEXT("and that is not pure, so the Inputs a graph sets are the ones it sends"), Function->HasAnyFunctionFlags(FUNC_BlueprintPure));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectBlueprintStaleStatusIsNotAnnouncedTest,
	"CrowdySDK.CrowdyExec.ServerObjectBlueprint.StaleStatusIsNotAnnounced", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectBlueprintStaleStatusIsNotAnnouncedTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	if (!Object)
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Listener = MakeListener();
	Object->StatusChanged.AddDynamic(Listener.Get(), &UCrowdyServerObjectTestListener::HandleStatus);
	if (!Rig.Open() || !Rig.AnswerFirstRead(Object, Boss(3000, false, ECrowdyExecTestPhase::Calm), 5))
	{
		return false;
	}

	// A native listener rejoins the moment it Fails, which restarts it while the Failed announcement is still running.
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Again = MakeOwner();
	FRig* RigPtr = &Rig;
	const UCrowdyServerObjectTestOwner* AgainOwner = Again.Get();
	Object->OnStatusChanged.AddLambda([RigPtr, AgainOwner](UCrowdyServerObject*, ECrowdyServerObjectStatus NewStatus)
	{
		if (NewStatus == ECrowdyServerObjectStatus::Failed)
		{
			RigPtr->Acquire(RigPtr->InstanceId, AgainOwner);
		}
	});
	Rig.Subsystem->SignOutForTest();

	TestEqual(TEXT("the object ends Connecting"), Object->GetStatus(), ECrowdyServerObjectStatus::Connecting);
	const TArray<ECrowdyServerObjectStatus> Expected = {ECrowdyServerObjectStatus::Ready, ECrowdyServerObjectStatus::Connecting};
	TestTrue(FString::Printf(TEXT("Blueprint never hears the Failed that was already over, last (got %s)"), *StatusNames(Listener->StatusEvents)),
		Listener->StatusEvents == Expected);
	return true;
}

namespace CrowdyServerObjectTests
{
	/** Over 2^53, so an id that passed through a double would come back changed. */
	constexpr int64 BigUserId = 9007199254740993ll;

	/** Waits for a call of Method to go out, answers it, and waits for its OnDone. */
	bool AnswerMemberCall(FRig& Rig, const FString& Method, ECrowdyNativeExecStatus Status, const TArray<uint8>& Payload, const FCallSpy& Spy,
		FSentFrame* OutFrame = nullptr)
	{
		FSentFrame Frame;
		if (!Rig.Test.TestTrue(FString::Printf(TEXT("%s is sent"), *Method), Rig.WaitForFrame(KindCall, Method, Frame)))
		{
			return false;
		}
		if (OutFrame)
		{
			*OutFrame = Frame;
		}
		const int32 Before = Spy.Num();
		Rig.Reply(Frame.Rid, Status, Payload);
		return Rig.Test.TestTrue(FString::Printf(TEXT("%s completes"), *Method), Rig.PumpUntil([&Spy, Before]() { return Spy.Num() > Before; }));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectMembersReadDecodesIntoGettersTest,
	"CrowdySDK.CrowdyExec.ServerObjectMembers.ReadDecodesIntoGetters", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectMembersReadDecodesIntoGettersTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this, ECrowdyServerObjectVisibility::Public, ECrowdyServerMembersSource::ThisObject);
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
	FValuesSpy Values;
	Values.Watch(Object);
	FMembersKeys Keys;
	Keys.Members = TArray<int64>{11, BigUserId, 33};
	Keys.MemberCount = 3;
	Keys.Leader = BigUserId;
	Keys.bOpen = false;
	Keys.bIsMember = true;
	Keys.bIsLeader = true;
	if (!Rig.AnswerFirstRead(Object, Boss(3000, false, ECrowdyExecTestPhase::Calm), 5, Keys))
	{
		return false;
	}
	TestTrue(TEXT("Get Members holds the read's ids in join order"), Object->GetMembers() == TArray<int64>({11, BigUserId, 33}));
	TestEqual(TEXT("Get Member Count holds the read's count"), Object->GetMemberCount(), 3);
	TestEqual(TEXT("Get Leader holds the read's leader as a whole int64"), Object->GetLeader(), BigUserId);
	TestFalse(TEXT("Is Open For Joining holds the read's open"), Object->IsOpenForJoining());
	TestTrue(TEXT("Is Member holds the read's is_member"), Object->IsMember());
	TestTrue(TEXT("Is Leader holds the read's is_leader"), Object->IsLeader());
	TArray<FString> FirstNames = AllWatched();
	FirstNames.Append({TEXT("Members"), TEXT("MemberCount"), TEXT("Leader"), TEXT("OpenForJoining")});
	TestEqual(TEXT("the first read raised one event"), Values.Num(), 1);
	TestTrue(FString::Printf(TEXT("naming the variables and each members key (got %s)"), *Joined(Values.Last())), NamesAre(Values.Last(), FirstNames));

	// Another epoch forces a read, and that read carries only open, so every other key goes back to its default.
	Rig.Sent.Reset();
	Rig.Push(Rig.StateMessage(false, Epoch + 1, 6, nullptr, {}));
	FSentFrame Read;
	if (!TestTrue(TEXT("a push from another epoch triggers a read"), Rig.WaitForFrame(KindCall, TEXT("read"), Read)))
	{
		return false;
	}
	FMembersKeys OpenOnly;
	OpenOnly.bOpen = true;
	Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::Ok, Rig.ReadMessage(6, Boss(3000, false, ECrowdyExecTestPhase::Calm), OpenOnly));
	TestTrue(TEXT("the second read raises an event"), Rig.PumpUntil([&Values]() { return Values.Num() >= 2; }));
	Rig.PumpFor(0.1);
	TestEqual(TEXT("the second read raised exactly one event"), Values.Num(), 2);
	const TArray<FString> ResetNames = {TEXT("Members"), TEXT("MemberCount"), TEXT("Leader"), TEXT("OpenForJoining")};
	TestTrue(FString::Printf(TEXT("naming each members key that changed, and no unchanged variable (got %s)"), *Joined(Values.Last())),
		NamesAre(Values.Last(), ResetNames));
	TestTrue(TEXT("a read without members empties Get Members"), Object->GetMembers().IsEmpty());
	TestEqual(TEXT("a read without member_count resets Get Member Count"), Object->GetMemberCount(), 0);
	TestEqual(TEXT("a read without leader resets Get Leader"), Object->GetLeader(), static_cast<int64>(0));
	TestTrue(TEXT("a read with open true reopens joining"), Object->IsOpenForJoining());
	TestFalse(TEXT("a read without is_member clears Is Member"), Object->IsMember());
	TestFalse(TEXT("a read without is_leader clears Is Leader"), Object->IsLeader());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectMembersEveryPlayerPushAppliesTest,
	"CrowdySDK.CrowdyExec.ServerObjectMembers.EveryPlayerPushAppliesWithoutRead", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectMembersEveryPlayerPushAppliesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this, ECrowdyServerObjectVisibility::Public, ECrowdyServerMembersSource::ThisObject);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	FMembersKeys Keys;
	Keys.Members = TArray<int64>{11};
	Keys.MemberCount = 1;
	Keys.Leader = 11;
	Keys.bIsMember = true;
	UCrowdyServerObject* Object = Rig.AcquireReady(Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5, Keys);
	if (!Object)
	{
		return false;
	}
	FValuesSpy Values;
	Values.Watch(Object);
	Rig.Sent.Reset();

	FMembersKeys Roster;
	Roster.Members = TArray<int64>{11, 22};
	Roster.MemberCount = 2;
	Roster.Leader = 11;
	Roster.bOpen = false;
	Rig.Push(Rig.StateMessage(false, Epoch, 6, nullptr, {}, Roster));
	TestTrue(TEXT("a roster push raises an event"), Rig.PumpUntil([&Values]() { return Values.Num() >= 2; }));
	Rig.PumpFor(0.1);
	TestEqual(TEXT("a roster push raises exactly one event"), Values.Num(), 2);
	const TArray<FString> RosterNames = {TEXT("Members"), TEXT("MemberCount"), TEXT("OpenForJoining")};
	TestTrue(FString::Printf(TEXT("naming what changed, not the unchanged leader (got %s)"), *Joined(Values.Last())), NamesAre(Values.Last(), RosterNames));
	TestTrue(TEXT("the pushed list is applied"), Object->GetMembers() == TArray<int64>({11, 22}));
	TestEqual(TEXT("the pushed count is applied"), Object->GetMemberCount(), 2);
	TestFalse(TEXT("the pushed open is applied"), Object->IsOpenForJoining());
	TestTrue(TEXT("with no signed-in user id, Is Member stays as the read said"), Object->IsMember());
	TestFalse(TEXT("the push is applied without a read"), Rig.SentAny(KindCall, TEXT("read")));

	// A hidden list sends only the count.
	FMembersKeys CountOnly;
	CountOnly.MemberCount = 900;
	Rig.Push(Rig.StateMessage(false, Epoch, 7, nullptr, {}, CountOnly));
	TestTrue(TEXT("a count-only push raises an event"), Rig.PumpUntil([&Values]() { return Values.Num() >= 3; }));
	TestTrue(FString::Printf(TEXT("naming only the count (got %s)"), *Joined(Values.Last())), NamesAre(Values.Last(), {TEXT("MemberCount")}));
	TestEqual(TEXT("the pushed count is applied"), Object->GetMemberCount(), 900);
	TestTrue(TEXT("a push without members keeps the list"), Object->GetMembers() == TArray<int64>({11, 22}));
	TestEqual(TEXT("a push without leader keeps the leader"), Object->GetLeader(), static_cast<int64>(11));
	TestFalse(TEXT("a push without open keeps it closed"), Object->IsOpenForJoining());
	TestFalse(TEXT("neither push forced a read"), Rig.SentAny(KindCall, TEXT("read")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectMembersVisibilityRereadsOnPushTest,
	"CrowdySDK.CrowdyExec.ServerObjectMembers.MembersVisibilityRereadsOnPush", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectMembersVisibilityRereadsOnPushTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this, ECrowdyServerObjectVisibility::Members, ECrowdyServerMembersSource::ThisObject);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	FMembersKeys Keys;
	Keys.MemberCount = 1;
	Keys.bIsMember = true;
	UCrowdyServerObject* Object = Rig.AcquireReady(Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5, Keys);
	if (!Object)
	{
		return false;
	}
	Rig.Sent.Reset();

	Rig.Push(Rig.StateMessage(false, Epoch, 6, nullptr, {}));
	FSentFrame Read;
	if (!TestTrue(TEXT("a push on a Members object, which carries no values, triggers a read"), Rig.WaitForFrame(KindCall, TEXT("read"), Read)))
	{
		return false;
	}
	FMembersKeys Grown;
	Grown.MemberCount = 2;
	Grown.bIsMember = true;
	Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::Ok, Rig.ReadMessage(6, Boss(2800, false, ECrowdyExecTestPhase::Calm), Grown));
	TestTrue(TEXT("the read it triggered delivers the new count and variables"),
		Rig.PumpUntil([Object]() { return Object->GetMemberCount() == 2 && HealthOf(Object) == 2800; }));
	TestTrue(TEXT("and the member stays a member"), Object->IsMember());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectMembersNonMemberReadIsReadyTest,
	"CrowdySDK.CrowdyExec.ServerObjectMembers.NonMemberReadIsReadyWithDefaults", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectMembersNonMemberReadIsReadyTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this, ECrowdyServerObjectVisibility::Members, ECrowdyServerMembersSource::ThisObject);
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
	FValuesSpy Values;
	Values.Watch(Object);
	FSentFrame Read;
	if (!TestTrue(TEXT("the object reads"), Rig.WaitForFrame(KindCall, TEXT("read"), Read)))
	{
		return false;
	}
	Rig.AckSubscribes();
	// No members list (hidden), and a leader and open at their defaults, which a first read still names.
	FMembersKeys Keys;
	Keys.MemberCount = 2;
	Keys.Leader = 0;
	Keys.bOpen = true;
	Keys.bIsMember = false;
	Keys.bIsLeader = false;
	Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::Ok, Rig.EmptyFieldsRead(5, Keys));
	if (!TestTrue(TEXT("a non-member's read with no variables makes the object Ready"),
		Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; })))
	{
		return false;
	}
	const FCrowdyExecTestBossState Defaults;
	const FCrowdyExecTestBossState* State = StateOf(Object);
	TestTrue(TEXT("the variables keep their defaults"),
		State && State->Health == Defaults.Health && State->bDefeated == Defaults.bDefeated && State->Phase == Defaults.Phase);
	TestEqual(TEXT("the member count is read"), Object->GetMemberCount(), 2);
	TestTrue(TEXT("no list was sent, so Get Members is empty"), Object->GetMembers().IsEmpty());
	TestFalse(TEXT("the reader is not a member"), Object->IsMember());
	TArray<FString> Names = AllWatched();
	Names.Append({TEXT("MemberCount"), TEXT("Leader"), TEXT("OpenForJoining")});
	TestTrue(FString::Printf(TEXT("the first read names each key it carried and not the list it left out (got %s)"), *Joined(Values.Last())),
		NamesAre(Values.Last(), Names));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectMembersIsMemberFollowsListAndCallsTest,
	"CrowdySDK.CrowdyExec.ServerObjectMembers.IsMemberFollowsListAndCalls", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectMembersIsMemberFollowsListAndCallsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this, ECrowdyServerObjectVisibility::Public, ECrowdyServerMembersSource::ThisObject);
	if (!Rig.IsValid())
	{
		return false;
	}
	Rig.Session->SetUserID(77);
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	FMembersKeys Keys;
	Keys.Members = TArray<int64>{5};
	Keys.MemberCount = 1;
	Keys.Leader = 5;
	UCrowdyServerObject* Object = Rig.AcquireReady(Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5, Keys);
	if (!Object)
	{
		return false;
	}
	TestFalse(TEXT("the read says the player is not a member"), Object->IsMember());

	FMembersKeys WithPlayer;
	WithPlayer.Members = TArray<int64>{5, 77};
	WithPlayer.MemberCount = 2;
	Rig.Push(Rig.StateMessage(false, Epoch, 6, nullptr, {}, WithPlayer));
	TestTrue(TEXT("a shown list holding the player's id makes it a member"), Rig.PumpUntil([Object]() { return Object->IsMember(); }));
	TestFalse(TEXT("but not the leader"), Object->IsLeader());

	FMembersKeys Led;
	Led.Leader = 77;
	Rig.Push(Rig.StateMessage(false, Epoch, 7, nullptr, {}, Led));
	TestTrue(TEXT("a pushed leader equal to the player's id makes it the leader"), Rig.PumpUntil([Object]() { return Object->IsLeader(); }));

	FMembersKeys WithoutPlayer;
	WithoutPlayer.Members = TArray<int64>{5};
	WithoutPlayer.MemberCount = 1;
	WithoutPlayer.Leader = 5;
	Rig.Push(Rig.StateMessage(false, Epoch, 8, nullptr, {}, WithoutPlayer));
	TestTrue(TEXT("a shown list without the player's id ends its membership"), Rig.PumpUntil([Object]() { return !Object->IsMember(); }));
	TestFalse(TEXT("and another leader ends its leadership"), Object->IsLeader());

	FCallSpy Refused;
	Object->Call(TEXT("Join"), FInstancedStruct(), Refused.OnDone());
	FSentFrame JoinFrame;
	if (!AnswerMemberCall(Rig, TEXT("join"), ECrowdyNativeExecStatus::AppError, Utf8Bytes(TEXT("It is full")), Refused, &JoinFrame))
	{
		return false;
	}
	TestTrue(TEXT("Join sends method join with an empty map"), JoinFrame.Payload == TArray<uint8>({0x80}));
	TestFalse(TEXT("a refused Join leaves the player out"), Object->IsMember());

	// The shown list is pushed to every player here, so it alone decides membership after a Join or Leave.
	Rig.Sent.Reset();
	FCallSpy Accepted;
	Object->Call(TEXT("Join"), FInstancedStruct(), Accepted.OnDone());
	if (!AnswerMemberCall(Rig, TEXT("join"), ECrowdyNativeExecStatus::Ok, {}, Accepted))
	{
		return false;
	}
	TestEqual(TEXT("the Join succeeds"), (*Accepted.Results)[0].Outcome, ECrowdyServerCallOutcome::Success);
	TestFalse(TEXT("a Join that succeeded leaves Is Member to the pushed list"), Object->IsMember());
	TestFalse(TEXT("and reads nothing, since the push brings the list"), Rig.SentAny(KindCall, TEXT("read")));
	Rig.Push(Rig.StateMessage(false, Epoch, 9, nullptr, {}, WithPlayer));
	TestTrue(TEXT("the pushed list with the player makes it a member"), Rig.PumpUntil([Object]() { return Object->IsMember(); }));

	Rig.Push(Rig.StateMessage(false, Epoch, 10, nullptr, {}, Led));
	TestTrue(TEXT("the player leads again"), Rig.PumpUntil([Object]() { return Object->IsLeader(); }));
	FCallSpy Left;
	Object->Call(TEXT("Leave"), FInstancedStruct(), Left.OnDone());
	if (!AnswerMemberCall(Rig, TEXT("leave"), ECrowdyNativeExecStatus::Ok, {}, Left))
	{
		return false;
	}
	TestTrue(TEXT("a Leave that succeeded leaves Is Member to the pushed list"), Object->IsMember());
	Rig.Push(Rig.StateMessage(false, Epoch, 11, nullptr, {}, WithoutPlayer));
	TestTrue(TEXT("the pushed list without the player ends membership"), Rig.PumpUntil([Object]() { return !Object->IsMember(); }));
	TestFalse(TEXT("and leadership"), Object->IsLeader());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectMembersJoinRereadsTest,
	"CrowdySDK.CrowdyExec.ServerObjectMembers.JoinAndLeaveRereadWithoutPushedList", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectMembersJoinRereadsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this, ECrowdyServerObjectVisibility::Members, ECrowdyServerMembersSource::ThisObject);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	FMembersKeys Outside;
	Outside.MemberCount = 0;
	Outside.bIsMember = false;
	Outside.bIsLeader = false;
	UCrowdyServerObject* Object = Rig.AcquireReady(Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5, Outside);
	if (!Object)
	{
		return false;
	}
	Rig.Sent.Reset();

	FCallSpy Joined;
	Object->Call(TEXT("Join"), FInstancedStruct(), Joined.OnDone());
	if (!AnswerMemberCall(Rig, TEXT("join"), ECrowdyNativeExecStatus::Ok, {}, Joined))
	{
		return false;
	}
	TestFalse(TEXT("a Join that succeeded does not guess Is Member"), Object->IsMember());
	FSentFrame Read;
	if (!TestTrue(TEXT("it reads membership from the server instead"), Rig.WaitForFrame(KindCall, TEXT("read"), Read)))
	{
		return false;
	}
	FMembersKeys Leading;
	Leading.MemberCount = 1;
	Leading.Leader = 77;
	Leading.bIsMember = true;
	Leading.bIsLeader = true;
	Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::Ok, Rig.ReadMessage(6, Boss(3000, false, ECrowdyExecTestPhase::Calm), Leading));
	TestTrue(TEXT("the read makes the player a member"), Rig.PumpUntil([Object]() { return Object->IsMember(); }));
	TestTrue(TEXT("and, as the only member, the leader"), Object->IsLeader());

	FCallSpy Left;
	Object->Call(TEXT("Leave"), FInstancedStruct(), Left.OnDone());
	if (!AnswerMemberCall(Rig, TEXT("leave"), ECrowdyNativeExecStatus::Ok, {}, Left)
		|| !TestTrue(TEXT("a Leave that succeeded reads membership too"), Rig.WaitForFrame(KindCall, TEXT("read"), Read)))
	{
		return false;
	}
	TestTrue(TEXT("until that read answers, Is Member is as last read"), Object->IsMember());
	Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::Ok, Rig.ReadMessage(7, Boss(3000, false, ECrowdyExecTestPhase::Calm), Outside));
	TestTrue(TEXT("the read ends membership"), Rig.PumpUntil([Object]() { return !Object->IsMember(); }));
	TestFalse(TEXT("and leadership"), Object->IsLeader());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectMembersKeysFollowDefinitionTest,
	"CrowdySDK.CrowdyExec.ServerObjectMembers.KeysFollowTheDefinition", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectMembersKeysFollowDefinitionTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FMembersKeys Everything;
	Everything.Members = TArray<int64>{77};
	Everything.MemberCount = 4;
	Everything.Leader = 77;
	Everything.bOpen = false;
	Everything.bIsMember = true;
	Everything.bIsLeader = true;
	{
		FRig Rig(*this, ECrowdyServerObjectVisibility::Public, ECrowdyServerMembersSource::ThisObject);
		Rig.Definition->bShowMembers = false;
		Rig.Session->SetUserID(77);
		TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
		UCrowdyServerObject* Object = Rig.IsValid() ? Rig.AcquireReady(Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5, Everything) : nullptr;
		if (!Object)
		{
			return false;
		}
		TestTrue(TEXT("a hidden list ignores a members list the read carried"), Object->GetMembers().IsEmpty());
		TestEqual(TEXT("but keeps the count"), Object->GetMemberCount(), 4);
		TestTrue(TEXT("and Is Member from the read"), Object->IsMember());
		FMembersKeys Pushed;
		Pushed.Members = TArray<int64>{5};
		Rig.Push(Rig.StateMessage(false, Epoch, 6, nullptr, {}, Pushed));
		Rig.PumpFor(0.2);
		TestTrue(TEXT("a hidden list ignores a pushed members list"), Object->GetMembers().IsEmpty());
		TestTrue(TEXT("and does not take Is Member from it"), Object->IsMember());
	}
	{
		FRig Rig(*this);
		TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
		UCrowdyServerObject* Object = Rig.IsValid() ? Rig.AcquireReady(Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5, Everything) : nullptr;
		if (!Object)
		{
			return false;
		}
		TestTrue(TEXT("with no members, every members key is ignored"), Object->GetMembers().IsEmpty() && Object->GetMemberCount() == 0
			&& Object->GetLeader() == 0 && Object->IsOpenForJoining() && !Object->IsMember() && !Object->IsLeader());
	}
	{
		FRig Rig(*this, ECrowdyServerObjectVisibility::Public, ECrowdyServerMembersSource::CrowdyTeam);
		TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
		UCrowdyServerObject* Object = Rig.IsValid() ? Rig.AcquireReady(Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5, Everything) : nullptr;
		if (!Object)
		{
			return false;
		}
		TestTrue(TEXT("a Crowdy Team object ignores the keys only an object's own members send"), Object->GetMembers().IsEmpty()
			&& Object->GetMemberCount() == 0 && Object->GetLeader() == 0 && Object->IsOpenForJoining());
		TestTrue(TEXT("but takes Is Member and Is Leader"), Object->IsMember() && Object->IsLeader());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectMembersDeniedAndCooldownTest,
	"CrowdySDK.CrowdyExec.ServerObjectMembers.DeniedAndCooldownOutcomes", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectMembersDeniedAndCooldownTest::RunTest(const FString& Parameters)
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
	struct FRefusalCase
	{
		const TCHAR* Message;
		ECrowdyServerCallOutcome Outcome;
		bool bRetryable;
		const TCHAR* Reason;
	};
	const FRefusalCase Cases[] = {
		{TEXT("denied: only members may call AttackBoss "), ECrowdyServerCallOutcome::Denied, false, TEXT("only members may call AttackBoss")},
		{TEXT("cooldown: AttackBoss can be called again in 3 s"), ECrowdyServerCallOutcome::Denied, true, TEXT("AttackBoss can be called again in 3 s")},
		{TEXT("denied:"), ECrowdyServerCallOutcome::Denied, false, TEXT("This player may not call AttackBoss on this Server Object")},
		{TEXT("bad_params: Damage must be 1 to 100"), ECrowdyServerCallOutcome::ServerError, false, TEXT("bad_params: Damage must be 1 to 100")},
		{TEXT("Denied: the token is lowercase"), ECrowdyServerCallOutcome::ServerError, false, TEXT("Denied: the token is lowercase")}};
	for (const FRefusalCase& Case : Cases)
	{
		FCallSpy Call;
		Object->Call(AttackFunction, AttackParams(10), Call.OnDone());
		if (!AnswerMemberCall(Rig, AttackMethod, ECrowdyNativeExecStatus::AppError, Utf8Bytes(Case.Message), Call))
		{
			return false;
		}
		const FCrowdyServerCallResult& Result = (*Call.Results)[0];
		TestEqual(FString::Printf(TEXT("'%s' has its outcome"), Case.Message), Result.Outcome, Case.Outcome);
		TestEqual(FString::Printf(TEXT("'%s' is retryable only for a cooldown"), Case.Message), Result.bRetryable, Case.bRetryable);
		TestEqualSensitive(FString::Printf(TEXT("'%s' has its reason"), Case.Message), Result.Reason, FString(Case.Reason));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectMembersBuiltInInputsTravelTest,
	"CrowdySDK.CrowdyExec.ServerObjectMembers.BuiltInInputsTravel", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectMembersBuiltInInputsTravelTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this, ECrowdyServerObjectVisibility::Public, ECrowdyServerMembersSource::ThisObject);
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
	FInstancedStruct Inputs = Object->MakeInputs(TEXT("AddMember"));
	FCrowdyServerMemberInputs* Member = Inputs.GetMutablePtr<FCrowdyServerMemberInputs>();
	if (!TestNotNull(TEXT("Make Inputs for Add Member gives its Player input"), Member))
	{
		return false;
	}
	Member->Player = BigUserId;
	FCallSpy Call;
	Object->Call(TEXT("AddMember"), Inputs, Call.OnDone());
	FSentFrame Frame;
	if (!TestTrue(TEXT("Add Member is sent as add_member"), Rig.WaitForFrame(KindCall, TEXT("add_member"), Frame)))
	{
		return false;
	}
	TArray<uint8> Expected;
	FCrowdyExecWriter Writer(Expected);
	Writer.MapHeader(1);
	WriteKey(Writer, "Player");
	Writer.Int(BigUserId);
	TestTrue(TEXT("Add Member carries Player as a whole int64"), Frame.Payload == Expected);
	TestEqual(TEXT("nothing was refused on the way"), Call.Num(), 0);

	const FInstancedStruct Open = Object->MakeInputs(TEXT("SetOpenForJoining"));
	const FCrowdyServerOpenInputs* OpenInputs = Open.GetPtr<FCrowdyServerOpenInputs>();
	TestTrue(TEXT("Make Inputs for Set Open For Joining gives bOpen, true by default"), OpenInputs && OpenInputs->bOpen);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectMembersMalformedRefusesReadTest,
	"CrowdySDK.CrowdyExec.ServerObjectMembers.MalformedMembersRefuseRead", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectMembersMalformedRefusesReadTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	auto ReadFailsWith = [this](const FMembersKeys& Keys, const FString& Expected)
	{
		FRig Rig(*this, ECrowdyServerObjectVisibility::Public, ECrowdyServerMembersSource::ThisObject);
		TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
		UCrowdyServerObject* Object = Rig.IsValid() ? Rig.Acquire(Rig.InstanceId, Owner.Get()) : nullptr;
		FSentFrame Read;
		if (!Object || !Rig.Open() || !TestTrue(TEXT("the object reads"), Rig.WaitForFrame(KindCall, TEXT("read"), Read)))
		{
			return;
		}
		Rig.AckSubscribes();
		Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::Ok, Rig.ReadMessage(5, Boss(3000, false, ECrowdyExecTestPhase::Calm), Keys));
		TestTrue(FString::Printf(TEXT("a read refused for '%s' fails the object"), *Expected),
			Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Failed; }));
		TestTrue(FString::Printf(TEXT("the failure says '%s' (got '%s')"), *Expected, *Object->GetFailureReason()),
			Object->GetFailureReason().Contains(Expected, ESearchCase::CaseSensitive));
	};

	FMembersKeys StringEntry;
	StringEntry.RawMembers = TArray<uint8>({0x92, 0x05, 0xa1, 'x'});
	ReadFailsWith(StringEntry, TEXT("members: expected a user id, got a string"));

	FMembersKeys Negative;
	Negative.RawMembers = TArray<uint8>({0x91, 0xff});
	ReadFailsWith(Negative, TEXT("members: expected a user id, got an integer"));

	FMembersKeys OverInt64;
	OverInt64.RawMembers = TArray<uint8>({0x91, 0xcf, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff});
	ReadFailsWith(OverInt64, TEXT("members: a user id over the int64 range"));

	FMembersKeys TooMany;
	TArray<uint8> Many = {0xdc, 0x10, 0x01};
	Many.AddUninitialized(4097);
	FMemory::Memset(Many.GetData() + 3, 0x01, 4097);
	TooMany.RawMembers = Many;
	ReadFailsWith(TooMany, TEXT("members: a container of 4097 elements, over the limit of 4096"));

	FMembersKeys Twice;
	Twice.Members = TArray<int64>{5};
	Twice.RawMembers = TArray<uint8>({0x91, 0x05});
	ReadFailsWith(Twice, TEXT("the envelope key members appears twice"));

	FRig Rig(*this, ECrowdyServerObjectVisibility::Public, ECrowdyServerMembersSource::ThisObject);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	FMembersKeys Full;
	TArray<int64> Ids;
	for (int32 Index = 1; Index <= CrowdyExec::MaxElements; ++Index)
	{
		Ids.Add(Index);
	}
	Full.Members = Ids;
	UCrowdyServerObject* Object = Rig.AcquireReady(Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5, Full);
	TestTrue(TEXT("a list of exactly 4,096 members is read"), Object && Object->GetMembers().Num() == 4096);
	return true;
}

/** Reaches the component's private steps, which a component outside a world cannot reach through BeginPlay or a sign-in. */
struct FCrowdyServerObjectComponentTestAccess
{
	static void UseServerObjects(UCrowdyServerObjectComponent& Component, UCrowdyServerObjectSubsystem* Subsystem) { Component.ServerObjectsForTest = Subsystem; }
	static void Join(UCrowdyServerObjectComponent& Component) { Component.Join(); }
	static void WatchSource(UCrowdyServerObjectComponent& Component) { Component.WatchSource(); }
	static void StopWatchingSource(UCrowdyServerObjectComponent& Component) { Component.StopWatchingSource(); }
	static void SignIn(UCrowdyServerObjectComponent& Component) { Component.HandleSignedIn(); }
	static UCrowdyServerObject* GetSource(const UCrowdyServerObjectComponent& Component) { return Component.SourceObject; }
};

namespace CrowdyServerObjectTests
{
	TStrongObjectPtr<UCrowdyServerObjectComponent> MakeRigComponent(FRig& Rig, ECrowdyServerObjectInstanceMode Mode)
	{
		TStrongObjectPtr<UCrowdyServerObjectComponent> Component(NewObject<UCrowdyServerObjectComponent>(GetTransientPackage(), NAME_None, RF_Transient));
		Component->Definition = Rig.Definition.Get();
		Component->InstanceMode = Mode;
		FCrowdyServerObjectComponentTestAccess::UseServerObjects(*Component, Rig.Subsystem.Get());
		return Component;
	}

	/** A From Server Value component following the Health of the rig's instance. */
	TStrongObjectPtr<UCrowdyServerObjectComponent> MakeFollower(FRig& Rig)
	{
		TStrongObjectPtr<UCrowdyServerObjectComponent> Component = MakeRigComponent(Rig, ECrowdyServerObjectInstanceMode::FromServerValue);
		Component->SourceDefinition = Rig.Definition.Get();
		Component->SourceInstance = ECrowdyServerObjectSourceInstance::InstanceId;
		Component->SourceInstanceId = Rig.InstanceId;
		Component->SourceVariable = TEXT("Health");
		return Component;
	}

	/** Instance 3000 reads Health 3000, so the follower's source is also the object it holds. */
	UCrowdyServerObject* FollowItself(FRig& Rig, UCrowdyServerObjectComponent& Component)
	{
		FCrowdyServerObjectComponentTestAccess::WatchSource(Component);
		UCrowdyServerObject* Source = FCrowdyServerObjectComponentTestAccess::GetSource(Component);
		if (!Rig.Test.TestNotNull(TEXT("the component watches its source"), Source) || !Rig.Open()
			|| !Rig.AnswerFirstRead(Source, Boss(3000, false, ECrowdyExecTestPhase::Calm), 5))
		{
			return nullptr;
		}
		return Rig.Test.TestTrue(TEXT("and holds the object the value names, the source itself"), Component.GetServerObject() == Source) ? Source : nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectComponentLeaveKeepsSourceTest,
	"CrowdySDK.CrowdyExec.ServerObjectComponent.LeavingKeepsTheWatchedSource", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectComponentLeaveKeepsSourceTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	Rig.InstanceId = TEXT("3000");
	TStrongObjectPtr<UCrowdyServerObjectComponent> Component = MakeFollower(Rig);
	UCrowdyServerObject* Source = FollowItself(Rig, *Component);
	if (!Source)
	{
		return false;
	}
	const TArray<FString> HealthOnly = {TEXT("Health")};
	Rig.Push(Rig.PushMessage(Epoch, 6, Boss(3001, false, ECrowdyExecTestPhase::Calm), HealthOnly));
	if (!TestTrue(TEXT("a new value moves the component to the object it names"),
		Rig.PumpUntil([&Component, Source]() { return Component->GetServerObject() && Component->GetServerObject() != Source; })))
	{
		return false;
	}
	Rig.Tick(UCrowdyServerObjectSubsystem::GracePeriodSeconds + 1.f);
	TestTrue(TEXT("leaving the old object does not give back the source it still watches"), Source->GetStatus() != ECrowdyServerObjectStatus::Released);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectComponentUnwatchKeepsHeldTest,
	"CrowdySDK.CrowdyExec.ServerObjectComponent.UnwatchingKeepsTheHeldObject", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectComponentUnwatchKeepsHeldTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	Rig.InstanceId = TEXT("3000");
	TStrongObjectPtr<UCrowdyServerObjectComponent> Component = MakeFollower(Rig);
	UCrowdyServerObject* Source = FollowItself(Rig, *Component);
	if (!Source)
	{
		return false;
	}
	FCrowdyServerObjectComponentTestAccess::StopWatchingSource(*Component);
	Rig.Tick(UCrowdyServerObjectSubsystem::GracePeriodSeconds + 1.f);
	TestTrue(TEXT("no longer watching the source does not give back the object still held"), Source->GetStatus() != ECrowdyServerObjectStatus::Released);
	TestTrue(TEXT("which the component still holds"), Component->GetServerObject() == Source);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectComponentFailedSourceTest,
	"CrowdySDK.CrowdyExec.ServerObjectComponent.FailedSourceIsPassedOn", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectComponentFailedSourceTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	AddExpectedMessagePlain(TEXT("could not join"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	Rig.InstanceId = TEXT("src-1");
	TStrongObjectPtr<UCrowdyServerObjectComponent> Component = MakeFollower(Rig);
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Listener = MakeListener();
	Component->OnStatusChanged.AddDynamic(Listener.Get(), &UCrowdyServerObjectTestListener::HandleStatus);
	FCrowdyServerObjectComponentTestAccess::WatchSource(*Component);
	UCrowdyServerObject* Source = FCrowdyServerObjectComponentTestAccess::GetSource(*Component);
	FSentFrame Read;
	if (!TestNotNull(TEXT("the component watches its source"), Source) || !Rig.Open()
		|| !TestTrue(TEXT("the source reads"), Rig.WaitForFrame(KindCall, TEXT("read"), Read)))
	{
		return false;
	}
	Rig.AckSubscribes();
	Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::AppError, Utf8Bytes(TEXT("denied: not a member")));
	if (!TestTrue(TEXT("the refused read fails the source"), Rig.PumpUntil([Source]() { return Source->GetStatus() == ECrowdyServerObjectStatus::Failed; })))
	{
		return false;
	}
	TestTrue(FString::Printf(TEXT("the component announces one failure, with no object (got %s)"), *StatusNames(Listener->StatusEvents)),
		Listener->StatusEvents == TArray<ECrowdyServerObjectStatus>({ECrowdyServerObjectStatus::Failed}) && Listener->StatusHadObject == TArray<bool>({false}));
	TestEqualSensitive(TEXT("and gives the source's reason"), Component->GetFailureReason(), FString(TEXT("This player may not read this Server Object")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectComponentTeamSignInTest,
	"CrowdySDK.CrowdyExec.ServerObjectComponent.PlayersTeamSignInWaitsForTeams", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectComponentTeamSignInTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectComponent> Component = MakeRigComponent(Rig, ECrowdyServerObjectInstanceMode::PlayersTeam);
	Component->TeamId = 7;
	FCrowdyServerObjectComponentTestAccess::Join(*Component);
	if (!TestNotNull(TEXT("with Team Id 7 the component holds team 7's object"), Component->GetServerObject()))
	{
		return false;
	}
	// The teams cache can still hold the previous account's teams, so it is not used until this account's arrive.
	Component->TeamId = 0;
	FCrowdyServerObjectComponentTestAccess::SignIn(*Component);
	TestNull(TEXT("a sign-in using the player's only team lets go of the object held"), Component->GetServerObject());
	TestEqualSensitive(TEXT("and waits for this account's teams"), Component->GetFailureReason(), FString(TEXT("Waiting for the player's teams")));
	return true;
}

/** Reaches the Link's sign-in and teams steps, which a Link outside a real game instance cannot hear. */
struct FCrowdyServerObjectLinkTestAccess
{
	static void SignIn(UCrowdyServerObjectLink& Link) { Link.HandleSignedIn(); }
	static void TeamsArrived(UCrowdyServerObjectLink& Link, const TArray<FCrowdyTeamMembership>& Teams) { Link.HandleMyTeamsChanged(Teams); }
	static int32 TeamsRequests(const UCrowdyServerObjectLink& Link) { return Link.TeamsRequests; }
};

namespace CrowdyServerObjectTests
{
	FCrowdyExecTestEveryKind EveryKindValues()
	{
		FCrowdyExecTestEveryKind Values;
		Values.Int8Value = -100;
		Values.Int16Value = -30000;
		Values.Int32Value = -2000000000;
		Values.Int64Value = -9000000000000ll;
		Values.UInt8Value = 200;
		Values.UInt16Value = 60000;
		Values.UInt32Value = 4000000000u;
		Values.UInt64Value = 9000000000000000000ull;
		Values.FloatValue = 1.5f;
		Values.DoubleValue = 2.25;
		Values.bBoolValue = true;
		Values.StringValue = TEXT("gold");
		Values.NameValue = TEXT("Keep");
		Values.EnumValue = ECrowdyExecTestPhase::Final;
		Values.StructValue.Damage = 7;
		Values.StructValue.Source = TEXT("Bow");
		Values.ArrayValue = {1, 2, 3};
		Values.SetValue = {TEXT("a")};
		Values.MapValue = {{TEXT("x"), 1}};
		Values.Int16Array = {-30000, 5};
		Values.UInt64Array = {9000000000000000000ull};
		Values.UInt16Set = {60000};
		Values.NarrowMap = {{4000000000u, -100}};
		Values.FloatArray = {1.5f, -0.25f};
		Values.PhaseSpeeds = {{ECrowdyExecTestPhase::Final, 2.5f}};
		Values.OptionalValue = 5;
		Values.VectorValue = FVector(1.0, 2.0, 3.0);
		Values.Vector2DValue = FVector2D(4.0, 5.0);
		Values.RotatorValue = FRotator(10.0, 20.0, 30.0);
		Values.QuatValue = FQuat(0.5, 0.5, 0.5, 0.5);
		Values.IntPointValue = FIntPoint(6, 7);
		Values.IntVectorValue = FIntVector(8, 9, 10);
		Values.LinearColorValue = FLinearColor(0.25f, 0.5f, 0.75f, 1.f);
		Values.ColorValue = FColor(1, 2, 3, 4);
		Values.DateTimeValue = FDateTime(2026, 9, 30);
		Values.TimespanValue = FTimespan::FromSeconds(90.0);
		Values.GuidValue = FGuid(1, 2, 3, 4);
		Values.SoftObjectValue = TSoftObjectPtr<UObject>(FSoftObjectPath(TEXT("/Game/Things/Crate.Crate")));
		Values.SoftClassValue = TSoftClassPtr<UObject>(FSoftObjectPath(TEXT("/Game/Things/BP_Crate.BP_Crate_C")));
		return Values;
	}

	/** EveryKindValues as the pins of typed Blueprint nodes hold them. */
	FCrowdyServerObjectTestEveryKindPins EveryKindPins()
	{
		const FCrowdyExecTestEveryKind Values = EveryKindValues();
		FCrowdyServerObjectTestEveryKindPins Pins;
		Pins.Int8Value = Values.Int8Value;
		Pins.Int16Value = Values.Int16Value;
		Pins.Int32Value = Values.Int32Value;
		Pins.Int64Value = Values.Int64Value;
		Pins.UInt8Value = Values.UInt8Value;
		Pins.UInt16Value = Values.UInt16Value;
		Pins.UInt32Value = Values.UInt32Value;
		Pins.UInt64Value = static_cast<int64>(Values.UInt64Value);
		Pins.FloatValue = Values.FloatValue;
		Pins.DoubleValue = Values.DoubleValue;
		Pins.bBoolValue = Values.bBoolValue;
		Pins.StringValue = Values.StringValue;
		Pins.NameValue = Values.NameValue;
		Pins.EnumValue = Values.EnumValue;
		Pins.StructValue = Values.StructValue;
		Pins.ArrayValue = Values.ArrayValue;
		Pins.SetValue = Values.SetValue;
		Pins.MapValue = Values.MapValue;
		Pins.Int16Array = {-30000, 5};
		Pins.UInt64Array = {9000000000000000000ll};
		Pins.UInt16Set = {60000};
		Pins.NarrowMap = {{4000000000ll, -100}};
		Pins.FloatArray = {1.5, -0.25};
		Pins.PhaseSpeeds = {{ECrowdyExecTestPhase::Final, 2.5}};
		Pins.OptionalValue = Values.OptionalValue.GetValue();
		Pins.VectorValue = Values.VectorValue;
		Pins.Vector2DValue = Values.Vector2DValue;
		Pins.RotatorValue = Values.RotatorValue;
		Pins.QuatValue = Values.QuatValue;
		Pins.IntPointValue = Values.IntPointValue;
		Pins.IntVectorValue = Values.IntVectorValue;
		Pins.LinearColorValue = Values.LinearColorValue;
		Pins.ColorValue = Values.ColorValue;
		Pins.DateTimeValue = Values.DateTimeValue;
		Pins.TimespanValue = Values.TimespanValue;
		Pins.GuidValue = Values.GuidValue;
		Pins.TagValue = Values.TagValue;
		Pins.SoftObjectValue = Values.SoftObjectValue;
		Pins.SoftClassValue = Values.SoftClassValue;
		return Pins;
	}

	/** Makes the rig's State FCrowdyExecTestEveryKind, every variable watched, with Echo taking and giving it. */
	bool UseEveryKind(FRig& Rig)
	{
		Rig.Definition->State = FCrowdyExecTestEveryKind::StaticStruct();
		Rig.Definition->WatchedFields.Reset();
		for (TFieldIterator<FProperty> It(FCrowdyExecTestEveryKind::StaticStruct()); It; ++It)
		{
			Rig.Definition->WatchedFields.Add(It->GetFName());
		}
		FCrowdyServerFunction& Echo = Rig.Definition->Functions.AddDefaulted_GetRef();
		Echo.Name = TEXT("Echo");
		Echo.Params = FCrowdyExecTestEveryKind::StaticStruct();
		Echo.Reply = FCrowdyExecTestEveryKind::StaticStruct();
		TArray<FString> Errors;
		const bool bBaked = Rig.Definition->Bake(Errors);
		return Rig.Test.TestTrue(FString::Printf(TEXT("the every-kind definition bakes (%s)"), *FString::Join(Errors, TEXT("; "))), bBaked);
	}

	/** A read reply carrying every field of Values. */
	template <typename T>
	TArray<uint8> FullRead(FRig& Rig, const T& Values, uint64 Seq)
	{
		TArray<uint8> Bytes;
		FCrowdyExecWriter Writer(Bytes);
		Writer.MapHeader(4);
		WriteKey(Writer, "contract");
		Writer.UInt(CrowdyExec::ContractVersion);
		WriteKey(Writer, "epoch");
		Writer.UInt(Epoch);
		WriteKey(Writer, "seq");
		Writer.UInt(Seq);
		WriteKey(Writer, "fields");
		Bytes.Append(Rig.Encode(Values));
		return Bytes;
	}

	/** Acknowledges the next subscribe and answers the next read with Message. */
	bool AnswerRead(FRig& Rig, UCrowdyServerObject* Object, const TArray<uint8>& Message)
	{
		FSentFrame Subscribe;
		FSentFrame Read;
		if (!Rig.WaitForFrame(KindSubscribe, TEXT("state"), Subscribe) || !Rig.WaitForFrame(KindCall, TEXT("read"), Read))
		{
			return Rig.Test.TestTrue(TEXT("the object subscribes and reads"), false);
		}
		Rig.Reply(Subscribe.Rid, ECrowdyNativeExecStatus::Ok, {});
		Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::Ok, Message);
		return Rig.Test.TestTrue(TEXT("the read makes the object Ready"), Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; }));
	}

	UCrowdyServerObject* AcquireEveryKind(FRig& Rig, const UObject* Owner)
	{
		UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner);
		if (!Object || !Rig.Open() || !AnswerRead(Rig, Object, FullRead(Rig, EveryKindValues(), 5)))
		{
			return nullptr;
		}
		return Object;
	}

	const FProperty* IntegersField(FName Name)
	{
		return FCrowdyServerObjectTestIntegers::StaticStruct()->FindPropertyByName(Name);
	}

	const FProperty* EveryKindPin(FName Name)
	{
		return FCrowdyServerObjectTestEveryKindPins::StaticStruct()->FindPropertyByName(Name);
	}

	/** Reads Values' Name into Pins' Pin, as Get Server Value does. */
	bool ReadPin(const FInstancedStruct& Values, FName Name, FCrowdyServerObjectTestEveryKindPins& Pins, FName Pin)
	{
		const FProperty* Property = EveryKindPin(Pin);
		return Property && UCrowdyServerObjectLibrary::GetServerValueInto(Values, Name, Property, Property->ContainerPtrToValuePtr<void>(&Pins));
	}

	/** Sets Values' Name from Pins' Pin, as Set Server Value does. */
	bool WritePin(FInstancedStruct& Values, FName Name, const FCrowdyServerObjectTestEveryKindPins& Pins, FName Pin)
	{
		const FProperty* Property = EveryKindPin(Pin);
		return Property && UCrowdyServerObjectLibrary::SetServerValueFrom(Values, Name, Property, Property->ContainerPtrToValuePtr<void>(&Pins));
	}

	void Bind(UObject* Target, FRig& Rig, FName Variable, UCrowdyServerObjectTestListener* Listener, FName Function)
	{
		UCrowdyServerObjectLibrary::BindServerVariableChanged(Target, Rig.Definition.Get(), Variable, Listener, Function);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectTypedWideningTest,
	"CrowdySDK.CrowdyExec.ServerObjectTyped.IntegersWidenWhenTheyFit", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectTypedWideningTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FInstancedStruct Values = FInstancedStruct::Make(FCrowdyServerObjectTestIntegers());
	FCrowdyServerObjectTestIntegers& Held = Values.GetMutable<FCrowdyServerObjectTestIntegers>();
	Held.Int8Value = -100;
	Held.Int16Value = -30000;
	Held.UInt16Value = 60000;
	Held.UInt32Value = 4000000000u;
	Held.UInt64Value = 9000000000000000000ull;

	FCrowdyServerObjectTestEveryKindPins Pins;
	TestTrue(TEXT("an int8 is read into an Integer pin"), ReadPin(Values, TEXT("Int8Value"), Pins, TEXT("Int8Value")) && Pins.Int8Value == -100);
	TestTrue(TEXT("an int16 is read into an Integer pin"), ReadPin(Values, TEXT("Int16Value"), Pins, TEXT("Int16Value")) && Pins.Int16Value == -30000);
	TestTrue(TEXT("a uint16 is read into an Integer pin"), ReadPin(Values, TEXT("UInt16Value"), Pins, TEXT("UInt16Value")) && Pins.UInt16Value == 60000);
	TestTrue(TEXT("a uint32 is read into an Integer64 pin"), ReadPin(Values, TEXT("UInt32Value"), Pins, TEXT("UInt32Value")) && Pins.UInt32Value == 4000000000ll);
	TestTrue(TEXT("a uint64 within int64 is read into an Integer64 pin"), ReadPin(Values, TEXT("UInt64Value"), Pins, TEXT("UInt64Value")) && Pins.UInt64Value == 9000000000000000000ll);

	Held.UInt64Value = MAX_uint64;
	Pins.UInt64Value = 3;
	TestFalse(TEXT("a uint64 above int64 is refused for an Integer64 pin"), ReadPin(Values, TEXT("UInt64Value"), Pins, TEXT("UInt64Value")));
	TestEqual(TEXT("which leaves the pin alone"), Pins.UInt64Value, int64(3));
	Held.Int64Value = MIN_int64;
	Pins.Int8Value = 3;
	TestFalse(TEXT("an int64 below int32 is refused for an Integer pin"), ReadPin(Values, TEXT("Int64Value"), Pins, TEXT("Int8Value")));
	TestEqual(TEXT("which leaves the pin alone"), Pins.Int8Value, 3);

	struct FWrite
	{
		const TCHAR* Field;
		const TCHAR* Pin;
		int64 Value;
		bool bFits;
	};
	const FWrite Writes[] = {
		{TEXT("Int8Value"), TEXT("Int8Value"), 127, true}, {TEXT("Int8Value"), TEXT("Int8Value"), 128, false}, {TEXT("Int8Value"), TEXT("Int8Value"), -129, false},
		{TEXT("Int16Value"), TEXT("Int16Value"), -32768, true}, {TEXT("Int16Value"), TEXT("Int16Value"), -32769, false},
		{TEXT("UInt16Value"), TEXT("UInt16Value"), 65535, true}, {TEXT("UInt16Value"), TEXT("UInt16Value"), 65536, false}, {TEXT("UInt16Value"), TEXT("UInt16Value"), -1, false},
		{TEXT("UInt32Value"), TEXT("UInt32Value"), 4294967295ll, true}, {TEXT("UInt32Value"), TEXT("UInt32Value"), 4294967296ll, false},
		{TEXT("UInt64Value"), TEXT("UInt64Value"), MAX_int64, true}, {TEXT("UInt64Value"), TEXT("UInt64Value"), -1, false},
		{TEXT("Int32Value"), TEXT("Int64Value"), -2147483648ll, true}, {TEXT("Int32Value"), TEXT("Int64Value"), 2147483648ll, false}};
	for (const FWrite& Write : Writes)
	{
		const FProperty* Field = IntegersField(Write.Field);
		const FProperty* Pin = EveryKindPin(Write.Pin);
		const FNumericProperty* PinNumber = CastField<FNumericProperty>(Pin);
		if (!Field || !PinNumber)
		{
			AddError(FString::Printf(TEXT("%s or its pin is missing"), Write.Field));
			continue;
		}
		PinNumber->SetIntPropertyValue(Pin->ContainerPtrToValuePtr<void>(&Pins), Write.Value);
		FString Before;
		Field->ExportTextItem_InContainer(Before, &Held, nullptr, nullptr, PPF_None);
		const bool bSet = WritePin(Values, Write.Field, Pins, Write.Pin);
		FString After;
		Field->ExportTextItem_InContainer(After, &Held, nullptr, nullptr, PPF_None);
		TestEqual(FString::Printf(TEXT("%lld into %s %s"), Write.Value, Write.Field, Write.bFits ? TEXT("is set") : TEXT("is refused")), bSet, Write.bFits);
		TestEqualSensitive(FString::Printf(TEXT("%lld into %s leaves %s"), Write.Value, Write.Field, Write.bFits ? TEXT("the value set") : TEXT("the value as it was")),
			After, Write.bFits ? FString::Printf(TEXT("%lld"), Write.Value) : Before);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectTypedContainerWideningTest,
	"CrowdySDK.CrowdyExec.ServerObjectTyped.IntegerContainersWidenWhenEveryElementFits", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectTypedContainerWideningTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FInstancedStruct Values = FInstancedStruct::Make(FCrowdyServerObjectTestIntegers());
	FCrowdyServerObjectTestIntegers& Held = Values.GetMutable<FCrowdyServerObjectTestIntegers>();
	Held.Int16Array = {1, -2};
	Held.UInt16Set = {60000};
	Held.NarrowMap = {{4000000000u, -100}};
	Held.UInt64Array = {1, MAX_uint64};

	FCrowdyServerObjectTestEveryKindPins Pins;
	TestTrue(TEXT("an int16 array is read into an Integer array pin"), ReadPin(Values, TEXT("Int16Array"), Pins, TEXT("Int16Array")) && Pins.Int16Array == TArray<int32>({1, -2}));
	TestTrue(TEXT("a uint16 set is read into an Integer set pin"), ReadPin(Values, TEXT("UInt16Set"), Pins, TEXT("UInt16Set")) && Pins.UInt16Set.Num() == 1 && Pins.UInt16Set.Contains(60000));
	TestTrue(TEXT("a uint32 to int8 map is read into an Integer64 to Integer map pin"), ReadPin(Values, TEXT("NarrowMap"), Pins, TEXT("NarrowMap"))
		&& Pins.NarrowMap.Num() == 1 && Pins.NarrowMap.FindRef(4000000000ll) == -100);
	Pins.UInt64Array = {5};
	TestFalse(TEXT("a uint64 array holding a value above int64 is refused for an Integer64 array pin"), ReadPin(Values, TEXT("UInt64Array"), Pins, TEXT("UInt64Array")));
	TestTrue(TEXT("which leaves the pin alone"), Pins.UInt64Array == TArray<int64>({5}));

	Pins.Int16Array = {7, 40000};
	TestFalse(TEXT("an Integer array with one element too large for int16 is refused"), WritePin(Values, TEXT("Int16Array"), Pins, TEXT("Int16Array")));
	TestTrue(TEXT("which leaves the array as it was"), Held.Int16Array == TArray<int16>({1, -2}));
	Pins.Int16Array = {7, -8, 9};
	TestTrue(TEXT("an Integer array that fits sets it"), WritePin(Values, TEXT("Int16Array"), Pins, TEXT("Int16Array")) && Held.Int16Array == TArray<int16>({7, -8, 9}));

	Pins.UInt16Set = {-1};
	TestFalse(TEXT("an Integer set with a negative element is refused for a uint16 set"), WritePin(Values, TEXT("UInt16Set"), Pins, TEXT("UInt16Set")));
	TestTrue(TEXT("which leaves the set as it was"), Held.UInt16Set.Num() == 1 && Held.UInt16Set.Contains(60000));
	Pins.UInt16Set = {3, 65535};
	TestTrue(TEXT("an Integer set that fits sets it"), WritePin(Values, TEXT("UInt16Set"), Pins, TEXT("UInt16Set"))
		&& Held.UInt16Set.Num() == 2 && Held.UInt16Set.Contains(3) && Held.UInt16Set.Contains(65535));

	Pins.NarrowMap = {{5, 200}};
	TestFalse(TEXT("a map with a value too large for int8 is refused"), WritePin(Values, TEXT("NarrowMap"), Pins, TEXT("NarrowMap")));
	Pins.NarrowMap = {{-1, 1}};
	TestFalse(TEXT("a map with a negative key is refused for uint32 keys"), WritePin(Values, TEXT("NarrowMap"), Pins, TEXT("NarrowMap")));
	TestTrue(TEXT("which leaves the map as it was"), Held.NarrowMap.Num() == 1 && Held.NarrowMap.FindRef(4000000000u) == -100);
	Pins.NarrowMap = {{5, 100}, {4294967295ll, -128}};
	TestTrue(TEXT("a map that fits sets it"), WritePin(Values, TEXT("NarrowMap"), Pins, TEXT("NarrowMap"))
		&& Held.NarrowMap.Num() == 2 && Held.NarrowMap.FindRef(5u) == 100 && Held.NarrowMap.FindRef(4294967295u) == -128);

	TestFalse(TEXT("an int16 array is refused for a string set pin"), ReadPin(Values, TEXT("Int16Array"), Pins, TEXT("SetValue")));
	TestFalse(TEXT("and for a string array value"), ReadPin(FInstancedStruct::Make(FCrowdyExecTestExtras()), TEXT("Notes"), Pins, TEXT("Int16Array")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectTypedRealAndEnumContainersTest,
	"CrowdySDK.CrowdyExec.ServerObjectTyped.RealAndEnumContainersConvert", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectTypedRealAndEnumContainersTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FInstancedStruct Values = FInstancedStruct::Make(FCrowdyServerObjectTestIntegers());
	FCrowdyServerObjectTestIntegers& Held = Values.GetMutable<FCrowdyServerObjectTestIntegers>();
	Held.FloatArray = {1.5f, -0.25f};
	Held.PhaseArray = {ECrowdyExecTestPhase::Enraged, ECrowdyExecTestPhase::Final};
	Held.PhaseSpeeds = {{ECrowdyExecTestPhase::Final, 2.5f}};

	FCrowdyServerObjectTestEveryKindPins Pins;
	TestTrue(TEXT("a float array is read into a Float (double) array pin"), ReadPin(Values, TEXT("FloatArray"), Pins, TEXT("FloatArray")) && Pins.FloatArray == TArray<double>({1.5, -0.25}));
	TestTrue(TEXT("a map of float values is read into a map of doubles"), ReadPin(Values, TEXT("PhaseSpeeds"), Pins, TEXT("PhaseSpeeds"))
		&& Pins.PhaseSpeeds.Num() == 1 && Pins.PhaseSpeeds.FindRef(ECrowdyExecTestPhase::Final) == 2.5);
	Pins.FloatArray = {3.0, 4.5};
	TestTrue(TEXT("a Float array pin sets a float array"), WritePin(Values, TEXT("FloatArray"), Pins, TEXT("FloatArray")) && Held.FloatArray == TArray<float>({3.f, 4.5f}));
	Pins.PhaseSpeeds = {{ECrowdyExecTestPhase::Calm, 0.5}};
	TestTrue(TEXT("a map of doubles sets a map of floats"), WritePin(Values, TEXT("PhaseSpeeds"), Pins, TEXT("PhaseSpeeds"))
		&& Held.PhaseSpeeds.Num() == 1 && Held.PhaseSpeeds.FindRef(ECrowdyExecTestPhase::Calm) == 0.5f);

	// A Blueprint array of an enum holds bytes naming the enum; heap-built so its field classes are set.
	TUniquePtr<FArrayProperty> PhasePin(new FArrayProperty(FFieldVariant(), TEXT("Phases")));
	FByteProperty* PhaseByte = new FByteProperty(PhasePin.Get(), TEXT("Phase"));
	PhaseByte->Enum = StaticEnum<ECrowdyExecTestPhase>();
	PhasePin->AddCppProperty(PhaseByte);
	TArray<uint8> Bytes;
	TestTrue(TEXT("an enum class array is read into a byte array of its enum"), UCrowdyServerObjectLibrary::GetServerValueInto(Values, TEXT("PhaseArray"), PhasePin.Get(), &Bytes)
		&& Bytes == TArray<uint8>({static_cast<uint8>(ECrowdyExecTestPhase::Enraged), static_cast<uint8>(ECrowdyExecTestPhase::Final)}));
	Bytes = {static_cast<uint8>(ECrowdyExecTestPhase::Calm)};
	TestTrue(TEXT("and a byte array of its enum sets one"), UCrowdyServerObjectLibrary::SetServerValueFrom(Values, TEXT("PhaseArray"), PhasePin.Get(), &Bytes)
		&& Held.PhaseArray == TArray<ECrowdyExecTestPhase>({ECrowdyExecTestPhase::Calm}));

	PhaseByte->Enum = StaticEnum<ECrowdyServerObjectStatus>();
	Bytes = {static_cast<uint8>(ECrowdyServerObjectStatus::Ready)};
	TestFalse(TEXT("a byte array of another enum is refused"), UCrowdyServerObjectLibrary::SetServerValueFrom(Values, TEXT("PhaseArray"), PhasePin.Get(), &Bytes));
	TestTrue(TEXT("which leaves the array as it was"), Held.PhaseArray == TArray<ECrowdyExecTestPhase>({ECrowdyExecTestPhase::Calm}));
	TestFalse(TEXT("and a Float array pin is refused for an enum array"), ReadPin(Values, TEXT("PhaseArray"), Pins, TEXT("FloatArray")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectTypedOptionalsTest,
	"CrowdySDK.CrowdyExec.ServerObjectTyped.OptionalsSetAndEmpty", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectTypedOptionalsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FInstancedStruct Values = FInstancedStruct::Make(FCrowdyServerObjectTestIntegers());
	const FCrowdyServerObjectTestIntegers& Held = Values.Get<FCrowdyServerObjectTestIntegers>();
	FCrowdyServerObjectTestEveryKindPins Pins;
	Pins.OptionalValue = 9;
	TestFalse(TEXT("an empty optional is not read"), ReadPin(Values, TEXT("OptionalValue"), Pins, TEXT("OptionalValue")));
	TestEqual(TEXT("and leaves the pin alone"), Pins.OptionalValue, 9);

	Pins.OptionalValue = 5;
	TestTrue(TEXT("a plain pin sets an optional"), WritePin(Values, TEXT("OptionalValue"), Pins, TEXT("OptionalValue")));
	TestTrue(TEXT("to its value"), Held.OptionalValue.IsSet() && Held.OptionalValue.GetValue() == 5);
	Pins.OptionalValue = 0;
	TestTrue(TEXT("a set optional is read"), ReadPin(Values, TEXT("OptionalValue"), Pins, TEXT("OptionalValue")));
	TestEqual(TEXT("into the pin"), Pins.OptionalValue, 5);
	TestTrue(TEXT("a set optional int32 widens into an Integer64 pin"), ReadPin(Values, TEXT("OptionalValue"), Pins, TEXT("Int64Value")) && Pins.Int64Value == 5);

	Pins.Int64Value = 5000000000ll;
	TestFalse(TEXT("an Integer64 too large for an optional int32 is refused"), WritePin(Values, TEXT("OptionalValue"), Pins, TEXT("Int64Value")));
	TestTrue(TEXT("which keeps its value"), Held.OptionalValue.IsSet() && Held.OptionalValue.GetValue() == 5);
	TestFalse(TEXT("and an empty optional given a value that does not fit"), WritePin(Values, TEXT("OptionalBig"), Pins, TEXT("StringValue")));
	TestFalse(TEXT("stays empty"), Held.OptionalBig.IsSet());

	const FProperty* Pin = EveryKindPin(TEXT("OptionalValue"));
	TestTrue(TEXT("Set Server Value Or Unset without a value empties an optional"),
		UCrowdyServerObjectLibrary::SetServerValueOrUnsetFrom(Values, TEXT("OptionalValue"), false, Pin, Pin->ContainerPtrToValuePtr<void>(&Pins)));
	TestFalse(TEXT("which is then empty"), Held.OptionalValue.IsSet());
	TestTrue(TEXT("and with a value sets it"),
		UCrowdyServerObjectLibrary::SetServerValueOrUnsetFrom(Values, TEXT("OptionalValue"), true, Pin, Pin->ContainerPtrToValuePtr<void>(&Pins)));
	TestTrue(TEXT("to the pin's value"), Held.OptionalValue.IsSet() && Held.OptionalValue.GetValue() == 5);
	TestFalse(TEXT("unsetting a value that is not optional is refused"),
		UCrowdyServerObjectLibrary::SetServerValueOrUnsetFrom(Values, TEXT("Int32Value"), false, Pin, Pin->ContainerPtrToValuePtr<void>(&Pins)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectTypedEveryKindTest,
	"CrowdySDK.CrowdyExec.ServerObjectTyped.EveryKindReadsAndCalls", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectTypedEveryKindTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid() || !UseEveryKind(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Early = Rig.Acquire(Rig.InstanceId, Owner.Get());
	FString Before = TEXT("unset");
	const FProperty* StringPin = EveryKindPin(TEXT("StringValue"));
	TestFalse(TEXT("before Ready a variable is not read from the server"),
		UCrowdyServerObjectLibrary::ReadServerVariableInto(Early, Rig.Definition.Get(), TEXT("StringValue"), StringPin, &Before));
	TestTrue(TEXT("and gives its default"), Before.IsEmpty());

	UCrowdyServerObject* Object = Early && Rig.Open() && AnswerRead(Rig, Early, FullRead(Rig, EveryKindValues(), 5)) ? Early : nullptr;
	if (!Object)
	{
		return false;
	}
	const FCrowdyServerObjectTestEveryKindPins Expected = EveryKindPins();
	FCrowdyServerObjectTestEveryKindPins Read;
	for (TFieldIterator<FProperty> Pin(FCrowdyServerObjectTestEveryKindPins::StaticStruct()); Pin; ++Pin)
	{
		const bool bRead = UCrowdyServerObjectLibrary::ReadServerVariableInto(Object, Rig.Definition.Get(), Pin->GetFName(), *Pin, Pin->ContainerPtrToValuePtr<void>(&Read));
		TestTrue(FString::Printf(TEXT("%s is read into its pin"), *Pin->GetName()), bRead);
		TestTrue(FString::Printf(TEXT("%s holds the value read"), *Pin->GetName()), Pin->Identical_InContainer(&Read, &Expected));
	}

	const FProperty* IntPin = EveryKindPin(TEXT("Int32Value"));
	int32 Empty = 9;
	TestFalse(TEXT("an empty optional is not read"), UCrowdyServerObjectLibrary::ReadServerVariableInto(Object, Rig.Definition.Get(), TEXT("EmptyOptional"), IntPin, &Empty));
	TestEqual(TEXT("and leaves the pin at its default"), Empty, 0);
	Empty = 9;
	TestFalse(TEXT("a variable the State lacks is not read"), UCrowdyServerObjectLibrary::ReadServerVariableInto(Object, Rig.Definition.Get(), TEXT("Nope"), IntPin, &Empty));
	TestFalse(TEXT("nor one of another definition's object"), UCrowdyServerObjectLibrary::ReadServerVariableInto(Object, NewObject<UCrowdyServerObjectDefinition>(), TEXT("Int32Value"), IntPin, &Empty));

	FInstancedStruct Inputs = UCrowdyServerObjectLibrary::MakeFunctionInputs(Rig.Definition.Get(), TEXT("Echo"));
	for (TFieldIterator<FProperty> Pin(FCrowdyServerObjectTestEveryKindPins::StaticStruct()); Pin; ++Pin)
	{
		TestTrue(FString::Printf(TEXT("%s is set from its pin"), *Pin->GetName()),
			UCrowdyServerObjectLibrary::SetServerValueFrom(Inputs, Pin->GetFName(), *Pin, Pin->ContainerPtrToValuePtr<void>(&Expected)));
	}
	const FCrowdyExecTestEveryKind Values = EveryKindValues();
	TestTrue(TEXT("the inputs set from the pins hold every value"), Inputs.GetScriptStruct() == FCrowdyExecTestEveryKind::StaticStruct()
		&& FCrowdyExecTestEveryKind::StaticStruct()->CompareScriptStruct(Inputs.GetMemory(), &Values, PPF_None));
	FCallSpy Call;
	Object->Call(TEXT("Echo"), Inputs, Call.OnDone());
	FSentFrame Sent;
	if (!TestTrue(TEXT("the call is sent"), Rig.WaitForFrame(KindCall, TEXT("echo"), Sent)))
	{
		return false;
	}
	TestTrue(TEXT("carrying every value"), Sent.Payload == Rig.Encode(Values));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectTypedBindKindsTest,
	"CrowdySDK.CrowdyExec.ServerObjectTyped.BindPassesWidenedOptionalAndStructValues", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectTypedBindKindsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid() || !UseEveryKind(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquireEveryKind(Rig, Owner.Get());
	if (!Object)
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Narrow = MakeListener();
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Set = MakeListener();
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Unset = MakeListener();
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Hit = MakeListener();
	Bind(Object, Rig, TEXT("Int8Value"), Narrow.Get(), TEXT("HandleInt"));
	Bind(Object, Rig, TEXT("OptionalValue"), Set.Get(), TEXT("HandleOptionalInt"));
	Bind(Object, Rig, TEXT("EmptyOptional"), Unset.Get(), TEXT("HandleOptionalInt"));
	Bind(Object, Rig, TEXT("StructValue"), Hit.Get(), TEXT("HandleHit"));
	TestTrue(TEXT("an int8 reaches an int32 handler"), Narrow->IntValues == TArray<int32>({-100}));
	TestTrue(TEXT("a set optional reaches its handler with Has Value"), Set->IntValues == TArray<int32>({5}) && Set->HasValues == TArray<bool>({true}));
	TestTrue(TEXT("an empty optional reaches its handler without Has Value"), Unset->IntValues == TArray<int32>({0}) && Unset->HasValues == TArray<bool>({false}));
	TestTrue(TEXT("a struct reaches its handler"), Hit->HitValues.Num() == 1 && Hit->HitValues[0].Damage == 7 && Hit->HitValues[0].Source == FName(TEXT("Bow")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectTypedBindTest,
	"CrowdySDK.CrowdyExec.ServerObjectTyped.BindFiresNowAndOnItsVariableOnly", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectTypedBindTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	if (!Object)
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Early = MakeListener();
	Bind(Object, Rig, TEXT("Health"), Early.Get(), TEXT("HandleInt"));
	TestEqual(TEXT("a handler bound while Connecting is not run at once"), Early->IntValues.Num(), 0);
	if (!Rig.Open() || !Rig.AnswerFirstRead(Object, Boss(3000, false, ECrowdyExecTestPhase::Calm), 5))
	{
		return false;
	}
	TestTrue(TEXT("it runs once on the first read, with the value"), Early->IntValues == TArray<int32>({3000}));

	TStrongObjectPtr<UCrowdyServerObjectTestListener> Late = MakeListener();
	Bind(Object, Rig, TEXT("health"), Late.Get(), TEXT("HandleInt"));
	TestTrue(TEXT("a handler bound after Ready runs once, at once"), Late->IntValues == TArray<int32>({3000}));

	const TArray<FString> PhaseOnly = {TEXT("Phase")};
	const TArray<FString> HealthOnly = {TEXT("Health")};
	Rig.Push(Rig.PushMessage(Epoch, 6, Boss(3000, false, ECrowdyExecTestPhase::Final), PhaseOnly));
	TestTrue(TEXT("the other variable's change arrives"), Rig.PumpUntil([Object]() { return StateOf(Object) && StateOf(Object)->Phase == ECrowdyExecTestPhase::Final; }));
	Rig.PumpFor(0.1);
	TestEqual(TEXT("a change of another variable does not run it"), Early->IntValues.Num() + Late->IntValues.Num(), 2);

	Rig.Push(Rig.PushMessage(Epoch, 7, Boss(2500, false, ECrowdyExecTestPhase::Final), HealthOnly));
	TestTrue(TEXT("a change of its variable runs it"), Rig.PumpUntil([&Late]() { return Late->IntValues.Num() >= 2; }));
	Rig.PumpFor(0.1);
	TestTrue(TEXT("once, with the new value"), Late->IntValues == TArray<int32>({3000, 2500}) && Early->IntValues == TArray<int32>({3000, 2500}));

	Bind(Object, Rig, TEXT("Health"), Late.Get(), TEXT("HandleInt"));
	TestEqual(TEXT("binding the same handler again runs it at once"), Late->IntValues.Num(), 3);
	TestEqual(TEXT("and replaces its binding"), Object->GetVariableBindings().Num(), 2);
	UCrowdyServerObjectLibrary::UnbindServerVariableChanged(Object, Rig.Definition.Get(), TEXT("Health"), Early.Get(), TEXT("HandleInt"));
	Rig.Push(Rig.PushMessage(Epoch, 8, Boss(2000, false, ECrowdyExecTestPhase::Final), HealthOnly));
	TestTrue(TEXT("the next change runs the handler still bound"), Rig.PumpUntil([&Late]() { return Late->IntValues.Num() >= 4; }));
	Rig.PumpFor(0.1);
	TestEqual(TEXT("exactly once"), Late->IntValues.Num(), 4);
	TestEqual(TEXT("and not the one unbound"), Early->IntValues.Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectTypedDeadHandlerTest,
	"CrowdySDK.CrowdyExec.ServerObjectTyped.DestroyedHandlerIsDropped", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectTypedDeadHandlerTest::RunTest(const FString& Parameters)
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
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Gone = MakeListener();
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Kept = MakeListener();
	Bind(Object, Rig, TEXT("Health"), Gone.Get(), TEXT("HandleInt"));
	Bind(Object, Rig, TEXT("Health"), Kept.Get(), TEXT("HandleInt"));
	TestEqual(TEXT("both handlers are bound"), Object->GetVariableBindings().Num(), 2);

	Gone->MarkAsGarbage();
	Gone.Reset();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

	const TArray<FString> HealthOnly = {TEXT("Health")};
	Rig.Push(Rig.PushMessage(Epoch, 6, Boss(2500, false, ECrowdyExecTestPhase::Calm), HealthOnly));
	TestTrue(TEXT("a change after a handler was destroyed still runs the other"), Rig.PumpUntil([&Kept]() { return Kept->IntValues.Num() >= 2; }));
	TestEqual(TEXT("and the destroyed handler's binding is dropped"), Object->GetVariableBindings().Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectTypedComponentRejoinTest,
	"CrowdySDK.CrowdyExec.ServerObjectTyped.BindFollowsComponentRejoin", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectTypedComponentRejoinTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectComponent> Component = MakeRigComponent(Rig, ECrowdyServerObjectInstanceMode::InstanceId);
	Component->InstanceId = Rig.InstanceId;
	FCrowdyServerObjectComponentTestAccess::Join(*Component);
	UCrowdyServerObject* First = Component->GetServerObject();
	if (!TestNotNull(TEXT("the component joins"), First) || !Rig.Open() || !Rig.AnswerFirstRead(First, Boss(3000, false, ECrowdyExecTestPhase::Calm), 5))
	{
		return false;
	}
	TestTrue(TEXT("the component is resolved to the object it holds"), UCrowdyServerObjectLibrary::ResolveServerObject(Component.Get(), Rig.Definition.Get()) == First);
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Listener = MakeListener();
	Bind(Component.Get(), Rig, TEXT("Health"), Listener.Get(), TEXT("HandleInt"));
	TestTrue(TEXT("a handler bound to the component runs at once with its object's value"), Listener->IntValues == TArray<int32>({3000}));

	Component->InstanceId = TEXT("boss-2");
	FCrowdyServerObjectComponentTestAccess::Join(*Component);
	UCrowdyServerObject* Second = Component->GetServerObject();
	if (!TestTrue(TEXT("the component joins another object"), Second && Second != First) || !Rig.AnswerFirstRead(Second, Boss(1234, false, ECrowdyExecTestPhase::Calm), 5))
	{
		return false;
	}
	TestTrue(TEXT("the handler runs with the new object's value"), Listener->IntValues == TArray<int32>({3000, 1234}));

	const TArray<FString> HealthOnly = {TEXT("Health")};
	Rig.Push(Rig.PushMessage(Epoch, 6, Boss(1, false, ECrowdyExecTestPhase::Calm), HealthOnly));
	TestTrue(TEXT("the object left behind changes"), Rig.PumpUntil([First]() { return HealthOf(First) == 1; }));
	Rig.PumpFor(0.1);
	TestEqual(TEXT("a change of the object left behind does not run it"), Listener->IntValues.Num(), 2);
	Rig.InstanceId = TEXT("boss-2");
	Rig.Push(Rig.PushMessage(Epoch, 6, Boss(999, false, ECrowdyExecTestPhase::Calm), HealthOnly));
	TestTrue(TEXT("a change of the object joined runs it"), Rig.PumpUntil([&Listener]() { return Listener->IntValues.Num() >= 3; }));
	TestEqual(TEXT("with that value"), Listener->IntValues.Last(), 999);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectTypedLinkInstanceIdTest,
	"CrowdySDK.CrowdyExec.ServerObjectTyped.LinkFindsByInstanceId", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectTypedLinkInstanceIdTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Widget = MakeOwner();
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Other = MakeOwner();
	UCrowdyServerObjectLink* Link = Rig.Subsystem->FindLink(Widget.Get(), Rig.Definition.Get(), ECrowdyServerObjectFind::InstanceId, Rig.InstanceId, 0);
	if (!TestNotNull(TEXT("a Link is made"), Link) || !TestNotNull(TEXT("and joins the instance at once"), Link->GetServerObject()))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Listener = MakeListener();
	Bind(Link, Rig, TEXT("Health"), Listener.Get(), TEXT("HandleInt"));
	if (!Rig.Open() || !Rig.AnswerFirstRead(Link->GetServerObject(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5))
	{
		return false;
	}
	TestTrue(TEXT("a handler bound to the Link runs when its object is first read"), Listener->IntValues == TArray<int32>({3000}));

	TestTrue(TEXT("the same object another owner gets"), Link->GetServerObject() == Rig.Acquire(Rig.InstanceId, Other.Get()));
	TestTrue(TEXT("asking again gives the same Link"), Rig.Subsystem->FindLink(Widget.Get(), Rig.Definition.Get(), ECrowdyServerObjectFind::InstanceId, Rig.InstanceId, 0) == Link);
	TestTrue(TEXT("a Team Id the instance does not use gives the same Link"), Rig.Subsystem->FindLink(Widget.Get(), Rig.Definition.Get(), ECrowdyServerObjectFind::InstanceId, Rig.InstanceId, 7) == Link);
	TestTrue(TEXT("another Instance Id gives another Link"), Rig.Subsystem->FindLink(Widget.Get(), Rig.Definition.Get(), ECrowdyServerObjectFind::InstanceId, TEXT("boss-9"), 0) != Link);
	TestTrue(TEXT("another owner gives another Link"), Rig.Subsystem->FindLink(Other.Get(), Rig.Definition.Get(), ECrowdyServerObjectFind::InstanceId, Rig.InstanceId, 0) != Link);
	TestEqual(TEXT("three Links are kept"), Rig.Subsystem->NumLinksForTest(), 3);
	TestTrue(TEXT("a Link is resolved to the object it follows"), UCrowdyServerObjectLibrary::ResolveServerObject(Link, Rig.Definition.Get()) == Link->GetServerObject());
	TestNull(TEXT("but not for another definition"), UCrowdyServerObjectLibrary::ResolveServerObject(Link, NewObject<UCrowdyServerObjectDefinition>()));

	Destroy(Widget);
	Rig.Tick(0.1f);
	TestEqual(TEXT("the Links of a destroyed owner are dropped"), Rig.Subsystem->NumLinksForTest(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectTypedLinkSignedInTest,
	"CrowdySDK.CrowdyExec.ServerObjectTyped.LinkWaitsForTheSignedInPlayer", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectTypedLinkSignedInTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Widget = MakeOwner();
	UCrowdyServerObjectLink* Link = Rig.Subsystem->FindLink(Widget.Get(), Rig.Definition.Get(), ECrowdyServerObjectFind::SignedInPlayer, TEXT("ignored"), 0);
	if (!TestNotNull(TEXT("a Link is made"), Link))
	{
		return false;
	}
	TestNull(TEXT("with nobody signed in it holds nothing"), Link->GetServerObject());
	TestEqualSensitive(TEXT("and waits"), Link->GetFailureReason(), FString(TEXT("Waiting for the player to sign in")));
	TestTrue(TEXT("an Instance Id the instance does not use gives the same Link"),
		Rig.Subsystem->FindLink(Widget.Get(), Rig.Definition.Get(), ECrowdyServerObjectFind::SignedInPlayer, FString(), 0) == Link);

	Rig.Session->SetUserID(77);
	FCrowdyServerObjectLinkTestAccess::SignIn(*Link);
	UCrowdyServerObject* Object = Link->GetServerObject();
	TestTrue(TEXT("a sign-in joins the player's own instance"), Object && Object->GetInstanceId().Equals(TEXT("77"), ESearchCase::CaseSensitive));

	Rig.Session->SetUserID(78);
	FCrowdyServerObjectLinkTestAccess::SignIn(*Link);
	Object = Link->GetServerObject();
	TestTrue(TEXT("another account's sign-in joins its instance"), Object && Object->GetInstanceId().Equals(TEXT("78"), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectTypedLinkTeamTest,
	"CrowdySDK.CrowdyExec.ServerObjectTyped.LinkFindsThePlayersTeam", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectTypedLinkTeamTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Widget = MakeOwner();
	UCrowdyServerObjectLink* Chosen = Rig.Subsystem->FindLink(Widget.Get(), Rig.Definition.Get(), ECrowdyServerObjectFind::PlayersTeam, FString(), 7);
	UCrowdyServerObject* Object = Chosen ? Chosen->GetServerObject() : nullptr;
	TestTrue(TEXT("a Team Id is the Instance Id"), Object && Object->GetInstanceId().Equals(TEXT("7"), ESearchCase::CaseSensitive));

	UCrowdyServerObjectLink* Only = Rig.Subsystem->FindLink(Widget.Get(), Rig.Definition.Get(), ECrowdyServerObjectFind::PlayersTeam, FString(), 0);
	if (!TestTrue(TEXT("the player's only team is another Link"), Only && Only != Chosen))
	{
		return false;
	}
	TestNull(TEXT("which holds nothing before the teams arrive"), Only->GetServerObject());
	TestEqualSensitive(TEXT("and waits for them"), Only->GetFailureReason(), FString(TEXT("Waiting for the player's teams")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectTypedOnlyOneInstanceTest,
	"CrowdySDK.CrowdyExec.ServerObjectTyped.OnlyOneInstanceIgnoresTheInstanceChoice", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectTypedOnlyOneInstanceTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	Rig.Definition->bOnlyOneInstance = true;
	TArray<FString> Errors;
	const bool bBaked = Rig.Definition->Bake(Errors);
	if (!TestTrue(FString::Printf(TEXT("an Every Player type with Only One Instance bakes (%s)"), *FString::Join(Errors, TEXT("; "))), bBaked))
	{
		return false;
	}
	const ECrowdyServerObjectInstanceMode Modes[] = {ECrowdyServerObjectInstanceMode::InstanceId, ECrowdyServerObjectInstanceMode::SignedInPlayer,
		ECrowdyServerObjectInstanceMode::ThisActor, ECrowdyServerObjectInstanceMode::PlayersTeam, ECrowdyServerObjectInstanceMode::FromServerValue};
	for (const ECrowdyServerObjectInstanceMode Mode : Modes)
	{
		TStrongObjectPtr<UCrowdyServerObjectComponent> Component = MakeRigComponent(Rig, Mode);
		FString Error;
		TestEqualSensitive(FString::Printf(TEXT("the component in %s mode uses the only instance"), *UEnum::GetValueAsString(Mode)), Component->ResolveInstanceId(Error),
			FString(UCrowdyServerObjectDefinition::OnlyInstanceId));
		TestTrue(TEXT("with no error"), Error.IsEmpty());
	}

	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Widget = MakeOwner();
	UCrowdyServerObjectLink* ById = Rig.Subsystem->FindLink(Widget.Get(), Rig.Definition.Get(), ECrowdyServerObjectFind::InstanceId, TEXT("chest-7"), 0);
	UCrowdyServerObjectLink* ByPlayer = Rig.Subsystem->FindLink(Widget.Get(), Rig.Definition.Get(), ECrowdyServerObjectFind::SignedInPlayer, FString(), 0);
	UCrowdyServerObjectLink* ByTeam = Rig.Subsystem->FindLink(Widget.Get(), Rig.Definition.Get(), ECrowdyServerObjectFind::PlayersTeam, FString(), 3);
	TestTrue(TEXT("every way of finding it gives one Link"), ById && ById == ByPlayer && ById == ByTeam);
	UCrowdyServerObject* Object = ById ? ById->GetServerObject() : nullptr;
	TestTrue(TEXT("which holds the only instance, with nobody signed in"),
		Object && Object->GetInstanceId().Equals(UCrowdyServerObjectDefinition::OnlyInstanceId, ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectTypedServerNamesTest,
	"CrowdySDK.CrowdyExec.ServerObjectTyped.ChangedNamesAreTheUnrealNames", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectTypedServerNamesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	FCrowdyServerFieldName& Rename = Rig.Definition->FieldNames.AddDefaulted_GetRef();
	Rename.Struct = FCrowdyExecTestBossState::StaticStruct();
	Rename.Field = TEXT("Health");
	Rename.ServerName = TEXT("hp");
	TArray<FString> Errors;
	const bool bBaked = Rig.Definition->Bake(Errors);
	if (!TestTrue(FString::Printf(TEXT("the renamed definition bakes (%s)"), *FString::Join(Errors, TEXT("; "))), bBaked))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Watcher = MakeListener();
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Handler = MakeListener();
	if (!Object)
	{
		return false;
	}
	Object->K2_WatchVariables(VariablesEventOf(Watcher.Get()));
	Bind(Object, Rig, TEXT("Health"), Handler.Get(), TEXT("HandleInt"));
	const FCrowdyExecTestBossState Values = Boss(3000, false, ECrowdyExecTestPhase::Calm);
	const TArray<FString> ServerNames = {TEXT("hp"), TEXT("bDefeated"), TEXT("Phase")};
	if (!Rig.Open() || !AnswerRead(Rig, Object, Rig.StateMessage(true, Epoch, 5, &Values, ServerNames)))
	{
		return false;
	}
	TestTrue(FString::Printf(TEXT("Watch Variables names the renamed variable as Get Server Value finds it (got %s)"), *Joined(LastVariables(*Watcher))),
		NamesAre(LastVariables(*Watcher), AllWatched()));
	FCrowdyServerObjectTestPins Pins;
	TestTrue(TEXT("and Get Server Value reads it by that name"), GetIntoPin(Object->GetState(), TEXT("Health"), Pins, TEXT("Count")) && Pins.Count == 3000);
	TestTrue(TEXT("and a handler bound to it runs"), Handler->IntValues == TArray<int32>({3000}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectTypedDeadWatcherTest,
	"CrowdySDK.CrowdyExec.ServerObjectTyped.WatchVariablesDropsDestroyedWatchers", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectTypedDeadWatcherTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	if (!Object)
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Gone = MakeListener();
	Object->K2_WatchVariables(VariablesEventOf(Gone.Get()));
	Gone->MarkAsGarbage();
	Gone.Reset();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

	TStrongObjectPtr<UCrowdyServerObjectTestListener> Kept = MakeListener();
	Object->K2_WatchVariables(VariablesEventOf(Kept.Get()));
	TestEqual(TEXT("adding a watcher drops one whose object was destroyed, with no change in between"), Object->NumVariableWatchersForTest(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectTypedResolveTest,
	"CrowdySDK.CrowdyExec.ServerObjectTyped.ResolveAndMakeInputs", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectTypedResolveTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	UCrowdyServerObjectDefinition* Other = NewObject<UCrowdyServerObjectDefinition>();
	TestTrue(TEXT("a Server Object resolves to itself"), UCrowdyServerObjectLibrary::ResolveServerObject(Object, Rig.Definition.Get()) == Object);
	TestTrue(TEXT("and the pure node gives the same"), UCrowdyServerObjectLibrary::GetHeldServerObject(Object, Rig.Definition.Get()) == Object);
	TestNull(TEXT("but not for another definition"), UCrowdyServerObjectLibrary::ResolveServerObject(Object, Other));
	TestNull(TEXT("nothing resolves from None"), UCrowdyServerObjectLibrary::ResolveServerObject(nullptr, Rig.Definition.Get()));
	TestNull(TEXT("nor from an object of another kind"), UCrowdyServerObjectLibrary::ResolveServerObject(Owner.Get(), Rig.Definition.Get()));
	TStrongObjectPtr<UCrowdyServerObjectComponent> Unjoined = MakeRigComponent(Rig, ECrowdyServerObjectInstanceMode::InstanceId);
	TestNull(TEXT("nor from a component before it joins"), UCrowdyServerObjectLibrary::ResolveServerObject(Unjoined.Get(), Rig.Definition.Get()));

	const FInstancedStruct Inputs = UCrowdyServerObjectLibrary::MakeFunctionInputs(Rig.Definition.Get(), AttackFunction);
	TestTrue(TEXT("Make Function Inputs gives a function's inputs"), Inputs.GetScriptStruct() == FCrowdyServerObjectTestAttackParams::StaticStruct());
	TestFalse(TEXT("nothing for an unknown function"), UCrowdyServerObjectLibrary::MakeFunctionInputs(Rig.Definition.Get(), TEXT("Nope")).IsValid());
	TestFalse(TEXT("nor without a definition"), UCrowdyServerObjectLibrary::MakeFunctionInputs(nullptr, AttackFunction).IsValid());
	TestTrue(TEXT("Find Variable finds a State variable by its Unreal name"), Rig.Definition->FindVariable(TEXT("bdefeated")) == FCrowdyExecTestBossState::StaticStruct()->FindPropertyByName(TEXT("bDefeated")));
	TestNull(TEXT("and nothing for a name the State lacks"), Rig.Definition->FindVariable(TEXT("Nope")));
	Rig.Open();
	return true;
}

namespace CrowdyServerObjectTests
{
	const TArray<FString> HealthChanged = {TEXT("Health")};

	/** Instance boss-2, Ready with Health, held for Owner; the rig's connection must be open. */
	UCrowdyServerObject* AcquireSecondReady(FRig& Rig, const UObject* Owner, int32 Health)
	{
		UCrowdyServerObject* Second = Rig.Acquire(TEXT("boss-2"), Owner);
		return Second && Rig.AnswerFirstRead(Second, Boss(Health, false, ECrowdyExecTestPhase::Calm), 5) ? Second : nullptr;
	}

	bool UseOnlyOneInstance(FRig& Rig)
	{
		Rig.Definition->bOnlyOneInstance = true;
		TArray<FString> Errors;
		const bool bBaked = Rig.Definition->Bake(Errors);
		return Rig.Test.TestTrue(FString::Printf(TEXT("the Only One Instance definition bakes (%s)"), *FString::Join(Errors, TEXT("; "))), bBaked);
	}

	/** A component of the rig's definition holding boss-1, Ready with Health 3000. */
	TStrongObjectPtr<UCrowdyServerObjectComponent> JoinedComponent(FRig& Rig)
	{
		TStrongObjectPtr<UCrowdyServerObjectComponent> Component = MakeRigComponent(Rig, ECrowdyServerObjectInstanceMode::InstanceId);
		Component->InstanceId = Rig.InstanceId;
		FCrowdyServerObjectComponentTestAccess::Join(*Component);
		UCrowdyServerObject* First = Component->GetServerObject();
		const bool bReady = First && Rig.Open() && Rig.AnswerFirstRead(First, Boss(3000, false, ECrowdyExecTestPhase::Calm), 5);
		return Rig.Test.TestTrue(TEXT("the component joins boss-1"), bReady) ? Component : nullptr;
	}

	TArray<FCrowdyTeamMembership> OneTeam(int64 TeamId)
	{
		FCrowdyTeamMembership Membership;
		Membership.Team.TeamId = TeamId;
		return {Membership};
	}

	/** Runs a Custom Thunk as a compiled Blueprint calls it: each parameter read, in order, from its own slot in Locals. */
	bool RunThunk(FName FunctionName, uint8* Locals)
	{
		UFunction* Function = UCrowdyServerObjectLibrary::StaticClass()->FindFunctionByName(FunctionName);
		TArray<uint8> Script;
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm) && !It->HasAnyPropertyFlags(CPF_ReturnParm); ++It)
		{
			Script.Add(EX_LocalVariable);
			const ScriptPointerType Pointer = reinterpret_cast<ScriptPointerType>(*It);
			Script.Append(reinterpret_cast<const uint8*>(&Pointer), sizeof(Pointer));
		}
		Script.Add(EX_EndFunctionParms);
		UObject* Context = UCrowdyServerObjectLibrary::StaticClass()->GetDefaultObject();
		FFrame Stack(Context, Function, Locals, nullptr, Function->ChildProperties);
		Stack.Code = Script.GetData();
		bool bResult = false;
		Function->Invoke(Context, Stack, &bResult);
		return bResult;
	}

	/** Locals laid out as FunctionName's parameters, initialized, and destroyed with it. */
	struct FThunkLocals
	{
		UFunction* Function = nullptr;
		TArray<uint8, TAlignedHeapAllocator<16>> Memory;

		explicit FThunkLocals(FName FunctionName)
			: Function(UCrowdyServerObjectLibrary::StaticClass()->FindFunctionByName(FunctionName))
		{
			Memory.SetNumZeroed(Function->ParmsSize);
			for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
			{
				It->InitializeValue_InContainer(Memory.GetData());
			}
		}

		~FThunkLocals()
		{
			for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
			{
				It->DestroyValue_InContainer(Memory.GetData());
			}
		}

		template <typename T>
		T& Param(const TCHAR* Name)
		{
			return *Function->FindPropertyByName(Name)->ContainerPtrToValuePtr<T>(Memory.GetData());
		}
	};

	/** HandleInt with a local variable after its parameter, as a Blueprint function with locals has. */
	UFunction* HandlerWithLocal()
	{
		UClass* Class = UCrowdyServerObjectTestListener::StaticClass();
		const FName Name(TEXT("HandleIntWithLocal"));
		if (UFunction* Made = Class->FindFunctionByName(Name))
		{
			return Made;
		}
		const UFunction* HandleInt = Class->FindFunctionByName(TEXT("HandleInt"));
		UFunction* Function = NewObject<UFunction>(Class, Name, RF_Transient);
		Function->FunctionFlags = HandleInt->FunctionFlags | FUNC_HasDefaults;
		FIntProperty* Value = new FIntProperty(Function, TEXT("Value"));
		Value->PropertyFlags |= CPF_Parm;
		FStructProperty* Scratch = new FStructProperty(Function, TEXT("Scratch"));
		Scratch->Struct = FCrowdyServerObjectTestCounted::StaticStruct();
		Value->Next = Scratch;
		Function->ChildProperties = Value;
		Function->StaticLink(true);
		Function->SetNativeFunc(HandleInt->GetNativeFunc());
		Function->AddToRoot();
		Class->AddFunctionToFunctionMap(Function, Name);
		return Function;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectFixUnusedLinkAgesOutTest,
	"CrowdySDK.CrowdyExec.ServerObjectFixes.UnusedLinkAgesOut", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectFixUnusedLinkAgesOutTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Widget = MakeOwner();
	auto Find = [&Rig, &Widget](const TCHAR* Id) { return Rig.Subsystem->FindLink(Widget.Get(), Rig.Definition.Get(), ECrowdyServerObjectFind::InstanceId, Id, 0); };
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Handler = MakeListener();
	UCrowdyServerObjectLink* Recycled = Find(TEXT("a"));
	Bind(Recycled, Rig, TEXT("Health"), Handler.Get(), TEXT("HandleInt"));
	UCrowdyServerObjectLink* Current = Find(TEXT("b"));
	Bind(Current, Rig, TEXT("Health"), Handler.Get(), TEXT("HandleInt"));
	Find(TEXT("c"));
	TestEqual(TEXT("three Links are made"), Rig.Subsystem->NumLinksForTest(), 3);
	TestEqual(TEXT("binding the handler to another Link moves it off the first"), Recycled->GetVariableBindings().Num(), 0);

	for (int32 Second = 0; Second < 12; ++Second)
	{
		Find(TEXT("c"));
		Rig.Tick(1.f);
	}
	TestEqual(TEXT("past the grace period only the Link asked for and the Link with a live handler are kept"), Rig.Subsystem->NumLinksForTest(), 2);
	TestTrue(TEXT("the kept Link with a handler is the one it was moved to"), Find(TEXT("b")) == Current);

	Handler->MarkAsGarbage();
	Handler.Reset();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	for (int32 Second = 0; Second < 12; ++Second)
	{
		Find(TEXT("c"));
		Rig.Tick(1.f);
	}
	TestEqual(TEXT("once its handler is destroyed that Link is dropped too"), Rig.Subsystem->NumLinksForTest(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectFixRebindMovesTest,
	"CrowdySDK.CrowdyExec.ServerObjectFixes.RebindingToAnotherTargetMovesTheHandler", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectFixRebindMovesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* First = Rig.AcquireReady(Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5);
	UCrowdyServerObject* Second = First ? AcquireSecondReady(Rig, Owner.Get(), 1234) : nullptr;
	if (!Second)
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Listener = MakeListener();
	Bind(First, Rig, TEXT("Health"), Listener.Get(), TEXT("HandleInt"));
	Bind(Second, Rig, TEXT("Health"), Listener.Get(), TEXT("HandleInt"));
	TestTrue(TEXT("each bind runs at once with its object's value"), Listener->IntValues == TArray<int32>({3000, 1234}));
	TestEqual(TEXT("the first object no longer holds the binding"), First->GetVariableBindings().Num(), 0);

	Rig.Push(Rig.PushMessage(Epoch, 6, Boss(2500, false, ECrowdyExecTestPhase::Calm), HealthChanged));
	TestTrue(TEXT("the first object changes"), Rig.PumpUntil([First]() { return HealthOf(First) == 2500; }));
	Rig.PumpFor(0.1);
	TestTrue(TEXT("and the handler moved off it does not run"), Listener->IntValues == TArray<int32>({3000, 1234}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectFixLinkOwnsItsHoldTest,
	"CrowdySDK.CrowdyExec.ServerObjectFixes.LinkHoldsItsObjectAsItsOwnOwner", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectFixLinkOwnsItsHoldTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	Rig.Session->SetUserID(77);
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Widget = MakeOwner();
	UCrowdyServerObjectLink* ById = Rig.Subsystem->FindLink(Widget.Get(), Rig.Definition.Get(), ECrowdyServerObjectFind::InstanceId, TEXT("77"), 0);
	UCrowdyServerObjectLink* ByPlayer = Rig.Subsystem->FindLink(Widget.Get(), Rig.Definition.Get(), ECrowdyServerObjectFind::SignedInPlayer, FString(), 0);
	UCrowdyServerObject* Object = ById ? ById->GetServerObject() : nullptr;
	if (!TestTrue(TEXT("two Links of one owner follow the same object"), Object && ByPlayer && ByPlayer->GetServerObject() == Object))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Keeper = MakeListener();
	Bind(ById, Rig, TEXT("Health"), Keeper.Get(), TEXT("HandleInt"));

	Rig.Session->SetUserID(78);
	FCrowdyServerObjectLinkTestAccess::SignIn(*ByPlayer);
	UCrowdyServerObjectLibrary::ReleaseServerObject(Object, Widget.Get());
	Rig.Tick(UCrowdyServerObjectSubsystem::GracePeriodSeconds + 1.f);
	TestTrue(TEXT("the sibling Link moving on and the owner releasing it leave the other Link's object held"), Object->GetStatus() != ECrowdyServerObjectStatus::Released);
	TestTrue(TEXT("which that Link still follows"), ById->GetServerObject() == Object);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectFixTeamsAskedAgainTest,
	"CrowdySDK.CrowdyExec.ServerObjectFixes.WaitingTeamLinkAsksAgainThenJoins", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectFixTeamsAskedAgainTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Widget = MakeOwner();
	UCrowdyServerObjectLink* Link = Rig.Subsystem->FindLink(Widget.Get(), Rig.Definition.Get(), ECrowdyServerObjectFind::PlayersTeam, FString(), 0);
	if (!TestNotNull(TEXT("a Player's Team Link is made"), Link))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Keeper = MakeListener();
	Bind(Link, Rig, TEXT("Health"), Keeper.Get(), TEXT("HandleInt"));
	TestEqual(TEXT("waiting for the teams asks for them once"), FCrowdyServerObjectLinkTestAccess::TeamsRequests(*Link), 1);
	Rig.Tick(1.f);
	TestEqual(TEXT("and not again at once"), FCrowdyServerObjectLinkTestAccess::TeamsRequests(*Link), 1);
	Rig.Tick(5.f);
	TestEqual(TEXT("a request that brought nothing is repeated after the backoff"), FCrowdyServerObjectLinkTestAccess::TeamsRequests(*Link), 2);
	Rig.Tick(5.f);
	TestEqual(TEXT("and again while it waits"), FCrowdyServerObjectLinkTestAccess::TeamsRequests(*Link), 3);

	FCrowdyServerObjectLinkTestAccess::TeamsArrived(*Link, OneTeam(7));
	UCrowdyServerObject* Object = Link->GetServerObject();
	TestTrue(TEXT("the teams arriving join the player's only team"), Object && Object->GetInstanceId().Equals(TEXT("7"), ESearchCase::CaseSensitive));
	Rig.Tick(10.f);
	TestEqual(TEXT("and it stops asking"), FCrowdyServerObjectLinkTestAccess::TeamsRequests(*Link), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectFixHandlerLocalsTest,
	"CrowdySDK.CrowdyExec.ServerObjectFixes.HandlerLocalsAreLeftToProcessEvent", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectFixHandlerLocalsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.AcquireReady(Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5);
	const UFunction* Handler = HandlerWithLocal();
	if (!Object || !TestTrue(TEXT("the handler's local lies past its parameters"), Handler->PropertiesSize > Handler->ParmsSize))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Listener = MakeListener();
	const int32 Before = FCrowdyServerObjectTestCounted::Constructed;
	Bind(Object, Rig, TEXT("Health"), Listener.Get(), Handler->GetFName());
	TestTrue(TEXT("a handler with a local variable gets the value"), Listener->IntValues == TArray<int32>({3000}));
	TestEqual(TEXT("and its local is set up once, by ProcessEvent alone"), FCrowdyServerObjectTestCounted::Constructed - Before, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectFixRejoinMidChangeTest,
	"CrowdySDK.CrowdyExec.ServerObjectFixes.RejoinDuringAChangeStopsTheOldObjectsHandlers", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectFixRejoinMidChangeTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectComponent> Component = JoinedComponent(Rig);
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Other = MakeOwner();
	if (!Component || !AcquireSecondReady(Rig, Other.Get(), 1234))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Mover = MakeListener();
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Next = MakeListener();
	Bind(Component.Get(), Rig, TEXT("Health"), Mover.Get(), TEXT("HandleInt"));
	Bind(Component.Get(), Rig, TEXT("Health"), Next.Get(), TEXT("HandleInt"));
	bool bMoved = false;
	UCrowdyServerObjectComponent* Held = Component.Get();
	Mover->OnInt = [&bMoved, Held]()
	{
		if (bMoved)
		{
			return;
		}
		bMoved = true;
		Held->InstanceId = TEXT("boss-2");
		FCrowdyServerObjectComponentTestAccess::Join(*Held);
	};
	Rig.Push(Rig.PushMessage(Epoch, 6, Boss(2500, false, ECrowdyExecTestPhase::Calm), HealthChanged));
	TestTrue(TEXT("the first handler moves the component"), Rig.PumpUntil([&bMoved]() { return bMoved; }));
	Rig.PumpFor(0.1);
	Mover->OnInt = nullptr;
	TestTrue(TEXT("the handler that moved it saw the change"), Mover->IntValues == TArray<int32>({3000, 2500}));
	TestTrue(FString::Printf(TEXT("the next handler hears the object now held, and nothing older after it (got %d values)"), Next->IntValues.Num()),
		Next->IntValues == TArray<int32>({3000, 1234}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectFixBindDuringChangeTest,
	"CrowdySDK.CrowdyExec.ServerObjectFixes.BindingDuringAChangeRunsOnce", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectFixBindDuringChangeTest::RunTest(const FString& Parameters)
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
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Late = MakeListener();
	UCrowdyServerObjectTestListener* LateHandler = Late.Get();
	UCrowdyServerObjectDefinition* Definition = Rig.Definition.Get();
	TSharedRef<int32> Calls = MakeShared<int32>(0);
	const FDelegateHandle Watch = Object->WatchValues(FOnCrowdyServerValuesChanged::FDelegate::CreateLambda(
		[Calls, LateHandler, Definition](UCrowdyServerObject* Changed, TConstArrayView<FString>)
		{
			if (++*Calls == 2)
			{
				UCrowdyServerObjectLibrary::BindServerVariableChanged(Changed, Definition, TEXT("Health"), LateHandler, TEXT("HandleInt"));
			}
		}));
	Rig.Push(Rig.PushMessage(Epoch, 6, Boss(2500, false, ECrowdyExecTestPhase::Calm), HealthChanged));
	TestTrue(TEXT("a handler bound while a change is announced runs"), Rig.PumpUntil([&Late]() { return Late->IntValues.Num() > 0; }));
	Rig.PumpFor(0.1);
	TestTrue(FString::Printf(TEXT("once, with the new value (got %d runs)"), Late->IntValues.Num()), Late->IntValues == TArray<int32>({2500}));
	Object->UnwatchValues(Watch);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectFixSelfRebindTest,
	"CrowdySDK.CrowdyExec.ServerObjectFixes.HandlerBindingItselfDoesNotRecurse", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectFixSelfRebindTest::RunTest(const FString& Parameters)
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
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Self = MakeListener();
	UCrowdyServerObjectTestListener* Handler = Self.Get();
	UCrowdyServerObjectDefinition* Definition = Rig.Definition.Get();
	Self->OnInt = [Handler, Object, Definition]()
	{
		if (Handler->IntValues.Num() < 5)
		{
			UCrowdyServerObjectLibrary::BindServerVariableChanged(Object, Definition, TEXT("Health"), Handler, TEXT("HandleInt"));
		}
	};
	Bind(Object, Rig, TEXT("Health"), Handler, TEXT("HandleInt"));
	TestEqual(TEXT("a handler that binds itself again runs once when bound"), Self->IntValues.Num(), 1);
	Rig.Push(Rig.PushMessage(Epoch, 6, Boss(2500, false, ECrowdyExecTestPhase::Calm), HealthChanged));
	TestTrue(TEXT("and runs on a change"), Rig.PumpUntil([&Self]() { return Self->IntValues.Num() >= 2; }));
	Rig.PumpFor(0.1);
	TestTrue(TEXT("once per change"), Self->IntValues == TArray<int32>({3000, 2500}));
	TestEqual(TEXT("and stays bound once"), Object->GetVariableBindings().Num(), 1);
	Self->OnInt = nullptr;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectTwoBindingsOneVariableTest,
	"CrowdySDK.CrowdyExec.ServerObjectFixes.TwoHandlersOnOneVariableBothFireOnce", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectTwoBindingsOneVariableTest::RunTest(const FString& Parameters)
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
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Holder = MakeListener();
	Bind(Object, Rig, TEXT("Health"), Holder.Get(), TEXT("HandleInt"));
	Bind(Object, Rig, TEXT("Health"), Holder.Get(), TEXT("HandleIntSecond"));
	TestEqual(TEXT("two handlers on one variable are two bindings"), Object->GetVariableBindings().Num(), 2);
	Rig.Push(Rig.PushMessage(Epoch, 6, Boss(2500, false, ECrowdyExecTestPhase::Calm), HealthChanged));
	TestTrue(TEXT("both handlers run on the change"), Rig.PumpUntil([&Holder]() { return Holder->IntValues.Num() >= 2 && Holder->SecondIntValues.Num() >= 2; }));
	Rig.PumpFor(0.1);
	TestTrue(TEXT("the first runs once when bound and once per change"), Holder->IntValues == TArray<int32>({3000, 2500}));
	TestTrue(TEXT("and so does the second"), Holder->SecondIntValues == TArray<int32>({3000, 2500}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectFixUnbindDuringChangeTest,
	"CrowdySDK.CrowdyExec.ServerObjectFixes.HandlerUnboundDuringAChangeIsSkipped", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectFixUnbindDuringChangeTest::RunTest(const FString& Parameters)
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
	TStrongObjectPtr<UCrowdyServerObjectTestListener> First = MakeListener();
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Second = MakeListener();
	Bind(Object, Rig, TEXT("Health"), First.Get(), TEXT("HandleInt"));
	Bind(Object, Rig, TEXT("Health"), Second.Get(), TEXT("HandleInt"));
	UCrowdyServerObjectTestListener* Unbound = Second.Get();
	UCrowdyServerObjectDefinition* Definition = Rig.Definition.Get();
	First->OnInt = [Object, Definition, Unbound]()
	{
		UCrowdyServerObjectLibrary::UnbindServerVariableChanged(Object, Definition, TEXT("Health"), Unbound, TEXT("HandleInt"));
	};
	Rig.Push(Rig.PushMessage(Epoch, 6, Boss(2500, false, ECrowdyExecTestPhase::Calm), HealthChanged));
	TestTrue(TEXT("the first handler runs"), Rig.PumpUntil([&First]() { return First->IntValues.Num() >= 2; }));
	Rig.PumpFor(0.1);
	First->OnInt = nullptr;
	TestTrue(TEXT("a handler it unbound is skipped for the change under way"), Second->IntValues == TArray<int32>({3000}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectFixComponentHolderCheckTest,
	"CrowdySDK.CrowdyExec.ServerObjectFixes.ComponentMovedByItsEventSkipsTheOldChange", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectFixComponentHolderCheckTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectComponent> Component = JoinedComponent(Rig);
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Other = MakeOwner();
	if (!Component || !AcquireSecondReady(Rig, Other.Get(), 1234))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Watcher = MakeListener();
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Handler = MakeListener();
	Component->OnVariablesChanged.AddDynamic(Watcher.Get(), &UCrowdyServerObjectTestListener::HandleVariables);
	Bind(Component.Get(), Rig, TEXT("Health"), Handler.Get(), TEXT("HandleInt"));
	bool bMoved = false;
	UCrowdyServerObjectComponent* Held = Component.Get();
	Watcher->OnVariables = [&bMoved, Held]()
	{
		if (bMoved)
		{
			return;
		}
		bMoved = true;
		Held->InstanceId = TEXT("boss-2");
		FCrowdyServerObjectComponentTestAccess::Join(*Held);
	};
	Rig.Push(Rig.PushMessage(Epoch, 6, Boss(2500, false, ECrowdyExecTestPhase::Calm), HealthChanged));
	TestTrue(TEXT("On Variables Changed moves the component"), Rig.PumpUntil([&bMoved]() { return bMoved; }));
	Rig.PumpFor(0.1);
	Watcher->OnVariables = nullptr;
	TestTrue(FString::Printf(TEXT("the handler hears the object joined once, and not the old change (got %d values)"), Handler->IntValues.Num()),
		Handler->IntValues == TArray<int32>({3000, 1234}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectFixComponentServerNamesTest,
	"CrowdySDK.CrowdyExec.ServerObjectFixes.ComponentVariablesChangedGivesUnrealNames", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectFixComponentServerNamesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	FCrowdyServerFieldName& Rename = Rig.Definition->FieldNames.AddDefaulted_GetRef();
	Rename.Struct = FCrowdyExecTestBossState::StaticStruct();
	Rename.Field = TEXT("Health");
	Rename.ServerName = TEXT("hp");
	TArray<FString> Errors;
	if (!TestTrue(FString::Printf(TEXT("the renamed definition bakes (%s)"), *FString::Join(Errors, TEXT("; "))), Rig.Definition->Bake(Errors)))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectComponent> Component = MakeRigComponent(Rig, ECrowdyServerObjectInstanceMode::InstanceId);
	Component->InstanceId = Rig.InstanceId;
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Watcher = MakeListener();
	Component->OnVariablesChanged.AddDynamic(Watcher.Get(), &UCrowdyServerObjectTestListener::HandleVariables);
	FCrowdyServerObjectComponentTestAccess::Join(*Component);
	UCrowdyServerObject* Object = Component->GetServerObject();
	const FCrowdyExecTestBossState Values = Boss(3000, false, ECrowdyExecTestPhase::Calm);
	const TArray<FString> ServerNames = {TEXT("hp"), TEXT("bDefeated"), TEXT("Phase")};
	if (!Object || !Rig.Open() || !AnswerRead(Rig, Object, Rig.StateMessage(true, Epoch, 5, &Values, ServerNames)))
	{
		return false;
	}
	TestTrue(FString::Printf(TEXT("the component names a renamed variable by its Unreal name (got %s)"), *Joined(LastVariables(*Watcher))),
		NamesAre(LastVariables(*Watcher), AllWatched()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectFixFollowersRebindTest,
	"CrowdySDK.CrowdyExec.ServerObjectFixes.HoldersFireForTheObjectTheyMoveTo", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectFixFollowersRebindTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	Rig.InstanceId = TEXT("3000");
	TStrongObjectPtr<UCrowdyServerObjectComponent> Follower = MakeFollower(Rig);
	UCrowdyServerObject* Source = FollowItself(Rig, *Follower);
	if (!Source)
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestListener> FromValue = MakeListener();
	Bind(Follower.Get(), Rig, TEXT("Health"), FromValue.Get(), TEXT("HandleInt"));
	Rig.Push(Rig.PushMessage(Epoch, 6, Boss(3001, false, ECrowdyExecTestPhase::Calm), HealthChanged));
	UCrowdyServerObject* Moved = nullptr;
	TestTrue(TEXT("a new source value moves the follower"), Rig.PumpUntil([&Follower, Source, &Moved]()
	{
		Moved = Follower->GetServerObject();
		return Moved && Moved != Source;
	}));
	if (!Moved || !Rig.AnswerFirstRead(Moved, Boss(555, false, ECrowdyExecTestPhase::Calm), 5))
	{
		return false;
	}
	TestTrue(FString::Printf(TEXT("a From Server Value switch fires with the new object's value (got %s)"), *FString::JoinBy(FromValue->IntValues, TEXT(", "), [](int32 Value) { return LexToString(Value); })),
		FromValue->IntValues.Num() >= 2 && FromValue->IntValues[0] == 3000 && FromValue->IntValues.Last() == 555);

	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Other = MakeOwner();
	UCrowdyServerObject* Ready = AcquireSecondReady(Rig, Other.Get(), 1234);
	TStrongObjectPtr<UCrowdyServerObjectComponent> Rejoiner = MakeRigComponent(Rig, ECrowdyServerObjectInstanceMode::InstanceId);
	Rejoiner->InstanceId = Rig.InstanceId;
	FCrowdyServerObjectComponentTestAccess::Join(*Rejoiner);
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Immediate = MakeListener();
	Bind(Rejoiner.Get(), Rig, TEXT("Health"), Immediate.Get(), TEXT("HandleInt"));
	Rejoiner->InstanceId = TEXT("boss-2");
	FCrowdyServerObjectComponentTestAccess::Join(*Rejoiner);
	TestTrue(FString::Printf(TEXT("a rejoin to an object already Ready fires at once with its value (got %d values)"), Immediate->IntValues.Num()),
		Ready && Rejoiner->GetServerObject() == Ready && Immediate->IntValues == TArray<int32>({3001, 1234}));

	Rig.Session->SetUserID(77);
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Widget = MakeOwner();
	UCrowdyServerObjectLink* Link = Rig.Subsystem->FindLink(Widget.Get(), Rig.Definition.Get(), ECrowdyServerObjectFind::SignedInPlayer, FString(), 0);
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Player = MakeListener();
	Bind(Link, Rig, TEXT("Health"), Player.Get(), TEXT("HandleInt"));
	if (!Link || !Link->GetServerObject() || !Rig.AnswerFirstRead(Link->GetServerObject(), Boss(77, false, ECrowdyExecTestPhase::Calm), 5))
	{
		return false;
	}
	Rig.Session->SetUserID(78);
	FCrowdyServerObjectLinkTestAccess::SignIn(*Link);
	UCrowdyServerObject* Rejoined = Link->GetServerObject();
	if (!Rejoined || !Rig.AnswerFirstRead(Rejoined, Boss(78, false, ECrowdyExecTestPhase::Calm), 5))
	{
		return false;
	}
	TestTrue(TEXT("a Link fires its handlers again after a sign-in rejoin"), Player->IntValues == TArray<int32>({77, 78}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectFixReleaseDropsBindingsTest,
	"CrowdySDK.CrowdyExec.ServerObjectFixes.ReleasedObjectAndStoppedLinkDropTheirHandlers", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectFixReleaseDropsBindingsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	TStrongObjectPtr<UCrowdyServerObject> Object(Rig.AcquireReady(Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5));
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Widget = MakeOwner();
	TStrongObjectPtr<UCrowdyServerObjectLink> Link(Rig.Subsystem->FindLink(Widget.Get(), Rig.Definition.Get(), ECrowdyServerObjectFind::InstanceId, Rig.InstanceId, 0));
	if (!Object || !Link)
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestListener> OnObject = MakeListener();
	TStrongObjectPtr<UCrowdyServerObjectTestListener> OnLink = MakeListener();
	Bind(Object.Get(), Rig, TEXT("Health"), OnObject.Get(), TEXT("HandleInt"));
	Bind(Link.Get(), Rig, TEXT("Health"), OnLink.Get(), TEXT("HandleInt"));
	Destroy(Widget);
	Destroy(Owner);
	Rig.Tick(UCrowdyServerObjectSubsystem::GracePeriodSeconds + 1.f);
	TestTrue(TEXT("the object is given back"), Object->GetStatus() == ECrowdyServerObjectStatus::Released);
	TestEqual(TEXT("dropping its handlers"), Object->GetVariableBindings().Num(), 0);
	int32 Health = 0;
	TestFalse(TEXT("and a given-back object's variables no longer read as the server's"),
		UCrowdyServerObjectLibrary::ReadServerVariableInto(Object.Get(), Rig.Definition.Get(), TEXT("Health"), FCrowdyServerObjectTestPins::StaticStruct()->FindPropertyByName(TEXT("Count")), &Health));
	TestEqual(TEXT("and the Link of a destroyed owner drops its handlers"), Link->GetVariableBindings().Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectFixOnlyOneSignInTest,
	"CrowdySDK.CrowdyExec.ServerObjectFixes.OnlyOneInstanceComponentKeepsItsObjectAtSignIn", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectFixOnlyOneSignInTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid() || !UseOnlyOneInstance(Rig))
	{
		return false;
	}
	Rig.InstanceId = UCrowdyServerObjectDefinition::OnlyInstanceId;
	TStrongObjectPtr<UCrowdyServerObjectComponent> Component = MakeRigComponent(Rig, ECrowdyServerObjectInstanceMode::SignedInPlayer);
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Listener = MakeListener();
	Component->OnStatusChanged.AddDynamic(Listener.Get(), &UCrowdyServerObjectTestListener::HandleStatus);
	FCrowdyServerObjectComponentTestAccess::Join(*Component);
	UCrowdyServerObject* Object = Component->GetServerObject();
	if (!Object || !Rig.Open() || !Rig.AnswerFirstRead(Object, Boss(3000, false, ECrowdyExecTestPhase::Calm), 5))
	{
		return false;
	}
	const int32 Announced = Listener->StatusEvents.Num();
	Rig.Session->SetUserID(77);
	FCrowdyServerObjectComponentTestAccess::SignIn(*Component);
	TestTrue(TEXT("a sign-in keeps the only instance"), Component->GetServerObject() == Object);
	TestEqual(FString::Printf(TEXT("without joining it again (got %s)"), *StatusNames(Listener->StatusEvents)), Listener->StatusEvents.Num(), Announced);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectFixOnlyOneAcquireTest,
	"CrowdySDK.CrowdyExec.ServerObjectFixes.OnlyOneInstanceAcquireUsesTheOnlyInstance", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectFixOnlyOneAcquireTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	FRig Rig(*this);
	if (!Rig.IsValid() || !UseOnlyOneInstance(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* ByChest = Rig.Acquire(TEXT("chest-7"), Owner.Get());
	UCrowdyServerObject* ByOther = Rig.Acquire(TEXT("other"), Owner.Get());
	TestTrue(TEXT("any Instance Id gives the only instance"), ByChest && ByChest->GetInstanceId().Equals(UCrowdyServerObjectDefinition::OnlyInstanceId, ESearchCase::CaseSensitive));
	TestTrue(TEXT("the same object whatever Instance Id is asked for"), ByChest && ByChest == ByOther);
	TestEqual(TEXT("one object is held"), Rig.Subsystem->NumObjectsForTest(), 1);
	Rig.Open();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectFixSetOptionalUnfitTest,
	"CrowdySDK.CrowdyExec.ServerObjectFixes.SetOptionalThatDoesNotFitWarns", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectFixSetOptionalUnfitTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	AddExpectedMessagePlain(TEXT("cannot take OptionalValue's value"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	FRig Rig(*this);
	if (!Rig.IsValid() || !UseEveryKind(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquireEveryKind(Rig, Owner.Get());
	if (!Object)
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Hit = MakeListener();
	Bind(Object, Rig, TEXT("OptionalValue"), Hit.Get(), TEXT("HandleHit"));
	TestEqual(TEXT("a set optional its handler cannot take is not passed on as empty"), Hit->HitValues.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectFixUnfitReadWarnsTest,
	"CrowdySDK.CrowdyExec.ServerObjectFixes.UnfitReadWarnsOncePerVariable", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectFixUnfitReadWarnsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	AddExpectedMessagePlain(TEXT("UInt64Value's value does not fit"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	FRig Rig(*this);
	if (!Rig.IsValid() || !UseEveryKind(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquireEveryKind(Rig, Owner.Get());
	if (!Object)
	{
		return false;
	}
	const FProperty* IntPin = EveryKindPin(TEXT("Int32Value"));
	int32 Read = 9;
	TestFalse(TEXT("a value too large for its pin is not read"), UCrowdyServerObjectLibrary::ReadServerVariableInto(Object, Rig.Definition.Get(), TEXT("UInt64Value"), IntPin, &Read));
	TestEqual(TEXT("and leaves the pin at its default"), Read, 0);
	TestFalse(TEXT("nor the second time"), UCrowdyServerObjectLibrary::ReadServerVariableInto(Object, Rig.Definition.Get(), TEXT("UInt64Value"), IntPin, &Read));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectFixByteEnumContainersTest,
	"CrowdySDK.CrowdyExec.ServerObjectFixes.ByteArraysOfAnotherEnumAreRefused", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectFixByteEnumContainersTest::RunTest(const FString& Parameters)
{
	FInstancedStruct Values = FInstancedStruct::Make(FCrowdyServerObjectTestByteEnum());
	FCrowdyServerObjectTestByteEnum& Held = Values.GetMutable<FCrowdyServerObjectTestByteEnum>();
	Held.Channels = {ECC_Pawn};
	TUniquePtr<FArrayProperty> Pin(new FArrayProperty(FFieldVariant(), TEXT("Channels")));
	FByteProperty* Element = new FByteProperty(Pin.Get(), TEXT("Channel"));
	Pin->AddCppProperty(Element);
	TArray<uint8> Bytes = {7};

	Element->Enum = StaticEnum<ECrowdyExecTestPhase>();
	TestFalse(TEXT("a byte array of one enum is not read into a byte array of another"), UCrowdyServerObjectLibrary::GetServerValueInto(Values, TEXT("Channels"), Pin.Get(), &Bytes));
	TestTrue(TEXT("which is left alone"), Bytes == TArray<uint8>({7}));
	TestFalse(TEXT("nor set from one"), UCrowdyServerObjectLibrary::SetServerValueFrom(Values, TEXT("Channels"), Pin.Get(), &Bytes));
	TestTrue(TEXT("which leaves the array as it was"), Held.Channels.Num() == 1 && Held.Channels[0] == ECC_Pawn);
	Element->Enum = nullptr;
	TestFalse(TEXT("nor read into a plain byte array"), UCrowdyServerObjectLibrary::GetServerValueInto(Values, TEXT("Channels"), Pin.Get(), &Bytes));
	Element->Enum = StaticEnum<ECollisionChannel>();
	TestTrue(TEXT("a byte array of the same enum is read"), UCrowdyServerObjectLibrary::GetServerValueInto(Values, TEXT("Channels"), Pin.Get(), &Bytes)
		&& Bytes == TArray<uint8>({static_cast<uint8>(ECC_Pawn)}));

	TestTrue(TEXT("kept names match ignoring case and dropped characters"), CrowdyExec::SameKeptName(TEXT("Max HP"), TEXT("max_hp")) == false
		&& CrowdyExec::SameKeptName(TEXT("Max HP!"), TEXT("maxhp")) && CrowdyExec::SameKeptName(TEXT("!!"), TEXT("")));
	TestFalse(TEXT("and differ by any kept character"), CrowdyExec::SameKeptName(TEXT("Health"), TEXT("Healt")) || CrowdyExec::SameKeptName(TEXT("a_b"), TEXT("ab")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectFixThunksTest,
	"CrowdySDK.CrowdyExec.ServerObjectFixes.CustomThunksReadTheirParameters", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectFixThunksTest::RunTest(const FString& Parameters)
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
	{
		FThunkLocals Read(TEXT("ReadServerVariable"));
		Read.Param<TObjectPtr<UObject>>(TEXT("Target")) = Object;
		Read.Param<TObjectPtr<UObject>>(TEXT("Definition")) = Rig.Definition.Get();
		Read.Param<FName>(TEXT("Variable")) = TEXT("Health");
		TestTrue(TEXT("Read Server Variable's thunk reads the variable named"), RunThunk(TEXT("ReadServerVariable"), Read.Memory.GetData()));
		TestEqual(TEXT("into its Value"), Read.Param<int32>(TEXT("Value")), 3000);
		Read.Param<FName>(TEXT("Variable")) = TEXT("Nope");
		TestFalse(TEXT("and refuses a variable the object lacks"), RunThunk(TEXT("ReadServerVariable"), Read.Memory.GetData()));
		TestEqual(TEXT("leaving Value at its default"), Read.Param<int32>(TEXT("Value")), 0);
	}
	{
		FThunkLocals Set(TEXT("SetServerValueOrUnset"));
		Set.Param<FInstancedStruct>(TEXT("Values")) = FInstancedStruct::Make(FCrowdyServerObjectTestIntegers());
		Set.Param<FName>(TEXT("Name")) = TEXT("OptionalValue");
		Set.Param<bool>(TEXT("bHasValue")) = true;
		Set.Param<int32>(TEXT("Value")) = 42;
		TestTrue(TEXT("Set Server Value Or Unset's thunk sets an optional"), RunThunk(TEXT("SetServerValueOrUnset"), Set.Memory.GetData()));
		const FCrowdyServerObjectTestIntegers* Held = Set.Param<FInstancedStruct>(TEXT("Values")).GetPtr<FCrowdyServerObjectTestIntegers>();
		TestTrue(TEXT("to the Value given, in the caller's Values"), Held && Held->OptionalValue.IsSet() && Held->OptionalValue.GetValue() == 42);
		Set.Param<bool>(TEXT("bHasValue")) = false;
		TestTrue(TEXT("and without Has Value"), RunThunk(TEXT("SetServerValueOrUnset"), Set.Memory.GetData()));
		Held = Set.Param<FInstancedStruct>(TEXT("Values")).GetPtr<FCrowdyServerObjectTestIntegers>();
		TestTrue(TEXT("empties it"), Held && !Held->OptionalValue.IsSet());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectFixInputErrorTest,
	"CrowdySDK.CrowdyExec.ServerObjectFixes.CallWithAnInputErrorFailsWithoutSending", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectFixInputErrorTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	AddExpectedMessagePlain(NoWorldWarning, ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	TestEqualSensitive(TEXT("a set input adds no error"), UCrowdyServerObjectLibrary::NoteInputError(FString(), true, TEXT("Damage")), FString());
	const FString One = UCrowdyServerObjectLibrary::NoteInputError(FString(), false, TEXT("Damage"));
	TestEqualSensitive(TEXT("an input that was not set is named"), One, FString(TEXT("Damage does not fit its type.")));
	TestEqualSensitive(TEXT("and several are joined"), UCrowdyServerObjectLibrary::NoteInputError(One, false, TEXT("Weapon")),
		FString(TEXT("Damage does not fit its type. Weapon does not fit its type.")));

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
	Rig.Sent.Reset();
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Listener = MakeListener();
	TStrongObjectPtr<UCrowdyServerCallAction> Action(UCrowdyServerTypedCallAction::CallServerFunctionWithInputs(nullptr, Object, AttackFunction, AttackParams(5), One));
	Action->OnSuccess.AddDynamic(Listener.Get(), &UCrowdyServerObjectTestListener::HandleSuccess);
	Action->OnFailed.AddDynamic(Listener.Get(), &UCrowdyServerObjectTestListener::HandleFailed);
	Action->Activate();
	if (TestEqual(TEXT("an input error fails the call once"), Listener->FailedOutcomes.Num(), 1))
	{
		TestTrue(TEXT("as a Bad Request"), Listener->FailedOutcomes[0] == ECrowdyServerCallOutcome::BadRequest);
		TestEqualSensitive(TEXT("giving the input error"), Listener->FailedReasons[0], One);
		TestFalse(TEXT("not retryable"), Listener->FailedRetryable[0]);
	}
	TestFalse(TEXT("and sends nothing"), Rig.SentAnyCall());
	TestEqual(TEXT("On Success never fires"), Listener->SuccessOutcomes.Num(), 0);

	TStrongObjectPtr<UCrowdyServerCallAction> Clean(UCrowdyServerTypedCallAction::CallServerFunctionWithInputs(nullptr, Object, AttackFunction, AttackParams(5), FString()));
	Clean->Activate();
	FSentFrame Sent;
	TestTrue(TEXT("without an input error the call is sent"), Rig.WaitForFrame(KindCall, AttackMethod, Sent));
	return true;
}

#endif
