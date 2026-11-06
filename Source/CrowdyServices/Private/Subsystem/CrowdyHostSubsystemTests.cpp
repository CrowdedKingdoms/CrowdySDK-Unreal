#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyHostTestTypes.h"
#include "Async/TaskGraphInterfaces.h"
#include "Engine/GameInstance.h"
#include "Misc/AutomationTest.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Subsystem/CrowdyHostSubsystem.h"
#include "UObject/Package.h"
#include "Utils/HelperFunctions.h"

PRAGMA_DISABLE_DEPRECATION_WARNINGS

namespace
{
	constexpr EAutomationTestFlags HostTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	constexpr int64 SignedInUserID = 7;

	struct FHostRig
	{
		UCrowdyGameSession* Session = nullptr;
		UCrowdyHostSubsystem* Host = nullptr;
		UCrowdyHostTestListener* Listener = nullptr;
	};

	FHostRig MakeHostRig()
	{
		FHostRig Rig;
		UGameInstance* GameInstance = NewObject<UGameInstance>(GetTransientPackage());
		Rig.Session = NewObject<UCrowdyGameSession>(GameInstance);
		Rig.Session->SetUserID(SignedInUserID);
		Rig.Host = NewObject<UCrowdyHostSubsystem>(GetTransientPackage());
		Rig.Host->SetGameSessionForTest(Rig.Session);
		Rig.Listener = NewObject<UCrowdyHostTestListener>(GetTransientPackage());
		Rig.Host->OnHostElected.AddDynamic(Rig.Listener, &UCrowdyHostTestListener::HandleHostElected);
		return Rig;
	}

	// SetHostUserID applies the id on the game thread, so the test runs that hop before reading the result.
	void SetHostAndSettle(UCrowdyHostSubsystem* Host, const int64 UserID)
	{
		Host->SetHostUserID(UserID);
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyHostSignOutClearsHostTest,
	"CrowdySDK.CrowdyServices.HostSignOutClearsHost", HostTestFlags)
bool FCrowdyHostSignOutClearsHostTest::RunTest(const FString& Parameters)
{
	const FHostRig Rig = MakeHostRig();
	const FGuid Elected = UHelperFunctions::GetDeterministicID(42);

	SetHostAndSettle(Rig.Host, 42);
	TestTrue(TEXT("an elected host is set"), Rig.Host->IsHostSet());
	TestEqual(TEXT("the session holds the elected host"), Rig.Session->GetHostID(), Elected);

	// Signing out clears the signed-in user and sets host user id 0: the session must hold no host, not a hash of zero.
	Rig.Session->SetUserID(0);
	SetHostAndSettle(Rig.Host, 0);
	TestFalse(TEXT("no host is set after sign-out"), Rig.Host->IsHostSet());
	TestFalse(TEXT("the session's host id is empty after sign-out"), Rig.Session->GetHostID().IsValid());

	if (!TestEqual(TEXT("On Host Elected fired for the election and the sign-out"), Rig.Listener->HostIDs.Num(), 2))
	{
		return false;
	}
	TestFalse(TEXT("the first election has no previous host"), Rig.Listener->PreviousHostIDs[0].IsValid());
	TestEqual(TEXT("the first election names the elected host"), Rig.Listener->HostIDs[0], Elected);
	TestFalse(TEXT("the sign-out names no host"), Rig.Listener->HostIDs[1].IsValid());
	TestEqual(TEXT("the sign-out names the host it replaced"), Rig.Listener->PreviousHostIDs[1], Elected);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyHostAnswerAfterSignOutIgnoredTest,
	"CrowdySDK.CrowdyServices.HostAnswerAfterSignOutIgnored", HostTestFlags)
bool FCrowdyHostAnswerAfterSignOutIgnoredTest::RunTest(const FString& Parameters)
{
	const FHostRig Rig = MakeHostRig();

	// A poll that was in flight when the player signed out answers with a real host.
	Rig.Session->SetUserID(0);
	SetHostAndSettle(Rig.Host, 42);
	TestFalse(TEXT("no host is set while nobody is signed in"), Rig.Host->IsHostSet());
	TestEqual(TEXT("the host user id stays 0"), Rig.Host->GetHostUserID(), static_cast<int64>(0));
	TestEqual(TEXT("On Host Elected does not fire"), Rig.Listener->HostIDs.Num(), 0);

	// The same answer after signing in again is applied, so the ignored one left nothing behind.
	Rig.Session->SetUserID(SignedInUserID);
	SetHostAndSettle(Rig.Host, 42);
	TestTrue(TEXT("the host is set once signed in"), Rig.Host->IsHostSet());
	TestEqual(TEXT("On Host Elected fires once"), Rig.Listener->HostIDs.Num(), 1);
	return true;
}

PRAGMA_ENABLE_DEPRECATION_WARNINGS

#endif
