#include "CrowdyServerObjectTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyLeaderSubsystem.h"
#include "CrowdyServerObject.h"
#include "CrowdyServerObjectTestRig.h"
#include "Misc/AutomationTest.h"
#include "Subsystem/CrowdyGameSession.h"
#include "UObject/StrongObjectPtr.h"

namespace CrowdyLeaderSubsystemTests
{
	constexpr int64 LocalUser = 77;
	constexpr uint64 FirstSeq = 5;
	const TCHAR* const NoActorIdWarning = TEXT("holds no actor id, so it cannot lead");
	const TCHAR* const EnterFailedWarning = TEXT("Entering the Leader type failed");
	const TCHAR* const RedeployWarning = TEXT("deploy the app's server code again from Server Compute");
	const TCHAR* const EnterMethod = TEXT("enter");
	const TCHAR* const ExitMethod = TEXT("exit");

	/** A Leader and the actor id written for it, as the subsystem hands them to the session. */
	using FWrite = TPair<int64, FGuid>;
	using FChange = TPair<int64, int64>;

	TArray<FString> LeaderFields()
	{
		return {TEXT("LeaderActorUser"), TEXT("LeaderActorID")};
	}

	/** Gives the rig's definition the Leader type's published actor id and its Enter and Exit functions. */
	bool UseLeaderShape(CrowdyServerObjectTests::FRig& Rig)
	{
		Rig.Definition->State = FCrowdyServerObjectTestLeaderState::StaticStruct();
		Rig.Definition->WatchedFields = {TEXT("LeaderActorUser"), TEXT("LeaderActorID")};
		Rig.Definition->Functions.Reset();
		FCrowdyServerFunction& Enter = Rig.Definition->Functions.AddDefaulted_GetRef();
		Enter.Name = TEXT("Enter");
		Enter.Params = FCrowdyLeaderEnterInputs::StaticStruct();
		FCrowdyServerFunction& Exit = Rig.Definition->Functions.AddDefaulted_GetRef();
		Exit.Name = TEXT("Exit");
		Exit.Params = FCrowdyLeaderEnterInputs::StaticStruct();
		TArray<FString> Errors;
		const bool bBaked = Rig.Definition->Bake(Errors);
		return Rig.Test.TestTrue(FString::Printf(TEXT("the Leader-shaped definition bakes (%s)"), *FString::Join(Errors, TEXT("; "))), bBaked);
	}

	FCrowdyServerObjectTestLeaderState Published(const int64 User, const FString& ActorID)
	{
		FCrowdyServerObjectTestLeaderState State;
		State.LeaderActorUser = User;
		State.LeaderActorID = ActorID;
		return State;
	}

	FCrowdyServerObjectTestLeaderState Published(const int64 User, const FGuid& ActorID)
	{
		return Published(User, ActorID.ToString(EGuidFormats::Digits));
	}

	FCrowdyLeaderEnterInputs EnterInputs(const FGuid& ActorID)
	{
		FCrowdyLeaderEnterInputs Inputs;
		Inputs.ActorID = ActorID.ToString(EGuidFormats::Digits);
		return Inputs;
	}

	/** A read reply or a push carrying both published values, and Keys. */
	TArray<uint8> LeaderMessage(CrowdyServerObjectTests::FRig& Rig, const bool bRead, const uint64 Seq, const FCrowdyServerObjectTestLeaderState& Values,
		const CrowdyServerObjectTests::FMembersKeys& Keys = CrowdyServerObjectTests::FMembersKeys(), const uint64 MessageEpoch = CrowdyServerObjectTests::Epoch)
	{
		using namespace CrowdyServerObjectTests;
		TArray<uint8> Bytes;
		FCrowdyExecWriter Writer(Bytes);
		Writer.MapHeader((bRead ? 4 : 3) + Keys.Num());
		if (bRead)
		{
			WriteKey(Writer, "contract");
			Writer.UInt(CrowdyExec::ContractVersion);
		}
		WriteKey(Writer, "epoch");
		Writer.UInt(MessageEpoch);
		WriteKey(Writer, "seq");
		Writer.UInt(Seq);
		WriteKey(Writer, "fields");
		Bytes.Append(Rig.Encode(Values, LeaderFields()));
		Keys.Write(Bytes);
		return Bytes;
	}

