#include "CrowdyServerObjectTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyExecLog.h"
#include "CrowdyServerObjectTestRig.h"
#include "HAL/IConsoleManager.h"
#include "Misc/OutputDeviceRedirector.h"

namespace CrowdyExecTraceTests
{
	using namespace CrowdyServerObjectTests;

	/** Sets crowdy.exec.trace for one test and puts the earlier value back. */
	struct FTraceSwitch
	{
		IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("crowdy.exec.trace"));
		int32 Was = Variable ? Variable->GetInt() : 0;

		explicit FTraceSwitch(bool bOn)
		{
			Set(bOn);
		}

		~FTraceSwitch()
		{
			if (Variable)
			{
				Variable->Set(Was, ECVF_SetByConsole);
			}
		}

		void Set(bool bOn) const
		{
			if (Variable)
			{
				Variable->Set(bOn ? 1 : 0, ECVF_SetByConsole);
			}
		}
	};

	/** Keeps every trace line LogCrowdyExec writes, as each is written. */
	struct FTraceLines : public FOutputDevice
	{
		TArray<FString> Lines;

		FTraceLines()
		{
			GLog->AddOutputDevice(this);
		}

		virtual ~FTraceLines() override
		{
			GLog->RemoveOutputDevice(this);
		}

		virtual void Serialize(const TCHAR* Line, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (Category == LogCrowdyExec.GetCategoryName() && FCString::Strncmp(Line, TEXT("exec: "), 6) == 0)
			{
				Lines.Add(Line);
			}
		}

		// Unbuffered, so a line is kept as soon as the logging call returns.
		virtual bool CanBeUsedOnMultipleThreads() const override
		{
			return true;
		}

		int32 Count(const FString& Start) const
		{
			return Lines.FilterByPredicate([&Start](const FString& Line) { return Line.StartsWith(Start, ESearchCase::CaseSensitive); }).Num();
		}

		/** The first line starting with Start, or empty. */
		FString Find(const FString& Start) const
		{
			const FString* Found = Lines.FindByPredicate([&Start](const FString& Line) { return Line.StartsWith(Start, ESearchCase::CaseSensitive); });
			return Found ? *Found : FString();
		}

		/** The first line holding any of Texts, or empty. */
		FString FindHolding(const TArray<FString>& Texts) const
		{
			for (const FString& Line : Lines)
			{
				if (Texts.ContainsByPredicate([&Line](const FString& Text) { return Line.Contains(Text, ESearchCase::CaseSensitive); }))
				{
					return Line;
				}
			}
			return FString();
		}
	};

	FString Head(const FRig& Rig)
	{
		return FString::Printf(TEXT("exec: %s/%s "), TypeName, *Rig.InstanceId);
	}

	FString Applied(const FRig& Rig, const TCHAR* Message, uint64 MessageEpoch, uint64 Seq)
	{
		return Head(Rig) + FString::Printf(TEXT("%s applied epoch=%llu seq=%llu changed="), Message, MessageEpoch, Seq);
	}

	/** The names after changed= in a line. */
	TArray<FString> ChangedOf(const FString& Line)
	{
		FString Names;
		TArray<FString> Out;
		if (Line.Split(TEXT("changed="), nullptr, &Names, ESearchCase::CaseSensitive))
		{
			Names.ParseIntoArray(Out, TEXT(","));
		}
		return Out;
	}

	/** The tokens the rig signs in with; no trace line may carry them. */
	TArray<FString> Tokens()
	{
		return {TEXT("app-token"), TEXT("connect-1")};
	}

	/** An Owner Only instance id, which is the owning player's user id. */
	constexpr const TCHAR* OwnerUserId = TEXT("4242424242");

	/** How every line names an Owner Only instance. */
	FString OwnerHead()
	{
		return FString::Printf(TEXT("exec: %s/owner "), TypeName);
	}

	constexpr const TCHAR* ClosedLine = TEXT("exec: connection closed");
	constexpr const TCHAR* IdleClosedLine = TEXT("exec: connection closed (no Server Object in use)");
	constexpr const TCHAR* SignedOutClosedLine = TEXT("exec: connection closed (signed out)");
	constexpr const TCHAR* RedialOkLine = TEXT("exec: redial ok");

	/** Where the first line exactly Line was captured, or INDEX_NONE. */
	int32 IndexOf(const FTraceLines& Capture, const FString& Line)
	{
		return Capture.Lines.IndexOfByPredicate([&Line](const FString& Each) { return Each.Equals(Line, ESearchCase::CaseSensitive); });
	}

	/** Drops the open socket, waits for the connection to redial it, then opens it and waits for the reconnect's read. */
	bool DropAndReopen(FRig& Rig, bool bTickBeforeOpen, FSentFrame& OutRead)
	{
		const int32 Dials = DialsOf(Rig) + 1;
		Rig.Client->TestExecCloseFromServer(1006, false);
		if (!Rig.Test.TestTrue(TEXT("the dropped socket is redialled"), Rig.PumpUntil([&Rig, Dials]() { return DialsOf(Rig) >= Dials; }, 6.0)))
		{
			return false;
		}
		if (bTickBeforeOpen)
		{
			Rig.Tick(0.2f, 0.1f);
		}
		Rig.Sent.Reset();
		Rig.Client->TestExecOpen();
		if (!Rig.Test.TestTrue(TEXT("the reconnect reads again"), Rig.WaitForFrame(KindCall, TEXT("read"), OutRead, 5.0)))
		{
			return false;
		}
		Rig.AckSubscribes();
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecTraceOnlyWithTheCVarTest,
	"CrowdySDK.CrowdyExec.Trace.LinesOnlyWithTheCVarOn", CrowdyServerObjectTests::TestFlags)
