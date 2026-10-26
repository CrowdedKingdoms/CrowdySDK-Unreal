#include "CrowdyServerObjectTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyExecCodec.h"
#include "CrowdyExecInternal.h"
#include "CrowdyGameKitMatch.h"
#include "CrowdyGameKitTypes.h"
#include "CrowdyNativeExec.h"
#include "CrowdyNativeKit.h"
#include "CrowdyServerObject.h"
#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectLibrary.h"
#include "CrowdyServerObjectLink.h"
#include "CrowdyServerObjectShared.h"
#include "CrowdyServerObjectSubsystem.h"
#include "CrowdyServerObjectTestRig.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

namespace CrowdyServerObjectKitTests
{
	using namespace CrowdyServerObjectTests;

	inline constexpr const TCHAR* Topic = TEXT("boss");
	inline constexpr const TCHAR* TauntFunction = TEXT("Taunt");
	inline constexpr const TCHAR* RoaredMessage = TEXT("Roared");
	inline constexpr const TCHAR* HealedMessage = TEXT("Healed");
	inline constexpr const TCHAR* HealsTopic = TEXT("heals");

	/** Makes the rig's test_boss a Game Kit starter on the boss topic: Attack Boss is sent once, Taunt is not, and two events are declared. */
	bool MakeKitType(FRig& Rig)
	{
		UCrowdyServerObjectDefinition& Definition = *Rig.Definition;
		Definition.CodeSource = ECrowdyServerCodeSource::GameKitStarter;
		Definition.KitStateTopic = Topic;
		Definition.WatchedFields.Reset();
		Definition.Functions[0].bSendOnce = true;
		Definition.Functions.AddDefaulted_GetRef().Name = TauntFunction;
		FCrowdyServerMessage& Roared = Definition.Messages.AddDefaulted_GetRef();
		Roared.Name = RoaredMessage;
		Roared.ServerName = Topic;
		Roared.Payload = FCrowdyServerObjectTestMessagePayload::StaticStruct();
		FCrowdyServerMessage& Healed = Definition.Messages.AddDefaulted_GetRef();
		Healed.Name = HealedMessage;
		Healed.ServerName = HealsTopic;
		Healed.Payload = FCrowdyServerObjectTestMessagePayload::StaticStruct();
		TArray<FString> Errors;
		const bool bBaked = Definition.Bake(Errors);
		return Rig.Test.TestTrue(FString::Printf(TEXT("the Game Kit starter bakes (%s)"), *FString::Join(Errors, TEXT("; "))), bBaked);
	}

	void WriteText(FCrowdyExecWriter& Writer, const ANSICHAR* Text)
	{
		Writer.String(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Text), FCStringAnsi::Strlen(Text)));
	}

	/** A Game Kit message: the reserved keys, then Body's entries beside them. Body is an encoded struct of fewer than 16 fields. */
	TArray<uint8> KitMessage(uint64 MessageEpoch, uint64 Seq, const ANSICHAR* Kind, const TArray<uint8>& Body, int32 ExtraPairs = 0, const TArray<uint8>& Extra = {})
	{
		const int32 Count = Body.IsEmpty() ? 0 : Body[0] & 0x0f;
		TArray<uint8> Bytes;
		FCrowdyExecWriter Writer(Bytes);
		Writer.MapHeader(4 + Count + ExtraPairs);
		WriteKey(Writer, "v");
		Writer.UInt(CrowdyExec::KitProtocolVersion);
		WriteKey(Writer, "epoch");
		Writer.UInt(MessageEpoch);
		WriteKey(Writer, "seq");
		Writer.UInt(Seq);
		WriteKey(Writer, "kind");
		WriteText(Writer, Kind);
		if (Count > 0)
		{
			Bytes.Append(Body.GetData() + 1, Body.Num() - 1);
		}
		Bytes.Append(Extra);
		return Bytes;
	}

	FCrowdyServerObjectTestMessagePayload Payload(int32 Amount, const TCHAR* Source)
	{
		FCrowdyServerObjectTestMessagePayload Value;
		Value.Amount = Amount;
		Value.Source = Source;
		return Value;
	}

	bool Contains(const TArray<uint8>& Bytes, const ANSICHAR* Text)
	{
		const int32 Length = FCStringAnsi::Strlen(Text);
		for (int32 At = 0; At + Length <= Bytes.Num(); ++At)
		{
			if (FMemory::Memcmp(Bytes.GetData() + At, Text, Length) == 0)
			{
				return true;
			}
		}
		return false;
	}

	/** Acquires the starter, opens the connection, and answers its subscribe and first snapshot with Values at Seq. */
	UCrowdyServerObject* AcquireKitReady(FRig& Rig, const UObject* Owner, const FCrowdyExecTestBossState& Values, uint64 Seq)
	{
		UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner);
		FSentFrame Subscribe;
		FSentFrame Read;
		if (!Object || !Rig.Open() || !Rig.Test.TestTrue(TEXT("the starter subscribes to its State Topic"), Rig.WaitForFrame(KindSubscribe, Topic, Subscribe))
			|| !Rig.Test.TestTrue(TEXT("the starter reads with snapshot"), Rig.WaitForFrame(KindCall, TEXT("snapshot"), Read)))
		{
			return nullptr;
		}
		Rig.Reply(Subscribe.Rid, ECrowdyNativeExecStatus::Ok, {});
		Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::Ok, KitMessage(Epoch, Seq, "snapshot", Rig.Encode(Values)));
		const bool bReady = Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; });
		return Rig.Test.TestTrue(TEXT("the first snapshot makes the starter Ready"), bReady) ? Object : nullptr;
	}

	/** Calls Function and answers it with Status and Payload, returning the result. */
	TOptional<FCrowdyServerCallResult> CallAnswered(FRig& Rig, UCrowdyServerObject& Object, FName Function, const FInstancedStruct& Params,
		ECrowdyNativeExecStatus Status, const TArray<uint8>& Answer, const FString& Method, FSentFrame* OutFrame = nullptr)
	{
		FCallSpy Spy;
		Object.Call(Function, Params, Spy.OnDone());
		FSentFrame Frame;
		if (!Rig.Test.TestTrue(FString::Printf(TEXT("%s is sent"), *Method), Rig.WaitForFrame(KindCall, Method, Frame)))
		{
			return {};
		}
		Rig.Reply(Frame.Rid, Status, Answer);
		if (!Rig.Test.TestTrue(TEXT("the call completes"), Rig.PumpUntil([&Spy]() { return Spy.Num() > 0; })))
		{
			return {};
		}
		if (OutFrame)
		{
			*OutFrame = Frame;
		}
		return (*Spy.Results)[0];
	}
}

using namespace CrowdyServerObjectKitTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterBakesSnakeCaseNames,
	"CrowdySDK.CrowdyExec.GameKitStarter.BakesSnakeCaseNames", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterBakesSnakeCaseNames::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	if (!Rig.IsValid() || !MakeKitType(Rig))
	{
		return false;
	}
	const UCrowdyServerObjectDefinition& Definition = *Rig.Definition;
	const UScriptStruct& State = *FCrowdyExecTestBossState::StaticStruct();
	TestNotNull(TEXT("Health travels as health"), CrowdyExec::FindResolvedPropertyForTest(Definition, State, TEXT("health")));
	TestNotNull(TEXT("a bool's b prefix is dropped: bDefeated travels as defeated"), CrowdyExec::FindResolvedPropertyForTest(Definition, State, TEXT("defeated")));
	TestNull(TEXT("not as the authored name"), CrowdyExec::FindResolvedPropertyForTest(Definition, State, TEXT("Health")));
	TestNotNull(TEXT("an output too: HealthLeft travels as health_left"),
		CrowdyExec::FindResolvedPropertyForTest(Definition, *FCrowdyServerObjectTestAttackReply::StaticStruct(), TEXT("health_left")));
	TestTrue(TEXT("the starter is open to players whoever its functions name"), Definition.IsReachableByPlayers());

	FCrowdyServerFieldName& Kept = Rig.Definition->FieldNames.AddDefaulted_GetRef();
	Kept.Struct = FCrowdyExecTestBossState::StaticStruct();
	Kept.Field = TEXT("Health");
	Kept.ServerName = TEXT("hp");
	TArray<FString> Errors;
	TestTrue(TEXT("the override bakes"), Rig.Definition->Bake(Errors));
	TestNotNull(TEXT("Server Names still wins"), CrowdyExec::FindResolvedPropertyForTest(Definition, State, TEXT("hp")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterKeepsOnlyWhatTheKitCarries,
	"CrowdySDK.CrowdyExec.GameKitStarter.KeepsOnlyWhatTheKitCarries", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterKeepsOnlyWhatTheKitCarries::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	if (!Rig.IsValid() || !MakeKitType(Rig))
	{
		return false;
	}
	UCrowdyServerObjectDefinition& Definition = *Rig.Definition;
	Definition.bOnlyOneInstance = true;
	TestEqual(TEXT("a starter with one instance is reached by the empty key, as world is"), Definition.GetOnlyInstanceId(), FString());

	TArray<FString> Errors;
	Definition.MembersFrom = ECrowdyServerMembersSource::ThisObject;
	Definition.MaxMembers = 10;
	TestFalse(TEXT("members of its own are refused"), Definition.Bake(Errors));
	TestTrue(TEXT("saying the server code decides"), Errors.ContainsByPredicate([](const FString& Error) { return Error.Contains(TEXT("Game Kit starter keeps one set")); }));

	Errors.Reset();
	Definition.MembersFrom = ECrowdyServerMembersSource::None;
	Definition.KitStateTopic = TEXT("Boss Topic");
	TestFalse(TEXT("a State Topic that is no topic name is refused"), Definition.Bake(Errors));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterReadsKitMessages,
	"CrowdySDK.CrowdyExec.GameKitStarter.ReadsKitMessages", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterReadsKitMessages::RunTest(const FString& Parameters)
{
	auto Header = [](const TArray<uint8>& Bytes, FCrowdyExecKitHeader& Out)
	{
		FString Error;
		return CrowdyExec::ReadKitHeader(Bytes, Out, Error);
	};
	FCrowdyExecKitHeader Read;
	TestTrue(TEXT("an event reads"), Header(KitMessage(7, 3, "event", {}), Read));
	TestEqual(TEXT("with its epoch"), Read.Epoch, 7ull);
	TestEqual(TEXT("its seq"), Read.Seq, 3ull);
	TestTrue(TEXT("and its kind"), Read.Kind == ECrowdyExecKitKind::Event);
	TestTrue(TEXT("a delta reads as one"), Header(KitMessage(7, 4, "delta", {}), Read) && Read.Kind == ECrowdyExecKitKind::Delta);

	TArray<uint8> Unknown = KitMessage(7, 3, "patch", {});
	TestFalse(TEXT("a kind this client does not read is refused"), Header(Unknown, Read));

	TArray<uint8> Version;
	FCrowdyExecWriter Writer(Version);
	Writer.MapHeader(4);
	WriteKey(Writer, "v");
	Writer.UInt(2);
	WriteKey(Writer, "epoch");
	Writer.UInt(7);
	WriteKey(Writer, "seq");
	Writer.UInt(3);
	WriteKey(Writer, "kind");
	WriteText(Writer, "snapshot");
	TestFalse(TEXT("another version is refused"), Header(Version, Read));

	TArray<uint8> NoKind;
	FCrowdyExecWriter NoKindWriter(NoKind);
	NoKindWriter.MapHeader(3);
	WriteKey(NoKindWriter, "v");
	NoKindWriter.UInt(1);
	WriteKey(NoKindWriter, "epoch");
	NoKindWriter.UInt(7);
	WriteKey(NoKindWriter, "seq");
	NoKindWriter.UInt(3);
	TestFalse(TEXT("a message without its kind is no kit message"), Header(NoKind, Read));

	TArray<uint8> Twice = KitMessage(7, 3, "event", {}, 1);
	FCrowdyExecWriter TwiceWriter(Twice);
	WriteKey(TwiceWriter, "seq");
	TwiceWriter.UInt(9);
	TestFalse(TEXT("a reserved key sent twice is refused"), Header(Twice, Read));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterSnapshotIsTheVariables,
	"CrowdySDK.CrowdyExec.GameKitStarter.SnapshotIsTheVariables", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterSnapshotIsTheVariables::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	if (!Rig.IsValid() || !MakeKitType(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	FSentFrame Subscribe;
	FSentFrame Read;
	if (!Object || !Rig.Open() || !TestTrue(TEXT("the starter subscribes to its State Topic"), Rig.WaitForFrame(KindSubscribe, Topic, Subscribe))
		|| !TestTrue(TEXT("the starter reads with snapshot"), Rig.WaitForFrame(KindCall, TEXT("snapshot"), Read)))
	{
		return false;
	}
	TestTrue(TEXT("addressed to its hub"), Rig.IsAddressed(Read));
	TestTrue(TEXT("asking for its State Topic's snapshot"), Read.Payload == TArray<uint8>({0x81, 0xa5, 't', 'o', 'p', 'i', 'c', 0xa4, 'b', 'o', 's', 's'}));
	TestEqual(TEXT("never the Server Object state topic"), Rig.FindFrame(KindSubscribe, TEXT("state")), INDEX_NONE);
	TestEqual(TEXT("nor its read"), Rig.FindFrame(KindCall, TEXT("read")), INDEX_NONE);

	// A key the State lacks is skipped, and a field the snapshot lacks keeps its default.
	TArray<uint8> Weather;
	FCrowdyExecWriter Writer(Weather);
	WriteKey(Writer, "weather");
	WriteText(Writer, "rain");
	const TArray<FString> Carried = {TEXT("health"), TEXT("defeated"), TEXT("phase")};
	FCrowdyExecTestBossState Values = Boss(1234, true, ECrowdyExecTestPhase::Enraged);
	Values.Secret = 99;
	Rig.Reply(Subscribe.Rid, ECrowdyNativeExecStatus::Ok, {});
	Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::Ok, KitMessage(Epoch, 4, "snapshot", Rig.Encode(Values, Carried), 1, Weather));
	if (!TestTrue(TEXT("the snapshot makes it Ready"), Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; })))
	{
		return false;
	}
	const FCrowdyExecTestBossState* State = StateOf(Object);
	if (!TestNotNull(TEXT("the State struct is held"), State))
	{
		return false;
	}
	TestEqual(TEXT("health is read into Health"), State->Health, 1234);
	TestTrue(TEXT("defeated into bDefeated"), State->bDefeated);
	TestTrue(TEXT("phase into Phase"), State->Phase == ECrowdyExecTestPhase::Enraged);
	TestEqual(TEXT("a field the snapshot left out keeps its default"), State->Secret, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterPushedSnapshotsReplaceTheValues,
	"CrowdySDK.CrowdyExec.GameKitStarter.PushedSnapshotsReplaceTheValues", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterPushedSnapshotsReplaceTheValues::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	if (!Rig.IsValid() || !MakeKitType(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquireKitReady(Rig, Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5);
	if (!Object)
	{
		return false;
	}
	FValuesSpy Spy;
	Spy.Watch(Object);
	const int32 Before = Spy.Num();

	Rig.PushTopic(Topic, KitMessage(Epoch, 6, "snapshot", Rig.Encode(Boss(2500, false, ECrowdyExecTestPhase::Calm))));
	TestTrue(TEXT("the next snapshot is applied"), Rig.PumpUntil([Object]() { return HealthOf(Object) == 2500; }));
	TestTrue(TEXT("naming only what changed"), Spy.Num() == Before + 1 && NamesAre(Spy.Last(), {TEXT("health")}));

	Rig.PushTopic(Topic, KitMessage(Epoch, 6, "snapshot", Rig.Encode(Boss(1, false, ECrowdyExecTestPhase::Calm))));
	Rig.PumpFor(0.15);
	TestEqual(TEXT("one at a seq already applied is stale"), HealthOf(Object), 2500);

	Rig.PushTopic(Topic, KitMessage(Epoch, 9, "snapshot", Rig.Encode(Boss(2000, false, ECrowdyExecTestPhase::Final))));
	TestTrue(TEXT("a whole snapshot past a gap is applied as it is"), Rig.PumpUntil([Object]() { return HealthOf(Object) == 2000; }));
	TestFalse(TEXT("without reading again"), Rig.SentAny(KindCall, TEXT("snapshot")));

	Rig.PushTopic(Topic, KitMessage(Epoch + 1, 1, "snapshot", Rig.Encode(Boss(4000, false, ECrowdyExecTestPhase::Calm))));
	TestTrue(TEXT("a new epoch's first snapshot is taken, its seq starting again"), Rig.PumpUntil([Object]() { return HealthOf(Object) == 4000; }));

	Rig.PushTopic(Topic, KitMessage(Epoch + 1, 2, "delta", {}));
	TestTrue(TEXT("a delta on the State Topic reads the whole snapshot again"), Rig.SentAny(KindCall, TEXT("snapshot")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterCallsSendOnceAndReadRefusalCodes,
	"CrowdySDK.CrowdyExec.GameKitStarter.CallsSendOnceAndReadRefusalCodes", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterCallsSendOnceAndReadRefusalCodes::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	if (!Rig.IsValid() || !MakeKitType(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquireKitReady(Rig, Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5);
	if (!Object)
	{
		return false;
	}

	FSentFrame Frame;
	TOptional<FCrowdyServerCallResult> Refused = CallAnswered(Rig, *Object, AttackFunction, AttackParams(25), ECrowdyNativeExecStatus::AppError,
		Utf8Bytes(TEXT("kit:full: the fight is full")), AttackMethod, &Frame);
	if (!Refused.IsSet())
	{
		return false;
	}
	TestTrue(TEXT("a Send Once function carries an action id"), Contains(Frame.Payload, "action_id"));
	TestTrue(TEXT("and its inputs by their snake case names"), Contains(Frame.Payload, "damage") && Contains(Frame.Payload, "weapon"));
	TestTrue(TEXT("a hub's coded refusal is Refused"), Refused->Outcome == ECrowdyServerCallOutcome::Refused);
	TestEqual(TEXT("with its code"), Refused->Code, FName(TEXT("full")));
	TestEqual(TEXT("and its words"), Refused->Reason, FString(TEXT("the fight is full")));
	TestFalse(TEXT("which is not retryable"), Refused->bRetryable);

	TOptional<FCrowdyServerCallResult> Cooldown = CallAnswered(Rig, *Object, AttackFunction, AttackParams(25), ECrowdyNativeExecStatus::AppError,
		Utf8Bytes(TEXT("kit:cooldown: wait a moment")), AttackMethod);
	TestTrue(TEXT("cooldown is Denied, as the outcome a call already has"), Cooldown.IsSet() && Cooldown->Outcome == ECrowdyServerCallOutcome::Denied);
	TestTrue(TEXT("retryable, and still coded"), Cooldown.IsSet() && Cooldown->bRetryable && Cooldown->Code == FName(TEXT("cooldown")));

	TStrongObjectPtr<UCrowdyServerObjectTestListener> Listener(NewObject<UCrowdyServerObjectTestListener>(GetTransientPackage(), NAME_None, RF_Transient));
	UCrowdyServerCallAction* Action = UCrowdyServerCallAction::CallServerFunction(nullptr, Object, AttackFunction, AttackParams(25));
	if (!TestNotNull(TEXT("the Blueprint call starts"), Action))
	{
		return false;
	}
	Action->OnFailed.AddDynamic(Listener.Get(), &UCrowdyServerObjectTestListener::HandleFailed);
	Action->Activate();
	FSentFrame BlueprintFrame;
	if (TestTrue(TEXT("the Blueprint call is sent"), Rig.WaitForFrame(KindCall, AttackMethod, BlueprintFrame)))
	{
		Rig.Reply(BlueprintFrame.Rid, ECrowdyNativeExecStatus::AppError, Utf8Bytes(TEXT("kit:not_your_turn: wait for it")));
		TestTrue(TEXT("Call Server Function fails"), Rig.PumpUntil([&Listener]() { return Listener->FailedCodes.Num() > 0; }));
		TestTrue(TEXT("with the code on its Code pin"), Listener->FailedCodes.Num() == 1 && Listener->FailedCodes[0] == FName(TEXT("not_your_turn")));
	}

	TOptional<FCrowdyServerCallResult> Forbidden = CallAnswered(Rig, *Object, AttackFunction, AttackParams(25), ECrowdyNativeExecStatus::AppError,
		Utf8Bytes(TEXT("kit:forbidden: not yours")), AttackMethod);
	TestTrue(TEXT("forbidden is Denied and not retryable"), Forbidden.IsSet() && Forbidden->Outcome == ECrowdyServerCallOutcome::Denied && !Forbidden->bRetryable);

	FCrowdyServerObjectTestAttackReply Answer;
	Answer.HealthLeft = 2975;
	TOptional<FCrowdyServerCallResult> Success = CallAnswered(Rig, *Object, AttackFunction, AttackParams(25), ECrowdyNativeExecStatus::Ok, Rig.Encode(Answer), AttackMethod);
	const FCrowdyServerObjectTestAttackReply* Outputs = Success.IsSet() ? Success->Reply.GetPtr<FCrowdyServerObjectTestAttackReply>() : nullptr;
	TestTrue(TEXT("a reply decodes from health_left"), Outputs && Outputs->HealthLeft == 2975);
	TestTrue(TEXT("with no code"), Success.IsSet() && Success->Code.IsNone());

	FSentFrame Taunt;
	TOptional<FCrowdyServerCallResult> Plain = CallAnswered(Rig, *Object, TauntFunction, FInstancedStruct(), ECrowdyNativeExecStatus::Ok, {}, TEXT("taunt"), &Taunt);
	TestTrue(TEXT("a function not sent once carries no action id"), Plain.IsSet() && Taunt.Payload == TArray<uint8>({0x80}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterEventsAreMessages,
	"CrowdySDK.CrowdyExec.GameKitStarter.EventsAreMessages", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterEventsAreMessages::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	if (!Rig.IsValid() || !MakeKitType(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquireKitReady(Rig, Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5);
	if (!Object)
	{
		return false;
	}
	TSharedRef<TArray<TPair<FName, int32>>> Heard = MakeShared<TArray<TPair<FName, int32>>>();
	const auto Listener = [Heard](UCrowdyServerObject*, FName Message, const FInstancedStruct& Value)
	{
		const FCrowdyServerObjectTestMessagePayload* Got = Value.GetPtr<FCrowdyServerObjectTestMessagePayload>();
		Heard->Emplace(Message, Got ? Got->Amount : -1);
	};
	Object->WatchMessage(RoaredMessage, FOnCrowdyServerMessage::FDelegate::CreateLambda(Listener));
	Object->WatchMessage(HealedMessage, FOnCrowdyServerMessage::FDelegate::CreateLambda(Listener));
	if (!Rig.AckTopic(HealsTopic))
	{
		return false;
	}
	TestEqual(TEXT("an event on the State Topic needs no subscription of its own"), Rig.CountFrames(KindSubscribe, Topic), 0);

	Rig.PushTopic(Topic, KitMessage(Epoch, 6, "event", Rig.Encode(Payload(7, TEXT("roar")))));
	TestTrue(TEXT("an event on the State Topic is its message"), Rig.PumpUntil([Heard]() { return Heard->Num() > 0; }));
	Rig.PumpFor(0.1);
	TestTrue(TEXT("heard once, with its payload"), Heard->Num() == 1 && (*Heard)[0].Key == FName(RoaredMessage) && (*Heard)[0].Value == 7);
	TestEqual(TEXT("and not taken for the values"), HealthOf(Object), 3000);

	Rig.PushTopic(HealsTopic, KitMessage(Epoch, 1, "changed", Rig.Encode(Payload(5, TEXT("x")))));
	Rig.PushTopic(HealsTopic, KitMessage(Epoch, 2, "event", Rig.Encode(Payload(3, TEXT("potion")))));
	TestTrue(TEXT("an event on its own topic is heard"), Rig.PumpUntil([Heard]() { return Heard->Num() > 1; }));
	Rig.PumpFor(0.1);
	TestTrue(TEXT("and a changed message is no event"), Heard->Num() == 2 && (*Heard)[1].Key == FName(HealedMessage) && (*Heard)[1].Value == 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterKitClosesWithTheConnection,
	"CrowdySDK.CrowdyExec.GameKitStarter.KitClosesWithTheConnection", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterKitClosesWithTheConnection::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	if (!Rig.IsValid() || !MakeKitType(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquireKitReady(Rig, Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5);
	if (!Object || !TestNotNull(TEXT("the open connection carries a kit"), Rig.Subsystem->GetKit()))
	{
		return false;
	}
	FCallSpy Spy;
	Object->Call(AttackFunction, AttackParams(25), Spy.OnDone());
	FSentFrame Frame;
	if (!TestTrue(TEXT("the call is sent"), Rig.WaitForFrame(KindCall, AttackMethod, Frame)))
	{
		return false;
	}
	Rig.Subsystem->DropConnectionForTest();
	TestNull(TEXT("the kit closes with the connection"), Rig.Subsystem->GetKit());
	TestEqual(TEXT("nothing runs inside the close"), Spy.Num(), 0);
	Rig.Tick(0.1f);
	TestTrue(TEXT("the call it carried completes on the next tick"), Rig.PumpUntil([&Spy]() { return Spy.Num() > 0; }));
	const bool bClosed = Spy.Num() == 1 && (*Spy.Results)[0].Outcome == ECrowdyServerCallOutcome::Unavailable && (*Spy.Results)[0].bRetryable;
	TestTrue(TEXT("as Unavailable and retryable, the closed connection's answer"), bClosed);
	TestEqual(TEXT("saying the connection closed"), Spy.Num() == 1 ? (*Spy.Results)[0].Reason : FString(), FString(TEXT("The connection to the server was closed")));

	// The next connection gets a kit of its own, so the starter is watched again on it.
	Rig.Sent.Reset();
	Rig.Tick(3.f);
	if (!TestTrue(TEXT("the subsystem dials again"), Rig.PumpUntil([&Rig]() { return DialsOf(Rig) >= 2; })))
	{
		return false;
	}
	Rig.Client->TestExecOpen();
	TestTrue(TEXT("and connects"), Rig.PumpUntil([&Rig]() { return Rig.Subsystem->GetConnection() != nullptr; }));
	FSentFrame Again;
	TestTrue(TEXT("the starter subscribes again on the new connection's kit"), Rig.WaitForFrame(KindSubscribe, Topic, Again));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterRefusedSubscribeFreesItsTopic,
	"CrowdySDK.CrowdyExec.GameKitStarter.RefusedSubscribeFreesItsTopic", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterRefusedSubscribeFreesItsTopic::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	if (!Rig.IsValid() || !MakeKitType(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquireKitReady(Rig, Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5);
	FCrowdyNativeKit* Kit = Rig.Subsystem->GetKit();
	if (!Object || !TestNotNull(TEXT("the connection carries a kit"), Kit))
	{
		return false;
	}
	const int32 Held = Kit->NumSubscriptions();
	Object->WatchMessage(HealedMessage, FOnCrowdyServerMessage::FDelegate::CreateLambda([](UCrowdyServerObject*, FName, const FInstancedStruct&) {}));
	FSentFrame Subscribe;
	if (!TestTrue(TEXT("the message topic is subscribed"), Rig.WaitForFrame(KindSubscribe, HealsTopic, Subscribe)))
	{
		return false;
	}
	TestEqual(TEXT("which the kit counts while it waits"), Kit->NumSubscriptions(), Held + 1);
	Rig.Reply(Subscribe.Rid, ECrowdyNativeExecStatus::NotFound, Utf8Bytes(TEXT("no such topic")));
	TestTrue(TEXT("a refused subscribe stops counting toward the kit's limit"), Rig.PumpUntil([Kit, Held]() { return Kit->NumSubscriptions() == Held; }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterSnapshotsDuringAReadReadOnce,
	"CrowdySDK.CrowdyExec.GameKitStarter.SnapshotsDuringAReadReadOnce", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterSnapshotsDuringAReadReadOnce::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	if (!Rig.IsValid() || !MakeKitType(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	FSentFrame Subscribe;
	FSentFrame Read;
	if (!Object || !Rig.Open() || !TestTrue(TEXT("the starter subscribes"), Rig.WaitForFrame(KindSubscribe, Topic, Subscribe))
		|| !TestTrue(TEXT("and reads"), Rig.WaitForFrame(KindCall, TEXT("snapshot"), Read)))
	{
		return false;
	}
	Rig.Reply(Subscribe.Rid, ECrowdyNativeExecStatus::Ok, {});
	// More whole snapshots than a Server Object holds pushes for while its read is answered.
	for (uint64 Seq = 6; Seq < 46; ++Seq)
	{
		Rig.PushTopic(Topic, KitMessage(Epoch, Seq, "snapshot", Rig.Encode(Boss(static_cast<int32>(Seq), false, ECrowdyExecTestPhase::Calm))));
	}
	Rig.PumpFor(0.1);
	Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::Ok, KitMessage(Epoch, 5, "snapshot", Rig.Encode(Boss(5, false, ECrowdyExecTestPhase::Calm))));
	TestTrue(TEXT("the newest snapshot held is applied over the read"), Rig.PumpUntil([Object]() { return HealthOf(Object) == 45; }));
	TestFalse(TEXT("with no second read"), Rig.SentAny(KindCall, TEXT("snapshot")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterBusyStaysRetryable,
	"CrowdySDK.CrowdyExec.GameKitStarter.BusyStaysRetryable", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterBusyStaysRetryable::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	if (!Rig.IsValid() || !MakeKitType(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquireKitReady(Rig, Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5);
	if (!Object)
	{
		return false;
	}
	FCallSpy Spy;
	Object->Call(AttackFunction, AttackParams(25), Spy.OnDone());
	// The kit sends a Busy call again itself, so every send is answered Busy until the call gives up.
	int32 Sends = 0;
	while (Spy.Num() == 0 && Sends < 6)
	{
		FSentFrame Frame;
		if (!Rig.WaitForFrame(KindCall, AttackMethod, Frame, 5.0))
		{
			break;
		}
		++Sends;
		Rig.Reply(Frame.Rid, ECrowdyNativeExecStatus::Busy, Utf8Bytes(TEXT("busy")));
		Rig.PumpFor(0.05);
	}
	if (!TestTrue(TEXT("the call completes"), Rig.PumpUntil([&Spy]() { return Spy.Num() > 0; })))
	{
		return false;
	}
	TestTrue(TEXT("as Busy"), (*Spy.Results)[0].Outcome == ECrowdyServerCallOutcome::Busy);
	TestTrue(TEXT("after the kit's own few sends, which the object does not repeat"), Sends <= 3);
	TestTrue(TEXT("which calling again later may fix"), (*Spy.Results)[0].bRetryable);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterWithoutStateTopicReadsNothing,
	"CrowdySDK.CrowdyExec.GameKitStarter.WithoutStateTopicReadsNothing", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterWithoutStateTopicReadsNothing::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	if (!Rig.IsValid() || !MakeKitType(Rig))
	{
		return false;
	}
	Rig.Definition->KitStateTopic.Reset();
	TArray<FString> Errors;
	if (!TestTrue(TEXT("a starter with no State Topic bakes"), Rig.Definition->Bake(Errors)))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	if (!Object || !Rig.Open())
	{
		return false;
	}
	TestTrue(TEXT("it is Ready once connected, with nothing to watch"), Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; }));
	TestFalse(TEXT("having read nothing"), Rig.SentAny(KindCall, TEXT("snapshot")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterOwnTypesKeepTheirRefusals,
	"CrowdySDK.CrowdyExec.GameKitStarter.OwnTypesKeepTheirRefusals", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterOwnTypesKeepTheirRefusals::RunTest(const FString& Parameters)
{
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
	FCrowdyNativeExecReply Reply;
	Reply.Status = ECrowdyNativeExecStatus::AppError;
	Reply.Message = TEXT("kit:full: words of an own type");
	const FCrowdyServerCallResult Result = Object->MakeCallResultForTest(AttackFunction, Reply);
	TestTrue(TEXT("an own type's refusal stays a Server Error, whatever its words"), Result.Outcome == ECrowdyServerCallOutcome::ServerError);
	TestTrue(TEXT("with no code"), Result.Code.IsNone());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterDecodesTheStartersValues,
	"CrowdySDK.CrowdyExec.GameKitStarter.DecodesTheStartersValues", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterDecodesTheStartersValues::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Kit(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
	Kit->TypeName = TEXT("session");
	Kit->CodeSource = ECrowdyServerCodeSource::GameKitStarter;
	Kit->KitStateTopic = TEXT("session");
	Kit->State = FCrowdyKitSessionState::StaticStruct();
	FCrowdyServerFunction& Ready = Kit->Functions.AddDefaulted_GetRef();
	Ready.Name = TEXT("Ready");
	Ready.Reply = FCrowdyKitSessionReadyReply::StaticStruct();
	TArray<FString> Errors;
	if (!TestTrue(FString::Printf(TEXT("the starter bakes (%s)"), *FString::Join(Errors, TEXT("; "))), Kit->Bake(Errors)))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Own(DuplicateObject(Kit.Get(), GetTransientPackage()));
	Own->CodeSource = ECrowdyServerCodeSource::Generated;
	Own->WatchedFields = {TEXT("Phase")};
	if (!TestTrue(TEXT("its own-type twin bakes"), Own->Bake(Errors)))
	{
		return false;
	}

	const FCrowdyExecBakedEnum* Phase = Kit->BakedEnums.FindByPredicate([](const FCrowdyExecBakedEnum& Enum) { return Enum.Enum == StaticEnum<ECrowdyKitSessionPhase>(); });
	TestTrue(TEXT("a starter's enum values take the hub's snake case names"), Phase && Phase->ServerNames == TArray<FString>({TEXT("lobby"), TEXT("playing"), TEXT("finished")}));
	const FCrowdyExecBakedEnum* OwnPhase = Own->BakedEnums.FindByPredicate([](const FCrowdyExecBakedEnum& Enum) { return Enum.Enum == StaticEnum<ECrowdyKitSessionPhase>(); });
	TestTrue(TEXT("an own type's keep their Unreal names"), OwnPhase && OwnPhase->ServerNames == TArray<FString>({TEXT("Lobby"), TEXT("Playing"), TEXT("Finished")}));

	TArray<uint8> Bytes;
	FCrowdyExecWriter Writer(Bytes);
	Writer.MapHeader(4);
	WriteKey(Writer, "phase");
	WriteText(Writer, "playing");
	WriteKey(Writer, "host");
	Writer.Nil();
	WriteKey(Writer, "round");
	Writer.UInt(3);
	WriteKey(Writer, "scores");
	Writer.MapHeader(1);
	Writer.UInt(7);
	Writer.Int(5);
	FCrowdyKitSessionState Read;
	FString Error;
	if (!TestTrue(FString::Printf(TEXT("a session decodes (%s)"), *Error), CrowdyExec::Decode(*Kit, *FCrowdyKitSessionState::StaticStruct(), Bytes, &Read, Error)))
	{
		return false;
	}
	TestTrue(TEXT("its phase by name"), Read.Phase == ECrowdyKitSessionPhase::Playing);
	TestEqual(TEXT("a nil host keeps the default"), Read.Host, int64(0));
	TestEqual(TEXT("the fields after it still read"), Read.Round, 3);
	TestEqual(TEXT("scores keyed by user id"), Read.Scores.FindRef(7), int64(5));

	TArray<uint8> NilOnly;
	FCrowdyExecWriter NilWriter(NilOnly);
	NilWriter.MapHeader(1);
	WriteKey(NilWriter, "Host");
	NilWriter.Nil();
	FCrowdyKitSessionState OwnRead;
	TestFalse(TEXT("an own type still refuses nil for a field that is not optional"), CrowdyExec::Decode(*Own, *FCrowdyKitSessionState::StaticStruct(), NilOnly, &OwnRead, Error));

	const TArray<uint8> Bare = {0xc3};
	FCrowdyKitSessionReadyReply Reply;
	TestTrue(TEXT("a hub's bare answer fills a reply of one field"), CrowdyExec::Decode(*Kit, *FCrowdyKitSessionReadyReply::StaticStruct(), Bare, &Reply, Error) && Reply.bReady);
	FCrowdyKitSessionReadyReply OwnReply;
	TestFalse(TEXT("an own type's reply is a map"), CrowdyExec::Decode(*Own, *FCrowdyKitSessionReadyReply::StaticStruct(), Bare, &OwnReply, Error));

	const TArray<uint8> Nil = {0xc0};
	FCrowdyKitSessionState NilRead;
	NilRead.Round = 9;
	TestTrue(TEXT("a hub's nil answer, such as a leaderboard place for a player not on it, leaves a reply of many fields at its defaults"),
		CrowdyExec::Decode(*Kit, *FCrowdyKitSessionState::StaticStruct(), Nil, &NilRead, Error) && NilRead.Round == 0);
	FCrowdyKitSessionState OwnNilRead;
	TestFalse(TEXT("an own type's nil answer is refused"), CrowdyExec::Decode(*Own, *FCrowdyKitSessionState::StaticStruct(), Nil, &OwnNilRead, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterDecodesRustTuples,
	"CrowdySDK.CrowdyExec.GameKitStarter.DecodesRustTuples", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterDecodesRustTuples::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Kit(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
	Kit->TypeName = TEXT("market");
	Kit->CodeSource = ECrowdyServerCodeSource::GameKitStarter;
	Kit->KitStateTopic = TEXT("market");
	Kit->State = FCrowdyKitMarketState::StaticStruct();
	FCrowdyServerFunction& Book = Kit->Functions.AddDefaulted_GetRef();
	Book.Name = TEXT("GetBook");
	Book.Reply = FCrowdyKitBook::StaticStruct();
	FCrowdyServerFunction& Progress = Kit->Functions.AddDefaulted_GetRef();
	Progress.Name = TEXT("GetProgress");
	Progress.Reply = FCrowdyKitProgress::StaticStruct();
	TArray<FString> Errors;
	if (!TestTrue(FString::Printf(TEXT("the starter bakes (%s)"), *FString::Join(Errors, TEXT("; "))), Kit->Bake(Errors)))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Own(DuplicateObject(Kit.Get(), GetTransientPackage()));
	Own->CodeSource = ECrowdyServerCodeSource::Generated;
	Own->WatchedFields = {TEXT("Currency")};
	if (!TestTrue(TEXT("its own-type twin bakes"), Own->Bake(Errors)))
	{
		return false;
	}

	TArray<uint8> Bytes;
	FCrowdyExecWriter Writer(Bytes);
	Writer.MapHeader(3);
	WriteKey(Writer, "item");
	WriteText(Writer, "wood");
	WriteKey(Writer, "bids");
	Writer.ArrayHeader(2);
	Writer.ArrayHeader(2);
	Writer.Int(12);
	Writer.UInt(3);
	Writer.ArrayHeader(2);
	Writer.Int(10);
	Writer.UInt(1);
	WriteKey(Writer, "last");
	Writer.Nil();
	FCrowdyKitBook Read;
	FString Error;
	if (!TestTrue(FString::Printf(TEXT("a book decodes (%s)"), *Error), CrowdyExec::Decode(*Kit, *FCrowdyKitBook::StaticStruct(), Bytes, &Read, Error)))
	{
		return false;
	}
	TestTrue(TEXT("each (price, count) pair fills a price level in field order"),
		Read.Bids.Num() == 2 && Read.Bids[0].Price == 12 && Read.Bids[0].Count == 3 && Read.Bids[1].Price == 10 && Read.Bids[1].Count == 1);
	TestEqualSensitive(TEXT("the fields around the pairs still read"), Read.Item, FString(TEXT("wood")));

	TArray<uint8> Short;
	FCrowdyExecWriter ShortWriter(Short);
	ShortWriter.MapHeader(1);
	WriteKey(ShortWriter, "bids");
	ShortWriter.ArrayHeader(1);
	ShortWriter.ArrayHeader(1);
	ShortWriter.Int(12);
	FCrowdyKitBook ShortRead;
	TestFalse(TEXT("a tuple of another length than the struct's fields is refused"), CrowdyExec::Decode(*Kit, *FCrowdyKitBook::StaticStruct(), Short, &ShortRead, Error));
	TestTrue(FString::Printf(TEXT("saying how many it got (%s)"), *Error), Error.Contains(TEXT("1 values for")));

	TArray<uint8> OwnBytes;
	FCrowdyExecWriter OwnWriter(OwnBytes);
	OwnWriter.MapHeader(1);
	WriteKey(OwnWriter, "Bids");
	OwnWriter.ArrayHeader(1);
	OwnWriter.ArrayHeader(2);
	OwnWriter.Int(12);
	OwnWriter.UInt(3);
	FCrowdyKitBook OwnRead;
	TestFalse(TEXT("an own type's struct is a map, never an array"), CrowdyExec::Decode(*Own, *FCrowdyKitBook::StaticStruct(), OwnBytes, &OwnRead, Error));

	TArray<uint8> Steep;
	FCrowdyExecWriter SteepWriter(Steep);
	SteepWriter.MapHeader(1);
	WriteKey(SteepWriter, "next");
	SteepWriter.UInt(MAX_uint64);
	FCrowdyKitProgress SteepRead;
	TestTrue(FString::Printf(TEXT("a hub's u64 past the int64 maximum reads as that maximum (%s)"), *Error),
		CrowdyExec::Decode(*Kit, *FCrowdyKitProgress::StaticStruct(), Steep, &SteepRead, Error) && SteepRead.Next == MAX_int64);
	TArray<uint8> OwnSteep;
	FCrowdyExecWriter OwnSteepWriter(OwnSteep);
	OwnSteepWriter.MapHeader(1);
	WriteKey(OwnSteepWriter, "Next");
	OwnSteepWriter.UInt(MAX_uint64);
	FCrowdyKitProgress OwnSteepRead;
	TestFalse(TEXT("an own type's int64 still refuses it"), CrowdyExec::Decode(*Own, *FCrowdyKitProgress::StaticStruct(), OwnSteep, &OwnSteepRead, Error));

	TArray<uint8> View;
	FCrowdyExecWriter ViewWriter(View);
	ViewWriter.MapHeader(2);
	WriteKey(ViewWriter, "quotes");
	ViewWriter.MapHeader(1);
	WriteText(ViewWriter, "wood");
	ViewWriter.MapHeader(2);
	WriteKey(ViewWriter, "bid");
	ViewWriter.Int(12);
	WriteKey(ViewWriter, "ask");
	ViewWriter.Nil();
	WriteKey(ViewWriter, "vendors");
	ViewWriter.ArrayHeader(1);
	ViewWriter.MapHeader(2);
	WriteKey(ViewWriter, "id");
	WriteText(ViewWriter, "smith");
	WriteKey(ViewWriter, "offers");
	ViewWriter.ArrayHeader(1);
	ViewWriter.MapHeader(2);
	WriteKey(ViewWriter, "stock");
	ViewWriter.Nil();
	WriteKey(ViewWriter, "per_player");
	ViewWriter.UInt(1);
	FCrowdyKitMarketState State;
	if (!TestTrue(FString::Printf(TEXT("a market view decodes (%s)"), *Error), CrowdyExec::Decode(*Kit, *FCrowdyKitMarketState::StaticStruct(), View, &State, Error)))
	{
		return false;
	}
	const FCrowdyKitQuote Wood = State.Quotes.FindRef(TEXT("wood"));
	TestTrue(TEXT("quotes keyed by item, an empty side 0"), Wood.Bid == 12 && Wood.Ask == 0);
	TestTrue(TEXT("an offer with no stock limit reads -1, and its limit per player as sent"),
		State.Vendors.Num() == 1 && State.Vendors[0].Offers.Num() == 1 && State.Vendors[0].Offers[0].Stock == -1 && State.Vendors[0].Offers[0].PerPlayer == 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterCodedSnapshotRefusalsAreFinal,
	"CrowdySDK.CrowdyExec.GameKitStarter.CodedSnapshotRefusalsAreFinal", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterCodedSnapshotRefusalsAreFinal::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	if (!Rig.IsValid() || !MakeKitType(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Forbidden = Rig.Acquire(Rig.InstanceId, Owner.Get());
	FSentFrame Subscribe;
	FSentFrame Read;
	if (!Forbidden || !Rig.Open() || !Rig.WaitForFrame(KindSubscribe, Topic, Subscribe) || !Rig.WaitForFrame(KindCall, TEXT("snapshot"), Read))
	{
		return false;
	}
	Rig.Reply(Subscribe.Rid, ECrowdyNativeExecStatus::Ok, {});
	Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::AppError, Utf8Bytes(TEXT("kit:forbidden: not for you")));
	TestTrue(TEXT("a coded refusal of the snapshot fails the starter"), Rig.PumpUntil([Forbidden]() { return Forbidden->GetStatus() == ECrowdyServerObjectStatus::Failed; }));
	TestTrue(TEXT("naming the code and the hub's words"), Forbidden->GetFailureReason().Contains(TEXT("(forbidden)")) && Forbidden->GetFailureReason().Contains(TEXT("not for you")));

	Rig.Sent.Reset();
	UCrowdyServerObject* Cooling = Rig.Acquire(TEXT("boss-2"), Owner.Get());
	FSentFrame SecondRead;
	if (!Cooling || !Rig.WaitForFrame(KindCall, TEXT("snapshot"), SecondRead))
	{
		return false;
	}
	Rig.Reply(SecondRead.Rid, ECrowdyNativeExecStatus::AppError, Utf8Bytes(TEXT("kit:cooldown: soon")));
	Rig.PumpFor(0.3);
	TestTrue(TEXT("cooldown says asking again may help, so it waits"), Cooling->GetStatus() == ECrowdyServerObjectStatus::Connecting);
	return true;
}

namespace CrowdyServerObjectKitTests
{
	/** Makes the rig's type the world starter on the test topic, and points the Game Kit World setting at it until Restore runs. */
	bool MakeKitWorld(FRig& Rig)
	{
		UCrowdyServerObjectDefinition& World = *Rig.Definition;
		World.TypeName = TEXT("world");
		World.CodeSource = ECrowdyServerCodeSource::GameKitStarter;
		World.KitStateTopic = Topic;
		World.State = FCrowdyKitWorldConditions::StaticStruct();
		World.WatchedFields.Reset();
		World.Functions.Reset();
		World.bOnlyOneInstance = true;
		TArray<FString> Errors;
		const bool bBaked = World.Bake(Errors);
		GetMutableDefault<UCrowdySDKDeveloperSettings>()->GameKitWorld = FSoftObjectPath(&World);
		return Rig.Test.TestTrue(FString::Printf(TEXT("the world starter bakes (%s)"), *FString::Join(Errors, TEXT("; "))), bBaked);
	}

	/** Acquires the world and answers its subscribe; returns the snapshot read to answer. */
	UCrowdyServerObject* AcquireKitWorld(FRig& Rig, const UObject* Owner, FSentFrame& OutRead)
	{
		UCrowdyServerObject* Object = Rig.Acquire(FString(), Owner);
		FSentFrame Subscribe;
		if (!Object || !Rig.Open() || !Rig.WaitForFrame(KindSubscribe, Topic, Subscribe) || !Rig.WaitForFrame(KindCall, TEXT("snapshot"), OutRead))
		{
			return nullptr;
		}
		Rig.Reply(Subscribe.Rid, ECrowdyNativeExecStatus::Ok, {});
		return Object;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterKitWorldHoldsPlayersOnline,
	"CrowdySDK.CrowdyExec.GameKitStarter.KitWorldHoldsPlayersOnline", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterKitWorldHoldsPlayersOnline::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	const FSoftObjectPath Saved = GetDefault<UCrowdySDKDeveloperSettings>()->GameKitWorld;
	ON_SCOPE_EXIT { GetMutableDefault<UCrowdySDKDeveloperSettings>()->GameKitWorld = Saved; };
	if (!Rig.IsValid() || !MakeKitWorld(Rig))
	{
		return false;
	}
	TestTrue(TEXT("the Game Kit World setting names the world starter"), UCrowdyServerObjectLibrary::GetGameKitWorldDefinition() == Rig.Definition.Get());
	TestNotNull(TEXT("whose Online is a variable Get Players Online can read"), Rig.Definition->FindVariable(GET_MEMBER_NAME_CHECKED(FCrowdyKitWorldConditions, Online)));

	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	FSentFrame Read;
	UCrowdyServerObject* Object = AcquireKitWorld(Rig, Owner.Get(), Read);
	if (!Object)
	{
		return false;
	}
	FCrowdyKitWorldConditions Conditions;
	Conditions.Online = 7;
	Conditions.bHasOnline = true;
	Conditions.Weather = ECrowdyKitWeather::Rain;
	Conditions.Modifiers.Add(TEXT("xp"), 2.0);
	Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::Ok, KitMessage(Epoch, 3, "snapshot", Rig.Encode(Conditions)));
	if (!TestTrue(TEXT("the world's snapshot makes it Ready"), Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; })))
	{
		return false;
	}
	const FCrowdyKitWorldConditions* Shown = Object->GetState().GetPtr<FCrowdyKitWorldConditions>();
	TestTrue(TEXT("its players online arrive on its topic"), Shown && Shown->Online == 7);
	TestTrue(TEXT("with the weather by name and the modifiers by key"), Shown && Shown->Weather == ECrowdyKitWeather::Rain && Shown->Modifiers.FindRef(TEXT("xp")) == 2.0);

	GetMutableDefault<UCrowdySDKDeveloperSettings>()->GameKitWorld = FSoftObjectPath(GetTransientPackage());
	TestNull(TEXT("a setting that names no starter's definition gives none"), UCrowdyServerObjectLibrary::GetGameKitWorldDefinition());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterWorldWithNoOnlineHasNoCount,
	"CrowdySDK.CrowdyExec.Kit.WorldWithNoOnlineHasNoCount", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterWorldWithNoOnlineHasNoCount::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	const FSoftObjectPath Saved = GetDefault<UCrowdySDKDeveloperSettings>()->GameKitWorld;
	ON_SCOPE_EXIT { GetMutableDefault<UCrowdySDKDeveloperSettings>()->GameKitWorld = Saved; };
	if (!Rig.IsValid() || !MakeKitWorld(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	FSentFrame Read;
	UCrowdyServerObject* Object = AcquireKitWorld(Rig, Owner.Get(), Read);
	if (!Object)
	{
		return false;
	}
	FCrowdyKitWorldConditions Conditions;
	Conditions.Online = 7;
	Conditions.bHasOnline = true;
	Conditions.TimeOfDay = 0.25;
	Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::Ok, KitMessage(Epoch, 1, "snapshot", Rig.Encode(Conditions)));
	if (!TestTrue(TEXT("the world's snapshot makes it Ready"), Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; })))
	{
		return false;
	}
	const UCrowdyServerObjectDefinition* World = Rig.Definition.Get();
	int32 Online = -1;
	bool bUnsent = true;
	TestTrue(TEXT("a world that sends its count has one"), UCrowdyServerObjectLibrary::ReadKitWorldOnline(Object, World, Online, bUnsent) && Online == 7 && !bUnsent);

	uint64 Seq = 1;
	auto Push = [this, &Rig, Object, &Seq](FCrowdyKitWorldConditions& Next, bool bLeaveOnlineOut)
	{
		Next.TimeOfDay = static_cast<double>(++Seq);
		const TArray<FString> WithoutOnline = {TEXT("time_of_day"), TEXT("night"), TEXT("weather"), TEXT("modifiers")};
		const TArray<uint8> Message = KitMessage(Epoch, Seq, "snapshot", bLeaveOnlineOut ? Rig.Encode(Next, WithoutOnline) : Rig.Encode(Next));
		// The world is its own type and key, not the rig's default instance.
		Rig.Client->TestExecReceiveBinary(TopicPushFrame(Object->GetInstanceId(), Topic, Message, TEXT("world")));
		const double Marker = Next.TimeOfDay;
		return TestTrue(FString::Printf(TEXT("snapshot %d arrives"), static_cast<int32>(Seq)),
			Rig.PumpUntil([Object, Marker]() { const FCrowdyKitWorldConditions* Now = Object->GetState().GetPtr<FCrowdyKitWorldConditions>(); return Now && Now->TimeOfDay == Marker; }));
	};

	Conditions.Online = 5;
	if (!Push(Conditions, true))
	{
		return false;
	}
	const FCrowdyKitWorldConditions* Shown = Object->GetState().GetPtr<FCrowdyKitWorldConditions>();
	TestTrue(TEXT("a snapshot without online leaves the world holding no count"), Shown && !Shown->bHasOnline);
	Online = -1;
	bUnsent = false;
	TestFalse(TEXT("so it has none to give, though it is Ready"), UCrowdyServerObjectLibrary::ReadKitWorldOnline(Object, World, Online, bUnsent));
	TestTrue(TEXT("with 0, and saying the world sent none"), Online == 0 && bUnsent);

	Conditions.Online = 7;
	if (!Push(Conditions, false))
	{
		return false;
	}
	bUnsent = true;
	TestTrue(TEXT("a snapshot carrying online gives the count"), UCrowdyServerObjectLibrary::ReadKitWorldOnline(Object, World, Online, bUnsent) && Online == 7 && !bUnsent);

	Conditions.bHasOnline = false;
	if (!Push(Conditions, false))
	{
		return false;
	}
	TestFalse(TEXT("online sent as nil is no count"), UCrowdyServerObjectLibrary::ReadKitWorldOnline(Object, World, Online, bUnsent));
	TestTrue(TEXT("with 0, and saying the world sent none"), Online == 0 && bUnsent);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterMissingKitWorldWaits,
	"CrowdySDK.CrowdyExec.GameKitStarter.MissingKitWorldWaits", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterMissingKitWorldWaits::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	const FSoftObjectPath Saved = GetDefault<UCrowdySDKDeveloperSettings>()->GameKitWorld;
	ON_SCOPE_EXIT { GetMutableDefault<UCrowdySDKDeveloperSettings>()->GameKitWorld = Saved; };
	if (!Rig.IsValid() || !MakeKitWorld(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	FSentFrame Read;
	UCrowdyServerObject* Object = AcquireKitWorld(Rig, Owner.Get(), Read);
	if (!Object)
	{
		return false;
	}
	Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::NotFound, Utf8Bytes(TEXT("no such node type")));
	TestTrue(TEXT("a missing world waits, saying so"), Rig.PumpUntil([Object]() { return Object->GetFailureReason().Contains(TEXT("no Game Kit world")); }));
	TestTrue(TEXT("still connecting"), Object->GetStatus() == ECrowdyServerObjectStatus::Connecting);
	Rig.Sent.Reset();
	Rig.PumpFor(2.0);
	TestFalse(TEXT("and does not ask again within seconds, as a missing root does not"), Rig.SentAny(KindCall, TEXT("snapshot")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterRolledBackKitWorldWaits,
	"CrowdySDK.CrowdyExec.GameKitStarter.RolledBackKitWorldWaits", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterRolledBackKitWorldWaits::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	const FSoftObjectPath Saved = GetDefault<UCrowdySDKDeveloperSettings>()->GameKitWorld;
	ON_SCOPE_EXIT { GetMutableDefault<UCrowdySDKDeveloperSettings>()->GameKitWorld = Saved; };
	if (!Rig.IsValid() || !MakeKitWorld(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	FSentFrame Read;
	UCrowdyServerObject* Object = AcquireKitWorld(Rig, Owner.Get(), Read);
	if (!Object)
	{
		return false;
	}
	// What the platform answers once the app runs a version without the world.
	Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::Denied, Utf8Bytes(TEXT("clients may not subscribe to that type")));
	TestTrue(TEXT("a world the active version lacks waits, saying the app has none"), Rig.PumpUntil([Object]() { return Object->GetFailureReason().Contains(TEXT("no Game Kit world")); }));
	TestTrue(TEXT("still connecting"), Object->GetStatus() == ECrowdyServerObjectStatus::Connecting);
	Rig.Sent.Reset();
	Rig.PumpFor(2.0);
	TestFalse(TEXT("and asks again only after the minute"), Rig.SentAny(KindCall, TEXT("snapshot")));
	return true;
}

namespace CrowdyServerObjectKitTests
{
	inline constexpr const TCHAR* NodesTopic = TEXT("nodes");

	/** Makes the rig's type a starter shaped like world, whose nodes topic is its collection of FCrowdyKitWorldNode records. */
	bool MakeKitNodes(FRig& Rig)
	{
		UCrowdyServerObjectDefinition& World = *Rig.Definition;
		World.CodeSource = ECrowdyServerCodeSource::GameKitStarter;
		World.KitStateTopic = Topic;
		World.State = FCrowdyKitWorldConditions::StaticStruct();
		World.KitCollectionTopic = NodesTopic;
		World.KitRecord = FCrowdyKitWorldNode::StaticStruct();
		World.WatchedFields.Reset();
		World.Functions.Reset();
		World.bOnlyOneInstance = true;
		FCrowdyServerMessage& Roared = World.Messages.AddDefaulted_GetRef();
		Roared.Name = RoaredMessage;
		Roared.ServerName = Topic;
		Roared.Payload = FCrowdyServerObjectTestMessagePayload::StaticStruct();
		// The world's one instance has the empty key, which the rig's pushes must name.
		Rig.InstanceId.Reset();
		TArray<FString> Errors;
		const bool bBaked = World.Bake(Errors);
		return Rig.Test.TestTrue(FString::Printf(TEXT("the world starter with a collection bakes (%s)"), *FString::Join(Errors, TEXT("; "))), bBaked);
	}

	/** Pumps until a snapshot call naming Wanted was sent, and takes it. */
	bool WaitForSnapshotOf(FRig& Rig, const ANSICHAR* Wanted, FSentFrame& Out, double Seconds = 3.0)
	{
		auto Find = [&Rig, Wanted]()
		{
			return Rig.Sent.IndexOfByPredicate([Wanted](const FSentFrame& Frame)
			{
				return Frame.Kind == KindCall && Frame.Name.Equals(TEXT("snapshot"), ESearchCase::CaseSensitive) && Contains(Frame.Payload, Wanted);
			});
		};
		if (!Rig.PumpUntil([&Find]() { return Find() != INDEX_NONE; }, Seconds))
		{
			return false;
		}
		const int32 Index = Find();
		Out = Rig.Sent[Index];
		Rig.Sent.RemoveAt(Index);
		return true;
	}

	/** A node's props as the world sends them: amount and regrow_at, not its id. */
	TArray<uint8> NodeProps(FRig& Rig, int32 Amount, int64 RegrowAt)
	{
		FCrowdyKitWorldNode Node;
		Node.Amount = Amount;
		Node.RegrowAt = RegrowAt;
		return Rig.Encode(Node, TArray<FString>{TEXT("amount"), TEXT("regrow_at")});
	}

	/** A member's keys: id, type_name and props, or id and removed when Props is empty. */
	TArray<uint8> MemberPairs(uint64 Id, const TArray<uint8>& Props, int32& OutPairs)
	{
		TArray<uint8> Bytes;
		FCrowdyExecWriter Writer(Bytes);
		WriteKey(Writer, "id");
		Writer.UInt(Id);
		if (Props.IsEmpty())
		{
			WriteKey(Writer, "removed");
			Writer.Bool(true);
			OutPairs = 2;
			return Bytes;
		}
		WriteKey(Writer, "type_name");
		WriteText(Writer, "Node");
		WriteKey(Writer, "props");
		Bytes.Append(Props);
		OutPairs = 3;
		return Bytes;
	}

	TArray<uint8> NodeDelta(uint64 Seq, uint64 Id, const TArray<uint8>& Props, uint64 MessageEpoch = Epoch)
	{
		int32 Pairs = 0;
		const TArray<uint8> Member = MemberPairs(Id, Props, Pairs);
		return KitMessage(MessageEpoch, Seq, "delta", {}, Pairs, Member);
	}

	TArray<uint8> NodesSnapshot(uint64 Seq, const TArray<TPair<uint64, TArray<uint8>>>& Nodes, uint64 MessageEpoch = Epoch)
	{
		TArray<uint8> Containers;
		FCrowdyExecWriter Writer(Containers);
		WriteKey(Writer, "containers");
		Writer.ArrayHeader(Nodes.Num());
		for (const TPair<uint64, TArray<uint8>>& Node : Nodes)
		{
			int32 Pairs = 0;
			const TArray<uint8> Member = MemberPairs(Node.Key, Node.Value, Pairs);
			Writer.MapHeader(Pairs);
			Containers.Append(Member);
		}
		return KitMessage(MessageEpoch, Seq, "snapshot", {}, 1, Containers);
	}

	/** Each records announcement: the changed ids with their changed variables' names, then the removed ids. */
	struct FNodesSpy
	{
		TSharedRef<TArray<TTuple<TArray<FString>, TArray<FString>, TArray<FString>>>> Events = MakeShared<TArray<TTuple<TArray<FString>, TArray<FString>, TArray<FString>>>>();

		void Listen(UCrowdyServerObject& Object)
		{
			Object.OnRecordsChanged.AddLambda([Out = Events](UCrowdyServerObject* Announcer, TConstArrayView<FString> Changed, TConstArrayView<FString> Removed,
				TConstArrayView<TBitArray<>> ChangedFields)
			{
				TArray<FString> ByField;
				Announcer->GetFieldVariableNames(ByField);
				TArray<FString> Names;
				for (const TBitArray<>& Fields : ChangedFields)
				{
					for (TConstSetBitIterator<> It(Fields); It; ++It)
					{
						Names.AddUnique(ByField.IsValidIndex(It.GetIndex()) ? ByField[It.GetIndex()] : FString());
					}
				}
				Out->Emplace(TArray<FString>(Changed), MoveTemp(Names), TArray<FString>(Removed));
			});
		}
	};

	const FCrowdyKitWorldNode* FindNode(const UCrowdyServerObject& Object, const TCHAR* Id)
	{
		const FCrowdyServerRecord* Record = Object.GetRecord(Id);
		return Record ? Record->Values.GetPtr<FCrowdyKitWorldNode>() : nullptr;
	}

	/** Acquires the world and answers both subscribes; returns the state's and the members' reads to answer. */
	UCrowdyServerObject* AcquireKitNodes(FRig& Rig, const UObject* Owner, FSentFrame& OutStateRead, FSentFrame& OutNodesRead)
	{
		UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner);
		if (!Object || !Rig.Open() || !Rig.AckTopic(Topic) || !Rig.AckTopic(NodesTopic)
			|| !Rig.Test.TestTrue(TEXT("the state is read"), WaitForSnapshotOf(Rig, "boss", OutStateRead))
			|| !Rig.Test.TestTrue(TEXT("the members are read with snapshot on their topic"), WaitForSnapshotOf(Rig, "nodes", OutNodesRead)))
		{
			return nullptr;
		}
		return Object;
	}

	void AnswerState(FRig& Rig, const FSentFrame& StateRead)
	{
		Rig.Reply(StateRead.Rid, ECrowdyNativeExecStatus::Ok, KitMessage(Epoch, 1, "snapshot", Rig.Encode(FCrowdyKitWorldConditions())));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterCollectionMembersAreRecords,
	"CrowdySDK.CrowdyExec.GameKitStarter.CollectionMembersAreRecords", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterCollectionMembersAreRecords::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	if (!Rig.IsValid() || !MakeKitNodes(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	FSentFrame StateRead;
	FSentFrame NodesRead;
	UCrowdyServerObject* Object = AcquireKitNodes(Rig, Owner.Get(), StateRead, NodesRead);
	if (!Object)
	{
		return false;
	}
	FNodesSpy Spy;
	Spy.Listen(*Object);
	AnswerState(Rig, StateRead);
	Rig.PumpFor(0.2);
	TestTrue(TEXT("the state alone does not make it Ready while its members are unread"), Object->GetStatus() == ECrowdyServerObjectStatus::Connecting);
	Rig.Reply(NodesRead.Rid, ECrowdyNativeExecStatus::Ok, NodesSnapshot(4, {{0, NodeProps(Rig, 5, 0)}, {1, NodeProps(Rig, 3, 0)}}));
	if (!TestTrue(TEXT("the members' read makes it Ready"), Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; })))
	{
		return false;
	}
	FInstancedStruct Values;
	int64 OwnerId = 0;
	FString Team;
	TestTrue(TEXT("a member is a record by its id"), Object->K2_FindRecord(TEXT("1"), Values, OwnerId, Team));
	const FCrowdyKitWorldNode* Read = Values.GetPtr<FCrowdyKitWorldNode>();
	TestTrue(TEXT("holding its props, and its id in Id"), Read && Read->Amount == 3 && Read->Id == 1);
	TestEqual(TEXT("one record per member"), Object->NumRecords(), 2);
	TestTrue(TEXT("Holds Records says so"), Object->HoldsRecords());
	TestTrue(TEXT("both are announced once"), Spy.Events->Num() == 1 && (*Spy.Events)[0].Get<0>().Num() == 2);

	Rig.PushTopic(NodesTopic, NodeDelta(5, 1, NodeProps(Rig, 2, 1700)));
	if (!TestTrue(TEXT("a delta is announced"), Rig.PumpUntil([&Spy]() { return Spy.Events->Num() == 2; })))
	{
		return false;
	}
	const TArray<FString> Changed = {TEXT("1")};
	const TArray<FString> Names = {TEXT("Amount"), TEXT("RegrowAt")};
	TestTrue(TEXT("changing its member alone"), (*Spy.Events)[1].Get<0>() == Changed);
	TestTrue(TEXT("naming the Record struct's changed variables"), (*Spy.Events)[1].Get<1>() == Names);
	const FCrowdyKitWorldNode* Updated = FindNode(*Object, TEXT("1"));
	TestTrue(TEXT("with the delta's props"), Updated && Updated->Amount == 2 && Updated->RegrowAt == 1700 && Updated->Id == 1);

	Rig.PushTopic(NodesTopic, NodeDelta(6, 0, {}));
	if (!TestTrue(TEXT("a removal is announced"), Rig.PumpUntil([&Spy]() { return Spy.Events->Num() == 3; })))
	{
		return false;
	}
	const TArray<FString> Removed = {TEXT("0")};
	TestTrue(TEXT("as a removed record"), (*Spy.Events)[2].Get<2>() == Removed && !Object->GetRecord(TEXT("0")));

	Rig.PushTopic(NodesTopic, NodeDelta(7, 9, NodeProps(Rig, 1, 0)));
	TestTrue(TEXT("a delta of a new member adds a record"), Rig.PumpUntil([Object]() { return FindNode(*Object, TEXT("9")) != nullptr; }));
	Rig.Sent.Reset();
	Rig.PushTopic(NodesTopic, NodeDelta(7, 9, NodeProps(Rig, 4, 0)));
	Rig.PumpFor(0.2);
	const FCrowdyKitWorldNode* Kept = FindNode(*Object, TEXT("9"));
	TestTrue(TEXT("a delta at a seq already applied is dropped"), Kept && Kept->Amount == 1 && Spy.Events->Num() == 4);
	FSentFrame Reread;
	TestFalse(TEXT("without reading the members again"), WaitForSnapshotOf(Rig, "nodes", Reread, 0.2));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterCollectionGapsReadAgain,
	"CrowdySDK.CrowdyExec.GameKitStarter.CollectionGapsReadAgain", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterCollectionGapsReadAgain::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	if (!Rig.IsValid() || !MakeKitNodes(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	FSentFrame StateRead;
	FSentFrame NodesRead;
	UCrowdyServerObject* Object = AcquireKitNodes(Rig, Owner.Get(), StateRead, NodesRead);
	if (!Object)
	{
		return false;
	}
	AnswerState(Rig, StateRead);
	Rig.Reply(NodesRead.Rid, ECrowdyNativeExecStatus::Ok, NodesSnapshot(4, {{0, NodeProps(Rig, 5, 0)}}));
	if (!TestTrue(TEXT("Ready"), Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; })))
	{
		return false;
	}
	Rig.Sent.Reset();
	Rig.PushTopic(NodesTopic, NodeDelta(6, 0, NodeProps(Rig, 1, 0)));
	FSentFrame Again;
	if (!TestTrue(TEXT("a skipped seq reads the members again"), WaitForSnapshotOf(Rig, "nodes", Again)))
	{
		return false;
	}
	const FCrowdyKitWorldNode* Node = FindNode(*Object, TEXT("0"));
	TestTrue(TEXT("and the delta past the gap is not applied"), Node && Node->Amount == 5);
	Rig.Reply(Again.Rid, ECrowdyNativeExecStatus::Ok, NodesSnapshot(8, {{0, NodeProps(Rig, 4, 0)}}));
	TestTrue(TEXT("the read replaces the members"), Rig.PumpUntil([Object]() { const FCrowdyKitWorldNode* Each = FindNode(*Object, TEXT("0")); return Each && Each->Amount == 4; }));

	Rig.PushTopic(NodesTopic, NodeDelta(1, 0, NodeProps(Rig, 3, 0), Epoch + 1));
	FSentFrame NewEpoch;
	if (!TestTrue(TEXT("a delta of a new epoch reads the members again"), WaitForSnapshotOf(Rig, "nodes", NewEpoch)))
	{
		return false;
	}
	Rig.Reply(NewEpoch.Rid, ECrowdyNativeExecStatus::Ok, NodesSnapshot(1, {{0, NodeProps(Rig, 2, 0)}, {5, NodeProps(Rig, 7, 0)}}, Epoch + 1));
	TestTrue(TEXT("whose read brings the new epoch's members"), Rig.PumpUntil([Object]() { return FindNode(*Object, TEXT("5")) != nullptr; }));

	Rig.PushTopic(NodesTopic, NodesSnapshot(3, {{0, NodeProps(Rig, 2, 0)}}, Epoch + 1));
	TestTrue(TEXT("a pushed snapshot replaces the members whole, removing those it leaves out"),
		Rig.PumpUntil([Object]() { return !Object->GetRecord(TEXT("5")) && Object->GetRecord(TEXT("0")); }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterCollectionReadFirstWaitsForTheState,
	"CrowdySDK.CrowdyExec.GameKitStarter.CollectionReadFirstWaitsForTheState", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterCollectionReadFirstWaitsForTheState::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	if (!Rig.IsValid() || !MakeKitNodes(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	FSentFrame StateRead;
	FSentFrame NodesRead;
	UCrowdyServerObject* Object = AcquireKitNodes(Rig, Owner.Get(), StateRead, NodesRead);
	if (!Object)
	{
		return false;
	}
	FNodesSpy Spy;
	Spy.Listen(*Object);
	Rig.Reply(NodesRead.Rid, ECrowdyNativeExecStatus::Ok, NodesSnapshot(4, {{0, NodeProps(Rig, 5, 0)}, {1, NodeProps(Rig, 3, 0)}}));
	Rig.PushTopic(NodesTopic, NodeDelta(5, 1, NodeProps(Rig, 2, 0)));
	Rig.PumpFor(0.2);
	TestTrue(TEXT("members read before the state leave it Connecting"), Object->GetStatus() == ECrowdyServerObjectStatus::Connecting);
	TestTrue(TEXT("and are not announced yet"), Spy.Events->IsEmpty());
	AnswerState(Rig, StateRead);
	if (!TestTrue(TEXT("the state's read makes it Ready"), Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; })))
	{
		return false;
	}
	TestTrue(TEXT("announcing every member held, once"), Spy.Events->Num() == 1 && (*Spy.Events)[0].Get<0>().Num() == 2);
	const FCrowdyKitWorldNode* Node = FindNode(*Object, TEXT("1"));
	TestTrue(TEXT("with the delta that came meanwhile applied"), Node && Node->Amount == 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterCollectionReadyBringsValuesAndMessages,
	"CrowdySDK.CrowdyExec.GameKitStarter.CollectionReadyBringsValuesAndMessages", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterCollectionReadyBringsValuesAndMessages::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	if (!Rig.IsValid() || !MakeKitNodes(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	FSentFrame StateRead;
	FSentFrame NodesRead;
	UCrowdyServerObject* Object = AcquireKitNodes(Rig, Owner.Get(), StateRead, NodesRead);
	if (!Object)
	{
		return false;
	}
	TSharedRef<int32> ValueCalls = MakeShared<int32>(0);
	TSharedRef<int32> Messages = MakeShared<int32>(0);
	Object->WatchValues(FOnCrowdyServerValuesChanged::FDelegate::CreateLambda([ValueCalls](UCrowdyServerObject*, TConstArrayView<FString>) { ++*ValueCalls; }));
	Object->WatchMessage(RoaredMessage, FOnCrowdyServerMessage::FDelegate::CreateLambda([Messages](UCrowdyServerObject*, FName, const FInstancedStruct&) { ++*Messages; }));
	AnswerState(Rig, StateRead);
	Rig.PumpFor(0.2);
	Rig.PushTopic(Topic, KitMessage(Epoch, 2, "event", Rig.Encode(Payload(5, TEXT("boss")))));
	Rig.PumpFor(0.2);
	TestTrue(TEXT("nothing is heard while the members are unread"), *ValueCalls == 0 && *Messages == 0);
	Rig.Reply(NodesRead.Rid, ECrowdyNativeExecStatus::Ok, NodesSnapshot(4, {{0, NodeProps(Rig, 5, 0)}}));
	if (!TestTrue(TEXT("the members' read makes it Ready"), Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; })))
	{
		return false;
	}
	TestEqual(TEXT("the values read before are announced once, as it becomes Ready"), *ValueCalls, 1);
	TestEqual(TEXT("and the message held meanwhile is delivered"), *Messages, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterCollectionFailureKeepsItsReason,
	"CrowdySDK.CrowdyExec.GameKitStarter.CollectionFailureKeepsItsReason", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterCollectionFailureKeepsItsReason::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	if (!Rig.IsValid() || !MakeKitNodes(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	FSentFrame StateRead;
	FSentFrame NodesRead;
	UCrowdyServerObject* Object = AcquireKitNodes(Rig, Owner.Get(), StateRead, NodesRead);
	if (!Object)
	{
		return false;
	}
	AnswerState(Rig, StateRead);
	Rig.Reply(NodesRead.Rid, ECrowdyNativeExecStatus::Internal, {});
	if (!TestTrue(TEXT("a refused members' read gives a reason"), Rig.PumpUntil([Object]() { return !Object->GetFailureReason().IsEmpty(); })))
	{
		return false;
	}
	Rig.Tick(3.f, 0.25f);
	FSentFrame StateAgain;
	if (!TestTrue(TEXT("the retry reads the state again"), WaitForSnapshotOf(Rig, "boss", StateAgain)))
	{
		return false;
	}
	AnswerState(Rig, StateAgain);
	Rig.PumpFor(0.2);
	TestFalse(TEXT("a good state read leaves the members' reason"), Object->GetFailureReason().IsEmpty());
	TestTrue(TEXT("and it stays Connecting"), Object->GetStatus() == ECrowdyServerObjectStatus::Connecting);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterCollectionRedialSubscribesAgain,
	"CrowdySDK.CrowdyExec.GameKitStarter.CollectionRedialSubscribesAgain", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterCollectionRedialSubscribesAgain::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	if (!Rig.IsValid() || !MakeKitNodes(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	FSentFrame StateRead;
	FSentFrame NodesRead;
	UCrowdyServerObject* Object = AcquireKitNodes(Rig, Owner.Get(), StateRead, NodesRead);
	if (!Object)
	{
		return false;
	}
	AnswerState(Rig, StateRead);
	Rig.Reply(NodesRead.Rid, ECrowdyNativeExecStatus::Ok, NodesSnapshot(4, {{0, NodeProps(Rig, 5, 0)}}));
	if (!TestTrue(TEXT("Ready"), Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; })))
	{
		return false;
	}
	// A change of app drops the connection and dials a new one, with a new Game Kit whose handles start again.
	const int32 Dials = DialsOf(Rig);
	Rig.Session->SetAppID(43);
	Rig.Subsystem->TickForTest(0.1f);
	if (!TestTrue(TEXT("the new app is dialled"), Rig.PumpUntil([&Rig, Dials]() { return DialsOf(Rig) > Dials; })))
	{
		return false;
	}
	Rig.Sent.Reset();
	Rig.Client->TestExecOpen();
	FSentFrame Again;
	TestTrue(TEXT("the new connection subscribes to the members again"), Rig.WaitForFrame(KindSubscribe, NodesTopic, Again, 5.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterCollectionChecks,
	"CrowdySDK.CrowdyExec.GameKitStarter.CollectionChecks", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterCollectionChecks::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	if (!Rig.IsValid() || !MakeKitNodes(Rig))
	{
		return false;
	}
	UCrowdyServerObjectDefinition& Definition = *Rig.Definition;
	TestNotNull(TEXT("the Record struct bakes with the starter's names"),
		CrowdyExec::FindResolvedPropertyForTest(Definition, *FCrowdyKitWorldNode::StaticStruct(), TEXT("regrow_at")));

	TArray<FString> Errors;
	Definition.KitRecord = nullptr;
	TestFalse(TEXT("a Collection Topic without a Record Struct is refused"), Definition.Bake(Errors));
	Definition.KitRecord = FCrowdyKitWorldNode::StaticStruct();
	Definition.KitCollectionTopic = Topic;
	TestFalse(TEXT("a Collection Topic that is the State Topic is refused"), Definition.Bake(Errors));
	Definition.KitCollectionTopic = NodesTopic;
	Definition.KitRecord = FCrowdyKitWorldConditions::StaticStruct();
	TestFalse(TEXT("a Record Struct that is the State is refused"), Definition.Bake(Errors));

	Definition.KitRecord = FCrowdyKitWorldNode::StaticStruct();
	Errors.Reset();
	if (!TestTrue(TEXT("restored, it bakes"), Definition.Bake(Errors)))
	{
		return false;
	}
	TArray<FCrowdyExecKitMember> Members;
	FCrowdyExecKitHeader Header;
	FString Error;
	TestTrue(TEXT("a snapshot decodes"), CrowdyExec::DecodeKitCollection(Definition, NodesSnapshot(2, {{1, NodeProps(Rig, 3, 0)}, {2, TArray<uint8>()}}), Header, Members, Error));
	TestTrue(TEXT("leaving out a member marked removed"), Members.Num() == 1 && Members[0].Id == TEXT("1") && Header.Seq == 2);
	TestTrue(TEXT("a removal decodes"), CrowdyExec::DecodeKitCollection(Definition, NodeDelta(3, 2, {}), Header, Members, Error) && Members.Num() == 1 && Members[0].bRemoved);

	TArray<uint8> NoId;
	FCrowdyExecWriter Writer(NoId);
	WriteKey(Writer, "props");
	NoId.Append(NodeProps(Rig, 1, 0));
	TestFalse(TEXT("a delta without an id is refused"), CrowdyExec::DecodeKitCollection(Definition, KitMessage(Epoch, 4, "delta", {}, 1, NoId), Header, Members, Error));
	TestFalse(TEXT("a snapshot without containers is refused"), CrowdyExec::DecodeKitCollection(Definition, KitMessage(Epoch, 5, "snapshot", {}), Header, Members, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterMemberTypeNameFillsTheRecord,
	"CrowdySDK.CrowdyExec.GameKitStarter.MemberTypeNameFillsTheRecord", CrowdyServerObjectTests::TestFlags)
bool FCrowdyGameKitStarterMemberTypeNameFillsTheRecord::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectKitTests;
	// An npcs area as Add starter makes it: no State, and records of three shapes told apart by type_name.
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Area(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage()));
	Area->TypeName = TEXT("npcs");
	Area->CodeSource = ECrowdyServerCodeSource::GameKitStarter;
	Area->StateForm = ECrowdyServerValuesForm::Struct;
	Area->State = FCrowdyKitAreaState::StaticStruct();
	Area->KitCollectionTopic = TEXT("area");
	Area->KitRecord = FCrowdyKitAreaMember::StaticStruct();
	UEnum* MemberType = StaticEnum<ECrowdyKitAreaMemberType>();
	Area->FieldNames = {{FCrowdyKitAreaMember::StaticStruct(), GET_MEMBER_NAME_CHECKED(FCrowdyKitAreaMember, Type), TEXT("type_name")}};
	Area->EnumValueNames = {{MemberType, TEXT("Mob"), TEXT("Mob")}, {MemberType, TEXT("Npc"), TEXT("Npc")}, {MemberType, TEXT("Pet"), TEXT("Pet")}};
	TArray<FString> Errors;
	if (!TestTrue(FString::Printf(TEXT("an npcs area bakes (%s)"), *FString::Join(Errors, TEXT("; "))), Area->Bake(Errors)))
	{
		return false;
	}

	TArray<uint8> Containers;
	FCrowdyExecWriter Writer(Containers);
	WriteKey(Writer, "containers");
	Writer.ArrayHeader(2);
	Writer.MapHeader(3);
	WriteKey(Writer, "id");
	Writer.UInt(4);
	WriteKey(Writer, "type_name");
	WriteText(Writer, "Pet");
	WriteKey(Writer, "props");
	Writer.MapHeader(3);
	WriteKey(Writer, "name");
	WriteText(Writer, "Mog");
	WriteKey(Writer, "x");
	Writer.Double(10.5);
	WriteKey(Writer, "y");
	Writer.Double(-3.5);
	Writer.MapHeader(3);
	WriteKey(Writer, "id");
	Writer.UInt(5);
	WriteKey(Writer, "type_name");
	WriteText(Writer, "Mob");
	WriteKey(Writer, "props");
	Writer.MapHeader(2);
	WriteKey(Writer, "kind");
	WriteText(Writer, "wolf");
	WriteKey(Writer, "hp");
	Writer.UInt(40);

	FCrowdyExecKitHeader Header;
	TArray<FCrowdyExecKitMember> Members;
	FString Error;
	const bool bDecoded = CrowdyExec::DecodeKitCollection(*Area, KitMessage(Epoch, 1, "snapshot", {}, 1, Containers), Header, Members, Error);
	if (!TestTrue(FString::Printf(TEXT("an area's snapshot decodes (%s)"), *Error), bDecoded) || !TestEqual(TEXT("with both members"), Members.Num(), 2))
	{
		return false;
	}
	const FCrowdyKitAreaMember* Pet = Members[0].Values.GetPtr<FCrowdyKitAreaMember>();
	const FCrowdyKitAreaMember* Mob = Members[1].Values.GetPtr<FCrowdyKitAreaMember>();
	if (!TestTrue(TEXT("each one an area member"), Pet && Mob))
	{
		return false;
	}
	TestTrue(TEXT("the pet is a Pet, by its type_name"), Pet->Type == ECrowdyKitAreaMemberType::Pet && Pet->Id == 4 && Pet->Name == TEXT("Mog"));
	TestTrue(TEXT("on the ground at x and y"), Pet->X == 10.5 && Pet->Y == -3.5);
	TestTrue(TEXT("the mob a Mob, with its own fields"), Mob->Type == ECrowdyKitAreaMemberType::Mob && Mob->Kind == TEXT("wolf") && Mob->Hp == 40 && Mob->Name.IsEmpty());
	return true;
}

namespace CrowdyServerObjectKitTests
{
	inline constexpr int64 Me = 7;
	inline constexpr int64 Rival = 9;

	FCrowdyServerFunction KitFunction(const TCHAR* Name, const TCHAR* Method, UScriptStruct* Params, UScriptStruct* Reply, bool bSendOnce)
	{
		FCrowdyServerFunction Out;
		Out.Name = Name;
		Out.ServerName = Method;
		Out.ParamsForm = ECrowdyServerValuesForm::Struct;
		Out.Params = Params;
		Out.ReplyForm = ECrowdyServerValuesForm::Struct;
		Out.Reply = Reply;
		Out.bSendOnce = bSendOnce;
		return Out;
	}

	/** Makes the rig's type a starter shaped like matchmaker, on its queue topic, with the signed-in player Me. */
	bool MakeKitQueue(FRig& Rig)
	{
		UCrowdyServerObjectDefinition& Queue = *Rig.Definition;
		Queue.CodeSource = ECrowdyServerCodeSource::GameKitStarter;
		Queue.KitStateTopic = TEXT("queue");
		Queue.State = FCrowdyKitQueueCounts::StaticStruct();
		Queue.WatchedFields.Reset();
		Queue.Functions = {
			KitFunction(TEXT("Join"), TEXT("join"), FCrowdyKitMatchJoinInputs::StaticStruct(), FCrowdyKitMatchPlace::StaticStruct(), true),
			KitFunction(TEXT("Leave"), TEXT("leave"), nullptr, nullptr, true),
			KitFunction(TEXT("Accept"), TEXT("accept"), nullptr, FCrowdyKitMatchPlace::StaticStruct(), true),
			KitFunction(TEXT("GetStatus"), TEXT("status"), nullptr, FCrowdyKitMatchPlace::StaticStruct(), false),
		};
		Rig.Session->SetUserID(Me);
		TArray<FString> Errors;
		const bool bBaked = Queue.Bake(Errors);
		return Rig.Test.TestTrue(FString::Printf(TEXT("the matchmaker starter bakes (%s)"), *FString::Join(Errors, TEXT("; "))), bBaked);
	}

	UCrowdyServerObject* AcquireQueue(FRig& Rig, const UObject* Owner)
	{
		UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner);
		FSentFrame Subscribe;
		FSentFrame Read;
		if (!Object || !Rig.Open() || !Rig.WaitForFrame(KindSubscribe, TEXT("queue"), Subscribe) || !Rig.WaitForFrame(KindCall, TEXT("snapshot"), Read))
		{
			return nullptr;
		}
		Rig.Reply(Subscribe.Rid, ECrowdyNativeExecStatus::Ok, {});
		Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::Ok, KitMessage(Epoch, 1, "snapshot", Rig.Encode(FCrowdyKitQueueCounts())));
		const bool bReady = Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; });
		return Rig.Test.TestTrue(TEXT("the queue is Ready"), bReady) ? Object : nullptr;
	}

	TArray<uint8> Place(FRig& Rig, ECrowdyKitMatchState State, TArray<int64> Accepted = {}, const TCHAR* Session = TEXT(""))
	{
		FCrowdyKitMatchPlace Value;
		Value.State = State;
		if (State == ECrowdyKitMatchState::Proposed)
		{
			Value.Players = {Me, Rival};
			Value.Accepted = MoveTemp(Accepted);
		}
		Value.Session = Session;
		return Rig.Encode(Value);
	}

	/** Waits for the search's call of Method and answers it, then pumps until the search has the answer. */
	bool Answer(FRig& Rig, FCrowdyKitMatchSearch& Search, const TCHAR* Method, ECrowdyNativeExecStatus Status, const TArray<uint8>& Payload)
	{
		FSentFrame Frame;
		if (!Rig.Test.TestTrue(FString::Printf(TEXT("the search sends %s"), Method), Rig.WaitForFrame(KindCall, Method, Frame)))
		{
			return false;
		}
		Rig.Reply(Frame.Rid, Status, Payload);
		return Rig.Test.TestTrue(FString::Printf(TEXT("and takes the answer to %s"), Method), Rig.PumpUntil([&Search]() { return !Search.IsCallOutForTest(); }));
	}

	FString Stages(const TArray<FCrowdyKitMatchProgress>& Seen)
	{
		return FString::JoinBy(Seen, TEXT(","), [](const FCrowdyKitMatchProgress& Progress)
		{
			const FString Stage = StaticEnum<ECrowdyKitMatchStage>()->GetNameStringByValue(static_cast<int64>(Progress.Stage));
			return Progress.Code.IsNone() ? Stage : Stage + TEXT(":") + Progress.Code.ToString();
		});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterFindMatchAcceptsAndMatches,
	"CrowdySDK.CrowdyExec.GameKitStarter.FindMatchAcceptsAndMatches", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterFindMatchAcceptsAndMatches::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	if (!Rig.IsValid() || !MakeKitQueue(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Queue = AcquireQueue(Rig, Owner.Get());
	if (!Queue)
	{
		return false;
	}
	// Taken before the search starts, so a step at Now plus a second is never early for it.
	const double Now = FPlatformTime::Seconds();
	TArray<FCrowdyKitMatchProgress> Seen;
	const TSharedRef<FCrowdyKitMatchSearch> Search = CrowdyKit::FindMatch(Queue, 1200, true, 0.f, [&Seen](const FCrowdyKitMatchProgress& Progress) { Seen.Add(Progress); });
	// Ended before Seen goes, so a search a failed check left running cannot write to it later.
	ON_SCOPE_EXIT { Search->Cancel(); };
	FSentFrame Join;
	if (!TestTrue(TEXT("Find Match joins at once"), Rig.WaitForFrame(KindCall, TEXT("join"), Join)))
	{
		return false;
	}
	TestTrue(TEXT("with its rating"), Contains(Join.Payload, "rating") && Contains(Join.Payload, "action_id"));
	Rig.Reply(Join.Rid, ECrowdyNativeExecStatus::Ok, Place(Rig, ECrowdyKitMatchState::Queued));
	Rig.PumpUntil([&Search]() { return !Search->IsCallOutForTest(); });

	Search->Tick(Now + 0.5);
	TestFalse(TEXT("it asks the status no sooner than a second later"), Search->IsCallOutForTest());
	Search->Tick(Now + 1.5);
	Answer(Rig, *Search, TEXT("status"), ECrowdyNativeExecStatus::Ok, Place(Rig, ECrowdyKitMatchState::Queued));
	Search->Tick(Now + 3.0);
	Answer(Rig, *Search, TEXT("status"), ECrowdyNativeExecStatus::Ok, Place(Rig, ECrowdyKitMatchState::Proposed));
	Search->Tick(Now + 3.1);
	Answer(Rig, *Search, TEXT("accept"), ECrowdyNativeExecStatus::Ok, Place(Rig, ECrowdyKitMatchState::Proposed, {Me}));
	Search->Tick(Now + 3.2);
	TestFalse(TEXT("an accepted match is not accepted again"), Search->IsCallOutForTest());
	Search->Tick(Now + 4.5);
	Answer(Rig, *Search, TEXT("status"), ECrowdyNativeExecStatus::Ok, Place(Rig, ECrowdyKitMatchState::Matched, {}, TEXT("ranked-3-1")));

	TestEqualSensitive(TEXT("Queued once, Proposed for the proposal and for this player's accept, then Matched"), Stages(Seen), FString(TEXT("Queued,Proposed,Proposed,Matched")));
	TestTrue(TEXT("a proposal names its players"), Seen.Num() > 1 && Seen[1].Players == TArray<int64>({Me, Rival}) && Seen[1].Accepted.IsEmpty());
	TestTrue(TEXT("and then who accepted"), Seen.Num() > 2 && Seen[2].Accepted == TArray<int64>({Me}));
	TestTrue(TEXT("Matched names the session"), Seen.Num() > 3 && Seen[3].Session == TEXT("ranked-3-1"));
	TestTrue(TEXT("and the search is over"), Search->IsOver() && !Search->Tick(Now + 9.0) && !Search->IsCallOutForTest());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterFindMatchLostProposals,
	"CrowdySDK.CrowdyExec.GameKitStarter.FindMatchLostProposals", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterFindMatchLostProposals::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	if (!Rig.IsValid() || !MakeKitQueue(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Queue = AcquireQueue(Rig, Owner.Get());
	if (!Queue)
	{
		return false;
	}
	const double Now = FPlatformTime::Seconds();
	TArray<FCrowdyKitMatchProgress> Accepting;
	const TSharedRef<FCrowdyKitMatchSearch> Accepter = CrowdyKit::FindMatch(Queue, 1000, true, 0.f, [&Accepting](const FCrowdyKitMatchProgress& Progress) { Accepting.Add(Progress); });
	ON_SCOPE_EXIT { Accepter->Cancel(); };
	Answer(Rig, *Accepter, TEXT("join"), ECrowdyNativeExecStatus::Ok, Place(Rig, ECrowdyKitMatchState::Proposed));
	Accepter->Tick(Now + 0.1);
	Answer(Rig, *Accepter, TEXT("accept"), ECrowdyNativeExecStatus::Ok, Place(Rig, ECrowdyKitMatchState::Proposed, {Me}));
	Accepter->Tick(Now + 1.5);
	Answer(Rig, *Accepter, TEXT("status"), ECrowdyNativeExecStatus::Ok, Place(Rig, ECrowdyKitMatchState::Idle));
	Accepter->Tick(Now + 1.6);
	Answer(Rig, *Accepter, TEXT("join"), ECrowdyNativeExecStatus::Ok, Place(Rig, ECrowdyKitMatchState::Queued));
	TestEqualSensitive(TEXT("a match that fell through after this player accepted is searched for again"), Stages(Accepting), FString(TEXT("Proposed,Proposed,Queued")));
	Accepter->Cancel();
	FSentFrame Leave;
	TestTrue(TEXT("Cancel leaves the queue"), Rig.WaitForFrame(KindCall, TEXT("leave"), Leave));
	TestTrue(TEXT("and ends with cancelled"), Accepter->IsOver() && Accepting.Num() == 4 && Accepting[3].Code == FName(TEXT("cancelled")));

	TArray<FCrowdyKitMatchProgress> Waiting;
	const TSharedRef<FCrowdyKitMatchSearch> Waiter = CrowdyKit::FindMatch(Queue, 1000, false, 0.f, [&Waiting](const FCrowdyKitMatchProgress& Progress) { Waiting.Add(Progress); });
	ON_SCOPE_EXIT { Waiter->Cancel(); };
	Answer(Rig, *Waiter, TEXT("join"), ECrowdyNativeExecStatus::Ok, Place(Rig, ECrowdyKitMatchState::Queued));
	Waiter->Tick(Now + 1.5);
	Answer(Rig, *Waiter, TEXT("status"), ECrowdyNativeExecStatus::Ok, Place(Rig, ECrowdyKitMatchState::Proposed));
	Waiter->Tick(Now + 3.0);
	TestFalse(TEXT("without Auto Accept nothing is accepted"), Rig.SentAny(KindCall, TEXT("accept")));
	Answer(Rig, *Waiter, TEXT("status"), ECrowdyNativeExecStatus::Ok, Place(Rig, ECrowdyKitMatchState::Idle));
	TestEqualSensitive(TEXT("a match this player did not accept in time ends the search"), Stages(Waiting), FString(TEXT("Queued,Proposed,Failed:expired")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterFindMatchEnds,
	"CrowdySDK.CrowdyExec.GameKitStarter.FindMatchEnds", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterFindMatchEnds::RunTest(const FString& Parameters)
{
	AddExpectedMessagePlain(TEXT("GetWorldFromContextObject"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	FRig Rig(*this);
	if (!Rig.IsValid() || !MakeKitQueue(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Queue = AcquireQueue(Rig, Owner.Get());
	if (!Queue)
	{
		return false;
	}
	const double Now = FPlatformTime::Seconds();
	TArray<FCrowdyKitMatchProgress> Full;
	const TSharedRef<FCrowdyKitMatchSearch> Refused = CrowdyKit::FindMatch(Queue, 1000, true, 0.f, [&Full](const FCrowdyKitMatchProgress& Progress) { Full.Add(Progress); });
	ON_SCOPE_EXIT { Refused->Cancel(); };
	Answer(Rig, *Refused, TEXT("join"), ECrowdyNativeExecStatus::AppError, Utf8Bytes(TEXT("kit:full: the queue is full")));
	TestTrue(TEXT("a refused join ends with its code"), Full.Num() == 1 && Full[0].Code == FName(TEXT("full")) && Full[0].Reason == TEXT("the queue is full"));
	FSentFrame RefusedLeave;
	TestTrue(TEXT("and leaves the queue, so no one is proposed with a player who stopped looking"), Rig.WaitForFrame(KindCall, TEXT("leave"), RefusedLeave));

	TArray<FCrowdyKitMatchProgress> Stopped;
	const TSharedRef<FCrowdyKitMatchSearch> Cancelled = CrowdyKit::FindMatch(Queue, 1000, true, 0.f, [&Stopped](const FCrowdyKitMatchProgress& Progress) { Stopped.Add(Progress); });
	ON_SCOPE_EXIT { Cancelled->Cancel(); };
	FSentFrame Pending;
	if (!TestTrue(TEXT("a join is out"), Rig.WaitForFrame(KindCall, TEXT("join"), Pending)))
	{
		return false;
	}
	Cancelled->Cancel();
	Rig.PumpFor(0.1);
	TestFalse(TEXT("Cancel with a call out leaves nothing yet, so a join it overtook cannot queue the player again"), Cancelled->IsOver() || Rig.SentAny(KindCall, TEXT("leave")));
	Rig.Reply(Pending.Rid, ECrowdyNativeExecStatus::Ok, Place(Rig, ECrowdyKitMatchState::Queued));
	FSentFrame CancelledLeave;
	TestTrue(TEXT("once it is answered the search leaves"), Rig.WaitForFrame(KindCall, TEXT("leave"), CancelledLeave));
	TestEqualSensitive(TEXT("and ends cancelled"), Stages(Stopped), FString(TEXT("Failed:cancelled")));

	TArray<FCrowdyKitMatchProgress> Made;
	const TSharedRef<FCrowdyKitMatchSearch> Overrun = CrowdyKit::FindMatch(Queue, 1000, true, 5.f, [&Made](const FCrowdyKitMatchProgress& Progress) { Made.Add(Progress); });
	ON_SCOPE_EXIT { Overrun->Cancel(); };
	Answer(Rig, *Overrun, TEXT("join"), ECrowdyNativeExecStatus::Ok, Place(Rig, ECrowdyKitMatchState::Proposed));
	Overrun->Tick(Now + 0.1);
	FSentFrame OverrunAccept;
	if (!TestTrue(TEXT("an accept is out"), Rig.WaitForFrame(KindCall, TEXT("accept"), OverrunAccept)))
	{
		return false;
	}
	Overrun->Tick(Now + 60.0);
	TestFalse(TEXT("the timeout waits for the accept"), Overrun->IsOver());
	Rig.Reply(OverrunAccept.Rid, ECrowdyNativeExecStatus::Ok, Place(Rig, ECrowdyKitMatchState::Matched, {}, TEXT("ranked-5-1")));
	Rig.PumpUntil([&Overrun]() { return Overrun->IsOver(); });
	TestEqualSensitive(TEXT("and a match it made is not lost to the timeout"), Stages(Made), FString(TEXT("Proposed,Matched")));

	Rig.Session->SetUserID(0);
	TArray<FCrowdyKitMatchProgress> Unknown;
	const TSharedRef<FCrowdyKitMatchSearch> Anonymous = CrowdyKit::FindMatch(Queue, 1000, true, 0.f, [&Unknown](const FCrowdyKitMatchProgress& Progress) { Unknown.Add(Progress); });
	ON_SCOPE_EXIT { Anonymous->Cancel(); };
	Answer(Rig, *Anonymous, TEXT("join"), ECrowdyNativeExecStatus::Ok, Place(Rig, ECrowdyKitMatchState::Proposed));
	Anonymous->Tick(Now + 0.1);
	Answer(Rig, *Anonymous, TEXT("accept"), ECrowdyNativeExecStatus::Ok, Place(Rig, ECrowdyKitMatchState::Proposed, {Me}));
	Anonymous->Tick(Now + 0.2);
	TestFalse(TEXT("an accept's answer counts as accepted when the player's user id is not known"), Anonymous->IsCallOutForTest());
	Anonymous->Tick(Now + 30.0);
	Answer(Rig, *Anonymous, TEXT("status"), ECrowdyNativeExecStatus::Ok, Place(Rig, ECrowdyKitMatchState::Proposed, {Me}));
	Anonymous->Tick(Now + 30.1);
	TestFalse(TEXT("and stays so while the same match is proposed"), Anonymous->IsCallOutForTest());
	Rig.Session->SetUserID(Me);

	TArray<FCrowdyKitMatchProgress> Late;
	const TSharedRef<FCrowdyKitMatchSearch> Manual = CrowdyKit::FindMatch(Queue, 1000, false, 0.f, [&Late](const FCrowdyKitMatchProgress& Progress) { Late.Add(Progress); });
	ON_SCOPE_EXIT { Manual->Cancel(); };
	Answer(Rig, *Manual, TEXT("join"), ECrowdyNativeExecStatus::Ok, Place(Rig, ECrowdyKitMatchState::Proposed));
	Manual->Tick(Now + 30.0);
	Manual->Accept();
	Answer(Rig, *Manual, TEXT("status"), ECrowdyNativeExecStatus::Ok, Place(Rig, ECrowdyKitMatchState::Proposed));
	Manual->Tick(Now + 30.1);
	Answer(Rig, *Manual, TEXT("accept"), ECrowdyNativeExecStatus::AppError, Utf8Bytes(TEXT("kit:not_found: you have no match to accept")));
	TestFalse(TEXT("an Accept asked for while a status was out is sent, and one the proposal did not outlive does not end the search"), Manual->IsOver());
	Manual->Tick(Now + 60.0);
	Answer(Rig, *Manual, TEXT("status"), ECrowdyNativeExecStatus::Ok, Place(Rig, ECrowdyKitMatchState::Queued));
	TestEqualSensitive(TEXT("it asks the status and waits on"), Stages(Late), FString(TEXT("Proposed,Queued")));
	Manual->Cancel();

	TArray<FCrowdyKitMatchProgress> Gone;
	const TSharedRef<FCrowdyKitMatchSearch> Left = CrowdyKit::FindMatch(Queue, 1000, true, 0.f, [&Gone](const FCrowdyKitMatchProgress& Progress) { Gone.Add(Progress); });
	ON_SCOPE_EXIT { Left->Cancel(); };
	Answer(Rig, *Left, TEXT("join"), ECrowdyNativeExecStatus::Ok, Place(Rig, ECrowdyKitMatchState::Queued));
	Left->Tick(Now + 30.0);
	Answer(Rig, *Left, TEXT("status"), ECrowdyNativeExecStatus::Ok, Place(Rig, ECrowdyKitMatchState::Idle));
	TestEqualSensitive(TEXT("a player taken out of the queue another way ends the search"), Stages(Gone), FString(TEXT("Queued,Failed:left")));

	TArray<FCrowdyKitMatchProgress> Slow;
	const TSharedRef<FCrowdyKitMatchSearch> Timed = CrowdyKit::FindMatch(Queue, 1000, true, 5.f, [&Slow](const FCrowdyKitMatchProgress& Progress) { Slow.Add(Progress); });
	ON_SCOPE_EXIT { Timed->Cancel(); };
	Answer(Rig, *Timed, TEXT("join"), ECrowdyNativeExecStatus::Ok, Place(Rig, ECrowdyKitMatchState::Queued));
	Rig.Sent.Reset();
	TestFalse(TEXT("after its Timeout the search ends"), Timed->Tick(Now + 60.0));
	FSentFrame Leave;
	TestTrue(TEXT("leaving the queue"), Rig.WaitForFrame(KindCall, TEXT("leave"), Leave));
	TestEqualSensitive(TEXT("with timeout"), Stages(Slow), FString(TEXT("Queued,Failed:timeout")));

	TStrongObjectPtr<UCrowdyKitMatchTestListener> Listener(NewObject<UCrowdyKitMatchTestListener>(GetTransientPackage(), NAME_None, RF_Transient));
	TStrongObjectPtr<UCrowdyFindMatchAction> Action(UCrowdyFindMatchAction::FindMatch(nullptr, Queue, 1000, true, 0.f));
	Action->Queued.AddDynamic(Listener.Get(), &UCrowdyKitMatchTestListener::HandleQueued);
	Action->Proposed.AddDynamic(Listener.Get(), &UCrowdyKitMatchTestListener::HandleProposed);
	Action->Matched.AddDynamic(Listener.Get(), &UCrowdyKitMatchTestListener::HandleMatched);
	Action->Failed.AddDynamic(Listener.Get(), &UCrowdyKitMatchTestListener::HandleFailed);
	Action->Activate();
	if (!TestNotNull(TEXT("the Blueprint node starts a search"), Action->GetSearchForTest()))
	{
		return false;
	}
	Answer(Rig, *Action->GetSearchForTest(), TEXT("join"), ECrowdyNativeExecStatus::Ok, Place(Rig, ECrowdyKitMatchState::Proposed));
	Action->GetSearchForTest()->Tick(Now + 0.1);
	Answer(Rig, *Action->GetSearchForTest(), TEXT("accept"), ECrowdyNativeExecStatus::Ok, Place(Rig, ECrowdyKitMatchState::Matched, {}, TEXT("ranked-4-2")));
	TestEqualSensitive(TEXT("Find Match's pins fire as the search goes"), FString::Join(Listener->Pins, TEXT(",")), FString(TEXT("Proposed 0/2,Matched ranked-4-2")));

	TStrongObjectPtr<UCrowdyFindMatchAction> NoQueue(UCrowdyFindMatchAction::FindMatch(nullptr, nullptr, 1000, true, 0.f));
	NoQueue->Failed.AddDynamic(Listener.Get(), &UCrowdyKitMatchTestListener::HandleFailed);
	NoQueue->Activate();
	TestEqualSensitive(TEXT("with no queue Failed does not run before the node's own output"), Listener->Pins.Last(), FString(TEXT("Matched ranked-4-2")));
	if (TestNotNull(TEXT("it has a search"), NoQueue->GetSearchForTest()))
	{
		NoQueue->GetSearchForTest()->Tick(Now);
	}
	TestEqualSensitive(TEXT("but on its first tick"), Listener->Pins.Last(), FString(TEXT("Failed None")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterTurns,
	"CrowdySDK.CrowdyExec.GameKitStarter.Turns", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterTurns::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	UCrowdyServerObjectDefinition& Session = *Rig.Definition;
	Session.CodeSource = ECrowdyServerCodeSource::GameKitStarter;
	Session.KitStateTopic = TEXT("session");
	Session.State = FCrowdyKitSessionState::StaticStruct();
	Session.WatchedFields.Reset();
	Session.Functions.Reset();
	Rig.Session->SetUserID(Me);
	TArray<FString> Errors;
	if (!Rig.IsValid() || !TestTrue(TEXT("the session starter bakes"), Session.Bake(Errors)))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	FSentFrame Subscribe;
	FSentFrame Read;
	if (!Object || !Rig.Open() || !Rig.WaitForFrame(KindSubscribe, TEXT("session"), Subscribe) || !Rig.WaitForFrame(KindCall, TEXT("snapshot"), Read))
	{
		return false;
	}
	int64 Player = -1;
	TestFalse(TEXT("nobody's turn before the session is read"), CrowdyKit::GetTurnPlayer(Object, Player) || CrowdyKit::IsMyTurn(Object));
	TestEqual(TEXT("and the player is 0"), Player, int64(0));

	FCrowdyKitSessionState State;
	State.Phase = ECrowdyKitSessionPhase::Playing;
	State.Players = {Rival, Me};
	State.Turn = 1;
	Rig.Reply(Subscribe.Rid, ECrowdyNativeExecStatus::Ok, {});
	Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::Ok, KitMessage(Epoch, 1, "snapshot", Rig.Encode(State)));
	if (!TestTrue(TEXT("the session is Ready"), Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; })))
	{
		return false;
	}
	TestTrue(TEXT("the turn index names its player"), UCrowdyGameKitLibrary::GetTurnPlayer(Object, Player) && Player == Me);
	TestTrue(TEXT("whose turn is the signed-in player's"), UCrowdyGameKitLibrary::IsMyTurn(Object));

	auto Push = [&Rig, Object](const FCrowdyKitSessionState& Next, uint64 Seq)
	{
		Rig.PushTopic(TEXT("session"), KitMessage(Epoch, Seq, "snapshot", Rig.Encode(Next)));
		return Rig.PumpUntil([Object, &Next]() { const FCrowdyKitSessionState* Now = Object->GetState().GetPtr<FCrowdyKitSessionState>(); return Now && Now->Turn == Next.Turn && Now->Phase == Next.Phase; });
	};
	State.Turn = 0;
	TestTrue(TEXT("the next turn arrives"), Push(State, 2));
	TestTrue(TEXT("and is the rival's"), CrowdyKit::GetTurnPlayer(Object, Player) && Player == Rival && !CrowdyKit::IsMyTurn(Object));
	State.Turn = 2;
	TestTrue(TEXT("a turn past the players arrives"), Push(State, 3));
	TestFalse(TEXT("and names nobody"), CrowdyKit::GetTurnPlayer(Object, Player));
	State.Turn = 1;
	State.Phase = ECrowdyKitSessionPhase::Finished;
	TestTrue(TEXT("the finish arrives"), Push(State, 4));
	TestFalse(TEXT("and once it finished it is nobody's turn"), CrowdyKit::GetTurnPlayer(Object, Player) || CrowdyKit::IsMyTurn(Object));
	return true;
}

namespace CrowdyServerObjectKitTests
{
	inline constexpr const TCHAR* PlayerTopic = TEXT("player");

	/** Makes the rig's type a starter shaped like player: its profile on the player topic, the wallet and inventory as Owner Values, the signed-in player Me. */
	bool MakeKitPlayer(FRig& Rig, bool bKeyedByPlayer)
	{
		UCrowdyServerObjectDefinition& Player = *Rig.Definition;
		Player.CodeSource = ECrowdyServerCodeSource::GameKitStarter;
		Player.KitStateTopic = PlayerTopic;
		Player.State = FCrowdyKitPlayerState::StaticStruct();
		Player.WatchedFields.Reset();
		Player.Functions.Reset();
		Player.bKitKeyedByPlayer = bKeyedByPlayer;
		Player.KitOwnerValues = {{TEXT("wallet"), TEXT("wallet"), TEXT("Wallet")}, {TEXT("inventory"), TEXT("inventory"), TEXT("Inventory")}};
		Player.Messages.Reset();
		FCrowdyServerMessage& Levelled = Player.Messages.AddDefaulted_GetRef();
		Levelled.Name = RoaredMessage;
		Levelled.ServerName = PlayerTopic;
		Levelled.Payload = FCrowdyServerObjectTestMessagePayload::StaticStruct();
		Rig.Session->SetUserID(Me);
		Rig.InstanceId = LexToString(Me);
		TArray<FString> Errors;
		const bool bBaked = Player.Bake(Errors);
		return Rig.Test.TestTrue(FString::Printf(TEXT("the player starter bakes (%s)"), *FString::Join(Errors, TEXT("; "))), bBaked);
	}

	TArray<uint8> Profile(FRig& Rig, uint64 Seq, int32 Level)
	{
		FCrowdyKitPlayerState Value;
		Value.Player = Me;
		Value.Level = Level;
		return KitMessage(Epoch, Seq, "snapshot", Rig.Encode(Value, TArray<FString>{TEXT("player"), TEXT("level"), TEXT("achievements"), TEXT("quests_done")}));
	}

	TArray<uint8> Wallet(FRig& Rig, int64 Gold)
	{
		FCrowdyKitWallet Value;
		Value.Balances.Add(TEXT("gold"), Gold);
		return Rig.Encode(Value);
	}

	/** A changed message naming Who; What null leaves it out. */
	TArray<uint8> Changed(uint64 Seq, TArray<int64> Who, const ANSICHAR* What)
	{
		TArray<uint8> Pairs;
		FCrowdyExecWriter Writer(Pairs);
		WriteKey(Writer, "who");
		Writer.ArrayHeader(Who.Num());
		for (const int64 Id : Who)
		{
			Writer.UInt(Id);
		}
		if (What)
		{
			WriteKey(Writer, "what");
			WriteText(Writer, What);
		}
		return KitMessage(Epoch, Seq, "changed", {}, What ? 2 : 1, Pairs);
	}

	/** Pushes a changed message and ticks once, which is when the Owner Values it names are read. */
	void PushChanged(FRig& Rig, const FString& ChangedTopic, const TArray<uint8>& Message)
	{
		Rig.PushTopic(ChangedTopic, Message);
		Rig.PumpFor(0.05);
		Rig.Tick(0.1f);
	}

	const FCrowdyKitPlayerState* PlayerOf(const UCrowdyServerObject& Object)
	{
		return Object.GetState().GetPtr<FCrowdyKitPlayerState>();
	}

	/** Acquires Id, answers its subscribe and its profile, and returns it still waiting for its Owner Values. */
	UCrowdyServerObject* AcquirePlayer(FRig& Rig, const UObject* Owner, const FString& Id)
	{
		UCrowdyServerObject* Object = Rig.Acquire(Id, Owner);
		FSentFrame Subscribe;
		FSentFrame Read;
		if (!Object || !Rig.Open() || !Rig.WaitForFrame(KindSubscribe, PlayerTopic, Subscribe) || !Rig.WaitForFrame(KindCall, TEXT("snapshot"), Read))
		{
			return nullptr;
		}
		Rig.Reply(Subscribe.Rid, ECrowdyNativeExecStatus::Ok, {});
		Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::Ok, Profile(Rig, 1, 3));
		return Object;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterOwnerValuesFillTheState,
	"CrowdySDK.CrowdyExec.GameKitStarter.OwnerValuesFillTheState", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterOwnerValuesFillTheState::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	if (!Rig.IsValid() || !MakeKitPlayer(Rig, true))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquirePlayer(Rig, Owner.Get(), Rig.InstanceId);
	FSentFrame WalletRead;
	FSentFrame InventoryRead;
	if (!Object || !TestTrue(TEXT("the wallet is read by its own call"), Rig.WaitForFrame(KindCall, TEXT("wallet"), WalletRead))
		|| !TestTrue(TEXT("and the inventory"), Rig.WaitForFrame(KindCall, TEXT("inventory"), InventoryRead)))
	{
		return false;
	}
	TestTrue(TEXT("addressed to the player's own hub"), Rig.IsAddressed(WalletRead));
	Rig.Reply(WalletRead.Rid, ECrowdyNativeExecStatus::Ok, Wallet(Rig, 100));
	Rig.PumpFor(0.2);
	TestTrue(TEXT("the profile and one Owner Value leave it Connecting"), Object->GetStatus() == ECrowdyServerObjectStatus::Connecting);
	FCrowdyKitInventory Inventory;
	Inventory.Slots = 30;
	Rig.Reply(InventoryRead.Rid, ECrowdyNativeExecStatus::Ok, Rig.Encode(Inventory));
	if (!TestTrue(TEXT("the last Owner Value makes it Ready"), Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; })))
	{
		return false;
	}
	const FCrowdyKitPlayerState* Read = PlayerOf(*Object);
	TestTrue(TEXT("its State holds the profile and both Owner Values"), Read && Read->Level == 3 && Read->Wallet.Balances.FindRef(TEXT("gold")) == 100 && Read->Inventory.Slots == 30);

	TSharedRef<TArray<FString>> Heard = MakeShared<TArray<FString>>();
	Object->WatchValues(FOnCrowdyServerValuesChanged::FDelegate::CreateLambda([Heard](UCrowdyServerObject*, TConstArrayView<FString> Fields) { Heard->Append(Fields.GetData(), Fields.Num()); }));
	// A new listener's catch-up call names every value.
	Rig.PumpFor(0.1);
	Heard->Reset();
	Rig.PushTopic(PlayerTopic, Profile(Rig, 2, 4));
	TestTrue(TEXT("a pushed profile arrives"), Rig.PumpUntil([Object]() { return PlayerOf(*Object)->Level == 4; }));
	TestEqual(TEXT("and keeps the Owner Values, which no snapshot carries"), PlayerOf(*Object)->Wallet.Balances.FindRef(TEXT("gold")), int64(100));
	TestFalse(TEXT("announcing no change to them"), Heard->Contains(TEXT("wallet")));

	PushChanged(Rig, PlayerTopic, Changed(3, {Rival}, "wallet"));
	TestFalse(TEXT("a change named for another player reads nothing"), Rig.SentAny(KindCall, TEXT("wallet")));
	PushChanged(Rig, PlayerTopic, Changed(4, {Me}, "progress"));
	TestFalse(TEXT("nor one of a value it does not hold"), Rig.SentAnyCall());
	PushChanged(Rig, PlayerTopic, Changed(5, {Rival, Me}, "wallet"));
	FSentFrame Again;
	if (!TestTrue(TEXT("a change named for this player reads that value again"), Rig.WaitForFrame(KindCall, TEXT("wallet"), Again)))
	{
		return false;
	}
	TestFalse(TEXT("and only that one"), Rig.SentAny(KindCall, TEXT("inventory")));
	Rig.Reply(Again.Rid, ECrowdyNativeExecStatus::Ok, Wallet(Rig, 80));
	TestTrue(TEXT("the new balance arrives"), Rig.PumpUntil([Object]() { return PlayerOf(*Object)->Wallet.Balances.FindRef(TEXT("gold")) == 80; }));
	TestTrue(TEXT("and is announced by its name"), Heard->Contains(TEXT("wallet")));
	TestFalse(TEXT("alone, the other Owner Value being unchanged"), Heard->Contains(TEXT("inventory")));

	PushChanged(Rig, PlayerTopic, Changed(6, {Me}, nullptr));
	FSentFrame WalletAll;
	FSentFrame InventoryAll;
	TestTrue(TEXT("a changed message it cannot read reads every Owner Value again"),
		Rig.WaitForFrame(KindCall, TEXT("wallet"), WalletAll) && Rig.WaitForFrame(KindCall, TEXT("inventory"), InventoryAll));
	Rig.Reply(WalletAll.Rid, ECrowdyNativeExecStatus::Ok, Wallet(Rig, 80));
	Rig.Reply(InventoryAll.Rid, ECrowdyNativeExecStatus::Ok, Rig.Encode(Inventory));
	Rig.PumpFor(0.2);

	Rig.PushTopic(PlayerTopic, KitMessage(Epoch, 7, "delta", {}));
	FSentFrame Reread;
	FSentFrame WalletAfter;
	if (!TestTrue(TEXT("a delta on the state topic reads the profile again"), Rig.WaitForFrame(KindCall, TEXT("snapshot"), Reread)))
	{
		return false;
	}
	Rig.Reply(Reread.Rid, ECrowdyNativeExecStatus::Ok, Profile(Rig, 8, 4));
	TestTrue(TEXT("and every read of the profile reads the Owner Values after it"), Rig.WaitForFrame(KindCall, TEXT("wallet"), WalletAfter));
	TestTrue(TEXT("which the read kept meanwhile"), PlayerOf(*Object)->Wallet.Balances.FindRef(TEXT("gold")) == 80);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterRefusedOwnerValueClearsThem,
	"CrowdySDK.CrowdyExec.GameKitStarter.RefusedOwnerValueClearsThem", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterRefusedOwnerValueClearsThem::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	if (!Rig.IsValid() || !MakeKitPlayer(Rig, true))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquirePlayer(Rig, Owner.Get(), Rig.InstanceId);
	FSentFrame WalletRead;
	FSentFrame InventoryRead;
	if (!Object || !Rig.WaitForFrame(KindCall, TEXT("wallet"), WalletRead) || !Rig.WaitForFrame(KindCall, TEXT("inventory"), InventoryRead))
	{
		return false;
	}
	Rig.Reply(WalletRead.Rid, ECrowdyNativeExecStatus::Ok, Wallet(Rig, 100));
	Rig.Reply(InventoryRead.Rid, ECrowdyNativeExecStatus::Ok, Rig.Encode(FCrowdyKitInventory()));
	if (!TestTrue(TEXT("Ready"), Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; })))
	{
		return false;
	}
	PushChanged(Rig, PlayerTopic, Changed(2, {Me}, "wallet"));
	FSentFrame Refused;
	if (!Rig.WaitForFrame(KindCall, TEXT("wallet"), Refused))
	{
		return false;
	}
	Rig.Reply(Refused.Rid, ECrowdyNativeExecStatus::AppError, Utf8Bytes(TEXT("kit:forbidden: this is another player's")));
	if (!TestTrue(TEXT("a coded refusal of an Owner Value fails the object"), Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Failed; })))
	{
		return false;
	}
	TestTrue(TEXT("and leaves no balance behind"), PlayerOf(*Object)->Wallet.Balances.IsEmpty());
	TestEqual(TEXT("while the profile stays"), PlayerOf(*Object)->Level, 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterKeyedByPlayer,
	"CrowdySDK.CrowdyExec.GameKitStarter.KeyedByPlayer", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterKeyedByPlayer::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	if (!Rig.IsValid() || !MakeKitPlayer(Rig, true))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	Rig.InstanceId = LexToString(Rival);
	UCrowdyServerObject* Other = AcquirePlayer(Rig, Owner.Get(), Rig.InstanceId);
	if (!Other || !TestTrue(TEXT("another player's profile alone makes it Ready"), Rig.PumpUntil([Other]() { return Other->GetStatus() == ECrowdyServerObjectStatus::Ready; })))
	{
		return false;
	}
	TestFalse(TEXT("reading none of their Owner Values, which their hub keeps from this player"), Rig.SentAny(KindCall, TEXT("wallet")));
	PushChanged(Rig, PlayerTopic, Changed(2, {Me}, "wallet"));
	TestFalse(TEXT("not even on a changed message"), Rig.SentAny(KindCall, TEXT("wallet")));

	UCrowdyServerObject* Mine = Rig.Acquire(FString(), Owner.Get());
	TestTrue(TEXT("an empty Instance Id is the signed-in player's"), Mine && Mine->GetInstanceId() == LexToString(Me));
	TestTrue(TEXT("the same object as their user id gives"), Mine && Rig.Acquire(LexToString(Me), Owner.Get()) == Mine);

	Rig.Session->SetUserID(0);
	FString Error;
	TestNull(TEXT("with nobody signed in an empty Instance Id is refused"), Rig.Subsystem->Acquire(Rig.Definition.Get(), FString(), Owner.Get(), Error));
	TestTrue(FString::Printf(TEXT("saying so (%s)"), *Error), Error.StartsWith(TEXT("Nobody is signed in yet")));

	Rig.Definition->bKitKeyedByPlayer = false;
	Rig.Session->SetUserID(Me);
	TestNull(TEXT("a starter not keyed by player still needs an Instance Id"), Rig.Subsystem->Acquire(Rig.Definition.Get(), FString(), Owner.Get(), Error));
	return true;
}

namespace CrowdyServerObjectKitTests
{
	/** Acquires Me's player and answers its profile, wallet and inventory, so it is Ready with Gold. */
	UCrowdyServerObject* AcquirePlayerReady(FRig& Rig, const UObject* Owner, int64 Gold)
	{
		UCrowdyServerObject* Object = AcquirePlayer(Rig, Owner, Rig.InstanceId);
		FSentFrame WalletRead;
		FSentFrame InventoryRead;
		if (!Object || !Rig.WaitForFrame(KindCall, TEXT("wallet"), WalletRead) || !Rig.WaitForFrame(KindCall, TEXT("inventory"), InventoryRead))
		{
			return nullptr;
		}
		Rig.Reply(WalletRead.Rid, ECrowdyNativeExecStatus::Ok, Wallet(Rig, Gold));
		Rig.Reply(InventoryRead.Rid, ECrowdyNativeExecStatus::Ok, Rig.Encode(FCrowdyKitInventory()));
		const bool bReady = Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; });
		return Rig.Test.TestTrue(TEXT("the player is Ready"), bReady) ? Object : nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterMessagesWaitForOwnerValues,
	"CrowdySDK.CrowdyExec.GameKitStarter.MessagesWaitForOwnerValues", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterMessagesWaitForOwnerValues::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	if (!Rig.IsValid() || !MakeKitPlayer(Rig, true))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquirePlayerReady(Rig, Owner.Get(), 100);
	if (!Object)
	{
		return false;
	}
	TSharedRef<int64> GoldAtMessage = MakeShared<int64>(-1);
	Object->WatchMessage(RoaredMessage, FOnCrowdyServerMessage::FDelegate::CreateLambda([GoldAtMessage](UCrowdyServerObject* Announcer, FName, const FInstancedStruct&)
	{
		*GoldAtMessage = PlayerOf(*Announcer)->Wallet.Balances.FindRef(TEXT("gold"));
	}));
	PushChanged(Rig, PlayerTopic, Changed(2, {Me}, "wallet"));
	FSentFrame WalletRead;
	if (!Rig.WaitForFrame(KindCall, TEXT("wallet"), WalletRead))
	{
		return false;
	}
	Rig.PushTopic(PlayerTopic, KitMessage(Epoch, 3, "event", Rig.Encode(Payload(5, TEXT("level")))));
	Rig.PumpFor(0.2);
	TestEqual(TEXT("an event after a changed message waits for the value it changed"), *GoldAtMessage, int64(-1));
	Rig.Reply(WalletRead.Rid, ECrowdyNativeExecStatus::Ok, Wallet(Rig, 140));
	TestTrue(TEXT("and is heard once it arrives, seeing it"), Rig.PumpUntil([GoldAtMessage]() { return *GoldAtMessage == 140; }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterMessagesAreNotHeldByLaterReads,
	"CrowdySDK.CrowdyExec.GameKitStarter.MessagesAreNotHeldByLaterReads", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterMessagesAreNotHeldByLaterReads::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	if (!Rig.IsValid() || !MakeKitPlayer(Rig, true))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquirePlayerReady(Rig, Owner.Get(), 100);
	if (!Object)
	{
		return false;
	}
	TSharedRef<int32> Heard = MakeShared<int32>(0);
	Object->WatchMessage(RoaredMessage, FOnCrowdyServerMessage::FDelegate::CreateLambda([Heard](UCrowdyServerObject*, FName, const FInstancedStruct&) { ++*Heard; }));
	FSentFrame WalletRead;
	FSentFrame InventoryRead;
	FSentFrame WalletAgain;
	PushChanged(Rig, PlayerTopic, Changed(2, {Me}, "wallet"));
	if (!Rig.WaitForFrame(KindCall, TEXT("wallet"), WalletRead))
	{
		return false;
	}
	PushChanged(Rig, PlayerTopic, Changed(3, {Me}, "inventory"));
	if (!Rig.WaitForFrame(KindCall, TEXT("inventory"), InventoryRead))
	{
		return false;
	}
	Rig.PushTopic(PlayerTopic, KitMessage(Epoch, 4, "event", Rig.Encode(Payload(5, TEXT("level")))));
	Rig.PumpFor(0.1);
	TestEqual(TEXT("an event waits for the reads out when it arrived"), *Heard, 0);
	Rig.Reply(WalletRead.Rid, ECrowdyNativeExecStatus::Ok, Wallet(Rig, 140));
	Rig.PumpFor(0.1);
	TestEqual(TEXT("all of them"), *Heard, 0);

	PushChanged(Rig, PlayerTopic, Changed(5, {Me}, "wallet"));
	if (!Rig.WaitForFrame(KindCall, TEXT("wallet"), WalletAgain))
	{
		return false;
	}
	Rig.Reply(InventoryRead.Rid, ECrowdyNativeExecStatus::Ok, Rig.Encode(FCrowdyKitInventory()));
	TestTrue(TEXT("and is heard when they end, whatever read started since"), Rig.PumpUntil([Heard]() { return *Heard == 1; }));
	Rig.Reply(WalletAgain.Rid, ECrowdyNativeExecStatus::Ok, Wallet(Rig, 150));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterChangedBurstsReadOnce,
	"CrowdySDK.CrowdyExec.GameKitStarter.ChangedBurstsReadOnce", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterChangedBurstsReadOnce::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	if (!Rig.IsValid() || !MakeKitPlayer(Rig, true))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquirePlayerReady(Rig, Owner.Get(), 100);
	if (!Object)
	{
		return false;
	}
	TSharedRef<TArray<FString>> Heard = MakeShared<TArray<FString>>();
	Object->WatchValues(FOnCrowdyServerValuesChanged::FDelegate::CreateLambda([Heard](UCrowdyServerObject*, TConstArrayView<FString> Fields) { Heard->Append(Fields.GetData(), Fields.Num()); }));
	Rig.PumpFor(0.1);
	Heard->Reset();
	Rig.Sent.Reset();

	for (uint64 Seq = 2; Seq < 7; ++Seq)
	{
		Rig.PushTopic(PlayerTopic, Changed(Seq, {Me}, "wallet"));
	}
	Rig.PumpFor(0.1);
	Rig.Tick(0.1f);
	Rig.PumpFor(0.1);
	TestEqual(TEXT("a burst of changed messages reads the value once"), Rig.CountFrames(KindCall, TEXT("wallet")), 1);
	FSentFrame Burst;
	if (!Rig.WaitForFrame(KindCall, TEXT("wallet"), Burst))
	{
		return false;
	}
	Rig.Reply(Burst.Rid, ECrowdyNativeExecStatus::Ok, Wallet(Rig, 100));
	Rig.PumpFor(0.1);
	Rig.Tick(0.1f);
	Rig.PumpFor(0.1);
	TestFalse(TEXT("and its answer ends it, with no second read for the same burst"), Rig.SentAny(KindCall, TEXT("wallet")));

	Rig.PushTopic(PlayerTopic, Changed(7, {Me}, "wallet"));
	Rig.PumpFor(0.1);
	Rig.Tick(0.1f);
	FSentFrame First;
	if (!TestTrue(TEXT("a later change reads again"), Rig.WaitForFrame(KindCall, TEXT("wallet"), First)))
	{
		return false;
	}

	for (uint64 Seq = 8; Seq < 11; ++Seq)
	{
		Rig.PushTopic(PlayerTopic, Changed(Seq, {Me}, "wallet"));
	}
	Rig.PumpFor(0.1);
	Rig.Tick(0.1f);
	Rig.PumpFor(0.1);
	TestEqual(TEXT("changes while the read is out wait for its answer"), Rig.CountFrames(KindCall, TEXT("wallet")), 0);
	Rig.Reply(First.Rid, ECrowdyNativeExecStatus::Ok, Wallet(Rig, 100));
	FSentFrame Second;
	if (!TestTrue(TEXT("and are then read once more"), Rig.WaitForFrame(KindCall, TEXT("wallet"), Second)))
	{
		return false;
	}
	TestFalse(TEXT("an answer that holds the same value announces nothing"), Heard->Contains(TEXT("wallet")));
	Rig.Reply(Second.Rid, ECrowdyNativeExecStatus::Ok, Wallet(Rig, 90));
	TestTrue(TEXT("the last answer is kept"), Rig.PumpUntil([Object]() { return PlayerOf(*Object)->Wallet.Balances.FindRef(TEXT("gold")) == 90; }));
	Rig.Tick(0.1f);
	TestFalse(TEXT("and nothing is read after it"), Rig.SentAny(KindCall, TEXT("wallet")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterAnotherPlayerSigningInReadsAnew,
	"CrowdySDK.CrowdyExec.GameKitStarter.AnotherPlayerSigningInReadsAnew", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterAnotherPlayerSigningInReadsAnew::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	if (!Rig.IsValid() || !MakeKitPlayer(Rig, false))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquirePlayerReady(Rig, Owner.Get(), 100);
	if (!Object)
	{
		return false;
	}
	TSharedRef<TArray<FString>> Heard = MakeShared<TArray<FString>>();
	Object->WatchValues(FOnCrowdyServerValuesChanged::FDelegate::CreateLambda([Heard](UCrowdyServerObject*, TConstArrayView<FString> Fields) { Heard->Append(Fields.GetData(), Fields.Num()); }));
	Rig.PumpFor(0.1);
	Heard->Reset();
	Rig.Sent.Reset();

	Rig.Session->SetUserID(Rival);
	Rig.Subsystem->SignInForTest();
	TestNull(TEXT("another player signing in closes the connection that carries the last one's token"), Rig.Subsystem->GetConnection());
	TestTrue(TEXT("and drops the last player's wallet"), PlayerOf(*Object)->Wallet.Balances.IsEmpty());
	TestTrue(TEXT("announcing it"), Heard->Contains(TEXT("wallet")));
	TestEqual(TEXT("while the profile stays"), PlayerOf(*Object)->Level, 3);

	Rig.Tick(0.1f);
	if (!TestTrue(TEXT("the subsystem dials again"), Rig.PumpUntil([&Rig]() { return DialsOf(Rig) >= 2; })))
	{
		return false;
	}
	Rig.Client->TestExecOpen();
	if (!TestTrue(TEXT("and connects"), Rig.PumpUntil([&Rig]() { return Rig.Subsystem->GetConnection() != nullptr; })))
	{
		return false;
	}
	FSentFrame Subscribe;
	FSentFrame Read;
	FSentFrame WalletRead;
	if (!Rig.WaitForFrame(KindSubscribe, PlayerTopic, Subscribe) || !Rig.WaitForFrame(KindCall, TEXT("snapshot"), Read))
	{
		return false;
	}
	Rig.Reply(Subscribe.Rid, ECrowdyNativeExecStatus::Ok, {});
	Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::Ok, Profile(Rig, 9, 4));
	if (!TestTrue(TEXT("the new connection reads the wallet again"), Rig.WaitForFrame(KindCall, TEXT("wallet"), WalletRead)))
	{
		return false;
	}
	Rig.Reply(WalletRead.Rid, ECrowdyNativeExecStatus::Ok, Wallet(Rig, 55));
	TestTrue(TEXT("for the new player"), Rig.PumpUntil([Object]() { return PlayerOf(*Object)->Wallet.Balances.FindRef(TEXT("gold")) == 55; }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterAnswerForAMissingOwnerValueReadsAgain,
	"CrowdySDK.CrowdyExec.GameKitStarter.AnswerForAMissingOwnerValueReadsAgain", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterAnswerForAMissingOwnerValueReadsAgain::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	if (!Rig.IsValid() || !MakeKitPlayer(Rig, true))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquirePlayerReady(Rig, Owner.Get(), 100);
	if (!Object)
	{
		return false;
	}
	FSentFrame WalletRead;
	PushChanged(Rig, PlayerTopic, Changed(2, {Me}, "wallet"));
	if (!Rig.WaitForFrame(KindCall, TEXT("wallet"), WalletRead))
	{
		return false;
	}
	Rig.Sent.Reset();
	Rig.Definition->KitOwnerValues.Reset();
	Rig.Reply(WalletRead.Rid, ECrowdyNativeExecStatus::Ok, Wallet(Rig, 140));
	FSentFrame Reread;
	TestTrue(TEXT("an answer to an Owner Value the definition no longer has reads the State again"), Rig.WaitForFrame(KindCall, TEXT("snapshot"), Reread));
	TestEqual(TEXT("and is not applied"), PlayerOf(*Object)->Wallet.Balances.FindRef(TEXT("gold")), int64(100));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterKeyedLinkWaitsForSignIn,
	"CrowdySDK.CrowdyExec.GameKitStarter.KeyedLinkWaitsForSignIn", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterKeyedLinkWaitsForSignIn::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	if (!Rig.IsValid() || !MakeKitPlayer(Rig, true))
	{
		return false;
	}
	Rig.Session->SetUserID(0);
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObjectLink* Link = Rig.Subsystem->FindLink(Owner.Get(), Rig.Definition.Get(), ECrowdyServerObjectFind::InstanceId, FString(), 0);
	if (!TestNotNull(TEXT("a Link is made"), Link))
	{
		return false;
	}
	TestNull(TEXT("with nobody signed in it holds nothing"), Link->GetServerObject());
	TestEqualSensitive(TEXT("and waits for the sign-in, which is no failure to join"), Link->GetFailureReason(), FString(CrowdyServerObjectShared::WaitingForSignIn));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterOwnerValuesFollowTheSignedInPlayer,
	"CrowdySDK.CrowdyExec.GameKitStarter.OwnerValuesFollowTheSignedInPlayer", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterOwnerValuesFollowTheSignedInPlayer::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	if (!Rig.IsValid() || !MakeKitPlayer(Rig, true))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquirePlayerReady(Rig, Owner.Get(), 100);
	if (!Object)
	{
		return false;
	}
	TSharedRef<TArray<FString>> Heard = MakeShared<TArray<FString>>();
	Object->WatchValues(FOnCrowdyServerValuesChanged::FDelegate::CreateLambda([Heard](UCrowdyServerObject*, TConstArrayView<FString> Fields) { Heard->Append(Fields.GetData(), Fields.Num()); }));
	Rig.PumpFor(0.1);
	Heard->Reset();
	Rig.Sent.Reset();
	Rig.Session->SetUserID(Rival);
	Rig.PushTopic(PlayerTopic, Profile(Rig, 2, 4));
	FSentFrame Reread;
	if (!TestTrue(TEXT("a snapshot after another player signed in reads the State again"), Rig.WaitForFrame(KindCall, TEXT("snapshot"), Reread)))
	{
		return false;
	}
	TestEqual(TEXT("keeping the last player's wallet meanwhile out of the new snapshot"), PlayerOf(*Object)->Level, 3);
	Rig.Reply(Reread.Rid, ECrowdyNativeExecStatus::Ok, Profile(Rig, 2, 4));
	TestTrue(TEXT("whose read drops the last player's wallet"), Rig.PumpUntil([Object]() { return PlayerOf(*Object)->Wallet.Balances.IsEmpty() && PlayerOf(*Object)->Level == 4; }));
	TestTrue(TEXT("announcing it"), Heard->Contains(TEXT("wallet")));
	TestFalse(TEXT("and reads none for a player whose hub this is not"), Rig.SentAny(KindCall, TEXT("wallet")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterOneFailingOwnerValueKeepsItsReason,
	"CrowdySDK.CrowdyExec.GameKitStarter.OneFailingOwnerValueKeepsItsReason", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterOneFailingOwnerValueKeepsItsReason::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	if (!Rig.IsValid() || !MakeKitPlayer(Rig, true))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquirePlayer(Rig, Owner.Get(), Rig.InstanceId);
	FSentFrame WalletRead;
	FSentFrame InventoryRead;
	if (!Object || !Rig.WaitForFrame(KindCall, TEXT("wallet"), WalletRead) || !Rig.WaitForFrame(KindCall, TEXT("inventory"), InventoryRead))
	{
		return false;
	}
	Rig.Reply(WalletRead.Rid, ECrowdyNativeExecStatus::Unavailable, Utf8Bytes(TEXT("the hub is moving")));
	Rig.PumpFor(0.2);
	const FString Reason = Object->GetFailureReason();
	TestFalse(TEXT("an Owner Value that cannot be read says why"), Reason.IsEmpty());
	Rig.Reply(InventoryRead.Rid, ECrowdyNativeExecStatus::Ok, Rig.Encode(FCrowdyKitInventory()));
	Rig.PumpFor(0.2);
	TestEqual(TEXT("and another one read meanwhile does not clear it"), Object->GetFailureReason(), Reason);
	TestTrue(TEXT("nor makes it Ready"), Object->GetStatus() == ECrowdyServerObjectStatus::Connecting);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterOwnerValueChecks,
	"CrowdySDK.CrowdyExec.GameKitStarter.OwnerValueChecks", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterOwnerValueChecks::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	if (!Rig.IsValid() || !MakeKitPlayer(Rig, true))
	{
		return false;
	}
	UCrowdyServerObjectDefinition& Player = *Rig.Definition;
	TArray<FString> Errors;
	Player.KitOwnerValues.Add({TEXT("progress"), TEXT("progress"), TEXT("Prgress")});
	TestFalse(TEXT("an Owner Value of no State variable does not bake"), Player.Bake(Errors));
	TestTrue(FString::Printf(TEXT("naming it (%s)"), *FString::Join(Errors, TEXT("; "))), Errors.ContainsByPredicate([](const FString& Each) { return Each.Contains(TEXT("Prgress is not a variable")); }));
	Player.KitOwnerValues.Pop();
	Errors.Reset();
	Player.KitOwnerValues.Add({TEXT("Progress"), TEXT("progress"), TEXT("Progress")});
	TestFalse(TEXT("nor one whose method is not a server name"), Player.Bake(Errors));
	Player.KitOwnerValues.Pop();
	Errors.Reset();
	Player.KitStateTopic.Reset();
	TestFalse(TEXT("nor Owner Values without a State Topic"), Player.Bake(Errors));
	TestTrue(TEXT("saying they need one"), Errors.ContainsByPredicate([](const FString& Each) { return Each.Contains(TEXT("Owner Values need a State Topic")); }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterHasFlagTracksAnOptionalIndex,
	"CrowdySDK.CrowdyExec.GameKitStarter.HasFlagTracksAnOptionalIndex", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterHasFlagTracksAnOptionalIndex::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Kit(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
	Kit->TypeName = TEXT("waves");
	Kit->CodeSource = ECrowdyServerCodeSource::GameKitStarter;
	Kit->KitStateTopic = TEXT("waves");
	Kit->State = FCrowdyExecTestWaveState::StaticStruct();
	Kit->Functions.AddDefaulted_GetRef().Name = TEXT("Ready");
	TArray<FString> Errors;
	if (!TestTrue(FString::Printf(TEXT("the starter bakes (%s)"), *FString::Join(Errors, TEXT("; "))), Kit->Bake(Errors)))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Own(DuplicateObject(Kit.Get(), GetTransientPackage()));
	Own->CodeSource = ECrowdyServerCodeSource::Generated;
	Own->WatchedFields = {TEXT("Wave"), TEXT("bHasWave")};
	if (!TestTrue(FString::Printf(TEXT("its own-type twin bakes (%s)"), *FString::Join(Errors, TEXT("; "))), Own->Bake(Errors)))
	{
		return false;
	}

	const UScriptStruct& Struct = *FCrowdyExecTestWaveState::StaticStruct();
	const FCrowdyExecBakedStruct* Baked = Kit->BakedStructs.FindByPredicate([&Struct](const FCrowdyExecBakedStruct& Each) { return Each.Struct == &Struct; });
	TestTrue(TEXT("a presence flag is no baked field: only wave and round travel"), Baked && Baked->Fields.Num() == 2
		&& !Baked->Fields.ContainsByPredicate([](const FCrowdyExecBakedField& Field) { return Field.ServerName.Contains(TEXT("has"), ESearchCase::IgnoreCase); }));
	TestTrue(TEXT("Wave's baked field names its flag"), Baked && Baked->Fields.ContainsByPredicate([](const FCrowdyExecBakedField& Field)
	{
		return Field.ServerName == TEXT("wave") && Field.PresenceProperty == FName(TEXT("bHasWave"));
	}));
	TestNotNull(TEXT("Wave travels as wave"), CrowdyExec::FindResolvedPropertyForTest(*Kit, Struct, TEXT("wave")));
	TestNull(TEXT("the flag has no server name"), CrowdyExec::FindResolvedPropertyForTest(*Kit, Struct, TEXT("has_wave")));
	const FCrowdyExecBakedStruct* OwnBaked = Own->BakedStructs.FindByPredicate([&Struct](const FCrowdyExecBakedStruct& Each) { return Each.Struct == &Struct; });
	TestTrue(TEXT("an own type's bHasWave is an ordinary field"), OwnBaked && OwnBaked->Fields.Num() == 3
		&& CrowdyExec::FindResolvedPropertyForTest(*Own, Struct, TEXT("bHasWave")));

	// A snapshot is a whole State, so what an earlier one left in Out must not survive it.
	auto Read = [this, &Kit](int32 Mode, int32 Wave, FCrowdyExecTestWaveState& Out)
	{
		TArray<uint8> Body;
		FCrowdyExecWriter Writer(Body);
		Writer.MapHeader(Mode == 0 ? 1 : 2);
		WriteKey(Writer, "round");
		Writer.UInt(2);
		if (Mode != 0)
		{
			WriteKey(Writer, "wave");
		}
		if (Mode == 1)
		{
			Writer.UInt(Wave);
		}
		else if (Mode == 2)
		{
			Writer.Nil();
		}
		Out.Wave = 99;
		Out.bHasWave = Mode != 1;
		FCrowdyExecStateHeader Header;
		FString Error;
		return TestTrue(FString::Printf(TEXT("the snapshot decodes (%s)"), *Error),
			CrowdyExec::DecodeKitSnapshot(*Kit, KitMessage(Epoch, 1, "snapshot", Body), &Out, Header, Error));
	};
	FCrowdyExecTestWaveState State;
	if (!Read(1, 0, State))
	{
		return false;
	}
	TestTrue(TEXT("wave 0 is a value: Wave 0 and bHasWave"), State.Wave == 0 && State.bHasWave);
	if (!Read(2, 0, State))
	{
		return false;
	}
	TestTrue(TEXT("a nil wave is no value: Wave is its default and bHasWave is false"), State.Wave == -1 && !State.bHasWave);
	if (!Read(1, 3, State))
	{
		return false;
	}
	TestTrue(TEXT("wave 3 is Wave 3 and bHasWave"), State.Wave == 3 && State.bHasWave);
	if (!Read(0, 0, State))
	{
		return false;
	}
	TestTrue(TEXT("a snapshot without wave is no value either"), State.Wave == -1 && !State.bHasWave && State.Round == 2);

	FCrowdyExecTestWaveState Sent;
	Sent.Wave = 5;
	Sent.bHasWave = true;
	TArray<uint8> Bytes;
	FString Error;
	TestTrue(TEXT("a state with a value encodes"), CrowdyExec::Encode(*Kit, &Struct, &Sent, Bytes, Error));
	TestFalse(TEXT("without the flag"), Contains(Bytes, "has_wave") || Contains(Bytes, "bHasWave"));
	FCrowdyExecTestWaveState Back;
	TestTrue(TEXT("and reads back as it was sent"), CrowdyExec::Decode(*Kit, Struct, Bytes, &Back, Error) && Back.Wave == 5 && Back.bHasWave);
	Sent.bHasWave = false;
	TestTrue(TEXT("a state without one encodes"), CrowdyExec::Encode(*Kit, &Struct, &Sent, Bytes, Error));
	TestTrue(TEXT("and sends wave as nil, whatever Wave holds"), CrowdyExec::Decode(*Kit, Struct, Bytes, &Back, Error) && Back.Wave == -1 && !Back.bHasWave);

	TArray<uint8> OwnBytes;
	FCrowdyExecWriter OwnWriter(OwnBytes);
	OwnWriter.MapHeader(2);
	WriteKey(OwnWriter, "Wave");
	OwnWriter.UInt(3);
	WriteKey(OwnWriter, "bHasWave");
	OwnWriter.Bool(false);
	FCrowdyExecTestWaveState OwnRead;
	OwnRead.bHasWave = true;
	TestTrue(TEXT("an own type reads bHasWave as it comes, not from Wave"), CrowdyExec::Decode(*Own, Struct, OwnBytes, &OwnRead, Error) && OwnRead.Wave == 3 && !OwnRead.bHasWave);
	TestTrue(TEXT("and writes it"), CrowdyExec::Encode(*Own, &Struct, &Sent, OwnBytes, Error) && Contains(OwnBytes, "bHasWave"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterHasFlagChangeIsAChange,
	"CrowdySDK.CrowdyExec.GameKitStarter.HasFlagChangeIsAChange", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterHasFlagChangeIsAChange::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	UCrowdyServerObjectDefinition& Waves = *Rig.Definition;
	Waves.CodeSource = ECrowdyServerCodeSource::GameKitStarter;
	Waves.KitStateTopic = TEXT("waves");
	Waves.State = FCrowdyExecTestWaveState::StaticStruct();
	Waves.WatchedFields.Reset();
	Waves.Functions.Reset();
	TArray<FString> Errors;
	if (!Rig.IsValid() || !TestTrue(FString::Printf(TEXT("the starter bakes (%s)"), *FString::Join(Errors, TEXT("; "))), Waves.Bake(Errors)))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	FSentFrame Subscribe;
	FSentFrame Read;
	if (!Object || !Rig.Open() || !Rig.WaitForFrame(KindSubscribe, TEXT("waves"), Subscribe) || !Rig.WaitForFrame(KindCall, TEXT("snapshot"), Read))
	{
		return false;
	}
	FCrowdyExecTestWaveState First;
	First.Round = 1;
	Rig.Reply(Subscribe.Rid, ECrowdyNativeExecStatus::Ok, {});
	Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::Ok, KitMessage(Epoch, 1, "snapshot", Rig.Encode(First)));
	if (!TestTrue(TEXT("the first snapshot makes the starter Ready"), Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; })))
	{
		return false;
	}
	FValuesSpy Spy;
	Spy.Watch(Object);

	// Wave holds -1 either way, so only its flag differs between snapshots; Round tells each one arrived.
	uint64 Seq = 1;
	auto Push = [this, &Rig, &Spy, Object, &Seq](bool bHasWave, bool bWaveChanged, const TCHAR* What)
	{
		FCrowdyExecTestWaveState Next;
		Next.bHasWave = bHasWave;
		Next.Round = static_cast<int32>(++Seq);
		const int32 Before = Spy.Num();
		Rig.PushTopic(TEXT("waves"), KitMessage(Epoch, Seq, "snapshot", Rig.Encode(Next)));
		const int32 Round = Next.Round;
		const bool bArrived = Rig.PumpUntil([Object, Round]() { const FCrowdyExecTestWaveState* Now = Object->GetState().GetPtr<FCrowdyExecTestWaveState>(); return Now && Now->Round == Round; });
		const bool bNamed = Spy.Num() == Before + 1 && Spy.Last().Contains(TEXT("wave"));
		return TestTrue(FString::Printf(TEXT("%s arrives"), What), bArrived) && TestEqual(FString::Printf(TEXT("and %s reports wave changed or not"), What), bNamed, bWaveChanged);
	};
	if (!Push(true, true, TEXT("a wave appearing as the value it was already holding"))
		|| !Push(true, false, TEXT("the same wave again"))
		|| !Push(false, true, TEXT("a wave going back to nil"))
		|| !Push(false, false, TEXT("nil again")))
	{
		return false;
	}
	return true;
}

namespace CrowdyServerObjectKitTests
{
	inline constexpr const TCHAR* DeckTopic = TEXT("table");

	/** Shapes Deck like the deck starter: the hand an Owner Value, and the cards read from the hub's face keys beside the text it also sends. */
	void ShapeDeckKit(UCrowdyServerObjectDefinition& Deck)
	{
		UScriptStruct* Hand = FCrowdyKitDeckHand::StaticStruct();
		UEnum* Suit = StaticEnum<ECrowdyKitCardSuit>();
		Deck.CodeSource = ECrowdyServerCodeSource::GameKitStarter;
		Deck.StateForm = ECrowdyServerValuesForm::Struct;
		Deck.KitStateTopic = DeckTopic;
		Deck.State = FCrowdyKitDeckState::StaticStruct();
		Deck.WatchedFields.Reset();
		Deck.Messages.Reset();
		Deck.KitOwnerValues = {{TEXT("hand"), TEXT("hand"), TEXT("Hand")}};
		Deck.FieldNames = {
			{Hand, GET_MEMBER_NAME_CHECKED(FCrowdyKitDeckHand, Cards), TEXT("faces")},
			{Hand, GET_MEMBER_NAME_CHECKED(FCrowdyKitDeckHand, Playable), TEXT("playable_faces")},
			{FCrowdyKitDeckState::StaticStruct(), GET_MEMBER_NAME_CHECKED(FCrowdyKitDeckState, Top), TEXT("top_face")},
			{FCrowdyKitDeckPlayed::StaticStruct(), GET_MEMBER_NAME_CHECKED(FCrowdyKitDeckPlayed, Card), TEXT("face")},
		};
		Deck.EnumValueNames = {{Suit, TEXT("Spades"), TEXT("S")}, {Suit, TEXT("Hearts"), TEXT("H")}, {Suit, TEXT("Diamonds"), TEXT("D")}, {Suit, TEXT("Clubs"), TEXT("C")}};
		Deck.Functions = {
			KitFunction(TEXT("Play"), TEXT("play"), FCrowdyKitDeckPlayInputs::StaticStruct(), FCrowdyKitDeckPlayReply::StaticStruct(), true),
			KitFunction(TEXT("GetHand"), TEXT("hand"), nullptr, Hand, false),
		};
	}

	/** Makes the rig's type a starter shaped like deck, with the signed-in player Me. */
	bool MakeKitDeck(FRig& Rig)
	{
		ShapeDeckKit(*Rig.Definition);
		Rig.Session->SetUserID(Me);
		TArray<FString> Errors;
		const bool bBaked = Rig.Definition->Bake(Errors);
		return Rig.Test.TestTrue(FString::Printf(TEXT("the deck starter bakes (%s)"), *FString::Join(Errors, TEXT("; "))), bBaked);
	}

	void WriteDeckFace(FCrowdyExecWriter& Writer, const ANSICHAR* Id, int32 Rank, const ANSICHAR* Suit)
	{
		Writer.MapHeader(3);
		WriteKey(Writer, "id");
		WriteText(Writer, Id);
		WriteKey(Writer, "rank");
		Writer.UInt(Rank);
		WriteKey(Writer, "suit");
		WriteText(Writer, Suit);
	}

	/** A deck table as the hub sends it, each card as text and as {id, rank, suit}. A lobby has no top card, suit, turn or last move. */
	TArray<uint8> DeckTableBody(bool bLobby)
	{
		TArray<uint8> Bytes;
		FCrowdyExecWriter Writer(Bytes);
		Writer.MapHeader(13);
		WriteKey(Writer, "phase");
		WriteText(Writer, bLobby ? "lobby" : "playing");
		WriteKey(Writer, "players");
		Writer.ArrayHeader(2);
		Writer.UInt(Me);
		Writer.UInt(Rival);
		WriteKey(Writer, "host");
		Writer.UInt(Me);
		WriteKey(Writer, "hand_counts");
		Writer.MapHeader(2);
		Writer.UInt(Me);
		Writer.UInt(bLobby ? 0 : 4);
		Writer.UInt(Rival);
		Writer.UInt(bLobby ? 0 : 5);
		WriteKey(Writer, "top");
		if (bLobby)
		{
			Writer.Nil();
		}
		else
		{
			WriteText(Writer, "9H");
		}
		WriteKey(Writer, "top_face");
		if (bLobby)
		{
			Writer.Nil();
		}
		else
		{
			WriteDeckFace(Writer, "9H", 9, "H");
		}
		WriteKey(Writer, "suit");
		if (bLobby)
		{
			Writer.Nil();
		}
		else
		{
			WriteText(Writer, "H");
		}
		WriteKey(Writer, "draw_count");
		Writer.UInt(bLobby ? 0 : 41);
		WriteKey(Writer, "turn");
		if (bLobby)
		{
			Writer.Nil();
		}
		else
		{
			Writer.UInt(Me);
		}
		WriteKey(Writer, "turn_ends");
		if (bLobby)
		{
			Writer.Nil();
		}
		else
		{
			Writer.UInt(Epoch + 30000);
		}
		WriteKey(Writer, "last");
		if (bLobby)
		{
			Writer.Nil();
		}
		else
		{
			Writer.MapHeader(4);
			WriteKey(Writer, "player");
			Writer.UInt(Rival);
			WriteKey(Writer, "card");
			WriteText(Writer, "9H");
			WriteKey(Writer, "action");
			WriteText(Writer, "play");
			WriteKey(Writer, "face");
			WriteDeckFace(Writer, "9H", 9, "H");
		}
		WriteKey(Writer, "winner");
		Writer.Nil();
		WriteKey(Writer, "scores");
		Writer.MapHeader(2);
		Writer.UInt(Me);
		Writer.Int(0);
		Writer.UInt(Rival);
		Writer.Int(0);
		return Bytes;
	}

	/** The signed-in player's hand as the hub answers it: the text lists, then the same cards as faces. Holds 8C and 9S, of which only 8C may go. */
	TArray<uint8> DeckHandBody(bool bTheirTurn)
	{
		TArray<uint8> Bytes;
		FCrowdyExecWriter Writer(Bytes);
		Writer.MapHeader(5);
		WriteKey(Writer, "cards");
		Writer.ArrayHeader(2);
		WriteText(Writer, "8C");
		WriteText(Writer, "9S");
		WriteKey(Writer, "playable");
		Writer.ArrayHeader(bTheirTurn ? 1 : 0);
		if (bTheirTurn)
		{
			WriteText(Writer, "8C");
		}
		WriteKey(Writer, "drawn");
		Writer.Bool(false);
		WriteKey(Writer, "faces");
		Writer.ArrayHeader(2);
		WriteDeckFace(Writer, "8C", 8, "C");
		WriteDeckFace(Writer, "9S", 9, "S");
		WriteKey(Writer, "playable_faces");
		Writer.ArrayHeader(bTheirTurn ? 1 : 0);
		if (bTheirTurn)
		{
			WriteDeckFace(Writer, "8C", 8, "C");
		}
		return Bytes;
	}

	TStrongObjectPtr<UCrowdyServerObjectDefinition> MakeDeckDefinition(FAutomationTestBase& Test)
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Deck(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
		Deck->TypeName = TEXT("deck");
		ShapeDeckKit(*Deck);
		TArray<FString> Errors;
		const bool bBaked = Deck->Bake(Errors);
		if (!Test.TestTrue(FString::Printf(TEXT("the deck starter bakes (%s)"), *FString::Join(Errors, TEXT("; "))), bBaked))
		{
			Deck.Reset();
		}
		return Deck;
	}

	const FCrowdyKitDeckState* DeckOf(const UCrowdyServerObject& Object)
	{
		return Object.GetState().GetPtr<FCrowdyKitDeckState>();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterDeckTableReadsTheFaceKeys,
	"CrowdySDK.CrowdyExec.GameKitStarter.DeckTableReadsTheFaceKeys", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterDeckTableReadsTheFaceKeys::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Deck = MakeDeckDefinition(*this);
	if (!Deck)
	{
		return false;
	}
	FCrowdyKitDeckState Table;
	FString Error;
	if (!TestTrue(FString::Printf(TEXT("a table sent with both the text and the face keys decodes (%s)"), *Error),
		CrowdyExec::Decode(*Deck, *FCrowdyKitDeckState::StaticStruct(), DeckTableBody(false), &Table, Error)))
	{
		return false;
	}
	TestTrue(TEXT("the phase, players and host read by name"), Table.Phase == ECrowdyKitDeckPhase::Playing && Table.Players == TArray<int64>({Me, Rival}) && Table.Host == Me);
	TestTrue(TEXT("the top card reads from top_face, not the text top"), Table.Top.Id == TEXT("9H") && Table.Top.Rank == 9 && Table.Top.Suit == ECrowdyKitCardSuit::Hearts);
	TestTrue(TEXT("the suit in play is Hearts, and present"), Table.Suit == ECrowdyKitCardSuit::Hearts && Table.bHasSuit);
	TestTrue(TEXT("each player's count of cards is public"), Table.HandCounts.FindRef(Me) == 4 && Table.HandCounts.FindRef(Rival) == 5);
	TestTrue(TEXT("the draw pile and the turn read"), Table.DrawCount == 41 && Table.Turn == Me && Table.TurnEnds == static_cast<int64>(Epoch + 30000));
	TestTrue(TEXT("the last move names its player, action and the card from face"), Table.Last.Player == Rival && Table.Last.Action == ECrowdyKitDeckAction::Play
		&& Table.Last.Card.Id == TEXT("9H") && Table.Last.Card.Rank == 9 && Table.Last.Card.Suit == ECrowdyKitCardSuit::Hearts);
	TestEqual(TEXT("no player has won yet"), Table.Winner, int64(0));
	TestTrue(TEXT("and the hand, which no snapshot carries, stays empty"), Table.Hand.Cards.IsEmpty() && Table.Hand.Playable.IsEmpty());

	FCrowdyKitDeckState Lobby;
	Lobby.bHasSuit = true;
	TestTrue(FString::Printf(TEXT("a lobby with a nil suit, top card and last move decodes (%s)"), *Error),
		CrowdyExec::Decode(*Deck, *FCrowdyKitDeckState::StaticStruct(), DeckTableBody(true), &Lobby, Error));
	TestTrue(TEXT("the lobby has no suit in play"), !Lobby.bHasSuit);
	TestTrue(TEXT("no top card"), Lobby.Top.Id.IsEmpty() && Lobby.Top.Rank == 0);
	TestTrue(TEXT("no last move"), Lobby.Last.Player == 0 && Lobby.Last.Card.Id.IsEmpty());
	TestTrue(TEXT("and nobody's turn"), Lobby.Phase == ECrowdyKitDeckPhase::Lobby && Lobby.Turn == 0 && Lobby.TurnEnds == 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterDeckHandAndPlayUseFaces,
	"CrowdySDK.CrowdyExec.GameKitStarter.DeckHandAndPlayUseFaces", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterDeckHandAndPlayUseFaces::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Deck = MakeDeckDefinition(*this);
	if (!Deck)
	{
		return false;
	}
	FCrowdyKitDeckHand Hand;
	FString Error;
	if (!TestTrue(FString::Printf(TEXT("a hand sent with both the text and the face lists decodes (%s)"), *Error),
		CrowdyExec::Decode(*Deck, *FCrowdyKitDeckHand::StaticStruct(), DeckHandBody(true), &Hand, Error)))
	{
		return false;
	}
	TestTrue(TEXT("the cards read from faces, in order"), Hand.Cards.Num() == 2 && Hand.Cards[0].Id == TEXT("8C") && Hand.Cards[0].Rank == 8 && Hand.Cards[0].Suit == ECrowdyKitCardSuit::Clubs
		&& Hand.Cards[1].Id == TEXT("9S") && Hand.Cards[1].Rank == 9 && Hand.Cards[1].Suit == ECrowdyKitCardSuit::Spades);
	TestTrue(TEXT("the playable cards from playable_faces"), Hand.Playable.Num() == 1 && Hand.Playable[0].Id == TEXT("8C"));
	TestFalse(TEXT("and a hand that has not drawn says so"), Hand.bDrawn);

	FCrowdyKitDeckHand Waiting;
	TestTrue(FString::Printf(TEXT("a hand on another player's turn decodes (%s)"), *Error),
		CrowdyExec::Decode(*Deck, *FCrowdyKitDeckHand::StaticStruct(), DeckHandBody(false), &Waiting, Error) && Waiting.Cards.Num() == 2 && Waiting.Playable.IsEmpty());

	auto Expected = [](bool bWithSuit)
	{
		TArray<uint8> Bytes;
		FCrowdyExecWriter Writer(Bytes);
		Writer.MapHeader(2);
		WriteKey(Writer, "card");
		WriteText(Writer, "8C");
		WriteKey(Writer, "suit");
		if (bWithSuit)
		{
			WriteText(Writer, "S");
		}
		else
		{
			Writer.Nil();
		}
		return Bytes;
	};
	FCrowdyKitDeckPlayInputs Eight;
	Eight.Card = TEXT("8C");
	Eight.Suit = ECrowdyKitCardSuit::Spades;
	Eight.bHasSuit = true;
	TArray<uint8> Sent;
	TestTrue(FString::Printf(TEXT("an eight that names a suit encodes (%s)"), *Error), CrowdyExec::Encode(*Deck, FCrowdyKitDeckPlayInputs::StaticStruct(), &Eight, Sent, Error));
	TestTrue(TEXT("as the card and the suit's letter"), Sent == Expected(true));
	Eight.bHasSuit = false;
	Sent.Reset();
	TestTrue(FString::Printf(TEXT("the same play without Has Suit encodes (%s)"), *Error), CrowdyExec::Encode(*Deck, FCrowdyKitDeckPlayInputs::StaticStruct(), &Eight, Sent, Error));
	TestTrue(TEXT("with a nil suit"), Sent == Expected(false));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterDeckHandIsAnOwnerValue,
	"CrowdySDK.CrowdyExec.GameKitStarter.DeckHandIsAnOwnerValue", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterDeckHandIsAnOwnerValue::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	if (!Rig.IsValid() || !MakeKitDeck(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	FSentFrame Subscribe;
	FSentFrame Read;
	FSentFrame HandRead;
	if (!Object || !Rig.Open() || !Rig.WaitForFrame(KindSubscribe, DeckTopic, Subscribe) || !Rig.WaitForFrame(KindCall, TEXT("snapshot"), Read))
	{
		return false;
	}
	Rig.Reply(Subscribe.Rid, ECrowdyNativeExecStatus::Ok, {});
	Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::Ok, KitMessage(Epoch, 1, "snapshot", DeckTableBody(false)));
	if (!TestTrue(TEXT("the hand is read by its own call after the table"), Rig.WaitForFrame(KindCall, TEXT("hand"), HandRead)))
	{
		return false;
	}
	Rig.Reply(HandRead.Rid, ECrowdyNativeExecStatus::Ok, DeckHandBody(true));
	if (!TestTrue(TEXT("the hand makes the table Ready"), Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; })))
	{
		return false;
	}
	const FCrowdyKitDeckState* Table = DeckOf(*Object);
	if (!TestNotNull(TEXT("the table is a deck table"), Table))
	{
		return false;
	}
	TestTrue(TEXT("its State holds the public table"), Table->Phase == ECrowdyKitDeckPhase::Playing && Table->Top.Rank == 9 && Table->bHasSuit);
	TestTrue(TEXT("and the signed-in player's own cards, with the one that may go"), Table->Hand.Cards.Num() == 2 && Table->Hand.Playable.Num() == 1 && Table->Hand.Playable[0].Id == TEXT("8C"));

	PushChanged(Rig, DeckTopic, Changed(2, {Rival}, "hand"));
	TestFalse(TEXT("a change to another player's hand reads nothing"), Rig.SentAny(KindCall, TEXT("hand")));
	PushChanged(Rig, DeckTopic, Changed(3, {Me}, "hand"));
	FSentFrame Again;
	if (!TestTrue(TEXT("a change named for this player reads the hand again"), Rig.WaitForFrame(KindCall, TEXT("hand"), Again)))
	{
		return false;
	}
	Rig.Reply(Again.Rid, ECrowdyNativeExecStatus::Ok, DeckHandBody(false));
	TestTrue(TEXT("the hand when the turn ended has nothing playable"), Rig.PumpUntil([Object]() { return DeckOf(*Object)->Hand.Playable.IsEmpty(); }));

	Rig.PushTopic(DeckTopic, KitMessage(Epoch, 4, "snapshot", DeckTableBody(true)));
	TestTrue(TEXT("a pushed table arrives"), Rig.PumpUntil([Object]() { return DeckOf(*Object)->Phase == ECrowdyKitDeckPhase::Lobby; }));
	TestEqual(TEXT("and keeps the hand, which no snapshot carries"), DeckOf(*Object)->Hand.Cards.Num(), 2);
	return true;
}

namespace CrowdyServerObjectKitTests
{
	/** Shapes Arena like the arena starter: positions in metres, the hero class sent as class, and its events as one message. */
	TStrongObjectPtr<UCrowdyServerObjectDefinition> MakeArenaDefinition(FAutomationTestBase& Test)
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Arena(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
		Arena->TypeName = TEXT("arena");
		Arena->CodeSource = ECrowdyServerCodeSource::GameKitStarter;
		Arena->StateForm = ECrowdyServerValuesForm::Struct;
		Arena->KitStateTopic = TEXT("arena");
		Arena->State = FCrowdyKitArenaState::StaticStruct();
		Arena->WatchedFields.Reset();
		FCrowdyServerMessage& Event = Arena->Messages.AddDefaulted_GetRef();
		Event.Name = TEXT("ArenaEvent");
		Event.ServerName = TEXT("arena");
		Event.Payload = FCrowdyKitArenaEvent::StaticStruct();
		Arena->FieldNames = {
			{FCrowdyKitArenaFighter::StaticStruct(), GET_MEMBER_NAME_CHECKED(FCrowdyKitArenaFighter, HeroClass), TEXT("class")},
			{FCrowdyKitArenaJoinInputs::StaticStruct(), GET_MEMBER_NAME_CHECKED(FCrowdyKitArenaJoinInputs, HeroClass), TEXT("class")},
		};
		Arena->KitPositionFunction = TEXT("Move");
		Arena->Functions = {
			KitFunction(TEXT("Join"), TEXT("join"), FCrowdyKitArenaJoinInputs::StaticStruct(), nullptr, true),
			KitFunction(TEXT("Move"), TEXT("move"), FCrowdyKitArenaMoveInputs::StaticStruct(), FCrowdyKitGroundPosition::StaticStruct(), false),
			KitFunction(TEXT("Cast"), TEXT("cast"), FCrowdyKitArenaCastInputs::StaticStruct(), FCrowdyKitArenaCastReply::StaticStruct(), true),
		};
		TArray<FString> Errors;
		const bool bBaked = Arena->Bake(Errors);
		if (!Test.TestTrue(FString::Printf(TEXT("the arena starter bakes (%s)"), *FString::Join(Errors, TEXT("; "))), bBaked))
		{
			Arena.Reset();
		}
		return Arena;
	}

	void WriteArenaPoint(FCrowdyExecWriter& Writer, double X, double Y)
	{
		Writer.ArrayHeader(2);
		Writer.Double(X);
		Writer.Double(Y);
	}

	void WriteArenaFighter(FCrowdyExecWriter& Writer, int64 Player, const ANSICHAR* HeroClass, double X, double Y, int64 Hp, int64 MaxHp, bool bDown)
	{
		Writer.MapHeader(11);
		WriteKey(Writer, "player");
		Writer.UInt(Player);
		WriteKey(Writer, "class");
		WriteText(Writer, HeroClass);
		WriteKey(Writer, "x");
		Writer.Double(X);
		WriteKey(Writer, "y");
		Writer.Double(Y);
		WriteKey(Writer, "hp");
		Writer.UInt(Hp);
		WriteKey(Writer, "max_hp");
		Writer.UInt(MaxHp);
		WriteKey(Writer, "resource");
		Writer.UInt(bDown ? 0 : 40);
		WriteKey(Writer, "statuses");
		Writer.ArrayHeader(bDown ? 0 : 1);
		if (!bDown)
		{
			WriteText(Writer, "slow");
		}
		WriteKey(Writer, "kills");
		Writer.UInt(bDown ? 0 : 2);
		WriteKey(Writer, "deaths");
		Writer.UInt(bDown ? 3 : 1);
		WriteKey(Writer, "down");
		Writer.Bool(bDown);
	}

	/** A running arena as the hub sends it: two fighters (the second down), a shot, a blast, no winner. */
	TArray<uint8> ArenaStateBody()
	{
		TArray<uint8> Bytes;
		FCrowdyExecWriter Writer(Bytes);
		Writer.MapHeader(10);
		WriteKey(Writer, "phase");
		WriteText(Writer, "running");
		WriteKey(Writer, "origin");
		WriteArenaPoint(Writer, 10.0, 20.0);
		WriteKey(Writer, "width");
		Writer.Double(40.0);
		WriteKey(Writer, "depth");
		Writer.Double(30.0);
		WriteKey(Writer, "fighters");
		Writer.ArrayHeader(2);
		WriteArenaFighter(Writer, Me, "warrior", 12.5, 30.25, 80, 100, false);
		WriteArenaFighter(Writer, Rival, "mage", 31.0, 20.0, 0, 70, true);
		WriteKey(Writer, "projectiles");
		Writer.ArrayHeader(1);
		Writer.MapHeader(4);
		WriteKey(Writer, "ability");
		WriteText(Writer, "bolt");
		WriteKey(Writer, "caster");
		Writer.UInt(Me);
		WriteKey(Writer, "x");
		Writer.Double(15.5);
		WriteKey(Writer, "y");
		Writer.Double(25.0);
		WriteKey(Writer, "areas");
		Writer.ArrayHeader(1);
		Writer.MapHeader(6);
		WriteKey(Writer, "ability");
		WriteText(Writer, "quake");
		WriteKey(Writer, "caster");
		Writer.UInt(Rival);
		WriteKey(Writer, "x");
		Writer.Double(20.0);
		WriteKey(Writer, "y");
		Writer.Double(22.5);
		WriteKey(Writer, "radius");
		Writer.Double(4.0);
		WriteKey(Writer, "at");
		Writer.UInt(Epoch + 1500);
		WriteKey(Writer, "kill_target");
		Writer.UInt(10);
		WriteKey(Writer, "ends_at");
		Writer.UInt(Epoch + 60000);
		WriteKey(Writer, "winner");
		Writer.Nil();
		return Bytes;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterArenaStateReadsMetres,
	"CrowdySDK.CrowdyExec.GameKitStarter.ArenaStateReadsMetres", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterArenaStateReadsMetres::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Arena = MakeArenaDefinition(*this);
	if (!Arena)
	{
		return false;
	}
	FCrowdyKitArenaState Fight;
	FString Error;
	if (!TestTrue(FString::Printf(TEXT("a running arena decodes (%s)"), *Error), CrowdyExec::Decode(*Arena, *FCrowdyKitArenaState::StaticStruct(), ArenaStateBody(), &Fight, Error)))
	{
		return false;
	}
	TestTrue(TEXT("the phase and size read by name"), Fight.Phase == ECrowdyKitArenaPhase::Running && Fight.Width == 40.0 && Fight.Depth == 30.0 && Fight.KillTarget == 10);
	TestTrue(TEXT("the corner reads from its [x, y]"), Fight.Origin.X == 10.0 && Fight.Origin.Y == 20.0);
	TestTrue(TEXT("the end is a time, and nil winner is no winner"), Fight.EndsAt == static_cast<int64>(Epoch + 60000) && Fight.Winner == 0);
	if (!TestEqual(TEXT("two fighters"), Fight.Fighters.Num(), 2))
	{
		return false;
	}
	const FCrowdyKitArenaFighter& First = Fight.Fighters[0];
	TestTrue(TEXT("a fighter's class reads from class, with the metres it stands at"), First.Player == Me && First.HeroClass == TEXT("warrior") && First.X == 12.5 && First.Y == 30.25);
	TestTrue(TEXT("and its health, resource, effects and record"), First.Hp == 80 && First.MaxHp == 100 && First.Resource == 40 && First.Statuses == TArray<FString>({TEXT("slow")})
		&& First.Kills == 2 && First.Deaths == 1 && !First.bDown);
	const FCrowdyKitArenaFighter& Second = Fight.Fighters[1];
	TestTrue(TEXT("a fighter who is down says so"), Second.Player == Rival && Second.HeroClass == TEXT("mage") && Second.bDown && Second.Hp == 0 && Second.Statuses.IsEmpty());
	TestTrue(TEXT("a projectile has its caster and place"), Fight.Projectiles.Num() == 1 && Fight.Projectiles[0].Ability == TEXT("bolt") && Fight.Projectiles[0].Caster == Me
		&& Fight.Projectiles[0].X == 15.5 && Fight.Projectiles[0].Y == 25.0);
	TestTrue(TEXT("an area has its place, radius and landing time"), Fight.Areas.Num() == 1 && Fight.Areas[0].Ability == TEXT("quake") && Fight.Areas[0].Caster == Rival
		&& Fight.Areas[0].X == 20.0 && Fight.Areas[0].Y == 22.5 && Fight.Areas[0].Radius == 4.0 && Fight.Areas[0].At == static_cast<int64>(Epoch + 1500));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterArenaEventsDecode,
	"CrowdySDK.CrowdyExec.GameKitStarter.ArenaEventsDecode", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterArenaEventsDecode::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Arena = MakeArenaDefinition(*this);
	if (!Arena)
	{
		return false;
	}
	TArray<uint8> Kill;
	FCrowdyExecWriter KillWriter(Kill);
	KillWriter.MapHeader(3);
	WriteKey(KillWriter, "type");
	WriteText(KillWriter, "kill");
	WriteKey(KillWriter, "killer");
	KillWriter.UInt(Me);
	WriteKey(KillWriter, "victim");
	KillWriter.UInt(Rival);
	FCrowdyKitArenaEvent Event;
	FString Error;
	if (!TestTrue(FString::Printf(TEXT("a kill decodes (%s)"), *Error), CrowdyExec::Decode(*Arena, *FCrowdyKitArenaEvent::StaticStruct(), Kill, &Event, Error)))
	{
		return false;
	}
	TestTrue(TEXT("it is a known kill by Me of Rival"), Event.bHasType && Event.Type == ECrowdyKitArenaEventType::Kill && Event.Killer == Me && Event.Victim == Rival);
	TestTrue(TEXT("and fills nothing of the other types"), Event.Ability.IsEmpty() && Event.Caster == 0 && Event.Damage == 0 && Event.Player == 0 && Event.Winner == 0);

	TArray<uint8> Unknown;
	FCrowdyExecWriter UnknownWriter(Unknown);
	UnknownWriter.MapHeader(2);
	WriteKey(UnknownWriter, "type");
	WriteText(UnknownWriter, "emote");
	WriteKey(UnknownWriter, "player");
	UnknownWriter.UInt(Me);
	FCrowdyKitArenaEvent Later;
	Later.bHasType = true;
	TestTrue(FString::Printf(TEXT("an event of a type this client does not know still decodes (%s)"), *Error),
		CrowdyExec::Decode(*Arena, *FCrowdyKitArenaEvent::StaticStruct(), Unknown, &Later, Error));
	TestFalse(TEXT("and says its type is unknown"), Later.bHasType);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterArenaMoveAndCastTravelAsMetres,
	"CrowdySDK.CrowdyExec.GameKitStarter.ArenaMoveAndCastTravelAsMetres", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterArenaMoveAndCastTravelAsMetres::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Arena = MakeArenaDefinition(*this);
	if (!Arena)
	{
		return false;
	}
	TArray<uint8> Kept;
	FCrowdyExecWriter KeptWriter(Kept);
	KeptWriter.MapHeader(2);
	WriteKey(KeptWriter, "x");
	KeptWriter.Double(12.5);
	WriteKey(KeptWriter, "y");
	KeptWriter.Double(30.25);
	FCrowdyKitGroundPosition Position;
	FString Error;
	TestTrue(FString::Printf(TEXT("a move's {x, y} answer fills a ground position (%s)"), *Error), CrowdyExec::Decode(*Arena, *FCrowdyKitGroundPosition::StaticStruct(), Kept, &Position, Error));
	TestTrue(TEXT("with the kept place in metres"), Position.X == 12.5 && Position.Y == 30.25);

	FCrowdyKitArenaMoveInputs Move;
	Move.X = 12.5;
	Move.Y = 30.25;
	TArray<uint8> Sent;
	TestTrue(FString::Printf(TEXT("a move encodes (%s)"), *Error), CrowdyExec::Encode(*Arena, FCrowdyKitArenaMoveInputs::StaticStruct(), &Move, Sent, Error));
	TArray<uint8> Expected;
	FCrowdyExecWriter ExpectedWriter(Expected);
	ExpectedWriter.MapHeader(2);
	WriteKey(ExpectedWriter, "x");
	ExpectedWriter.Double(12.5);
	WriteKey(ExpectedWriter, "y");
	ExpectedWriter.Double(30.25);
	TestTrue(TEXT("as the doubles x and y"), Sent == Expected);

	for (const bool bHit : {true, false})
	{
		const TArray<uint8> Bare = {static_cast<uint8>(bHit ? 0xc3 : 0xc2)};
		FCrowdyKitArenaCastReply Reply;
		Reply.bHit = !bHit;
		TestTrue(FString::Printf(TEXT("a cast's bare %s answer fills the reply (%s)"), bHit ? TEXT("true") : TEXT("false"), *Error),
			CrowdyExec::Decode(*Arena, *FCrowdyKitArenaCastReply::StaticStruct(), Bare, &Reply, Error) && Reply.bHit == bHit);
	}
	return true;
}

namespace CrowdyServerObjectKitTests
{
	/** Shapes Race like the race starter: positions in metres, and its events as one message. No server name is overridden. */
	TStrongObjectPtr<UCrowdyServerObjectDefinition> MakeRaceDefinition(FAutomationTestBase& Test)
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Race(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
		Race->TypeName = TEXT("race");
		Race->CodeSource = ECrowdyServerCodeSource::GameKitStarter;
		Race->StateForm = ECrowdyServerValuesForm::Struct;
		Race->KitStateTopic = TEXT("race");
		Race->State = FCrowdyKitRaceState::StaticStruct();
		Race->WatchedFields.Reset();
		FCrowdyServerMessage& Event = Race->Messages.AddDefaulted_GetRef();
		Event.Name = TEXT("RaceEvent");
		Event.ServerName = TEXT("race");
		Event.Payload = FCrowdyKitRaceEvent::StaticStruct();
		Race->KitPositionFunction = TEXT("Move");
		Race->Functions = {
			KitFunction(TEXT("Join"), TEXT("join"), nullptr, nullptr, true),
			KitFunction(TEXT("Move"), TEXT("move"), FCrowdyKitGroundPosition::StaticStruct(), FCrowdyKitGroundPosition::StaticStruct(), false),
			KitFunction(TEXT("GetGhost"), TEXT("ghost"), nullptr, FCrowdyKitRaceGhost::StaticStruct(), false),
		};
		TArray<FString> Errors;
		const bool bBaked = Race->Bake(Errors);
		if (!Test.TestTrue(FString::Printf(TEXT("the race starter bakes (%s)"), *FString::Join(Errors, TEXT("; "))), bBaked))
		{
			Race.Reset();
		}
		return Race;
	}

	void WriteRacer(FCrowdyExecWriter& Writer, int64 Player, double X, double Y, int32 Lap, int32 NextGate, int64 BestLapMs, int32 Place, int64 TotalMs, bool bDidNotFinish)
	{
		Writer.MapHeader(9);
		WriteKey(Writer, "player");
		Writer.UInt(Player);
		WriteKey(Writer, "x");
		Writer.Double(X);
		WriteKey(Writer, "y");
		Writer.Double(Y);
		WriteKey(Writer, "lap");
		Writer.UInt(Lap);
		WriteKey(Writer, "next_gate");
		Writer.UInt(NextGate);
		const auto WriteOrNil = [&Writer](int64 Value)
		{
			if (Value > 0)
			{
				Writer.UInt(Value);
				return;
			}
			Writer.Nil();
		};
		WriteKey(Writer, "best_lap_ms");
		WriteOrNil(BestLapMs);
		WriteKey(Writer, "place");
		WriteOrNil(Place);
		WriteKey(Writer, "total_ms");
		WriteOrNil(TotalMs);
		WriteKey(Writer, "did_not_finish");
		Writer.Bool(bDidNotFinish);
	}

	/** A race under way as the hub sends it: three gates (the second's radius a whole number of metres), one racer finished, one out, no time limit. */
	TArray<uint8> RaceStateBody()
	{
		TArray<uint8> Bytes;
		FCrowdyExecWriter Writer(Bytes);
		Writer.MapHeader(8);
		WriteKey(Writer, "phase");
		WriteText(Writer, "racing");
		WriteKey(Writer, "course_id");
		WriteText(Writer, "loop");
		WriteKey(Writer, "gates");
		Writer.ArrayHeader(3);
		Writer.MapHeader(3);
		WriteKey(Writer, "x");
		Writer.Double(0.5);
		WriteKey(Writer, "y");
		Writer.Double(0.25);
		WriteKey(Writer, "radius");
		Writer.Double(3.5);
		Writer.MapHeader(3);
		WriteKey(Writer, "x");
		Writer.Double(30.5);
		WriteKey(Writer, "y");
		Writer.Double(0.75);
		WriteKey(Writer, "radius");
		Writer.UInt(3);
		Writer.MapHeader(3);
		WriteKey(Writer, "x");
		Writer.Double(15.25);
		WriteKey(Writer, "y");
		Writer.Double(25.5);
		WriteKey(Writer, "radius");
		Writer.Double(2.5);
		WriteKey(Writer, "laps");
		Writer.UInt(2);
		WriteKey(Writer, "go_at");
		Writer.UInt(Epoch + 3000);
		WriteKey(Writer, "closes_at");
		Writer.UInt(Epoch + 93000);
		WriteKey(Writer, "ends_at");
		Writer.Nil();
		WriteKey(Writer, "racers");
		Writer.ArrayHeader(3);
		WriteRacer(Writer, Me, 12.25, 30.5, 2, 1, 41250, 0, 0, false);
		WriteRacer(Writer, Rival, 0.5, 0.25, 2, 0, 40000, 1, 83500, false);
		WriteRacer(Writer, 11, 5.75, 8.5, 1, 2, 0, 0, 0, true);
		return Bytes;
	}

	/** Shapes Ball like the ball starter: positions in metres, the team sent by name, and its events as one message. */
	TStrongObjectPtr<UCrowdyServerObjectDefinition> MakeBallDefinition(FAutomationTestBase& Test)
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Ball(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
		UScriptStruct* Ground = FCrowdyKitGroundPosition::StaticStruct();
		Ball->TypeName = TEXT("ball");
		Ball->CodeSource = ECrowdyServerCodeSource::GameKitStarter;
		Ball->StateForm = ECrowdyServerValuesForm::Struct;
		Ball->KitStateTopic = TEXT("ball");
		Ball->State = FCrowdyKitBallState::StaticStruct();
		Ball->WatchedFields.Reset();
		FCrowdyServerMessage& Event = Ball->Messages.AddDefaulted_GetRef();
		Event.Name = TEXT("BallEvent");
		Event.ServerName = TEXT("ball");
		Event.Payload = FCrowdyKitBallEvent::StaticStruct();
		Ball->KitPositionFunction = TEXT("Move");
		Ball->Functions = {
			KitFunction(TEXT("Join"), TEXT("join"), FCrowdyKitBallJoinInputs::StaticStruct(), nullptr, true),
			KitFunction(TEXT("Move"), TEXT("move"), Ground, Ground, false),
			KitFunction(TEXT("Pass"), TEXT("pass"), FCrowdyKitBallPassInputs::StaticStruct(), nullptr, true),
			KitFunction(TEXT("Shoot"), TEXT("shoot"), Ground, nullptr, true),
		};
		TArray<FString> Errors;
		const bool bBaked = Ball->Bake(Errors);
		if (!Test.TestTrue(FString::Printf(TEXT("the ball starter bakes (%s)"), *FString::Join(Errors, TEXT("; "))), bBaked))
		{
			Ball.Reset();
		}
		return Ball;
	}

	/** A ball game as the hub sends it: held by Me, or for bLoose a finished game won by red with the ball loose, where its holder and touches are nil. */
	TArray<uint8> BallStateBody(bool bLoose)
	{
		TArray<uint8> Bytes;
		FCrowdyExecWriter Writer(Bytes);
		Writer.MapHeader(10);
		WriteKey(Writer, "phase");
		WriteText(Writer, bLoose ? "finished" : "playing");
		WriteKey(Writer, "origin");
		WriteArenaPoint(Writer, 10.5, 20.25);
		WriteKey(Writer, "width");
		Writer.UInt(60);
		WriteKey(Writer, "depth");
		Writer.Double(40.5);
		WriteKey(Writer, "goal_width");
		Writer.Double(12.25);
		WriteKey(Writer, "ball");
		Writer.MapHeader(7);
		WriteKey(Writer, "x");
		Writer.Double(40.5);
		WriteKey(Writer, "y");
		Writer.Double(30.25);
		WriteKey(Writer, "vx");
		Writer.Double(-3.5);
		WriteKey(Writer, "vy");
		Writer.UInt(0);
		const auto WriteOrNil = [&Writer, bLoose](uint64 Value)
		{
			if (bLoose)
			{
				Writer.Nil();
				return;
			}
			Writer.UInt(Value);
		};
		WriteKey(Writer, "holder");
		WriteOrNil(Me);
		WriteKey(Writer, "held_since");
		WriteOrNil(Epoch + 1000);
		WriteKey(Writer, "last_touch");
		WriteOrNil(Me);
		WriteKey(Writer, "players");
		Writer.ArrayHeader(2);
		for (const bool bBlue : {false, true})
		{
			Writer.MapHeader(4);
			WriteKey(Writer, "player");
			Writer.UInt(bBlue ? Rival : Me);
			WriteKey(Writer, "team");
			WriteText(Writer, bBlue ? "blue" : "red");
			WriteKey(Writer, "x");
			Writer.Double(bBlue ? 55.5 : 25.5);
			WriteKey(Writer, "y");
			Writer.Double(30.25);
		}
		WriteKey(Writer, "score");
		Writer.ArrayHeader(2);
		Writer.UInt(bLoose ? 3 : 2);
		Writer.UInt(1);
		WriteKey(Writer, "ends_at");
		Writer.UInt(Epoch + 240000);
		WriteKey(Writer, "winner");
		if (!bLoose)
		{
			Writer.Nil();
			return Bytes;
		}
		WriteText(Writer, "red");
		return Bytes;
	}

	/** A ball event with a type, a player and a team, and Extra's keys after them. */
	TArray<uint8> BallEvent(const ANSICHAR* Type, int64 Player, const ANSICHAR* Team, int32 ExtraPairs, const TArray<uint8>& Extra)
	{
		TArray<uint8> Bytes;
		FCrowdyExecWriter Writer(Bytes);
		Writer.MapHeader(3 + ExtraPairs);
		WriteKey(Writer, "type");
		WriteText(Writer, Type);
		WriteKey(Writer, "player");
		Writer.UInt(Player);
		WriteKey(Writer, "team");
		WriteText(Writer, Team);
		Bytes.Append(Extra);
		return Bytes;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterRaceStateReadsMetres,
	"CrowdySDK.CrowdyExec.GameKitStarter.RaceStateReadsMetres", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterRaceStateReadsMetres::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Race = MakeRaceDefinition(*this);
	if (!Race)
	{
		return false;
	}
	FCrowdyKitRaceState Course;
	FString Error;
	if (!TestTrue(FString::Printf(TEXT("a race under way decodes (%s)"), *Error), CrowdyExec::Decode(*Race, *FCrowdyKitRaceState::StaticStruct(), RaceStateBody(), &Course, Error)))
	{
		return false;
	}
	TestTrue(TEXT("the phase, course and laps read by name"), Course.Phase == ECrowdyKitRacePhase::Racing && Course.CourseId == TEXT("loop") && Course.Laps == 2);
	TestTrue(TEXT("the times are Unix milliseconds, and nil ends_at is no time limit"), Course.GoAt == static_cast<int64>(Epoch + 3000) && Course.ClosesAt == static_cast<int64>(Epoch + 93000) && Course.EndsAt == 0);
	if (!TestEqual(TEXT("three gates"), Course.Gates.Num(), 3))
	{
		return false;
	}
	TestTrue(TEXT("a gate is a place and a radius in metres"), Course.Gates[0].X == 0.5 && Course.Gates[0].Y == 0.25 && Course.Gates[0].Radius == 3.5);
	TestTrue(TEXT("and a radius that is a whole number of metres, sent as an integer, still reads"), Course.Gates[1].X == 30.5 && Course.Gates[1].Radius == 3.0);
	TestTrue(TEXT("the last gate reads in order"), Course.Gates[2].X == 15.25 && Course.Gates[2].Y == 25.5 && Course.Gates[2].Radius == 2.5);
	if (!TestEqual(TEXT("three racers"), Course.Racers.Num(), 3))
	{
		return false;
	}
	const FCrowdyKitRacer& Driving = Course.Racers[0];
	TestTrue(TEXT("a racer under way has its place in metres, lap and next gate"), Driving.Player == Me && Driving.X == 12.25 && Driving.Y == 30.5 && Driving.Lap == 2 && Driving.NextGate == 1);
	TestTrue(TEXT("and a best lap, with nil place and total for a racer who has not finished"), Driving.BestLapMs == 41250 && Driving.Place == 0 && Driving.TotalMs == 0 && !Driving.bDidNotFinish);
	const FCrowdyKitRacer& Finished = Course.Racers[1];
	TestTrue(TEXT("a finisher has a place and a total"), Finished.Player == Rival && Finished.Place == 1 && Finished.TotalMs == 83500 && Finished.BestLapMs == 40000);
	const FCrowdyKitRacer& Out = Course.Racers[2];
	TestTrue(TEXT("a racer who did not finish says so, with nil best lap"), Out.Player == 11 && Out.bDidNotFinish && Out.BestLapMs == 0 && Out.Place == 0 && Out.NextGate == 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterRaceEventsDecode,
	"CrowdySDK.CrowdyExec.GameKitStarter.RaceEventsDecode", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterRaceEventsDecode::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Race = MakeRaceDefinition(*this);
	if (!Race)
	{
		return false;
	}
	FString Error;
	const auto Decode = [&Race, &Error](const TArray<uint8>& Bytes, FCrowdyKitRaceEvent& Out)
	{
		return CrowdyExec::Decode(*Race, *FCrowdyKitRaceEvent::StaticStruct(), Bytes, &Out, Error);
	};

	TArray<uint8> Go;
	FCrowdyExecWriter GoWriter(Go);
	GoWriter.MapHeader(2);
	WriteKey(GoWriter, "type");
	WriteText(GoWriter, "go");
	WriteKey(GoWriter, "at");
	GoWriter.UInt(Epoch + 3000);
	FCrowdyKitRaceEvent Event;
	if (!TestTrue(FString::Printf(TEXT("a go decodes (%s)"), *Error), Decode(Go, Event)))
	{
		return false;
	}
	TestTrue(TEXT("it is a known go at its time"), Event.bHasType && Event.Type == ECrowdyKitRaceEventType::Go && Event.At == static_cast<int64>(Epoch + 3000));

	TArray<uint8> Timing;
	FCrowdyExecWriter TimingWriter(Timing);
	TimingWriter.MapHeader(6);
	WriteKey(TimingWriter, "type");
	WriteText(TimingWriter, "timing");
	WriteKey(TimingWriter, "stage");
	WriteText(TimingWriter, "checkpoint");
	WriteKey(TimingWriter, "player");
	TimingWriter.UInt(Me);
	WriteKey(TimingWriter, "checkpoint");
	TimingWriter.UInt(1);
	WriteKey(TimingWriter, "lap");
	TimingWriter.UInt(2);
	WriteKey(TimingWriter, "elapsed_ms");
	TimingWriter.UInt(4200);
	FCrowdyKitRaceEvent Timed;
	if (!TestTrue(FString::Printf(TEXT("a timing decodes (%s)"), *Error), Decode(Timing, Timed)))
	{
		return false;
	}
	TestTrue(TEXT("it is a known checkpoint of Me"), Timed.bHasType && Timed.Type == ECrowdyKitRaceEventType::Timing && Timed.bHasStage && Timed.Stage == ECrowdyKitRaceStage::Checkpoint && Timed.Player == Me);
	TestTrue(TEXT("with its gate, lap and elapsed time"), Timed.Checkpoint == 1 && Timed.Lap == 2 && Timed.ElapsedMs == 4200 && Timed.At == 0);

	TArray<uint8> Strange;
	FCrowdyExecWriter StrangeWriter(Strange);
	StrangeWriter.MapHeader(3);
	WriteKey(StrangeWriter, "type");
	WriteText(StrangeWriter, "timing");
	WriteKey(StrangeWriter, "stage");
	WriteText(StrangeWriter, "sprint");
	WriteKey(StrangeWriter, "player");
	StrangeWriter.UInt(Me);
	FCrowdyKitRaceEvent Later;
	Later.bHasStage = true;
	TestTrue(FString::Printf(TEXT("a timing of a stage this client does not know still decodes (%s)"), *Error), Decode(Strange, Later));
	TestTrue(TEXT("and says its stage is unknown while its type is known"), !Later.bHasStage && Later.bHasType && Later.Type == ECrowdyKitRaceEventType::Timing);

	TArray<uint8> Violation;
	FCrowdyExecWriter ViolationWriter(Violation);
	ViolationWriter.MapHeader(4);
	WriteKey(ViolationWriter, "type");
	WriteText(ViolationWriter, "violation");
	WriteKey(ViolationWriter, "player");
	ViolationWriter.UInt(Rival);
	WriteKey(ViolationWriter, "what");
	WriteText(ViolationWriter, "teleport");
	WriteKey(ViolationWriter, "detail");
	WriteText(ViolationWriter, "jumped 25.5 m");
	FCrowdyKitRaceEvent Refused;
	TestTrue(FString::Printf(TEXT("a violation decodes (%s)"), *Error), Decode(Violation, Refused));
	TestTrue(TEXT("naming who and what"), Refused.Type == ECrowdyKitRaceEventType::Violation && Refused.Player == Rival && Refused.What == TEXT("teleport") && Refused.Detail == TEXT("jumped 25.5 m"));

	TArray<uint8> Retired;
	FCrowdyExecWriter RetiredWriter(Retired);
	RetiredWriter.MapHeader(3);
	WriteKey(RetiredWriter, "type");
	WriteText(RetiredWriter, "retired");
	WriteKey(RetiredWriter, "player");
	RetiredWriter.UInt(Rival);
	WriteKey(RetiredWriter, "why");
	WriteText(RetiredWriter, "idle");
	FCrowdyKitRaceEvent Gone;
	TestTrue(FString::Printf(TEXT("a retirement decodes (%s)"), *Error), Decode(Retired, Gone));
	TestTrue(TEXT("naming who and why"), Gone.Type == ECrowdyKitRaceEventType::Retired && Gone.Player == Rival && Gone.Why == TEXT("idle"));

	TArray<uint8> Finished;
	FCrowdyExecWriter FinishedWriter(Finished);
	FinishedWriter.MapHeader(2);
	WriteKey(FinishedWriter, "type");
	WriteText(FinishedWriter, "finished");
	WriteKey(FinishedWriter, "results");
	FinishedWriter.ArrayHeader(2);
	for (const int32 Place : {1, 2})
	{
		FinishedWriter.MapHeader(3);
		WriteKey(FinishedWriter, "player");
		FinishedWriter.UInt(Place == 1 ? Rival : Me);
		WriteKey(FinishedWriter, "place");
		FinishedWriter.UInt(Place);
		WriteKey(FinishedWriter, "total_ms");
		FinishedWriter.UInt(Place == 1 ? 83500 : 91250);
	}
	FCrowdyKitRaceEvent Closed;
	TestTrue(FString::Printf(TEXT("the end of the race decodes (%s)"), *Error), Decode(Finished, Closed));
	TestTrue(TEXT("with its finishers in place order"), Closed.Type == ECrowdyKitRaceEventType::Finished && Closed.Results.Num() == 2
		&& Closed.Results[0].Player == Rival && Closed.Results[0].Place == 1 && Closed.Results[0].TotalMs == 83500
		&& Closed.Results[1].Player == Me && Closed.Results[1].Place == 2 && Closed.Results[1].TotalMs == 91250);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterRaceMoveAndGhostTravelAsMetres,
	"CrowdySDK.CrowdyExec.GameKitStarter.RaceMoveAndGhostTravelAsMetres", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterRaceMoveAndGhostTravelAsMetres::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Race = MakeRaceDefinition(*this);
	if (!Race)
	{
		return false;
	}
	TArray<uint8> Kept;
	FCrowdyExecWriter KeptWriter(Kept);
	KeptWriter.MapHeader(2);
	WriteKey(KeptWriter, "x");
	KeptWriter.UInt(10);
	WriteKey(KeptWriter, "y");
	KeptWriter.Double(30.5);
	FCrowdyKitGroundPosition Position;
	FString Error;
	TestTrue(FString::Printf(TEXT("a move's {x, y} answer fills a ground position (%s)"), *Error), CrowdyExec::Decode(*Race, *FCrowdyKitGroundPosition::StaticStruct(), Kept, &Position, Error));
	TestTrue(TEXT("with the kept place in metres, a whole number of them sent as an integer"), Position.X == 10.0 && Position.Y == 30.5);

	FCrowdyKitGroundPosition Move;
	Move.X = 12.25;
	Move.Y = 30.5;
	TArray<uint8> Sent;
	TestTrue(FString::Printf(TEXT("a move encodes (%s)"), *Error), CrowdyExec::Encode(*Race, FCrowdyKitGroundPosition::StaticStruct(), &Move, Sent, Error));
	TArray<uint8> Expected;
	FCrowdyExecWriter ExpectedWriter(Expected);
	ExpectedWriter.MapHeader(2);
	WriteKey(ExpectedWriter, "x");
	ExpectedWriter.Double(12.25);
	WriteKey(ExpectedWriter, "y");
	ExpectedWriter.Double(30.5);
	TestTrue(TEXT("as the doubles x and y"), Sent == Expected);

	TArray<uint8> Run;
	FCrowdyExecWriter RunWriter(Run);
	RunWriter.MapHeader(3);
	WriteKey(RunWriter, "player");
	RunWriter.UInt(Rival);
	WriteKey(RunWriter, "total_ms");
	RunWriter.UInt(83500);
	WriteKey(RunWriter, "frames");
	RunWriter.ArrayHeader(2);
	RunWriter.MapHeader(3);
	WriteKey(RunWriter, "at_ms");
	RunWriter.UInt(0);
	WriteKey(RunWriter, "x");
	RunWriter.Double(0.5);
	WriteKey(RunWriter, "y");
	RunWriter.Double(0.25);
	RunWriter.MapHeader(3);
	WriteKey(RunWriter, "at_ms");
	RunWriter.UInt(250);
	WriteKey(RunWriter, "x");
	RunWriter.Double(2.75);
	WriteKey(RunWriter, "y");
	RunWriter.Double(0.25);
	FCrowdyKitRaceGhost Ghost;
	TestTrue(FString::Printf(TEXT("the winner's run decodes (%s)"), *Error), CrowdyExec::Decode(*Race, *FCrowdyKitRaceGhost::StaticStruct(), Run, &Ghost, Error));
	TestTrue(TEXT("with the winner and their time"), Ghost.Player == Rival && Ghost.TotalMs == 83500);
	TestTrue(TEXT("and each frame's time and place in metres"), Ghost.Frames.Num() == 2 && Ghost.Frames[0].AtMs == 0 && Ghost.Frames[0].X == 0.5 && Ghost.Frames[0].Y == 0.25
		&& Ghost.Frames[1].AtMs == 250 && Ghost.Frames[1].X == 2.75 && Ghost.Frames[1].Y == 0.25);

	const TArray<uint8> Nothing = {0xc0};
	FCrowdyKitRaceGhost None;
	TestTrue(FString::Printf(TEXT("before a winner the answer is nil, which decodes (%s)"), *Error), CrowdyExec::Decode(*Race, *FCrowdyKitRaceGhost::StaticStruct(), Nothing, &None, Error));
	TestTrue(TEXT("to a run with no winner and no frames"), None.Player == 0 && None.TotalMs == 0 && None.Frames.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterBallStateReadsMetres,
	"CrowdySDK.CrowdyExec.GameKitStarter.BallStateReadsMetres", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterBallStateReadsMetres::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Ball = MakeBallDefinition(*this);
	if (!Ball)
	{
		return false;
	}
	FCrowdyKitBallState Game;
	FString Error;
	if (!TestTrue(FString::Printf(TEXT("a ball game decodes (%s)"), *Error), CrowdyExec::Decode(*Ball, *FCrowdyKitBallState::StaticStruct(), BallStateBody(false), &Game, Error)))
	{
		return false;
	}
	TestTrue(TEXT("the phase reads by name, and the corner from its [x, y]"), Game.Phase == ECrowdyKitBallPhase::Playing && Game.Origin.X == 10.5 && Game.Origin.Y == 20.25);
	TestTrue(TEXT("the pitch's size reads in metres, a whole width sent as an integer"), Game.Width == 60.0 && Game.Depth == 40.5 && Game.GoalWidth == 12.25);
	TestTrue(TEXT("the ball has its place, speed and holder"), Game.Ball.X == 40.5 && Game.Ball.Y == 30.25 && Game.Ball.VX == -3.5 && Game.Ball.VY == 0.0 && Game.Ball.Holder == Me);
	TestTrue(TEXT("and when it was taken and by whom it was touched"), Game.Ball.HeldSince == static_cast<int64>(Epoch + 1000) && Game.Ball.LastTouch == Me);
	if (!TestEqual(TEXT("two players"), Game.Players.Num(), 2))
	{
		return false;
	}
	TestTrue(TEXT("a player has its team and place in metres"), Game.Players[0].Player == Me && Game.Players[0].Team == ECrowdyKitBallTeam::Red && Game.Players[0].X == 25.5 && Game.Players[0].Y == 30.25);
	TestTrue(TEXT("the other is on blue"), Game.Players[1].Player == Rival && Game.Players[1].Team == ECrowdyKitBallTeam::Blue && Game.Players[1].X == 55.5);
	TestTrue(TEXT("the score is red then blue, and nil winner is no winner"), Game.Score == TArray<int32>({2, 1}) && Game.EndsAt == static_cast<int64>(Epoch + 240000) && !Game.bHasWinner);

	FCrowdyKitBallState Over;
	if (!TestTrue(FString::Printf(TEXT("a finished game with the ball loose decodes (%s)"), *Error), CrowdyExec::Decode(*Ball, *FCrowdyKitBallState::StaticStruct(), BallStateBody(true), &Over, Error)))
	{
		return false;
	}
	TestTrue(TEXT("a loose ball has no holder, no time held and no last touch"), Over.Ball.Holder == 0 && Over.Ball.HeldSince == 0 && Over.Ball.LastTouch == 0);
	TestTrue(TEXT("and a winner is a team"), Over.Phase == ECrowdyKitBallPhase::Finished && Over.bHasWinner && Over.Winner == ECrowdyKitBallTeam::Red && Over.Score == TArray<int32>({3, 1}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterBallEventsDecode,
	"CrowdySDK.CrowdyExec.GameKitStarter.BallEventsDecode", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterBallEventsDecode::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Ball = MakeBallDefinition(*this);
	if (!Ball)
	{
		return false;
	}
	FString Error;
	const auto Decode = [&Ball, &Error](const TArray<uint8>& Bytes, FCrowdyKitBallEvent& Out)
	{
		return CrowdyExec::Decode(*Ball, *FCrowdyKitBallEvent::StaticStruct(), Bytes, &Out, Error);
	};
	const auto Pairs = [](TFunctionRef<void(FCrowdyExecWriter&)> Fill)
	{
		TArray<uint8> Bytes;
		FCrowdyExecWriter Writer(Bytes);
		Fill(Writer);
		return Bytes;
	};

	FCrowdyKitBallEvent Claimed;
	const TArray<uint8> From = Pairs([](FCrowdyExecWriter& Writer)
	{
		WriteKey(Writer, "from");
		Writer.UInt(Rival);
	});
	if (!TestTrue(FString::Printf(TEXT("a claim decodes (%s)"), *Error), Decode(BallEvent("claimed", Me, "red", 1, From), Claimed)))
	{
		return false;
	}
	TestTrue(TEXT("it is a known claim by Me for red, taking the ball from Rival"), Claimed.bHasType && Claimed.Type == ECrowdyKitBallEventType::Claimed && Claimed.Player == Me
		&& Claimed.bHasTeam && Claimed.Team == ECrowdyKitBallTeam::Red && Claimed.From == Rival);
	const TArray<uint8> FromNone = Pairs([](FCrowdyExecWriter& Writer)
	{
		WriteKey(Writer, "from");
		Writer.Nil();
	});
	FCrowdyKitBallEvent Loose;
	TestTrue(FString::Printf(TEXT("a claim of a loose ball, with a nil from, decodes (%s)"), *Error), Decode(BallEvent("claimed", Rival, "blue", 1, FromNone), Loose));
	TestTrue(TEXT("to blue taking it from nobody"), Loose.Team == ECrowdyKitBallTeam::Blue && Loose.bHasTeam && Loose.From == 0);

	const TArray<uint8> To = Pairs([](FCrowdyExecWriter& Writer)
	{
		WriteKey(Writer, "to");
		Writer.UInt(Rival);
	});
	FCrowdyKitBallEvent Passed;
	TestTrue(FString::Printf(TEXT("a pass decodes (%s)"), *Error), Decode(BallEvent("passed", Me, "red", 1, To), Passed));
	TestTrue(TEXT("naming who it went to"), Passed.Type == ECrowdyKitBallEventType::Passed && Passed.Player == Me && Passed.To == Rival);

	const TArray<uint8> Score = Pairs([](FCrowdyExecWriter& Writer)
	{
		WriteKey(Writer, "score");
		Writer.ArrayHeader(2);
		Writer.UInt(2);
		Writer.UInt(1);
	});
	FCrowdyKitBallEvent Goal;
	TestTrue(FString::Printf(TEXT("a goal decodes (%s)"), *Error), Decode(BallEvent("goal", Me, "red", 1, Score), Goal));
	TestTrue(TEXT("naming the team that scored, the scorer and the score"), Goal.Type == ECrowdyKitBallEventType::Goal && Goal.Team == ECrowdyKitBallTeam::Red && Goal.Player == Me && Goal.Score == TArray<int32>({2, 1}));

	FCrowdyKitBallEvent Shot;
	TestTrue(FString::Printf(TEXT("a shot decodes (%s)"), *Error), Decode(BallEvent("shot", Rival, "blue", 0, {}), Shot));
	TestTrue(TEXT("naming who and which team"), Shot.Type == ECrowdyKitBallEventType::Shot && Shot.Player == Rival && Shot.Team == ECrowdyKitBallTeam::Blue);

	FCrowdyKitBallEvent Left;
	TestTrue(FString::Printf(TEXT("a departure decodes (%s)"), *Error), Decode(BallEvent("left", Rival, "blue", 0, {}), Left));
	TestTrue(TEXT("naming who and which team"), Left.Type == ECrowdyKitBallEventType::Left && Left.Player == Rival && Left.Team == ECrowdyKitBallTeam::Blue);

	const TArray<uint8> Idle = Pairs([](FCrowdyExecWriter& Writer)
	{
		WriteKey(Writer, "why");
		WriteText(Writer, "idle");
	});
	FCrowdyKitBallEvent Removed;
	TestTrue(FString::Printf(TEXT("a removal decodes (%s)"), *Error), Decode(BallEvent("removed", Rival, "blue", 1, Idle), Removed));
	TestTrue(TEXT("naming who, which team and why"), Removed.Type == ECrowdyKitBallEventType::Removed && Removed.Player == Rival && Removed.Team == ECrowdyKitBallTeam::Blue && Removed.Why == TEXT("idle"));

	const TArray<uint8> Wrong = Pairs([](FCrowdyExecWriter& Writer)
	{
		WriteKey(Writer, "what");
		WriteText(Writer, "speed");
		WriteKey(Writer, "detail");
		WriteText(Writer, "ran 14.5 m in a second");
	});
	FCrowdyKitBallEvent Violation;
	TestTrue(FString::Printf(TEXT("a violation decodes (%s)"), *Error), Decode(BallEvent("violation", Me, "red", 2, Wrong), Violation));
	TestTrue(TEXT("naming who and what"), Violation.Type == ECrowdyKitBallEventType::Violation && Violation.Player == Me && Violation.What == TEXT("speed") && Violation.Detail == TEXT("ran 14.5 m in a second"));

	TArray<uint8> Won;
	FCrowdyExecWriter WonWriter(Won);
	WonWriter.MapHeader(3);
	WriteKey(WonWriter, "type");
	WriteText(WonWriter, "finished");
	WriteKey(WonWriter, "winner");
	WriteText(WonWriter, "blue");
	WriteKey(WonWriter, "score");
	WonWriter.ArrayHeader(2);
	WonWriter.UInt(1);
	WonWriter.UInt(2);
	FCrowdyKitBallEvent Ended;
	TestTrue(FString::Printf(TEXT("the end of the game decodes (%s)"), *Error), Decode(Won, Ended));
	TestTrue(TEXT("with its winner and score"), Ended.Type == ECrowdyKitBallEventType::Finished && Ended.bHasWinner && Ended.Winner == ECrowdyKitBallTeam::Blue && Ended.Score == TArray<int32>({1, 2}) && !Ended.bHasTeam);

	TArray<uint8> Drawn;
	FCrowdyExecWriter DrawnWriter(Drawn);
	DrawnWriter.MapHeader(3);
	WriteKey(DrawnWriter, "type");
	WriteText(DrawnWriter, "finished");
	WriteKey(DrawnWriter, "winner");
	DrawnWriter.Nil();
	WriteKey(DrawnWriter, "score");
	DrawnWriter.ArrayHeader(2);
	DrawnWriter.UInt(2);
	DrawnWriter.UInt(2);
	FCrowdyKitBallEvent Draw;
	Draw.bHasWinner = true;
	TestTrue(FString::Printf(TEXT("a drawn game decodes (%s)"), *Error), Decode(Drawn, Draw));
	TestTrue(TEXT("with no winner"), Draw.Type == ECrowdyKitBallEventType::Finished && !Draw.bHasWinner && Draw.Score == TArray<int32>({2, 2}));

	FCrowdyKitBallEvent Later;
	Later.bHasType = true;
	TestTrue(FString::Printf(TEXT("an event of a type this client does not know still decodes (%s)"), *Error), Decode(BallEvent("emote", Me, "red", 0, {}), Later));
	TestFalse(TEXT("and says its type is unknown"), Later.bHasType);
	FCrowdyKitBallEvent Stranger;
	Stranger.bHasTeam = true;
	TestTrue(FString::Printf(TEXT("a team this client does not know still decodes (%s)"), *Error), Decode(BallEvent("shot", Me, "green", 0, {}), Stranger));
	TestTrue(TEXT("and says it has no team"), !Stranger.bHasTeam && Stranger.bHasType && Stranger.Type == ECrowdyKitBallEventType::Shot);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterBallMoveJoinAndShootTravelAsMetres,
	"CrowdySDK.CrowdyExec.GameKitStarter.BallMoveJoinAndShootTravelAsMetres", CrowdyServerObjectTests::TestFlags)

bool FCrowdyGameKitStarterBallMoveJoinAndShootTravelAsMetres::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Ball = MakeBallDefinition(*this);
	if (!Ball)
	{
		return false;
	}
	FString Error;
	FCrowdyKitGroundPosition Aim;
	Aim.X = 70.5;
	Aim.Y = 30.25;
	TArray<uint8> SentAim;
	TestTrue(FString::Printf(TEXT("a move or a shot encodes (%s)"), *Error), CrowdyExec::Encode(*Ball, FCrowdyKitGroundPosition::StaticStruct(), &Aim, SentAim, Error));
	TArray<uint8> ExpectedAim;
	FCrowdyExecWriter AimWriter(ExpectedAim);
	AimWriter.MapHeader(2);
	WriteKey(AimWriter, "x");
	AimWriter.Double(70.5);
	WriteKey(AimWriter, "y");
	AimWriter.Double(30.25);
	TestTrue(TEXT("as the doubles x and y"), SentAim == ExpectedAim);

	TArray<uint8> Kept;
	FCrowdyExecWriter KeptWriter(Kept);
	KeptWriter.MapHeader(2);
	WriteKey(KeptWriter, "x");
	KeptWriter.Double(25.5);
	WriteKey(KeptWriter, "y");
	KeptWriter.UInt(30);
	FCrowdyKitGroundPosition Position;
	TestTrue(FString::Printf(TEXT("a move's {x, y} answer fills a ground position (%s)"), *Error), CrowdyExec::Decode(*Ball, *FCrowdyKitGroundPosition::StaticStruct(), Kept, &Position, Error));
	TestTrue(TEXT("with the kept place in metres, a whole number of them sent as an integer"), Position.X == 25.5 && Position.Y == 30.0);

	for (const ECrowdyKitBallTeam Team : {ECrowdyKitBallTeam::Red, ECrowdyKitBallTeam::Blue})
	{
		FCrowdyKitBallJoinInputs Join;
		Join.Team = Team;
		TArray<uint8> Sent;
		TestTrue(FString::Printf(TEXT("a join encodes (%s)"), *Error), CrowdyExec::Encode(*Ball, FCrowdyKitBallJoinInputs::StaticStruct(), &Join, Sent, Error));
		TArray<uint8> Expected;
		FCrowdyExecWriter ExpectedWriter(Expected);
		ExpectedWriter.MapHeader(1);
		WriteKey(ExpectedWriter, "team");
		WriteText(ExpectedWriter, Team == ECrowdyKitBallTeam::Red ? "red" : "blue");
		TestTrue(Team == ECrowdyKitBallTeam::Red ? TEXT("red joins as red") : TEXT("blue joins as blue"), Sent == Expected);
	}

	FCrowdyKitBallPassInputs Pass;
	Pass.To = Rival;
	TArray<uint8> SentPass;
	TestTrue(FString::Printf(TEXT("a pass encodes (%s)"), *Error), CrowdyExec::Encode(*Ball, FCrowdyKitBallPassInputs::StaticStruct(), &Pass, SentPass, Error));
	TArray<uint8> ExpectedPass;
	FCrowdyExecWriter PassWriter(ExpectedPass);
	PassWriter.MapHeader(1);
	WriteKey(PassWriter, "to");
	PassWriter.UInt(Rival);
	TestTrue(TEXT("as the teammate's id under to"), SentPass == ExpectedPass);
	return true;
}

#endif