	/** Makes the rig Leader-shaped and acquires its object, Ready, led by Leader with Values published. */
	UCrowdyServerObject* AcquireLeader(CrowdyServerObjectTests::FRig& Rig, const UObject* Owner, const int64 Leader,
		const FCrowdyServerObjectTestLeaderState& Values = FCrowdyServerObjectTestLeaderState())
	{
		using namespace CrowdyServerObjectTests;
		if (!UseLeaderShape(Rig))
		{
			return nullptr;
		}
		UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner);
		if (!Object || !Rig.Open())
		{
			return nullptr;
		}
		FSentFrame Subscribe;
		FSentFrame Read;
		if (!Rig.Test.TestTrue(TEXT("the object subscribes to its watched values"), Rig.WaitForFrame(KindSubscribe, TEXT("state"), Subscribe))
			|| !Rig.Test.TestTrue(TEXT("the object reads its watched values"), Rig.WaitForFrame(KindCall, TEXT("read"), Read)))
		{
			return nullptr;
		}
		FMembersKeys Keys;
		Keys.MemberCount = 1;
		Keys.Leader = Leader;
		Rig.Reply(Subscribe.Rid, ECrowdyNativeExecStatus::Ok, {});
		Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::Ok, LeaderMessage(Rig, true, FirstSeq, Values, Keys));
		const bool bReady = Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; });
		return Rig.Test.TestTrue(TEXT("the first read makes the object Ready"), bReady) ? Object : nullptr;
	}

	/** Pushes a Leader change at Seq, with Values when they change in the same push, and pumps until the subsystem has written one more Leader. */
	bool PushLeader(CrowdyServerObjectTests::FRig& Rig, const uint64 Seq, const int64 NewLeader, const TArray<FWrite>& Written,
		const FCrowdyServerObjectTestLeaderState* Values = nullptr)
	{
		CrowdyServerObjectTests::FMembersKeys Keys;
		Keys.Leader = NewLeader;
		const int32 Before = Written.Num();
		Rig.Push(Values ? LeaderMessage(Rig, false, Seq, *Values, Keys) : Rig.StateMessage(false, CrowdyServerObjectTests::Epoch, Seq, nullptr, {}, Keys));
		return Rig.PumpUntil([&Written, Before]() { return Written.Num() > Before; });
	}

	/** Pushes a Leader change at Seq, with Values when they change in the same push, and pumps until the session's app writer is Expected. */
	bool PushLeaderTo(CrowdyServerObjectTests::FRig& Rig, const uint64 Seq, const int64 NewLeader, const FGuid& Expected,
		const FCrowdyServerObjectTestLeaderState* Values = nullptr)
	{
		CrowdyServerObjectTests::FMembersKeys Keys;
		Keys.Leader = NewLeader;
		Rig.Push(Values ? LeaderMessage(Rig, false, Seq, *Values, Keys) : Rig.StateMessage(false, CrowdyServerObjectTests::Epoch, Seq, nullptr, {}, Keys));
		return Rig.PumpUntil([&Rig, &Expected]() { return Rig.Session->GetAppWriterID() == Expected; });
	}

	/** Pushes Values at Seq and pumps until the session's app writer is Expected. */
	bool PushPublished(CrowdyServerObjectTests::FRig& Rig, const uint64 Seq, const FCrowdyServerObjectTestLeaderState& Values, const FGuid& Expected)
	{
		Rig.Push(LeaderMessage(Rig, false, Seq, Values));
		return Rig.PumpUntil([&Rig, &Expected]() { return Rig.Session->GetAppWriterID() == Expected; });
	}

	/** The random session id a Player Derived avatar sets at a sign-in. */
	FString RandomPlayerUUID()
	{
		return FGuid::NewGuid().ToString(EGuidFormats::Digits);
	}

	TFunction<void(int64, const FGuid&)> Recorder(TArray<FWrite>& Written)
	{
		return [&Written](const int64 Id, const FGuid& ActorID) { Written.Emplace(Id, ActorID); };
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderSubsystemFollowsTheLeaderTest,
	"CrowdySDK.CrowdyExec.Leader.FollowsTheLeaderAndJoins", CrowdyServerObjectTests::TestFlags)
bool FCrowdyLeaderSubsystemFollowsTheLeaderTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyLeaderSubsystemTests;
	FRig Rig(*this, ECrowdyServerObjectVisibility::Public, ECrowdyServerMembersSource::ThisObject);
	if (!Rig.IsValid())
	{
		return false;
	}
	Rig.Session->SetUserID(LocalUser);
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquireLeader(Rig, Owner.Get(), 5);
	if (!Object)
	{
		return false;
	}
	Rig.Sent.Reset();

	Rig.Session->SetUUID(RandomPlayerUUID());

	TArray<FWrite> Written;
	TStrongObjectPtr<UCrowdyLeaderSubsystem> Leader(NewObject<UCrowdyLeaderSubsystem>(Rig.Instance.Get()));
	Leader->FollowForTest(Object, Rig.Session.Get(), Recorder(Written));
	TestTrue(TEXT("following a Ready object writes its Leader once"), Written.Num() == 1 && Written[0].Key == 5);
	TestEqual(TEXT("and reports it"), Leader->GetLeaderUserID(), static_cast<int64>(5));
	TestFalse(TEXT("another player leading is not this client"), Leader->IsLocalLeader());
	TestFalse(TEXT("not yet in the realtime world, it does not enter"), Rig.SentAny(KindCall, EnterMethod));
	Rig.Sent.Reset();
	Leader->HandleConnectionUp();
	FSentFrame Enter;
	TestTrue(TEXT("once its realtime connection is up it enters"), Rig.WaitForFrame(KindCall, EnterMethod, Enter));
	Rig.Reply(Enter.Rid, ECrowdyNativeExecStatus::Ok, {});
	Rig.PumpFor(0.15);
	Rig.Sent.Reset();

	const FCrowdyServerObjectTestLeaderState Own = Published(LocalUser, Rig.Session->GetID());
	if (!TestTrue(TEXT("a push making this client the Leader under its actor id is written"), PushLeader(Rig, 6, LocalUser, Written, &Own)))
	{
		return false;
	}
	TestTrue(TEXT("as this client's user id and actor id, in one write"), Written.Last() == FWrite(LocalUser, Rig.Session->GetID()));
	TestTrue(TEXT("and this client is the local Leader"), Leader->IsLocalLeader());
	TestFalse(TEXT("leading, it does not enter again"), Rig.SentAny(KindCall, EnterMethod));
	Rig.Sent.Reset();

	if (!TestTrue(TEXT("another player taking the lead is written"), PushLeader(Rig, 7, 9, Written)))
	{
		return false;
	}
	TestTrue(TEXT("as that player's user id, with no actor id since the published one is not theirs"), Written.Last() == FWrite(9, FGuid()));
	TestFalse(TEXT("and this client is no longer the local Leader"), Leader->IsLocalLeader());
	TestTrue(TEXT("and this client, the Leader before, enters again, since nothing tells it whether it was removed"), Rig.WaitForFrame(KindCall, EnterMethod, Enter));
	Rig.PumpFor(0.15);
	Rig.Sent.Reset();

	Leader->HandleConnectionLost();
	FSentFrame Exit;
	TestTrue(TEXT("a failed realtime connection exits"), Rig.WaitForFrame(KindCall, ExitMethod, Exit));
	TestTrue(TEXT("carrying the actor id it entered with, so another session of its user stays in"), Exit.Payload == Rig.Encode(EnterInputs(Rig.Session->GetID())));
	if (!TestTrue(TEXT("a Leader change while the connection is down is written"), PushLeader(Rig, 8, 10, Written)))
	{
		return false;
	}
	TestFalse(TEXT("but does not enter"), Rig.SentAny(KindCall, EnterMethod));
	Rig.Sent.Reset();
	Leader->HandleConnectionUp();
	TestTrue(TEXT("the connection coming back enters again"), Rig.WaitForFrame(KindCall, EnterMethod, Enter));
	return true;
}