bool FCrowdyExecTraceOnlyWithTheCVarTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecTraceTests;
	FTraceSwitch Trace(false);
	if (!TestNotNull(TEXT("crowdy.exec.trace is registered"), Trace.Variable))
	{
		return false;
	}
	FTraceLines Capture;
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
	Rig.Push(Rig.PushMessage(Epoch, 6, Boss(2900, false, ECrowdyExecTestPhase::Calm), HealthOnly));
	TestTrue(TEXT("the push is applied with the trace off"), Rig.PumpUntil([Object]() { return HealthOf(Object) == 2900; }));
	FCallSpy Call;
	Object->Call(AttackFunction, AttackParams(10), Call.OnDone());
	FSentFrame Sent;
	if (!TestTrue(TEXT("the call is sent with the trace off"), Rig.WaitForFrame(KindCall, AttackMethod, Sent)))
	{
		return false;
	}
	Rig.Reply(Sent.Rid, ECrowdyNativeExecStatus::Ok, Rig.Encode(FCrowdyServerObjectTestAttackReply()));
	TestTrue(TEXT("and answered"), Rig.PumpUntil([&Call]() { return Call.Num() == 1; }));
	TestEqual(FString::Printf(TEXT("with crowdy.exec.trace off, a subscribe, a read, Ready, a push and a call print nothing (first: %s)"),
		Capture.Lines.IsEmpty() ? TEXT("none") : *Capture.Lines[0]), Capture.Lines.Num(), 0);

	Trace.Set(true);
	Rig.Push(Rig.PushMessage(Epoch, 7, Boss(2800, false, ECrowdyExecTestPhase::Calm), HealthOnly));
	TestTrue(TEXT("the next push is applied"), Rig.PumpUntil([Object]() { return HealthOf(Object) == 2800; }));
	TestEqual(TEXT("switched on, that push prints one line"), Capture.Lines.Num(), 1);
	TestEqual(TEXT("its applied line"), Capture.Count(Applied(Rig, TEXT("push"), Epoch, 7)), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecTracePushAppliedTest,
	"CrowdySDK.CrowdyExec.Trace.PushPrintsAppliedEpochSeqAndUnrealNames", CrowdyServerObjectTests::TestFlags)
bool FCrowdyExecTracePushAppliedTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecTraceTests;
	FTraceSwitch Trace(true);
	FTraceLines Capture;
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
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	FSentFrame Subscribe;
	FSentFrame Read;
	if (!Object || !Rig.Open() || !TestTrue(TEXT("the object subscribes"), Rig.WaitForFrame(KindSubscribe, TEXT("state"), Subscribe))
		|| !TestTrue(TEXT("and reads"), Rig.WaitForFrame(KindCall, TEXT("read"), Read)))
	{
		return false;
	}
	const FCrowdyExecTestBossState First = Boss(3131, false, ECrowdyExecTestPhase::Calm);
	const TArray<FString> ServerNames = {TEXT("hp"), TEXT("bDefeated"), TEXT("Phase")};
	Rig.Reply(Subscribe.Rid, ECrowdyNativeExecStatus::Ok, {});
	Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::Ok, Rig.StateMessage(true, Epoch, 5, &First, ServerNames));
	if (!TestTrue(TEXT("the read makes the object Ready"), Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; })))
	{
		return false;
	}
	TestEqual(TEXT("the subscribe's answer is printed"), Capture.Count(Head(Rig) + TEXT("subscribe ok")), 1);
	TestEqual(TEXT("becoming Ready is printed"), Capture.Count(Head(Rig) + TEXT("status Connecting -> Ready")), 1);
	const FString ReadLine = Capture.Find(Applied(Rig, TEXT("read"), Epoch, 5));
	TestTrue(FString::Printf(TEXT("the read prints its applied epoch and seq, naming every watched variable by its Unreal name (got %s)"), *ReadLine),
		NamesAre(ChangedOf(ReadLine), {TEXT("Health"), TEXT("bDefeated"), TEXT("Phase")}));

	const TArray<FString> Pushed = {TEXT("hp"), TEXT("bDefeated")};
	Rig.Push(Rig.PushMessage(Epoch, 6, Boss(2777, true, ECrowdyExecTestPhase::Final), Pushed));
	if (!TestTrue(TEXT("the push is applied"), Rig.PumpUntil([Object]() { return HealthOf(Object) == 2777; })))
	{
		return false;
	}
	const FString PushLine = Capture.Find(Applied(Rig, TEXT("push"), Epoch, 6));
	TestTrue(FString::Printf(TEXT("the push prints its applied epoch and seq and the Unreal names, Health not hp (got %s)"), *PushLine),
		NamesAre(ChangedOf(PushLine), {TEXT("Health"), TEXT("bDefeated")}));

	TArray<FString> Secrets = Tokens();
	Secrets.Append({TEXT("3131"), TEXT("2777"), TEXT("Final")});
	const FString Leak = Capture.FindHolding(Secrets);
	TestTrue(FString::Printf(TEXT("no line carries a token or a value (%s)"), *Leak), Leak.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecTraceStalePushTest,
	"CrowdySDK.CrowdyExec.Trace.StalePushPrintsStaleNotApplied", CrowdyServerObjectTests::TestFlags)
bool FCrowdyExecTraceStalePushTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecTraceTests;
	FTraceSwitch Trace(true);
	FTraceLines Capture;
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
	Rig.Push(Rig.PushMessage(Epoch, 6, Boss(2900, false, ECrowdyExecTestPhase::Calm), HealthOnly));
	TestTrue(TEXT("seq 6 is applied"), Rig.PumpUntil([Object]() { return HealthOf(Object) == 2900; }));
	Rig.Push(Rig.PushMessage(Epoch, 4, Boss(1, false, ECrowdyExecTestPhase::Calm), HealthOnly));
	Rig.Push(Rig.PushMessage(Epoch, 7, Boss(2800, false, ECrowdyExecTestPhase::Calm), HealthOnly));
	if (!TestTrue(TEXT("the push after the stale one is applied, so the stale one was handled first"), Rig.PumpUntil([Object]() { return HealthOf(Object) == 2800; })))
	{
		return false;
	}
	const FString Stale = Capture.Find(Head(Rig) + TEXT("push stale "));
	TestEqualSensitive(TEXT("the stale push prints the epoch and seq still applied, not its own"), Stale,
		Head(Rig) + FString::Printf(TEXT("push stale epoch=%llu seq=6"), Epoch));
	TestEqual(TEXT("and no applied line"), Capture.Count(Applied(Rig, TEXT("push"), Epoch, 4)), 0);
	TestEqual(TEXT("only seq 6 and 7 print applied lines"), Capture.Count(Head(Rig) + TEXT("push applied ")), 2);
	TestEqual(TEXT("seq 7's line"), Capture.Count(Applied(Rig, TEXT("push"), Epoch, 7)), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecTraceGapTest,
	"CrowdySDK.CrowdyExec.Trace.GapAndEpochPrintReread", CrowdyServerObjectTests::TestFlags)
bool FCrowdyExecTraceGapTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecTraceTests;
	FTraceSwitch Trace(true);
	FTraceLines Capture;
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
	if (!TestTrue(TEXT("a push that skips a seq reads again"), Rig.WaitForFrame(KindCall, TEXT("read"), GapRead)))
	{
		return false;
	}
	TestEqual(TEXT("the gap prints its reread line"), Capture.Count(Head(Rig) + TEXT("reread reason=gap")), 1);
	TestEqual(TEXT("and no applied push"), Capture.Count(Head(Rig) + TEXT("push applied ")), 0);
	Rig.Reply(GapRead.Rid, ECrowdyNativeExecStatus::Ok, Rig.ReadMessage(7, Boss(2222, false, ECrowdyExecTestPhase::Calm)));
	if (!TestTrue(TEXT("the new read is applied"), Rig.PumpUntil([Object]() { return HealthOf(Object) == 2222; })))
	{
		return false;
	}
	const FString ReadLine = Capture.Find(Applied(Rig, TEXT("read"), Epoch, 7));
	TestTrue(FString::Printf(TEXT("the read that replaced the pushes prints seq 7 and only what changed (got %s)"), *ReadLine),
		NamesAre(ChangedOf(ReadLine), HealthOnly));

	Rig.Sent.Reset();
	Rig.Push(Rig.PushMessage(Epoch + 1, 8, Boss(999, false, ECrowdyExecTestPhase::Calm), HealthOnly));
	FSentFrame EpochRead;
	TestTrue(TEXT("a push from another epoch reads again"), Rig.WaitForFrame(KindCall, TEXT("read"), EpochRead));
	TestEqual(TEXT("the new epoch prints its reread line"), Capture.Count(Head(Rig) + TEXT("reread reason=epoch")), 1);
	TestEqual(TEXT("and still no applied push"), Capture.Count(Head(Rig) + TEXT("push applied ")), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecTraceOwnerOnlyTest,
	"CrowdySDK.CrowdyExec.Trace.OwnerOnlyPushPrintsReread", CrowdyServerObjectTests::TestFlags)
