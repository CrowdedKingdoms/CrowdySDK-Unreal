#include "CrowdyServerObjectTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyLeaderSubsystem.h"
#include "CrowdyServerObject.h"
#include "CrowdyServerObjectTestRig.h"
#include "Misc/AutomationTest.h"
#include "Subsystem/CrowdyGameSession.h"
#include "UObject/StrongObjectPtr.h"
#include "Utils/HelperFunctions.h"

namespace CrowdyLeaderSubsystemTests
{
	constexpr int64 LocalUser = 77;

	/** Pushes a Leader change at Seq and pumps until the subsystem has written one more Leader. */
	bool PushLeader(CrowdyServerObjectTests::FRig& Rig, const uint64 Seq, const int64 NewLeader, const TArray<int64>& Written)
	{
		CrowdyServerObjectTests::FMembersKeys Keys;
		Keys.Leader = NewLeader;
		const int32 Before = Written.Num();
		Rig.Push(Rig.StateMessage(false, CrowdyServerObjectTests::Epoch, Seq, nullptr, {}, Keys));
		return Rig.PumpUntil([&Written, Before]() { return Written.Num() > Before; });
	}

	/** The session id a Player Derived avatar sets for User. */
	FString PlayerUUID(const int64 User)
	{
		return UHelperFunctions::GetDeterministicID(User).ToString(EGuidFormats::Digits);
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
	FMembersKeys Keys;
	Keys.MemberCount = 1;
	Keys.Leader = 5;
	UCrowdyServerObject* Object = Rig.AcquireReady(Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5, Keys);
	if (!Object)
	{
		return false;
	}
	Rig.Sent.Reset();

	Rig.Session->SetUUID(PlayerUUID(LocalUser));

	TArray<int64> Written;
	TStrongObjectPtr<UCrowdyLeaderSubsystem> Leader(NewObject<UCrowdyLeaderSubsystem>(Rig.Instance.Get()));
	Leader->FollowForTest(Object, Rig.Session.Get(), [&Written](const int64 Id) { Written.Add(Id); });
	TestTrue(TEXT("following a Ready object writes its Leader once"), Written == TArray<int64>{5});
	TestEqual(TEXT("and reports it"), Leader->GetLeaderUserID(), static_cast<int64>(5));
	TestFalse(TEXT("another player leading is not this client"), Leader->IsLocalLeader());
	TestFalse(TEXT("not yet in the realtime world, it does not join"), Rig.SentAny(KindCall, TEXT("join")));
	Rig.Sent.Reset();
	Leader->HandleConnectionUp();
	FSentFrame Join;
	TestTrue(TEXT("once its realtime connection is up it joins"), Rig.WaitForFrame(KindCall, TEXT("join"), Join));
	Rig.PumpFor(0.15);
	Rig.Sent.Reset();

	if (!TestTrue(TEXT("a push making this client the Leader is written"), PushLeader(Rig, 6, LocalUser, Written)))
	{
		return false;
	}
	TestEqual(TEXT("as this client's user id"), Written.Last(), LocalUser);
	TestTrue(TEXT("and this client is the local Leader"), Leader->IsLocalLeader());
	TestFalse(TEXT("leading, it does not join again"), Rig.SentAny(KindCall, TEXT("join")));
	Rig.Sent.Reset();

	if (!TestTrue(TEXT("another player taking the lead is written"), PushLeader(Rig, 7, 9, Written)))
	{
		return false;
	}
	TestEqual(TEXT("as that player's user id"), Written.Last(), static_cast<int64>(9));
	TestFalse(TEXT("and this client is no longer the local Leader"), Leader->IsLocalLeader());
	TestTrue(TEXT("and this client joins again, since nothing tells it whether it was removed"), Rig.WaitForFrame(KindCall, TEXT("join"), Join));
	Rig.PumpFor(0.15);
	Rig.Sent.Reset();

	Leader->HandleConnectionLost();
	FSentFrame Leave;
	TestTrue(TEXT("a failed realtime connection leaves"), Rig.WaitForFrame(KindCall, TEXT("leave"), Leave));
	if (!TestTrue(TEXT("a Leader change while the connection is down is written"), PushLeader(Rig, 8, 10, Written)))
	{
		return false;
	}
	TestFalse(TEXT("but does not join"), Rig.SentAny(KindCall, TEXT("join")));
	Rig.Sent.Reset();
	Leader->HandleConnectionUp();
	TestTrue(TEXT("the connection coming back joins again"), Rig.WaitForFrame(KindCall, TEXT("join"), Join));
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
	FMembersKeys Keys;
	Keys.MemberCount = 1;
	Keys.Leader = 5;
	UCrowdyServerObject* Object = Rig.AcquireReady(Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5, Keys);
	if (!Object)
	{
		return false;
	}
	Rig.Sent.Reset();

	TArray<int64> Written;
	TStrongObjectPtr<UCrowdyLeaderSubsystem> Leader(NewObject<UCrowdyLeaderSubsystem>(Rig.Instance.Get()));
	Leader->FollowForTest(Object, Rig.Session.Get(), [&Written](const int64 Id) { Written.Add(Id); });

	Leader->HandleConnectionUp();
	Rig.PumpFor(0.15);
	TestFalse(TEXT("with no avatar yet, a connected client does not join"), Rig.SentAny(KindCall, TEXT("join")));

	AddExpectedMessage(TEXT("came from before this sign-in"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	Rig.Session->SetUUID(PlayerUUID(LocalUser + 1));
	Rig.PumpFor(0.15);
	TestFalse(TEXT("the id another account left behind does not count"), Rig.SentAny(KindCall, TEXT("join")));

	Rig.Session->SetUUID(PlayerUUID(LocalUser));
	FSentFrame Join;
	TestTrue(TEXT("this player's avatar setting the id joins at once"), Rig.WaitForFrame(KindCall, TEXT("join"), Join));
	Rig.PumpFor(0.15);
	Rig.Sent.Reset();

	Rig.Session->ClearCurrentSessionData();
	Rig.Session->SetUserID(LocalUser);
	Leader->HandleConnectionUp();
	Rig.PumpFor(0.15);
	TestFalse(TEXT("signing back in as the same player needs a new avatar too"), Rig.SentAny(KindCall, TEXT("join")));
	return true;
}

#endif