// A client with no avatar has no player id to write with, so leading would stop writing for everyone.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderSubsystemJoinsOnlyWithAPlayerIdTest,
	"CrowdySDK.CrowdyExec.Leader.JoinsOnlyWithAPlayerId", CrowdyServerObjectTests::TestFlags)
bool FCrowdyLeaderSubsystemJoinsOnlyWithAPlayerIdTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyLeaderSubsystemTests;
	FRig Rig(*this, ECrowdyServerObjectVisibility::Public, ECrowdyServerMembersSource::ThisObject);
	if (!Rig.IsValid())
	{
		return false;
	}
	Rig.Session->SetUserID(LocalUser);
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquireLeader(Rig, Owner.Get(), 5);
	if (!Object)
	{
		return false;
	}
	Rig.Sent.Reset();

	TArray<FWrite> Written;
	TStrongObjectPtr<UCrowdyLeaderSubsystem> Leader(NewObject<UCrowdyLeaderSubsystem>(Rig.Instance.Get()));
	Leader->FollowForTest(Object, Rig.Session.Get(), Recorder(Written));

	Leader->HandleConnectionUp();
	Rig.PumpFor(0.15);
	TestFalse(TEXT("with no avatar yet, a connected client does not enter"), Rig.SentAny(KindCall, EnterMethod));

	Rig.Session->SetUUID(RandomPlayerUUID());
	const FGuid FirstId = Rig.Session->GetID();
	FSentFrame Join;
	TestTrue(TEXT("any valid id enters at once, whatever the user"), Rig.WaitForFrame(KindCall, EnterMethod, Join));
	Rig.PumpFor(0.15);
	Rig.Sent.Reset();

	// A sign-out drops the id, and the next sign-in gives the session a fresh one before its connection opens.
	Rig.Session->ClearCurrentSessionData();
	Leader->SignOutForTest();
	TestFalse(TEXT("signed out, the session holds no id"), Rig.Session->GetID().IsValid());
	Rig.Session->SetUserID(LocalUser);
	Rig.Session->BeginSignIn();
	Leader->SignInForTest();
	const FGuid SecondId = Rig.Session->GetID();
	TestTrue(TEXT("the same player signing back in gets a new id"), SecondId.IsValid() && SecondId != FirstId);
	Rig.PumpFor(0.15);
	TestFalse(TEXT("which enters nothing before the connection is up"), Rig.SentAny(KindCall, EnterMethod));
	Leader->HandleConnectionUp();
	TestTrue(TEXT("and enters once it is"), Rig.WaitForFrame(KindCall, EnterMethod, Join));
	TestTrue(TEXT("with the new id"), Join.Payload == Rig.Encode(EnterInputs(SecondId)));
	return true;
}

// The warning is for a client that stays without an id while it could lead, not for the moment before its pawn spawns.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderSubsystemWarnsOncePerSignInTest,
	"CrowdySDK.CrowdyExec.Leader.NoActorIdWarnsOncePerSignInAfterASecond", CrowdyServerObjectTests::TestFlags)
bool FCrowdyLeaderSubsystemWarnsOncePerSignInTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyLeaderSubsystemTests;
	FRig Rig(*this, ECrowdyServerObjectVisibility::Public, ECrowdyServerMembersSource::ThisObject);
	if (!Rig.IsValid())
	{
		return false;
	}
	Rig.Session->SetUserID(LocalUser);
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquireLeader(Rig, Owner.Get(), 5);
	if (!Object)
	{
		return false;
	}

	// One warning for the first sign-in, however often it tries, and one more for the next sign-in.
	AddExpectedMessagePlain(NoActorIdWarning, ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);
	TStrongObjectPtr<UCrowdyLeaderSubsystem> Leader(NewObject<UCrowdyLeaderSubsystem>(Rig.Instance.Get()));
	Leader->FollowForTest(Object, Rig.Session.Get(), nullptr);
	TestFalse(TEXT("before realtime is up nothing waits to warn"), Leader->RunPlayerIDWarningForTest());

	Leader->HandleConnectionUp();
	Leader->HandleConnectionUp();
	Leader->HandleConnectionUp();
	Rig.PumpFor(0.15);
	TestFalse(TEXT("with no actor id it does not enter"), Rig.SentAny(KindCall, EnterMethod));
	TestTrue(TEXT("the warning waits for a second after joining was first possible"), Leader->RunPlayerIDWarningForTest());
	Leader->HandleConnectionUp();
	TestFalse(TEXT("once warned, this sign-in waits for nothing more"), Leader->RunPlayerIDWarningForTest());

	Leader->SignInForTest();
	Leader->HandleConnectionUp();
	TestTrue(TEXT("the next sign-in waits again"), Leader->RunPlayerIDWarningForTest());

	// The normal moment between the connection coming up and the pawn spawning warns nothing.
	Leader->SignInForTest();
	Leader->HandleConnectionUp();
	Rig.Session->SetUUID(RandomPlayerUUID());
	FSentFrame Join;
	TestTrue(TEXT("a pawn spawning inside that second enters"), Rig.WaitForFrame(KindCall, EnterMethod, Join));
	TestTrue(TEXT("and the check that was waiting"), Leader->RunPlayerIDWarningForTest());
	return true;
}