bool FCrowdyExecTraceOwnerOnlyTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecTraceTests;
	FTraceSwitch Trace(true);
	FTraceLines Capture;
	FRig Rig(*this, ECrowdyServerObjectVisibility::OwnerOnly);
	if (!Rig.IsValid())
	{
		return false;
	}
	Rig.InstanceId = OwnerUserId;
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	if (!Rig.AcquireReady(Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5))
	{
		return false;
	}
	Rig.Sent.Reset();
	Rig.Push(Rig.StateMessage(false, Epoch, 6, nullptr, {}));
	FSentFrame Read;
	TestTrue(TEXT("a value-less push reads again"), Rig.WaitForFrame(KindCall, TEXT("read"), Read));
	TestEqual(TEXT("and prints why, naming the instance as owner"), Capture.Count(OwnerHead() + TEXT("reread reason=ownerOnly")), 1);
	TestEqual(TEXT("becoming Ready names it as owner too"), Capture.Count(OwnerHead() + TEXT("status Connecting -> Ready")), 1);
	const FString Leak = Capture.FindHolding({OwnerUserId});
	TestTrue(FString::Printf(TEXT("no line carries the player's user id (%s)"), *Leak), Leak.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecTraceOwnerOnlySubscribeTest,
	"CrowdySDK.CrowdyExec.Trace.OwnerOnlySubscribeFailureHidesTheUserId", CrowdyServerObjectTests::TestFlags)
bool FCrowdyExecTraceOwnerOnlySubscribeTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecTraceTests;
	FTraceSwitch Trace(true);
	FTraceLines Capture;
	FRig Rig(*this, ECrowdyServerObjectVisibility::OwnerOnly);
	if (!Rig.IsValid())
	{
		return false;
	}
	Rig.InstanceId = OwnerUserId;
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	FSentFrame Subscribe;
	FSentFrame Read;
	if (!Object || !Rig.Open() || !TestTrue(TEXT("the object subscribes"), Rig.WaitForFrame(KindSubscribe, TEXT("state"), Subscribe))
		|| !TestTrue(TEXT("and reads"), Rig.WaitForFrame(KindCall, TEXT("read"), Read)))
	{
		return false;
	}
	Rig.Reply(Subscribe.Rid, ECrowdyNativeExecStatus::Denied, Utf8Bytes(FString::Printf(TEXT("player %s may not watch"), OwnerUserId)));
	Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::Ok, Rig.ReadMessage(5, Boss(3000, false, ECrowdyExecTestPhase::Calm)));
	if (!TestTrue(TEXT("the read makes the object Ready"), Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; })))
	{
		return false;
	}
	TestEqualSensitive(TEXT("the refused subscribe prints its reason with the user id replaced"), Capture.Find(OwnerHead() + TEXT("subscribe ")),
		OwnerHead() + TEXT("subscribe failed player owner may not watch"));
	const FString Leak = Capture.FindHolding({OwnerUserId});
	TestTrue(FString::Printf(TEXT("no line carries the player's user id (%s)"), *Leak), Leak.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecTraceCallTest,
	"CrowdySDK.CrowdyExec.Trace.CallPrintsSentAndAnswered", CrowdyServerObjectTests::TestFlags)
bool FCrowdyExecTraceCallTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecTraceTests;
	FTraceSwitch Trace(true);
	FTraceLines Capture;
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
	const FString Sent = Head(Rig) + TEXT("call AttackBoss sent");
	const FString Answered = Head(Rig) + TEXT("call AttackBoss answered ");
	FCallSpy Call;
	Object->Call(AttackFunction, AttackParams(4321), Call.OnDone());
	FSentFrame First;
	if (!TestTrue(TEXT("the call is sent"), Rig.WaitForFrame(KindCall, AttackMethod, First)))
	{
		return false;
	}
	TestEqual(TEXT("sending prints the function"), Capture.Count(Sent), 1);
	TestEqual(TEXT("nothing is answered yet"), Capture.Count(Answered), 0);
	FCrowdyServerObjectTestAttackReply Reply;
	Reply.HealthLeft = 8765;
	Rig.Reply(First.Rid, ECrowdyNativeExecStatus::Ok, Rig.Encode(Reply));
	if (!TestTrue(TEXT("the call completes"), Rig.PumpUntil([&Call]() { return Call.Num() == 1; })))
	{
		return false;
	}
	FString Ms;
	const FString Success = Capture.Find(Answered + TEXT("Success ms="));
	TestTrue(FString::Printf(TEXT("the answer prints its outcome and whole milliseconds (got %s)"), *Success),
		Success.Split(TEXT("ms="), nullptr, &Ms) && !Ms.IsEmpty() && Ms.IsNumeric() && !Ms.Contains(TEXT(".")));

	Object->Call(AttackFunction, AttackParams(4321), Call.OnDone());
	FSentFrame Second;
	if (!TestTrue(TEXT("a second call is sent"), Rig.WaitForFrame(KindCall, AttackMethod, Second)))
	{
		return false;
	}
	Rig.Reply(Second.Rid, ECrowdyNativeExecStatus::AppError, Utf8Bytes(TEXT("cooldown: slow down")));
	TestTrue(TEXT("the refused call completes"), Rig.PumpUntil([&Call]() { return Call.Num() == 2; }));
	TestEqual(TEXT("each send is printed"), Capture.Count(Sent), 2);
	TestEqual(TEXT("the refusal prints the outcome the caller gets"), Capture.Count(Answered + TEXT("Denied ms=")), 1);
	TestEqual(TEXT("two answers in all"), Capture.Count(Answered), 2);

	TArray<FString> Secrets = Tokens();
	Secrets.Append({TEXT("4321"), TEXT("8765"), TEXT("axe"), TEXT("slow down")});
	const FString Leak = Capture.FindHolding(Secrets);
	TestTrue(FString::Printf(TEXT("no line carries a token, an input, an output or a reason (%s)"), *Leak), Leak.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecTraceConnectionTest,
	"CrowdySDK.CrowdyExec.Trace.DroppedConnectionPrintsClosedAndRedial", CrowdyServerObjectTests::TestFlags)
bool FCrowdyExecTraceConnectionTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecTraceTests;
	FTraceSwitch Trace(true);
	FTraceLines Capture;
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
	const FString Closed = TEXT("exec: connection closed");
	TestEqual(TEXT("an open connection prints no close"), Capture.Count(Closed), 0);
	Rig.Sent.Reset();
	Rig.Client->TestExecCloseFromServer(1006, false);
	if (!TestTrue(TEXT("the dropped socket is redialled"), Rig.PumpUntil([&Rig]() { return Rig.Client->NumTestExecConnections() >= 2; }, 6.0)))
	{
		return false;
	}
	Rig.Tick(0.1f);
	Rig.Tick(0.1f);
	TestEqual(TEXT("the drop prints one close, however often the subsystem ticks"), Capture.Count(Closed), 1);
	TestTrue(TEXT("and names no reason, since it redials"), IndexOf(Capture, ClosedLine) != INDEX_NONE);
	TestEqual(TEXT("no redial is printed before the socket opens"), Capture.Count(TEXT("exec: redial ok")), 0);

	Rig.Client->TestExecOpen();
	if (!TestTrue(TEXT("the reconnect reads again"), Rig.PumpUntil([&Rig]() { return Rig.FindFrame(KindCall, TEXT("read")) != INDEX_NONE; }, 5.0)))
	{
		return false;
	}
	TestEqual(TEXT("the reconnect prints the redial"), Capture.Count(TEXT("exec: redial ok")), 1);
	TestEqual(TEXT("and the reread with its reason"), Capture.Count(Head(Rig) + TEXT("reread reason=reconnect")), 1);
	Rig.AckSubscribes();
	TestTrue(TEXT("the fresh subscribe's answer is printed"), Rig.PumpUntil([&Capture, &Rig]() { return Capture.Count(Head(Rig) + TEXT("subscribe ok")) == 2; }));

	Rig.Subsystem->SignOutForTest();
	TestEqual(TEXT("signing out prints the status change"), Capture.Count(Head(Rig) + TEXT("status Ready -> Failed")), 1);
	TestEqual(TEXT("and the close of the open connection"), Capture.Count(Closed), 2);
	TestEqual(TEXT("naming the sign-out"), Capture.Count(SignedOutClosedLine), 1);
	const FString Leak = Capture.FindHolding(Tokens());
	TestTrue(FString::Printf(TEXT("no line carries a token (%s)"), *Leak), Leak.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecTraceOffAcrossFlowsTest,
	"CrowdySDK.CrowdyExec.Trace.NothingWithTheCVarOffAcrossRereadsAndRedials", CrowdyServerObjectTests::TestFlags)