// Every client reads who writes replicated subsystems from the game session, so the actor id the Leader published must land there.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderSubsystemWritesTheSessionTest,
	"CrowdySDK.CrowdyExec.Leader.AppWriterIsThePublishedActorId", CrowdyServerObjectTests::TestFlags)
bool FCrowdyLeaderSubsystemWritesTheSessionTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyLeaderSubsystemTests;
	FRig Rig(*this, ECrowdyServerObjectVisibility::Public, ECrowdyServerMembersSource::ThisObject);
	if (!Rig.IsValid())
	{
		return false;
	}
	Rig.Session->SetUserID(LocalUser);
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	const FGuid FirstID = FGuid::NewGuid();
	UCrowdyServerObject* Object = AcquireLeader(Rig, Owner.Get(), 5, Published(5, FirstID.ToString(EGuidFormats::Digits)));
	if (!Object)
	{
		return false;
	}

	TStrongObjectPtr<UCrowdyLeaderSubsystem> Leader(NewObject<UCrowdyLeaderSubsystem>(Rig.Instance.Get()));
	Leader->FollowForTest(Object, Rig.Session.Get(), nullptr);
	TestEqual(TEXT("the actor id the Leader published is the session's app writer"), Rig.Session->GetAppWriterID(), FirstID);

	FMembersKeys NewLeader;
	NewLeader.Leader = 9;
	Rig.Push(Rig.StateMessage(false, Epoch, FirstSeq + 1, nullptr, {}, NewLeader));
	TestTrue(TEXT("a new Leader that published nothing leaves no app writer, not the old Leader's actor id"),
		Rig.PumpUntil([&Rig]() { return !Rig.Session->GetAppWriterID().IsValid(); }));
	TestEqual(TEXT("while the new Leader is still reported"), Leader->GetLeaderUserID(), static_cast<int64>(9));

	const FGuid SecondID = FGuid::NewGuid();
	TestTrue(TEXT("the new Leader's published actor id becomes the app writer"), PushPublished(Rig, FirstSeq + 2, Published(9, SecondID.ToString(EGuidFormats::Digits)), SecondID));
	// Each push changes the app writer, so every wait below sees its own push land.
	const FString SecondDigits = SecondID.ToString(EGuidFormats::Digits);
	TestTrue(TEXT("a published id that is not 32 hex digits is no app writer"), PushPublished(Rig, FirstSeq + 3, Published(9, TEXT("not-an-actor-id")), FGuid()));
	TestTrue(TEXT("a well-formed one is again"), PushPublished(Rig, FirstSeq + 4, Published(9, SecondDigits), SecondID));
	TestTrue(TEXT("an all-zero published id is no app writer"), PushPublished(Rig, FirstSeq + 5, Published(9, FString::ChrN(32, TEXT('0'))), FGuid()));
	TestTrue(TEXT("the Leader's own is again"), PushPublished(Rig, FirstSeq + 6, Published(9, SecondDigits), SecondID));
	TestTrue(TEXT("one published by a player who is not the Leader is no app writer"), PushPublished(Rig, FirstSeq + 7, Published(5, SecondDigits), FGuid()));
	TestTrue(TEXT("and the Leader's own is again"), PushPublished(Rig, FirstSeq + 8, Published(9, SecondDigits), SecondID));
	Rig.Session->SetUUID(RandomPlayerUUID());
	TestTrue(TEXT("another user's Leader publishing this session's actor id does not make this client the writer"),
		PushPublished(Rig, FirstSeq + 9, Published(9, Rig.Session->GetID()), FGuid()));
	TestFalse(TEXT("nor the local Leader"), Leader->IsLocalLeader());
	if (!TestTrue(TEXT("and the Leader's own is again"), PushPublished(Rig, FirstSeq + 10, Published(9, SecondDigits), SecondID)))
	{
		return false;
	}

	// A restarted hub starts a new epoch; its first read is taken whole, cleared values included.
	Rig.Sent.Reset();
	Rig.Push(LeaderMessage(Rig, false, FirstSeq + 11, Published(9, SecondDigits), FMembersKeys(), Epoch + 1));
	FSentFrame Reread;
	if (!TestTrue(TEXT("a push from a new epoch reads again"), Rig.WaitForFrame(KindCall, TEXT("read"), Reread)))
	{
		return false;
	}
	FMembersKeys Restarted;
	Restarted.MemberCount = 1;
	Restarted.Leader = 9;
	Rig.Reply(Reread.Rid, ECrowdyNativeExecStatus::Ok, LeaderMessage(Rig, true, 1, FCrowdyServerObjectTestLeaderState(), Restarted, Epoch + 1));
	TestTrue(TEXT("a re-read with the published values cleared leaves no app writer"), Rig.PumpUntil([&Rig]() { return !Rig.Session->GetAppWriterID().IsValid(); }));
	TestEqual(TEXT("while the Leader is still reported"), Leader->GetLeaderUserID(), static_cast<int64>(9));
	Rig.Push(LeaderMessage(Rig, false, 2, Published(9, SecondDigits), FMembersKeys(), Epoch + 1));
	if (!TestTrue(TEXT("the new epoch's next push publishes again"), Rig.PumpUntil([&Rig, &SecondID]() { return Rig.Session->GetAppWriterID() == SecondID; })))
	{
		return false;
	}

	Leader->SignOutForTest();
	TestFalse(TEXT("a sign-out leaves no app writer"), Rig.Session->GetAppWriterID().IsValid());
	TestEqual(TEXT("and no Leader"), Leader->GetLeaderUserID(), static_cast<int64>(0));
	return true;
}

// Enter carries the actor id this client writes under, so the server can publish it in the same push that makes this client the Leader.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderSubsystemEntersWithItsActorIdTest,
	"CrowdySDK.CrowdyExec.Leader.EntersWithItsSessionIdAndAgainAfterARekey", CrowdyServerObjectTests::TestFlags)
bool FCrowdyLeaderSubsystemEntersWithItsActorIdTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyLeaderSubsystemTests;
	FRig Rig(*this, ECrowdyServerObjectVisibility::Public, ECrowdyServerMembersSource::ThisObject);
	if (!Rig.IsValid())
	{
		return false;
	}
	Rig.Session->SetUserID(LocalUser);
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquireLeader(Rig, Owner.Get(), 5);
	if (!Object)
	{
		return false;
	}
	Rig.Sent.Reset();
	Rig.Session->SetUUID(RandomPlayerUUID());
	const FGuid FirstID = Rig.Session->GetID();

	TArray<FWrite> Written;
	TStrongObjectPtr<UCrowdyLeaderSubsystem> Leader(NewObject<UCrowdyLeaderSubsystem>(Rig.Instance.Get()));
	Leader->FollowForTest(Object, Rig.Session.Get(), Recorder(Written));
	Leader->HandleConnectionUp();
	FSentFrame Enter;
	if (!TestTrue(TEXT("connected, it enters"), Rig.WaitForFrame(KindCall, EnterMethod, Enter)))
	{
		return false;
	}
	TestTrue(TEXT("carrying its session's actor id as 32 hex digits"), Enter.Payload == Rig.Encode(EnterInputs(FirstID)));
	Leader->HandleConnectionUp();
	TestFalse(TEXT("while that Enter is in flight, nothing sends it again"), Rig.SentAny(KindCall, EnterMethod));
	Rig.Reply(Enter.Rid, ECrowdyNativeExecStatus::Ok, {});
	Rig.PumpFor(0.15);
	Rig.Sent.Reset();

	// Two sessions of one user share one membership: the last to enter writes, and the other must not take it back.
	const FGuid OtherSession = FGuid::NewGuid();
	const FCrowdyServerObjectTestLeaderState Other = Published(LocalUser, OtherSession);
	if (!TestTrue(TEXT("this user leading under another session's actor id is written"), PushLeader(Rig, FirstSeq + 1, LocalUser, Written, &Other)))
	{
		return false;
	}
	TestTrue(TEXT("as that session's actor id"), Written.Last() == FWrite(LocalUser, OtherSession));
	TestFalse(TEXT("which does not make this session the local Leader"), Leader->IsLocalLeader());
	TestFalse(TEXT("and does not make it enter again"), Rig.SentAny(KindCall, EnterMethod));
	Rig.Sent.Reset();

	Rig.Session->SetUUID(RandomPlayerUUID());
	const FGuid SecondID = Rig.Session->GetID();
	TestTrue(TEXT("a re-key gives the session a new actor id"), SecondID.IsValid() && SecondID != FirstID);
	if (!TestTrue(TEXT("which it enters with"), Rig.WaitForFrame(KindCall, EnterMethod, Enter)))
	{
		return false;
	}
	TestTrue(TEXT("carrying the new id"), Enter.Payload == Rig.Encode(EnterInputs(SecondID)));
	Rig.Reply(Enter.Rid, ECrowdyNativeExecStatus::Ok, {});
	Rig.PumpFor(0.15);
	Rig.Sent.Reset();

	const FCrowdyServerObjectTestLeaderState Own = Published(LocalUser, SecondID);
	if (!TestTrue(TEXT("its own actor id published is written"), PushLeader(Rig, FirstSeq + 2, LocalUser, Written, &Own)))
	{
		return false;
	}
	TestTrue(TEXT("making it the local Leader"), Written.Last() == FWrite(LocalUser, SecondID) && Leader->IsLocalLeader());
	Rig.Push(LeaderMessage(Rig, false, FirstSeq + 3, Other));
	TestTrue(TEXT("the other session entering last is written"), Rig.PumpUntil([&Written, &OtherSession]() { return Written.Last() == FWrite(LocalUser, OtherSession); }));
	TestFalse(TEXT("and this session neither leads nor enters to take it back"), Leader->IsLocalLeader() || Rig.SentAny(KindCall, EnterMethod));
	return true;
}

// A handler that spawns a Leader-owned entity on becoming the Leader is refused unless this client can already write as the Leader.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderSubsystemAnnouncesWhenItCanWriteTest,
	"CrowdySDK.CrowdyExec.Leader.TheNewLocalLeaderIsAnnouncedOnceItCanWrite", CrowdyServerObjectTests::TestFlags)