bool FCrowdyExecTraceOffAcrossFlowsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecTraceTests;
	FTraceSwitch Trace(false);
	FTraceLines Capture;
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
	Rig.Push(Rig.PushMessage(Epoch, 6, Boss(2900, false, ECrowdyExecTestPhase::Calm), HealthOnly));
	if (!TestTrue(TEXT("seq 6 is applied"), Rig.PumpUntil([Object]() { return HealthOf(Object) == 2900; })))
	{
		return false;
	}
	Rig.Sent.Reset();
	Rig.Push(Rig.PushMessage(Epoch, 4, Boss(1, false, ECrowdyExecTestPhase::Calm), HealthOnly));
	Rig.Push(Rig.PushMessage(Epoch, 8, Boss(111, false, ECrowdyExecTestPhase::Calm), HealthOnly));
	FSentFrame GapRead;
	if (!TestTrue(TEXT("after a stale push, a gap reads again"), Rig.WaitForFrame(KindCall, TEXT("read"), GapRead)))
	{
		return false;
	}
	Rig.Reply(GapRead.Rid, ECrowdyNativeExecStatus::Ok, Rig.ReadMessage(8, Boss(2222, false, ECrowdyExecTestPhase::Calm)));
	if (!TestTrue(TEXT("the gap's read is applied"), Rig.PumpUntil([Object]() { return HealthOf(Object) == 2222; })))
	{
		return false;
	}

	FSentFrame Reread;
	if (!DropAndReopen(Rig, true, Reread))
	{
		return false;
	}
	Rig.Reply(Reread.Rid, ECrowdyNativeExecStatus::Ok, Rig.ReadMessage(9, Boss(2100, false, ECrowdyExecTestPhase::Calm)));
	if (!TestTrue(TEXT("a drop the poll sees is reread"), Rig.PumpUntil([Object]() { return HealthOf(Object) == 2100; })))
	{
		return false;
	}
	if (!DropAndReopen(Rig, false, Reread))
	{
		return false;
	}
	Rig.Reply(Reread.Rid, ECrowdyNativeExecStatus::Ok, Rig.ReadMessage(10, Boss(2000, false, ECrowdyExecTestPhase::Calm)));
	if (!TestTrue(TEXT("a redial faster than the poll is reread"), Rig.PumpUntil([Object]() { return HealthOf(Object) == 2000; })))
	{
		return false;
	}

	Rig.Sent.Reset();
	Rig.Push(Rig.PushMessage(Epoch + 1, 11, Boss(999, false, ECrowdyExecTestPhase::Calm), HealthOnly));
	FSentFrame EpochRead;
	TestTrue(TEXT("a push from another epoch reads again"), Rig.WaitForFrame(KindCall, TEXT("read"), EpochRead));
	Rig.Subsystem->SignOutForTest();
	TestEqual(FString::Printf(TEXT("with crowdy.exec.trace off, stale, gap, epoch, drops, redials and a sign-out print nothing (first: %s)"),
		Capture.Lines.IsEmpty() ? TEXT("none") : *Capture.Lines[0]), Capture.Lines.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecTraceBufferOverflowTest,
	"CrowdySDK.CrowdyExec.Trace.BufferOverflowPrintsRereadOverflow", CrowdyServerObjectTests::TestFlags)
bool FCrowdyExecTraceBufferOverflowTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecTraceTests;
	FTraceSwitch Trace(true);
	FTraceLines Capture;
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
	const TArray<FString> HealthOnly = {TEXT("Health")};
	Rig.Sent.Reset();
	Rig.Push(Rig.PushMessage(Epoch, 7, Boss(111, false, ECrowdyExecTestPhase::Calm), HealthOnly));
	FSentFrame GapRead;
	if (!TestTrue(TEXT("a gap reads again"), Rig.WaitForFrame(KindCall, TEXT("read"), GapRead)))
	{
		return false;
	}
	// The read stays unanswered, so every push is buffered until the buffer's 32 are full.
	for (uint64 Seq = 8; Seq <= 40; ++Seq)
	{
		Rig.Push(Rig.PushMessage(Epoch, Seq, Boss(static_cast<int32>(Seq), false, ECrowdyExecTestPhase::Calm), HealthOnly));
	}
	Rig.PumpFor(0.2);
	TestEqual(TEXT("the 33rd buffered push prints an overflow reread"), Capture.Count(Head(Rig) + TEXT("reread reason=overflow")), 1);
	TestEqual(TEXT("and only the real gap prints a gap"), Capture.Count(Head(Rig) + TEXT("reread reason=gap")), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecTraceFastRedialTest,
	"CrowdySDK.CrowdyExec.Trace.FastRedialPrintsClosedBeforeRedialOk", CrowdyServerObjectTests::TestFlags)
bool FCrowdyExecTraceFastRedialTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecTraceTests;
	FTraceSwitch Trace(true);
	FTraceLines Capture;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	FSentFrame Reread;
	if (!Rig.AcquireReady(Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5) || !DropAndReopen(Rig, false, Reread))
	{
		return false;
	}
	Rig.Tick(0.1f);
	Rig.Tick(0.1f);
	TestEqual(TEXT("a redial with no tick between drop and reopen prints one redial"), Capture.Count(RedialOkLine), 1);
	TestEqual(TEXT("and exactly one close, however often the subsystem ticks after"), Capture.Count(ClosedLine), 1);
	const int32 ClosedAt = IndexOf(Capture, ClosedLine);
	TestTrue(TEXT("the close comes before the redial"), ClosedAt != INDEX_NONE && ClosedAt < IndexOf(Capture, RedialOkLine));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecTraceDropThenSignOutTest,
	"CrowdySDK.CrowdyExec.Trace.DropThenSignOutPrintsOneClose", CrowdyServerObjectTests::TestFlags)
bool FCrowdyExecTraceDropThenSignOutTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecTraceTests;
	FTraceSwitch Trace(true);
	FTraceLines Capture;
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
	Rig.Client->TestExecCloseFromServer(1006, false);
	if (!TestTrue(TEXT("the dropped socket is redialled"), Rig.PumpUntil([&Rig]() { return DialsOf(Rig) >= 2; }, 6.0)))
	{
		return false;
	}
	Rig.Tick(0.1f);
	TestEqual(TEXT("the poll prints the drop"), Capture.Count(ClosedLine), 1);
	Rig.Subsystem->SignOutForTest();
	TestEqual(TEXT("signing out before the socket reopens prints no second close"), Capture.Count(ClosedLine), 1);
	TestEqual(TEXT("and no redial"), Capture.Count(TEXT("exec: redial")), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecTraceDialOutcomesTest,
	"CrowdySDK.CrowdyExec.Trace.DialOutcomesPrintRedialOnlyAfterAClose", CrowdyServerObjectTests::TestFlags)
bool FCrowdyExecTraceDialOutcomesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecTraceTests;
	AddExpectedErrorPlain(ConnectFailedWarning, EAutomationExpectedErrorFlags::Contains, 2);
	FTraceSwitch Trace(true);
	FTraceLines Capture;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	auto OpenDial = [&Rig](int32 Dial)
	{
		if (!Rig.Test.TestTrue(FString::Printf(TEXT("dial %d reaches the gateway"), Dial), Rig.PumpUntil([&Rig, Dial]() { return DialsOf(Rig) >= Dial; })))
		{
			return false;
		}
		Rig.Client->TestExecOpen();
		return Rig.Test.TestTrue(FString::Printf(TEXT("dial %d opens"), Dial), Rig.PumpUntil([&Rig]() { return Rig.Subsystem->GetConnection() != nullptr; }));
	};
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	if (!Rig.Acquire(Rig.InstanceId, Owner.Get()) || !FailDial(Rig, 1))
	{
		return false;
	}
	TestEqual(TEXT("a failed first dial prints dial failed"), Capture.Count(TEXT("exec: dial failed ")), 1);
	TestEqual(TEXT("not redial failed, since nothing was redialled"), Capture.Count(TEXT("exec: redial failed")), 0);
	Rig.Tick(1.1f, 0.1f);
	if (!OpenDial(2))
	{
		return false;
	}
	TestEqual(TEXT("the dial that then works is no redial"), Capture.Count(RedialOkLine), 0);

	Rig.Session->SetAppID(43);
	Rig.Subsystem->TickForTest(0.1f);
	TestEqual(TEXT("dropping the old app's open connection prints its close"), Capture.Count(ClosedLine), 1);
	TestTrue(TEXT("with no reason, since it redials"), IndexOf(Capture, ClosedLine) != INDEX_NONE);
	if (!FailDial(Rig, 3))
	{
		return false;
	}
	TestEqual(TEXT("the dial after that close fails as a redial"), Capture.Count(TEXT("exec: redial failed ")), 1);
	TestEqual(TEXT("and dial failed stays at the first dial's one"), Capture.Count(TEXT("exec: dial failed ")), 1);
	Rig.Tick(1.1f, 0.1f);
	if (!OpenDial(4))
	{
		return false;
	}
	TestEqual(TEXT("the redial that works prints redial ok"), Capture.Count(RedialOkLine), 1);

	Destroy(Owner);
	for (int32 Step = 0; Step < 150 && Rig.Subsystem->NumObjectsForTest() > 0; ++Step)
	{
		Rig.Subsystem->TickForTest(0.1f);
	}
	if (!TestEqual(TEXT("every object was given back"), Rig.Subsystem->NumObjectsForTest(), 0))
	{
		return false;
	}
	TestEqual(TEXT("going idle closes the connection"), Capture.Count(ClosedLine), 2);
	TestEqual(TEXT("and names why, so a reader does not take it for a drop"), Capture.Count(IdleClosedLine), 1);
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Fresh = MakeOwner();
	if (!Rig.Acquire(Rig.InstanceId, Fresh.Get()) || !OpenDial(5))
	{
		return false;
	}
	TestEqual(TEXT("a fresh dial after going idle is no redial"), Capture.Count(RedialOkLine), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecTraceCanceledCallsTest,
	"CrowdySDK.CrowdyExec.Trace.CanceledCallsPrintAnswered", CrowdyServerObjectTests::TestFlags)
bool FCrowdyExecTraceCanceledCallsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecTraceTests;
	FTraceSwitch Trace(true);
	FTraceLines Capture;
	{
		FRig Rig(*this);
		if (!Rig.IsValid())
		{
			return false;
		}
		TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
		UCrowdyServerObject* Object = Rig.AcquireReady(Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5);
		FCallSpy Call;
		FSentFrame Sent;
		if (!Object)
		{
			return false;
		}
		Object->Call(AttackFunction, AttackParams(10), Call.OnDone());
		if (!TestTrue(TEXT("the call is sent"), Rig.WaitForFrame(KindCall, AttackMethod, Sent)))
		{
			return false;
		}
		Rig.Subsystem->SignOutForTest();
		TestEqual(TEXT("signing out answers the call in flight as Canceled"), Capture.Count(Head(Rig) + TEXT("call AttackBoss answered Canceled ms=")), 1);
		TestEqual(TEXT("and its caller hears it"), Call.Num(), 1);
	}
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	Rig.InstanceId = TEXT("boss-2");
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.AcquireReady(Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5);
	FCallSpy Call;
	FSentFrame Sent;
	if (!Object)
	{
		return false;
	}
	Object->Call(AttackFunction, AttackParams(10), Call.OnDone());
	if (!TestTrue(TEXT("the call is sent"), Rig.WaitForFrame(KindCall, AttackMethod, Sent)))
	{
		return false;
	}
	Rig.Deinitialize();
	TestEqual(TEXT("teardown traces the call in flight as Canceled"), Capture.Count(Head(Rig) + TEXT("call AttackBoss answered Canceled ms=")), 1);
	TestEqual(TEXT("without running its caller's code"), Call.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecTraceRetriedCallTest,
	"CrowdySDK.CrowdyExec.Trace.RetriedCallPrintsOneSentAndOneAnswered", CrowdyServerObjectTests::TestFlags)
bool FCrowdyExecTraceRetriedCallTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecTraceTests;
	FTraceSwitch Trace(true);
	FTraceLines Capture;
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
	Object->Call(AttackFunction, AttackParams(10), Call.OnDone());
	FSentFrame First;
	if (!TestTrue(TEXT("the call is sent"), Rig.WaitForFrame(KindCall, AttackMethod, First)))
	{
		return false;
	}
	Rig.Reply(First.Rid, ECrowdyNativeExecStatus::Busy, Utf8Bytes(TEXT("busy")));
	Rig.PumpFor(0.1);
	Rig.Tick(1.f);
	FSentFrame Retry;
	if (!TestTrue(TEXT("the busy call is sent again"), Rig.WaitForFrame(KindCall, AttackMethod, Retry)))
	{
		return false;
	}
	Rig.Reply(Retry.Rid, ECrowdyNativeExecStatus::Ok, Rig.Encode(FCrowdyServerObjectTestAttackReply()));
	if (!TestTrue(TEXT("the retried call completes"), Rig.PumpUntil([&Call]() { return Call.Num() == 1; })))
	{
		return false;
	}
	TestEqual(TEXT("two sends print one sent line"), Capture.Count(Head(Rig) + TEXT("call AttackBoss sent")), 1);
	TestEqual(TEXT("and one answered line"), Capture.Count(Head(Rig) + TEXT("call AttackBoss answered ")), 1);
	TestEqual(TEXT("with the outcome the caller got"), Capture.Count(Head(Rig) + TEXT("call AttackBoss answered Success ms=")), 1);
	return true;
}

#endif