bool FCrowdyLeaderSubsystemAnnouncesWhenItCanWriteTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyLeaderSubsystemTests;
	FRig Rig(*this, ECrowdyServerObjectVisibility::Public, ECrowdyServerMembersSource::ThisObject);
	if (!Rig.IsValid())
	{
		return false;
	}
	Rig.Session->SetUserID(LocalUser);
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	const FGuid FirstLeaderID = FGuid::NewGuid();
	UCrowdyServerObject* Object = AcquireLeader(Rig, Owner.Get(), 5, Published(5, FirstLeaderID));
	if (!Object)
	{
		return false;
	}
	Rig.Session->SetUUID(RandomPlayerUUID());
	const FGuid OwnID = Rig.Session->GetID();

	TStrongObjectPtr<UCrowdyLeaderSubsystem> Leader(NewObject<UCrowdyLeaderSubsystem>(Rig.Instance.Get()));
	TStrongObjectPtr<UCrowdyLeaderTestListener> Listener(NewObject<UCrowdyLeaderTestListener>());
	TArray<bool> LocalAtChange;
	TArray<FGuid> WriterAtChange;
	Listener->OnChanged = [&LocalAtChange, &WriterAtChange, &Leader, &Rig]()
	{
		LocalAtChange.Add(Leader->IsLocalLeader());
		WriterAtChange.Add(Rig.Session->GetAppWriterID());
	};
	Leader->OnLeaderChanged.AddDynamic(Listener.Get(), &UCrowdyLeaderTestListener::HandleLeaderChanged);
	Leader->FollowForTest(Object, Rig.Session.Get(), nullptr);
	TestTrue(TEXT("another player's lead is announced at once"), Listener->Changes == TArray<FChange>{FChange(5, 0)});

	// The members' Leader moving before this client's actor id is published, as the built-in member functions do.
	if (!TestTrue(TEXT("this client taking the lead before its actor id is published leaves no app writer"), PushLeaderTo(Rig, FirstSeq + 1, LocalUser, FGuid())))
	{
		return false;
	}
	TestEqual(TEXT("while this client's user is reported as the Leader"), Leader->GetLeaderUserID(), LocalUser);
	TestFalse(TEXT("it is not the local Leader yet"), Leader->IsLocalLeader());
	TestEqual(TEXT("and the change is not announced yet"), Listener->Changes.Num(), 1);

	if (!TestTrue(TEXT("its own actor id published makes it the app writer"), PushPublished(Rig, FirstSeq + 2, Published(LocalUser, OwnID), OwnID)))
	{
		return false;
	}
	if (!TestTrue(TEXT("and then announces the change, once, from the Leader announced before"), Listener->Changes == TArray<FChange>({FChange(5, 0), FChange(LocalUser, 5)})))
	{
		return false;
	}
	TestTrue(TEXT("a handler sees this client as the local Leader"), LocalAtChange.Last());
	TestEqual(TEXT("and the session's app writer as this client's actor id"), WriterAtChange.Last(), OwnID);

	TestTrue(TEXT("another player taking the lead with nothing published yet"), PushLeaderTo(Rig, FirstSeq + 3, 9, FGuid()));
	TestTrue(TEXT("is announced at once on this client"), Listener->Changes.Num() == 3 && Listener->Changes.Last() == FChange(9, LocalUser));
	TestFalse(TEXT("which no longer leads"), LocalAtChange.Last());
	const FGuid NinthID = FGuid::NewGuid();
	TestTrue(TEXT("that player's actor id published becomes the app writer"), PushPublished(Rig, FirstSeq + 4, Published(9, NinthID), NinthID));
	TestEqual(TEXT("without announcing the same Leader again"), Listener->Changes.Num(), 3);

	TestTrue(TEXT("this client taking the lead again before its actor id is published"), PushLeaderTo(Rig, FirstSeq + 5, LocalUser, FGuid()));
	TestEqual(TEXT("waits to be announced"), Listener->Changes.Num(), 3);
	Leader->SignOutForTest();
	TestTrue(TEXT("a sign-out drops the waiting announcement and announces no Leader, from the one announced last"),
		Listener->Changes.Num() == 4 && Listener->Changes.Last() == FChange(0, 9));
	Listener->OnChanged = nullptr;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderSubsystemRefusedEnterTest,
	"CrowdySDK.CrowdyExec.Leader.ARefusedEnterWarnsOnceAndARetryEntersAgain", CrowdyServerObjectTests::TestFlags)
bool FCrowdyLeaderSubsystemRefusedEnterTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyLeaderSubsystemTests;
	FRig Rig(*this, ECrowdyServerObjectVisibility::Public, ECrowdyServerMembersSource::ThisObject);
	if (!Rig.IsValid())
	{
		return false;
	}
	Rig.Session->SetUserID(LocalUser);
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquireLeader(Rig, Owner.Get(), 5);
	if (!Object)
	{
		return false;
	}
	Rig.Sent.Reset();
	Rig.Session->SetUUID(RandomPlayerUUID());

	// Exactly one: a second warning in this sign-in, or one for the answer from before the next sign-in, fails the test.
	AddExpectedMessagePlain(EnterFailedWarning, ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	TArray<FWrite> Written;
	TStrongObjectPtr<UCrowdyLeaderSubsystem> Leader(NewObject<UCrowdyLeaderSubsystem>(Rig.Instance.Get()));
	Leader->FollowForTest(Object, Rig.Session.Get(), Recorder(Written));
	Leader->HandleConnectionUp();
	FSentFrame Enter;
	if (!TestTrue(TEXT("connected, it enters"), Rig.WaitForFrame(KindCall, EnterMethod, Enter)))
	{
		return false;
	}
	Rig.Reply(Enter.Rid, ECrowdyNativeExecStatus::Denied, Utf8Bytes(TEXT("players may not call this function")));
	Rig.PumpFor(0.2);
	Rig.Sent.Reset();
	TestEqual(TEXT("a refusal arms the retry"), Leader->GetPendingRetrySecondsForTest(), 10.f);
	if (!TestTrue(TEXT("which runs"), Leader->RunRetryForTest()) || !TestTrue(TEXT("and enters again once it finds the object Ready"), Rig.WaitForFrame(KindCall, EnterMethod, Enter)))
	{
		return false;
	}
	Rig.Reply(Enter.Rid, ECrowdyNativeExecStatus::Denied, Utf8Bytes(TEXT("players may not call this function")));
	Rig.PumpFor(0.2);
	Rig.Sent.Reset();

	if (!TestTrue(TEXT("a second refusal arms it again"), Leader->RunRetryForTest()) || !TestTrue(TEXT("and it enters again"), Rig.WaitForFrame(KindCall, EnterMethod, Enter)))
	{
		return false;
	}
	// The next sign-in resets the warning, so only ignoring this answer keeps it from warning.
	Leader->SignInForTest();
	Rig.Reply(Enter.Rid, ECrowdyNativeExecStatus::Denied, Utf8Bytes(TEXT("players may not call this function")));
	Rig.PumpFor(0.2);
	Rig.Sent.Reset();
	TestEqual(TEXT("an answer from before the sign-in arms no retry"), Leader->GetPendingRetrySecondsForTest(), 0.f);
	Leader->HandleConnectionUp();
	TestTrue(TEXT("the new sign-in enters"), Rig.WaitForFrame(KindCall, EnterMethod, Enter));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderSubsystemOlderServerTest,
	"CrowdySDK.CrowdyExec.Leader.AServerWithoutEnterAsksForARedeployAndRetriesSlowly", CrowdyServerObjectTests::TestFlags)
bool FCrowdyLeaderSubsystemOlderServerTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyLeaderSubsystemTests;
	FRig Rig(*this, ECrowdyServerObjectVisibility::Public, ECrowdyServerMembersSource::ThisObject);
	if (!Rig.IsValid())
	{
		return false;
	}
	Rig.Session->SetUserID(LocalUser);
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquireLeader(Rig, Owner.Get(), 5);
	if (!Object)
	{
		return false;
	}
	Rig.Sent.Reset();
	Rig.Session->SetUUID(RandomPlayerUUID());

	AddExpectedMessagePlain(RedeployWarning, ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	TArray<FWrite> Written;
	TStrongObjectPtr<UCrowdyLeaderSubsystem> Leader(NewObject<UCrowdyLeaderSubsystem>(Rig.Instance.Get()));
	Leader->FollowForTest(Object, Rig.Session.Get(), Recorder(Written));
	Leader->HandleConnectionUp();
	FSentFrame Enter;
	if (!TestTrue(TEXT("connected, it enters"), Rig.WaitForFrame(KindCall, EnterMethod, Enter)))
	{
		return false;
	}
	Rig.Reply(Enter.Rid, ECrowdyNativeExecStatus::AppError, Utf8Bytes(TEXT("unknown_method: enter")));
	Rig.PumpFor(0.2);
	Rig.Sent.Reset();
	TestEqual(TEXT("a server without Enter is asked again only by a slow retry"), Leader->GetPendingRetrySecondsForTest(), 60.f);
	Leader->HandleConnectionUp();
	TestFalse(TEXT("not by a new connection"), Rig.SentAny(KindCall, EnterMethod));

	if (!TestTrue(TEXT("the slow retry runs"), Leader->RunRetryForTest()) || !TestTrue(TEXT("and asks again"), Rig.WaitForFrame(KindCall, EnterMethod, Enter)))
	{
		return false;
	}
	// The expected warning's count of one fails the test if this second refusal warns again.
	Rig.Reply(Enter.Rid, ECrowdyNativeExecStatus::AppError, Utf8Bytes(TEXT("unknown_method: enter")));
	Rig.PumpFor(0.2);
	Rig.Sent.Reset();
	TestEqual(TEXT("a second refusal arms the slow retry again, silently"), Leader->GetPendingRetrySecondsForTest(), 60.f);

	Leader->SignInForTest();
	TestEqual(TEXT("a new sign-in drops the waiting retry"), Leader->GetPendingRetrySecondsForTest(), 0.f);
	Leader->HandleConnectionUp();
	TestTrue(TEXT("and asks again at once"), Rig.WaitForFrame(KindCall, EnterMethod, Enter));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderSubsystemCooldownTest,
	"CrowdySDK.CrowdyExec.Leader.ACooldownRefusalRetriesSoonWithoutWarning", CrowdyServerObjectTests::TestFlags)
bool FCrowdyLeaderSubsystemCooldownTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyLeaderSubsystemTests;
	FRig Rig(*this, ECrowdyServerObjectVisibility::Public, ECrowdyServerMembersSource::ThisObject);
	if (!Rig.IsValid())
	{
		return false;
	}
	Rig.Session->SetUserID(LocalUser);
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquireLeader(Rig, Owner.Get(), 5);
	if (!Object)
	{
		return false;
	}
	Rig.Sent.Reset();
	Rig.Session->SetUUID(RandomPlayerUUID());

	TArray<FWrite> Written;
	TStrongObjectPtr<UCrowdyLeaderSubsystem> Leader(NewObject<UCrowdyLeaderSubsystem>(Rig.Instance.Get()));
	Leader->FollowForTest(Object, Rig.Session.Get(), Recorder(Written));
	Leader->HandleConnectionUp();
	FSentFrame Enter;
	if (!TestTrue(TEXT("connected, it enters"), Rig.WaitForFrame(KindCall, EnterMethod, Enter)))
	{
		return false;
	}
	// As the generated check_cooldown words it.
	Rig.Reply(Enter.Rid, ECrowdyNativeExecStatus::AppError, Utf8Bytes(TEXT("cooldown: Enter can be called again in 1 s")));
	Rig.PumpFor(0.2);
	Rig.Sent.Reset();
	TestEqual(TEXT("a cooldown refusal retries just past the cooldown, not after the usual wait"), Leader->GetPendingRetrySecondsForTest(), 1.5f);
	if (!TestTrue(TEXT("the short retry runs"), Leader->RunRetryForTest()) || !TestTrue(TEXT("and enters again"), Rig.WaitForFrame(KindCall, EnterMethod, Enter)))
	{
		return false;
	}
	Rig.Reply(Enter.Rid, ECrowdyNativeExecStatus::AppError, Utf8Bytes(TEXT("cooldown: Enter can be called again in 1 s")));
	Rig.PumpFor(0.2);
	Rig.Sent.Reset();

	// With no Enter accepted yet, any Leader change may mean this client was never let in.
	if (!TestTrue(TEXT("a Leader change is written"), PushLeader(Rig, FirstSeq + 1, 9, Written)))
	{
		return false;
	}
	TestTrue(TEXT("and a client with no accepted Enter enters at it"), Rig.WaitForFrame(KindCall, EnterMethod, Enter));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderSubsystemReentersOnlyThePreviousLeaderTest,
	"CrowdySDK.CrowdyExec.Leader.AHandoverReentersOnlyThePreviousLeader", CrowdyServerObjectTests::TestFlags)
bool FCrowdyLeaderSubsystemReentersOnlyThePreviousLeaderTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyLeaderSubsystemTests;
	FRig Rig(*this, ECrowdyServerObjectVisibility::Public, ECrowdyServerMembersSource::ThisObject);
	if (!Rig.IsValid())
	{
		return false;
	}
	Rig.Session->SetUserID(LocalUser);
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquireLeader(Rig, Owner.Get(), 5);
	if (!Object)
	{
		return false;
	}
	Rig.Sent.Reset();
	Rig.Session->SetUUID(RandomPlayerUUID());

	TArray<FWrite> Written;
	TStrongObjectPtr<UCrowdyLeaderSubsystem> Leader(NewObject<UCrowdyLeaderSubsystem>(Rig.Instance.Get()));
	Leader->FollowForTest(Object, Rig.Session.Get(), Recorder(Written));
	Leader->HandleConnectionUp();
	FSentFrame Enter;
	if (!TestTrue(TEXT("connected, it enters"), Rig.WaitForFrame(KindCall, EnterMethod, Enter)))
	{
		return false;
	}
	Rig.Reply(Enter.Rid, ECrowdyNativeExecStatus::Ok, {});
	Rig.PumpFor(0.15);
	Rig.Sent.Reset();

	if (!TestTrue(TEXT("a handover between two other players is written"), PushLeader(Rig, FirstSeq + 1, 9, Written)))
	{
		return false;
	}
	TestFalse(TEXT("and a member that was not the Leader does not enter again"), Rig.SentAny(KindCall, EnterMethod));

	const FCrowdyServerObjectTestLeaderState Own = Published(LocalUser, Rig.Session->GetID());
	if (!TestTrue(TEXT("this client becoming the Leader is written"), PushLeader(Rig, FirstSeq + 2, LocalUser, Written, &Own)))
	{
		return false;
	}
	Rig.Sent.Reset();
	if (!TestTrue(TEXT("its lead passing to another player is written"), PushLeader(Rig, FirstSeq + 3, 5, Written)))
	{
		return false;
	}
	TestTrue(TEXT("and the Leader before enters again, since it may have been removed"), Rig.WaitForFrame(KindCall, EnterMethod, Enter));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderSubsystemOtherAccountTest,
	"CrowdySDK.CrowdyExec.Leader.EntersNothingOverAnotherPlayersConnection", CrowdyServerObjectTests::TestFlags)
bool FCrowdyLeaderSubsystemOtherAccountTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyLeaderSubsystemTests;
	FRig Rig(*this, ECrowdyServerObjectVisibility::Public, ECrowdyServerMembersSource::ThisObject);
	if (!Rig.IsValid())
	{
		return false;
	}
	Rig.Session->SetUserID(LocalUser);
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquireLeader(Rig, Owner.Get(), 5);
	if (!Object)
	{
		return false;
	}
	Rig.Sent.Reset();
	Rig.Session->SetUUID(RandomPlayerUUID());

	TArray<FWrite> Written;
	TStrongObjectPtr<UCrowdyLeaderSubsystem> Leader(NewObject<UCrowdyLeaderSubsystem>(Rig.Instance.Get()));
	Leader->FollowForTest(Object, Rig.Session.Get(), Recorder(Written));
	TestEqual(TEXT("the connection carries this player's token"), Object->GetConnectedUserId(), LocalUser);

	// A sign-in as another player sets the session's user before the connection dialed for the last one closes.
	Rig.Session->SetUserID(LocalUser + 1);
	Leader->HandleConnectionUp();
	TestFalse(TEXT("while the connection carries another player's token, nothing enters"), Rig.SentAny(KindCall, EnterMethod));
	Rig.Session->SetUserID(LocalUser);
	Leader->HandleConnectionUp();
	FSentFrame Enter;
	TestTrue(TEXT("once the session and the connection agree, it enters"), Rig.WaitForFrame(KindCall, EnterMethod, Enter));
	return true;
}

#endif
